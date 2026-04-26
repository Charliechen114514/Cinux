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

#include <stddef.h>
#include <stdint.h>

#include "kernel/mm/address_space.hpp"
#include "kernel/proc/elf_types.hpp"
#include "kernel/proc/pid.hpp"

namespace cinux::fs {
class FDTable;
}  // namespace cinux::fs

namespace cinux::proc {

class SchedulingClass;

// ============================================================
// Task lifecycle states
// ============================================================

enum class TaskState : uint8_t {
    Running,
    Ready,
    Blocked,
    Zombie,
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
    uint64_t gs_base;
    uint64_t kgs_base;
};

static_assert(offsetof(CpuContext, r15) == 0, "r15 at offset 0");
static_assert(offsetof(CpuContext, r14) == 8, "r14 at offset 8");
static_assert(offsetof(CpuContext, r13) == 16, "r13 at offset 16");
static_assert(offsetof(CpuContext, r12) == 24, "r12 at offset 24");
static_assert(offsetof(CpuContext, rbp) == 32, "rbp at offset 32");
static_assert(offsetof(CpuContext, rbx) == 40, "rbx at offset 40");
static_assert(offsetof(CpuContext, rsp) == 48, "rsp at offset 48");
static_assert(offsetof(CpuContext, rip) == 56, "rip at offset 56");
static_assert(offsetof(CpuContext, gs_base) == 64, "gs_base at offset 64");
static_assert(offsetof(CpuContext, kgs_base) == 72, "kgs_base at offset 72");
static_assert(sizeof(CpuContext) == 80, "CpuContext must be 80 bytes");

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

    /**
     * Virtual address of the guard page below the kernel stack.
     * This page is intentionally left unmapped; a fault here
     * indicates a kernel stack overflow.
     * Zero means no guard page (e.g. the boot task).
     */
    uint64_t kernel_stack_guard_page;

    /** Per-process page tables (nullptr for kernel-only threads). */
    cinux::mm::AddressSpace* addr_space;

    /** Per-process file descriptor table (nullptr = use global). */
    cinux::fs::FDTable* fd_table;

    /** Human-readable task name (static storage, not owned). */
    const char* name;

    /** Process ID (assigned by PidAllocator; 0 = uninitialised). */
    int pid;

    /** Parent process ID (0 for the kernel init task). */
    int ppid;

    /** Exit status code (valid when state == Zombie). */
    int exit_status;

    /** Singly-linked list of child tasks (head pointer). */
    Task* children;

    /** Pointer to the parent task (nullptr for the kernel init task). */
    Task* parent;

    /** Scheduling class this task belongs to. */
    SchedulingClass* sched_class;

    /** Intrusive link for wait-queue linked lists (Mutex / Semaphore). */
    Task* wait_next;

    /** FPU/SSE state (512 bytes, 16-byte aligned for fxsave/fxrstor). */
    alignas(16) uint8_t fpu_state[512];

    /** Per-process current working directory (absolute path, NUL-terminated). */
    char cwd[256];
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
    void (*entry_)()                      = nullptr;
    const char*              name_        = "unnamed";
    uint64_t                 priority_    = 0;
    cinux::mm::AddressSpace* addr_space_  = nullptr;
    SchedulingClass*         sched_class_ = nullptr;
};

// ============================================================
// Fork
// ============================================================

/**
 * @brief Fork the current task (Copy-On-Write semantics)
 *
 * Creates a near-identical copy of the calling task:
 *   - Allocates a new PID from the global PidAllocator
 *   - Copies the TCB (pid, ppid, state, etc.)
 *   - If the parent has an AddressSpace, creates a CoW copy of
 *     the user-space page tables (all writable PTEs are marked
 *     read-only with FLAG_COW set)
 *   - Links the child into the parent's children list
 *   - Adds the child task to the scheduler's run queue
 *
 * Return value semantics (set in the child's TCB via ctx.rax):
 *   - Parent: returns child PID (> 0)
 *   - Child:  returns 0
 *
 * @param pid_alloc  Reference to the global PID allocator
 * @return Child PID on success (parent perspective), or -1 on failure
 */
int fork(PidAllocator& pid_alloc);

// ============================================================
// CoW page fault handling
// ============================================================

/**
 * @brief Attempt to resolve a page fault as a Copy-On-Write trigger
 *
 * Called from the page fault handler when a write to a read-only page
 * is detected.  If the PTE has the FLAG_COW bit set, this function:
 *   1. Allocates a new physical page
 *   2. Copies the contents from the shared page to the new page
 *   3. Updates the PTE to point to the new page with write permission
 *   4. Clears FLAG_COW from the PTE
 *
 * @param fault_vaddr  Virtual address that triggered the fault
 * @return true if the fault was handled as CoW, false otherwise
 */
bool handle_cow_fault(uint64_t fault_vaddr);

