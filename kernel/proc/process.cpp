/**
 * @file kernel/proc/process.cpp
 * @brief Task construction via TaskBuilder
 */

#include "kernel/proc/process.hpp"

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
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/elf_types.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/scheduler.hpp"
#include "proc/per_cpu.hpp"

namespace cinux::proc {

// ============================================================
// Internal state
// ============================================================

namespace {

cinux::lib::Atomic<uint64_t> next_tid{1};

cinux::lib::Atomic<uint64_t> next_stack_vaddr{cinux::arch::KMEM_STACK_BASE};

uint64_t alloc_stack_vaddr(uint64_t pages) {
    uint64_t vaddr = next_stack_vaddr.fetch_add(pages * cinux::arch::PAGE_SIZE,
                                                cinux::lib::MemoryOrder::Relaxed);
    return vaddr;
}

}  // anonymous namespace

// ============================================================
// TaskBuilder setter implementations
// ============================================================

TaskBuilder& TaskBuilder::set_entry(void (*entry)()) {
    entry_ = entry;
    return *this;
}

TaskBuilder& TaskBuilder::set_name(const char* name) {
    name_ = name;
    return *this;
}

TaskBuilder& TaskBuilder::set_priority(uint64_t priority) {
    priority_ = priority;
    return *this;
}

TaskBuilder& TaskBuilder::set_addr_space(cinux::mm::AddressSpace* space) {
    addr_space_ = space;
    return *this;
}

TaskBuilder& TaskBuilder::set_sched_class(SchedulingClass* sched_class) {
    sched_class_ = sched_class;
    return *this;
}

// ============================================================
// TaskBuilder::build
// ============================================================

Task* TaskBuilder::build() {
    if (entry_ == nullptr) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: entry point is null\n");
        return nullptr;
    }

    // Step 1: Allocate the Task struct from the kernel heap
    auto* task = new (std::align_val_t{alignof(Task)}) Task;
    if (task == nullptr) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: TCB allocation failed\n");
        return nullptr;
    }

    // Step 2: Zero-initialise the task
    for (uint8_t* p = reinterpret_cast<uint8_t*>(task); p < reinterpret_cast<uint8_t*>(task + 1);
         p++) {
        *p = 0;
    }

    // Step 3: Allocate contiguous physical pages for the kernel stack
    // We reserve STACK_PAGES + 1 virtual pages: the lowest page is a guard
    // page (intentionally left unmapped) and the upper STACK_PAGES pages form
    // the actual kernel stack.  This way a stack overflow hits the guard page
    // and triggers a page fault instead of silently corrupting adjacent memory.
    uint64_t stack_phys = cinux::mm::g_pmm.alloc_pages(STACK_PAGES);
    if (stack_phys == 0) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: stack allocation failed\n");
        delete task;
        return nullptr;
    }

    // Step 4: Map the stack into the kernel virtual address space
    // Reserve 1 extra virtual page for the guard page (left unmapped).
    uint64_t guard_virt = alloc_stack_vaddr(STACK_PAGES + 1);
    uint64_t stack_virt = guard_virt + cinux::arch::PAGE_SIZE;
    uint64_t stack_size = STACK_PAGES * cinux::arch::PAGE_SIZE;

    // DO NOT map guard_virt -- it stays unmapped on purpose.
    // Map only the STACK_PAGES above the guard page.
    for (uint64_t i = 0; i < STACK_PAGES; i++) {
        uint64_t phys = stack_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = stack_virt + i * cinux::arch::PAGE_SIZE;
        if (!cinux::mm::g_vmm.map(virt, phys, 0x03)) {
            cinux::lib::kprintf("[PROC] TaskBuilder::build: stack map failed at page %u\n", i);
            delete task;
            return nullptr;
        }
    }

    // Step 5: Write stack overflow magic at the very bottom
    *reinterpret_cast<uint64_t*>(stack_virt) = STACK_MAGIC;

    // Step 6: Initialise the CPU context
    // Push exit_current as the return address so that when the thread
    // function returns, it lands in exit_current() for clean teardown.
    task->ctx.rsp = stack_virt + stack_size - 8;
    *reinterpret_cast<uint64_t*>(task->ctx.rsp) =
        reinterpret_cast<uint64_t>(&cinux::proc::Scheduler::exit_current);
    task->ctx.rip      = reinterpret_cast<uint64_t>(entry_);
    task->ctx.r15      = 0;
    task->ctx.r14      = 0;
    task->ctx.r13      = 0;
    task->ctx.r12      = 0;
    task->ctx.rbp      = 0;
    task->ctx.rbx      = 0;
    task->ctx.gs_base  = 0;
    task->ctx.kgs_base = g_per_cpu.gs_page_vaddr;

    // Step 6.5: Initialise FPU state
    __asm__ volatile("fninit");
    __asm__ volatile("fxsave %0" : : "m"(task->fpu_state));

    // Step 7: Fill in the remaining task fields
    task->state                   = TaskState::Ready;
    task->tid                     = next_tid.fetch_add(1, cinux::lib::MemoryOrder::Relaxed);
    task->priority                = priority_;
    task->kernel_stack            = stack_virt;
    task->kernel_stack_top        = stack_virt + stack_size;
    task->kernel_stack_guard_page = guard_virt;
    task->addr_space              = addr_space_;
    task->sched_class             = sched_class_;
    task->name                    = name_;

    // Step 7.5: Initialise cwd to "/"
    task->cwd[0] = '/';
    task->cwd[1] = '\0';

    cinux::lib::kprintf("[PROC] Created task tid=%u name='%s' stack=0x%p\n", task->tid, task->name,
                        reinterpret_cast<void*>(task->kernel_stack_top));

    return task;
}

