/**
 * @file kernel/proc/process.cpp
 * @brief Shared process-internal state, CoW fault handler, and waitpid
 *
 * Houses the TID counter and stack-virtual-address allocator used by
 * TaskBuilder::build() and fork(), the Copy-On-Write page fault
 * resolver, and the waitpid() system call implementation.
 */

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/process_internal.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

// ============================================================
// Shared internal state (used by task_builder.cpp and fork.cpp)
// ============================================================

cinux::lib::Atomic<uint64_t> next_tid{1};

cinux::lib::Atomic<uint64_t> next_stack_vaddr{cinux::arch::KMEM_STACK_BASE};

uint64_t alloc_stack_vaddr(uint64_t pages) {
    uint64_t vaddr = next_stack_vaddr.fetch_add(pages * cinux::arch::PAGE_SIZE,
                                                cinux::lib::MemoryOrder::Relaxed);
    return vaddr;
}

// ============================================================
// CoW page fault handler
// ============================================================

namespace {

constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

using namespace cinux::arch;

PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}

PageEntry* get_pte(uint64_t pml4_phys, uint64_t virt) {
    auto*      pml4  = phys_to_virt(pml4_phys);
    PageEntry& pml4e = pml4[PML4_INDEX(virt)];
    if (!pml4e.is_present())
        return nullptr;

    auto*      pdpt  = phys_to_virt(pml4e.phys_addr());
    PageEntry& pdpte = pdpt[PDPT_INDEX(virt)];
    if (!pdpte.is_present())
        return nullptr;

    auto*      pd  = phys_to_virt(pdpte.phys_addr());
    PageEntry& pde = pd[PD_INDEX(virt)];
    if (!pde.is_present())
        return nullptr;

    auto* pt = phys_to_virt(pde.phys_addr());
    return &pt[PT_INDEX(virt)];
}

}  // anonymous namespace

bool handle_cow_fault(uint64_t fault_vaddr) {
    auto* task = Scheduler::current();
    if (task == nullptr || task->addr_space == nullptr) {
        return false;
    }

    uint64_t   pml4_phys = task->addr_space->pml4_phys();
    PageEntry* pte       = get_pte(pml4_phys, fault_vaddr);
    if (pte == nullptr) {
        return false;
    }

    if (!pte->is_present())
        return false;
    if (pte->raw & FLAG_WRITABLE)
        return false;
    if (!(pte->raw & FLAG_COW))
        return false;

    uint64_t old_phys = pte->phys_addr();
    uint64_t new_phys = cinux::mm::g_pmm.alloc_page();
    if (new_phys == 0) {
        cinux::lib::kprintf("[COW] page allocation failed for vaddr=%p\n",
                            reinterpret_cast<void*>(fault_vaddr));
        return false;
    }

    auto* src = reinterpret_cast<uint8_t*>(old_phys + KERNEL_VMA);
    auto* dst = reinterpret_cast<uint8_t*>(new_phys + KERNEL_VMA);
    for (uint64_t i = 0; i < cinux::arch::PAGE_SIZE; i++) {
        dst[i] = src[i];
    }

    pte->set_phys_addr(new_phys);
    pte->raw |= FLAG_WRITABLE;
    pte->raw &= ~FLAG_COW;

    cinux::arch::flush_tlb(fault_vaddr & ~(cinux::arch::PAGE_SIZE - 1));

    cinux::lib::kprintf("[COW] resolved fault at vaddr=%p old_phys=%p new_phys=%p\n",
                        reinterpret_cast<void*>(fault_vaddr), reinterpret_cast<void*>(old_phys),
                        reinterpret_cast<void*>(new_phys));

    return true;
}

// ============================================================
// waitpid implementation
// ============================================================

WaitpidResult waitpid(int pid, int* status, PidAllocator& pid_alloc) {
    auto* parent = Scheduler::current();
    if (parent == nullptr) {
        cinux::lib::kprintf("[WAITPID] no current task\n");
        return WaitpidResult::NoChildren;
    }

    if (pid != -1 && pid <= 0) {
        cinux::lib::kprintf("[WAITPID] invalid pid=%d\n", pid);
        return WaitpidResult::InvalidPid;
    }

    if (parent->children == nullptr) {
        cinux::lib::kprintf("[WAITPID] pid=%d has no children\n", parent->pid);
        return WaitpidResult::NoChildren;
    }

    Task* target = nullptr;
    Task* prev   = nullptr;

    if (pid == -1) {
        Task* cur      = parent->children;
        Task* cur_prev = nullptr;

        while (cur != nullptr) {
            if (cur->state == TaskState::Zombie) {
                target = cur;
                prev   = cur_prev;
                break;
            }
            cur_prev = cur;
            cur      = cur->wait_next;
        }

        if (target == nullptr) {
            cinux::lib::kprintf("[WAITPID] pid=%d children exist but none exited\n", parent->pid);
            return WaitpidResult::NotExited;
        }
    } else {
        Task* cur      = parent->children;
        Task* cur_prev = nullptr;

        while (cur != nullptr) {
            if (cur->pid == pid) {
                target = cur;
                prev   = cur_prev;
                break;
            }
            cur_prev = cur;
            cur      = cur->wait_next;
        }

        if (target == nullptr) {
            cinux::lib::kprintf("[WAITPID] pid=%d is not a child of pid=%d\n", pid, parent->pid);
            return WaitpidResult::NotFound;
        }

        if (target->state != TaskState::Zombie) {
            cinux::lib::kprintf("[WAITPID] child pid=%d has not exited yet\n", pid);
            return WaitpidResult::NotExited;
        }
    }

    if (status != nullptr) {
        *status = target->exit_status;
    }

    if (prev != nullptr) {
        prev->wait_next = target->wait_next;
    } else {
        parent->children = target->wait_next;
    }

    pid_alloc.free(target->pid);

    target->state  = TaskState::Dead;
    target->parent = nullptr;

    cinux::lib::kprintf("[WAITPID] reaped child pid=%d exit_status=%d by parent pid=%d\n",
                        target->pid, target->exit_status, parent->pid);

    return WaitpidResult::Ok;
}

}  // namespace cinux::proc
