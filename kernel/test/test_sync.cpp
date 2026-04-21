/**
 * @file kernel/test/test_sync.cpp
 * @brief QEMU in-kernel integration tests for synchronization primitives (021)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * real Spinlock, Mutex, and Semaphore with actual atomic operations and
 * Scheduler block/unblock.  Tests:
 *   - Spinlock: acquire/release, RAII guard
 *   - Mutex: lock/unlock, try_lock, FIFO wait queue, RAII guard,
 *            owner transfer, contention with real block/unblock
 *   - Semaphore: post/wait/try_wait, count tracking, FIFO wait queue,
 *                negative-count blocking
 *   - Intrusive wait queue: enqueue/dequeue, FIFO ordering
 *   - Task wait_next field
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised
 *   - VMM initialised
 *   - Heap initialised
 *   - AddressSpace kernel PML4 initialised
 *   - Scheduler initialised
 */

#include <stdint.h>
#include <stddef.h>

#include "big_kernel_test.h"

#include "kernel/proc/sync.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/per_cpu.hpp"

using cinux::proc::Spinlock;
using cinux::proc::Mutex;
using cinux::proc::Semaphore;
using cinux::proc::Task;
using cinux::proc::TaskBuilder;
using cinux::proc::TaskState;
using cinux::proc::Scheduler;
using cinux::proc::g_per_cpu;

// ============================================================
// Test 1: Spinlock acquire and release
// ============================================================

namespace test_spinlock {

void test_acquire_release() {
    Spinlock s;
    s.acquire();
    // If we got here, the spinlock was acquired successfully
    s.release();
    TEST_ASSERT_TRUE(true);  // reached without deadlock
}

void test_guard_raii() {
    Spinlock s;
    {
        auto g = s.guard();
        (void)g;
        // Lock should be held inside the scope
    }
    // Lock should be released after scope exit
    // Verify by acquiring again -- should not deadlock
    s.acquire();
    s.release();
    TEST_ASSERT_TRUE(true);
}

}  // namespace test_spinlock

// ============================================================
// Test 2: Mutex basic lock/unlock
// ============================================================

namespace test_mutex_basic {

void dummy_entry() {
    // Never actually runs; just a non-null entry point
}

void test_lock_unlock() {
    Scheduler::init();

    // Create a task and set it as current
    Task* task = TaskBuilder()
        .set_entry(dummy_entry)
        .set_name("mutex_owner")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    g_per_cpu.current = task;

    Mutex m;
    m.lock();
    // Lock succeeded without deadlock

    m.unlock();
    TEST_ASSERT_TRUE(true);  // unlock succeeded without crash
}

void test_try_lock_free() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(dummy_entry)
        .set_name("trylock_free")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    g_per_cpu.current = task;

    Mutex m;
    bool result = m.try_lock();
    TEST_ASSERT_TRUE(result);
}

void test_try_lock_held() {
    Scheduler::init();

    Task* owner = TaskBuilder()
        .set_entry(dummy_entry)
        .set_name("trylock_owner")
        .build();
    TEST_ASSERT_NOT_NULL(owner);

    g_per_cpu.current = owner;

    Mutex m;
    m.lock();  // owner holds the mutex

    // Now simulate another task trying
    Task* other = TaskBuilder()
        .set_entry(dummy_entry)
        .set_name("trylock_other")
        .build();
    TEST_ASSERT_NOT_NULL(other);

    g_per_cpu.current = other;
    bool result = m.try_lock();
    TEST_ASSERT_FALSE(result);  // should fail -- mutex is held
}

}  // namespace test_mutex_basic

// ============================================================
// Test 3: Mutex contention and FIFO wait queue
// ============================================================