// ============================================================
// Internal helpers for fork and CoW
// ============================================================

namespace {

constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

using namespace cinux::arch;

/** Convert a physical address to a virtual address via the higher-half mapping. */
PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}

/**
 * @brief Recursively copy a page table level for CoW fork
 *
 * At the PT (leaf) level, shares physical pages and marks writable
 * entries as read-only with FLAG_COW.  At intermediate levels, allocates
 * new page table pages and recurses.
 *
 * @param src_phys  Physical address of the source page table
 * @param dst_phys  Physical address of the destination page table
 * @param level     Current level: 3=PDPT, 2=PD, 1=PT
 */
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    auto* src_table = phys_to_virt(src_phys);
    auto* dst_table = phys_to_virt(dst_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!src_table[i].is_present())
            continue;

        // Skip kernel mappings (identity-mapped RAM, MMIO).  These lack
        // FLAG_USER; the child accesses kernel resources through the
        // shared PML4[256-511] higher-half mapping instead.
        if (!(src_table[i].raw & FLAG_USER))
            continue;

        // Huge page with FLAG_USER: share directly, never CoW.
        // Currently unused but ready for future user-space huge pages.
        if (src_table[i].huge) {
            dst_table[i].raw = src_table[i].raw;
            continue;
        }

        if (level > 1) {
            // Intermediate level: allocate a new page table and recurse
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
            // PT level (leaf): share the physical page, mark CoW if writable
            uint64_t entry_flags = src_table[i].raw & FLAG_MASK;

            // Share the same physical page (increment reference conceptually)
            dst_table[i].raw = src_table[i].raw;

            // If the page is writable, make it read-only and set CoW flag
            if (entry_flags & FLAG_WRITABLE) {
                dst_table[i].raw &= ~FLAG_WRITABLE;
                dst_table[i].raw |= FLAG_COW;

                // Also mark the parent's PTE as read-only + CoW
                src_table[i].raw &= ~FLAG_WRITABLE;
                src_table[i].raw |= FLAG_COW;
            }
        }
    }
}

/**
 * @brief Walk page table levels to reach the PT entry for a virtual address
 *
 * @param pml4_phys  Physical address of the PML4 root
 * @param virt       Virtual address to look up
 * @return Pointer to the PT entry, or nullptr if the mapping does not exist
 */
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

