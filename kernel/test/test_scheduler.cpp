/**
 * @file kernel/test/test_scheduler.cpp
 * @brief QEMU in-kernel integration tests for process/scheduler (019)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * real Scheduler, TaskBuilder, RoundRobin, and context_switch with actual
 * PMM/VMM/Heap backing.  Tests:
 *   - TaskBuilder: build with entry/name/priority
 *   - RoundRobin: enqueue/dequeue/pick_next with real Task objects
 *   - Scheduler: init, add_task, register_class
 *   - context_switch: cooperative switch between two kernel tasks
 *   - Task lifecycle: state transitions through Ready/Running
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised
 *   - VMM initialised
 *   - Heap initialised
 *   - AddressSpace kernel PML4 initialised
 */

#include <stdint.h>
#include <stddef.h>

namespace {
void kmemset(void* dst, int val, size_t n) {
    auto* p = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i++) p[i] = static_cast<uint8_t>(val);
}
}  // anonymous namespace

#include "big_kernel_test.h"

#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"

using cinux::proc::Task;
using cinux::proc::TaskBuilder;
using cinux::proc::TaskState;
using cinux::proc::CpuContext;
using cinux::proc::Scheduler;
using cinux::proc::RoundRobin;
using cinux::proc::SchedulingClass;
using cinux::proc::context_switch;

// ============================================================
// Test 1: TaskBuilder creates a valid kernel task
// ============================================================

namespace test_task_builder {

void dummy_entry() {
    // Never actually runs in this test; just a non-null entry point
}

void test_build_basic_task() {
    Task* task = TaskBuilder()
        .set_entry(dummy_entry)
        .set_name("test_basic")
        .set_priority(1)
        .build();

    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQ(task->tid, 1u);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));
    TEST_ASSERT(task->name != nullptr);
    // Name should be "test_basic"
    TEST_ASSERT_EQ(task->priority, 1u);
    TEST_ASSERT_NULL(task->addr_space);
    TEST_ASSERT_NULL(task->sched_class);

    // CpuContext should have rip pointing to dummy_entry
    TEST_ASSERT_EQ(task->ctx.rip,
                   reinterpret_cast<uint64_t>(dummy_entry));

    // Stack should be non-zero and aligned
    TEST_ASSERT_NE(task->kernel_stack, 0u);
    TEST_ASSERT_NE(task->kernel_stack_top, 0u);
    TEST_ASSERT_GT(task->kernel_stack_top, task->kernel_stack);

    // Stack overflow magic at the bottom
    TEST_ASSERT_EQ(
        *reinterpret_cast<uint64_t*>(task->kernel_stack),
        TaskBuilder::STACK_MAGIC);
}

}  // namespace test_task_builder

// ============================================================
// Test 2: TaskBuilder returns nullptr for null entry
// ============================================================

namespace test_task_builder_null {

void test_build_null_entry() {
    Task* task = TaskBuilder()
        .set_name("null_entry")
        .build();

    TEST_ASSERT_NULL(task);
}

}  // namespace test_task_builder_null

// ============================================================
// Test 3: Scheduler init registers default RoundRobin
// ============================================================

namespace test_scheduler_init {

void test_init_registers_default() {
    Scheduler::init();

    // After init, current should be null
    TEST_ASSERT_NULL(Scheduler::current());

    // Adding a task with null sched_class should use default RR
    Task* task = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("init_test")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);

    // sched_class should have been assigned
    TEST_ASSERT_NOT_NULL(task->sched_class);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));
}

}  // namespace test_scheduler_init

// ============================================================
// Test 4: RoundRobin enqueue/dequeue/pick_next with real tasks
// ============================================================

