/**
 * @file tests/unit/test_kprintf.cpp
 * @brief Host-side unit tests for the big kernel kprintf formatting engine
 *
 * Compile condition: -DCINUX_HOST_TEST
 *
 * Strategy:
 *   We include the real vkprintf_impl template from the big kernel
 *   (kernel/lib/private/vkprintf_impl.hpp), which is hardware-independent.
 *   A mock OutputFn captures characters into a std::string so we can
 *   assert exact output.
 *
 * Test coverage (per milestone 012 checklist):
 *   - %d  signed decimal (positive, negative, zero)
 *   - %u  unsigned decimal
 *   - %x  lowercase hex
 *   - %X  uppercase hex
 *   - %s  string (normal, nullptr, empty, width, left-align)
 *   - %p  pointer with 0x + 16-digit hex
 *   - %%  literal percent
 *   - %c  character
 *   - Width modifiers: %08x, %4d, %-10s, %-10d
 *   - Negative number formatting with width and zero-pad
 *   - Mixed format strings
 *   - Unknown specifier fallback
 *   - Edge cases
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <string>
#include <stdarg.h>

// Include the big kernel formatting engine
#include "lib/private/vkprintf_impl.hpp"

using namespace cinux::lib::detail;

// ============================================================
// Mock output capture
// ============================================================

/**
 * @brief Captures formatted output into a std::string
 *
 * Simulates Serial::putc so we can assert the exact output
 * of vkprintf_impl without any hardware dependency.
 */
class MockOutput {
public:
    void putc(char c) { buffer_.push_back(c); }

    std::string result() const { return buffer_; }
    void        clear() { buffer_.clear(); }

private:
    std::string buffer_;
};

/**
 * @brief Thin wrapper that calls vkprintf_impl with our mock
 */
static std::string do_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    MockOutput mock;
    vkprintf_impl([&](char c) { mock.putc(c); }, fmt, args);

    va_end(args);
    return mock.result();
}

// ============================================================
// %d signed decimal tests
// ============================================================

TEST("kprintf: %d positive") {
    ASSERT_EQ(do_printf("%d", 42), "42");
}

TEST("kprintf: %d negative") {
    ASSERT_EQ(do_printf("%d", -99), "-99");
}

TEST("kprintf: %d zero") {
    ASSERT_EQ(do_printf("%d", 0), "0");
}

TEST("kprintf: %d large positive") {
    ASSERT_EQ(do_printf("%d", 123456), "123456");
}

TEST("kprintf: %d large negative") {
    ASSERT_EQ(do_printf("%d", -123456), "-123456");
}

// ============================================================
// %u unsigned decimal tests
// ============================================================

TEST("kprintf: %u small") {
    ASSERT_EQ(do_printf("%u", 42u), "42");
}

TEST("kprintf: %u large") {
    ASSERT_EQ(do_printf("%u", 4000000000u), "4000000000");
}

TEST("kprintf: %u zero") {
    ASSERT_EQ(do_printf("%u", 0u), "0");
}

// ============================================================
// %x lowercase hex tests
// ============================================================

TEST("kprintf: %x lowercase") {
    ASSERT_EQ(do_printf("0x%x", 0xDEAD), "0xdead");
}

TEST("kprintf: %x zero") {
    ASSERT_EQ(do_printf("%x", 0), "0");
}

TEST("kprintf: %x single digit") {
    ASSERT_EQ(do_printf("%x", 0xA), "a");
}

TEST("kprintf: %x all digits") {
    ASSERT_EQ(do_printf("%x", 0x123456789ABCDEF0ULL), "123456789abcdef0");
}

// ============================================================
// %X uppercase hex tests
// ============================================================

TEST("kprintf: %X uppercase") {
    ASSERT_EQ(do_printf("0x%X", 0xBEEF), "0xBEEF");
}

TEST("kprintf: %X zero") {
    ASSERT_EQ(do_printf("%X", 0), "0");
}

// ============================================================
// %s string tests
// ============================================================

TEST("kprintf: %s normal") {
    ASSERT_EQ(do_printf("hello %s!", "world"), "hello world!");
}

TEST("kprintf: %s nullptr") {
    ASSERT_EQ(do_printf("%s", nullptr), "(null)");
}

