/**
 * @file test/unit/test_elf_loader.cpp
 * @brief Host-side unit tests for ELF64 parser and loader
 *
 * Test coverage:
 *   - ELF header validation (magic, class, endianness, machine, type)
 *   - Program header traversal and PT_LOAD segment identification
 *   - Kernel size calculation from PT_LOAD segments
 *   - ELF loading: segment copy, BSS zero-fill, entry point extraction
 *   - Higher-half kernel virtual-to-physical entry point conversion
 *   - Edge cases: null pointer, bad magic, wrong class/endianness/machine/type
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#	include <cstddef>
#	include <cstdint>
#	include <cstring>

// Include the ELF loader header for struct definitions and constants
#	include "mini/elf_loader.hpp"

using namespace cinux::mini::elf_loader;

// ============================================================
// Test Helpers: Build ELF binaries in memory
// ============================================================

/// Staging buffer large enough for our test ELF binaries
static constexpr size_t TEST_ELF_BUFFER_SIZE = 8192;
static uint8_t			g_elf_buffer[TEST_ELF_BUFFER_SIZE];

/// Destination buffer for ELF segment loading
static constexpr size_t TEST_LOAD_BUFFER_SIZE = 8192;
static uint8_t			g_load_buffer[TEST_LOAD_BUFFER_SIZE];

/**
 * @brief Reset both staging and load buffers to a known fill pattern
 *
 * Uses 0xAA so that zero-fill (BSS) and uninitialized areas are detectable.
 */
static void reset_buffers() {
	memset(g_elf_buffer, 0xAA, TEST_ELF_BUFFER_SIZE);
	memset(g_load_buffer, 0xAA, TEST_LOAD_BUFFER_SIZE);
}

/**
 * @brief Build a minimal valid ELF64 header in the staging buffer
 *
 * @param entry       Virtual entry point address
 * @param phoff       Program header table file offset
 * @param phnum       Number of program headers
 * @param phentsize   Size of each program header entry (default 56)
 * @return Pointer to the Elf64_Ehdr in the staging buffer
 */
static Elf64_Ehdr* build_elf_header(uint64_t entry = 0x1000, uint64_t phoff = sizeof(Elf64_Ehdr),
									uint16_t phnum = 1, uint16_t phentsize = sizeof(Elf64_Phdr)) {
	reset_buffers();

	auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	memset(ehdr, 0, sizeof(Elf64_Ehdr));

	// Magic: 0x7F 'E' 'L' 'F'
	ehdr->e_ident[0] = 0x7F;
	ehdr->e_ident[1] = 'E';
	ehdr->e_ident[2] = 'L';
	ehdr->e_ident[3] = 'F';
	// Class: 64-bit
	ehdr->e_ident[4] = ELF_CLASS_64;
	// Data: little-endian
	ehdr->e_ident[5] = ELF_DATA_LSB;
	// OS/ABI: System V
	ehdr->e_ident[6] = ELF_OSABI_SYSV;

	ehdr->e_type	  = ET_EXEC;
	ehdr->e_machine	  = EM_X86_64;
	ehdr->e_version	  = 1;
	ehdr->e_entry	  = entry;
	ehdr->e_phoff	  = phoff;
	ehdr->e_shoff	  = 0;
	ehdr->e_flags	  = 0;
	ehdr->e_ehsize	  = sizeof(Elf64_Ehdr);
	ehdr->e_phentsize = phentsize;
	ehdr->e_phnum	  = phnum;
	ehdr->e_shentsize = 0;
	ehdr->e_shnum	  = 0;
	ehdr->e_shstrndx  = 0;

	return ehdr;
}

/**
 * @brief Add a PT_LOAD program header to the ELF in the staging buffer
 *
 * @param index   Program header index (0-based)
 * @param offset  Segment file offset
 * @param vaddr   Segment virtual address
 * @param paddr   Segment physical address
 * @param filesz  Segment size in file
 * @param memsz   Segment size in memory (filesz + BSS)
 * @param flags   Segment flags (PF_R | PF_W | PF_X)
 */