namespace test_mutex_contention {

void test_lock_blocks_and_enqueues() {
    Scheduler::init();

    Task* owner = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("mx_owner")
        .build();
    TEST_ASSERT_NOT_NULL(owner);

    Task* waiter = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("mx_waiter")
        .build();
    TEST_ASSERT_NOT_NULL(waiter);

    // Owner locks the mutex
    g_per_cpu.current = owner;
    Mutex m;
    m.lock();

    // Waiter tries to lock -- should be blocked
    g_per_cpu.current = waiter;
    m.lock();  // this calls Scheduler::block(waiter, "mutex")

    TEST_ASSERT_EQ(static_cast<int>(waiter->state),
                   static_cast<int>(TaskState::Blocked));
}

void test_unlock_transfers_to_waiter() {
    Scheduler::init();

    Task* owner = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("mx_owner2")
        .build();
    TEST_ASSERT_NOT_NULL(owner);

    Task* waiter = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("mx_waiter2")
        .build();
    TEST_ASSERT_NOT_NULL(waiter);

    // Owner locks
    g_per_cpu.current = owner;
    Mutex m;
    m.lock();

    // Waiter blocks
    g_per_cpu.current = waiter;
    m.lock();
    TEST_ASSERT_EQ(static_cast<int>(waiter->state),
                   static_cast<int>(TaskState::Blocked));

    // Owner unlocks -- transfers to waiter
    g_per_cpu.current = owner;
    m.unlock();

    // Waiter should now be unblocked (Ready)
    TEST_ASSERT_EQ(static_cast<int>(waiter->state),
                   static_cast<int>(TaskState::Ready));
}

void test_fifo_ordering() {
    Scheduler::init();

    Task* owner = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("fifo_owner")
        .build();
    TEST_ASSERT_NOT_NULL(owner);

    Task* w1 = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("fifo_w1")
        .build();
    Task* w2 = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("fifo_w2")
        .build();
    Task* w3 = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("fifo_w3")
        .build();
    TEST_ASSERT_NOT_NULL(w1);
    TEST_ASSERT_NOT_NULL(w2);
    TEST_ASSERT_NOT_NULL(w3);

    // Owner locks
    g_per_cpu.current = owner;
    Mutex m;
    m.lock();

    // Three waiters enqueue
    g_per_cpu.current = w1;
    m.lock();
    g_per_cpu.current = w2;
    m.lock();
    g_per_cpu.current = w3;
    m.lock();

    // First unlock: should wake w1
    m.unlock();
    TEST_ASSERT_EQ(static_cast<int>(w1->state),
                   static_cast<int>(TaskState::Ready));

    // Second unlock: should wake w2
    m.unlock();
    TEST_ASSERT_EQ(static_cast<int>(w2->state),
                   static_cast<int>(TaskState::Ready));

    // Third unlock: should wake w3
    m.unlock();
    TEST_ASSERT_EQ(static_cast<int>(w3->state),
                   static_cast<int>(TaskState::Ready));
}

}  // namespace test_mutex_contention

// ============================================================
// Test 4: Mutex RAII guard
// ============================================================

namespace test_mutex_guard {

void test_guard_scope() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("guard_task")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    g_per_cpu.current = task;

    Mutex m;
    {
        auto g = m.guard();
        (void)g;
        // Mutex is held here
    }
    // After scope exit, mutex should be released
    // Verify by acquiring again
    bool ok = m.try_lock();
    TEST_ASSERT_TRUE(ok);
    m.unlock();
}

}  // namespace test_mutex_guard

// ============================================================
// Test 5: Semaphore basic operations
// ============================================================

namespace test_semaphore_basic {

void test_initial_count() {
    Semaphore s(5);
    TEST_ASSERT_EQ(s.count(), 5);
}

void test_default_count_zero() {
    Semaphore s;
    TEST_ASSERT_EQ(s.count(), 0);
}

void test_post_increments() {
    Semaphore s(0);
    s.post();
    TEST_ASSERT_EQ(s.count(), 1);
}

void test_wait_decrements_when_positive() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("sem_waiter")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    g_per_cpu.current = task;

    Semaphore s(3);
    s.wait();
    TEST_ASSERT_EQ(s.count(), 2);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));
}

void test_wait_blocks_when_zero() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("sem_block")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    g_per_cpu.current = task;

    Semaphore s(0);
    s.wait();
    TEST_ASSERT_EQ(s.count(), -1);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Blocked));
}

}  // namespace test_semaphore_basic

// ============================================================
// Test 6: Semaphore try_wait
// ============================================================

namespace test_semaphore_try {

void test_try_wait_success() {
    Semaphore s(2);
    bool result = s.try_wait();
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQ(s.count(), 1);
}

void test_try_wait_fail_zero() {
    Semaphore s(0);
    bool result = s.try_wait();
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQ(s.count(), 0);
}

void test_try_wait_all() {
    Semaphore s(2);
    TEST_ASSERT_TRUE(s.try_wait());
    TEST_ASSERT_TRUE(s.try_wait());
    TEST_ASSERT_EQ(s.count(), 0);
    TEST_ASSERT_FALSE(s.try_wait());  // should fail now
}

}  // namespace test_semaphore_try

// ============================================================
// Test 7: Semaphore post wakes waiters
// ============================================================

namespace test_semaphore_wake {

void test_post_wakes_blocked_waiter() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("sem_wake")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    g_per_cpu.current = task;

    Semaphore s(0);
    s.wait();  // count -> -1, blocks
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Blocked));

    s.post();  // count -> 0, unblocks
    TEST_ASSERT_EQ(s.count(), 0);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));
}

