/**
 * @file kernel/mini/big_kernel_loader.cpp
 * @brief Big Kernel Loader Implementation
 *
 * Two-phase loading:
 *   Phase 1: Read ELF header + program headers, compute file size
 *   Phase 2: Extend paging, check overlaps, read full ELF, load segments
 */

#include "big_kernel_loader.hpp"

#include <stdint.h>

#include "arch/x86_64/paging.hpp"
#include "driver/ata.hpp"
#include "elf_loader.hpp"
#include "lib/kprintf.h"

using cinux::mini::arch::identity_map_up_to;
using cinux::mini::driver::ata::read;
using cinux::mini::driver::ata::read_large;
using cinux::mini::driver::ata::ATA_SECTOR_SIZE;
using cinux::mini::elf_loader::Elf64_Ehdr;
using cinux::mini::elf_loader::Elf64_Phdr;
using cinux::mini::elf_loader::PT_LOAD;
using cinux::mini::lib::kprintf;

namespace cinux::mini::loader {

// ============================================================
// Helper: align up
// ============================================================

static uint64_t align_up(uint64_t val, uint64_t align) {
	return (val + align - 1) & ~(align - 1);
}

// ============================================================
// Memory Overlap Checker
// ============================================================

bool check_memory_overlaps(const MemoryRegion* regions, uint32_t count) {
	kprintf("\n=== Memory Layout ===\n");
	for (uint32_t i = 0; i < count; i++) {
		uint64_t size_kb = (regions[i].end - regions[i].start) / 1024;
		kprintf("  %s: 0x%08p - 0x%08p (%u KB)\n",
				regions[i].name,
				reinterpret_cast<const void*>(regions[i].start),
				reinterpret_cast<const void*>(regions[i].end),
				static_cast<uint32_t>(size_kb));
	}

	// Check all pairs for overlap
	bool ok = true;
	for (uint32_t i = 0; i < count; i++) {
		for (uint32_t j = i + 1; j < count; j++) {
			if (regions[i].start < regions[j].end &&
				regions[j].start < regions[i].end) {
				uint64_t overlap_start = regions[i].start > regions[j].start
					? regions[i].start : regions[j].start;
				uint64_t overlap_end = regions[i].end < regions[j].end
					? regions[i].end : regions[j].end;
				kprintf("  OVERLAP: '%s' and '%s' at 0x%p - 0x%p\n",
						regions[i].name, regions[j].name,
						reinterpret_cast<const void*>(overlap_start),
						reinterpret_cast<const void*>(overlap_end));
				ok = false;
			}
		}
	}

	if (ok) {
		kprintf("  [OK] No overlaps detected.\n");
	} else {
		kprintf("  [FAIL] Memory overlap detected!\n");
	}
	kprintf("=====================\n\n");
	return ok;
}

// ============================================================
// Phase 1: Read ELF Headers
// ============================================================

bool load_big_kernel_phase1(uint64_t disk_lba, BigKernelLoadState& state) {
	// Map enough for the header read
	constexpr uint32_t header_bytes =
		static_cast<uint32_t>(ELF_HEADER_SECTORS) * ATA_SECTOR_SIZE;
	identity_map_up_to(BIG_KERNEL_LOAD_ADDR + header_bytes);

	// Read header sectors into staging buffer
	kprintf("[LOADER] Phase 1: Reading %u sectors from LBA 0x%x...\n",
			ELF_HEADER_SECTORS, static_cast<uint32_t>(disk_lba));
	if (!read(disk_lba, ELF_HEADER_SECTORS,
			  reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR))) {
		kprintf("[LOADER] ERROR: Failed to read ELF header from disk!\n");
		return false;
	}

	// Validate ELF magic
	const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
	if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
		kprintf("[LOADER] ERROR: No ELF magic! Got: %02x %02x %02x %02x\n",
				magic[0], magic[1], magic[2], magic[3]);
		return false;
	}

	// Parse ELF header
	const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(BIG_KERNEL_LOAD_ADDR);
	state.phnum = ehdr->e_phnum;
	if (state.phnum > MAX_PROGRAM_HEADERS) {
		kprintf("[LOADER] ERROR: Too many program headers (%u, max %u)\n",
				state.phnum, MAX_PROGRAM_HEADERS);
		return false;
	}

	// Verify all program headers fit within the header read
	uint64_t phdr_end = ehdr->e_phoff +
		static_cast<uint64_t>(state.phnum) * ehdr->e_phentsize;
	if (phdr_end > header_bytes) {
		kprintf("[LOADER] ERROR: Program headers extend beyond header read "
				"(end=0x%p, read=0x%p). Increase ELF_HEADER_SECTORS.\n",
				reinterpret_cast<const void*>(phdr_end),
				reinterpret_cast<const void*>(header_bytes));
		return false;
	}

	// Copy program headers to local array
	for (uint16_t i = 0; i < state.phnum; i++) {
		const auto* phdr = reinterpret_cast<const Elf64_Phdr*>(
			reinterpret_cast<const uint8_t*>(ehdr) + ehdr->e_phoff
			+ static_cast<uint64_t>(i) * ehdr->e_phentsize);
		state.phdrs[i] = *phdr;
	}

	// Compute total ELF file size from program headers + section headers
	uint64_t max_end = 0;
	for (uint16_t i = 0; i < state.phnum; i++) {
		if (state.phdrs[i].p_type == PT_LOAD) {
			uint64_t seg_end = state.phdrs[i].p_offset + state.phdrs[i].p_filesz;
			if (seg_end > max_end) max_end = seg_end;
		}
	}
	// Also consider section header table
	uint64_t sh_end = ehdr->e_shoff +
		static_cast<uint64_t>(ehdr->e_shnum) * ehdr->e_shentsize;
	if (sh_end > max_end) max_end = sh_end;

	state.raw_elf_end = max_end;
	state.total_elf_size = align_up(max_end, ATA_SECTOR_SIZE);
	state.total_sectors = static_cast<uint32_t>(state.total_elf_size / ATA_SECTOR_SIZE);

	// Safety cap
	if (state.total_elf_size > MAX_ELF_FILE_SIZE) {
		kprintf("[LOADER] ERROR: ELF file too large (0x%p, max 0x%p)\n",
				reinterpret_cast<const void*>(state.total_elf_size),
				reinterpret_cast<const void*>(MAX_ELF_FILE_SIZE));
		return false;
	}

	kprintf("[LOADER] ELF file: %u bytes (%u sectors)\n",
			static_cast<uint32_t>(state.total_elf_size), state.total_sectors);
	return true;
}

