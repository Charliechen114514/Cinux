/**
 * @file kernel/proc/elf_types.cpp
 * @brief ELF64 validation implementation
 */

#include "kernel/proc/elf_types.hpp"

namespace cinux::proc::elf {

// ============================================================
// ELF validation
// ============================================================

ElfValidateResult validate_elf_header(const Elf64_Ehdr* ehdr, uint64_t total_size) {
    // Check minimum size
    if (total_size < sizeof(Elf64_Ehdr)) {
        return ElfValidateResult::BadMagic;
    }

    // Check magic number: 0x7F 'E' 'L' 'F'
    uint32_t magic = static_cast<uint32_t>(ehdr->e_ident[0]) |
                     (static_cast<uint32_t>(ehdr->e_ident[1]) << 8) |
                     (static_cast<uint32_t>(ehdr->e_ident[2]) << 16) |
                     (static_cast<uint32_t>(ehdr->e_ident[3]) << 24);
    if (magic != ELF_MAGIC) {
        return ElfValidateResult::BadMagic;
    }

    // Check class: must be 64-bit
    if (ehdr->e_ident[4] != ELF_CLASS_64) {
        return ElfValidateResult::BadClass;
    }

    // Check data encoding: must be little-endian
    if (ehdr->e_ident[5] != ELF_DATA_LSB) {
        return ElfValidateResult::BadEndian;
    }

    // Check machine: must be x86-64
    if (ehdr->e_machine != EM_X86_64) {
        return ElfValidateResult::BadMachine;
    }

    // Check type: must be executable
    if (ehdr->e_type != ET_EXEC) {
        return ElfValidateResult::BadType;
    }

    // Check program header offset is within the file
    if (ehdr->e_phoff > total_size) {
        return ElfValidateResult::BadPhoff;
    }

    // Check program header entry size
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr)) {
        return ElfValidateResult::BadPhdrSize;
    }

    // Check there is at least one program header
    if (ehdr->e_phnum == 0) {
        return ElfValidateResult::NoPhdrs;
    }

    return ElfValidateResult::Ok;
}

}  // namespace cinux::proc::elf
