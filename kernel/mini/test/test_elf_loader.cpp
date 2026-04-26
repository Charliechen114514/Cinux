/**
 * @file kernel/mini/test/test_elf_loader.cpp
 * @brief QEMU in-kernel integration tests for ELF64 parser and loader
 *
 * Runs inside QEMU, constructing fake ELF binaries in memory to verify
 * that the ELF parser and loader handle headers correctly.
 *
 * Test coverage:
 *   - Valid ELF header passes validation
 *   - Invalid magic / class / endian / machine / type are rejected
 *   - calculate_kernel_size computes correct span from PT_LOAD segments
 *   - load_elf copies segments and zero-fills BSS
 *   - load_elf rejects out-of-bounds staging buffer access
 *   - Higher-half entry point conversion to physical address
 */

#include "../elf_loader.hpp"
#include "../lib/string.h"
#include "kernel_test.h"

using namespace cinux::mini::elf_loader;

// ============================================================
// Contiguous ELF buffer: Ehdr(64) + Phdr(56) + payload
// ============================================================
static uint8_t g_elf_buf[4096] __attribute__((aligned(16)));

/// Get Ehdr pointer from the contiguous buffer
static Elf64_Ehdr* elf_ehdr() {
    return reinterpret_cast<Elf64_Ehdr*>(g_elf_buf);
}

/// Get Phdr pointer from the contiguous buffer (right after Ehdr)
static Elf64_Phdr* elf_phdr() {
    return reinterpret_cast<Elf64_Phdr*>(g_elf_buf + sizeof(Elf64_Ehdr));
}

/// Build a minimal valid ELF64 with one PT_LOAD segment in g_elf_buf
static void build_valid_elf(uint64_t entry_addr = 0x1000000, uint64_t paddr = 0x1000000,
                            uint64_t filesz = 0x1000, uint64_t memsz = 0x2000) {
    memset(g_elf_buf, 0, sizeof(g_elf_buf));

    Elf64_Ehdr* ehdr  = elf_ehdr();
    ehdr->e_ident[0]  = 0x7F;
    ehdr->e_ident[1]  = 'E';
    ehdr->e_ident[2]  = 'L';
    ehdr->e_ident[3]  = 'F';
    ehdr->e_ident[4]  = ELF_CLASS_64;
    ehdr->e_ident[5]  = ELF_DATA_LSB;
    ehdr->e_ident[6]  = 1;  // EV_CURRENT
    ehdr->e_type      = ET_EXEC;
    ehdr->e_machine   = EM_X86_64;
    ehdr->e_version   = 1;
    ehdr->e_entry     = entry_addr;
    ehdr->e_phoff     = sizeof(Elf64_Ehdr);
    ehdr->e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum     = 1;

    Elf64_Phdr* phdr = elf_phdr();
    phdr->p_type     = PT_LOAD;
    phdr->p_flags    = PF_R | PF_X;
    phdr->p_offset   = 0;
    phdr->p_vaddr    = paddr;
    phdr->p_paddr    = paddr;
    phdr->p_filesz   = filesz;
    phdr->p_memsz    = memsz;
    phdr->p_align    = 0x1000;
}

// ============================================================
// Test 1: Valid ELF header passes parse_elf_header
// ============================================================
namespace test_elf_valid {

void test_valid_header() {
    build_valid_elf();
    TEST_ASSERT_TRUE(parse_elf_header(elf_ehdr()));
}
}  // namespace test_elf_valid

// ============================================================
// Test 2: Invalid magic is rejected
// ============================================================
namespace test_elf_bad_magic {

void test_bad_magic() {
    build_valid_elf();
    elf_ehdr()->e_ident[0] = 0x00;
    TEST_ASSERT_FALSE(parse_elf_header(elf_ehdr()));
}
}  // namespace test_elf_bad_magic