// ============================================================
// Phase 2: Full Read + Load
// ============================================================

uint64_t load_big_kernel_phase2(const BigKernelLoadState& state, uint64_t disk_lba) {
	// Determine highest physical address we need to map
	uint64_t highest_phys = BIG_KERNEL_LOAD_ADDR + state.total_elf_size;
	for (uint16_t i = 0; i < state.phnum; i++) {
		if (state.phdrs[i].p_type == PT_LOAD) {
			uint64_t seg_end = state.phdrs[i].p_paddr + state.phdrs[i].p_memsz;
			if (seg_end > highest_phys) highest_phys = seg_end;
		}
	}
	highest_phys = align_up(highest_phys, arch::PAGE_2MB_SIZE);

	// Extend identity mapping
	kprintf("[LOADER] Mapping physical memory up to 0x%p...\n",
			reinterpret_cast<const void*>(highest_phys));
	identity_map_up_to(highest_phys);

	// Register memory regions and check overlaps.
	// Note: we intentionally do NOT register the staging buffer as a separate
	// region because it intentionally overlaps with PT_LOAD targets (in-place
	// loading).  The dangerous overlaps we want to catch are:
	//   - Mini kernel vs PT_LOAD targets (would corrupt running code)
	//   - Page tables vs anything
	//   - PT_LOAD targets vs each other
	MemoryRegion regions[MAX_MEMORY_REGIONS];
	uint32_t rcount = 0;

	// Page tables (fixed by bootloader)
	regions[rcount++] = {0x1000, 0x4000, "Page Tables"};

	// Mini kernel (approximate; exact end from linker symbol would be better)
	regions[rcount++] = {MINI_KERNEL_LOAD_ADDR, BIG_KERNEL_LOAD_ADDR, "Mini Kernel"};

	// PT_LOAD target regions
	for (uint16_t i = 0; i < state.phnum; i++) {
		if (state.phdrs[i].p_type == PT_LOAD && state.phdrs[i].p_memsz > 0) {
			regions[rcount].start = state.phdrs[i].p_paddr;
			regions[rcount].end = state.phdrs[i].p_paddr + state.phdrs[i].p_memsz;
			regions[rcount].name = "PT_LOAD target";
			rcount++;
			if (rcount >= MAX_MEMORY_REGIONS) break;
		}
	}

	// Print staging buffer info separately (not in overlap check)
	kprintf("[LOADER] Staging buffer: 0x%p - 0x%p (%u KB)\n",
			reinterpret_cast<const void*>(BIG_KERNEL_LOAD_ADDR),
			reinterpret_cast<const void*>(BIG_KERNEL_LOAD_ADDR + state.total_elf_size),
			static_cast<uint32_t>(state.total_elf_size / 1024));

	if (!check_memory_overlaps(regions, rcount)) {
		kprintf("[LOADER] FATAL: Memory overlap detected, aborting load!\n");
		return 0;
	}

	// Read full ELF into staging buffer
	kprintf("[LOADER] Phase 2: Reading %u sectors from disk...\n",
			state.total_sectors);
	if (!read_large(disk_lba, state.total_sectors,
					reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR))) {
		kprintf("[LOADER] ERROR: Failed to read full ELF from disk!\n");
		return 0;
	}

	// Load ELF segments
	uint64_t entry = elf_loader::load_elf(
		reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR), state.total_elf_size);
	if (entry == 0) {
		kprintf("[LOADER] ERROR: ELF loading failed!\n");
		return 0;
	}

	kprintf("[LOADER] Big kernel loaded successfully.\n");
	kprintf("[LOADER] Entry point: 0x%p\n", reinterpret_cast<void*>(entry));
	return entry;
}

// ============================================================
// Convenience Wrapper
// ============================================================

uint64_t load_big_kernel(uint64_t disk_lba) {
	BigKernelLoadState state;
	if (!load_big_kernel_phase1(disk_lba, state)) {
		return 0;
	}
	return load_big_kernel_phase2(state, disk_lba);
}

}  // namespace cinux::mini::loader
