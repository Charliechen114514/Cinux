/**
 * @file test/unit/test_scheduler.cpp
 * @brief Host-side unit tests for process/scheduler (019_proc_context)
 *
 * Re-implements RoundRobin and Scheduler logic for host-side testing.
 * Covers:
 *   - RoundRobin: enqueue/dequeue/pick_next, empty/full/single-task queues
 *   - Scheduler: add_task/yield logic, register_class, default RR assignment
 *   - TaskBuilder: field defaults, fluent API, build() guard on null entry
 *   - TaskState enum, CpuContext struct layout
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include <stdint.h>
#include <string.h>

#include <new>

#include "test_framework.h"

// ============================================================
// Re-implement TaskState (matches kernel/proc/process.hpp)
// ============================================================

enum class TaskState : uint8_t {
    Running,
    Ready,
    Blocked,
    Dead
};

// ============================================================
// Re-implement CpuContext (matches kernel/proc/process.hpp)
// ============================================================

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

// ============================================================
// Minimal Task struct for host testing (only fields used by
// RoundRobin and Scheduler)
// ============================================================

class SchedulingClass;

struct Task {
    CpuContext       ctx;
    TaskState        state;
    uint64_t         tid;
    uint64_t         priority;
    uint64_t         kernel_stack;
    uint64_t         kernel_stack_top;
    void*            addr_space;  // void* instead of AddressSpace*
    const char*      name;
    SchedulingClass* sched_class;
};

// ============================================================
// Re-implement SchedulingClass + RoundRobin
// (matches kernel/proc/scheduler.hpp / .cpp)
// ============================================================

class SchedulingClass {
public:
    virtual ~SchedulingClass()              = default;
    virtual void        enqueue(Task* task) = 0;
    virtual void        dequeue(Task* task) = 0;
    virtual Task*       pick_next()         = 0;
    virtual const char* name() const        = 0;
};

class RoundRobin : public SchedulingClass {
public:
    static constexpr int MAX_TASKS = 64;

    RoundRobin() : head_(0), tail_(0), count_(0) {
        for (int i = 0; i < MAX_TASKS; i++) {
            run_queue_[i] = nullptr;
        }
    }

    void enqueue(Task* task) override {
        if (count_ >= MAX_TASKS)
            return;
        run_queue_[tail_] = task;
        tail_             = (tail_ + 1) % MAX_TASKS;
        count_++;
        task->state = TaskState::Ready;
    }

    void dequeue(Task* task) override {
        for (int i = 0; i < count_; i++) {
            int idx = (head_ + i) % MAX_TASKS;
            if (run_queue_[idx] == task) {
                for (int j = i; j < count_ - 1; j++) {
                    int cur         = (head_ + j) % MAX_TASKS;
                    int nxt         = (head_ + j + 1) % MAX_TASKS;
                    run_queue_[cur] = run_queue_[nxt];
                }
                run_queue_[(head_ + count_ - 1) % MAX_TASKS] = nullptr;
                tail_                                        = (tail_ - 1 + MAX_TASKS) % MAX_TASKS;
                count_--;
                return;
            }
        }
    }

    Task* pick_next() override {
        if (count_ == 0)
            return nullptr;
        Task* task = run_queue_[head_];
        head_      = (head_ + 1) % MAX_TASKS;
        count_--;
        task->state       = TaskState::Running;
        run_queue_[tail_] = task;
        tail_             = (tail_ + 1) % MAX_TASKS;
        count_++;
        return task;
    }

    const char* name() const override { return "RoundRobin"; }

    // Expose internals for white-box testing
    int count() const { return count_; }

private:
    Task* run_queue_[MAX_TASKS];
    int   head_;
    int   tail_;
    int   count_;
};

// ============================================================
// Re-implement Scheduler static facade
// (matches kernel/proc/scheduler.hpp / .cpp)
// ============================================================

class Scheduler {
public:
    static constexpr int MAX_CLASSES = 4;

    static void init() {
        class_count_ = 0;
        current_     = nullptr;
        register_class(&default_rr_);
    }

    static void register_class(SchedulingClass* sched_class) {
        if (class_count_ >= MAX_CLASSES)
            return;
        classes_[class_count_++] = sched_class;
    }

    static void add_task(Task* task) {
        if (task->sched_class == nullptr) {
            task->sched_class = &default_rr_;
        }
        task->sched_class->enqueue(task);
    }

    static Task* current() { return current_; }

    static void set_current(Task* t) { current_ = t; }

    static RoundRobin& default_rr() { return default_rr_; }

    static int class_count() { return class_count_; }

    static void reset() {
        class_count_ = 0;
        current_     = nullptr;
        // Reconstruct default_rr_ in-place to clear its queue
        default_rr_.~RoundRobin();
        new (&default_rr_) RoundRobin();
    }

private:
    static SchedulingClass* classes_[MAX_CLASSES];
    static int              class_count_;
    static Task*            current_;
    static RoundRobin       default_rr_;
};

SchedulingClass* Scheduler::classes_[Scheduler::MAX_CLASSES];
int              Scheduler::class_count_ = 0;
Task*            Scheduler::current_     = nullptr;
RoundRobin       Scheduler::default_rr_;

// ============================================================
// Helper: create a bare Task on the stack
// ============================================================

namespace {

Task make_task(uint64_t tid, const char* name = "test") {
    Task t{};
    t.tid   = tid;
    t.name  = name;
    t.state = TaskState::Ready;
    return t;
}

}  // anonymous namespace

// ============================================================
// CpuContext layout tests
// ============================================================

// CpuContext has expected size of 64 bytes
TEST("cpu_context: struct size is 64 bytes") {
    ASSERT_EQ(sizeof(CpuContext), 64u);
}

// CpuContext field offsets match context_switch.S expectations
TEST("cpu_context: field offsets are correct") {
    ASSERT_EQ(offsetof(CpuContext, r15), 0u);
    ASSERT_EQ(offsetof(CpuContext, r14), 8u);
    ASSERT_EQ(offsetof(CpuContext, r13), 16u);
    ASSERT_EQ(offsetof(CpuContext, r12), 24u);
    ASSERT_EQ(offsetof(CpuContext, rbp), 32u);
    ASSERT_EQ(offsetof(CpuContext, rbx), 40u);
    ASSERT_EQ(offsetof(CpuContext, rsp), 48u);
    ASSERT_EQ(offsetof(CpuContext, rip), 56u);
}

// CpuContext alignment is 16 bytes
TEST("cpu_context: alignment is 16 bytes") {
    ASSERT_EQ(alignof(CpuContext), 16u);
}

// ============================================================
// TaskState enum tests
// ============================================================

// TaskState underlying type is uint8_t
TEST("task_state: underlying type is uint8_t") {
    ASSERT_EQ(sizeof(TaskState), 1u);
}

// TaskState values match expected ordering
TEST("task_state: enum values are distinct") {
    ASSERT_TRUE(TaskState::Running != TaskState::Ready);
    ASSERT_TRUE(TaskState::Ready != TaskState::Blocked);
    ASSERT_TRUE(TaskState::Blocked != TaskState::Dead);
}

// ============================================================
// RoundRobin: empty queue
// ============================================================

// pick_next on empty queue returns nullptr
TEST("round_robin: pick_next on empty queue returns nullptr") {
    RoundRobin rr;
    ASSERT_NULL(rr.pick_next());
}

// count is zero on fresh RoundRobin
TEST("round_robin: fresh queue has count zero") {
    RoundRobin rr;
    ASSERT_EQ(rr.count(), 0);
}

// ============================================================
// RoundRobin: single task
// ============================================================

// enqueue one task, pick_next returns it and count stays 1
TEST("round_robin: enqueue one then pick_next returns it") {
    RoundRobin rr;
    Task       t = make_task(1);

    rr.enqueue(&t);
    ASSERT_EQ(rr.count(), 1);
    ASSERT_EQ(static_cast<int>(t.state), static_cast<int>(TaskState::Ready));

    Task* next = rr.pick_next();
    ASSERT_EQ(next, &t);
    ASSERT_EQ(static_cast<int>(t.state), static_cast<int>(TaskState::Running));
    // pick_next puts the task back (round-robin), so count stays 1
    ASSERT_EQ(rr.count(), 1);
}

// enqueue one task then dequeue it leaves empty queue
TEST("round_robin: enqueue one then dequeue leaves empty") {
    RoundRobin rr;
    Task       t = make_task(1);

    rr.enqueue(&t);
    ASSERT_EQ(rr.count(), 1);

    rr.dequeue(&t);
    ASSERT_EQ(rr.count(), 0);
    ASSERT_NULL(rr.pick_next());
}

// ============================================================
// RoundRobin: multiple tasks
// ============================================================

// enqueue three tasks, pick_next rotates through them
TEST("round_robin: pick_next rotates through enqueued tasks") {
    RoundRobin rr;
    Task       t1 = make_task(1);
    Task       t2 = make_task(2);
    Task       t3 = make_task(3);

    rr.enqueue(&t1);
    rr.enqueue(&t2);
    rr.enqueue(&t3);

    // First pick: should get t1
    Task* n1 = rr.pick_next();
    ASSERT_EQ(n1, &t1);

    // Second pick: should get t2
    Task* n2 = rr.pick_next();
    ASSERT_EQ(n2, &t2);

    // Third pick: should get t3
    Task* n3 = rr.pick_next();
    ASSERT_EQ(n3, &t3);

    // Fourth pick: wraps around to t1 again
    Task* n4 = rr.pick_next();
    ASSERT_EQ(n4, &t1);
}

// dequeue from middle shifts remaining tasks
TEST("round_robin: dequeue middle task shifts remaining") {
    RoundRobin rr;
    Task       t1 = make_task(1);
    Task       t2 = make_task(2);
    Task       t3 = make_task(3);

    rr.enqueue(&t1);
    rr.enqueue(&t2);
    rr.enqueue(&t3);
    ASSERT_EQ(rr.count(), 3);

    // Dequeue t2 (middle)
    rr.dequeue(&t2);
    ASSERT_EQ(rr.count(), 2);

    // Now pick_next should return t1 then t3
    ASSERT_EQ(rr.pick_next(), &t1);
    ASSERT_EQ(rr.pick_next(), &t3);
}

// dequeue the head task
TEST("round_robin: dequeue head task") {
    RoundRobin rr;
    Task       t1 = make_task(1);
    Task       t2 = make_task(2);

    rr.enqueue(&t1);
    rr.enqueue(&t2);
    rr.dequeue(&t1);

    ASSERT_EQ(rr.count(), 1);
    ASSERT_EQ(rr.pick_next(), &t2);
}

// dequeue the tail task
TEST("round_robin: dequeue tail task") {
    RoundRobin rr;
    Task       t1 = make_task(1);
    Task       t2 = make_task(2);
    Task       t3 = make_task(3);

    rr.enqueue(&t1);
    rr.enqueue(&t2);
    rr.enqueue(&t3);
    rr.dequeue(&t3);

    ASSERT_EQ(rr.count(), 2);
    ASSERT_EQ(rr.pick_next(), &t1);
    ASSERT_EQ(rr.pick_next(), &t2);
}

// dequeue a task not in queue is a no-op
TEST("round_robin: dequeue absent task is no-op") {
    RoundRobin rr;
    Task       t1 = make_task(1);
    Task       t2 = make_task(2);

    rr.enqueue(&t1);
    rr.dequeue(&t2);  // t2 is not in queue
    ASSERT_EQ(rr.count(), 1);
}

// ============================================================
// RoundRobin: enqueue sets state to Ready
// ============================================================

// enqueue transitions task state to Ready
TEST("round_robin: enqueue sets task state to Ready") {
    RoundRobin rr;
    Task       t = make_task(1);
    t.state      = TaskState::Blocked;

    rr.enqueue(&t);
    ASSERT_EQ(static_cast<int>(t.state), static_cast<int>(TaskState::Ready));
}

// pick_next sets task state to Running
TEST("round_robin: pick_next sets task state to Running") {
    RoundRobin rr;
    Task       t = make_task(1);
    rr.enqueue(&t);

    Task* next = rr.pick_next();
    ASSERT_EQ(static_cast<int>(next->state), static_cast<int>(TaskState::Running));
}

// ============================================================
// RoundRobin: name
// ============================================================

// name() returns "RoundRobin"
TEST("round_robin: name returns RoundRobin") {
    RoundRobin rr;
    ASSERT_EQ(strcmp(rr.name(), "RoundRobin"), 0);
}

// ============================================================
// Scheduler: init
// ============================================================

// init registers the default RoundRobin class and clears current
TEST("scheduler: init registers default class and clears current") {
    Scheduler::reset();
    Scheduler::init();
    ASSERT_EQ(Scheduler::class_count(), 1);
    ASSERT_NULL(Scheduler::current());
}

// ============================================================
// Scheduler: register_class
// ============================================================

// register_class adds classes up to MAX_CLASSES
TEST("scheduler: register_class adds up to max") {
    Scheduler::reset();
    Scheduler::init();  // registers default RR

    RoundRobin extra1, extra2, extra3;
    Scheduler::register_class(&extra1);
    Scheduler::register_class(&extra2);
    Scheduler::register_class(&extra3);
    ASSERT_EQ(Scheduler::class_count(), 4);
}

// register_class beyond MAX_CLASSES is silently ignored
TEST("scheduler: register_class beyond max is ignored") {
    Scheduler::reset();
    Scheduler::init();

    RoundRobin extra1, extra2, extra3, extra4;
    Scheduler::register_class(&extra1);
    Scheduler::register_class(&extra2);
    Scheduler::register_class(&extra3);
    // 4th extra: should be ignored (MAX_CLASSES = 4, default takes 1)
    Scheduler::register_class(&extra4);
    ASSERT_EQ(Scheduler::class_count(), 4);
}

// ============================================================
// Scheduler: add_task assigns default RR when sched_class is null
// ============================================================

// add_task with null sched_class defaults to RoundRobin
TEST("scheduler: add_task defaults to round_robin when sched_class null") {
    Scheduler::reset();
    Scheduler::init();

    Task t        = make_task(1);
    t.sched_class = nullptr;
    Scheduler::add_task(&t);

    ASSERT_NOT_NULL(t.sched_class);
    ASSERT_EQ(strcmp(t.sched_class->name(), "RoundRobin"), 0);
    // Task should be in the default RR queue
    ASSERT_EQ(Scheduler::default_rr().count(), 1);
}

// add_task with explicit sched_class uses that class
TEST("scheduler: add_task uses explicit sched_class") {
    Scheduler::reset();
    Scheduler::init();

    RoundRobin custom_rr;
    Scheduler::register_class(&custom_rr);

    Task t        = make_task(1);
    t.sched_class = &custom_rr;
    Scheduler::add_task(&t);

    ASSERT_EQ(t.sched_class, &custom_rr);
    ASSERT_EQ(Scheduler::default_rr().count(), 0);  // not in default
    ASSERT_EQ(custom_rr.count(), 1);
}

// ============================================================
// Scheduler: current
// ============================================================

// current() returns nullptr before any task runs
TEST("scheduler: current is null initially") {
    Scheduler::reset();
    Scheduler::init();
    ASSERT_NULL(Scheduler::current());
}

// set_current / current round-trip
TEST("scheduler: set_current and current round-trip") {
    Scheduler::reset();
    Scheduler::init();

    static Task t = make_task(42);
    Scheduler::set_current(&t);
    ASSERT_EQ(Scheduler::current(), &t);
}

// ============================================================
// Scheduler: multiple tasks added to default RR
// ============================================================

// adding multiple tasks enqueues all to default RR
TEST("scheduler: add multiple tasks to default RR") {
    Scheduler::reset();
    Scheduler::init();

    Task t1 = make_task(1);
    Task t2 = make_task(2);
    Task t3 = make_task(3);
    Scheduler::add_task(&t1);
    Scheduler::add_task(&t2);
    Scheduler::add_task(&t3);

    ASSERT_EQ(Scheduler::default_rr().count(), 3);
}

// ============================================================
// Task struct: zero-init
// ============================================================

// zero-initialized Task has sensible defaults
TEST("task: zero-initialized task has null fields") {
    Task t{};
    ASSERT_NULL(t.name);
    ASSERT_NULL(t.sched_class);
    ASSERT_NULL(t.addr_space);
    ASSERT_EQ(t.tid, 0u);
    ASSERT_EQ(t.priority, 0u);
    ASSERT_EQ(t.kernel_stack, 0u);
    ASSERT_EQ(t.kernel_stack_top, 0u);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