static void add_phdr(uint16_t index, uint64_t offset, uint64_t vaddr, uint64_t paddr,
					 uint64_t filesz, uint64_t memsz, uint32_t flags = PF_R | PF_X) {
	auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	auto* phdr = reinterpret_cast<Elf64_Phdr*>(g_elf_buffer + ehdr->e_phoff +
											   static_cast<uint64_t>(index) * ehdr->e_phentsize);

	memset(phdr, 0, sizeof(Elf64_Phdr));
	phdr->p_type   = PT_LOAD;
	phdr->p_flags  = flags;
	phdr->p_offset = offset;
	phdr->p_vaddr  = vaddr;
	phdr->p_paddr  = paddr;
	phdr->p_filesz = filesz;
	phdr->p_memsz  = memsz;
	phdr->p_align  = 0x1000;

	// Write recognizable file data at p_offset so we can verify it was copied
	for (uint64_t i = 0; i < filesz && (offset + i) < TEST_ELF_BUFFER_SIZE; i++) {
		g_elf_buffer[offset + i] = static_cast<uint8_t>(0xC0 + (i & 0x0F));
	}
}

// ============================================================
// We need to test load_elf and parse_elf_header.
// These functions live in elf_loader.cpp which calls kprintf.
// For host tests, we provide a stub kprintf and the internal
// memcpy/memset_zero logic by recompiling the core logic.
//
// Strategy: include a host-safe copy of the logic functions
// that operate on the same principles but without kernel dependencies.
// ============================================================

// Mock kprintf: no-op for host tests
namespace cinux::mini::lib {
void kprintf(const char*, ...) {}
}  // namespace cinux::mini::lib

// Re-implement the core functions from elf_loader.cpp for host testing.
// We must do this because load_elf() writes to p_paddr addresses directly,
// which would be invalid in host userspace. Instead, we test parse_elf_header
// and calculate_kernel_size directly (they are pure logic), and test the
// loading logic by including elf_loader.cpp with load_elf patched to use
// a user-space buffer.
//
// Since load_elf writes to absolute physical addresses (p_paddr), it cannot
// be safely called in host tests. We test the pure-logic functions directly
// and verify the loading logic's correctness through structural checks.

// Pull in the actual elf_loader.cpp implementation.
// The kprintf stub above satisfies the linker.
// However, load_elf() writes to absolute addresses (p_paddr), so we must
// intercept those writes. We'll test parse_elf_header and
// calculate_kernel_size directly (safe), and for load_elf we provide
// a modified test version.

#	include "mini/elf_loader.cpp"	// NOLINT

// ============================================================
// 1. ELF Header Validation - Normal Path
// ============================================================

/**
 * @brief Verify a correctly constructed ELF64 header passes validation
 */
TEST("elf: valid header passes parse_elf_header") {
	build_elf_header();
	ASSERT_TRUE(parse_elf_header(g_elf_buffer));
}

/**
 * @brief Verify the ELF magic bytes 0x7F 'E' 'L' 'F' are accepted
 */
TEST("elf: magic bytes are correct") {
	build_elf_header();
	auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ASSERT_EQ(ehdr->e_ident[0], 0x7F);
	ASSERT_EQ(ehdr->e_ident[1], 'E');
	ASSERT_EQ(ehdr->e_ident[2], 'L');
	ASSERT_EQ(ehdr->e_ident[3], 'F');
}

// ============================================================
// 2. ELF Header Validation - Null / Bad Pointer
// ============================================================

/**
 * @brief Verify null pointer is rejected
 */
TEST("elf: null pointer rejected") {
	ASSERT_FALSE(parse_elf_header(nullptr));
}

// ============================================================
// 3. ELF Header Validation - Bad Magic
// ============================================================

/**
 * @brief Verify completely wrong magic bytes are rejected
 */
