/**
 * @file kernel/proc/elf_types.hpp
 * @brief ELF64 type definitions for user-space executable loading
 *
 * Defines ELF64 header structures, program header structures, and
 * validation constants used by the execve() path to load user-space
 * executables from the VFS into a process address space.
 *
 * Namespace: cinux::proc::elf
 */

#pragma once

#include <stdint.h>

namespace cinux::proc::elf {

// ============================================================
// ELF64 magic and class constants
// ============================================================

/// ELF magic number: 0x7F 'E' 'L' 'F'
constexpr uint32_t ELF_MAGIC = 0x464C457F;

/// ELF class: 64-bit
constexpr uint8_t ELF_CLASS_64 = 2;

/// ELF data encoding: little-endian
constexpr uint8_t ELF_DATA_LSB = 1;

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
// ELF64 header structure (64 bytes, packed)
// ============================================================

/**
 * @brief ELF64 file header
 *
 * Located at the very beginning of every ELF binary.  Identifies the
 * file format, target architecture, and provides offsets to the
 * program and section header tables.
 */
struct Elf64_Ehdr {
    uint8_t  e_ident[16];  ///< ELF identification bytes
    uint16_t e_type;       ///< Object file type (ET_EXEC = 2)
    uint16_t e_machine;    ///< Target architecture (EM_X86_64 = 62)
    uint32_t e_version;    ///< Object file version
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

static_assert(sizeof(Elf64_Ehdr) == 64, "Elf64_Ehdr must be 64 bytes");

// ============================================================
// ELF64 program header structure (56 bytes, packed)
// ============================================================

/**
 * @brief ELF64 program header
 *
 * Each program header describes a segment or other information the
 * system needs to prepare the program for execution.  PT_LOAD
 * segments contain code or data to be mapped into the process
 * address space.
 */
struct Elf64_Phdr {
    uint32_t p_type;    ///< Segment type (PT_LOAD = 1)
    uint32_t p_flags;   ///< Segment flags (PF_R | PF_W | PF_X)
    uint64_t p_offset;  ///< Segment file offset
    uint64_t p_vaddr;   ///< Segment virtual address (target load address)
    uint64_t p_paddr;   ///< Segment physical address
    uint64_t p_filesz;  ///< Segment size in the file (bytes to copy)
    uint64_t p_memsz;   ///< Segment size in memory (filesz + BSS)
    uint64_t p_align;   ///< Segment alignment (usually 0x1000)
} __attribute__((packed));

static_assert(sizeof(Elf64_Phdr) == 56, "Elf64_Phdr must be 56 bytes");

// ============================================================
// ELF validation result codes
// ============================================================

/**
 * @brief Result codes from ELF header validation
 */
enum class ElfValidateResult : int {
    Ok = 0,       ///< Valid ELF64 x86_64 executable
    BadMagic,     ///< Magic number mismatch
    BadClass,     ///< Not a 64-bit ELF
    BadEndian,    ///< Not little-endian
    BadMachine,   ///< Not x86-64
    BadType,      ///< Not an executable
    BadPhoff,     ///< Program header offset too large
    BadPhdrSize,  ///< Program header entry size is not 56
    NoPhdrs,      ///< No program headers
};

// ============================================================
// ELF validation
// ============================================================

/**
 * @brief Validate an ELF64 header for x86_64 executable loading
 *
 * Checks the magic number, class (64-bit), data encoding (little-endian),
 * machine type (x86-64), and object type (executable).
 *
 * @param ehdr  Pointer to the ELF header to validate
 * @param total_size  Total size of the buffer containing the ELF data
 * @return ElfValidateResult::Ok on success, or an error code
 */
ElfValidateResult validate_elf_header(const Elf64_Ehdr* ehdr, uint64_t total_size);

}  // namespace cinux::proc::elf
