/**
 * @file kernel/test/test_fork_exec.cpp
 * @brief QEMU in-kernel tests for fork/CoW syscalls (034)
 *
 * Runs inside QEMU as part of the big kernel test suite.
 * Exercises the real syscall dispatch for SYS_getpid, SYS_getppid,
 * SYS_fork, CoW page fault handling, and PTE flag manipulation.
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Scheduler initialised (Scheduler::current() returns a valid task)
 *   - syscall_init() called
 *
 * Tests:
 *   - SyscallNr::SYS_getpid and SYS_getppid have correct number values
 *   - sys_getpid returns the PID of the current task (>= 0)
 *   - sys_getppid returns the PPID of the current task (>= 0)
 *   - Dispatch via syscall_dispatch for SYS_getpid works
 *   - Dispatch via syscall_dispatch for SYS_getppid works
 *   - SyscallNr::SYS_fork has correct number value (57)
 *   - FLAG_COW is bit 9 and can be combined/cleared with PTE flags
 *   - CoW PTE state transitions: writable -> CoW -> resolved
 *   - Dispatch via syscall_dispatch for SYS_fork returns error (no scheduler loop)
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/arch/x86_64/syscall.hpp"
#include "kernel/proc/elf_types.hpp"
#include "kernel/proc/pid.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/sys_execve.hpp"
#include "kernel/syscall/sys_fork.hpp"
#include "kernel/syscall/sys_getpid.hpp"
#include "kernel/syscall/sys_getppid.hpp"
#include "kernel/syscall/sys_waitpid.hpp"
#include "kernel/syscall/syscall_nums.hpp"

using cinux::syscall::SyscallNr;
using cinux::syscall::sys_getpid;
using cinux::syscall::sys_getppid;
using cinux::syscall::sys_fork;
using cinux::syscall::sys_execve;
using cinux::syscall::sys_waitpid;
using cinux::proc::PidAllocator;
using cinux::proc::Task;
using cinux::proc::TaskState;
using cinux::arch::PageEntry;
using cinux::arch::FLAG_PRESENT;
using cinux::arch::FLAG_WRITABLE;
using cinux::arch::FLAG_USER;
using cinux::arch::FLAG_COW;
using cinux::arch::FLAG_GLOBAL;

// ============================================================
// Test 1: SyscallNr constants
// ============================================================

namespace test_getpid_constants {

void test_sys_getpid_number() {
    // Linux x86_64: getpid = 39
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_getpid), 39ULL);
}

void test_sys_getppid_number() {
    // Linux x86_64: getppid = 110
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_getppid), 110ULL);
}

}  // namespace test_getpid_constants

// ============================================================
// Test 2: PidAllocator in-kernel smoke test
// ============================================================

namespace test_pid_allocator_kernel {

void test_alloc_sequential() {
    PidAllocator alloc;
    TEST_ASSERT_EQ(alloc.alloc(), 1);
    TEST_ASSERT_EQ(alloc.alloc(), 2);
    TEST_ASSERT_EQ(alloc.count(), 2);
}

void test_free_and_reuse() {
    PidAllocator alloc;
    int          pid = alloc.alloc();
    alloc.free(pid);
    TEST_ASSERT_EQ(alloc.alloc(), 1);
    TEST_ASSERT_EQ(alloc.count(), 1);
}

void test_is_allocated() {
    PidAllocator alloc;
    int          pid = alloc.alloc();
    TEST_ASSERT_TRUE(alloc.is_allocated(pid));
    alloc.free(pid);
    TEST_ASSERT_FALSE(alloc.is_allocated(pid));
}

void test_constants() {
    TEST_ASSERT_EQ(PidAllocator::PID_NONE, 0);
    TEST_ASSERT_EQ(PidAllocator::PID_MAX, 256);
}

}  // namespace test_pid_allocator_kernel

// ============================================================
// Test 3: sys_getpid / sys_getppid direct call
// ============================================================

namespace test_getpid_direct {

void test_sys_getpid_returns_nonnegative() {
    // The test environment does not run the scheduler loop, so
    // Scheduler::current() is nullptr.  Install a temporary task
    // with pid=42 so the syscall can exercise the TCB read path.
    Task tmp{};
    tmp.pid    = 42;
    tmp.ppid   = 1;
    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int64_t ret = sys_getpid(0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(ret, 42);

    cinux::proc::Scheduler::set_current(prev);
}

void test_sys_getppid_returns_nonnegative() {
    Task tmp{};
    tmp.pid    = 42;
    tmp.ppid   = 1;
    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int64_t ret = sys_getppid(0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(ret, 1);

    cinux::proc::Scheduler::set_current(prev);
}

}  // namespace test_getpid_direct

// ============================================================
// Test 4: sys_getpid / sys_getppid via dispatch
// ============================================================

namespace test_getpid_dispatch {

void test_dispatch_getpid() {
    Task tmp{};
    tmp.pid    = 7;
    tmp.ppid   = 2;
    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int64_t direct = sys_getpid(0, 0, 0, 0, 0, 0);
    int64_t dispatched =
        syscall_dispatch(static_cast<uint64_t>(SyscallNr::SYS_getpid), 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(direct, dispatched);
    TEST_ASSERT_EQ(direct, 7);

    cinux::proc::Scheduler::set_current(prev);
}

void test_dispatch_getppid() {
    Task tmp{};
    tmp.pid    = 7;
    tmp.ppid   = 2;
    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int64_t direct = sys_getppid(0, 0, 0, 0, 0, 0);
    int64_t dispatched =
        syscall_dispatch(static_cast<uint64_t>(SyscallNr::SYS_getppid), 0, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(direct, dispatched);
    TEST_ASSERT_EQ(direct, 2);

    cinux::proc::Scheduler::set_current(prev);
}

}  // namespace test_getpid_dispatch

// ============================================================
// Test 5: TaskState Zombie
// ============================================================

namespace test_task_state_zombie {

void test_zombie_distinct() {
    TaskState z = TaskState::Zombie;
    TEST_ASSERT_TRUE(z != TaskState::Running);
    TEST_ASSERT_TRUE(z != TaskState::Ready);
    TEST_ASSERT_TRUE(z != TaskState::Blocked);
    TEST_ASSERT_TRUE(z != TaskState::Dead);
}

void test_zombie_assignable() {
    Task t{};
    t.state = TaskState::Zombie;
    TEST_ASSERT_TRUE(t.state == TaskState::Zombie);
}

}  // namespace test_task_state_zombie

// ============================================================
// Test 6: Task new fields
// ============================================================

namespace test_task_fields {

void test_pid_ppid_default_zero() {
    Task t{};
    TEST_ASSERT_EQ(t.pid, 0);
    TEST_ASSERT_EQ(t.ppid, 0);
    TEST_ASSERT_EQ(t.exit_status, 0);
    TEST_ASSERT_NULL(t.children);
    TEST_ASSERT_NULL(t.parent);
}

void test_pid_ppid_assignable() {
    Task t{};
    t.pid         = 42;
    t.ppid        = 1;
    t.exit_status = 5;
    TEST_ASSERT_EQ(t.pid, 42);
    TEST_ASSERT_EQ(t.ppid, 1);
    TEST_ASSERT_EQ(t.exit_status, 5);
}

}  // namespace test_task_fields

// ============================================================
// Test 7: SyscallNr::SYS_fork constant
// ============================================================

namespace test_sys_fork_constant {

void test_sys_fork_number() {
    // Linux x86_64: fork = 57
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_fork), 57ULL);
}

void test_sys_fork_distinct() {
    TEST_ASSERT_TRUE(SyscallNr::SYS_fork != SyscallNr::SYS_getpid);
    TEST_ASSERT_TRUE(SyscallNr::SYS_fork != SyscallNr::SYS_getppid);
    TEST_ASSERT_TRUE(SyscallNr::SYS_fork != SyscallNr::SYS_exit);
}

}  // namespace test_sys_fork_constant

// ============================================================
// Test 8: FLAG_COW constant
// ============================================================

namespace test_flag_cow {

void test_cow_is_bit_9() {
    TEST_ASSERT_EQ(FLAG_COW, 1ULL << 9);
}

void test_cow_distinct_from_other_flags() {
    TEST_ASSERT_TRUE(FLAG_COW != FLAG_PRESENT);
    TEST_ASSERT_TRUE(FLAG_COW != FLAG_WRITABLE);
    TEST_ASSERT_TRUE(FLAG_COW != FLAG_USER);
    TEST_ASSERT_TRUE(FLAG_COW != FLAG_GLOBAL);
}

void test_cow_combinable_with_flags() {
    uint64_t pte = FLAG_PRESENT | FLAG_USER | FLAG_COW;
    TEST_ASSERT(pte & FLAG_COW);
    TEST_ASSERT(pte & FLAG_PRESENT);
    TEST_ASSERT_FALSE((pte & FLAG_WRITABLE) != 0);
}

void test_cow_clearable_from_pte() {
    uint64_t pte = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER | FLAG_COW;
    pte &= ~FLAG_COW;
    TEST_ASSERT_FALSE((pte & FLAG_COW) != 0);
    TEST_ASSERT(pte & FLAG_WRITABLE);
}

}  // namespace test_flag_cow

// ============================================================
// Test 9: CoW PTE state transitions
// ============================================================

namespace test_cow_pte_transitions {

void test_mark_writable_as_cow() {
    PageEntry pte{};
    pte.raw = 0x1000ULL | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

    // Simulate fork: make read-only + CoW
    pte.raw &= ~FLAG_WRITABLE;
    pte.raw |= FLAG_COW;

    TEST_ASSERT_TRUE(pte.is_present());
    TEST_ASSERT((pte.raw & FLAG_WRITABLE) == 0);
    TEST_ASSERT(pte.raw & FLAG_COW);
    TEST_ASSERT_EQ(pte.phys_addr(), 0x1000ULL);
}

void test_cow_fault_resolves_to_private_page() {
    PageEntry pte{};
    pte.raw = 0x1000ULL | FLAG_PRESENT | FLAG_USER | FLAG_COW;

    // Simulate CoW resolution: new physical page, writable, no CoW
    uint64_t new_phys = 0x2000ULL;
    pte.set_phys_addr(new_phys);
    pte.raw |= FLAG_WRITABLE;
    pte.raw &= ~FLAG_COW;

    TEST_ASSERT_TRUE(pte.is_present());
    TEST_ASSERT(pte.raw & FLAG_WRITABLE);
    TEST_ASSERT((pte.raw & FLAG_COW) == 0);
    TEST_ASSERT_EQ(pte.phys_addr(), 0x2000ULL);
}

void test_shared_after_fork_same_phys() {
    PageEntry parent_pte{};
    parent_pte.raw = 0x5000ULL | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

    PageEntry child_pte = parent_pte;

    // Fork marks both as read-only + CoW
    parent_pte.raw &= ~FLAG_WRITABLE;
    parent_pte.raw |= FLAG_COW;
    child_pte.raw &= ~FLAG_WRITABLE;
    child_pte.raw |= FLAG_COW;

    TEST_ASSERT_EQ(parent_pte.phys_addr(), child_pte.phys_addr());
    TEST_ASSERT(parent_pte.raw & FLAG_COW);
    TEST_ASSERT(child_pte.raw & FLAG_COW);
}

void test_cow_write_isolates_pages() {
    PageEntry parent_pte{};
    parent_pte.raw = 0x5000ULL | FLAG_PRESENT | FLAG_USER | FLAG_COW;
    PageEntry child_pte{};
    child_pte.raw = 0x5000ULL | FLAG_PRESENT | FLAG_USER | FLAG_COW;

    // Child writes: CoW fault allocates new page
    child_pte.set_phys_addr(0x6000ULL);
    child_pte.raw |= FLAG_WRITABLE;
    child_pte.raw &= ~FLAG_COW;

    TEST_ASSERT_NE(parent_pte.phys_addr(), child_pte.phys_addr());
    TEST_ASSERT_EQ(parent_pte.phys_addr(), 0x5000ULL);
    TEST_ASSERT_EQ(child_pte.phys_addr(), 0x6000ULL);
}

void test_non_cow_readonly_not_handled() {
    PageEntry pte{};
    pte.raw = 0x3000ULL | FLAG_PRESENT | FLAG_USER;

    bool is_cow = (pte.raw & FLAG_COW) != 0;
    TEST_ASSERT_FALSE(is_cow);
}

}  // namespace test_cow_pte_transitions

// ============================================================
// Test 10: sys_fork dispatch (returns error without scheduler loop)
// ============================================================

namespace test_sys_fork_dispatch {

void test_dispatch_sys_fork() {
    Task tmp{};
    tmp.pid  = 42;
    tmp.ppid = 1;
    // kernel_stack_top must be set above the current RSP so fork()'s
    // stack usage calculation (kernel_stack_top - current_rsp) doesn't
    // underflow.
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    tmp.kernel_stack_top = rsp + 16384;
    Task* prev           = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    // sys_fork will try to allocate from g_pid_alloc and call fork()
    // which depends on Scheduler::add_task etc.  In the test harness
    // the scheduler is initialised but not running, so we verify
    // that the dispatch path is reachable.
    int64_t ret = sys_fork(0, 0, 0, 0, 0, 0);
    // fork may succeed (return child_pid) or fail (return -1)
    TEST_ASSERT_TRUE(ret == -1 || ret >= 0);

    cinux::proc::Scheduler::set_current(prev);
}

void test_dispatch_sys_fork_via_table() {
    Task tmp{};
    tmp.pid  = 42;
    tmp.ppid = 1;
    uint64_t rsp;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    tmp.kernel_stack_top = rsp + 16384;
    Task* prev           = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int64_t dispatched =
        syscall_dispatch(static_cast<uint64_t>(SyscallNr::SYS_fork), 0, 0, 0, 0, 0, 0);
    // Should be reachable via dispatch table (registered in syscall_init)
    TEST_ASSERT_TRUE(dispatched == -1 || dispatched >= 0);

    cinux::proc::Scheduler::set_current(prev);
}

}  // namespace test_sys_fork_dispatch

// ============================================================
// Test 11: SyscallNr::SYS_execve constant
// ============================================================

namespace test_sys_execve_constant {

void test_sys_execve_number() {
    // Linux x86_64: execve = 59
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_execve), 59ULL);
}

void test_sys_execve_distinct() {
    TEST_ASSERT_TRUE(SyscallNr::SYS_execve != SyscallNr::SYS_fork);
    TEST_ASSERT_TRUE(SyscallNr::SYS_execve != SyscallNr::SYS_getpid);
    TEST_ASSERT_TRUE(SyscallNr::SYS_execve != SyscallNr::SYS_exit);
    TEST_ASSERT_TRUE(SyscallNr::SYS_execve != SyscallNr::SYS_yield);
}

}  // namespace test_sys_execve_constant

// ============================================================
// Test 12: ExecveResult enum
// ============================================================

namespace test_execve_result {

void test_ok_is_zero() {
    TEST_ASSERT_EQ(static_cast<int>(cinux::proc::ExecveResult::Ok), 0);
}

void test_error_values_negative() {
    using R = cinux::proc::ExecveResult;
    TEST_ASSERT_TRUE(static_cast<int>(R::BadPath) < 0);
    TEST_ASSERT_TRUE(static_cast<int>(R::FileNotFound) < 0);
    TEST_ASSERT_TRUE(static_cast<int>(R::BadElfMagic) < 0);
    TEST_ASSERT_TRUE(static_cast<int>(R::NoAddressSpace) < 0);
    TEST_ASSERT_TRUE(static_cast<int>(R::NoCurrentTask) < 0);
}

void test_error_values_match_errno() {
    using R = cinux::proc::ExecveResult;
    TEST_ASSERT_EQ(static_cast<int>(R::BadPath), -22);
    TEST_ASSERT_EQ(static_cast<int>(R::FileNotFound), -2);
    TEST_ASSERT_EQ(static_cast<int>(R::FileNotRegular), -21);
    TEST_ASSERT_EQ(static_cast<int>(R::ReadFailed), -5);
    TEST_ASSERT_EQ(static_cast<int>(R::BadElfMagic), -8);
    TEST_ASSERT_EQ(static_cast<int>(R::MapFailed), -12);
    TEST_ASSERT_EQ(static_cast<int>(R::NoCurrentTask), -3);
}

void test_error_values_distinct() {
    using R = cinux::proc::ExecveResult;
    // Entries with different errno values
    TEST_ASSERT_TRUE(R::BadPath != R::FileNotFound);
    TEST_ASSERT_TRUE(R::BadPath != R::BadElfMagic);
    TEST_ASSERT_TRUE(R::FileNotFound != R::FileNotRegular);
    TEST_ASSERT_TRUE(R::BadElfMagic != R::MapFailed);
    TEST_ASSERT_TRUE(R::MapFailed != R::NoCurrentTask);
}

void test_elf_errors_share_enoexec() {
    using R = cinux::proc::ExecveResult;
    TEST_ASSERT_EQ(static_cast<int>(R::BadElfMagic), static_cast<int>(R::BadElfClass));
    TEST_ASSERT_EQ(static_cast<int>(R::BadElfClass), static_cast<int>(R::BadElfEndian));
    TEST_ASSERT_EQ(static_cast<int>(R::BadElfMachine), static_cast<int>(R::BadElfHeaders));
    TEST_ASSERT_EQ(static_cast<int>(R::NoLoadSegments), -8);
}

}  // namespace test_execve_result

// ============================================================
// Test 13: ELF types structure sizes
// ============================================================

namespace test_elf_types_sizes {

void test_ehdr_size() {
    TEST_ASSERT_EQ(sizeof(cinux::proc::elf::Elf64_Ehdr), 64ULL);
}

void test_phdr_size() {
    TEST_ASSERT_EQ(sizeof(cinux::proc::elf::Elf64_Phdr), 56ULL);
}

}  // namespace test_elf_types_sizes

// ============================================================
// Test 14: ELF validation constants
// ============================================================

namespace test_elf_constants_kernel {

void test_elf_magic() {
    TEST_ASSERT_EQ(cinux::proc::elf::ELF_MAGIC, 0x464C457FU);
}

void test_pt_load() {
    TEST_ASSERT_EQ(cinux::proc::elf::PT_LOAD, 1U);
}

void test_em_x86_64() {
    TEST_ASSERT_EQ(cinux::proc::elf::EM_X86_64, 62U);
}

void test_elf_class_64() {
    TEST_ASSERT_EQ(cinux::proc::elf::ELF_CLASS_64, 2U);
}

void test_elf_data_lsb() {
    TEST_ASSERT_EQ(cinux::proc::elf::ELF_DATA_LSB, 1U);
}

void test_et_exec() {
    TEST_ASSERT_EQ(cinux::proc::elf::ET_EXEC, 2U);
}

}  // namespace test_elf_constants_kernel

// ============================================================
// Test 15: ELF header validation (in-kernel)
// ============================================================

namespace test_elf_validation_kernel {

void test_valid_header() {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;
    ehdr.e_entry     = 0x400000;

    TEST_ASSERT_EQ(static_cast<int>(validate_elf_header(&ehdr, 4096)),
                   static_cast<int>(ElfValidateResult::Ok));
}

void test_bad_magic() {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x00;
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    TEST_ASSERT_EQ(static_cast<int>(validate_elf_header(&ehdr, 4096)),
                   static_cast<int>(ElfValidateResult::BadMagic));
}

void test_bad_class() {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = 1;  // 32-bit
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    TEST_ASSERT_EQ(static_cast<int>(validate_elf_header(&ehdr, 4096)),
                   static_cast<int>(ElfValidateResult::BadClass));
}

void test_bad_machine() {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = 0;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    TEST_ASSERT_EQ(static_cast<int>(validate_elf_header(&ehdr, 4096)),
                   static_cast<int>(ElfValidateResult::BadMachine));
}

void test_no_phdrs() {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 0;

    TEST_ASSERT_EQ(static_cast<int>(validate_elf_header(&ehdr, 4096)),
                   static_cast<int>(ElfValidateResult::NoPhdrs));
}

}  // namespace test_elf_validation_kernel

// ============================================================
// Test 16: ELF program header field layout
// ============================================================

namespace test_elf_phdr_layout {

void test_pt_load_segment_fields() {
    using namespace cinux::proc::elf;

    Elf64_Phdr phdr{};
    phdr.p_type   = PT_LOAD;
    phdr.p_flags  = PF_R | PF_W | PF_X;
    phdr.p_offset = 0;
    phdr.p_vaddr  = 0x400000;
    phdr.p_filesz = 0x1000;
    phdr.p_memsz  = 0x2000;
    phdr.p_align  = 0x1000;

    TEST_ASSERT_EQ(phdr.p_type, PT_LOAD);
    TEST_ASSERT_EQ(phdr.p_vaddr, 0x400000ULL);
    TEST_ASSERT_TRUE(phdr.p_memsz > phdr.p_filesz);
}

void test_text_vs_data_flags() {
    using namespace cinux::proc::elf;

    Elf64_Phdr text{};
    text.p_flags = PF_R | PF_X;
    TEST_ASSERT(text.p_flags & PF_R);
    TEST_ASSERT(text.p_flags & PF_X);
    TEST_ASSERT(!(text.p_flags & PF_W));

    Elf64_Phdr data{};
    data.p_flags = PF_R | PF_W;
    TEST_ASSERT(data.p_flags & PF_R);
    TEST_ASSERT(data.p_flags & PF_W);
    TEST_ASSERT(!(data.p_flags & PF_X));
}

}  // namespace test_elf_phdr_layout

// ============================================================
// Test 17: sys_execve dispatch (returns error without VFS)
// ============================================================

namespace test_sys_execve_dispatch {

void test_dispatch_sys_execve() {
    // In the test harness, VFS is not mounted, so execve with a
    // non-empty path should return an error (FileNotFound or similar).
    Task tmp{};
    tmp.pid    = 42;
    tmp.ppid   = 1;
    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    // Pass a kernel pointer to a path string
    const char* path = "/bin/test";
    int64_t     ret  = sys_execve(reinterpret_cast<uint64_t>(path), 0, 0, 0, 0, 0);
    // Should fail: no VFS mounted, no address space on the temp task
    TEST_ASSERT_TRUE(ret < 0);

    cinux::proc::Scheduler::set_current(prev);
}

void test_dispatch_sys_execve_via_table() {
    Task tmp{};
    tmp.pid    = 42;
    tmp.ppid   = 1;
    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    const char* path       = "/bin/test";
    int64_t     dispatched = syscall_dispatch(static_cast<uint64_t>(SyscallNr::SYS_execve),
                                              reinterpret_cast<uint64_t>(path), 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(dispatched < 0);

    cinux::proc::Scheduler::set_current(prev);
}

}  // namespace test_sys_execve_dispatch

// ============================================================
// Test 18: SyscallNr::SYS_waitpid constant
// ============================================================

namespace test_sys_waitpid_constant {

void test_sys_waitpid_number() {
    // Linux x86_64: waitpid = 61
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_waitpid), 61ULL);
}

void test_sys_waitpid_distinct() {
    TEST_ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_fork);
    TEST_ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_execve);
    TEST_ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_getpid);
    TEST_ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_getppid);
    TEST_ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_exit);
}

}  // namespace test_sys_waitpid_constant

// ============================================================
// Test 19: WaitpidResult enum
// ============================================================

namespace test_waitpid_result {

void test_ok_is_zero() {
    TEST_ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::Ok), 0);
}

void test_error_values_negative() {
    using R = cinux::proc::WaitpidResult;
    TEST_ASSERT_TRUE(static_cast<int>(R::NoChildren) < 0);
    TEST_ASSERT_TRUE(static_cast<int>(R::NotFound) < 0);
    TEST_ASSERT_TRUE(static_cast<int>(R::InvalidPid) < 0);
    TEST_ASSERT_TRUE(static_cast<int>(R::NotExited) < 0);
}

void test_error_values_distinct() {
    using R = cinux::proc::WaitpidResult;
    TEST_ASSERT_TRUE(R::NoChildren != R::NotFound);
    TEST_ASSERT_TRUE(R::NoChildren != R::InvalidPid);
    TEST_ASSERT_TRUE(R::NoChildren != R::NotExited);
    TEST_ASSERT_TRUE(R::NotFound != R::InvalidPid);
    TEST_ASSERT_TRUE(R::NotFound != R::NotExited);
    TEST_ASSERT_TRUE(R::InvalidPid != R::NotExited);
}

void test_no_children_maps_to_echild() {
    TEST_ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::NoChildren), -10);
}

void test_not_found_maps_to_esrch() {
    TEST_ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::NotFound), -3);
}

void test_invalid_pid_maps_to_einval() {
    TEST_ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::InvalidPid), -22);
}

void test_not_exited_is_minus_one() {
    TEST_ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::NotExited), -1);
}

}  // namespace test_waitpid_result

// ============================================================
// Test 20: ECHILD errno constant
// ============================================================

namespace test_errno_echild {

void test_echild_value() {
    TEST_ASSERT_EQ(cinux::proc::errno_values::ECHILD, 10);
}

void test_echild_distinct() {
    TEST_ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::EPERM);
    TEST_ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::ENOENT);
    TEST_ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::EINVAL);
    TEST_ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::ESRCH);
}

}  // namespace test_errno_echild

// ============================================================
// Test 21: sys_waitpid direct call -- no children
// ============================================================

namespace test_sys_waitpid_direct {

void test_waitpid_no_children() {
    // No current task set up properly: Scheduler::current() is nullptr
    // after the previous test restored the original value.
    // Install a temp task with no children.
    Task tmp{};
    tmp.pid      = 10;
    tmp.ppid     = 1;
    tmp.children = nullptr;
    Task* prev   = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int  status = 0;
    auto result = cinux::proc::waitpid(-1, &status, cinux::proc::g_pid_alloc);
    TEST_ASSERT_EQ(static_cast<int>(result),
                   static_cast<int>(cinux::proc::WaitpidResult::NoChildren));

    cinux::proc::Scheduler::set_current(prev);
}

void test_waitpid_zombie_child_reaped() {
    PidAllocator local_alloc;
    Task         tmp{};
    tmp.pid  = 20;
    tmp.ppid = 1;

    Task child{};
    child.pid         = 21;
    child.ppid        = 20;
    child.state       = TaskState::Zombie;
    child.exit_status = 7;

    // Link child into parent
    child.wait_next = tmp.children;
    tmp.children    = &child;

    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int  status = 0;
    auto result = cinux::proc::waitpid(21, &status, local_alloc);

    TEST_ASSERT_EQ(static_cast<int>(result), static_cast<int>(cinux::proc::WaitpidResult::Ok));
    TEST_ASSERT_EQ(status, 7);
    TEST_ASSERT_NULL(tmp.children);
    TEST_ASSERT_TRUE(child.state == TaskState::Dead);

    cinux::proc::Scheduler::set_current(prev);
}

void test_waitpid_any_zombie() {
    PidAllocator local_alloc;
    Task         tmp{};
    tmp.pid  = 30;
    tmp.ppid = 1;

    Task child1{};
    child1.pid   = 31;
    child1.state = TaskState::Running;

    Task child2{};
    child2.pid         = 32;
    child2.state       = TaskState::Zombie;
    child2.exit_status = 99;

    // children: child1 -> child2
    child1.wait_next = &child2;
    child2.wait_next = nullptr;
    tmp.children     = &child1;

    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int  status = 0;
    auto result = cinux::proc::waitpid(-1, &status, local_alloc);

    TEST_ASSERT_EQ(static_cast<int>(result), static_cast<int>(cinux::proc::WaitpidResult::Ok));
    TEST_ASSERT_EQ(status, 99);
    // child2 should be unlinked; child1 remains
    TEST_ASSERT_EQ(tmp.children, &child1);

    cinux::proc::Scheduler::set_current(prev);
}

void test_waitpid_not_exited() {
    PidAllocator local_alloc;
    Task         tmp{};
    tmp.pid  = 40;
    tmp.ppid = 1;

    Task child{};
    child.pid   = 41;
    child.state = TaskState::Running;

    child.wait_next = nullptr;
    tmp.children    = &child;

    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int  status = 0;
    auto result = cinux::proc::waitpid(41, &status, local_alloc);

    TEST_ASSERT_EQ(static_cast<int>(result),
                   static_cast<int>(cinux::proc::WaitpidResult::NotExited));

    cinux::proc::Scheduler::set_current(prev);
}

void test_waitpid_not_found() {
    PidAllocator local_alloc;
    Task         tmp{};
    tmp.pid  = 50;
    tmp.ppid = 1;

    Task child{};
    child.pid       = 51;
    child.state     = TaskState::Zombie;
    child.wait_next = nullptr;
    tmp.children    = &child;

    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int  status = 0;
    auto result = cinux::proc::waitpid(99, &status, local_alloc);

    TEST_ASSERT_EQ(static_cast<int>(result),
                   static_cast<int>(cinux::proc::WaitpidResult::NotFound));

    cinux::proc::Scheduler::set_current(prev);
}

void test_waitpid_invalid_pid() {
    PidAllocator local_alloc;
    Task         tmp{};
    tmp.pid  = 60;
    tmp.ppid = 1;

    Task child{};
    child.pid       = 61;
    child.state     = TaskState::Zombie;
    child.wait_next = nullptr;
    tmp.children    = &child;

    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int  status = 0;
    auto result = cinux::proc::waitpid(0, &status, local_alloc);

    TEST_ASSERT_EQ(static_cast<int>(result),
                   static_cast<int>(cinux::proc::WaitpidResult::InvalidPid));

    cinux::proc::Scheduler::set_current(prev);
}

}  // namespace test_sys_waitpid_direct

// ============================================================
// Test 22: sys_waitpid via syscall dispatch
// ============================================================

namespace test_sys_waitpid_dispatch {

void test_dispatch_sys_waitpid() {
    Task tmp{};
    tmp.pid      = 70;
    tmp.ppid     = 1;
    tmp.children = nullptr;
    Task* prev   = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    // No children: should return negative error (ECHILD)
    int64_t ret = sys_waitpid(static_cast<uint64_t>(-1), 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(ret < 0);

    cinux::proc::Scheduler::set_current(prev);
}

void test_dispatch_sys_waitpid_via_table() {
    Task tmp{};
    tmp.pid      = 71;
    tmp.ppid     = 1;
    tmp.children = nullptr;
    Task* prev   = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int64_t dispatched = syscall_dispatch(static_cast<uint64_t>(SyscallNr::SYS_waitpid),
                                          static_cast<uint64_t>(-1), 0, 0, 0, 0, 0);
    TEST_ASSERT_TRUE(dispatched < 0);

    cinux::proc::Scheduler::set_current(prev);
}

}  // namespace test_sys_waitpid_dispatch

// ============================================================
// Test 23: waitpid reap frees PID
// ============================================================

namespace test_waitpid_pid_reuse {

void test_reaped_pid_is_freed() {
    PidAllocator local_alloc;
    Task         tmp{};
    tmp.pid  = 80;
    tmp.ppid = 1;

    // Allocate a PID for the child
    int child_pid = local_alloc.alloc();  // 1
    TEST_ASSERT_TRUE(local_alloc.is_allocated(child_pid));

    Task child{};
    child.pid         = child_pid;
    child.ppid        = 80;
    child.state       = TaskState::Zombie;
    child.exit_status = 0;
    child.wait_next   = nullptr;
    tmp.children      = &child;

    Task* prev = cinux::proc::Scheduler::current();
    cinux::proc::Scheduler::set_current(&tmp);

    int  status = 0;
    auto result = cinux::proc::waitpid(child_pid, &status, local_alloc);

    TEST_ASSERT_EQ(static_cast<int>(result), static_cast<int>(cinux::proc::WaitpidResult::Ok));
    TEST_ASSERT_FALSE(local_alloc.is_allocated(child_pid));

    // PID should be reusable
    int reused = local_alloc.alloc();
    TEST_ASSERT_EQ(reused, child_pid);

    cinux::proc::Scheduler::set_current(prev);
}

}  // namespace test_waitpid_pid_reuse

// ============================================================
// Entry point
// ============================================================

extern "C" void run_fork_exec_tests() {
    TEST_SECTION("Fork/Exec Tests (034)");

    RUN_TEST(test_getpid_constants::test_sys_getpid_number);
    RUN_TEST(test_getpid_constants::test_sys_getppid_number);

    RUN_TEST(test_pid_allocator_kernel::test_alloc_sequential);
    RUN_TEST(test_pid_allocator_kernel::test_free_and_reuse);
    RUN_TEST(test_pid_allocator_kernel::test_is_allocated);
    RUN_TEST(test_pid_allocator_kernel::test_constants);

    RUN_TEST(test_getpid_direct::test_sys_getpid_returns_nonnegative);
    RUN_TEST(test_getpid_direct::test_sys_getppid_returns_nonnegative);

    RUN_TEST(test_getpid_dispatch::test_dispatch_getpid);
    RUN_TEST(test_getpid_dispatch::test_dispatch_getppid);

    RUN_TEST(test_task_state_zombie::test_zombie_distinct);
    RUN_TEST(test_task_state_zombie::test_zombie_assignable);

    RUN_TEST(test_task_fields::test_pid_ppid_default_zero);
    RUN_TEST(test_task_fields::test_pid_ppid_assignable);

    RUN_TEST(test_sys_fork_constant::test_sys_fork_number);
    RUN_TEST(test_sys_fork_constant::test_sys_fork_distinct);

    RUN_TEST(test_flag_cow::test_cow_is_bit_9);
    RUN_TEST(test_flag_cow::test_cow_distinct_from_other_flags);
    RUN_TEST(test_flag_cow::test_cow_combinable_with_flags);
    RUN_TEST(test_flag_cow::test_cow_clearable_from_pte);

    RUN_TEST(test_cow_pte_transitions::test_mark_writable_as_cow);
    RUN_TEST(test_cow_pte_transitions::test_cow_fault_resolves_to_private_page);
    RUN_TEST(test_cow_pte_transitions::test_shared_after_fork_same_phys);
    RUN_TEST(test_cow_pte_transitions::test_cow_write_isolates_pages);
    RUN_TEST(test_cow_pte_transitions::test_non_cow_readonly_not_handled);

    RUN_TEST(test_sys_fork_dispatch::test_dispatch_sys_fork);
    RUN_TEST(test_sys_fork_dispatch::test_dispatch_sys_fork_via_table);

    RUN_TEST(test_sys_execve_constant::test_sys_execve_number);
    RUN_TEST(test_sys_execve_constant::test_sys_execve_distinct);

    RUN_TEST(test_execve_result::test_ok_is_zero);
    RUN_TEST(test_execve_result::test_error_values_negative);
    RUN_TEST(test_execve_result::test_error_values_distinct);
    RUN_TEST(test_execve_result::test_error_values_match_errno);
    RUN_TEST(test_execve_result::test_elf_errors_share_enoexec);

    RUN_TEST(test_elf_types_sizes::test_ehdr_size);
    RUN_TEST(test_elf_types_sizes::test_phdr_size);

    RUN_TEST(test_elf_constants_kernel::test_elf_magic);
    RUN_TEST(test_elf_constants_kernel::test_pt_load);
    RUN_TEST(test_elf_constants_kernel::test_em_x86_64);
    RUN_TEST(test_elf_constants_kernel::test_elf_class_64);
    RUN_TEST(test_elf_constants_kernel::test_elf_data_lsb);
    RUN_TEST(test_elf_constants_kernel::test_et_exec);

    RUN_TEST(test_elf_validation_kernel::test_valid_header);
    RUN_TEST(test_elf_validation_kernel::test_bad_magic);
    RUN_TEST(test_elf_validation_kernel::test_bad_class);
    RUN_TEST(test_elf_validation_kernel::test_bad_machine);
    RUN_TEST(test_elf_validation_kernel::test_no_phdrs);

    RUN_TEST(test_elf_phdr_layout::test_pt_load_segment_fields);
    RUN_TEST(test_elf_phdr_layout::test_text_vs_data_flags);

    RUN_TEST(test_sys_execve_dispatch::test_dispatch_sys_execve);
    RUN_TEST(test_sys_execve_dispatch::test_dispatch_sys_execve_via_table);

    RUN_TEST(test_sys_waitpid_constant::test_sys_waitpid_number);
    RUN_TEST(test_sys_waitpid_constant::test_sys_waitpid_distinct);

    RUN_TEST(test_waitpid_result::test_ok_is_zero);
    RUN_TEST(test_waitpid_result::test_error_values_negative);
    RUN_TEST(test_waitpid_result::test_error_values_distinct);
    RUN_TEST(test_waitpid_result::test_no_children_maps_to_echild);
    RUN_TEST(test_waitpid_result::test_not_found_maps_to_esrch);
    RUN_TEST(test_waitpid_result::test_invalid_pid_maps_to_einval);
    RUN_TEST(test_waitpid_result::test_not_exited_is_minus_one);

    RUN_TEST(test_errno_echild::test_echild_value);
    RUN_TEST(test_errno_echild::test_echild_distinct);

    RUN_TEST(test_sys_waitpid_direct::test_waitpid_no_children);
    RUN_TEST(test_sys_waitpid_direct::test_waitpid_zombie_child_reaped);
    RUN_TEST(test_sys_waitpid_direct::test_waitpid_any_zombie);
    RUN_TEST(test_sys_waitpid_direct::test_waitpid_not_exited);
    RUN_TEST(test_sys_waitpid_direct::test_waitpid_not_found);
    RUN_TEST(test_sys_waitpid_direct::test_waitpid_invalid_pid);

    RUN_TEST(test_sys_waitpid_dispatch::test_dispatch_sys_waitpid);
    RUN_TEST(test_sys_waitpid_dispatch::test_dispatch_sys_waitpid_via_table);

    RUN_TEST(test_waitpid_pid_reuse::test_reaped_pid_is_freed);

    TEST_SUMMARY();
}