// ============================================================
// fork implementation
// ============================================================

// Frame pointer is required: fork reads RBP to locate the return
// address on the parent stack and set up the child's ctx.rsp.
// Without it, -fomit-frame-pointer (Release mode) repurposes RBP
// and the ctx.rsp calculation produces garbage, crashing the child.
__attribute__((optimize("no-omit-frame-pointer"), noinline)) int fork(PidAllocator& pid_alloc) {
    auto* parent = Scheduler::current();
    if (parent == nullptr) {
        cinux::lib::kprintf("[PROC] fork: no current task\n");
        return -1;
    }

    // Step 1: Allocate a new PID for the child
    int child_pid = pid_alloc.alloc();
    if (child_pid == PidAllocator::PID_NONE) {
        cinux::lib::kprintf("[PROC] fork: PID allocator exhausted\n");
        return -1;
    }

    // Step 2: Allocate the child TCB from the kernel heap
    auto* child = new (std::align_val_t{alignof(Task)}) Task;
    if (child == nullptr) {
        cinux::lib::kprintf("[PROC] fork: TCB allocation failed\n");
        pid_alloc.free(child_pid);
        return -1;
    }

    // Step 3: Copy the parent TCB (memcpy for the full struct)
    std::memcpy(child, parent, sizeof(Task));

    // Step 4: Fix up child-specific fields
    child->tid         = next_tid.fetch_add(1, cinux::lib::MemoryOrder::Relaxed);
    child->pid         = child_pid;
    child->ppid        = parent->pid;
    child->state       = TaskState::Ready;
    child->parent      = parent;
    child->children    = nullptr;
    child->exit_status = 0;
    child->fd_table    = nullptr;

    // Step 5: Allocate a new kernel stack for the child
    // Reserve 1 extra virtual page for the guard page (left unmapped).
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

    // DO NOT map child_guard_virt -- it stays unmapped on purpose.
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

    // Write stack overflow magic at the bottom
    *reinterpret_cast<uint64_t*>(child_stack_virt) = TaskBuilder::STACK_MAGIC;

    // Step 6: Capture current RSP and RBP inside fork()'s frame.
    // RBP points to the saved caller's RBP on the stack (pushed by fork's
    // prologue).  The fork() return address is at [RBP + 8].
    uint64_t current_rsp;
    uint64_t current_rbp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(current_rsp));
    __asm__ volatile("movq %%rbp, %0" : "=r"(current_rbp));

    // Step 7: Copy the FULL parent stack to the child
    uint64_t full_stack_used   = parent->kernel_stack_top - current_rsp;
    uint64_t child_stack_start = child_stack_virt + stack_size - full_stack_used;
    std::memcpy(reinterpret_cast<void*>(child_stack_start), reinterpret_cast<void*>(current_rsp),
                full_stack_used);

    // Step 8: Set up child's stack pointers and context
    child->kernel_stack            = child_stack_virt;
    child->kernel_stack_top        = child_stack_virt + stack_size;
    child->kernel_stack_guard_page = child_guard_virt;

    // Position child->ctx.rsp so that [ctx.rsp] = fork's return address.
    // On the parent stack, the fork return address is at (current_rbp + 8).
    // On the child stack, the corresponding address is offset by the stack
    // relocation delta: (parent_addr - current_rsp + child_stack_start).
    child->ctx.rsp = (current_rbp + 8) - current_rsp + child_stack_start;

    // Set child's RBP to the caller's frame pointer (create_shell_terminal's
    // rbp).  This value was saved on the stack by fork()'s prologue.
    child->ctx.rbp = *reinterpret_cast<uint64_t*>(current_rbp);

    // The trampoline sets rax=0 and returns to [rsp] (fork's return address),
    // so the child takes the child_pid==0 branch.
    child->ctx.rip = reinterpret_cast<uint64_t>(fork_child_trampoline);

    // Step 8: CoW page table handling (if parent has an address space)
    if (parent->addr_space != nullptr) {
        // Create a new AddressSpace for the child
        child->addr_space = new cinux::mm::AddressSpace();
        if (child->addr_space == nullptr) {
            cinux::lib::kprintf("[PROC] fork: child address space allocation failed\n");
            delete child;
            pid_alloc.free(child_pid);
            return -1;
        }

        uint64_t parent_pml4 = parent->addr_space->pml4_phys();
        uint64_t child_pml4  = child->addr_space->pml4_phys();

        // Copy user-space PML4 entries (0..255) from parent to child
        auto* parent_pml4_table = phys_to_virt(parent_pml4);
        auto* child_pml4_table  = phys_to_virt(child_pml4);

        for (uint32_t i = 0; i < 256; i++) {
            if (!parent_pml4_table[i].is_present())
                continue;

            // Skip PML4 entries without FLAG_USER (kernel identity mappings
            // in PML4[0]).  The child inherits these through PML4[256-511].
            if (!(parent_pml4_table[i].raw & FLAG_USER))
                continue;

            // Allocate a new page for this level of the child's page tables
            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            if (new_page == 0) {
                cinux::lib::kprintf("[PROC] fork: page table allocation failed at PML4[%u]\n", i);
                delete child->addr_space;
                delete child;
                pid_alloc.free(child_pid);
                return -1;
            }

            // Zero the new page table page
            auto* new_table = phys_to_virt(new_page);
            for (uint32_t j = 0; j < PT_ENTRIES; j++) {
                new_table[j].raw = 0;
            }

            child_pml4_table[i].raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

            // Recursively copy lower levels and mark writable pages as CoW
            copy_page_table_level(parent_pml4_table[i].phys_addr(), new_page, 3);
        }
    }

    // Step 9: Link child into parent's children list
    child->wait_next = parent->children;
    parent->children = child;

    // Step 9b: Copy FDTable from parent (or global) into a private child FDTable
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

    // Step 10: Initialise child's GS MSR state
    // The child enters kernel mode (via context_switch restoring its ctx)
    // and must have the "unswapped" GS state: gs_base=0, kgs_base=per-CPU
    // data page.  This ensures the first swapgs in syscall_entry works
    // correctly when the child later enters user mode.
    child->ctx.gs_base  = 0;
    child->ctx.kgs_base = g_per_cpu.gs_page_vaddr;

    // Step 11: Add child to the scheduler
    Scheduler::add_task(child);

    cinux::lib::kprintf("[PROC] fork: created child pid=%d tid=%u parent_pid=%d\n", child->pid,
                        child->tid, parent->pid);

    return child_pid;
}

