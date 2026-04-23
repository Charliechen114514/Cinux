/**
 * @file kernel/proc/sync.hpp
 * @brief Synchronization primitives for kernel threading
 *
 * Provides Mutex and Semaphore built on top of the scheduler's
 * block/unblock mechanism.  Both use an intrusive singly-linked
 * wait queue (Task::wait_next) to avoid heap allocation when
 * threads contend.
 *
 * Spinlock is a simple test-and-set spin loop used internally
 * by Mutex and Semaphore to protect their own metadata during
 * short critical sections (never held across a context switch).
 *
 * Namespace: cinux::proc
 */

#pragma once

#include <stdint.h>

namespace cinux::proc {

struct Task;

// ============================================================
// Spinlock -- busy-wait mutual exclusion
// ============================================================

/**
 * @brief Lightweight spinlock using atomic test-and-set
 *
 * Used to protect the internal state of Mutex and Semaphore.
 * Never hold a Spinlock across a blocking operation (block/yield).
 */
class Spinlock {
public:
    Spinlock() = default;

    /** Acquire the lock, spinning until available. */
    void acquire();

    /** Release the lock. */
    void release();

    /** RAII guard -- acquires on construction, releases on destruction. */
    [[nodiscard]] auto guard() {
        return Guard(this);
    }

    /** IRQ-safe RAII guard -- disables interrupts then acquires. */
    [[nodiscard]] auto irq_guard() {
        return IrqGuard(this);
    }

private:
    volatile bool locked_ = false;

    class Guard {
    public:
        explicit Guard(Spinlock* lock) : lock_(lock) {
            lock_->acquire();
        }

        ~Guard() {
            lock_->release();
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Spinlock* lock_;
    };

    class IrqGuard {
    public:
        explicit IrqGuard(Spinlock* lock);
        ~IrqGuard();

        IrqGuard(const IrqGuard&) = delete;
        IrqGuard& operator=(const IrqGuard&) = delete;

    private:
        Spinlock* lock_;
        uint64_t saved_flags_;
    };
};

// ============================================================
// Mutex -- blocking mutual exclusion
// ============================================================

/**
 * @brief Kernel mutex with blocking wait queue
 *
 * When lock() is called on an already-held mutex, the calling task
 * is placed on a FIFO wait queue and blocked.  unlock() wakes the
 * head of the wait queue.
 *
 * The lock() / unlock() methods acquire an internal Spinlock only
 * long enough to manipulate the wait queue; the Spinlock is released
 * before the task blocks, so there is no deadlock risk.
 */
class Mutex {
public:
    Mutex() = default;

    /**
     * @brief Acquire the mutex, blocking if contended
     *
     * If the mutex is free, the caller becomes the owner immediately.
     * Otherwise the caller is appended to the wait queue and blocked.
     */
    void lock();

    /**
     * @brief Release the mutex and wake one waiter
     *
     * Transfers ownership to the head of the wait queue (if any).
     * Must only be called by the current owner.
     */
    void unlock();

    /**
     * @brief Attempt to acquire the mutex without blocking
     *
     * @return true if the mutex was acquired, false if contended
     */
    bool try_lock();

    /**
     * @brief RAII guard for scoped mutex ownership
     *
     * The returned object locks the mutex on construction and
     * unlocks on destruction.  Marked [[nodiscard]] to prevent
     * accidental discard of the guard.
     */
    [[nodiscard]] auto guard() {
        return Guard(this);
    }

private:
    Spinlock spin_;
    Task* owner_ = nullptr;
    Task* wait_head_ = nullptr;

    /** Append a task to the tail of the wait queue. */
    void enqueue_waiter(Task* task);

    /** Remove and return the head of the wait queue. */
    Task* dequeue_waiter();

    class Guard {
    public:
        explicit Guard(Mutex* mtx) : mtx_(mtx) {
            mtx_->lock();
        }

        ~Guard() {
            mtx_->unlock();
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Mutex* mtx_;
    };
};

// ============================================================
// Semaphore -- counting synchronization
// ============================================================

/**
 * @brief Counting semaphore with blocking wait queue
 *
 * Classic Dijkstra semaphore:
 *   - post() (V): increments count; if a waiter exists, wakes it.
 *   - wait()  (P): decrements count; if count goes negative, blocks.
 *
 * The internal Spinlock protects count_ and the wait queue only;
 * it is released before any blocking operation.
 */
class Semaphore {
public:
    /**
     * @brief Construct a semaphore with the given initial count
     * @param initial  Starting count (typically the buffer size for
     *                 a counting semaphore, or 0 for a signalling sema)
     */
    explicit Semaphore(int64_t initial = 0);

    /**
     * @brief V operation -- increment and optionally wake a waiter
     *
     * Atomically increments the count.  If the count was negative
     * (meaning waiters exist), the head waiter is unblocked.
     */
    void post();

    /**
     * @brief P operation -- decrement, blocking if count would go negative
     *
     * Atomically decrements the count.  If the result is negative,
     * the calling task is appended to the wait queue and blocked.
     */
    void wait();

    /**
     * @brief Attempt to decrement without blocking
     *
     * @return true if the count was decremented successfully,
     *         false if the count was already zero
     */
    bool try_wait();

    /** Return the current count (for diagnostics only). */
    int64_t count() const;

private:
    Spinlock spin_;
    int64_t count_;
    Task* wait_head_ = nullptr;

    /** Append a task to the tail of the wait queue. */
    void enqueue_waiter(Task* task);

    /** Remove and return the head of the wait queue. */
    Task* dequeue_waiter();
};

// ============================================================
// InterruptGuard -- RAII interrupt disable/restore
// ============================================================

/**
 * @brief RAII wrapper for interrupt state save/restore
 *
 * Saves RFLAGS (including IF) on construction and disables interrupts
 * (cli).  On destruction, restores the saved RFLAGS via popfq, which
 * correctly handles nesting -- if interrupts were already disabled when
 * the guard was created, they remain disabled after destruction.
 */
class InterruptGuard {
public:
    InterruptGuard();
    ~InterruptGuard();

    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

private:
    uint64_t saved_flags_;
};

}  // namespace cinux::proc