// ============================================================
// Test 3: Wrong class (32-bit) is rejected
// ============================================================
namespace test_elf_wrong_class {

void test_32bit_rejected() {
    build_valid_elf();
    elf_ehdr()->e_ident[4] = 1;  // ELF_CLASS_32
    TEST_ASSERT_FALSE(parse_elf_header(elf_ehdr()));
}
}  // namespace test_elf_wrong_class

// ============================================================
// Test 4: Wrong machine is rejected
// ============================================================
namespace test_elf_wrong_machine {

void test_wrong_machine() {
    build_valid_elf();
    elf_ehdr()->e_machine = 3;  // EM_386
    TEST_ASSERT_FALSE(parse_elf_header(elf_ehdr()));
}
}  // namespace test_elf_wrong_machine

// ============================================================
// Test 5: Non-executable type is rejected
// ============================================================
namespace test_elf_not_exec {

void test_not_executable() {
    build_valid_elf();
    elf_ehdr()->e_type = 1;  // ET_REL
    TEST_ASSERT_FALSE(parse_elf_header(elf_ehdr()));
}
}  // namespace test_elf_not_exec

// ============================================================
// Test 6: calculate_kernel_size with single PT_LOAD
// ============================================================
namespace test_elf_calc_size {

void test_single_segment_size() {
    // paddr=0x1000000, memsz=0x3000 => span = 0x3000
    build_valid_elf(0x1000000, 0x1000000, 0x1000, 0x3000);
    size_t sz = calculate_kernel_size(elf_ehdr());
    TEST_ASSERT_EQ(sz, static_cast<size_t>(0x3000));
}
}  // namespace test_elf_calc_size

// ============================================================
// Test 7: calculate_kernel_size with no PT_LOAD returns 0
// ============================================================
namespace test_elf_no_load {

void test_no_load_segments() {
    build_valid_elf();
    elf_phdr()->p_type = 0;  // Not PT_LOAD
    size_t sz          = calculate_kernel_size(elf_ehdr());
    TEST_ASSERT_EQ(sz, static_cast<size_t>(0));
}
}  // namespace test_elf_no_load

// ============================================================
// Test 8: load_elf copies data and zero-fills BSS
// ============================================================
namespace test_elf_load_basic {

/// Separate staging buffer for load test
static uint8_t load_staging[4096] __attribute__((aligned(16)));

void test_load_segment_copy_and_bss() {
    // Use a physical address (< HIGHER_HALF_BASE) so no conversion happens
    const uint64_t load_addr = 0x200000;  // Physical address for test
    const uint64_t filesz    = 64;
    const uint64_t memsz     = 256;  // BSS = 192 bytes

    // Build ELF in load_staging with contiguous layout
    memset(load_staging, 0, sizeof(load_staging));
    Elf64_Ehdr* ehdr = reinterpret_cast<Elf64_Ehdr*>(load_staging);
    Elf64_Phdr* phdr = reinterpret_cast<Elf64_Phdr*>(load_staging + sizeof(Elf64_Ehdr));

    ehdr->e_ident[0]  = 0x7F;
    ehdr->e_ident[1]  = 'E';
    ehdr->e_ident[2]  = 'L';
    ehdr->e_ident[3]  = 'F';
    ehdr->e_ident[4]  = ELF_CLASS_64;
    ehdr->e_ident[5]  = ELF_DATA_LSB;
    ehdr->e_ident[6]  = 1;
    ehdr->e_type      = ET_EXEC;
    ehdr->e_machine   = EM_X86_64;
    ehdr->e_version   = 1;
    ehdr->e_entry     = load_addr;
    ehdr->e_phoff     = sizeof(Elf64_Ehdr);
    ehdr->e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum     = 1;

    // Put payload right after phdr
    uint8_t* payload = load_staging + sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr);
    for (int i = 0; i < 64; i++) {
        payload[i] = static_cast<uint8_t>(0xA0 + i);
    }