// ============================================================
// CoW page fault handler
// ============================================================

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

    // Check: present, not writable, but has CoW flag
    if (!pte->is_present())
        return false;
    if (pte->raw & FLAG_WRITABLE)
        return false;
    if (!(pte->raw & FLAG_COW))
        return false;

    // Step 1: Allocate a new physical page
    uint64_t old_phys = pte->phys_addr();
    uint64_t new_phys = cinux::mm::g_pmm.alloc_page();
    if (new_phys == 0) {
        cinux::lib::kprintf("[COW] page allocation failed for vaddr=%p\n",
                            reinterpret_cast<void*>(fault_vaddr));
        return false;
    }

    // Step 2: Copy the page contents from old to new
    auto* src = reinterpret_cast<uint8_t*>(old_phys + KERNEL_VMA);
    auto* dst = reinterpret_cast<uint8_t*>(new_phys + KERNEL_VMA);
    for (uint64_t i = 0; i < cinux::arch::PAGE_SIZE; i++) {
        dst[i] = src[i];
    }

    // Step 3: Update the PTE: point to new page, restore write, clear CoW
    pte->set_phys_addr(new_phys);
    pte->raw |= FLAG_WRITABLE;
    pte->raw &= ~FLAG_COW;

    // Step 4: Flush TLB for this page
    cinux::arch::flush_tlb(fault_vaddr & ~(cinux::arch::PAGE_SIZE - 1));

    cinux::lib::kprintf("[COW] resolved fault at vaddr=%p old_phys=%p new_phys=%p\n",
                        reinterpret_cast<void*>(fault_vaddr), reinterpret_cast<void*>(old_phys),
                        reinterpret_cast<void*>(new_phys));

    return true;
}