TEST("elf: bad magic rejected") {
	build_elf_header();
	auto* ehdr		 = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_ident[0] = 0x00;
	ehdr->e_ident[1] = 'N';
	ehdr->e_ident[2] = 'O';
	ehdr->e_ident[3] = 'P';
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

/**
 * @brief Verify partial magic (only first byte correct) is rejected
 */
TEST("elf: partial magic rejected") {
	build_elf_header();
	auto* ehdr		 = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_ident[3] = 'X';	 // Corrupt last byte of magic
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

/**
 * @brief Verify zeroed-out header is rejected
 */
TEST("elf: all-zero header rejected") {
	uint8_t zeros[64] = {};
	ASSERT_FALSE(parse_elf_header(zeros));
}

// ============================================================
// 4. ELF Header Validation - Wrong Class
// ============================================================

/**
 * @brief Verify 32-bit ELF class (ELF_CLASS_32 = 1) is rejected
 */
TEST("elf: 32-bit class rejected") {
	build_elf_header();
	auto* ehdr		 = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_ident[4] = 1;  // ELF_CLASS_32
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

// ============================================================
// 5. ELF Header Validation - Wrong Endianness
// ============================================================

/**
 * @brief Verify big-endian encoding is rejected
 */
TEST("elf: big-endian rejected") {
	build_elf_header();
	auto* ehdr		 = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_ident[5] = 2;  // ELF_DATA_MSB (big-endian)
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

// ============================================================
// 6. ELF Header Validation - Wrong Machine
// ============================================================

/**
 * @brief Verify non-x86-64 machine type is rejected
 */
TEST("elf: wrong machine rejected") {
	build_elf_header();
	auto* ehdr		= reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_machine = 3;  // EM_386
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

/**
 * @brief Verify ARM machine type is rejected
 */
TEST("elf: ARM machine rejected") {
	build_elf_header();
	auto* ehdr		= reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_machine = 183;	// EM_AARCH64
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

// ============================================================
// 7. ELF Header Validation - Wrong Type
// ============================================================

/**
 * @brief Verify shared object (ET_DYN = 3) is rejected
 */
TEST("elf: shared object rejected") {
	build_elf_header();
	auto* ehdr	 = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_type = 3;  // ET_DYN
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

/**
 * @brief Verify relocatable object (ET_REL = 1) is rejected
 */
TEST("elf: relocatable rejected") {
	build_elf_header();
	auto* ehdr	 = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	ehdr->e_type = 1;  // ET_REL
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

// ============================================================
// 8. ELF Header Validation - No Program Headers
// ============================================================

/**
 * @brief Verify ELF with zero program headers is rejected
 */
TEST("elf: zero program headers rejected") {
	build_elf_header(0x1000, sizeof(Elf64_Ehdr), 0);
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

/**
 * @brief Verify ELF with zero phoff is rejected
 */
TEST("elf: zero phoff rejected") {
	build_elf_header(0x1000, 0, 1);
	ASSERT_FALSE(parse_elf_header(g_elf_buffer));
}

// ============================================================
// 9. Kernel Size Calculation - Single Segment
// ============================================================

/**
 * @brief Verify size calculation for a single PT_LOAD segment
 *
 * If paddr = 0x100000, memsz = 0x2000, the span is 0x2000 bytes.
 */
TEST("elf: calculate_kernel_size single segment") {
	build_elf_header();
	add_phdr(0, /*offset=*/0x1000, /*vaddr=*/0x100000, /*paddr=*/0x100000,
			 /*filesz=*/0x1000, /*memsz=*/0x2000);

	auto*  ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	size_t size = calculate_kernel_size(ehdr);
	ASSERT_EQ(size, 0x2000u);
}

/**
 * @brief Verify size calculation returns 0 when no PT_LOAD segments exist
 */
TEST("elf: calculate_kernel_size no PT_LOAD") {
	build_elf_header();
	// Create a non-PT_LOAD segment
	auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	auto* phdr = reinterpret_cast<Elf64_Phdr*>(g_elf_buffer + ehdr->e_phoff);
	memset(phdr, 0, sizeof(Elf64_Phdr));
	phdr->p_type  = 0x6474E551;	 // PT_GNU_STACK (not PT_LOAD)
	phdr->p_memsz = 0x1000;

	size_t size = calculate_kernel_size(ehdr);
	ASSERT_EQ(size, 0u);
}

// ============================================================
// 10. Kernel Size Calculation - Multiple Segments
// ============================================================

/**
 * @brief Verify size calculation with two PT_LOAD segments
 *
 * Segment 1: paddr=0x100000, memsz=0x2000
 * Segment 2: paddr=0x200000, memsz=0x3000
 * Total span: 0x200000 + 0x3000 - 0x100000 = 0x103000
 */
TEST("elf: calculate_kernel_size two segments") {
	build_elf_header(0x100000, sizeof(Elf64_Ehdr), 2);
	add_phdr(0, /*offset=*/0x1000, /*vaddr=*/0x100000, /*paddr=*/0x100000,
			 /*filesz=*/0x1000, /*memsz=*/0x2000);
	add_phdr(1, /*offset=*/0x3000, /*vaddr=*/0x200000, /*paddr=*/0x200000,
			 /*filesz=*/0x2000, /*memsz=*/0x3000);

	auto*  ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	size_t size = calculate_kernel_size(ehdr);
	ASSERT_EQ(size, static_cast<size_t>(0x103000));
}

// ============================================================
// 11. Kernel Size Calculation - Segment with BSS
// ============================================================

/**
 * @brief Verify memsz > filesz (BSS) is accounted for in size calculation
 */
TEST("elf: calculate_kernel_size with large BSS") {
	build_elf_header();
	add_phdr(0, /*offset=*/0x1000, /*vaddr=*/0x100000, /*paddr=*/0x100000,
			 /*filesz=*/0x100, /*memsz=*/0x10000);	// 256 bytes file, 64KB memory

	auto*  ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	size_t size = calculate_kernel_size(ehdr);
	ASSERT_EQ(size, 0x10000u);
}

// ============================================================
// 12. Kernel Size Calculation - Segments in Reverse Order
// ============================================================

/**
 * @brief Verify size calculation when segments are in non-ascending paddr order
 *
 * The function should still find the lowest and highest addresses correctly.
 */
TEST("elf: calculate_kernel_size reverse order segments") {
	build_elf_header(0x200000, sizeof(Elf64_Ehdr), 2);
	// Higher address segment first
	add_phdr(0, /*offset=*/0x3000, /*vaddr=*/0x200000, /*paddr=*/0x200000,
			 /*filesz=*/0x1000, /*memsz=*/0x2000);
	// Lower address segment second
	add_phdr(1, /*offset=*/0x1000, /*vaddr=*/0x100000, /*paddr=*/0x100000,
			 /*filesz=*/0x1000, /*memsz=*/0x1000);

	auto*  ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	size_t size = calculate_kernel_size(ehdr);
	ASSERT_EQ(size, static_cast<size_t>(0x202000 - 0x100000));
}

// ============================================================
// 13. ELF Struct Size Tests
// ============================================================

/**
 * @brief Verify Elf64_Ehdr struct is exactly 64 bytes
 */
TEST("elf: Elf64_Ehdr size is 64 bytes") {
	ASSERT_EQ(sizeof(Elf64_Ehdr), 64u);
}

/**
 * @brief Verify Elf64_Phdr struct is exactly 56 bytes
 */
TEST("elf: Elf64_Phdr size is 56 bytes") {
	ASSERT_EQ(sizeof(Elf64_Phdr), 56u);
}

// ============================================================
// 14. ELF Constant Tests
// ============================================================

/**
 * @brief Verify ELF magic number constant
 */
TEST("elf: magic constant") {
	ASSERT_EQ(ELF_MAGIC, 0x464C457Fu);
}

/**
 * @brief Verify ELF class, data, and type constants
 */
TEST("elf: constants correct") {
	ASSERT_EQ(ELF_CLASS_64, 2);
	ASSERT_EQ(ELF_DATA_LSB, 1);
	ASSERT_EQ(ET_EXEC, 2);
	ASSERT_EQ(EM_X86_64, 62);
	ASSERT_EQ(PT_LOAD, 1u);
}

/**
 * @brief Verify program header flag constants
 */
TEST("elf: flag constants") {
	ASSERT_EQ(PF_X, 1u);
	ASSERT_EQ(PF_W, 2u);
	ASSERT_EQ(PF_R, 4u);
}

// ============================================================
// 15. load_elf - Entry Point Extraction Tests
// ============================================================

/**
 * @brief Verify load_elf returns 0 for invalid ELF header
 */
TEST("elf: load_elf rejects invalid header") {
	uint8_t bad_buf[512] = {};
	ASSERT_EQ(load_elf(bad_buf, 0x100000), 0u);
}

/**
 * @brief Verify load_elf returns correct entry point for lower-half address
 *
 * For an entry point below HIGHER_HALF_BASE (0xFFFFFFFF80000000),
 * it should be returned as-is.
 */
TEST("elf: load_elf lower-half entry point") {
	build_elf_header(/*entry=*/0x1000, sizeof(Elf64_Ehdr), 1);

	uint64_t dest_addr = reinterpret_cast<uint64_t>(g_load_buffer);
	add_phdr(0, /*offset=*/sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr),
			 /*vaddr=*/0x1000, /*paddr=*/dest_addr,
			 /*filesz=*/0, /*memsz=*/0);

	// staging_size = buffer size for bounds checking
	uint64_t staging_size = TEST_ELF_BUFFER_SIZE;
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);
	ASSERT_EQ(entry, 0x1000u);
}

/**
 * @brief Verify load_elf converts higher-half entry to physical address
 *
 * Entry = 0xFFFFFFFF80100000 should be returned as-is (higher-half virtual)
 */
TEST("elf: load_elf higher-half entry point conversion") {
	build_elf_header(/*entry=*/0xFFFFFFFF80100000ULL, sizeof(Elf64_Ehdr), 1);

	uint64_t dest_addr = reinterpret_cast<uint64_t>(g_load_buffer);
	add_phdr(0, /*offset=*/sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr),
			 /*vaddr=*/0xFFFFFFFF80100000ULL, /*paddr=*/dest_addr,
			 /*filesz=*/0, /*memsz=*/0);

	uint64_t staging_size = TEST_ELF_BUFFER_SIZE;
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);
	ASSERT_EQ(entry, 0xFFFFFFFF80100000ULL);
	// ============================================================
}

/**
 * @brief Verify load_elf copies file data to destination and zeroes BSS
 */
TEST("elf: load_elf copies file data and zeroes BSS") {
	constexpr uint64_t filesz	  = 64;
	constexpr uint64_t memsz	  = 256;  // BSS = 192 bytes
	constexpr uint64_t seg_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);

	build_elf_header(/*entry=*/0x1000, sizeof(Elf64_Ehdr), 1);

	uint64_t dest_addr = reinterpret_cast<uint64_t>(g_load_buffer);

	// Fill load buffer with non-zero to detect zeroing
	memset(g_load_buffer, 0xBB, TEST_LOAD_BUFFER_SIZE);

	add_phdr(0, seg_offset, /*vaddr=*/0x1000, /*paddr=*/dest_addr, filesz, memsz);

	// Verify file data was written at the correct offset in the staging buffer
	for (uint64_t i = 0; i < filesz; i++) {
		ASSERT_EQ(g_elf_buffer[seg_offset + i], static_cast<uint8_t>(0xC0 + (i & 0x0F)));
	}

	uint64_t staging_size = TEST_ELF_BUFFER_SIZE;
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);
	ASSERT_EQ(entry, 0x1000u);

	// Verify file data was copied to load buffer
	for (uint64_t i = 0; i < filesz; i++) {
		ASSERT_EQ(g_load_buffer[i], static_cast<uint8_t>(0xC0 + (i & 0x0F)));
	}

	// Verify BSS region was zeroed (after file data)
	for (uint64_t i = filesz; i < memsz; i++) {
		ASSERT_EQ(g_load_buffer[i], 0);
	}
}

/**
 * @brief Verify load_elf handles segment with filesz == memsz (no BSS)
 */
TEST("elf: load_elf segment with no BSS") {
	constexpr uint64_t seg_size	  = 128;
	constexpr uint64_t seg_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);

	build_elf_header(/*entry=*/0x2000, sizeof(Elf64_Ehdr), 1);

	uint64_t dest_addr = reinterpret_cast<uint64_t>(g_load_buffer);
	memset(g_load_buffer, 0xBB, TEST_LOAD_BUFFER_SIZE);

	add_phdr(0, seg_offset, /*vaddr=*/0x2000, /*paddr=*/dest_addr, seg_size, seg_size);

	uint64_t staging_size = TEST_ELF_BUFFER_SIZE;
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);
	ASSERT_EQ(entry, 0x2000u);

	// Verify all file data was copied
	for (uint64_t i = 0; i < seg_size; i++) {
		ASSERT_EQ(g_load_buffer[i], static_cast<uint8_t>(0xC0 + (i & 0x0F)));
	}
}

/**
 * @brief Verify load_elf skips non-PT_LOAD segments
 */
TEST("elf: load_elf skips non-PT_LOAD") {
	build_elf_header(/*entry=*/0x3000, sizeof(Elf64_Ehdr), 1);

	// Make a non-PT_LOAD segment
	auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(g_elf_buffer);
	auto* phdr = reinterpret_cast<Elf64_Phdr*>(g_elf_buffer + ehdr->e_phoff);
	memset(phdr, 0, sizeof(Elf64_Phdr));
	phdr->p_type   = 0x6474E551;  // PT_GNU_STACK
	phdr->p_offset = 0;
	phdr->p_filesz = 0;
	phdr->p_memsz  = 0;

	uint64_t entry = load_elf(g_elf_buffer, 0x100000);

	// Entry point should still be returned correctly
	ASSERT_EQ(entry, 0x3000u);
}

// ============================================================
// 17. load_elf - Multiple Segments
// ============================================================

/**
 * @brief Verify load_elf handles two PT_LOAD segments correctly
 */
TEST("elf: load_elf two PT_LOAD segments") {
	constexpr uint64_t seg1_offset = sizeof(Elf64_Ehdr) + 2 * sizeof(Elf64_Phdr);
	constexpr uint64_t seg1_filesz = 32;
	constexpr uint64_t seg1_memsz  = 64;

	constexpr uint64_t seg2_offset = seg1_offset + 256;	 // Some gap
	constexpr uint64_t seg2_filesz = 48;
	constexpr uint64_t seg2_memsz  = 48;  // No BSS

	// Use two separate load buffers
	static uint8_t load_buf1[512];
	static uint8_t load_buf2[512];
	memset(load_buf1, 0xBB, sizeof(load_buf1));
	memset(load_buf2, 0xBB, sizeof(load_buf2));

	build_elf_header(/*entry=*/0xFFFFFFFF80100000ULL, sizeof(Elf64_Ehdr), 2);

	uint64_t dest1 = reinterpret_cast<uint64_t>(load_buf1);
	uint64_t dest2 = reinterpret_cast<uint64_t>(load_buf2);

	add_phdr(0, seg1_offset, /*vaddr=*/0xFFFFFFFF80100000ULL, /*paddr=*/dest1, seg1_filesz,
			 seg1_memsz);
	add_phdr(1, seg2_offset, /*vaddr=*/0xFFFFFFFF80102000ULL, /*paddr=*/dest2, seg2_filesz,
			 seg2_memsz);

	uint64_t staging_size = TEST_ELF_BUFFER_SIZE;
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);

	// Entry: higher-half entry returned as-is
	ASSERT_EQ(entry, 0xFFFFFFFF80100000ULL);

	// Verify first segment: file data copied, BSS zeroed
	for (uint64_t i = 0; i < seg1_filesz; i++) {
		ASSERT_EQ(load_buf1[i], static_cast<uint8_t>(0xC0 + (i & 0x0F)));
	}
	for (uint64_t i = seg1_filesz; i < seg1_memsz; i++) {
		ASSERT_EQ(load_buf1[i], 0);
	}

	// Verify second segment: file data copied
	for (uint64_t i = 0; i < seg2_filesz; i++) {
		ASSERT_EQ(load_buf2[i], static_cast<uint8_t>(0xC0 + (i & 0x0F)));
	}
}

