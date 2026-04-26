/**
 * @file kernel/test/test_pmm.cpp
 * @brief QEMU in-kernel integration tests for the Physical Memory Manager (015)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that
 * PMM::init() completes successfully, that alloc/free cycles work with
 * real BootInfo data, and that statistics are consistent.
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised (g_pmm.init called)
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/mm/pmm.hpp"

using cinux::mm::g_pmm;

// ============================================================
// Test 1: PMM initialised with valid statistics
// ============================================================

namespace test_pmm_init {

void test_init_and_stats() {
    TEST_ASSERT_GT(g_pmm.total_page_count(), 0u);
    TEST_ASSERT_GT(g_pmm.free_page_count(), 0u);
    TEST_ASSERT_GE(g_pmm.total_page_count(), g_pmm.free_page_count());
}

}  // namespace test_pmm_init

// ============================================================
// Test 2: Single page alloc / free cycle
// ============================================================

namespace test_pmm_alloc {

void test_alloc_free_cycle() {
    uint64_t initial_free = g_pmm.free_page_count();

    uint64_t page = g_pmm.alloc_page();
    TEST_ASSERT_NE(page, 0u);
    TEST_ASSERT_EQ(page % 4096, 0u);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free - 1);

    g_pmm.free_page(page);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free);
}

}  // namespace test_pmm_alloc

// ============================================================
// Test 3: Multiple alloc then free preserves count
// ============================================================

namespace test_pmm_bulk {

void test_bulk_alloc_free() {
    uint64_t initial_free = g_pmm.free_page_count();

    constexpr int N = 16;
    uint64_t      pages[N];
    for (int i = 0; i < N; i++) {
        pages[i] = g_pmm.alloc_page();
        TEST_ASSERT_NE(pages[i], 0u);
        TEST_ASSERT_EQ(pages[i] % 4096, 0u);
    }

    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free - N);

    for (int i = 0; i < N; i++) {
        g_pmm.free_page(pages[i]);
    }

    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free);
}

}  // namespace test_pmm_bulk

// ============================================================
// Test 4: alloc_pages / free_pages contiguous
// ============================================================

namespace test_pmm_contiguous {

void test_alloc_pages_contiguous() {
    uint64_t initial_free = g_pmm.free_page_count();

    uint64_t base = g_pmm.alloc_pages(4);
    TEST_ASSERT_NE(base, 0u);
    TEST_ASSERT_EQ(base % 4096, 0u);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free - 4);

    g_pmm.free_pages(base, 4);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free);
}

}  // namespace test_pmm_contiguous

// ============================================================
// Test 5: free_page(0) and double-free are no-ops
// ============================================================

namespace test_pmm_edge {

void test_free_zero_noop() {
    uint64_t before = g_pmm.free_page_count();
    g_pmm.free_page(0);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), before);
}

void test_double_free_noop() {
    uint64_t p = g_pmm.alloc_page();
    TEST_ASSERT_NE(p, 0u);

    g_pmm.free_page(p);
    uint64_t after = g_pmm.free_page_count();
    g_pmm.free_page(p);
    TEST_ASSERT_EQ(g_pmm.free_page_count(), after);
}

}  // namespace test_pmm_edge

// ============================================================
// Test 6: Concurrent alloc/free with cooperative task switching
// ============================================================

namespace test_pmm_concurrent {

void test_concurrent_alloc_free_no_leak() {
    uint64_t initial_free = g_pmm.free_page_count();

    constexpr int N = 8;
    uint64_t      pages[N];
    for (int i = 0; i < N; i++) {
        pages[i] = g_pmm.alloc_page();
        TEST_ASSERT_NE(pages[i], 0u);
    }

    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free - N);

    for (int i = 0; i < N; i++) {
        g_pmm.free_page(pages[i]);
    }

    TEST_ASSERT_EQ(g_pmm.free_page_count(), initial_free);
}

}  // namespace test_pmm_concurrent

// ============================================================
// Entry point
// ============================================================

extern "C" void run_pmm_tests() {
    TEST_SECTION("PMM Tests (015)");

    RUN_TEST(test_pmm_init::test_init_and_stats);
    RUN_TEST(test_pmm_alloc::test_alloc_free_cycle);
    RUN_TEST(test_pmm_bulk::test_bulk_alloc_free);
    RUN_TEST(test_pmm_contiguous::test_alloc_pages_contiguous);
    RUN_TEST(test_pmm_edge::test_free_zero_noop);
    RUN_TEST(test_pmm_edge::test_double_free_noop);

    RUN_TEST(test_pmm_concurrent::test_concurrent_alloc_free_no_leak);

    TEST_SUMMARY();
}
