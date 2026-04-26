/**
 * @file kernel/test/test_kprintf_format.cpp
 * @brief QEMU in-kernel tests for kprintf length modifiers (%lu, %llu, etc.)
 *
 * Validates that the vkprintf_impl engine correctly handles long/long-long
 * format specifiers at runtime inside the kernel, complementing the
 * host-side test_kprintf unit tests.
 *
 * Preconditions: serial + kprintf initialised, heap available.
 */

#include "big_kernel_test.h"
#include "kernel/lib/kprintf.hpp"

using cinux::lib::kprintf;

// ============================================================
// Helper: capture kprintf output via a registered sink
// ============================================================

namespace {

constexpr int CAP_BUF_SIZE = 128;

char g_cap_buf[CAP_BUF_SIZE];
int  g_cap_len    = 0;
bool g_cap_active = false;

void capture_sink(char c, void* /*ctx*/) {
    if (g_cap_active && g_cap_len < CAP_BUF_SIZE - 1) {
        g_cap_buf[g_cap_len++] = c;
    }
}

/// Capture kprintf output into g_cap_buf, return as C-string.
/// Usage: const char* out = capture("format", args...);
__attribute__((format(printf, 1, 2))) const char* capture(const char* fmt, ...) {
    g_cap_len    = 0;
    g_cap_buf[0] = '\0';
    g_cap_active = true;

    va_list ap;
    va_start(ap, fmt);
    cinux::lib::kvprintf(fmt, ap);
    va_end(ap);

    g_cap_active         = false;
    g_cap_buf[g_cap_len] = '\0';
    return g_cap_buf;
}

/// Compare captured output with expected string.
bool cap_eq(const char* expected) {
    for (int i = 0;; i++) {
        if (g_cap_buf[i] != expected[i])
            return false;
        if (expected[i] == '\0')
            return true;
    }
}

}  // anonymous namespace

// ============================================================
// %lu tests
// ============================================================

void test_kprintf_lu_basic() {
    capture("%lu", 42UL);
    TEST_ASSERT(cap_eq("42"));
}

void test_kprintf_lu_max() {
    capture("%lu", 18446744073709551615UL);
    TEST_ASSERT(cap_eq("18446744073709551615"));
}

void test_kprintf_lu_zero() {
    capture("%lu", 0UL);
    TEST_ASSERT(cap_eq("0"));
}

// ============================================================
// %llu tests
// ============================================================

void test_kprintf_llu_basic() {
    capture("%llu", 12345ULL);
    TEST_ASSERT(cap_eq("12345"));
}

void test_kprintf_llu_max() {
    capture("%llu", 18446744073709551615ULL);
    TEST_ASSERT(cap_eq("18446744073709551615"));
}

// ============================================================
// %ld tests
// ============================================================

void test_kprintf_ld_positive() {
    capture("%ld", 9223372036854775807L);
    TEST_ASSERT(cap_eq("9223372036854775807"));
}

void test_kprintf_ld_negative() {
    capture("%ld", -42L);
    TEST_ASSERT(cap_eq("-42"));
}

// ============================================================
// %lld tests
// ============================================================

void test_kprintf_lld_negative() {
    capture("%lld", -1000000000000LL);
    TEST_ASSERT(cap_eq("-1000000000000"));
}

// ============================================================
// %lx / %llX tests
// ============================================================

void test_kprintf_lx() {
    capture("%lx", 0xDEADBEEFUL);
    TEST_ASSERT(cap_eq("deadbeef"));
}

void test_kprintf_llX() {
    capture("%llX", 0xCAFEBABEDEADBEEFULL);
    TEST_ASSERT(cap_eq("CAFEBABEDEADBEEF"));
}

// ============================================================
// Mixed with width/padding
// ============================================================

void test_kprintf_08lx() {
    capture("%08lx", 0xFFUL);
    TEST_ASSERT(cap_eq("000000ff"));
}

void test_kprintf_10llu() {
    capture("%10llu", 42ULL);
    TEST_ASSERT(cap_eq("        42"));
}

// ============================================================
// %u still works (no length modifier, 32-bit)
// ============================================================

void test_kprintf_u_32bit() {
    capture("%u", 4294967295u);
    TEST_ASSERT(cap_eq("4294967295"));
}

// ============================================================
// Test runner
// ============================================================

extern "C" {

void run_kprintf_format_tests() {
    TEST_SECTION("kprintf format (%lu/%llu/%ld/%lld/%lx)");

    // Register our capture sink
    cinux::lib::kprintf_register_sink(capture_sink, nullptr);

    RUN_TEST(test_kprintf_lu_basic);
    RUN_TEST(test_kprintf_lu_max);
    RUN_TEST(test_kprintf_lu_zero);
    RUN_TEST(test_kprintf_llu_basic);
    RUN_TEST(test_kprintf_llu_max);
    RUN_TEST(test_kprintf_ld_positive);
    RUN_TEST(test_kprintf_ld_negative);
    RUN_TEST(test_kprintf_lld_negative);
    RUN_TEST(test_kprintf_lx);
    RUN_TEST(test_kprintf_llX);
    RUN_TEST(test_kprintf_08lx);
    RUN_TEST(test_kprintf_10llu);
    RUN_TEST(test_kprintf_u_32bit);

    TEST_SUMMARY();
}

}  // extern "C"
