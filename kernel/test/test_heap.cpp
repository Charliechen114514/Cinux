/**
 * @file kernel/test/test_heap.cpp
 * @brief QEMU in-kernel integration tests for the Heap allocator (017)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * real Heap class (g_heap) with actual PMM/VMM backing, testing alloc,
 * free, alignment, split/coalesce, and data integrity.
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised
 *   - VMM initialised
 *   - Heap initialised (g_heap.init called)
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

#include "kernel/mm/heap.hpp"

using cinux::mm::g_heap;

// ============================================================
// Test 1: alloc returns non-null for a basic request
// ============================================================

namespace test_heap_alloc {

void test_basic_alloc() {
    void* p = g_heap.alloc(64);
    TEST_ASSERT_NOT_NULL(p);

    // Write and read back to verify the mapping works
    auto* buf = static_cast<uint8_t*>(p);
    for (int i = 0; i < 64; i++) {
        buf[i] = static_cast<uint8_t>(i);
    }
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQ(buf[i], static_cast<uint8_t>(i));
    }

    g_heap.free(p);
}

}  // namespace test_heap_alloc

// ============================================================
// Test 2: alloc returns 16-byte aligned pointer
// ============================================================

namespace test_heap_alignment {

void test_default_alignment() {
    void* p = g_heap.alloc(64);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ(reinterpret_cast<uint64_t>(p) % 16, 0u);
    g_heap.free(p);
}

void test_4096_alignment() {
    void* p = g_heap.alloc(64, 4096);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQ(reinterpret_cast<uint64_t>(p) % 4096, 0u);
    g_heap.free(p);
}

}  // namespace test_heap_alignment

// ============================================================
// Test 3: alloc(0) returns nullptr
// ============================================================

namespace test_heap_zero {

void test_zero_alloc() {
    void* p = g_heap.alloc(0);
    TEST_ASSERT(p == nullptr);
}

}  // namespace test_heap_zero

// ============================================================
// Test 4: free(nullptr) is safe
// ============================================================

namespace test_heap_free_null {

void test_free_null() {
    g_heap.free(nullptr);  // must not crash
}

}  // namespace test_heap_free_null

// ============================================================
// Test 5: multiple allocs are distinct and do not overlap
// ============================================================

namespace test_heap_multiple {

void test_multiple_distinct() {
    void* a = g_heap.alloc(64);
    void* b = g_heap.alloc(64);
    void* c = g_heap.alloc(64);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    // Must be distinct addresses
    TEST_ASSERT(a != b);
    TEST_ASSERT(b != c);
    TEST_ASSERT(a != c);

    // Write distinct patterns to each
    kmemset(a, 0xAA, 64);
    kmemset(b, 0xBB, 64);
    kmemset(c, 0xCC, 64);

    // Verify no overlap corrupted the patterns
    auto* ba = static_cast<uint8_t*>(a);
    auto* bb = static_cast<uint8_t*>(b);
    auto* bc = static_cast<uint8_t*>(c);
    for (int i = 0; i < 64; i++) {
        TEST_ASSERT_EQ(ba[i], 0xAAu);
        TEST_ASSERT_EQ(bb[i], 0xBBu);
        TEST_ASSERT_EQ(bc[i], 0xCCu);
    }

    g_heap.free(a);
    g_heap.free(b);
    g_heap.free(c);
}

}  // namespace test_heap_multiple

// ============================================================
// Test 6: coalesce — alloc 3, free all 3, then alloc large
// ============================================================

namespace test_heap_coalesce {

void test_coalesce_and_reuse() {
    g_heap.dump_stats();

    void* a = g_heap.alloc(64);
    void* b = g_heap.alloc(64);
    void* c = g_heap.alloc(64);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    // Free middle, then ends
    g_heap.free(b);
    g_heap.free(a);
    g_heap.free(c);

    // Now try a larger allocation — should succeed if coalesce worked
    void* big = g_heap.alloc(256);
    TEST_ASSERT_NOT_NULL(big);

    // Write to verify
    kmemset(big, 0xDD, 256);
    auto* bp = static_cast<uint8_t*>(big);
    for (int i = 0; i < 256; i++) {
        TEST_ASSERT_EQ(bp[i], 0xDDu);
    }

    g_heap.free(big);
}

}  // namespace test_heap_coalesce

// ============================================================
// Test 7: non-power-of-2 sizes (the alignment padding bug)
// ============================================================

namespace test_heap_odd_sizes {

void test_odd_sizes_aligned() {
    void* a = g_heap.alloc(37);
    void* b = g_heap.alloc(23);
    void* c = g_heap.alloc(41);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    // All must be 16-byte aligned
    TEST_ASSERT_EQ(reinterpret_cast<uint64_t>(a) % 16, 0u);
    TEST_ASSERT_EQ(reinterpret_cast<uint64_t>(b) % 16, 0u);
    TEST_ASSERT_EQ(reinterpret_cast<uint64_t>(c) % 16, 0u);

    // Write to verify data integrity
    kmemset(a, 0x11, 37);
    kmemset(b, 0x22, 23);
    kmemset(c, 0x33, 41);

    auto* ba = static_cast<uint8_t*>(a);
    auto* bb = static_cast<uint8_t*>(b);
    auto* bc = static_cast<uint8_t*>(c);
    for (int i = 0; i < 37; i++) TEST_ASSERT_EQ(ba[i], 0x11u);
    for (int i = 0; i < 23; i++) TEST_ASSERT_EQ(bb[i], 0x22u);
    for (int i = 0; i < 41; i++) TEST_ASSERT_EQ(bc[i], 0x33u);

    g_heap.free(a);
    g_heap.free(b);
    g_heap.free(c);
}

}  // namespace test_heap_odd_sizes

// ============================================================
// Test 8: stress — many alloc/free cycles
// ============================================================

namespace test_heap_stress {

void test_many_cycles() {
    constexpr int N = 50;
    void* ptrs[N];
    size_t sizes[N];

    // Phase 1: allocate N blocks of varying sizes
    for (int i = 0; i < N; i++) {
        sizes[i] = static_cast<size_t>((i * 7 + 16) % 256 + 1);
        ptrs[i] = g_heap.alloc(sizes[i]);
        TEST_ASSERT_NOT_NULL(ptrs[i]);

        // Write a marker
        auto* buf = static_cast<uint8_t*>(ptrs[i]);
        buf[0] = static_cast<uint8_t>(i);
        buf[sizes[i] - 1] = static_cast<uint8_t>(i ^ 0xFF);
    }

    // Phase 2: free every other one
    for (int i = 0; i < N; i += 2) {
        g_heap.free(ptrs[i]);
        ptrs[i] = nullptr;
    }

    // Phase 3: re-allocate freed slots
    for (int i = 0; i < N; i += 2) {
        ptrs[i] = g_heap.alloc(sizes[i]);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }

    // Phase 4: verify surviving markers in odd slots
    for (int i = 1; i < N; i += 2) {
        auto* buf = static_cast<uint8_t*>(ptrs[i]);
        TEST_ASSERT_EQ(buf[0], static_cast<uint8_t>(i));
        TEST_ASSERT_EQ(buf[sizes[i] - 1], static_cast<uint8_t>(i ^ 0xFF));
    }

    // Phase 5: free everything
    for (int i = 0; i < N; i++) {
        g_heap.free(ptrs[i]);
    }
}

}  // namespace test_heap_stress

// ============================================================
// Test 9: dump_stats does not crash
// ============================================================

namespace test_heap_dump {

void test_dump_no_crash() {
    g_heap.dump_stats();  // must not crash
}

}  // namespace test_heap_dump

// ============================================================
// Entry point
// ============================================================

extern "C" void run_heap_tests() {
    TEST_SECTION("Heap Tests (017)");

    RUN_TEST(test_heap_alloc::test_basic_alloc);
    RUN_TEST(test_heap_alignment::test_default_alignment);
    RUN_TEST(test_heap_alignment::test_4096_alignment);
    RUN_TEST(test_heap_zero::test_zero_alloc);
    RUN_TEST(test_heap_free_null::test_free_null);
    RUN_TEST(test_heap_multiple::test_multiple_distinct);
    RUN_TEST(test_heap_coalesce::test_coalesce_and_reuse);
    RUN_TEST(test_heap_odd_sizes::test_odd_sizes_aligned);
    RUN_TEST(test_heap_stress::test_many_cycles);
    RUN_TEST(test_heap_dump::test_dump_no_crash);

    TEST_SUMMARY();
}

// ============================================================
// Test 10: sync safety — repeated alloc/free cycles (lock stress)
// ============================================================

namespace test_heap_lock_stress {

void test_lock_stress_cycles() {
    constexpr int N = 30;
    void* ptrs[N];
    size_t sizes[N];

    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < N; i++) {
            sizes[i] = static_cast<size_t>((i * 11 + 16 + round * 3) % 256 + 1);
            ptrs[i] = g_heap.alloc(sizes[i]);
            TEST_ASSERT_NOT_NULL(ptrs[i]);

            auto* buf = static_cast<uint8_t*>(ptrs[i]);
            buf[0] = static_cast<uint8_t>(i + round);
            buf[sizes[i] - 1] = static_cast<uint8_t>((i + round) ^ 0xFF);
        }

        for (int i = 0; i < N; i++) {
            g_heap.free(ptrs[i]);
        }
    }
}

}  // namespace test_heap_lock_stress

extern "C" void run_heap_lock_stress_tests() {
    TEST_SECTION("Heap Lock Stress Tests (028d)");

    RUN_TEST(test_heap_lock_stress::test_lock_stress_cycles);

    TEST_SUMMARY();
}