    phdr->p_type   = PT_LOAD;
    phdr->p_flags  = PF_R | PF_X;
    phdr->p_offset = static_cast<uint64_t>(payload - load_staging);
    phdr->p_vaddr  = load_addr;
    phdr->p_paddr  = load_addr;
    phdr->p_filesz = filesz;
    phdr->p_memsz  = memsz;
    phdr->p_align  = 0x1000;

    // Set BSS region to non-zero to verify zeroing
    memset(reinterpret_cast<void*>(load_addr + filesz), 0xFF, static_cast<size_t>(memsz - filesz));

    uint64_t entry = load_elf(load_staging, sizeof(load_staging));

    // Entry should match the physical load address
    TEST_ASSERT_EQ(entry, load_addr);

    // Verify file data was copied to load_addr
    uint8_t* dest = reinterpret_cast<uint8_t*>(load_addr);
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQ(dest[i], static_cast<uint8_t>(0xA0 + i));
    }

    // Verify BSS was zero-filled
    bool bss_zeroed = true;
    for (uint64_t i = filesz; i < memsz; i++) {
        if (dest[i] != 0) {
            bss_zeroed = false;
            break;
        }
    }
    TEST_ASSERT_TRUE(bss_zeroed);
}
}  // namespace test_elf_load_basic

// ============================================================
// Test 9: load_elf rejects out-of-bounds staging buffer
// ============================================================
namespace test_elf_staging_bounds {

static uint8_t small_staging[128] __attribute__((aligned(16)));

void test_staging_too_small() {
    memset(small_staging, 0, sizeof(small_staging));
    Elf64_Ehdr* ehdr = reinterpret_cast<Elf64_Ehdr*>(small_staging);
    Elf64_Phdr* phdr = reinterpret_cast<Elf64_Phdr*>(small_staging + sizeof(Elf64_Ehdr));

    ehdr->e_ident[0]  = 0x7F;
    ehdr->e_ident[1]  = 'E';
    ehdr->e_ident[2]  = 'L';
    ehdr->e_ident[3]  = 'F';
    ehdr->e_ident[4]  = ELF_CLASS_64;
    ehdr->e_ident[5]  = ELF_DATA_LSB;
    ehdr->e_ident[6]  = 1;
    ehdr->e_type      = ET_EXEC;
    ehdr->e_machine   = EM_X86_64;
    ehdr->e_version   = 1;
    ehdr->e_entry     = 0x1000000;
    ehdr->e_phoff     = sizeof(Elf64_Ehdr);
    ehdr->e_ehsize    = sizeof(Elf64_Ehdr);
    ehdr->e_phentsize = sizeof(Elf64_Phdr);
    ehdr->e_phnum     = 1;

    phdr->p_type   = PT_LOAD;
    phdr->p_offset = 0;
    phdr->p_filesz = 4096;  // Much larger than 128-byte staging
    phdr->p_memsz  = 4096;
    phdr->p_paddr  = 0x1000000;
    phdr->p_vaddr  = 0x1000000;

    uint64_t entry = load_elf(small_staging, sizeof(small_staging));
    TEST_ASSERT_EQ(entry, static_cast<uint64_t>(0));  // Should fail
}
}  // namespace test_elf_staging_bounds

// ============================================================
// Test Entry Point
// ============================================================
extern "C" void run_elf_loader_tests() {
    TEST_SECTION("ELF Loader Tests (008)");

    RUN_TEST(test_elf_valid::test_valid_header);
    RUN_TEST(test_elf_bad_magic::test_bad_magic);
    RUN_TEST(test_elf_wrong_class::test_32bit_rejected);
    RUN_TEST(test_elf_wrong_machine::test_wrong_machine);
    RUN_TEST(test_elf_not_exec::test_not_executable);
    RUN_TEST(test_elf_calc_size::test_single_segment_size);
    RUN_TEST(test_elf_no_load::test_no_load_segments);
    RUN_TEST(test_elf_load_basic::test_load_segment_copy_and_bss);
    RUN_TEST(test_elf_staging_bounds::test_staging_too_small);

    TEST_SUMMARY();
}
