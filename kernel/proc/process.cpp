/**
 * @file kernel/proc/process.cpp
 * @brief Task construction via TaskBuilder
 */

#include "kernel/proc/process.hpp"

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"
#include "kernel/proc/scheduler.hpp"

namespace cinux::proc {

// ============================================================
// Internal state
// ============================================================

namespace {

uint64_t next_tid = 1;

uint64_t next_stack_vaddr = 0xFFFF800000100000ULL;

uint64_t alloc_stack_vaddr(uint64_t pages) {
    uint64_t vaddr = next_stack_vaddr;
    next_stack_vaddr += pages * cinux::arch::PAGE_SIZE;
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
    for (uint8_t* p = reinterpret_cast<uint8_t*>(task);
         p < reinterpret_cast<uint8_t*>(task + 1); p++) {
        *p = 0;
    }

    // Step 3: Allocate contiguous physical pages for the kernel stack
    uint64_t stack_phys = cinux::mm::g_pmm.alloc_pages(STACK_PAGES);
    if (stack_phys == 0) {
        cinux::lib::kprintf("[PROC] TaskBuilder::build: stack allocation failed\n");
        delete task;
        return nullptr;
    }

    // Step 4: Map the stack into the kernel virtual address space
    uint64_t stack_virt = alloc_stack_vaddr(STACK_PAGES);
    uint64_t stack_size = STACK_PAGES * cinux::arch::PAGE_SIZE;

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
    task->ctx.rip = reinterpret_cast<uint64_t>(entry_);
    task->ctx.r15 = 0;
    task->ctx.r14 = 0;
    task->ctx.r13 = 0;
    task->ctx.r12 = 0;
    task->ctx.rbp = 0;
    task->ctx.rbx = 0;

    // Step 6.5: Initialise FPU state
    __asm__ volatile("fninit");
    __asm__ volatile("fxsave %0" : : "m"(task->fpu_state));

    // Step 7: Fill in the remaining task fields
    task->state            = TaskState::Ready;
    task->tid              = next_tid++;
    task->priority         = priority_;
    task->kernel_stack     = stack_virt;
    task->kernel_stack_top = stack_virt + stack_size;
    task->addr_space       = addr_space_;
    task->sched_class      = sched_class_;
    task->name             = name_;

    // Step 7.5: Initialise cwd to "/"
    task->cwd[0] = '/';
    task->cwd[1] = '\0';

    cinux::lib::kprintf("[PROC] Created task tid=%u name='%s' stack=0x%p\n",
                        task->tid, task->name,
                        reinterpret_cast<void*>(task->kernel_stack_top));

    return task;
}

}  // namespace cinux::proc
