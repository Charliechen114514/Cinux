/**
 * @file kernel/mini/elf_loader.cpp
 * @brief ELF64 Binary Parser and Loader Implementation
 *
 * Implements ELF64 header validation, size calculation, and segment loading
 * for the mini kernel's big kernel loader.
 */

#include "elf_loader.hpp"

#include <stddef.h>
#include <stdint.h>

#include "lib/kprintf.h"
#include "lib/string.h"

using cinux::mini::lib::kprintf;

namespace cinux::mini::elf_loader {

namespace {

/**
 * @brief Get the program header at the specified index
 * @param ehdr  Pointer to the ELF header
 * @param index Program header index
 * @return Pointer to the program header, or nullptr if index is out of bounds
 */
const Elf64_Phdr* get_phdr(const Elf64_Ehdr* ehdr, uint16_t index) {
	if (index >= ehdr->e_phnum) {
		return nullptr;
	}
	return reinterpret_cast<const Elf64_Phdr*>(reinterpret_cast<const uint8_t*>(ehdr) +
											   ehdr->e_phoff +
											   static_cast<uint64_t>(index) * ehdr->e_phentsize);
}

}  // anonymous namespace

// ============================================================
// ELF Validation
// ============================================================

bool parse_elf_header(const void* elf) {
	// Step 1: Check for null pointer
	if (elf == nullptr) {
		return false;
	}

	// Step 2: Cast to Elf64_Ehdr pointer
	const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf);

	// Step 3: Verify the ELF magic number (0x7F 'E' 'L' 'F')
	if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' || ehdr->e_ident[2] != 'L' ||
		ehdr->e_ident[3] != 'F') {
		kprintf("[ELF] ERROR: invalid magic: %02x %02x %02x %02x\n", ehdr->e_ident[0],
				ehdr->e_ident[1], ehdr->e_ident[2], ehdr->e_ident[3]);
		return false;
	}

	// Step 4: Verify ELF class is 64-bit
	if (ehdr->e_ident[4] != ELF_CLASS_64) {
		kprintf("[ELF] ERROR: not 64-bit ELF (class=%u)\n", ehdr->e_ident[4]);
		return false;
	}

	// Step 5: Verify data encoding is little-endian
	if (ehdr->e_ident[5] != ELF_DATA_LSB) {
		kprintf("[ELF] ERROR: not little-endian (encoding=%u)\n", ehdr->e_ident[5]);
		return false;
	}

	// Step 6: Verify target architecture is x86-64
	if (ehdr->e_machine != EM_X86_64) {
		kprintf("[ELF] ERROR: not x86-64 (machine=%u)\n", ehdr->e_machine);
		return false;
	}

	// Step 7: Verify object type is executable
	if (ehdr->e_type != ET_EXEC) {
		kprintf("[ELF] ERROR: not executable (type=%u)\n", ehdr->e_type);
		return false;
	}

	// Step 8: Verify program header table exists
	if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
		kprintf("[ELF] ERROR: no program headers\n");
		return false;
	}

	return true;
}

// ============================================================
// ELF Inspection
// ============================================================

size_t calculate_kernel_size(const Elf64_Ehdr* ehdr) {
	uint64_t lowest_addr  = UINT64_MAX;
	uint64_t highest_addr = 0;

	// Iterate through all program headers
	for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
		const Elf64_Phdr* phdr = get_phdr(ehdr, i);
		if (phdr == nullptr) {
			continue;
		}

		// Only process loadable segments
		if (phdr->p_type != PT_LOAD) {
			continue;
		}

		// Track the lowest and highest addresses
		if (phdr->p_paddr < lowest_addr) {
			lowest_addr = phdr->p_paddr;
		}
		uint64_t seg_end = phdr->p_paddr + phdr->p_memsz;
		if (seg_end > highest_addr) {
			highest_addr = seg_end;
		}
	}

	// Validate that PT_LOAD segments were found
	if (lowest_addr == UINT64_MAX) {
		return 0;
	}

	return static_cast<size_t>(highest_addr - lowest_addr);
}

// ============================================================
// ELF Loading
// ============================================================

