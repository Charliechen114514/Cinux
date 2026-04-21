/**
 * @file kernel/proc/sync.cpp
 * @brief Implementation of synchronization primitives (Spinlock, Mutex, Semaphore)
 */

#include "kernel/proc/sync.hpp"

#include <stdint.h>

#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/per_cpu.hpp"

namespace cinux::proc {

// ============================================================
// Spinlock implementation
// ============================================================

void Spinlock::acquire() {
    while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

void Spinlock::release() {
    __atomic_clear(&locked_, __ATOMIC_RELEASE);
}

// ============================================================
// Mutex implementation
// ============================================================

void Mutex::enqueue_waiter(Task* task) {
    task->wait_next = nullptr;

    if (wait_head_ == nullptr) {
        wait_head_ = task;
        return;
    }

    Task* tail = wait_head_;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}

Task* Mutex::dequeue_waiter() {
    if (wait_head_ == nullptr) {
        return nullptr;
    }

    Task* task = wait_head_;
    wait_head_ = task->wait_next;
    task->wait_next = nullptr;
    return task;
}

void Mutex::lock() {
    // Step 1: Acquire the internal spinlock to examine / modify state
    spin_.acquire();

    // Step 2: If the mutex is free, take ownership and return
    if (owner_ == nullptr) {
        owner_ = g_per_cpu.current;
        spin_.release();
        return;
    }

    // Step 3: Mutex is contended -- put the current task on the wait queue
    Task* self = g_per_cpu.current;
    enqueue_waiter(self);

    // Step 4: Release the spinlock BEFORE blocking (avoids deadlock)
    spin_.release();

    // Step 5: Block the current task; schedule() will pick another
    Scheduler::block(self, "mutex");
}

void Mutex::unlock() {
    // Step 1: Acquire the internal spinlock
    spin_.acquire();

    // Step 2: If there is no waiter, simply release the mutex
    Task* waiter = dequeue_waiter();
    if (waiter == nullptr) {
        owner_ = nullptr;
        spin_.release();
        return;
    }

    // Step 3: Transfer ownership to the head waiter
    owner_ = waiter;

    // Step 4: Release the spinlock before unblocking
    spin_.release();

    // Step 5: Wake the new owner
    Scheduler::unblock(waiter);
}

bool Mutex::try_lock() {
    spin_.acquire();

    if (owner_ != nullptr) {
        spin_.release();
        return false;
    }

    owner_ = g_per_cpu.current;
    spin_.release();
    return true;
}

// ============================================================
// Semaphore implementation
// ============================================================

Semaphore::Semaphore(int64_t initial)
    : count_(initial), wait_head_(nullptr) {
}

void Semaphore::enqueue_waiter(Task* task) {
    task->wait_next = nullptr;

    if (wait_head_ == nullptr) {
        wait_head_ = task;
        return;
    }

    Task* tail = wait_head_;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}

Task* Semaphore::dequeue_waiter() {
    if (wait_head_ == nullptr) {
        return nullptr;
    }

    Task* task = wait_head_;
    wait_head_ = task->wait_next;
    task->wait_next = nullptr;
    return task;
}

void Semaphore::post() {
    // Step 1: Acquire the internal spinlock
    spin_.acquire();

    // Step 2: Increment the count
    count_++;

    // Step 3: If there are waiters, wake the head
    Task* waiter = dequeue_waiter();

    // Step 4: Release spinlock before unblocking
    spin_.release();

    // Step 5: Unblock the waiter (if any)
    if (waiter != nullptr) {
        Scheduler::unblock(waiter);
    }
}

void Semaphore::wait() {
    // Step 1: Acquire the internal spinlock
    spin_.acquire();

    // Step 2: Decrement the count
    count_--;

    // Step 3: If the count is still >= 0, the resource is available
    if (count_ >= 0) {
        spin_.release();
        return;
    }

    // Step 4: No resource available -- enqueue the current task
    Task* self = g_per_cpu.current;
    enqueue_waiter(self);

    // Step 5: Release the spinlock before blocking
    spin_.release();

    // Step 6: Block the current task
    Scheduler::block(self, "semaphore");
}

bool Semaphore::try_wait() {
    spin_.acquire();

    if (count_ <= 0) {
        spin_.release();
        return false;
    }

    count_--;
    spin_.release();
    return true;
}

int64_t Semaphore::count() const {
    return count_;
}

}  // namespace cinux::proc
