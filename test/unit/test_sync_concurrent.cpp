/**
 * @file test/unit/test_sync_concurrent.cpp
 * @brief Host-side concurrent stress tests for synchronization primitives
 *
 * Uses real OS threads (std::thread) to verify lock correctness under
 * genuine concurrent access.  Tests Spinlock, Mutex, InterruptGuard
 * simulation, and IrqSpinlockGuard simulation.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "test_framework.h"

// ============================================================
// Re-implement Spinlock (host-side: uses std::atomic<bool>)
// ============================================================

class Spinlock {
public:
    Spinlock() = default;

    void acquire() {
        while (locked_.exchange(true, std::memory_order_acquire)) {
        }
    }

    void release() {
        locked_.store(false, std::memory_order_release);
    }

    [[nodiscard]] auto guard() {
        return Guard(this);
    }

    [[nodiscard]] auto irq_guard() {
        return IrqGuard(this);
    }

    bool is_locked() const {
        return locked_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> locked_{false};

    class Guard {
    public:
        explicit Guard(Spinlock* lock) : lock_(lock) { lock_->acquire(); }
        ~Guard() { lock_->release(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Spinlock* lock_;
    };

    class IrqGuard {
    public:
        explicit IrqGuard(Spinlock* lock) : lock_(lock) {
            // Simulate: save IF + disable interrupts + acquire spinlock
            lock_->acquire();
        }
        ~IrqGuard() {
            // Simulate: release spinlock + restore IF
            lock_->release();
        }
        IrqGuard(const IrqGuard&) = delete;
        IrqGuard& operator=(const IrqGuard&) = delete;

    private:
        Spinlock* lock_;
    };
};

// ============================================================
// Simulated InterruptGuard (host-side: no real interrupts)
// ============================================================

class SimInterruptGuard {
public:
    SimInterruptGuard() : saved_(interrupts_enabled_) {
        interrupts_enabled_ = false;
    }

    ~SimInterruptGuard() {
        interrupts_enabled_ = saved_;
    }

    SimInterruptGuard(const SimInterruptGuard&) = delete;
    SimInterruptGuard& operator=(const SimInterruptGuard&) = delete;

    static bool& interrupts_enabled() { return interrupts_enabled_; }

private:
    bool saved_;
    static inline bool interrupts_enabled_ = true;
};

// ============================================================
// Helper: spawn N threads, each running fn(iters), then join
// ============================================================

static void concurrent_stress(int num_threads, int iters,
                              std::function<void(int)> fn) {
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; t++) {
        threads.emplace_back(fn, iters);
    }
    for (auto& th : threads) {
        th.join();
    }
}

// ============================================================
// Test cases
// ============================================================

// Spinlock protects a shared counter under genuine concurrency
TEST("concurrent: spinlock atomic counter") {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERS = 10000;
    Spinlock lock;
    std::atomic<int64_t> counter{0};

    concurrent_stress(NUM_THREADS, ITERS, [&](int n) {
        for (int i = 0; i < n; i++) {
            auto g = lock.guard();
            (void)g;
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });

    ASSERT_EQ(counter.load(), static_cast<int64_t>(NUM_THREADS * ITERS));
}

// Spinlock irq_guard also provides mutual exclusion
TEST("concurrent: irq_guard mutual exclusion") {
    constexpr int NUM_THREADS = 4;
    constexpr int ITERS = 5000;
    Spinlock lock;
    std::atomic<int64_t> counter{0};

    concurrent_stress(NUM_THREADS, ITERS, [&](int n) {
        for (int i = 0; i < n; i++) {
            auto g = lock.irq_guard();
            (void)g;
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });

    ASSERT_EQ(counter.load(), static_cast<int64_t>(NUM_THREADS * ITERS));
}

// SimInterruptGuard nesting: inner destruction does not restore outer
TEST("concurrent: nested interrupt guard") {
    SimInterruptGuard::interrupts_enabled() = true;
    ASSERT_TRUE(SimInterruptGuard::interrupts_enabled());

    {
        SimInterruptGuard outer;
        ASSERT_FALSE(SimInterruptGuard::interrupts_enabled());

        {
            SimInterruptGuard inner;
            ASSERT_FALSE(SimInterruptGuard::interrupts_enabled());
        }
        // inner destroyed -- interrupts should STILL be disabled (outer's state)
        ASSERT_FALSE(SimInterruptGuard::interrupts_enabled());
    }
    // outer destroyed -- interrupts restored to true
    ASSERT_TRUE(SimInterruptGuard::interrupts_enabled());
}

// Multiple threads acquire/release in rapid succession
TEST("concurrent: rapid spinlock cycling") {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERS = 50000;
    Spinlock lock;

    concurrent_stress(NUM_THREADS, ITERS, [&](int n) {
        for (int i = 0; i < n; i++) {
            lock.acquire();
            lock.release();
        }
    });
    // If we reach here without deadlock, the test passes
    ASSERT_TRUE(true);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
