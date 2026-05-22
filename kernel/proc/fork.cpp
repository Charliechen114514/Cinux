/**
 * @file kernel/proc/fork.cpp
 * @brief fork() implementation with Copy-On-Write page table cloning
 *
 * Creates a near-identical copy of the current task: new PID, new TCB,
 * new kernel stack (with parent stack contents copied), and CoW page
 * table setup so that user-space pages are shared read-only until
 * either process writes to them.
 */

#include <stddef.h>
#include <stdint.h>

#include <cstring>
#include <new>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/process_internal.hpp"
#include "kernel/proc/scheduler.hpp"
#include "proc/per_cpu.hpp"

namespace cinux::proc {

// ============================================================
// Internal helpers for CoW page table cloning
// ============================================================

namespace {

constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

using namespace cinux::arch;

PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}

/**
 * @brief Recursively copy a page table level for CoW fork
 *
 * At the PT (leaf) level, shares physical pages and marks writable
 * entries as read-only with FLAG_COW.  At intermediate levels, allocates
 * new page table pages and recurses.
 */
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    auto* src_table = phys_to_virt(src_phys);
    auto* dst_table = phys_to_virt(dst_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!src_table[i].is_present())
            continue;

        if (!(src_table[i].raw & FLAG_USER))
            continue;

        if (src_table[i].huge) {
            dst_table[i].raw = src_table[i].raw;
            continue;
        }

        if (level > 1) {
            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            if (new_page == 0) {
                cinux::lib::kprintf("[PROC] fork: intermediate PT alloc failed\n");
                continue;
            }

            auto* new_table = phys_to_virt(new_page);
            for (uint32_t j = 0; j < PT_ENTRIES; j++) {
                new_table[j].raw = 0;
            }

            dst_table[i].raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
            copy_page_table_level(src_table[i].phys_addr(), new_page, level - 1);
        } else {
            uint64_t entry_flags = src_table[i].raw & FLAG_MASK;

            dst_table[i].raw = src_table[i].raw;

            if (entry_flags & FLAG_WRITABLE) {
                dst_table[i].raw &= ~FLAG_WRITABLE;
                dst_table[i].raw |= FLAG_COW;

                src_table[i].raw &= ~FLAG_WRITABLE;
                src_table[i].raw |= FLAG_COW;
            }
        }
    }
}

}  // anonymous namespace

// ============================================================
// fork implementation
// ============================================================

