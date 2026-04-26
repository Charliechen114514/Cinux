/**
 * @file kernel/mini/big_kernel_loader.hpp
 * @brief Big Kernel Loader - Orchestrates Disk Read + ELF Loading
 *
 * This module ties together the ATA disk driver and ELF loader to load
 * the "big kernel" from disk into memory.
 *
 * Loading Strategy (two-phase):
 *   Phase 1: Read just enough sectors to capture the ELF header and all
 *            program headers, then compute the total ELF file size.
 *   Phase 2: Extend paging, check memory overlaps, read the full ELF,
 *            and load PT_LOAD segments to their target addresses.
 *
 * Memory Layout During Loading:
 *   - 0x20000: Mini kernel (loaded by bootloader)
 *   - 0x1000000 (16MB): Staging buffer for raw ELF binary from disk
 *   - After ELF parsing: PT_LOAD segments placed at physical targets
 */

#pragma once

#include <stdint.h>

#include "elf_loader.hpp"

namespace cinux::mini::loader {

// ============================================================
// Constants
// ============================================================

/// Physical address where the mini kernel was loaded by the bootloader
constexpr uint64_t MINI_KERNEL_LOAD_ADDR = 0x20000;

/// Physical address where the big kernel ELF will be staged from disk
constexpr uint64_t BIG_KERNEL_LOAD_ADDR = 0x1000000;  // 16MB

/// Big kernel higher-half virtual base (must match kernel/linker.ld KERNEL_VMA)
constexpr uint64_t BIG_KERNEL_VMA = 0xFFFFFFFF80000000ULL;

/// Expected virtual entry point of the big kernel (_start)
constexpr uint64_t BIG_KERNEL_ENTRY_VADDR = BIG_KERNEL_VMA + BIG_KERNEL_LOAD_ADDR;

/// LBA on disk where the big kernel ELF binary starts
/// Must match the disk image layout in scripts/build_image.sh
constexpr uint64_t BIG_KERNEL_LBA = 848;

/// Number of sectors to read in Phase 1 (ELF header + program headers)
/// 16 sectors = 8192 bytes.  Covers ELF header (64B) + up to ~140
/// program headers (56B each), far more than any kernel needs.
constexpr uint16_t ELF_HEADER_SECTORS = 16;

/// Maximum number of program headers we support
constexpr uint16_t MAX_PROGRAM_HEADERS = 16;

/// Safety cap on ELF file size (1.25 GB).
/// Prevents a corrupted/malicious ELF header from causing an
/// out-of-bounds read of terabytes.
constexpr uint64_t MAX_ELF_FILE_SIZE = 0x50000000ULL;

// ============================================================
// Memory Overlap Checker
// ============================================================

/// Maximum number of regions the overlap checker can track
constexpr uint32_t MAX_MEMORY_REGIONS = 16;

/// Memory region descriptor for overlap checking
struct MemoryRegion {
    uint64_t    start;
    uint64_t    end;  // exclusive
    const char* name;
};

/**
 * @brief Check for memory region overlaps and print a memory map
 *
 * @param regions  Array of memory regions
 * @param count    Number of regions
 * @return true if no overlaps, false if any overlap detected
 */
bool check_memory_overlaps(const MemoryRegion* regions, uint32_t count);

// ============================================================
// Two-Phase Loading State
// ============================================================

/// State produced by Phase 1 and consumed by Phase 2
struct BigKernelLoadState {
    uint64_t               raw_elf_end;     ///< Actual end of ELF data (before sector alignment)
    uint64_t               total_elf_size;  ///< Total ELF file size (sector-aligned)
    uint32_t               total_sectors;   ///< Total sectors to read
    uint16_t               phnum;           ///< Number of program headers
    elf_loader::Elf64_Phdr phdrs[MAX_PROGRAM_HEADERS];  ///< Saved program headers
};

/**
 * @brief Phase 1: Read ELF headers and compute file size
 *
 * Reads ELF_HEADER_SECTORS from disk, validates the header, copies
 * program headers to @p state, and computes total_elf_size.
 *
 * @param disk_lba  Starting LBA of the big kernel
 * @param state     [out] Populated with header info
 * @return true on success, false on validation error
 */
bool load_big_kernel_phase1(uint64_t disk_lba, BigKernelLoadState& state);

/**
 * @brief Phase 2: Read full ELF + load segments + return entry point
 *
 * Must be called after a successful Phase 1.  Extends paging,
 * checks memory overlaps, reads the full ELF, and calls load_elf().
 *
 * @param state     State from Phase 1
 * @param disk_lba  Starting LBA of the big kernel
 * @return Virtual entry point on success, 0 on failure
 */
uint64_t load_big_kernel_phase2(const BigKernelLoadState& state, uint64_t disk_lba);

/**
 * @brief Convenience wrapper: Phase 1 + Phase 2 in one call
 */
uint64_t load_big_kernel(uint64_t disk_lba);

}  // namespace cinux::mini::loader
