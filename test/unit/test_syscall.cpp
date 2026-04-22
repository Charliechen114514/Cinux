/**
 * @file test/unit/test_syscall.cpp
 * @brief Host-side unit tests for syscall infrastructure (023)
 *
 * Test coverage:
 *   - SyscallNr enum values: SYS_read=0, SYS_write=1, SYS_exit=60, SYS_yield=24
 *   - SYSCALL_TABLE_SIZE constant (256)
 *   - Dispatch table: register, dispatch, null handler returns -1
 *   - Out-of-range syscall number returns -1
 *   - sys_write parameter validation: fd check, buf_virt USER_ADDR_MAX check
 *   - sys_exit task state transition: marks task as Dead
 *   - STAR MSR value computation (kernel CS selectors)
 *   - SFMASK value computation (IF bit clear)
 *   - SyscallFn type signature
 *
 * Pure arithmetic and logic -- no kernel code linked. Hardware dependencies
 * (MSR writes, kprintf, Scheduler) are isolated via mock reimplementation.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstddef>
#include <cstdint>
#include <cstring>

// ============================================================
// Mock / reimplementation layer
// ============================================================

// We mirror the essential types and logic from the kernel so that
// host-side tests can exercise the same arithmetic without linking
// any kernel code.

namespace mock {

constexpr uint64_t USER_ADDR_MAX = 0x800000000000ULL;

// Mirror of SyscallNr from kernel/syscall/syscall_nums.hpp
enum class SyscallNr : uint64_t {
    SYS_read  = 0,
    SYS_write = 1,
    SYS_exit  = 60,
    SYS_yield = 24,
};

constexpr uint64_t SYSCALL_TABLE_SIZE = 256;

using SyscallFn = int64_t(*)(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);

// Mirror of TaskState from kernel/proc/process.hpp
enum class TaskState : uint8_t {
    Running,
    Ready,
    Blocked,
    Dead
};

// Minimal Task mirror (only the fields sys_exit cares about)
struct Task {
    TaskState state;
    uint64_t tid;
    const char* name;
};

// Dispatch table (same layout as kernel)
SyscallFn syscall_table[SYSCALL_TABLE_SIZE] = {};

void reset_table() {
    memset(syscall_table, 0, sizeof(syscall_table));
}

// Mirror of syscall_register
void syscall_register(SyscallNr nr, SyscallFn handler) {
    uint64_t idx = static_cast<uint64_t>(nr);
    if (idx >= SYSCALL_TABLE_SIZE) {
        return;
    }
    syscall_table[idx] = handler;
}

// Mirror of syscall_dispatch
int64_t syscall_dispatch(uint64_t nr,
                         uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6) {
    if (nr >= SYSCALL_TABLE_SIZE) {
        return -1;
    }
    auto fn = syscall_table[nr];
    if (fn == nullptr) {
        return -1;
    }
    return fn(a1, a2, a3, a4, a5, a6);
}

// Mirror of sys_write logic (without kprintf output)
int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count,
                  uint64_t, uint64_t, uint64_t) {
    if (buf_virt >= USER_ADDR_MAX) {
        return -1;
    }
    if (fd != 1) {
        return -1;
    }
    return static_cast<int64_t>(count);
}

// Mirror of sys_exit logic (without Scheduler::yield)
int64_t sys_exit(Task* task, uint64_t code, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    if (task != nullptr) {
        task->state = TaskState::Dead;
    }
    (void)code;
    return 0;
}

// Mirror of sys_yield (returns 0 on success)
int64_t sys_yield(uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    return 0;
}

// Capture buffer for mock kprintf in sys_write integration
char kprintf_capture[4096];
int kprintf_capture_len = 0;

void reset_capture() {
    memset(kprintf_capture, 0, sizeof(kprintf_capture));
    kprintf_capture_len = 0;
}

}  // namespace mock

// ============================================================
// 1. Syscall Number Constants
// ============================================================

TEST("syscall: SYS_read equals 0") {
    ASSERT_EQ(static_cast<uint64_t>(mock::SyscallNr::SYS_read), 0ULL);
}

TEST("syscall: SYS_write equals 1") {
    ASSERT_EQ(static_cast<uint64_t>(mock::SyscallNr::SYS_write), 1ULL);
}

TEST("syscall: SYS_exit equals 60") {
    ASSERT_EQ(static_cast<uint64_t>(mock::SyscallNr::SYS_exit), 60ULL);
}

TEST("syscall: SYS_yield equals 24") {
    ASSERT_EQ(static_cast<uint64_t>(mock::SyscallNr::SYS_yield), 24ULL);
}

TEST("syscall: SYSCALL_TABLE_SIZE is 256") {
    ASSERT_EQ(mock::SYSCALL_TABLE_SIZE, 256ULL);
}

// ============================================================
// 2. Dispatch Table: Registration
// ============================================================

TEST("syscall: register handler stores in table") {
    mock::reset_table();

    auto handler = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 42;
    };

    mock::syscall_register(mock::SyscallNr::SYS_write, handler);
    ASSERT_TRUE(mock::syscall_table[static_cast<uint64_t>(mock::SyscallNr::SYS_write)] != nullptr);
}

TEST("syscall: register multiple handlers in different slots") {
    mock::reset_table();

    auto handler_read = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 1;
    };
    auto handler_write = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 2;
    };
    auto handler_exit = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 3;
    };

    mock::syscall_register(mock::SyscallNr::SYS_read, handler_read);
    mock::syscall_register(mock::SyscallNr::SYS_write, handler_write);
    mock::syscall_register(mock::SyscallNr::SYS_exit, handler_exit);

    ASSERT_TRUE(mock::syscall_table[0] != nullptr);
    ASSERT_TRUE(mock::syscall_table[1] != nullptr);
    ASSERT_TRUE(mock::syscall_table[60] != nullptr);
}

TEST("syscall: register overwrites previous handler") {
    mock::reset_table();

    auto handler_a = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 10;
    };
    auto handler_b = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 20;
    };

    mock::syscall_register(mock::SyscallNr::SYS_write, handler_a);
    ASSERT_EQ(mock::syscall_dispatch(1, 0, 0, 0, 0, 0, 0), 10);

    mock::syscall_register(mock::SyscallNr::SYS_write, handler_b);
    ASSERT_EQ(mock::syscall_dispatch(1, 0, 0, 0, 0, 0, 0), 20);
}

// ============================================================
// 3. Dispatch Table: Dispatch Logic
// ============================================================

TEST("syscall: dispatch calls registered handler with correct args") {
    mock::reset_table();

    auto handler = [](uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6) -> int64_t {
        return static_cast<int64_t>(a1 + a2 + a3 + a4 + a5 + a6);
    };

    mock::syscall_register(mock::SyscallNr::SYS_write, handler);
    int64_t result = mock::syscall_dispatch(1, 10, 20, 30, 40, 50, 60);
    ASSERT_EQ(result, 210);
}

TEST("syscall: dispatch unregistered syscall returns -1") {
    mock::reset_table();

    int64_t result = mock::syscall_dispatch(static_cast<uint64_t>(mock::SyscallNr::SYS_write),
                                            0, 0, 0, 0, 0, 0);
    ASSERT_EQ(result, -1);
}

TEST("syscall: dispatch out-of-range number returns -1") {
    mock::reset_table();

    ASSERT_EQ(mock::syscall_dispatch(256, 0, 0, 0, 0, 0, 0), -1);
    ASSERT_EQ(mock::syscall_dispatch(512, 0, 0, 0, 0, 0, 0), -1);
}

TEST("syscall: dispatch max valid number (255)") {
    mock::reset_table();

    auto handler = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 99;
    };

    mock::syscall_table[255] = handler;
    ASSERT_EQ(mock::syscall_dispatch(255, 0, 0, 0, 0, 0, 0), 99);
}

TEST("syscall: dispatch nr=0 works") {
    mock::reset_table();

    auto handler = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 7;
    };

    mock::syscall_register(mock::SyscallNr::SYS_read, handler);
    ASSERT_EQ(mock::syscall_dispatch(0, 0, 0, 0, 0, 0, 0), 7);
}

// ============================================================
// 4. sys_write Parameter Validation
// ============================================================

TEST("syscall: sys_write with fd=1 and valid buf returns count") {
    int64_t result = mock::sys_write(1, 0x1000, 5, 0, 0, 0);
    ASSERT_EQ(result, 5);
}

TEST("syscall: sys_write with fd!=1 returns -1") {
    ASSERT_EQ(mock::sys_write(0, 0x1000, 5, 0, 0, 0), -1);
    ASSERT_EQ(mock::sys_write(2, 0x1000, 5, 0, 0, 0), -1);
    ASSERT_EQ(mock::sys_write(3, 0x1000, 5, 0, 0, 0), -1);
    ASSERT_EQ(mock::sys_write(100, 0x1000, 5, 0, 0, 0), -1);
}

TEST("syscall: sys_write with buf_virt >= USER_ADDR_MAX returns -1") {
    ASSERT_EQ(mock::sys_write(1, 0x800000000000ULL, 5, 0, 0, 0), -1);
    ASSERT_EQ(mock::sys_write(1, 0xFFFFFFFF80000000ULL, 5, 0, 0, 0), -1);
    ASSERT_EQ(mock::sys_write(1, 0xFFFFFFFFFFFFFFFFULL, 5, 0, 0, 0), -1);
}

TEST("syscall: sys_write with buf_virt just below USER_ADDR_MAX succeeds") {
    int64_t result = mock::sys_write(1, 0x7FFFFFFFFFFFULL, 1, 0, 0, 0);
    ASSERT_EQ(result, 1);
}

TEST("syscall: sys_write with count=0 returns 0") {
    int64_t result = mock::sys_write(1, 0x1000, 0, 0, 0, 0);
    ASSERT_EQ(result, 0);
}

TEST("syscall: sys_write returns count as int64_t") {
    uint64_t large_count = 0x7FFFFFFFFFFFFFFFULL;
    int64_t result = mock::sys_write(1, 0x1000, large_count, 0, 0, 0);
    ASSERT_EQ(result, static_cast<int64_t>(large_count));
}

// ============================================================
// 5. sys_exit Task State Transition
// ============================================================

TEST("syscall: sys_exit marks task as Dead") {
    mock::Task task;
    task.state = mock::TaskState::Running;
    task.tid = 1;
    task.name = "test_task";

    mock::sys_exit(&task, 0, 0, 0, 0, 0, 0);
    ASSERT_TRUE(task.state == mock::TaskState::Dead);
}

TEST("syscall: sys_exit with non-zero exit code still marks Dead") {
    mock::Task task;
    task.state = mock::TaskState::Ready;
    task.tid = 2;
    task.name = "test_task2";

    mock::sys_exit(&task, 1, 0, 0, 0, 0, 0);
    ASSERT_TRUE(task.state == mock::TaskState::Dead);
}

TEST("syscall: sys_exit with null task does not crash") {
    mock::sys_exit(nullptr, 0, 0, 0, 0, 0, 0);
    ASSERT_TRUE(true);
}

TEST("syscall: sys_exit transitions from Blocked to Dead") {
    mock::Task task;
    task.state = mock::TaskState::Blocked;
    task.tid = 3;
    task.name = "blocked_task";

    mock::sys_exit(&task, 0, 0, 0, 0, 0, 0);
    ASSERT_TRUE(task.state == mock::TaskState::Dead);
}

// ============================================================
// 6. sys_yield Return Value
// ============================================================

TEST("syscall: sys_yield returns 0") {
    ASSERT_EQ(mock::sys_yield(0, 0, 0, 0, 0, 0), 0);
}

// ============================================================
// 7. STAR MSR Value Computation
// ============================================================

TEST("syscall: STAR MSR index constants") {
    constexpr uint32_t MSR_STAR   = 0xC0000081;
    constexpr uint32_t MSR_LSTAR  = 0xC0000082;
    constexpr uint32_t MSR_SFMASK = 0xC0000084;
    ASSERT_EQ(MSR_STAR, 0xC0000081u);
    ASSERT_EQ(MSR_LSTAR, 0xC0000082u);
    ASSERT_EQ(MSR_SFMASK, 0xC0000084u);
}

TEST("syscall: STAR value with kernel CS 0x08") {
    // From syscall.cpp: star_val = (GDT_KERNEL_CODE << 32) | (GDT_KERNEL_CODE << 48)
    constexpr uint16_t GDT_KERNEL_CODE = 0x08;
    uint64_t star_val = (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32)
                      | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 48);
    ASSERT_EQ(star_val, 0x0008000800000000ULL);
}

TEST("syscall: STAR high 32 bits encode SYSRET and SYSCALL CS") {
    constexpr uint16_t GDT_KERNEL_CODE = 0x08;
    uint64_t star_val = (static_cast<uint64_t>(GDT_KERNEL_CODE) << 32)
                      | (static_cast<uint64_t>(GDT_KERNEL_CODE) << 48);

    // STAR[47:32] = SYSCALL CS base = 0x08
    uint16_t syscall_cs = static_cast<uint16_t>((star_val >> 32) & 0xFFFF);
    ASSERT_EQ(syscall_cs, 0x08);

    // STAR[63:48] = SYSRET CS base = 0x08
    uint16_t sysret_cs = static_cast<uint16_t>(star_val >> 48);
    ASSERT_EQ(sysret_cs, 0x08);
}

TEST("syscall: SFMASK value 0x200 clears IF on SYSCALL entry") {
    constexpr uint64_t sfmask_val = 0x200;
    // Bit 9 = IF (Interrupt Flag)
    ASSERT_TRUE(sfmask_val & (1ULL << 9));
}

// ============================================================
// 8. SyscallFn Type Signature
// ============================================================

TEST("syscall: SyscallFn is 6-arg function pointer returning int64_t") {
    mock::SyscallFn fn = [](uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) -> int64_t {
        return static_cast<int64_t>(a1 + a2 + a3 + a4 + a5 + a6);
    };
    ASSERT_TRUE(fn != nullptr);
    ASSERT_EQ(fn(1, 2, 3, 4, 5, 6), 21);
}

// ============================================================
// 9. Dispatch Table Full Lifecycle
// ============================================================

TEST("syscall: full register-dispatch lifecycle for SYS_write") {
    mock::reset_table();

    char buf[] = "hello";
    auto handler = [](uint64_t fd, uint64_t buf_virt, uint64_t count,
                      uint64_t, uint64_t, uint64_t) -> int64_t {
        if (fd != 1) return -1;
        if (buf_virt >= 0x800000000000ULL) return -1;
        return static_cast<int64_t>(count);
    };

    mock::syscall_register(mock::SyscallNr::SYS_write, handler);

    int64_t r1 = mock::syscall_dispatch(1, 1, reinterpret_cast<uint64_t>(buf), 5, 0, 0, 0);
    ASSERT_EQ(r1, 5);

    int64_t r2 = mock::syscall_dispatch(1, 1, 0, 0, 0, 0, 0);
    ASSERT_EQ(r2, 0);

    int64_t r3 = mock::syscall_dispatch(1, 1, reinterpret_cast<uint64_t>(buf), 3, 0, 0, 0);
    ASSERT_EQ(r3, 3);

    int64_t r4 = mock::syscall_dispatch(1, 2, reinterpret_cast<uint64_t>(buf), 3, 0, 0, 0);
    ASSERT_EQ(r4, -1);
}

TEST("syscall: full register-dispatch lifecycle for SYS_exit") {
    mock::reset_table();

    static mock::Task exit_lifecycle_task;
    exit_lifecycle_task.state = mock::TaskState::Running;
    exit_lifecycle_task.tid = 42;
    exit_lifecycle_task.name = "exit_test";

    auto handler = [](uint64_t, uint64_t, uint64_t,
                      uint64_t, uint64_t, uint64_t) -> int64_t {
        exit_lifecycle_task.state = mock::TaskState::Dead;
        return 0;
    };

    mock::syscall_register(mock::SyscallNr::SYS_exit, handler);

    int64_t result = mock::syscall_dispatch(60, 0, 0, 0, 0, 0, 0);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(exit_lifecycle_task.state == mock::TaskState::Dead);
}

TEST("syscall: full register-dispatch lifecycle for SYS_yield") {
    mock::reset_table();

    auto handler = [](uint64_t, uint64_t, uint64_t,
                      uint64_t, uint64_t, uint64_t) -> int64_t {
        return 0;
    };

    mock::syscall_register(mock::SyscallNr::SYS_yield, handler);

    int64_t result = mock::syscall_dispatch(24, 0, 0, 0, 0, 0, 0);
    ASSERT_EQ(result, 0);
}

// ============================================================
// 10. Boundary Conditions for Dispatch Table
// ============================================================

TEST("syscall: reset clears all handlers") {
    mock::reset_table();

    auto handler = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 1;
    };

    mock::syscall_register(mock::SyscallNr::SYS_read, handler);
    ASSERT_EQ(mock::syscall_dispatch(0, 0, 0, 0, 0, 0, 0), 1);

    mock::reset_table();
    ASSERT_EQ(mock::syscall_dispatch(0, 0, 0, 0, 0, 0, 0), -1);
}

TEST("syscall: dispatch table slots are independent") {
    mock::reset_table();

    auto handler_a = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 100;
    };
    auto handler_b = [](uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) -> int64_t {
        return 200;
    };

    mock::syscall_register(mock::SyscallNr::SYS_read, handler_a);
    mock::syscall_register(mock::SyscallNr::SYS_write, handler_b);

    ASSERT_EQ(mock::syscall_dispatch(0, 0, 0, 0, 0, 0, 0), 100);
    ASSERT_EQ(mock::syscall_dispatch(1, 0, 0, 0, 0, 0, 0), 200);
    ASSERT_EQ(mock::syscall_dispatch(2, 0, 0, 0, 0, 0, 0), -1);
}

// ============================================================
// Main function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