namespace test_round_robin {

void test_enqueue_dequeue() {
    RoundRobin rr;

    Task* t1 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("rr_1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("rr_2")
        .build();
    Task* t3 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("rr_3")
        .build();

    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_NOT_NULL(t3);

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    // pick_next should rotate through t1, t2, t3
    Task* n1 = rr.pick_next();
    TEST_ASSERT_EQ(n1, t1);
    TEST_ASSERT_EQ(static_cast<int>(n1->state),
                   static_cast<int>(TaskState::Running));

    Task* n2 = rr.pick_next();
    TEST_ASSERT_EQ(n2, t2);

    Task* n3 = rr.pick_next();
    TEST_ASSERT_EQ(n3, t3);

    // Wrap around
    Task* n4 = rr.pick_next();
    TEST_ASSERT_EQ(n4, t1);
}

void test_dequeue_middle() {
    RoundRobin rr;

    Task* t1 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("dq_1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("dq_2")
        .build();
    Task* t3 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("dq_3")
        .build();

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    rr.dequeue(t2);

    // After dequeuing t2, pick_next should give t1 then t3
    TEST_ASSERT_EQ(rr.pick_next(), t1);
    TEST_ASSERT_EQ(rr.pick_next(), t3);
}

void test_empty_pick_next() {
    RoundRobin rr;
    TEST_ASSERT_NULL(rr.pick_next());
}

}  // namespace test_round_robin

// ============================================================
// Test 5: CpuContext struct layout (kernel-side verification)
// ============================================================

namespace test_cpu_context {

void test_layout() {
    TEST_ASSERT_EQ(sizeof(CpuContext), 64u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r15), 0u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r14), 8u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r13), 16u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r12), 24u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rbp), 32u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rbx), 40u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rsp), 48u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rip), 56u);
    TEST_ASSERT_EQ(alignof(CpuContext), 16u);
}

}  // namespace test_cpu_context

// ============================================================
// Test 6: context_switch cooperative multitasking
// ============================================================

namespace test_context_switch {

// Volatile counters to prevent compiler optimization
static volatile int task_a_count = 0;
static volatile int task_b_count = 0;
static volatile bool done = false;

// We need a boot context and two task contexts
static CpuContext boot_ctx;
static CpuContext task_a_ctx;
static CpuContext task_b_ctx;

static void task_a_func() {
    task_a_count++;
    // Switch to task B
    context_switch(&task_a_ctx, &task_b_ctx);
    // Comes back here after B yields
    task_a_count++;
    // Done
    done = true;
    // Switch back to boot
    context_switch(&task_a_ctx, &boot_ctx);
}

static void task_b_func() {
    task_b_count++;
    // Switch back to task A
    context_switch(&task_b_ctx, &task_a_ctx);
    // Should not reach here in this test
}

void test_cooperative_switch() {
    task_a_count = 0;
    task_b_count = 0;
    done = false;

    // Zero out contexts
    kmemset(&boot_ctx, 0, sizeof(boot_ctx));
    kmemset(&task_a_ctx, 0, sizeof(task_a_ctx));
    kmemset(&task_b_ctx, 0, sizeof(task_b_ctx));

    // Set up task A context: entry = task_a_func
    // Use a small static stack buffer for each task
    static uint8_t stack_a[4096] __attribute__((aligned(16)));
    static uint8_t stack_b[4096] __attribute__((aligned(16)));

    task_a_ctx.rip = reinterpret_cast<uint64_t>(task_a_func);
    task_a_ctx.rsp = reinterpret_cast<uint64_t>(&stack_a[4096]);

    task_b_ctx.rip = reinterpret_cast<uint64_t>(task_b_func);
    task_b_ctx.rsp = reinterpret_cast<uint64_t>(&stack_b[4096]);

    // Switch from boot to task A
    context_switch(&boot_ctx, &task_a_ctx);

    // We come back here when task A switches back to boot
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQ(task_a_count, 2);
    TEST_ASSERT_EQ(task_b_count, 1);
}

}  // namespace test_context_switch

// ============================================================
// Test 7: Task state transitions
// ============================================================

namespace test_task_state {

void test_state_transitions() {
    Task* task = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("state_test")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    // After build, state should be Ready
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));

    // Manually set to Running
    task->state = TaskState::Running;
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Running));

    // Manually set to Blocked
    task->state = TaskState::Blocked;
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Blocked));

    // Manually set to Dead
    task->state = TaskState::Dead;
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Dead));
}

}  // namespace test_task_state

// ============================================================
// Test 8: TaskBuilder default values
// ============================================================

namespace test_task_builder_defaults {

void test_default_name() {
    Task* task = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .build();
    TEST_ASSERT_NOT_NULL(task);
    // Default name should be "unnamed"
    TEST_ASSERT_NOT_NULL(task->name);
}

void test_default_priority() {
    Task* task = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .build();
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQ(task->priority, 0u);
}

void test_increasing_tid() {
    Task* t1 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("tid_a")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("tid_b")
        .build();

    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_GT(t2->tid, t1->tid);
}

}  // namespace test_task_builder_defaults

// ============================================================
// Test 9: Scheduler new APIs (020)
// ============================================================