__attribute__((optimize("no-omit-frame-pointer"), noinline)) int fork(PidAllocator& pid_alloc) {
    auto* parent = Scheduler::current();
    if (parent == nullptr) {
        cinux::lib::kprintf("[PROC] fork: no current task\n");
        return -1;
    }

    int child_pid = pid_alloc.alloc();
    if (child_pid == PidAllocator::PID_NONE) {
        cinux::lib::kprintf("[PROC] fork: PID allocator exhausted\n");
        return -1;
    }

    auto* child = new (std::align_val_t{alignof(Task)}) Task;
    if (child == nullptr) {
        cinux::lib::kprintf("[PROC] fork: TCB allocation failed\n");
        pid_alloc.free(child_pid);
        return -1;
    }

    std::memcpy(child, parent, sizeof(Task));

    child->tid         = next_tid.fetch_add(1, cinux::lib::MemoryOrder::Relaxed);
    child->pid         = child_pid;
    child->ppid        = parent->pid;
    child->state       = TaskState::Ready;
    child->parent      = parent;
    child->children    = nullptr;
    child->exit_status = 0;
    child->fd_table    = nullptr;

    uint64_t child_stack_phys = cinux::mm::g_pmm.alloc_pages(TaskBuilder::STACK_PAGES);
    if (child_stack_phys == 0) {
        cinux::lib::kprintf("[PROC] fork: child stack allocation failed\n");
        delete child;
        pid_alloc.free(child_pid);
        return -1;
    }

    uint64_t child_guard_virt = alloc_stack_vaddr(TaskBuilder::STACK_PAGES + 1);
    uint64_t child_stack_virt = child_guard_virt + cinux::arch::PAGE_SIZE;
    uint64_t stack_size       = TaskBuilder::STACK_PAGES * cinux::arch::PAGE_SIZE;

    for (uint64_t i = 0; i < TaskBuilder::STACK_PAGES; i++) {
        uint64_t phys = child_stack_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = child_stack_virt + i * cinux::arch::PAGE_SIZE;
        if (!cinux::mm::g_vmm.map(virt, phys, 0x03)) {
            cinux::lib::kprintf("[PROC] fork: child stack map failed at page %u\n",
                                static_cast<unsigned>(i));
            delete child;
            pid_alloc.free(child_pid);
            return -1;
        }
    }

    *reinterpret_cast<uint64_t*>(child_stack_virt) = TaskBuilder::STACK_MAGIC;

    uint64_t current_rsp;
    uint64_t current_rbp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(current_rsp));
    __asm__ volatile("movq %%rbp, %0" : "=r"(current_rbp));

    uint64_t full_stack_used   = parent->kernel_stack_top - current_rsp;
    uint64_t child_stack_start = child_stack_virt + stack_size - full_stack_used;
    std::memcpy(reinterpret_cast<void*>(child_stack_start), reinterpret_cast<void*>(current_rsp),
                full_stack_used);

    child->kernel_stack            = child_stack_virt;
    child->kernel_stack_top        = child_stack_virt + stack_size;
    child->kernel_stack_guard_page = child_guard_virt;

    child->ctx.rsp = (current_rbp + 8) - current_rsp + child_stack_start;
    child->ctx.rbp = *reinterpret_cast<uint64_t*>(current_rbp);
    child->ctx.rip = reinterpret_cast<uint64_t>(fork_child_trampoline);

    // CoW page table handling
    if (parent->addr_space != nullptr) {
        child->addr_space = new cinux::mm::AddressSpace();
        if (child->addr_space == nullptr) {
            cinux::lib::kprintf("[PROC] fork: child address space allocation failed\n");
            delete child;
            pid_alloc.free(child_pid);
            return -1;
        }

        uint64_t parent_pml4 = parent->addr_space->pml4_phys();
        uint64_t child_pml4  = child->addr_space->pml4_phys();

        auto* parent_pml4_table = phys_to_virt(parent_pml4);
        auto* child_pml4_table  = phys_to_virt(child_pml4);

        for (uint32_t i = 0; i < 256; i++) {
            if (!parent_pml4_table[i].is_present())
                continue;

            if (!(parent_pml4_table[i].raw & FLAG_USER))
                continue;

            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            if (new_page == 0) {
                cinux::lib::kprintf("[PROC] fork: page table allocation failed at PML4[%u]\n", i);
                delete child->addr_space;
                delete child;
                pid_alloc.free(child_pid);
                return -1;
            }

            auto* new_table = phys_to_virt(new_page);
            for (uint32_t j = 0; j < PT_ENTRIES; j++) {
                new_table[j].raw = 0;
            }

            child_pml4_table[i].raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;
            copy_page_table_level(parent_pml4_table[i].phys_addr(), new_page, 3);
        }
    }

    child->wait_next = parent->children;
    parent->children = child;

    // Copy FDTable from parent
    {
        auto* src         = parent->fd_table ? parent->fd_table : &cinux::fs::g_global_fd_table();
        bool  has_entries = false;
        for (uint32_t i = 0; i < cinux::fs::FD_TABLE_SIZE; i++) {
            if (src->get(static_cast<int>(i)) != nullptr) {
                has_entries = true;
                break;
            }
        }
        if (has_entries) {
            child->fd_table = new cinux::fs::FDTable();
            for (uint32_t i = 0; i < cinux::fs::FD_TABLE_SIZE; i++) {
                auto* f = src->get(static_cast<int>(i));
                if (f != nullptr) {
                    auto* copy = new cinux::fs::File(f->inode, f->offset, f->flags);
                    child->fd_table->set(static_cast<int>(i), copy);
                }
            }
        }
    }

    child->ctx.gs_base  = 0;
    child->ctx.kgs_base = g_per_cpu.gs_page_vaddr;

    Scheduler::add_task(child);

    cinux::lib::kprintf("[PROC] fork: created child pid=%d tid=%u parent_pid=%d\n", child->pid,
                        child->tid, parent->pid);

    return child_pid;
}

}  // namespace cinux::proc
