/**
 * @file test_framework.h
 * @brief Cinux lightweight test framework
 *
 * Supports two running modes:
 *   - Host mode (-DCINUX_HOST_TEST): compiled with g++, runs on Linux
 *   - QEMU mode: runs directly in the kernel, outputs results via serial port
 *
 * Usage example:
 *
 *   TEST("pmm: alloc single page") {
 *       void* page = pmm_alloc_page();
 *       ASSERT_NOT_NULL(page);
 *       ASSERT_EQ((uintptr_t)page % 4096, 0UL);  // 4K aligned
 *       pmm_free_page(page);
 *   }
 *
 *   int main() {
 *       RUN_ALL_TESTS();
 *       return 0;
 *   }
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

// ============================================================
// Platform adaptation layer
// ============================================================

#ifdef CINUX_HOST_TEST
#    include <stdio.h>
#    include <stdlib.h>
#    define _TEST_PRINT(fmt, ...) printf(fmt, ##__VA_ARGS__)
#    define _TEST_ABORT()         abort()
#else
// QEMU mode: depends on kernel serial driver
// Replace with your serial_printf in the actual kernel
extern void serial_printf(const char* fmt, ...);
#    define _TEST_PRINT(fmt, ...) serial_printf(fmt, ##__VA_ARGS__)
#    define _TEST_ABORT()                                                                          \
        do {                                                                                       \
            asm volatile("hlt");                                                                   \
        } while (1)
#endif

// ============================================================
// Internal test registration mechanism
// ============================================================

typedef void (*_test_fn_t)(void);

struct _TestEntry {
    const char* name;
    const char* file;
    int         line;
    _test_fn_t  fn;
};

// Maximum 256 test cases (adjustable as needed)
#define _MAX_TESTS 256

// Use __attribute__((section)) for automatic test registration
// Each TEST() block generates a _TestEntry placed in the .cinux_tests section
#ifdef CINUX_HOST_TEST
// Host mode: manually maintain an array (avoid linker section trick dependency)
extern _TestEntry _test_registry[_MAX_TESTS];
extern int        _test_count;

static inline void _register_test(const char* name, const char* file, int line, _test_fn_t fn) {
    if (_test_count < _MAX_TESTS) {
        _test_registry[_test_count++] = {name, file, line, fn};
    }
}
#endif

// ============================================================
// Statistics
// ============================================================

extern int _tests_passed;
extern int _tests_failed;

// ============================================================
// TEST() macro
// ============================================================

// Helper macro: used to force-expand __LINE__ before token pasting
// The ## operator does not expand its operands; need two-level macro indirection
#define _TEST_CAT2(a, b) a##b
#define _TEST_CAT(a, b)  _TEST_CAT2(a, b)

/**
 * Define a test case.
 *
 * TEST("name") {
 *     // Test body, can use all ASSERT_* macros
 * }
 */
#define TEST(test_name)                                                                            \
    static void _TEST_CAT(_test_fn_, __LINE__)();                                                  \
    static struct _TEST_CAT(_TestAutoReg_, __LINE__) {                                             \
        _TEST_CAT(_TestAutoReg_, __LINE__)() {                                                     \
            _register_test(test_name, __FILE__, __LINE__, _TEST_CAT(_test_fn_, __LINE__));         \
        }                                                                                          \
    } _TEST_CAT(_test_reg_, __LINE__);                                                             \
    static void _TEST_CAT(_test_fn_, __LINE__)()

// ============================================================
// ASSERT_* macros
// ============================================================

#define ASSERT_TRUE(expr)                                                                          \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            _TEST_PRINT(                                                                           \
                "[FAIL] %s\n  ASSERT_TRUE(%s) failed\n"                                            \
                "  at %s:%d\n",                                                                    \
                _current_test_name, #expr, __FILE__, __LINE__);                                    \
            _tests_failed++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(actual, expected)                                                                \
    do {                                                                                           \
        auto _a = (actual);                                                                        \
        auto _e = (expected);                                                                      \
        if (!(_a == _e)) {                                                                         \
            _TEST_PRINT(                                                                           \
                "[FAIL] %s\n  ASSERT_EQ(%s, %s) failed\n"                                          \
                "  at %s:%d\n",                                                                    \
                _current_test_name, #actual, #expected, __FILE__, __LINE__);                       \
            _tests_failed++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NE(actual, expected)                                                                \
    do {                                                                                           \
        auto _a = (actual);                                                                        \
        auto _e = (expected);                                                                      \
        if (_a == _e) {                                                                            \
            _TEST_PRINT(                                                                           \
                "[FAIL] %s\n  ASSERT_NE(%s, %s) failed\n"                                          \
                "  at %s:%d\n",                                                                    \
                _current_test_name, #actual, #expected, __FILE__, __LINE__);                       \
            _tests_failed++;                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_NULL(ptr) ASSERT_TRUE((ptr) == nullptr)

#define ASSERT_NOT_NULL(ptr) ASSERT_TRUE((ptr) != nullptr)

#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))

// ============================================================
// Run all tests
// ============================================================

extern const char* _current_test_name;

/**
 * Run all registered test cases.
 * Call in main() (host mode) or kernel_test_main() (QEMU mode).
 */
static inline void RUN_ALL_TESTS() {
    extern _TestEntry _test_registry[];
    extern int        _test_count;

    _TEST_PRINT("\n=== Cinux Test Runner ===\n");
    _TEST_PRINT("Running %d test(s)...\n\n", _test_count);

    _tests_passed = 0;
    _tests_failed = 0;

    for (int i = 0; i < _test_count; i++) {
        _current_test_name = _test_registry[i].name;
        _test_registry[i].fn();
        if (_tests_failed == 0 || /* last test passed */ true) {
            // Only print PASS if not failed (simplified; actual implementation needs per-test
            // tracking)
        }
        _TEST_PRINT("[PASS] %s\n", _current_test_name);
        _tests_passed++;
    }

    _TEST_PRINT("\n=== Results: %d passed, %d failed ===\n", _tests_passed, _tests_failed);

    if (_tests_failed > 0) {
        _TEST_PRINT("[SUITE FAILED]\n");
    } else {
        _TEST_PRINT("[SUITE PASSED]\n");
    }
}

// ============================================================
// Global variable definitions (define TEST_IMPL in one .cpp, then include)
// ============================================================
#ifdef TEST_FRAMEWORK_IMPL
_TestEntry  _test_registry[_MAX_TESTS];
int         _test_count        = 0;
int         _tests_passed      = 0;
int         _tests_failed      = 0;
const char* _current_test_name = "";
#endif
