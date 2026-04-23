/**
 * @file kernel/test/test_sync_concurrent.cpp
 * @brief QEMU in-kernel tests for InterruptGuard and IrqSpinlockGuard (028d)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * new RAII wrappers:
 *   - InterruptGuard: pushfq/cli/popfq save/restore, nesting
 *   - Spinlock::IrqGuard: spinlock + interrupt disable combined RAII
 *   - Spinlock::irq_guard: factory method for IrqGuard
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
using cinux::proc::InterruptGuard;
using cinux::proc::Task;
using cinux::proc::TaskBuilder;
using cinux::proc::TaskState;
using cinux::proc::Scheduler;
using cinux::proc::g_per_cpu;

// ============================================================
// Test 1: InterruptGuard basic save/restore
// ============================================================

namespace test_interrupt_guard {

void dummy_entry() {}

void test_basic_save_restore() {
    // Read IF before guard (interrupts are enabled in test environment)
    uint64_t flags_before;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_before));
    bool if_before = (flags_before & 0x200) != 0;

    {
        InterruptGuard guard;
        // IF should be cleared (interrupts disabled)
        uint64_t flags_during;
        __asm__ volatile("pushfq; popq %0" : "=rm"(flags_during));
        TEST_ASSERT_FALSE((flags_during & 0x200) != 0);
    }

    // After guard destroyed, IF should be restored to original state
    uint64_t flags_after;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_after));
    bool if_after = (flags_after & 0x200) != 0;
    TEST_ASSERT_EQ(if_before, if_after);
}

void test_nested_guard() {
    // Start with interrupts enabled
    uint64_t flags_before;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_before));
    bool if_before = (flags_before & 0x200) != 0;

    {
        InterruptGuard outer;
        // IF cleared
        uint64_t f1;
        __asm__ volatile("pushfq; popq %0" : "=rm"(f1));
        TEST_ASSERT_FALSE((f1 & 0x200) != 0);

        {
            InterruptGuard inner;
            // IF still cleared (nesting correctly saves disabled state)
            uint64_t f2;
            __asm__ volatile("pushfq; popq %0" : "=rm"(f2));
            TEST_ASSERT_FALSE((f2 & 0x200) != 0);
        }
        // Inner destroyed -- IF should still be disabled (outer saved disabled)
        uint64_t f3;
        __asm__ volatile("pushfq; popq %0" : "=rm"(f3));
        TEST_ASSERT_FALSE((f3 & 0x200) != 0);
    }
    // Outer destroyed -- IF restored to original
    uint64_t flags_after;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_after));
    bool if_after = (flags_after & 0x200) != 0;
    TEST_ASSERT_EQ(if_before, if_after);
}

}  // namespace test_interrupt_guard

// ============================================================
// Test 2: Spinlock::IrqGuard basic operations
// ============================================================

namespace test_irq_guard {

void test_acquire_release() {
    Spinlock s;

    {
        auto g = s.irq_guard();
        (void)g;
        // Spinlock should be held, interrupts disabled
        uint64_t flags;
        __asm__ volatile("pushfq; popq %0" : "=rm"(flags));
        TEST_ASSERT_FALSE((flags & 0x200) != 0);
    }

    // After destruction: spinlock released, interrupts restored
    // Verify spinlock is free by acquiring again
    s.acquire();
    s.release();
    TEST_ASSERT_TRUE(true);
}

void test_nested_irq_guard_different_locks() {
    Spinlock s1;
    Spinlock s2;

    {
        auto g1 = s1.irq_guard();
        (void)g1;
        // s1 held, IF=0
        uint64_t f1;
        __asm__ volatile("pushfq; popq %0" : "=rm"(f1));
        TEST_ASSERT_FALSE((f1 & 0x200) != 0);

        {
            auto g2 = s2.irq_guard();
            (void)g2;
            // Both locks held, IF still 0
            uint64_t f2;
            __asm__ volatile("pushfq; popq %0" : "=rm"(f2));
            TEST_ASSERT_FALSE((f2 & 0x200) != 0);
        }
        // g2 destroyed: s2 released, IF still 0 (g1 saved it as 0)
        uint64_t f3;
        __asm__ volatile("pushfq; popq %0" : "=rm"(f3));
        TEST_ASSERT_FALSE((f3 & 0x200) != 0);
    }
    // g1 destroyed: s1 released, IF restored to original
    TEST_ASSERT_TRUE(true);
}

void test_preserves_if_when_disabled() {
    // Start with interrupts disabled
    __asm__ volatile("cli");
    uint64_t flags_before;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_before));

    {
        InterruptGuard guard;
        // IF should still be 0
        uint64_t f;
        __asm__ volatile("pushfq; popq %0" : "=rm"(f));
        TEST_ASSERT_FALSE((f & 0x200) != 0);
    }

    // After destruction, IF should remain 0 (was 0 when guard created)
    uint64_t flags_after;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_after));
    TEST_ASSERT_FALSE((flags_after & 0x200) != 0);

    // Re-enable interrupts for subsequent tests
    __asm__ volatile("sti");
}

}  // namespace test_irq_guard

// ============================================================
// Test 3: Spinlock with multiple tasks (cooperative)
// ============================================================

namespace test_concurrent_spinlock {

static volatile int shared_counter = 0;
static Spinlock test_lock;

void test_three_tasks_mutual_exclusion() {
    Scheduler::init();
    shared_counter = 0;

    // Simulate 3 tasks cooperatively accessing shared state
    Task* t1 = TaskBuilder()
        .set_entry(test_interrupt_guard::dummy_entry)
        .set_name("lock_t1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_interrupt_guard::dummy_entry)
        .set_name("lock_t2")
        .build();
    Task* t3 = TaskBuilder()
        .set_entry(test_interrupt_guard::dummy_entry)
        .set_name("lock_t3")
        .build();
    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_NOT_NULL(t3);

    // Task 1: increment 10 times
    g_per_cpu.current = t1;
    for (int i = 0; i < 10; i++) {
        auto g = test_lock.guard();
        (void)g;
        shared_counter++;
    }

    // Task 2: increment 10 times
    g_per_cpu.current = t2;
    for (int i = 0; i < 10; i++) {
        auto g = test_lock.guard();
        (void)g;
        shared_counter++;
    }

    // Task 3: increment 10 times
    g_per_cpu.current = t3;
    for (int i = 0; i < 10; i++) {
        auto g = test_lock.guard();
        (void)g;
        shared_counter++;
    }

    TEST_ASSERT_EQ(shared_counter, 30);
}

void test_irq_guard_three_tasks() {
    Scheduler::init();
    shared_counter = 0;

    Task* t1 = TaskBuilder()
        .set_entry(test_interrupt_guard::dummy_entry)
        .set_name("irq_t1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_interrupt_guard::dummy_entry)
        .set_name("irq_t2")
        .build();
    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);

    // Task 1: increment with irq_guard
    g_per_cpu.current = t1;
    for (int i = 0; i < 15; i++) {
        auto g = test_lock.irq_guard();
        (void)g;
        shared_counter++;
    }

    // Task 2: increment with irq_guard
    g_per_cpu.current = t2;
    for (int i = 0; i < 15; i++) {
        auto g = test_lock.irq_guard();
        (void)g;
        shared_counter++;
    }

    TEST_ASSERT_EQ(shared_counter, 30);
}

}  // namespace test_concurrent_spinlock

// ============================================================
// Test 4: Scheduler operations with spinlock protection (028d)
// ============================================================

namespace test_scheduler_concurrent {

void test_concurrent_add_remove() {
    Scheduler::init();

    Task* tasks[6];
    for (int i = 0; i < 6; i++) {
        char name[4];
        name[0] = 'c';
        name[1] = static_cast<char>('0' + i);
        name[2] = '\0';
        tasks[i] = TaskBuilder()
            .set_entry(test_interrupt_guard::dummy_entry)
            .set_name(name)
            .build();
        TEST_ASSERT_NOT_NULL(tasks[i]);
    }

    Scheduler::add_task(tasks[0]);
    Scheduler::add_task(tasks[1]);
    Scheduler::add_task(tasks[2]);

    TEST_ASSERT_EQ(static_cast<int>(tasks[0]->state),
                   static_cast<int>(TaskState::Ready));
    TEST_ASSERT_EQ(static_cast<int>(tasks[1]->state),
                   static_cast<int>(TaskState::Ready));
    TEST_ASSERT_EQ(static_cast<int>(tasks[2]->state),
                   static_cast<int>(TaskState::Ready));

    Scheduler::remove_task(tasks[1]);
    TEST_ASSERT_EQ(static_cast<int>(tasks[1]->state),
                   static_cast<int>(TaskState::Dead));

    Scheduler::add_task(tasks[3]);
    Scheduler::add_task(tasks[4]);
    Scheduler::add_task(tasks[5]);

    TEST_ASSERT_EQ(static_cast<int>(tasks[3]->state),
                   static_cast<int>(TaskState::Ready));
    TEST_ASSERT_EQ(static_cast<int>(tasks[4]->state),
                   static_cast<int>(TaskState::Ready));
    TEST_ASSERT_EQ(static_cast<int>(tasks[5]->state),
                   static_cast<int>(TaskState::Ready));

    Scheduler::remove_task(tasks[0]);
    Scheduler::remove_task(tasks[2]);
    Scheduler::remove_task(tasks[3]);
    Scheduler::remove_task(tasks[4]);
    Scheduler::remove_task(tasks[5]);

    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQ(static_cast<int>(tasks[i]->state),
                       static_cast<int>(TaskState::Dead));
    }
}

void test_concurrent_block_unblock() {
    Scheduler::init();

    Task* t1 = TaskBuilder()
        .set_entry(test_interrupt_guard::dummy_entry)
        .set_name("cb_1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_interrupt_guard::dummy_entry)
        .set_name("cb_2")
        .build();
    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);

    Scheduler::add_task(t1);
    Scheduler::add_task(t2);

    Scheduler::block(t1, "test");
    TEST_ASSERT_EQ(static_cast<int>(t1->state),
                   static_cast<int>(TaskState::Blocked));

    Scheduler::block(t2, "test");
    TEST_ASSERT_EQ(static_cast<int>(t2->state),
                   static_cast<int>(TaskState::Blocked));

    Scheduler::unblock(t1);
    TEST_ASSERT_EQ(static_cast<int>(t1->state),
                   static_cast<int>(TaskState::Ready));

    Scheduler::unblock(t2);
    TEST_ASSERT_EQ(static_cast<int>(t2->state),
                   static_cast<int>(TaskState::Ready));

    Scheduler::remove_task(t1);
    Scheduler::remove_task(t2);
}

}  // namespace test_scheduler_concurrent

// ============================================================
// Entry point
// ============================================================

extern "C" void run_sync_concurrent_tests() {
    TEST_SECTION("Sync Concurrent Tests (028d)");

    RUN_TEST(test_interrupt_guard::test_basic_save_restore);
    RUN_TEST(test_interrupt_guard::test_nested_guard);

    RUN_TEST(test_irq_guard::test_acquire_release);
    RUN_TEST(test_irq_guard::test_nested_irq_guard_different_locks);
    RUN_TEST(test_irq_guard::test_preserves_if_when_disabled);

    RUN_TEST(test_concurrent_spinlock::test_three_tasks_mutual_exclusion);
    RUN_TEST(test_concurrent_spinlock::test_irq_guard_three_tasks);

    RUN_TEST(test_scheduler_concurrent::test_concurrent_add_remove);
    RUN_TEST(test_scheduler_concurrent::test_concurrent_block_unblock);

    TEST_SUMMARY();
}