uint64_t load_elf(void* elf_src, uint64_t staging_size) {
	// Step 1: Validate the ELF header
	if (!parse_elf_header(elf_src)) {
		kprintf("[ELF] ERROR: ELF header validation failed!\n");
		return 0;
	}

	// Step 2: Cast to Elf64_Ehdr pointer
	const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf_src);

	// Step 3: Save header fields before any segment copy.
	// Loading a PT_LOAD segment may write to p_paddr which can overlap
	// the staging buffer (e.g., when the kernel's physical base equals
	// the staging address).  Once that happens the ELF header in the
	// staging buffer is corrupted, so we must capture everything we
	// need up front.
	const uint64_t saved_entry	   = ehdr->e_entry;
	const uint16_t saved_phnum	   = ehdr->e_phnum;
	const uint64_t saved_phoff	   = ehdr->e_phoff;
	const uint16_t saved_phentsize = ehdr->e_phentsize;

	// Copy all program headers to local storage so they survive staging
	// buffer overwrites during the loading loop.
	if (saved_phnum > 16) {
		kprintf("[ELF] ERROR: too many program headers (%u)\n", saved_phnum);
		return 0;
	}
	Elf64_Phdr saved_phdrs[16];
	for (uint16_t i = 0; i < saved_phnum; i++) {
		const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(
			reinterpret_cast<const uint8_t*>(ehdr) + saved_phoff +
			static_cast<uint64_t>(i) * saved_phentsize);
		saved_phdrs[i] = *phdr;
	}

	// Step 4: Log ELF header information
	kprintf("[ELF] Entry point: 0x%p\n", saved_entry);
	kprintf("[ELF] Program headers: %u at offset 0x%p\n", saved_phnum, saved_phoff);

	// Step 5: Iterate through saved program headers and load PT_LOAD segments
	for (uint16_t i = 0; i < saved_phnum; i++) {
		const Elf64_Phdr& phdr = saved_phdrs[i];

		// Skip non-loadable segments
		if (phdr.p_type != PT_LOAD) {
			continue;
		}

		kprintf("[ELF] PT_LOAD[%u]: vaddr=0x%p paddr=0x%p filesz=0x%p memsz=0x%p\n", i,
				phdr.p_vaddr, phdr.p_paddr, phdr.p_filesz, phdr.p_memsz);

		// Validate that segment data lies within the staging buffer.
		if (phdr.p_offset + phdr.p_filesz > staging_size) {
			kprintf(
				"[ELF] ERROR: segment %u data exceeds staging buffer "
				"(offset=0x%p + filesz=0x%p > staging=0x%p)\n",
				i, phdr.p_offset, phdr.p_filesz, staging_size);
			return 0;
		}

		// Calculate destination address (physical target from p_paddr)
		uint64_t dest_addr = phdr.p_paddr;

		// Calculate source address within the staging buffer
		const void* src = reinterpret_cast<const uint8_t*>(elf_src) + phdr.p_offset;

		// Copy file data to destination
		if (phdr.p_filesz > 0) {
			memmove(reinterpret_cast<void*>(dest_addr), src, phdr.p_filesz);
		}

		// Zero-fill BSS (if memsz > filesz)
		if (phdr.p_memsz > phdr.p_filesz) {
			uint64_t bss_start = dest_addr + phdr.p_filesz;
			size_t	 bss_size  = static_cast<size_t>(phdr.p_memsz - phdr.p_filesz);
			memset(reinterpret_cast<void*>(bss_start), 0, bss_size);
		}

		kprintf(
			"[ELF] Loaded segment %u: 0x%p -> 0x%p (%u bytes, BSS %u bytes)\n", i, phdr.p_offset,
			dest_addr, phdr.p_filesz,
			phdr.p_memsz > phdr.p_filesz ? static_cast<uint32_t>(phdr.p_memsz - phdr.p_filesz) : 0);
	}

	// Step 6: Log successful load
	kprintf("[ELF] All PT_LOAD segments loaded.\n");

	// Step 7: Return the entry point address (higher-half virtual)
	return saved_entry;
}

}  // namespace cinux::mini::elf_loader