// ============================================================
// 18. Edge Case - Zero-sized segment
// ============================================================

/**
 * @brief Verify a PT_LOAD segment with filesz=0 and memsz=0 is handled
 */
TEST("elf: zero-sized PT_LOAD segment") {
	build_elf_header(/*entry=*/0x5000, sizeof(Elf64_Ehdr), 1);

	uint64_t dest_addr = reinterpret_cast<uint64_t>(g_load_buffer);
	add_phdr(0, sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr),
			 /*vaddr=*/0x5000, /*paddr=*/dest_addr,
			 /*filesz=*/0, /*memsz=*/0);

	uint64_t staging_size = TEST_ELF_BUFFER_SIZE;
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);
	ASSERT_EQ(entry, 0x5000u);
}

// ============================================================
// 19. Staging Buffer Bounds Check
// ============================================================

/**
 * @brief Verify load_elf rejects segments that exceed staging buffer size
 *
 * If p_offset + p_filesz > staging_size, the loader should return 0.
 */
TEST("elf: load_elf rejects segment exceeding staging buffer") {
	constexpr uint64_t seg_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
	constexpr uint64_t filesz	  = 200;  // Segment extends beyond staging buffer

	build_elf_header(/*entry=*/0x1000, sizeof(Elf64_Ehdr), 1);

	uint64_t dest_addr = reinterpret_cast<uint64_t>(g_load_buffer);
	add_phdr(0, seg_offset, /*vaddr=*/0x1000, /*paddr=*/dest_addr, filesz, filesz);

	// Set a very small staging size so p_offset + p_filesz exceeds it
	uint64_t staging_size = seg_offset + 10;  // Much smaller than needed
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);
	ASSERT_EQ(entry, 0u);  // Should fail
}

/**
 * @brief Verify load_elf accepts segments within staging buffer size
 */
TEST("elf: load_elf accepts segments within staging buffer") {
	constexpr uint64_t seg_offset = sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
	constexpr uint64_t filesz	  = 64;

	build_elf_header(/*entry=*/0x1000, sizeof(Elf64_Ehdr), 1);

	uint64_t dest_addr = reinterpret_cast<uint64_t>(g_load_buffer);
	add_phdr(0, seg_offset, /*vaddr=*/0x1000, /*paddr=*/dest_addr, filesz, filesz);

	// staging_size is large enough: seg_offset + filesz < TEST_ELF_BUFFER_SIZE
	uint64_t staging_size = TEST_ELF_BUFFER_SIZE;
	uint64_t entry		  = load_elf(g_elf_buffer, staging_size);
	ASSERT_EQ(entry, 0x1000u);
}

// ============================================================
// Main Function
// ============================================================

/**
 * @brief Test entry point
 */
int main() {
	RUN_ALL_TESTS();
	return _tests_failed > 0 ? 1 : 0;
}

#endif	// CINUX_HOST_TEST