// ============================================================
// Internal helpers for execve
// ============================================================

namespace {

/**
 * @brief Convert an ElfValidateResult to an ExecveResult
 */
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
 * Walks PML4 entries 0..255 and unmaps every mapped page.
 * Does NOT free the page table pages themselves -- that is
 * handled by AddressSpace::free_subtree on destruction.
 *
 * @param space  The address space to clear
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

                    // Free the data page
                    uint64_t data_phys = pt[l].phys_addr();
                    cinux::mm::g_pmm.free_page(data_phys);
                    pt[l].raw = 0;
                }

                // Free the PT page
                uint64_t pt_phys = pd[k].phys_addr();
                cinux::mm::g_pmm.free_page(pt_phys);
                pd[k].raw = 0;
            }

            // Free the PD page
            uint64_t pd_phys = pdpt[j].phys_addr();
            cinux::mm::g_pmm.free_page(pd_phys);
            pdpt[j].raw = 0;
        }

        // Free the PDPT page
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

    // Suppress unused parameter warnings for argv/envp (future use)
    (void)argv;
    (void)envp;

    // Step 1: Validate arguments
    if (path == nullptr || path[0] == '\0') {
        cinux::lib::kprintf("[EXECVE] invalid path\n");
        return ExecveResult::BadPath;
    }

    // Step 2: Get the current task
    auto* task = Scheduler::current();
    if (task == nullptr) {
        cinux::lib::kprintf("[EXECVE] no current task\n");
        return ExecveResult::NoCurrentTask;
    }

    // Step 3: Ensure the task has an address space
    if (task->addr_space == nullptr) {
        cinux::lib::kprintf("[EXECVE] task has no address space\n");
        return ExecveResult::NoAddressSpace;
    }

    // Step 4: Resolve the path through VFS
    const char* rel_path = nullptr;
    auto*       fs       = cinux::fs::vfs_resolve(path, &rel_path);
    if (fs == nullptr) {
        cinux::lib::kprintf("[EXECVE] path not found: %s\n", path);
        return ExecveResult::FileNotFound;
    }

    // Step 5: Look up the inode
    auto* inode = fs->lookup(rel_path);
    if (inode == nullptr) {
        cinux::lib::kprintf("[EXECVE] inode not found: %s\n", rel_path);
        return ExecveResult::FileNotFound;
    }

    // Step 6: Verify it is a regular file
    if (inode->type != cinux::fs::InodeType::Regular) {
        cinux::lib::kprintf("[EXECVE] not a regular file: %s\n", path);
        return ExecveResult::FileNotRegular;
    }

    // Step 7: Read the ELF header from the inode
    constexpr uint64_t ELF_HEADER_SIZE = sizeof(elf::Elf64_Ehdr);
    if (inode->size < ELF_HEADER_SIZE) {
        cinux::lib::kprintf("[EXECVE] file too small for ELF header: %lu bytes\n",
                            static_cast<unsigned long>(inode->size));
        return ExecveResult::ReadFailed;
    }

    // Allocate a temporary buffer for the ELF header and program headers
    // We need to read the ehdr first to get phnum, but let's read
    // enough to cover header + all program headers in one pass.
    // First read just the header.
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

    // Step 8: Validate the ELF header
    auto vr = elf::validate_elf_header(ehdr, inode->size);
    if (vr != elf::ElfValidateResult::Ok) {
        cinux::lib::kprintf("[EXECVE] ELF validation failed: %d\n", static_cast<int>(vr));
        return elf_error_to_execve(vr);
    }

    // Step 9: Read program headers
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

    // Step 10: Clear existing user-space mappings
    clear_user_mappings(*task->addr_space);

    // Step 11: Load PT_LOAD segments into the address space
    bool has_load_segment = false;

    for (uint16_t i = 0; i < phnum; i++) {
        const auto& phdr = phdrs[i];

        if (phdr.p_type != elf::PT_LOAD) {
            continue;
        }

        has_load_segment = true;

        // Calculate the range of pages this segment covers
        uint64_t seg_start = phdr.p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t seg_end   = (phdr.p_vaddr + phdr.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        // Determine page flags from segment flags
        uint64_t page_flags = FLAG_PRESENT | FLAG_USER;
        if (phdr.p_flags & elf::PF_W) {
            page_flags |= FLAG_WRITABLE;
        }
        if (!(phdr.p_flags & elf::PF_X)) {
            page_flags |= FLAG_NX;
        }

        // Map and fill each page in the segment
        for (uint64_t vaddr = seg_start; vaddr < seg_end; vaddr += PAGE_SIZE) {
            // Allocate a physical page
            uint64_t phys = cinux::mm::g_pmm.alloc_page();
            if (phys == 0) {
                cinux::lib::kprintf("[EXECVE] page alloc failed at vaddr=%p\n",
                                    reinterpret_cast<void*>(vaddr));
                delete[] phdrs;
                return ExecveResult::MapFailed;
            }

            // Zero the page
            auto* dst = reinterpret_cast<uint8_t*>(phys + KERNEL_VMA);
            for (uint64_t b = 0; b < PAGE_SIZE; b++) {
                dst[b] = 0;
            }

            // Compute where segment data lands within this page.
            // If the segment starts mid-page (p_vaddr not page-aligned),
            // the first page has a non-zero in_page_off; all others
            // start at offset 0.  Previous code used page_base_offset as
            // the in-page destination, which overflows past the page on
            // every page after the first.
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

            // Map the page into the address space
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

    // Step 12: Set the task's entry point
    task->ctx.rip = ehdr->e_entry;

    cinux::lib::kprintf("[EXECVE] loaded %s entry=%p pid=%d\n", path,
                        reinterpret_cast<void*>(ehdr->e_entry), task->pid);

    return ExecveResult::Ok;
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

    // Step 1: Validate pid argument
    if (pid != -1 && pid <= 0) {
        cinux::lib::kprintf("[WAITPID] invalid pid=%d\n", pid);
        return WaitpidResult::InvalidPid;
    }

    // Step 2: Check that the caller has children
    if (parent->children == nullptr) {
        cinux::lib::kprintf("[WAITPID] pid=%d has no children\n", parent->pid);
        return WaitpidResult::NoChildren;
    }

    // Step 3: Search for the target child in the children list
    // We track prev to allow unlinking from the singly-linked list.
    Task* target = nullptr;
    Task* prev   = nullptr;

    if (pid == -1) {
        // Wait for any child: scan for the first zombie
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
            // Children exist but none have exited yet
            cinux::lib::kprintf("[WAITPID] pid=%d children exist but none exited\n", parent->pid);
            return WaitpidResult::NotExited;
        }
    } else {
        // Wait for a specific child PID
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

        // Check if the specific child has exited
        if (target->state != TaskState::Zombie) {
            cinux::lib::kprintf("[WAITPID] child pid=%d has not exited yet\n", pid);
            return WaitpidResult::NotExited;
        }
    }

    // Step 4: Collect exit status
    if (status != nullptr) {
        *status = target->exit_status;
    }

    // Step 5: Unlink the child from the parent's children list
    if (prev != nullptr) {
        prev->wait_next = target->wait_next;
    } else {
        // Target is the head of the list
        parent->children = target->wait_next;
    }

    // Step 6: Free the child's PID
    pid_alloc.free(target->pid);

    // Step 7: Mark the child TCB as Dead (zombie reaped)
    target->state  = TaskState::Dead;
    target->parent = nullptr;

    cinux::lib::kprintf("[WAITPID] reaped child pid=%d exit_status=%d by parent pid=%d\n",
                        target->pid, target->exit_status, parent->pid);

    return WaitpidResult::Ok;
}

}  // namespace cinux::proc
