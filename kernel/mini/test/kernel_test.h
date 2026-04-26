/* ==============================================================
 * Cinux Mini Kernel - Test Framework
 * ==============================================================
 *
 * Lightweight kernel-mode test framework for Cinux.
 * Provides assertion macros, test runners, and statistics tracking.
 *
 * Usage:
 *   1. Include this header in your test file
 *   2. Use TEST_ASSERT_* macros for assertions
 *   3. Use RUN_TEST(fn) to execute test functions
 *   4. Call TEST_SUMMARY() to print results
 *
 * Example:
 *   void my_test() {
 *       TEST_ASSERT_EQ(2 + 2, 4);
 *   }
 *
 *   extern "C" void run_my_tests() {
 *       RUN_TEST(my_test);
 *       TEST_SUMMARY();
 *   }
 */

#pragma once

#include "../lib/kprintf.h"

using cinux::mini::lib::kprintf;

// ============================================================
// Test State Management
// ============================================================
namespace test {
inline int tests_passed = 0;
inline int tests_failed = 0;

// Reset test counters (useful for test isolation)
inline void reset() {
    tests_passed = 0;
    tests_failed = 0;
}

// Get total tests run
inline int total() {
    return tests_passed + tests_failed;
}

// Check if all tests passed
inline bool all_passed() {
    return tests_failed == 0;
}

// Get total failed tests
inline int get_total_failed() {
    return tests_failed;
}
}  // namespace test

// ============================================================
// Assertion Macros
// ============================================================

// Basic assertion - fails if condition is false
#define TEST_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            kprintf("[FAIL] %s at %s:%d\n", #cond, __FILE__, __LINE__);                            \
            test::tests_failed++;                                                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)

// Equality assertion
#define TEST_ASSERT_EQ(a, b) TEST_ASSERT((a) == (b))

// Inequality assertion
#define TEST_ASSERT_NE(a, b) TEST_ASSERT((a) != (b))

// Greater than assertion
#define TEST_ASSERT_GT(a, b) TEST_ASSERT((a) > (b))

// Greater than or equal assertion
#define TEST_ASSERT_GE(a, b) TEST_ASSERT((a) >= (b))

// Less than assertion
#define TEST_ASSERT_LT(a, b) TEST_ASSERT((a) < (b))

// Less than or equal assertion
#define TEST_ASSERT_LE(a, b) TEST_ASSERT((a) <= (b))

// Null pointer assertion
#define TEST_ASSERT_NULL(ptr) TEST_ASSERT((ptr) == nullptr)

// Non-null pointer assertion
#define TEST_ASSERT_NOT_NULL(ptr) TEST_ASSERT((ptr) != nullptr)

// True assertion (for boolean values)
#define TEST_ASSERT_TRUE(expr) TEST_ASSERT((expr) == true)

// False assertion
#define TEST_ASSERT_FALSE(expr) TEST_ASSERT((expr) == false)

// ============================================================
// Test Runner Macros
// ============================================================

// Run a single test function
#define RUN_TEST(fn)                                                                               \
    do {                                                                                           \
        kprintf("[RUN] %s\n", #fn);                                                                \
        int _failed_before = test::tests_failed;                                                   \
        fn();                                                                                      \
        if (test::tests_failed == _failed_before) {                                                \
            test::tests_passed++;                                                                  \
            kprintf("[PASS] %s\n", #fn);                                                           \
        }                                                                                          \
    } while (0)

// Print test summary
#define TEST_SUMMARY()                                                                             \
    do {                                                                                           \
        kprintf("\n=== Tests: %d passed, %d failed ===\n", test::tests_passed,                     \
                test::tests_failed);                                                               \
    } while (0)

// Print test section header
#define TEST_SECTION(name)                                                                         \
    do {                                                                                           \
        kprintf("\n=== %s ===\n", name);                                                           \
    } while (0)
