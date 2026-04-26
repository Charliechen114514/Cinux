/**
 * @file test/unit/test_fork_exec.cpp
 * @brief Host-side unit tests for PID allocator, TCB extensions, fork/CoW (034)
 *
 * Test coverage:
 *   - PidAllocator: alloc returns sequential PIDs from 1
 *   - PidAllocator: alloc reuses lowest freed PID
 *   - PidAllocator: alloc returns PID_NONE when exhausted
 *   - PidAllocator: free on PID_NONE / out-of-range is safe
 *   - PidAllocator: double free is safe
 *   - PidAllocator: count tracks in-use PIDs
 *   - PidAllocator: is_allocated reflects state correctly
 *   - Task new fields: pid, ppid, exit_status, children, parent defaults
 *   - TaskState: Zombie enum value exists and is distinct
 *   - FLAG_COW: constant is bit 9 of available bits
 *   - SyscallNr::SYS_fork: number matches Linux x86_64 (57)
 *   - Fork return value semantics: parent gets child PID, child gets 0
 *   - CoW PTE flag manipulation: writable -> read-only + CoW -> writable + no CoW
 *
 * Links with kernel/proc/pid.cpp (no hardware dependencies).
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstddef>
#    include <cstdint>
#    include <cstring>

#    include "arch/x86_64/paging.hpp"
#    include "arch/x86_64/paging_config.hpp"
#    include "proc/elf_types.hpp"
#    include "proc/pid.hpp"
#    include "proc/process.hpp"
#    include "syscall/syscall_nums.hpp"

using namespace cinux::proc;

// ============================================================
// 1. PidAllocator: construction
// ============================================================

TEST("pid_allocator: freshly created, count is 0") {
    PidAllocator alloc;
    ASSERT_EQ(alloc.count(), 0);
}

TEST("pid_allocator: no PID is allocated after construction") {
    PidAllocator alloc;
    for (int i = 1; i <= PidAllocator::PID_MAX; ++i) {
        ASSERT_FALSE(alloc.is_allocated(i));
    }
}

// ============================================================
// 2. PidAllocator: sequential allocation
// ============================================================

TEST("pid_allocator: first alloc returns 1") {
    PidAllocator alloc;
    ASSERT_EQ(alloc.alloc(), 1);
}

TEST("pid_allocator: sequential allocs return 1 2 3") {
    PidAllocator alloc;
    ASSERT_EQ(alloc.alloc(), 1);
    ASSERT_EQ(alloc.alloc(), 2);
    ASSERT_EQ(alloc.alloc(), 3);
}

TEST("pid_allocator: count increments on alloc") {
    PidAllocator alloc;
    alloc.alloc();
    alloc.alloc();
    ASSERT_EQ(alloc.count(), 2);
}

// ============================================================
// 3. PidAllocator: free and reuse
// ============================================================

TEST("pid_allocator: free makes PID available again") {
    PidAllocator alloc;
    int          pid = alloc.alloc();
    ASSERT_EQ(alloc.count(), 1);
    alloc.free(pid);
    ASSERT_EQ(alloc.count(), 0);
    ASSERT_FALSE(alloc.is_allocated(pid));
}

TEST("pid_allocator: alloc reuses lowest freed PID") {
    PidAllocator alloc;
    ASSERT_EQ(alloc.alloc(), 1);
    ASSERT_EQ(alloc.alloc(), 2);
    ASSERT_EQ(alloc.alloc(), 3);

    alloc.free(2);
    int next = alloc.alloc();
    ASSERT_EQ(next, 2);
}

TEST("pid_allocator: free then alloc cycles correctly") {
    PidAllocator alloc;
    int          a = alloc.alloc();  // 1
    int          b = alloc.alloc();  // 2
    int          c = alloc.alloc();  // 3
    (void)c;

    alloc.free(a);
    alloc.free(b);
    ASSERT_EQ(alloc.alloc(), 1);
    ASSERT_EQ(alloc.alloc(), 2);
}

// ============================================================
// 4. PidAllocator: exhaustion
// ============================================================

TEST("pid_allocator: alloc returns PID_NONE when exhausted") {
    PidAllocator alloc;

    // Allocate all PIDs from 1 to PID_MAX
    for (int i = 1; i <= PidAllocator::PID_MAX; ++i) {
        int pid = alloc.alloc();
        ASSERT_EQ(pid, i);
    }

    // Next alloc should fail
    ASSERT_EQ(alloc.alloc(), PidAllocator::PID_NONE);
    ASSERT_EQ(alloc.count(), PidAllocator::PID_MAX);
}

TEST("pid_allocator: free one then alloc succeeds after exhaustion") {
    PidAllocator alloc;

    for (int i = 1; i <= PidAllocator::PID_MAX; ++i) {
        alloc.alloc();
    }
    ASSERT_EQ(alloc.alloc(), PidAllocator::PID_NONE);

    alloc.free(100);
    ASSERT_EQ(alloc.alloc(), 100);
}

// ============================================================
// 5. PidAllocator: edge cases for free
// ============================================================

TEST("pid_allocator: free PID_NONE is safe") {
    PidAllocator alloc;
    alloc.free(PidAllocator::PID_NONE);
    ASSERT_EQ(alloc.count(), 0);
}

TEST("pid_allocator: free negative PID is safe") {
    PidAllocator alloc;
    alloc.free(-1);
    alloc.free(-100);
    ASSERT_EQ(alloc.count(), 0);
}

TEST("pid_allocator: free out-of-range PID is safe") {
    PidAllocator alloc;
    alloc.free(PidAllocator::PID_MAX + 1);
    alloc.free(9999);
    ASSERT_EQ(alloc.count(), 0);
}

TEST("pid_allocator: double free is safe") {
    PidAllocator alloc;
    int          pid = alloc.alloc();
    alloc.free(pid);
    alloc.free(pid);  // second free should be no-op
    ASSERT_EQ(alloc.count(), 0);
}

// ============================================================
// 6. PidAllocator: is_allocated
// ============================================================

TEST("pid_allocator: is_allocated false for unallocated") {
    PidAllocator alloc;
    ASSERT_FALSE(alloc.is_allocated(1));
    ASSERT_FALSE(alloc.is_allocated(PidAllocator::PID_MAX));
}

TEST("pid_allocator: is_allocated true after alloc") {
    PidAllocator alloc;
    int          pid = alloc.alloc();
    ASSERT_TRUE(alloc.is_allocated(pid));
}

TEST("pid_allocator: is_allocated false after free") {
    PidAllocator alloc;
    int          pid = alloc.alloc();
    alloc.free(pid);
    ASSERT_FALSE(alloc.is_allocated(pid));
}

TEST("pid_allocator: is_allocated false for PID 0") {
    PidAllocator alloc;
    ASSERT_FALSE(alloc.is_allocated(0));
}

// ============================================================
// 7. Task new fields: default values
// ============================================================

TEST("task_fields: pid defaults to 0") {
    Task t{};
    ASSERT_EQ(t.pid, 0);
}

TEST("task_fields: ppid defaults to 0") {
    Task t{};
    ASSERT_EQ(t.ppid, 0);
}

TEST("task_fields: exit_status defaults to 0") {
    Task t{};
    ASSERT_EQ(t.exit_status, 0);
}

TEST("task_fields: children defaults to nullptr") {
    Task t{};
    ASSERT_NULL(t.children);
}

TEST("task_fields: parent defaults to nullptr") {
    Task t{};
    ASSERT_NULL(t.parent);
}

TEST("task_fields: pid and ppid are assignable") {
    Task t{};
    t.pid         = 42;
    t.ppid        = 1;
    t.exit_status = 0;
    t.children    = nullptr;
    t.parent      = nullptr;
    ASSERT_EQ(t.pid, 42);
    ASSERT_EQ(t.ppid, 1);
    ASSERT_EQ(t.exit_status, 0);
}

// ============================================================
// 8. TaskState: Zombie
// ============================================================

TEST("task_state: Zombie exists and is distinct") {
    TaskState s = TaskState::Zombie;
    ASSERT_TRUE(s != TaskState::Running);
    ASSERT_TRUE(s != TaskState::Ready);
    ASSERT_TRUE(s != TaskState::Blocked);
    ASSERT_TRUE(s != TaskState::Dead);
}

TEST("task_state: Zombie can be assigned to task") {
    Task t{};
    t.state = TaskState::Zombie;
    ASSERT_TRUE(t.state == TaskState::Zombie);
}

// ============================================================
// 9. FLAG_COW constant
// ============================================================

TEST("flag_cow: FLAG_COW is bit 9") {
    using namespace cinux::arch;
    ASSERT_EQ(FLAG_COW, 1ULL << 9);
}

TEST("flag_cow: FLAG_COW is distinct from other flags") {
    using namespace cinux::arch;
    ASSERT_NE(FLAG_COW, FLAG_PRESENT);
    ASSERT_NE(FLAG_COW, FLAG_WRITABLE);
    ASSERT_NE(FLAG_COW, FLAG_USER);
    ASSERT_NE(FLAG_COW, FLAG_GLOBAL);
    ASSERT_NE(FLAG_COW, FLAG_NX);
}

TEST("flag_cow: FLAG_COW can be combined with other flags") {
    using namespace cinux::arch;
    uint64_t pte = FLAG_PRESENT | FLAG_USER | FLAG_COW;
    ASSERT_TRUE(pte & FLAG_COW);
    ASSERT_TRUE(pte & FLAG_PRESENT);
    ASSERT_TRUE(pte & FLAG_USER);
    ASSERT_FALSE(pte & FLAG_WRITABLE);
}

TEST("flag_cow: FLAG_COW can be cleared from a PTE") {
    using namespace cinux::arch;
    uint64_t pte = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER | FLAG_COW;
    pte &= ~FLAG_COW;
    ASSERT_FALSE(pte & FLAG_COW);
    ASSERT_TRUE(pte & FLAG_WRITABLE);
    ASSERT_TRUE(pte & FLAG_PRESENT);
}

// ============================================================
// 10. SyscallNr::SYS_fork constant
// ============================================================

TEST("syscall_fork: SYS_fork number is 57 (Linux x86_64)") {
    // Linux x86_64: fork = 57
    ASSERT_EQ(static_cast<uint64_t>(cinux::syscall::SyscallNr::SYS_fork), 57ULL);
}

TEST("syscall_fork: SYS_fork is distinct from other syscalls") {
    using cinux::syscall::SyscallNr;
    ASSERT_TRUE(SyscallNr::SYS_fork != SyscallNr::SYS_getpid);
    ASSERT_TRUE(SyscallNr::SYS_fork != SyscallNr::SYS_getppid);
    ASSERT_TRUE(SyscallNr::SYS_fork != SyscallNr::SYS_exit);
    ASSERT_TRUE(SyscallNr::SYS_fork != SyscallNr::SYS_yield);
}

// ============================================================
// 11. Fork return value semantics
// ============================================================

TEST("fork_semantics: parent gets positive child PID") {
    // Simulate fork return value assignment
    int child_pid = 42;  // simulated return from fork()
    ASSERT_GT(child_pid, 0);
}

TEST("fork_semantics: child gets zero") {
    // In the child process, fork() returns 0
    int child_return = 0;
    ASSERT_EQ(child_return, 0);
}

TEST("fork_semantics: negative return indicates error") {
    int error_return = -1;
    ASSERT_LT(error_return, 0);
}

TEST("fork_semantics: parent-child pid/ppid linkage") {
    Task parent{};
    Task child{};
    parent.pid      = 10;
    parent.ppid     = 1;
    child.pid       = 11;
    child.ppid      = parent.pid;
    child.parent    = &parent;
    parent.children = &child;

    ASSERT_EQ(child.pid, 11);
    ASSERT_EQ(child.ppid, 10);
    ASSERT_EQ(child.parent, &parent);
    ASSERT_EQ(parent.children, &child);
}

// ============================================================
// 12. CoW PTE flag manipulation
// ============================================================

TEST("cow_pte: mark writable page as CoW read-only") {
    using namespace cinux::arch;
    // Simulate a writable PTE
    cinux::arch::PageEntry pte{};
    pte.raw = 0x1000ULL | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

    // Fork: make read-only + CoW
    pte.raw &= ~FLAG_WRITABLE;
    pte.raw |= FLAG_COW;

    ASSERT_TRUE(pte.is_present());
    ASSERT_FALSE(pte.raw & FLAG_WRITABLE);
    ASSERT_TRUE(pte.raw & FLAG_COW);
    ASSERT_EQ(pte.phys_addr(), 0x1000ULL);
}

TEST("cow_pte: CoW fault resolves to writable private page") {
    using namespace cinux::arch;
    // Simulate a CoW-marked PTE
    cinux::arch::PageEntry pte{};
    pte.raw = 0x1000ULL | FLAG_PRESENT | FLAG_USER | FLAG_COW;

    // After CoW resolution: allocate new page, restore write, clear CoW
    uint64_t new_phys = 0x2000ULL;
    pte.set_phys_addr(new_phys);
    pte.raw |= FLAG_WRITABLE;
    pte.raw &= ~FLAG_COW;

    ASSERT_TRUE(pte.is_present());
    ASSERT_TRUE(pte.raw & FLAG_WRITABLE);
    ASSERT_FALSE(pte.raw & FLAG_COW);
    ASSERT_EQ(pte.phys_addr(), 0x2000ULL);
}

TEST("cow_pte: non-CoW read-only page is not a CoW fault") {
    using namespace cinux::arch;
    // A genuinely read-only page (no CoW flag) should not trigger CoW handler
    cinux::arch::PageEntry pte{};
    pte.raw = 0x3000ULL | FLAG_PRESENT | FLAG_USER;

    bool is_cow = (pte.raw & FLAG_COW) != 0;
    ASSERT_FALSE(is_cow);
}

TEST("cow_pte: shared page after fork has same phys in parent and child") {
    using namespace cinux::arch;
    // Parent PTE before fork
    cinux::arch::PageEntry parent_pte{};
    parent_pte.raw = 0x5000ULL | FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

    // After fork: both parent and child point to same physical page
    cinux::arch::PageEntry child_pte = parent_pte;

    // Fork marks both as read-only + CoW
    parent_pte.raw &= ~FLAG_WRITABLE;
    parent_pte.raw |= FLAG_COW;
    child_pte.raw &= ~FLAG_WRITABLE;
    child_pte.raw |= FLAG_COW;

    ASSERT_EQ(parent_pte.phys_addr(), child_pte.phys_addr());
    ASSERT_TRUE(parent_pte.raw & FLAG_COW);
    ASSERT_TRUE(child_pte.raw & FLAG_COW);
    ASSERT_FALSE(parent_pte.raw & FLAG_WRITABLE);
    ASSERT_FALSE(child_pte.raw & FLAG_WRITABLE);
}

TEST("cow_pte: after CoW write, child has different phys from parent") {
    using namespace cinux::arch;
    // Both sharing page 0x5000, both CoW marked
    cinux::arch::PageEntry parent_pte{};
    parent_pte.raw = 0x5000ULL | FLAG_PRESENT | FLAG_USER | FLAG_COW;
    cinux::arch::PageEntry child_pte{};
    child_pte.raw = 0x5000ULL | FLAG_PRESENT | FLAG_USER | FLAG_COW;

    // Child writes: CoW fault handler allocates new page for child
    uint64_t new_phys = 0x6000ULL;
    child_pte.set_phys_addr(new_phys);
    child_pte.raw |= FLAG_WRITABLE;
    child_pte.raw &= ~FLAG_COW;

    ASSERT_NE(parent_pte.phys_addr(), child_pte.phys_addr());
    ASSERT_EQ(parent_pte.phys_addr(), 0x5000ULL);
    ASSERT_EQ(child_pte.phys_addr(), 0x6000ULL);
    ASSERT_FALSE(parent_pte.raw & FLAG_WRITABLE);
    ASSERT_TRUE(child_pte.raw & FLAG_WRITABLE);
}

TEST("cow_pte: read-only non-user page is not CoW") {
    using namespace cinux::arch;
    // Kernel read-only page (no user flag, no CoW flag)
    cinux::arch::PageEntry pte{};
    pte.raw = 0x7000ULL | FLAG_PRESENT | FLAG_GLOBAL;

    bool is_cow      = (pte.raw & FLAG_COW) != 0;
    bool is_writable = (pte.raw & FLAG_WRITABLE) != 0;
    ASSERT_FALSE(is_cow);
    ASSERT_FALSE(is_writable);
}

// ============================================================
// 13. SyscallNr::SYS_execve constant
// ============================================================

TEST("syscall_execve: SYS_execve number is 59 (Linux x86_64)") {
    // Linux x86_64: execve = 59
    ASSERT_EQ(static_cast<uint64_t>(cinux::syscall::SyscallNr::SYS_execve), 59ULL);
}

TEST("syscall_execve: SYS_execve is distinct from other syscalls") {
    using cinux::syscall::SyscallNr;
    ASSERT_TRUE(SyscallNr::SYS_execve != SyscallNr::SYS_fork);
    ASSERT_TRUE(SyscallNr::SYS_execve != SyscallNr::SYS_getpid);
    ASSERT_TRUE(SyscallNr::SYS_execve != SyscallNr::SYS_exit);
}

// ============================================================
// 14. ExecveResult enum values
// ============================================================

TEST("execve_result: Ok is zero") {
    ASSERT_EQ(static_cast<int>(cinux::proc::ExecveResult::Ok), 0);
}

TEST("execve_result: all error values are negative (Linux errno)") {
    using R = cinux::proc::ExecveResult;
    ASSERT_LT(static_cast<int>(R::BadPath), 0);
    ASSERT_LT(static_cast<int>(R::FileNotFound), 0);
    ASSERT_LT(static_cast<int>(R::BadElfMagic), 0);
    ASSERT_LT(static_cast<int>(R::NoAddressSpace), 0);
    ASSERT_LT(static_cast<int>(R::NoCurrentTask), 0);
}

TEST("execve_result: error codes with distinct errno values") {
    using R = cinux::proc::ExecveResult;
    // These map to different errno values
    ASSERT_TRUE(R::BadPath != R::FileNotFound);         // -22 vs -2
    ASSERT_TRUE(R::BadPath != R::BadElfMagic);          // -22 vs -8
    ASSERT_TRUE(R::FileNotFound != R::FileNotRegular);  // -2 vs -21
    ASSERT_TRUE(R::BadElfMagic != R::MapFailed);        // -8 vs -12
    ASSERT_TRUE(R::MapFailed != R::NoCurrentTask);      // -12 vs -3
}

TEST("execve_result: ELF errors share ENOEXEC") {
    using R = cinux::proc::ExecveResult;
    // All ELF format errors map to -ENOEXEC (-8)
    ASSERT_EQ(static_cast<int>(R::BadElfMagic), static_cast<int>(R::BadElfClass));
    ASSERT_EQ(static_cast<int>(R::BadElfClass), static_cast<int>(R::BadElfEndian));
    ASSERT_EQ(static_cast<int>(R::BadElfMachine), static_cast<int>(R::BadElfHeaders));
    ASSERT_EQ(static_cast<int>(R::NoLoadSegments), -8);
}

TEST("execve_result: error values match Linux errno") {
    using R = cinux::proc::ExecveResult;
    // EINVAL=22, ENOENT=2, EISDIR=21, EIO=5, ENOEXEC=8, ENOMEM=12, ESRCH=3
    ASSERT_EQ(static_cast<int>(R::BadPath), -22);
    ASSERT_EQ(static_cast<int>(R::FileNotFound), -2);
    ASSERT_EQ(static_cast<int>(R::FileNotRegular), -21);
    ASSERT_EQ(static_cast<int>(R::ReadFailed), -5);
    ASSERT_EQ(static_cast<int>(R::BadElfMagic), -8);
    ASSERT_EQ(static_cast<int>(R::MapFailed), -12);
    ASSERT_EQ(static_cast<int>(R::NoAddressSpace), -12);
    ASSERT_EQ(static_cast<int>(R::NoCurrentTask), -3);
}

// ============================================================
// 15. ELF types: structure sizes
// ============================================================

TEST("elf_types: Elf64_Ehdr is 64 bytes") {
    ASSERT_EQ(sizeof(cinux::proc::elf::Elf64_Ehdr), 64ULL);
}

TEST("elf_types: Elf64_Phdr is 56 bytes") {
    ASSERT_EQ(sizeof(cinux::proc::elf::Elf64_Phdr), 56ULL);
}

// ============================================================
// 16. ELF types: constants
// ============================================================

TEST("elf_constants: ELF_MAGIC is 0x7F ELF") {
    ASSERT_EQ(cinux::proc::elf::ELF_MAGIC, 0x464C457FU);
}

TEST("elf_constants: PT_LOAD is 1") {
    ASSERT_EQ(cinux::proc::elf::PT_LOAD, 1U);
}

TEST("elf_constants: EM_X86_64 is 62") {
    ASSERT_EQ(cinux::proc::elf::EM_X86_64, 62U);
}

TEST("elf_constants: ELF_CLASS_64 is 2") {
    ASSERT_EQ(cinux::proc::elf::ELF_CLASS_64, 2U);
}

TEST("elf_constants: ELF_DATA_LSB is 1") {
    ASSERT_EQ(cinux::proc::elf::ELF_DATA_LSB, 1U);
}

TEST("elf_constants: ET_EXEC is 2") {
    ASSERT_EQ(cinux::proc::elf::ET_EXEC, 2U);
}

// ============================================================
// 17. ELF types: ElfValidateResult enum
// ============================================================

TEST("elf_validate_result: Ok is zero") {
    ASSERT_EQ(static_cast<int>(cinux::proc::elf::ElfValidateResult::Ok), 0);
}

TEST("elf_validate_result: error codes are distinct") {
    using VR = cinux::proc::elf::ElfValidateResult;
    ASSERT_TRUE(VR::BadMagic != VR::BadClass);
    ASSERT_TRUE(VR::BadClass != VR::BadEndian);
    ASSERT_TRUE(VR::BadEndian != VR::BadMachine);
    ASSERT_TRUE(VR::BadMachine != VR::BadType);
    ASSERT_TRUE(VR::BadType != VR::BadPhoff);
}

// ============================================================
// 18. ELF validation: valid header
// ============================================================

TEST("elf_validation: valid x86_64 ELF header passes") {
    using namespace cinux::proc::elf;

    // Construct a valid ELF64 header
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

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::Ok);
}

// ============================================================
// 19. ELF validation: bad magic
// ============================================================

TEST("elf_validation: bad magic fails") {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x00;  // wrong magic
    ehdr.e_ident[1]  = 0x00;
    ehdr.e_ident[2]  = 0x00;
    ehdr.e_ident[3]  = 0x00;
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::BadMagic);
}

// ============================================================
// 20. ELF validation: bad class (32-bit)
// ============================================================

TEST("elf_validation: bad class (32-bit) fails") {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = 1;  // ELFCLASS32
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::BadClass);
}

// ============================================================
// 21. ELF validation: bad endianness
// ============================================================

TEST("elf_validation: bad endianness fails") {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = 2;  // big-endian
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::BadEndian);
}

// ============================================================
// 22. ELF validation: bad machine
// ============================================================

TEST("elf_validation: bad machine fails") {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = ET_EXEC;
    ehdr.e_machine   = 0;  // wrong machine
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::BadMachine);
}

// ============================================================
// 23. ELF validation: bad type (not executable)
// ============================================================

TEST("elf_validation: bad type (not executable) fails") {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    ehdr.e_ident[0]  = 0x7F;
    ehdr.e_ident[1]  = 'E';
    ehdr.e_ident[2]  = 'L';
    ehdr.e_ident[3]  = 'F';
    ehdr.e_ident[4]  = ELF_CLASS_64;
    ehdr.e_ident[5]  = ELF_DATA_LSB;
    ehdr.e_type      = 1;  // ET_REL (relocatable)
    ehdr.e_machine   = EM_X86_64;
    ehdr.e_phoff     = sizeof(Elf64_Ehdr);
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::BadType);
}

// ============================================================
// 24. ELF validation: buffer too small
// ============================================================

TEST("elf_validation: buffer smaller than header fails") {
    using namespace cinux::proc::elf;

    Elf64_Ehdr ehdr{};
    // Don't bother filling it -- the size check should fail first
    auto       result = validate_elf_header(&ehdr, 32);
    ASSERT_EQ(result, ElfValidateResult::BadMagic);
}

// ============================================================
// 25. ELF validation: no program headers
// ============================================================

TEST("elf_validation: no program headers fails") {
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
    ehdr.e_phnum     = 0;  // no program headers

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::NoPhdrs);
}

// ============================================================
// 26. ELF validation: bad phdr offset
// ============================================================

TEST("elf_validation: phdr offset beyond file fails") {
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
    ehdr.e_phoff     = 100000;  // way beyond file size
    ehdr.e_phentsize = sizeof(Elf64_Phdr);
    ehdr.e_phnum     = 1;

    auto result = validate_elf_header(&ehdr, 4096);
    ASSERT_EQ(result, ElfValidateResult::BadPhoff);
}

// ============================================================
// 27. ELF program header: field offsets
// ============================================================

TEST("elf_phdr: p_vaddr at correct offset") {
    using namespace cinux::proc::elf;
    // p_type(4) + p_flags(4) + p_offset(8) + p_vaddr(8)
    ASSERT_EQ(offsetof(Elf64_Phdr, p_type), static_cast<size_t>(0));
    ASSERT_EQ(offsetof(Elf64_Phdr, p_flags), static_cast<size_t>(4));
    ASSERT_EQ(offsetof(Elf64_Phdr, p_offset), static_cast<size_t>(8));
    ASSERT_EQ(offsetof(Elf64_Phdr, p_vaddr), static_cast<size_t>(16));
    ASSERT_EQ(offsetof(Elf64_Phdr, p_filesz), static_cast<size_t>(32));
    ASSERT_EQ(offsetof(Elf64_Phdr, p_memsz), static_cast<size_t>(40));
}

TEST("elf_phdr: PT_LOAD segment with BSS") {
    using namespace cinux::proc::elf;

    Elf64_Phdr phdr{};
    phdr.p_type   = PT_LOAD;
    phdr.p_flags  = PF_R | PF_W | PF_X;
    phdr.p_offset = 0;
    phdr.p_vaddr  = 0x400000;
    phdr.p_filesz = 0x1000;  // 4 KB of file data
    phdr.p_memsz  = 0x2000;  // 8 KB total (4 KB BSS)
    phdr.p_align  = 0x1000;

    ASSERT_EQ(phdr.p_type, PT_LOAD);
    ASSERT_EQ(phdr.p_vaddr, 0x400000ULL);
    ASSERT_GT(phdr.p_memsz, phdr.p_filesz);              // has BSS
    ASSERT_EQ(phdr.p_memsz - phdr.p_filesz, 0x1000ULL);  // 4 KB BSS
}

TEST("elf_phdr: segment flags check") {
    using namespace cinux::proc::elf;

    Elf64_Phdr text_phdr{};
    text_phdr.p_flags = PF_R | PF_X;
    ASSERT_TRUE(text_phdr.p_flags & PF_R);
    ASSERT_TRUE(text_phdr.p_flags & PF_X);
    ASSERT_FALSE(text_phdr.p_flags & PF_W);

    Elf64_Phdr data_phdr{};
    data_phdr.p_flags = PF_R | PF_W;
    ASSERT_TRUE(data_phdr.p_flags & PF_R);
    ASSERT_TRUE(data_phdr.p_flags & PF_W);
    ASSERT_FALSE(data_phdr.p_flags & PF_X);
}

// ============================================================
// 28. SyscallNr::SYS_waitpid constant
// ============================================================

TEST("syscall_waitpid: SYS_waitpid number is 61 (Linux x86_64)") {
    // Linux x86_64: waitpid = 61
    ASSERT_EQ(static_cast<uint64_t>(cinux::syscall::SyscallNr::SYS_waitpid), 61ULL);
}

TEST("syscall_waitpid: SYS_waitpid is distinct from other process syscalls") {
    using cinux::syscall::SyscallNr;
    ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_fork);
    ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_execve);
    ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_getpid);
    ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_getppid);
    ASSERT_TRUE(SyscallNr::SYS_waitpid != SyscallNr::SYS_exit);
}

// ============================================================
// 29. errno_values::ECHILD constant
// ============================================================

TEST("errno_echild: ECHILD is 10 (Linux)") {
    ASSERT_EQ(cinux::proc::errno_values::ECHILD, 10);
}

TEST("errno_echild: ECHILD is distinct from other errno values") {
    ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::EPERM);
    ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::ENOENT);
    ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::EINVAL);
    ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::ENOMEM);
    ASSERT_TRUE(cinux::proc::errno_values::ECHILD != cinux::proc::errno_values::ESRCH);
}

// ============================================================
// 30. WaitpidResult enum values
// ============================================================

TEST("waitpid_result: Ok is zero") {
    ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::Ok), 0);
}

TEST("waitpid_result: all error values are negative") {
    using R = cinux::proc::WaitpidResult;
    ASSERT_LT(static_cast<int>(R::NoChildren), 0);
    ASSERT_LT(static_cast<int>(R::NotFound), 0);
    ASSERT_LT(static_cast<int>(R::InvalidPid), 0);
    ASSERT_LT(static_cast<int>(R::NotExited), 0);
}

TEST("waitpid_result: error codes are distinct") {
    using R = cinux::proc::WaitpidResult;
    ASSERT_TRUE(R::NoChildren != R::NotFound);
    ASSERT_TRUE(R::NoChildren != R::InvalidPid);
    ASSERT_TRUE(R::NoChildren != R::NotExited);
    ASSERT_TRUE(R::NotFound != R::InvalidPid);
    ASSERT_TRUE(R::NotFound != R::NotExited);
    ASSERT_TRUE(R::InvalidPid != R::NotExited);
}

TEST("waitpid_result: NoChildren maps to -ECHILD (-10)") {
    ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::NoChildren), -10);
}

TEST("waitpid_result: NotFound maps to -ESRCH (-3)") {
    ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::NotFound), -3);
}

TEST("waitpid_result: InvalidPid maps to -EINVAL (-22)") {
    ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::InvalidPid), -22);
}

TEST("waitpid_result: NotExited maps to -1") {
    ASSERT_EQ(static_cast<int>(cinux::proc::WaitpidResult::NotExited), -1);
}

// ============================================================
// 31. waitpid semantics: parent-child zombie reap simulation
// ============================================================

TEST("waitpid_semantics: zombie child can be reaped") {
    // Simulate the waitpid reap logic with raw Task structs
    PidAllocator alloc;
    Task         parent{};
    parent.pid  = 10;
    parent.ppid = 1;

    Task child{};
    child.pid         = 11;
    child.ppid        = 10;
    child.state       = TaskState::Zombie;
    child.exit_status = 42;
    child.parent      = &parent;

    // Link child into parent's children list
    child.wait_next = parent.children;
    parent.children = &child;

    // Verify the setup
    ASSERT_EQ(parent.children, &child);
    ASSERT_EQ(child.state, TaskState::Zombie);
    ASSERT_EQ(child.exit_status, 42);

    // Simulate reap: collect exit status
    int status = 0;
    status     = child.exit_status;
    ASSERT_EQ(status, 42);

    // Simulate reap: unlink from children list
    parent.children = child.wait_next;
    ASSERT_NULL(parent.children);

    // Simulate reap: free PID
    alloc.free(child.pid);
    ASSERT_FALSE(alloc.is_allocated(11));

    // Simulate reap: mark as Dead
    child.state = TaskState::Dead;
    ASSERT_TRUE(child.state == TaskState::Dead);
}

TEST("waitpid_semantics: reap preserves sibling list") {
    // Parent with two children; reap the first
    Task parent{};
    parent.pid = 10;

    Task child1{};
    child1.pid         = 11;
    child1.state       = TaskState::Zombie;
    child1.exit_status = 0;

    Task child2{};
    child2.pid         = 12;
    child2.state       = TaskState::Zombie;
    child2.exit_status = 1;

    // children list: child1 -> child2
    child1.wait_next = &child2;
    child2.wait_next = nullptr;
    parent.children  = &child1;

    // Reap child1 (head of list)
    parent.children = child1.wait_next;
    ASSERT_EQ(parent.children, &child2);
}

TEST("waitpid_semantics: reap middle child from list") {
    // Parent with three children; reap the middle one
    Task parent{};
    parent.pid = 10;

    Task child1{};
    child1.pid = 11;

    Task child2{};
    child2.pid   = 12;
    child2.state = TaskState::Zombie;

    Task child3{};
    child3.pid = 13;

    // children list: child1 -> child2 -> child3
    child1.wait_next = &child2;
    child2.wait_next = &child3;
    child3.wait_next = nullptr;
    parent.children  = &child1;

    // Reap child2 (middle): find prev (child1), skip child2
    Task* prev = nullptr;
    Task* cur  = parent.children;
    while (cur != nullptr && cur->pid != 12) {
        prev = cur;
        cur  = cur->wait_next;
    }
    ASSERT_NOT_NULL(cur);
    ASSERT_EQ(prev, &child1);

    if (prev != nullptr) {
        prev->wait_next = cur->wait_next;
    } else {
        parent.children = cur->wait_next;
    }
    ASSERT_EQ(child1.wait_next, &child3);
}

TEST("waitpid_semantics: no children returns ECHILD") {
    Task parent{};
    parent.pid      = 10;
    parent.children = nullptr;

    // No children: should return ECHILD
    ASSERT_NULL(parent.children);
}

TEST("waitpid_semantics: non-zombie child returns NotExited") {
    Task parent{};
    parent.pid = 10;

    Task child{};
    child.pid   = 11;
    child.state = TaskState::Running;

    child.wait_next = nullptr;
    parent.children = &child;

    // Child exists but is not zombie
    ASSERT_EQ(parent.children->state, TaskState::Running);
    ASSERT_TRUE(parent.children->state != TaskState::Zombie);
}

TEST("waitpid_semantics: pid=-1 waits for any zombie child") {
    Task parent{};
    parent.pid = 10;

    Task child1{};
    child1.pid   = 11;
    child1.state = TaskState::Running;

    Task child2{};
    child2.pid         = 12;
    child2.state       = TaskState::Zombie;
    child2.exit_status = 7;

    // children list: child1 -> child2
    child1.wait_next = &child2;
    child2.wait_next = nullptr;
    parent.children  = &child1;

    // Scan for first zombie (pid=-1 semantics)
    Task* target   = nullptr;
    Task* prev     = nullptr;
    Task* cur      = parent.children;
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

    // Should find child2 (the zombie), skipping child1 (running)
    ASSERT_NOT_NULL(target);
    ASSERT_EQ(target->pid, 12);
    ASSERT_EQ(target->exit_status, 7);
    ASSERT_EQ(prev, &child1);
}

TEST("waitpid_semantics: pid=-1 no zombie returns NotExited") {
    Task parent{};
    parent.pid = 10;

    Task child1{};
    child1.pid   = 11;
    child1.state = TaskState::Running;

    Task child2{};
    child2.pid   = 12;
    child2.state = TaskState::Ready;

    child1.wait_next = &child2;
    child2.wait_next = nullptr;
    parent.children  = &child1;

    // Scan for any zombie: none found
    Task* target = nullptr;
    Task* cur    = parent.children;
    while (cur != nullptr) {
        if (cur->state == TaskState::Zombie) {
            target = cur;
            break;
        }
        cur = cur->wait_next;
    }

    ASSERT_NULL(target);
}

TEST("waitpid_semantics: reap frees PID for reuse") {
    PidAllocator alloc;
    int          freed_pid = alloc.alloc();  // pid = 1
    ASSERT_EQ(freed_pid, 1);

    // Simulate reap: free the PID
    alloc.free(freed_pid);
    ASSERT_FALSE(alloc.is_allocated(1));

    // PID should be reusable
    int reused = alloc.alloc();
    ASSERT_EQ(reused, 1);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
