/**
 * @file kernel/proc/execve.cpp
 * @brief execve() implementation — ELF loading and address space replacement
 *
 * Reads an ELF binary from the VFS, validates headers, unmaps existing
 * user-space pages, loads PT_LOAD segments into the task's address
 * space, and sets the new entry point.  Also contains the helper that
 * converts ELF validation errors to ExecveResult codes.
 */

#include <stddef.h>
#include <stdint.h>

#include <new>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/elf_types.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

// ============================================================
// Internal helpers for execve
// ============================================================

namespace {

ExecveResult elf_error_to_execve(elf::ElfValidateResult r) {
    using ER = elf::ElfValidateResult;
    switch (r) {
    case ER::BadMagic:
        return ExecveResult::BadElfMagic;
    case ER::BadClass:
        return ExecveResult::BadElfClass;
    case ER::BadEndian:
        return ExecveResult::BadElfEndian;
    case ER::BadMachine:
        return ExecveResult::BadElfMachine;
    case ER::BadType:
        return ExecveResult::BadElfType;
    case ER::BadPhoff:
    case ER::BadPhdrSize:
    case ER::NoPhdrs:
        return ExecveResult::BadElfHeaders;
    default:
        return ExecveResult::BadElfHeaders;
    }
}

/**
 * @brief Unmap all user-space pages from an address space
 *
 * Walks PML4 entries 0..255 and frees every mapped data page and
 * page table page.  Does NOT free the PML4 itself.
 */
void clear_user_mappings(cinux::mm::AddressSpace& space) {
    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
    uint64_t           pml4_phys  = space.pml4_phys();
    auto*              pml4 = reinterpret_cast<cinux::arch::PageEntry*>(pml4_phys + KERNEL_VMA);

    for (uint32_t i = 0; i < 256; i++) {
        if (!pml4[i].is_present())
            continue;

        auto* pdpt = reinterpret_cast<cinux::arch::PageEntry*>(pml4[i].phys_addr() + KERNEL_VMA);

        for (uint32_t j = 0; j < cinux::arch::PT_ENTRIES; j++) {
            if (!pdpt[j].is_present())
                continue;

            auto* pd = reinterpret_cast<cinux::arch::PageEntry*>(pdpt[j].phys_addr() + KERNEL_VMA);

            for (uint32_t k = 0; k < cinux::arch::PT_ENTRIES; k++) {
                if (!pd[k].is_present())
                    continue;

                auto* pt =
                    reinterpret_cast<cinux::arch::PageEntry*>(pd[k].phys_addr() + KERNEL_VMA);

                for (uint32_t l = 0; l < cinux::arch::PT_ENTRIES; l++) {
                    if (!pt[l].is_present())
                        continue;

                    uint64_t data_phys = pt[l].phys_addr();
                    cinux::mm::g_pmm.free_page(data_phys);
                    pt[l].raw = 0;
                }

                uint64_t pt_phys = pd[k].phys_addr();
                cinux::mm::g_pmm.free_page(pt_phys);
                pd[k].raw = 0;
            }

            uint64_t pd_phys = pdpt[j].phys_addr();
            cinux::mm::g_pmm.free_page(pd_phys);
            pdpt[j].raw = 0;
        }

        uint64_t pdpt_phys = pml4[i].phys_addr();
        cinux::mm::g_pmm.free_page(pdpt_phys);
        pml4[i].raw = 0;
    }
}

}  // anonymous namespace

// ============================================================
// execve implementation
// ============================================================