void test_fifo_ordering() {
    Scheduler::init();

    Task* t1 = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("sem_f1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("sem_f2")
        .build();
    Task* t3 = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("sem_f3")
        .build();
    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_NOT_NULL(t3);

    Semaphore s(0);

    g_per_cpu.current = t1;
    s.wait();
    g_per_cpu.current = t2;
    s.wait();
    g_per_cpu.current = t3;
    s.wait();

    // All three should be blocked
    TEST_ASSERT_EQ(static_cast<int>(t1->state),
                   static_cast<int>(TaskState::Blocked));
    TEST_ASSERT_EQ(static_cast<int>(t2->state),
                   static_cast<int>(TaskState::Blocked));
    TEST_ASSERT_EQ(static_cast<int>(t3->state),
                   static_cast<int>(TaskState::Blocked));

    // First post wakes t1
    s.post();
    TEST_ASSERT_EQ(static_cast<int>(t1->state),
                   static_cast<int>(TaskState::Ready));
    TEST_ASSERT_EQ(static_cast<int>(t2->state),
                   static_cast<int>(TaskState::Blocked));

    // Second post wakes t2
    s.post();
    TEST_ASSERT_EQ(static_cast<int>(t2->state),
                   static_cast<int>(TaskState::Ready));

    // Third post wakes t3
    s.post();
    TEST_ASSERT_EQ(static_cast<int>(t3->state),
                   static_cast<int>(TaskState::Ready));
}

}  // namespace test_semaphore_wake

// ============================================================
// Test 8: Semaphore counting semaphore pattern
// ============================================================

namespace test_semaphore_counting {

void test_producer_consumer_no_blocking() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("pc_task")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    g_per_cpu.current = task;

    // Buffer of size 3
    Semaphore sem_free(3);
    Semaphore sem_used(0);

    // Producer: fill buffer
    sem_free.wait();  // 3->2
    sem_free.wait();  // 2->1
    sem_free.wait();  // 1->0
    TEST_ASSERT_EQ(sem_free.count(), 0);

    sem_used.post();  // 0->1
    sem_used.post();  // 1->2
    sem_used.post();  // 2->3
    TEST_ASSERT_EQ(sem_used.count(), 3);

    // Consumer: drain buffer
    sem_used.wait();  // 3->2
    sem_used.wait();  // 2->1
    sem_used.wait();  // 1->0
    TEST_ASSERT_EQ(sem_used.count(), 0);

    // No blocking should have occurred (task should still be Ready)
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));
}

}  // namespace test_semaphore_counting

// ============================================================
// Test 9: Task wait_next field
// ============================================================

namespace test_task_wait_next {

void test_wait_next_null_after_build() {
    // Build a task and verify wait_next is not in an invalid state
    // Note: TaskBuilder does not explicitly zero wait_next,
    // but the underlying heap allocation (knew) should zero it.
    Task* task = TaskBuilder()
        .set_entry(test_mutex_basic::dummy_entry)
        .set_name("waitnext_test")
        .build();
    TEST_ASSERT_NOT_NULL(task);
    // Verify wait_next is accessible and null
    TEST_ASSERT_NULL(task->wait_next);
}

}  // namespace test_task_wait_next

// ============================================================
// Entry point
// ============================================================

extern "C" void run_sync_tests() {
    TEST_SECTION("Sync Tests (021)");

    RUN_TEST(test_spinlock::test_acquire_release);
    RUN_TEST(test_spinlock::test_guard_raii);

    RUN_TEST(test_mutex_basic::test_lock_unlock);
    RUN_TEST(test_mutex_basic::test_try_lock_free);
    RUN_TEST(test_mutex_basic::test_try_lock_held);

    RUN_TEST(test_mutex_contention::test_lock_blocks_and_enqueues);
    RUN_TEST(test_mutex_contention::test_unlock_transfers_to_waiter);
    RUN_TEST(test_mutex_contention::test_fifo_ordering);

    RUN_TEST(test_mutex_guard::test_guard_scope);

    RUN_TEST(test_semaphore_basic::test_initial_count);
    RUN_TEST(test_semaphore_basic::test_default_count_zero);
    RUN_TEST(test_semaphore_basic::test_post_increments);
    RUN_TEST(test_semaphore_basic::test_wait_decrements_when_positive);
    RUN_TEST(test_semaphore_basic::test_wait_blocks_when_zero);

    RUN_TEST(test_semaphore_try::test_try_wait_success);
    RUN_TEST(test_semaphore_try::test_try_wait_fail_zero);
    RUN_TEST(test_semaphore_try::test_try_wait_all);

    RUN_TEST(test_semaphore_wake::test_post_wakes_blocked_waiter);
    RUN_TEST(test_semaphore_wake::test_fifo_ordering);

    RUN_TEST(test_semaphore_counting::test_producer_consumer_no_blocking);

    RUN_TEST(test_task_wait_next::test_wait_next_null_after_build);

    TEST_SUMMARY();
}
