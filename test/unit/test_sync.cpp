/**
 * @file test/unit/test_sync.cpp
 * @brief Host-side unit tests for synchronization primitives (021_proc_sync)
 *
 * Re-implements Spinlock, Mutex, and Semaphore logic for host-side testing.
 * Covers:
 *   - Spinlock: acquire/release, RAII guard, double-release safety
 *   - Mutex: lock/unlock, try_lock, FIFO wait queue, RAII guard,
 *            owner transfer, contention simulation
 *   - Semaphore: post/wait/try_wait, count tracking, FIFO wait queue,
 *                boundary conditions (count zero, negative count)
 *   - Intrusive wait queue: enqueue/dequeue, single/multiple waiters,
 *                            empty queue edge cases
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include <stdint.h>
#include <string.h>

#include <atomic>

#include "test_framework.h"

// ============================================================
// Re-implement minimal types (matches kernel/proc/process.hpp)
// ============================================================

enum class TaskState : uint8_t {
    Running,
    Ready,
    Blocked,
    Dead
};

struct Task {
    int       dummy;  // placeholder for CpuContext
    TaskState state;
    uint64_t  tid;
    Task*     wait_next;
};

// ============================================================
// Mock Scheduler: block / unblock with recording
// ============================================================

namespace mock_scheduler {

static Task* last_blocked   = nullptr;
static Task* last_unblocked = nullptr;
static int   block_count    = 0;
static int   unblock_count  = 0;

void reset() {
    last_blocked   = nullptr;
    last_unblocked = nullptr;
    block_count    = 0;
    unblock_count  = 0;
}

void block(Task* task, const char* reason) {
    (void)reason;
    task->state  = TaskState::Blocked;
    last_blocked = task;
    block_count++;
}

void unblock(Task* task) {
    task->state    = TaskState::Ready;
    last_unblocked = task;
    unblock_count++;
}

}  // namespace mock_scheduler

// ============================================================
// Mock g_per_cpu
// ============================================================

namespace mock_per_cpu {

static Task tasks[8];
static int  current_index = 0;

void init() {
    for (int i = 0; i < 8; i++) {
        memset(&tasks[i], 0, sizeof(Task));
        tasks[i].tid   = static_cast<uint64_t>(i + 1);
        tasks[i].state = TaskState::Ready;
    }
    current_index = 0;
}

Task* current() {
    return &tasks[current_index];
}

void set_current(int idx) {
    current_index = idx;
}

}  // namespace mock_per_cpu

// ============================================================
// Re-implement Spinlock (host-side: uses std::atomic<bool>)
// ============================================================

class Spinlock {
public:
    Spinlock() = default;

    void acquire() {
        while (locked_.exchange(true, std::memory_order_acquire)) {
            // spin (no pause instruction on host)
        }
    }

    void release() { locked_.store(false, std::memory_order_release); }

    [[nodiscard]] auto guard() { return Guard(this); }

    bool is_locked() const { return locked_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> locked_{false};

    class Guard {
    public:
        explicit Guard(Spinlock* lock) : lock_(lock) { lock_->acquire(); }
        ~Guard() { lock_->release(); }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Spinlock* lock_;
    };
};

// ============================================================
// Re-implement Mutex wait queue (matches sync.cpp logic)
// ============================================================

// Forward declare the enqueue/dequeue helpers used by Mutex and Semaphore
// These are duplicated because the kernel version operates on Task* directly.

static void enqueue_waiter(Task*& head, Task* task) {
    task->wait_next = nullptr;
    if (head == nullptr) {
        head = task;
        return;
    }
    Task* tail = head;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}

static Task* dequeue_waiter(Task*& head) {
    if (head == nullptr) {
        return nullptr;
    }
    Task* task      = head;
    head            = task->wait_next;
    task->wait_next = nullptr;
    return task;
}

// ============================================================
// Re-implement Mutex (matches kernel/proc/sync.cpp)
// ============================================================

class Mutex {
public:
    Mutex() = default;

    void lock() {
        spin_.acquire();
        if (owner_ == nullptr) {
            owner_ = mock_per_cpu::current();
            spin_.release();
            return;
        }
        Task* self = mock_per_cpu::current();
        enqueue_waiter(wait_head_, self);
        spin_.release();
        mock_scheduler::block(self, "mutex");
    }

    void unlock() {
        spin_.acquire();
        Task* waiter = dequeue_waiter(wait_head_);
        if (waiter == nullptr) {
            owner_ = nullptr;
            spin_.release();
            return;
        }
        owner_ = waiter;
        spin_.release();
        mock_scheduler::unblock(waiter);
    }

    bool try_lock() {
        spin_.acquire();
        if (owner_ != nullptr) {
            spin_.release();
            return false;
        }
        owner_ = mock_per_cpu::current();
        spin_.release();
        return true;
    }

    [[nodiscard]] auto guard() { return Guard(this); }

    // Test helpers
    Task* owner() const { return owner_; }
    Task* wait_head() const { return wait_head_; }

private:
    Spinlock spin_;
    Task*    owner_     = nullptr;
    Task*    wait_head_ = nullptr;

    class Guard {
    public:
        explicit Guard(Mutex* mtx) : mtx_(mtx) { mtx_->lock(); }
        ~Guard() { mtx_->unlock(); }
        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Mutex* mtx_;
    };
};

// ============================================================
// Re-implement Semaphore (matches kernel/proc/sync.cpp)
// ============================================================

class Semaphore {
public:
    explicit Semaphore(int64_t initial = 0) : count_(initial), wait_head_(nullptr) {}

    void post() {
        spin_.acquire();
        count_++;
        Task* waiter = dequeue_waiter(wait_head_);
        spin_.release();
        if (waiter != nullptr) {
            mock_scheduler::unblock(waiter);
        }
    }

    void wait() {
        spin_.acquire();
        count_--;
        if (count_ >= 0) {
            spin_.release();
            return;
        }
        Task* self = mock_per_cpu::current();
        enqueue_waiter(wait_head_, self);
        spin_.release();
        mock_scheduler::block(self, "semaphore");
    }

    bool try_wait() {
        spin_.acquire();
        if (count_ <= 0) {
            spin_.release();
            return false;
        }
        count_--;
        spin_.release();
        return true;
    }

    int64_t count() const { return count_; }

private:
    Spinlock spin_;
    int64_t  count_;
    Task*    wait_head_ = nullptr;
};

// ============================================================
// Helper: create a bare Task
// ============================================================

namespace {

Task make_task(uint64_t tid) {
    Task t{};
    t.tid   = tid;
    t.state = TaskState::Ready;
    return t;
}

}  // anonymous namespace

// ============================================================
// Spinlock tests
// ============================================================

// Fresh spinlock is unlocked
TEST("spinlock: initial state is unlocked") {
    Spinlock s;
    ASSERT_FALSE(s.is_locked());
}

// acquire then release cycles correctly
TEST("spinlock: acquire and release cycle") {
    Spinlock s;
    s.acquire();
    ASSERT_TRUE(s.is_locked());
    s.release();
    ASSERT_FALSE(s.is_locked());
}

// RAII guard acquires and releases
TEST("spinlock: guard acquires and releases") {
    Spinlock s;
    {
        auto g = s.guard();
        (void)g;
        ASSERT_TRUE(s.is_locked());
    }
    ASSERT_FALSE(s.is_locked());
}

// Double release does not corrupt state (second release is benign)
TEST("spinlock: double release is benign") {
    Spinlock s;
    s.acquire();
    s.release();
    s.release();  // Should not crash
    ASSERT_FALSE(s.is_locked());
}

// ============================================================
// Wait queue helpers: enqueue / dequeue
// ============================================================

// Enqueue one task, dequeue returns it
TEST("wait_queue: enqueue one dequeue one") {
    Task* head = nullptr;
    Task  t    = make_task(1);
    enqueue_waiter(head, &t);
    ASSERT_EQ(head, &t);
    ASSERT_NULL(t.wait_next);

    Task* out = dequeue_waiter(head);
    ASSERT_EQ(out, &t);
    ASSERT_NULL(head);
    ASSERT_NULL(t.wait_next);
}

// Enqueue multiple tasks, dequeue returns FIFO order
TEST("wait_queue: FIFO ordering") {
    Task* head = nullptr;
    Task  t1   = make_task(1);
    Task  t2   = make_task(2);
    Task  t3   = make_task(3);

    enqueue_waiter(head, &t1);
    enqueue_waiter(head, &t2);
    enqueue_waiter(head, &t3);

    ASSERT_EQ(dequeue_waiter(head), &t1);
    ASSERT_EQ(dequeue_waiter(head), &t2);
    ASSERT_EQ(dequeue_waiter(head), &t3);
    ASSERT_NULL(head);
}

// Dequeue from empty queue returns nullptr
TEST("wait_queue: dequeue empty returns nullptr") {
    Task* head = nullptr;
    ASSERT_NULL(dequeue_waiter(head));
}

// Enqueue sets wait_next to nullptr
TEST("wait_queue: enqueue clears wait_next") {
    Task* head  = nullptr;
    Task  t     = make_task(1);
    t.wait_next = reinterpret_cast<Task*>(0xDEAD);  // sentinel
    enqueue_waiter(head, &t);
    ASSERT_NULL(t.wait_next);
}

// ============================================================
// Mutex: lock / unlock basic
// ============================================================

// Locking an unlocked mutex sets owner to current task
TEST("mutex: lock sets owner to current") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    m.lock();

    ASSERT_EQ(m.owner(), mock_per_cpu::current());
    ASSERT_EQ(mock_scheduler::block_count, 0);
}

// Unlocking sets owner to nullptr when no waiters
TEST("mutex: unlock clears owner when no waiters") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    m.lock();
    m.unlock();

    ASSERT_NULL(m.owner());
}

// try_lock succeeds on unlocked mutex
TEST("mutex: try_lock succeeds on free mutex") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    ASSERT_TRUE(m.try_lock());
    ASSERT_EQ(m.owner(), mock_per_cpu::current());
}

// try_lock fails on locked mutex
TEST("mutex: try_lock fails on held mutex") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    m.lock();

    // Simulate a different task trying
    mock_per_cpu::set_current(1);
    ASSERT_FALSE(m.try_lock());
}

// Locking a held mutex blocks the caller and enqueues it
TEST("mutex: lock on held mutex blocks and enqueues") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    m.lock();

    // Task 1 tries to lock -- should block
    mock_per_cpu::set_current(1);
    m.lock();

    ASSERT_EQ(mock_scheduler::block_count, 1);
    ASSERT_EQ(mock_scheduler::last_blocked, mock_per_cpu::tasks + 1);
    ASSERT_EQ(static_cast<int>(mock_per_cpu::tasks[1].state), static_cast<int>(TaskState::Blocked));
}

// Unlocking transfers ownership to the head waiter
TEST("mutex: unlock transfers ownership to waiter") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    m.lock();

    mock_per_cpu::set_current(1);
    m.lock();  // blocks

    // Owner (task 0) unlocks
    mock_per_cpu::set_current(0);
    m.unlock();

    ASSERT_EQ(m.owner(), mock_per_cpu::tasks + 1);
    ASSERT_EQ(mock_scheduler::unblock_count, 1);
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 1);
}

// FIFO ordering: multiple waiters are woken in order
TEST("mutex: FIFO ordering of waiters") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    m.lock();

    mock_per_cpu::set_current(1);
    m.lock();  // blocks

    mock_per_cpu::set_current(2);
    m.lock();  // blocks

    mock_per_cpu::set_current(3);
    m.lock();  // blocks

    ASSERT_EQ(mock_scheduler::block_count, 3);

    // First unlock: transfers to task 1
    m.unlock();
    ASSERT_EQ(m.owner(), mock_per_cpu::tasks + 1);
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 1);

    // Second unlock: transfers to task 2
    m.unlock();
    ASSERT_EQ(m.owner(), mock_per_cpu::tasks + 2);
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 2);

    // Third unlock: transfers to task 3
    m.unlock();
    ASSERT_EQ(m.owner(), mock_per_cpu::tasks + 3);
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 3);

    // No more waiters
    m.unlock();
    ASSERT_NULL(m.owner());
}

// RAII guard locks and unlocks the mutex
TEST("mutex: guard locks and unlocks") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;
    mock_per_cpu::set_current(0);
    {
        auto g = m.guard();
        (void)g;
        ASSERT_EQ(m.owner(), mock_per_cpu::current());
    }
    ASSERT_NULL(m.owner());
}

// ============================================================
// Semaphore: basic operations
// ============================================================

// Initial count is set correctly
TEST("semaphore: initial count") {
    Semaphore s(5);
    ASSERT_EQ(s.count(), 5);
}

// Default initial count is 0
TEST("semaphore: default initial count is 0") {
    Semaphore s;
    ASSERT_EQ(s.count(), 0);
}

// post() increments count
TEST("semaphore: post increments count") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(0);
    s.post();
    ASSERT_EQ(s.count(), 1);
}

// wait() decrements count when available
TEST("semaphore: wait decrements count when positive") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(3);
    mock_per_cpu::set_current(0);
    s.wait();
    ASSERT_EQ(s.count(), 2);
    ASSERT_EQ(mock_scheduler::block_count, 0);
}

// wait() blocks when count would go negative
TEST("semaphore: wait blocks when count is zero") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(0);
    mock_per_cpu::set_current(0);
    s.wait();
    ASSERT_EQ(s.count(), -1);
    ASSERT_EQ(mock_scheduler::block_count, 1);
    ASSERT_EQ(mock_scheduler::last_blocked, mock_per_cpu::current());
}

// try_wait succeeds when count is positive
TEST("semaphore: try_wait succeeds when count positive") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(2);
    ASSERT_TRUE(s.try_wait());
    ASSERT_EQ(s.count(), 1);
}

// try_wait fails when count is zero
TEST("semaphore: try_wait fails when count zero") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(0);
    ASSERT_FALSE(s.try_wait());
    ASSERT_EQ(s.count(), 0);
}

// try_wait fails when count is negative (waiters exist)
TEST("semaphore: try_wait fails when count negative") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(0);
    mock_per_cpu::set_current(0);
    s.wait();  // count -> -1, blocks task 0
    // try_wait from another task perspective
    ASSERT_FALSE(s.try_wait());
}

// post() wakes a blocked waiter when count was negative
TEST("semaphore: post wakes waiter when count negative") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(0);
    mock_per_cpu::set_current(0);
    s.wait();  // count -> -1, blocks task 0

    s.post();  // count -> 0, unblocks task 0
    ASSERT_EQ(s.count(), 0);
    ASSERT_EQ(mock_scheduler::unblock_count, 1);
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 0);
}

// Multiple waiters are woken in FIFO order
TEST("semaphore: FIFO ordering of waiters") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(0);

    mock_per_cpu::set_current(0);
    s.wait();  // count -> -1
    mock_per_cpu::set_current(1);
    s.wait();  // count -> -2
    mock_per_cpu::set_current(2);
    s.wait();  // count -> -3

    ASSERT_EQ(mock_scheduler::block_count, 3);

    // First post wakes task 0
    s.post();
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 0);

    // Second post wakes task 1
    s.post();
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 1);

    // Third post wakes task 2
    s.post();
    ASSERT_EQ(mock_scheduler::last_unblocked, mock_per_cpu::tasks + 2);

    ASSERT_EQ(s.count(), 0);
}

// Counting semaphore: multiple posts before waits
TEST("semaphore: counting semaphore pattern") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    // Simulate a buffer of size 3
    Semaphore sem_free(3);
    Semaphore sem_used(0);

    // Producer posts 3 items without blocking
    sem_free.wait();  // count 3->2
    sem_free.wait();  // count 2->1
    sem_free.wait();  // count 1->0
    ASSERT_EQ(sem_free.count(), 0);

    sem_used.post();  // count 0->1
    sem_used.post();  // count 1->2
    sem_used.post();  // count 2->3
    ASSERT_EQ(sem_used.count(), 3);

    // Consumer can consume 3 without blocking
    mock_per_cpu::set_current(0);
    sem_used.wait();  // count 3->2
    sem_used.wait();  // count 2->1
    sem_used.wait();  // count 1->0
    ASSERT_EQ(sem_used.count(), 0);
    ASSERT_EQ(mock_scheduler::block_count, 0);
}

// ============================================================
// Semaphore: boundary conditions
// ============================================================

// Semaphore with large initial count
TEST("semaphore: large initial count") {
    Semaphore s(1000000);
    ASSERT_EQ(s.count(), 1000000);
    ASSERT_TRUE(s.try_wait());
    ASSERT_EQ(s.count(), 999999);
}

// Repeated post increments count without bound
TEST("semaphore: repeated post grows count") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Semaphore s(0);
    for (int i = 0; i < 100; i++) {
        s.post();
    }
    ASSERT_EQ(s.count(), 100);
}

// ============================================================
// Mutex: state transition scenarios
// ============================================================

// Lock -> unlock -> lock -> unlock cycle
TEST("mutex: lock unlock reuse cycle") {
    mock_per_cpu::init();
    mock_scheduler::reset();

    Mutex m;

    mock_per_cpu::set_current(0);
    m.lock();
    ASSERT_EQ(m.owner(), mock_per_cpu::current());
    m.unlock();
    ASSERT_NULL(m.owner());

    // Lock again
    m.lock();
    ASSERT_EQ(m.owner(), mock_per_cpu::current());
    m.unlock();
    ASSERT_NULL(m.owner());
}

// ============================================================
// Task wait_next field
// ============================================================

// Task has wait_next field initialized to null by zero-init
TEST("task: wait_next is null after zero init") {
    Task t{};
    ASSERT_NULL(t.wait_next);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