ExecveResult execve(const char* path, const char* const argv[], const char* const envp[]) {
    using namespace cinux::arch;

    (void)argv;
    (void)envp;

    if (path == nullptr || path[0] == '\0') {
        cinux::lib::kprintf("[EXECVE] invalid path\n");
        return ExecveResult::BadPath;
    }

    auto* task = Scheduler::current();
    if (task == nullptr) {
        cinux::lib::kprintf("[EXECVE] no current task\n");
        return ExecveResult::NoCurrentTask;
    }

    if (task->addr_space == nullptr) {
        cinux::lib::kprintf("[EXECVE] task has no address space\n");
        return ExecveResult::NoAddressSpace;
    }

    const char* rel_path = nullptr;
    auto*       fs       = cinux::fs::vfs_resolve(path, &rel_path);
    if (fs == nullptr) {
        cinux::lib::kprintf("[EXECVE] path not found: %s\n", path);
        return ExecveResult::FileNotFound;
    }

    auto* inode = fs->lookup(rel_path);
    if (inode == nullptr) {
        cinux::lib::kprintf("[EXECVE] inode not found: %s\n", rel_path);
        return ExecveResult::FileNotFound;
    }

    if (inode->type != cinux::fs::InodeType::Regular) {
        cinux::lib::kprintf("[EXECVE] not a regular file: %s\n", path);
        return ExecveResult::FileNotRegular;
    }

    constexpr uint64_t ELF_HEADER_SIZE = sizeof(elf::Elf64_Ehdr);
    if (inode->size < ELF_HEADER_SIZE) {
        cinux::lib::kprintf("[EXECVE] file too small for ELF header: %lu bytes\n",
                            static_cast<unsigned long>(inode->size));
        return ExecveResult::ReadFailed;
    }

    uint8_t ehdr_buf[ELF_HEADER_SIZE];
    if (inode->ops == nullptr) {
        cinux::lib::kprintf("[EXECVE] inode has no ops\n");
        return ExecveResult::ReadFailed;
    }

    int64_t nread = inode->ops->read(inode, 0, ehdr_buf, ELF_HEADER_SIZE);
    if (nread < static_cast<int64_t>(ELF_HEADER_SIZE)) {
        cinux::lib::kprintf("[EXECVE] failed to read ELF header\n");
        return ExecveResult::ReadFailed;
    }

    auto* ehdr = reinterpret_cast<const elf::Elf64_Ehdr*>(ehdr_buf);

    auto vr = elf::validate_elf_header(ehdr, inode->size);
    if (vr != elf::ElfValidateResult::Ok) {
        cinux::lib::kprintf("[EXECVE] ELF validation failed: %d\n", static_cast<int>(vr));
        return elf_error_to_execve(vr);
    }

    uint64_t phdr_offset = ehdr->e_phoff;
    uint16_t phnum       = ehdr->e_phnum;
    uint64_t phdr_bytes  = static_cast<uint64_t>(phnum) * sizeof(elf::Elf64_Phdr);
    auto*    phdrs       = new (std::align_val_t{alignof(elf::Elf64_Phdr)}) elf::Elf64_Phdr[phnum];
    if (phdrs == nullptr) {
        cinux::lib::kprintf("[EXECVE] failed to allocate phdr buffer\n");
        return ExecveResult::MapFailed;
    }

    nread = inode->ops->read(inode, phdr_offset, phdrs, phdr_bytes);
    if (nread < static_cast<int64_t>(phdr_bytes)) {
        cinux::lib::kprintf("[EXECVE] failed to read program headers\n");
        delete[] phdrs;
        return ExecveResult::ReadFailed;
    }

    clear_user_mappings(*task->addr_space);

    bool has_load_segment = false;

    for (uint16_t i = 0; i < phnum; i++) {
        const auto& phdr = phdrs[i];

        if (phdr.p_type != elf::PT_LOAD) {
            continue;
        }

        has_load_segment = true;

        uint64_t seg_start = phdr.p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = (phdr.p_vaddr + phdr.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        uint64_t page_flags = FLAG_PRESENT | FLAG_USER;
        if (phdr.p_flags & elf::PF_W) {
            page_flags |= FLAG_WRITABLE;
        }
        if (!(phdr.p_flags & elf::PF_X)) {
            page_flags |= FLAG_NX;
        }

        for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
            uint64_t phys = cinux::mm::g_pmm.alloc_page();
            if (phys == 0) {
                cinux::lib::kprintf("[EXECVE] page alloc failed at vaddr=%p\n",
                                    reinterpret_cast<void*>(vaddr));
                delete[] phdrs;
                return ExecveResult::MapFailed;
            }

            auto* dst = reinterpret_cast<uint8_t*>(phys + KERNEL_VMA);
            for (uint64_t b = 0; b < PAGE_SIZE; b++) {
                dst[b] = 0;
            }

            uint64_t data_vaddr  = (vaddr < phdr.p_vaddr) ? phdr.p_vaddr : vaddr;
            uint64_t in_page_off = data_vaddr - vaddr;
            uint64_t seg_offset  = data_vaddr - phdr.p_vaddr;

            if (seg_offset < phdr.p_filesz) {
                uint64_t copy_len = phdr.p_filesz - seg_offset;
                uint64_t avail    = PAGE_SIZE - in_page_off;
                if (copy_len > avail) {
                    copy_len = avail;
                }

                int64_t bread = inode->ops->read(inode, phdr.p_offset + seg_offset,
                                                 dst + in_page_off, copy_len);
                if (bread < static_cast<int64_t>(copy_len)) {
                    cinux::lib::kprintf("[EXECVE] segment read failed at offset %lu\n",
                                        static_cast<unsigned long>(phdr.p_offset + seg_offset));
                    cinux::mm::g_pmm.free_page(phys);
                    delete[] phdrs;
                    return ExecveResult::ReadFailed;
                }
            }

            if (!task->addr_space->map(vaddr, phys, page_flags)) {
                cinux::lib::kprintf("[EXECVE] map failed at vaddr=%p\n",
                                    reinterpret_cast<void*>(vaddr));
                cinux::mm::g_pmm.free_page(phys);
                delete[] phdrs;
                return ExecveResult::MapFailed;
            }
        }
    }

    delete[] phdrs;

    if (!has_load_segment) {
        cinux::lib::kprintf("[EXECVE] no PT_LOAD segments found\n");
        return ExecveResult::NoLoadSegments;
    }

    task->ctx.rip = ehdr->e_entry;

    cinux::lib::kprintf("[EXECVE] loaded %s entry=%p pid=%d\n", path,
                        reinterpret_cast<void*>(ehdr->e_entry), task->pid);

    return ExecveResult::Ok;
}

}  // namespace cinux::proc