TEST("kprintf: %s empty") {
    ASSERT_EQ(do_printf("[%s]", ""), "[]");
}

TEST("kprintf: %s with width right-align") {
    ASSERT_EQ(do_printf("[%10s]", "hi"), "[        hi]");
}

TEST("kprintf: %s with width left-align") {
    ASSERT_EQ(do_printf("[%-10s]", "hi"), "[hi        ]");
}

TEST("kprintf: %s with width exact fit") {
    ASSERT_EQ(do_printf("[%5s]", "hello"), "[hello]");
}

// ============================================================
// %p pointer tests (0x + 16-digit hex)
// ============================================================

TEST("kprintf: %p typical address") {
    ASSERT_EQ(do_printf("%p", 0x1000000ULL), "0x0000000001000000");
}

TEST("kprintf: %p zero address") {
    ASSERT_EQ(do_printf("%p", 0ULL), "0x0000000000000000");
}

TEST("kprintf: %p max 64-bit") {
    ASSERT_EQ(do_printf("%p", 0xFFFFFFFFFFFFFFFFULL),
              "0xFFFFFFFFFFFFFFFF");
}

TEST("kprintf: %p small value") {
    ASSERT_EQ(do_printf("%p", 0xFFULL), "0x00000000000000FF");
}

// ============================================================
// %c character tests
// ============================================================

TEST("kprintf: %c single character") {
    ASSERT_EQ(do_printf("%c", 'A'), "A");
}

TEST("kprintf: %c multiple characters") {
    ASSERT_EQ(do_printf("%c%c%c", 'A', 'B', 'C'), "ABC");
}

// ============================================================
// %% literal percent tests
// ============================================================

TEST("kprintf: %% literal percent") {
    ASSERT_EQ(do_printf("100%%"), "100%");
}

// ============================================================
// Width modifier tests
// ============================================================

TEST("kprintf: %08x zero-pad hex") {
    ASSERT_EQ(do_printf("%08x", 0xFF), "000000ff");
}

TEST("kprintf: %08X zero-pad uppercase hex") {
    ASSERT_EQ(do_printf("%08X", 0xFF), "000000FF");
}

TEST("kprintf: %4d space-pad decimal") {
    ASSERT_EQ(do_printf("[%4d]", 7), "[   7]");
}

TEST("kprintf: %08x already full width") {
    ASSERT_EQ(do_printf("%08x", 0xDEADBEEF), "deadbeef");
}

TEST("kprintf: %-10d left-align decimal") {
    ASSERT_EQ(do_printf("[%-10d]", 42), "[42        ]");
}

// ============================================================
// Negative number formatting with width
// ============================================================

TEST("kprintf: %d negative with width space-pad") {
    ASSERT_EQ(do_printf("[%6d]", -42), "[   -42]");
}

TEST("kprintf: %06d negative zero-pad") {
    ASSERT_EQ(do_printf("[%06d]", -42), "[-00042]");
}

TEST("kprintf: %-6d negative left-align") {
    ASSERT_EQ(do_printf("[%-6d]", -42), "[-42   ]");
}

// ============================================================
// Mixed format tests
// ============================================================

TEST("kprintf: mixed format string") {
    ASSERT_EQ(do_printf("%s=%d (0x%x)", "answer", 42, 42),
              "answer=42 (0x2a)");
}

TEST("kprintf: plain text only") {
    ASSERT_EQ(do_printf("hello world"), "hello world");
}

TEST("kprintf: multiple specifiers") {
    ASSERT_EQ(do_printf("%d %u %x %X", 10, 20u, 0xFF, 0xAB),
              "10 20 ff AB");
}

// ============================================================
// Unknown specifier fallback
// ============================================================

TEST("kprintf: unknown specifier fallback") {
    ASSERT_EQ(do_printf("%q"), "%q");
}

// ============================================================
// Edge case tests
// ============================================================

TEST("kprintf: empty format string") {
    ASSERT_EQ(do_printf(""), "");
}

TEST("kprintf: trailing literal") {
    ASSERT_EQ(do_printf("hello %d!", 42), "hello 42!");
}

// ============================================================
// Main entry point
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
