/**
 * @file kernel/proc/process.hpp
 * @brief Process control structures for kernel threading
 *
 * Defines the fundamental data types for task management:
 *   - TaskState: lifecycle states of a task
 *   - CpuContext: callee-saved register snapshot for context switching
 *   - Task: the full task control block (TCB)
 *   - TaskBuilder: fluent builder for constructing Task objects
 *
 * TaskBuilder provides a step-by-step configuration interface:
 *   TaskBuilder()
 *       .set_entry(thread_func)
 *       .set_name("thread_a")
 *       .build();
 *
 * The build() call allocates a TCB from the heap and a kernel stack
 * from the PMM, initialises the CpuContext, and writes a stack
 * overflow detection magic.
 *
 * Depends on: PMM (for stack allocation), Heap (for TCB allocation).
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "kernel/mm/address_space.hpp"

namespace cinux::proc {

class SchedulingClass;

// ============================================================
// Task lifecycle states
// ============================================================

enum class TaskState : uint8_t {
    Running,
    Ready,
    Blocked,
    Dead
};

// ============================================================
// CPU context for context switching
// ============================================================

/**
 * @brief Callee-saved register snapshot for cooperative context switch
 *
 * Only the callee-saved registers (r15-r12, rbp, rbx) plus rsp and
 * rip need to be saved/restored because the switch happens at known
 * call boundaries where caller-saved registers are already clobbered.
 *
 * Layout must match the offsets used in context_switch.S exactly.
 */
struct alignas(16) CpuContext {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rip;
};

static_assert(offsetof(CpuContext, r15) == 0,  "r15 at offset 0");
static_assert(offsetof(CpuContext, r14) == 8,  "r14 at offset 8");
static_assert(offsetof(CpuContext, r13) == 16, "r13 at offset 16");
static_assert(offsetof(CpuContext, r12) == 24, "r12 at offset 24");
static_assert(offsetof(CpuContext, rbp) == 32, "rbp at offset 32");
static_assert(offsetof(CpuContext, rbx) == 40, "rbx at offset 40");
static_assert(offsetof(CpuContext, rsp) == 48, "rsp at offset 48");
static_assert(offsetof(CpuContext, rip) == 56, "rip at offset 56");
static_assert(sizeof(CpuContext) == 64, "CpuContext must be 64 bytes");

// ============================================================
// Task Control Block
// ============================================================

/**
 * @brief Task Control Block (TCB) representing a kernel thread
 *
 * Contains everything the scheduler needs: saved CPU context,
 * lifecycle state, identity, scheduling priority, stack pointers,
 * and an optional address space for future user-mode tasks.
 */
struct Task {
    /** Saved callee-saved registers for context switching. */
    CpuContext ctx;

    /** Current lifecycle state. */
    TaskState state;

    /** Unique task identifier (monotonically increasing). */
    uint64_t tid;

    /** Scheduling priority (lower = higher priority, for future use). */
    uint64_t priority;

    /** Base of the kernel stack allocation (for freeing). */
    uint64_t kernel_stack;

    /** Top of the kernel stack (initial rsp). */
    uint64_t kernel_stack_top;

    /** Per-process page tables (nullptr for kernel-only threads). */
    cinux::mm::AddressSpace* addr_space;

    /** Human-readable task name (static storage, not owned). */
    const char* name;

    /** Scheduling class this task belongs to. */
    SchedulingClass* sched_class;

    /** Intrusive link for wait-queue linked lists (Mutex / Semaphore). */
    Task* wait_next;
};

// ============================================================
// TaskBuilder -- fluent builder for Task construction
// ============================================================

/**
 * @brief Fluent builder for constructing kernel Task objects
 *
 * Accumulates configuration via setter methods, then performs
 * allocation and initialisation in build().  Example usage:
 *
 *   auto* task = TaskBuilder()
 *       .set_entry(my_thread)
 *       .set_name("worker")
 *       .set_priority(1)
 *       .build();
 *
 * At minimum, set_entry() must be called before build().
 */
class TaskBuilder {
public:
    TaskBuilder() = default;

    /** Set the thread entry point.  Required before build(). */
    TaskBuilder& set_entry(void (*entry)());

    /** Set the human-readable task name.  Defaults to "unnamed". */
    TaskBuilder& set_name(const char* name);

    /** Set the scheduling priority.  Defaults to 0. */
    TaskBuilder& set_priority(uint64_t priority);

    /** Set the address space.  Defaults to nullptr (kernel-only). */
    TaskBuilder& set_addr_space(cinux::mm::AddressSpace* space);

    /** Set the scheduling class.  Defaults to nullptr. */
    TaskBuilder& set_sched_class(SchedulingClass* sched_class);

    /**
     * @brief Allocate and initialise the Task
     *
     * Allocates a Task struct from the kernel heap and a kernel
     * stack from the PMM.  Initialises CpuContext so that the
     * first context_switch jumps to the entry point.  Writes a
     * magic value at the stack bottom for overflow detection.
     *
     * @return Pointer to the fully initialised Task, or nullptr on failure
     */
    Task* build();

    /** Magic value written at the bottom of every kernel stack. */
    static constexpr uint64_t STACK_MAGIC = 0xDEADC0DE;

    /** Number of 4 KB pages per kernel stack (16 KB total). */
    static constexpr uint64_t STACK_PAGES = 4;

private:
    void (*entry_)() = nullptr;
    const char* name_ = "unnamed";
    uint64_t priority_ = 0;
    cinux::mm::AddressSpace* addr_space_ = nullptr;
    SchedulingClass* sched_class_ = nullptr;
};

// ============================================================
// Assembly entry point (C linkage)
// ============================================================

/**
 * @brief Low-level context switch primitive
 *
 * Saves callee-saved registers of the current task into `from`,
 * restores callee-saved registers from `to`, and jumps to the
 * saved rip of `to`.
 *
 * @param from  Pointer to the outgoing task's CpuContext
 * @param to    Pointer to the incoming task's CpuContext
 */
extern "C" void context_switch(CpuContext* from, CpuContext* to);

}  // namespace cinux::proc
