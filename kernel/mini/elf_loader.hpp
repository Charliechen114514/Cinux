/**
 * @file kernel/mini/elf_loader.hpp
 * @brief ELF64 Binary Parser and Loader
 *
 * Provides functions to validate, inspect, and load ELF64 binaries in memory.
 * This module is used by the big kernel loader to parse the ELF headers of
 * the "big kernel" binary that has been read from disk into a staging buffer,
 * then relocate its PT_LOAD segments to their proper virtual/physical addresses.
 *
 * ELF64 Standard Structures:
 *   - Elf64_Ehdr: ELF header (64 bytes) - magic, class, entry point, phdr offset
 *   - Elf64_Phdr: Program header (56 bytes) - segment type, flags, offsets, sizes
 *   - PT_LOAD:    Loadable segment type (type = 1) - segment to be mapped into memory
 *
 * Loading Process:
 *   1. Validate the ELF header (magic number, architecture, type)
 *   2. Iterate through program headers to find PT_LOAD segments
 *   3. Calculate total memory footprint of the kernel
 *   4. Copy each PT_LOAD segment from the staging buffer to its target address
 *   5. Zero-fill BSS (memsz > filesz portion of each segment)
 *   6. Return the entry point address from the ELF header
 *
 * @note This module operates on physical addresses. The mini kernel runs
 *       in long mode with identity mapping, so virtual addresses in the ELF
 *       headers must be converted to physical for loading.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::mini::elf_loader {

// ============================================================
// ELF64 Type Definitions
// ============================================================

/// ELF magic number: 0x7F 'E' 'L' 'F'
constexpr uint32_t ELF_MAGIC = 0x464C457F;

/// ELF class: 64-bit
constexpr uint8_t ELF_CLASS_64 = 2;

/// ELF data encoding: little-endian
constexpr uint8_t ELF_DATA_LSB = 1;

/// ELF OS/ABI: System V
constexpr uint8_t ELF_OSABI_SYSV = 0;

/// ELF object type: executable
constexpr uint16_t ET_EXEC = 2;

/// ELF machine architecture: x86-64
constexpr uint16_t EM_X86_64 = 62;

/// Program header type: loadable segment
constexpr uint32_t PT_LOAD = 1;

/// Program header flag: executable
constexpr uint32_t PF_X = 1;

/// Program header flag: writable
constexpr uint32_t PF_W = 2;

/// Program header flag: readable
constexpr uint32_t PF_R = 4;

// ============================================================
// ELF64 Header Structures (from ELF64 specification)
// ============================================================

/**
 * @brief ELF64 file header (64 bytes)
 *
 * This structure represents the ELF64 file header, located at the very
 * beginning of an ELF binary. It identifies the file format, target
 * architecture, and provides offsets to the program and section headers.
 */
struct Elf64_Ehdr {
    uint8_t  e_ident[16];  ///< ELF identification bytes (magic, class, data, etc.)
    uint16_t e_type;       ///< Object file type (ET_EXEC for executable)
    uint16_t e_machine;    ///< Target architecture (EM_X86_64)
    uint32_t e_version;    ///< Object file version (EV_CURRENT = 1)
    uint64_t e_entry;      ///< Virtual entry point address
    uint64_t e_phoff;      ///< Program header table file offset
    uint64_t e_shoff;      ///< Section header table file offset
    uint32_t e_flags;      ///< Processor-specific flags
    uint16_t e_ehsize;     ///< ELF header size (64 bytes)
    uint16_t e_phentsize;  ///< Program header entry size (56 bytes)
    uint16_t e_phnum;      ///< Number of program header entries
    uint16_t e_shentsize;  ///< Section header entry size
    uint16_t e_shnum;      ///< Number of section header entries
    uint16_t e_shstrndx;   ///< Section name string table index
} __attribute__((packed));

/**
 * @brief ELF64 program header (56 bytes)
 *
 * Each program header describes a segment that the loader must process.
 * PT_LOAD segments contain code or data that must be loaded into memory.
 */
struct Elf64_Phdr {
    uint32_t p_type;    ///< Segment type (PT_LOAD = 1 for loadable)
    uint32_t p_flags;   ///< Segment flags (PF_R | PF_W | PF_X)
    uint64_t p_offset;  ///< Segment file offset (start of segment data in the ELF file)
    uint64_t p_vaddr;   ///< Segment virtual address (target load address)
    uint64_t p_paddr;   ///< Segment physical address (often same as vaddr)
    uint64_t p_filesz;  ///< Segment size in the file (bytes to copy)
    uint64_t p_memsz;   ///< Segment size in memory (filesz + BSS zero-fill)
    uint64_t p_align;   ///< Segment alignment (usually 0x1000 = 4KB)
} __attribute__((packed));

// ============================================================
// ELF Validation
// ============================================================

/**
 * @brief Validate an ELF64 header
 *
 * Checks that the provided buffer starts with a valid ELF64 header
 * suitable for x86_64 execution. Verifies the magic number, class (64-bit),
 * data encoding (little-endian), machine type (x86-64), and object type
 * (executable).
 *
 * @param elf  Pointer to the ELF binary in memory (must be at least 64 bytes)
 * @return true if the header is a valid ELF64 x86_64 executable, false otherwise
 *
 * @note This function does not print error messages for invalid headers;
 *       the caller should log diagnostics if needed.
 */
bool parse_elf_header(const void* elf);

// ============================================================
// ELF Inspection
// ============================================================

/**
 * @brief Calculate the total memory size required by all PT_LOAD segments
 *
 * Iterates through all program headers in the ELF binary, finds the PT_LOAD
 * segments, and calculates the total span of memory they would occupy when
 * loaded. This is useful for pre-checking that the load address region is
 * large enough before attempting to load.
 *
 * @param ehdr  Pointer to a validated ELF64 header
 * @return Total memory size in bytes (span from lowest to highest PT_LOAD address)
 *         or 0 if no PT_LOAD segments are found
 */
size_t calculate_kernel_size(const Elf64_Ehdr* ehdr);

// ============================================================
// ELF Loading
// ============================================================

/**
 * @brief Load an ELF64 binary into memory and return its entry point
 *
 * Parses the ELF header, iterates through program headers, and for each
 * PT_LOAD segment:
 *   1. Copies p_filesz bytes from (elf_src + p_offset) to p_paddr
 *   2. Zero-fills the remaining (p_memsz - p_filesz) bytes (BSS section)
 *
 * The @p staging_size parameter specifies the size of the staging buffer in
 * bytes. This is used to validate that segment data (p_offset + p_filesz) does
 * not exceed the buffer, preventing out-of-bounds reads when the ELF binary
 * is larger than the number of sectors actually read from disk.
 *
 * @param elf_src       Pointer to the ELF binary in the staging buffer
 * @param staging_size  Size of the staging buffer in bytes (for bounds checking)
 * @return Entry point physical address on success, 0 on failure
 *
 * @note The caller must ensure the staging buffer is intact during the call
 *       and that the target memory region is available. Segment destinations
 *       are determined by p_paddr from the ELF headers, not by staging_size.
 */
uint64_t load_elf(void* elf_src, uint64_t staging_size);

}  // namespace cinux::mini::elf_loader