namespace test_scheduler_new {

void test_is_initialized() {
    Scheduler::init();
    TEST_ASSERT_TRUE(Scheduler::is_initialized());
}

void test_remove_task() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("remove_test")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);
    TEST_ASSERT_NOT_NULL(task->sched_class);

    Scheduler::remove_task(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Dead));
}

void test_block_unblock() {
    Scheduler::init();

    Task* task = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("block_test")
        .build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));

    Scheduler::block(task, "test block");
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Blocked));

    Scheduler::unblock(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state),
                   static_cast<int>(TaskState::Ready));
}

}  // namespace test_scheduler_new

// ============================================================
// Test 10: RoundRobin spinlock correctness (028d)
// ============================================================

namespace test_round_robin_locked {

void test_locked_enqueue_dequeue() {
    RoundRobin rr;

    Task* t1 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("lk_1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("lk_2")
        .build();
    Task* t3 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("lk_3")
        .build();

    TEST_ASSERT_NOT_NULL(t1);
    TEST_ASSERT_NOT_NULL(t2);
    TEST_ASSERT_NOT_NULL(t3);

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    Task* n1 = rr.pick_next();
    TEST_ASSERT_EQ(n1, t1);
    TEST_ASSERT_EQ(static_cast<int>(n1->state),
                   static_cast<int>(TaskState::Running));

    Task* n2 = rr.pick_next();
    TEST_ASSERT_EQ(n2, t2);

    Task* n3 = rr.pick_next();
    TEST_ASSERT_EQ(n3, t3);

    Task* n4 = rr.pick_next();
    TEST_ASSERT_EQ(n4, t1);
}

void test_locked_dequeue_middle() {
    RoundRobin rr;

    Task* t1 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("ldq_1")
        .build();
    Task* t2 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("ldq_2")
        .build();
    Task* t3 = TaskBuilder()
        .set_entry(test_task_builder::dummy_entry)
        .set_name("ldq_3")
        .build();

    rr.enqueue(t1);
    rr.enqueue(t2);
    rr.enqueue(t3);

    rr.dequeue(t2);

    TEST_ASSERT_EQ(rr.pick_next(), t1);
    TEST_ASSERT_EQ(rr.pick_next(), t3);
}

void test_locked_fifo_order() {
    RoundRobin rr;

    Task* tasks[8];
    for (int i = 0; i < 8; i++) {
        char name[4];
        name[0] = 'f';
        name[1] = static_cast<char>('0' + i);
        name[2] = '\0';
        tasks[i] = TaskBuilder()
            .set_entry(test_task_builder::dummy_entry)
            .set_name(name)
            .build();
        TEST_ASSERT_NOT_NULL(tasks[i]);
        rr.enqueue(tasks[i]);
    }

    for (int i = 0; i < 8; i++) {
        Task* n = rr.pick_next();
        TEST_ASSERT_EQ(n, tasks[i]);
    }
}

}  // namespace test_round_robin_locked

// ============================================================
// Entry point
// ============================================================

extern "C" void run_scheduler_tests() {
    TEST_SECTION("Scheduler/Process Tests (020)");

    RUN_TEST(test_task_builder::test_build_basic_task);
    RUN_TEST(test_task_builder_null::test_build_null_entry);
    RUN_TEST(test_scheduler_init::test_init_registers_default);
    RUN_TEST(test_round_robin::test_enqueue_dequeue);
    RUN_TEST(test_round_robin::test_dequeue_middle);
    RUN_TEST(test_round_robin::test_empty_pick_next);
    RUN_TEST(test_cpu_context::test_layout);
    RUN_TEST(test_context_switch::test_cooperative_switch);
    RUN_TEST(test_task_state::test_state_transitions);
    RUN_TEST(test_task_builder_defaults::test_default_name);
    RUN_TEST(test_task_builder_defaults::test_default_priority);
    RUN_TEST(test_task_builder_defaults::test_increasing_tid);
    RUN_TEST(test_scheduler_new::test_is_initialized);
    RUN_TEST(test_scheduler_new::test_remove_task);
    RUN_TEST(test_scheduler_new::test_block_unblock);

    RUN_TEST(test_round_robin_locked::test_locked_enqueue_dequeue);
    RUN_TEST(test_round_robin_locked::test_locked_dequeue_middle);
    RUN_TEST(test_round_robin_locked::test_locked_fifo_order);

    TEST_SUMMARY();
}
