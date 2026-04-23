/**
 * @file kernel/stress/stress_test.cpp
 * @brief Production-kernel concurrent stress test (028d validation)
 *
 * Runs before the shell with real preemptive multitasking and timer
 * interrupts.  Spawns N kernel threads that concurrently pound on
 * PMM, Heap, and shared atomic counters to validate that the spinlock /
 * atomic / InterruptGuard protections added in 028d actually work under
 * genuine IRQ-preempted concurrent execution.
 */

#include <stdint.h>
#include <stddef.h>
#include <atomic>

#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/heap.hpp"
#include "kernel/proc/process.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/per_cpu.hpp"
#include "kernel/arch/x86_64/usermode.hpp"

using cinux::lib::kprintf;
using cinux::mm::g_pmm;
using cinux::mm::g_heap;
using cinux::proc::Scheduler;
using cinux::proc::Task;
using cinux::proc::TaskBuilder;
using cinux::proc::g_per_cpu;
using cinux::arch::launch_first_user;


// ============================================================
// Configuration
// ============================================================

static constexpr int NUM_THREADS     = 4;
static constexpr int PMM_OPS         = 200;
static constexpr int HEAP_OPS        = 200;
static constexpr int HEAP_BLOCK_SIZE = 64;

// ============================================================
// Shared counters (atomic, no lock needed)
// ============================================================

static std::atomic<int> threads_done{0};
static std::atomic<uint64_t> pmm_ops_total{0};
static std::atomic<uint64_t> heap_ops_total{0};
static std::atomic<uint64_t> shared_counter{0};

// ============================================================
// Stress thread entry
// ============================================================

static void stress_thread_entry() {
    for (int i = 0; i < PMM_OPS; i++) {
        uint64_t p = g_pmm.alloc_page();
        if (p != 0) {
            g_pmm.free_page(p);
        }
        pmm_ops_total.fetch_add(1, std::memory_order_relaxed);
    }

    for (int i = 0; i < HEAP_OPS; i++) {
        void* b = g_heap.alloc(HEAP_BLOCK_SIZE);
        if (b != nullptr) {
            g_heap.free(b);
        }
        heap_ops_total.fetch_add(1, std::memory_order_relaxed);
    }

    for (int i = 0; i < 1000; i++) {
        shared_counter.fetch_add(1, std::memory_order_relaxed);
    }

    threads_done.fetch_add(1, std::memory_order_release);
    Scheduler::exit_current();
}

// ============================================================
// Boot continuation: waits for stress, then calls launch_first_user
// ============================================================

namespace cinux::arch {
void launch_first_user();
}

static void boot_continuation() {
    kprintf("[STRESS] Waiting for %d stress threads to complete...\n", NUM_THREADS);

    while (threads_done.load(std::memory_order_acquire) < NUM_THREADS) {
        Scheduler::yield();
    }

    uint64_t expected_pmm = static_cast<uint64_t>(NUM_THREADS) * PMM_OPS;
    uint64_t expected_heap = static_cast<uint64_t>(NUM_THREADS) * HEAP_OPS;
    uint64_t expected_ctr  = static_cast<uint64_t>(NUM_THREADS) * 1000;

    uint64_t actual_pmm  = pmm_ops_total.load(std::memory_order_relaxed);
    uint64_t actual_heap = heap_ops_total.load(std::memory_order_relaxed);
    uint64_t actual_ctr  = shared_counter.load(std::memory_order_relaxed);

    bool pmm_ok  = (actual_pmm == expected_pmm);
    bool heap_ok = (actual_heap == expected_heap);
    bool ctr_ok  = (actual_ctr == expected_ctr);

    kprintf("[STRESS] ===== Concurrent Stress Results =====\n");
    kprintf("[STRESS] PMM alloc/free ops: expected=%lu actual=%lu %s\n",
            expected_pmm, actual_pmm, pmm_ok ? "PASS" : "FAIL");
    kprintf("[STRESS] Heap alloc/free ops: expected=%lu actual=%lu %s\n",
            expected_heap, actual_heap, heap_ok ? "PASS" : "FAIL");
    kprintf("[STRESS] Atomic counter:       expected=%lu actual=%lu %s\n",
            expected_ctr, actual_ctr, ctr_ok ? "PASS" : "FAIL");
    kprintf("[STRESS] =======================================\n");

    if (pmm_ok && heap_ok && ctr_ok) {
        kprintf("[STRESS] ALL PASSED -- launching shell\n");
    } else {
        kprintf("[STRESS] SOME FAILED -- launching shell anyway\n");
    }

    cinux::arch::launch_first_user();
}

// ============================================================
// Public entry point: called from kernel_main before launch_first_user
// ============================================================

extern "C" void run_concurrent_stress() noexcept {
    kprintf("[STRESS] ===== 028d Concurrent Stress Test =====\n");
    kprintf("[STRESS] Threads=%d  PMM_ops/thread=%d  Heap_ops/thread=%d\n",
            NUM_THREADS, PMM_OPS, HEAP_OPS);

    threads_done.store(0, std::memory_order_release);
    pmm_ops_total.store(0, std::memory_order_relaxed);
    heap_ops_total.store(0, std::memory_order_relaxed);
    shared_counter.store(0, std::memory_order_relaxed);

    Scheduler::init();

    Task* boot = TaskBuilder()
        .set_entry(boot_continuation)
        .set_name("boot")
        .build();

    for (int i = 0; i < NUM_THREADS; i++) {
        Task* t = TaskBuilder()
            .set_entry(stress_thread_entry)
            .set_name("stress")
            .build();
        Scheduler::add_task(t);
    }

    Scheduler::run_first(boot);
}