// ============================================================
// Execve
// ============================================================

namespace errno_values {
constexpr int EPERM   = 1;   ///< Operation not permitted
constexpr int ENOENT  = 2;   ///< No such file or directory
constexpr int ESRCH   = 3;   ///< No such process
constexpr int EIO     = 5;   ///< I/O error
constexpr int ENOEXEC = 8;   ///< Exec format error
constexpr int ENOMEM  = 12;  ///< Out of memory
constexpr int EACCES  = 13;  ///< Permission denied
constexpr int EFAULT  = 14;  ///< Bad address
constexpr int ECHILD  = 10;  ///< No child processes
constexpr int EISDIR  = 21;  ///< Is a directory
constexpr int EINVAL  = 22;  ///< Invalid argument
}  // namespace errno_values

/**
 * @brief Result codes from execve() loading
 *
 * Values follow Linux errno conventions so that sys_execve can
 * return the negated value directly (e.g. -ENOENT, -ENOEXEC).
 */
enum class ExecveResult : int {
    Ok             = 0,    ///< Successfully loaded the new executable
    BadPath        = -22,  ///< Path is null or empty (EINVAL)
    FileNotFound   = -2,   ///< VFS could not resolve the path (ENOENT)
    FileNotRegular = -21,  ///< Path resolves to a non-regular file (EISDIR)
    ReadFailed     = -5,   ///< Failed to read the ELF data from the inode (EIO)
    BadElfMagic    = -8,   ///< ELF magic number mismatch (ENOEXEC)
    BadElfClass    = -8,   ///< Not a 64-bit ELF (ENOEXEC)
    BadElfEndian   = -8,   ///< Not little-endian (ENOEXEC)
    BadElfMachine  = -8,   ///< Not x86-64 (ENOEXEC)
    BadElfType     = -8,   ///< Not an executable (ENOEXEC)
    BadElfHeaders  = -8,   ///< Program header offset/size invalid (ENOEXEC)
    NoLoadSegments = -8,   ///< No PT_LOAD segments found (ENOEXEC)
    MapFailed      = -12,  ///< Address space map() failed for a segment (ENOMEM)
    NoAddressSpace = -12,  ///< Task has no address space (ENOMEM)
    NoCurrentTask  = -3,   ///< No current task in the scheduler (ESRCH)
};

/**
 * @brief Replace the current process image with a new ELF executable
 *
 * Reads the ELF binary from the VFS, validates the header, unmaps
 * existing user-space pages, loads PT_LOAD segments into the task's
 * address space, and sets the entry point.  The old process image is
 * destroyed but the PID, parent, and scheduler linkage are preserved.
 *
 * After a successful execve(), the caller is responsible for jumping
 * to the new entry point (typically via jump_to_usermode).
 *
 * @param path  Null-terminated path to the ELF executable
 * @param argv  Array of argument strings (may be nullptr)
 * @param envp  Array of environment strings (may be nullptr)
 * @return ExecveResult::Ok on success, or an error code
 */
ExecveResult execve(const char* path, const char* const argv[], const char* const envp[]);

// ============================================================
// Waitpid
// ============================================================

/**
 * @brief Result codes from waitpid()
 *
 * Values follow Linux errno conventions so that sys_waitpid can
 * return the negated value directly.
 */
enum class WaitpidResult : int {
    Ok         = 0,    ///< Successfully reaped a child
    NoChildren = -10,  ///< Caller has no children (ECHILD)
    NotFound   = -3,   ///< Specified PID is not a child (ESRCH)
    InvalidPid = -22,  ///< PID argument is invalid (EINVAL)
    NotExited  = -1,   ///< Child exists but has not exited yet
};

/**
 * @brief Wait for a child process to change state
 *
 * If pid > 0, waits for the specific child with that PID.
 * If pid == -1, waits for any child.
 *
 * When the target child is in Zombie state, this function:
 *   1. Collects the child's exit_status into *status
 *   2. Unlinks the child from the parent's children list
 *   3. Frees the child's PID via the PidAllocator
 *   4. Marks the child TCB as Dead
 *
 * If the child has not yet exited (state != Zombie), returns
 * WaitpidResult::NotExited.
 *
 * @param pid        PID of the child to wait for, or -1 for any child
 * @param status     Pointer to store the child's exit status (may be nullptr)
 * @param pid_alloc  Reference to the global PID allocator
 * @return WaitpidResult::Ok on success, or an error code
 */
WaitpidResult waitpid(int pid, int* status, PidAllocator& pid_alloc);

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

/**
 * @brief Fork child trampoline (assembly, sets rax=0 and returns)
 *
 * Called by the scheduler when the child task is first switched in.
 * Sets rax to 0 so that the child sees fork() return 0, then
 * returns to the fork() call site on the child's stack.
 */
extern "C" void fork_child_trampoline();

}  // namespace cinux::proc
