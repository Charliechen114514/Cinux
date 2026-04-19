/**
 * @file tests/unit/test_kprintf.cpp
 * @brief Host-side unit tests for the big kernel kprintf formatting engine
 *
 * Compile condition: -DCINUX_HOST_TEST
 *
 * Strategy:
 *   We cannot link the real serial driver (it touches I/O ports), so we
 *   mock the output by providing a custom OutputFn that appends to a
 *   std::string buffer.  The formatting logic in vkprintf_impl is a
 *   template, so it works with any callable.
 *
 *   To avoid pulling in the kernel's serial/IO dependencies, we:
 *   1. Include only the formatting helpers (format_decimal, format_hex)
 *      directly -- they are pure arithmetic.
 *   2. Re-implement a thin version of the vkprintf parser here so we
 *      can feed it a mock output function.
 *
 *   Alternatively, if the kprintf implementation is refactored to
 *   separate formatting from output, we can include it directly.
 *   For now, we test the formatting helpers and the %p / %d / %x
 *   specifier logic through a local test harness.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <stdarg.h>
#include <string>

// ============================================================
// Mock: capture formatted output into a string
// ============================================================

/**
 * @brief Simple formatter that writes to a captured string buffer
 *
 * This mirrors the logic in kernel/lib/kprintf.cpp so we can test
 * each format specifier without any hardware dependency.
 */
class MockFormatter {
public:
    void putc(char c) { buffer_.push_back(c); }

    void puts(const char* s) {
        while (*s) {
            if (*s == '\n') putc('\r');
            putc(*s++);
        }
    }

    std::string result() const { return buffer_; }
    void        clear() { buffer_.clear(); }

private:
    std::string buffer_;
};

// ============================================================
// Number formatting helpers (copied from kprintf.cpp for testing)
// ============================================================

static int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == (-9223372036854775807LL - 1)) {
            const char* min_str = "-9223372036854775808";
            int len = 0;
            while (min_str[len] != '\0' && idx < buffer_size - 1)
                buffer[idx++] = min_str[len++];
            buffer[idx] = '\0';
            return idx;
        }
        value = -value;
    }

    uint64_t abs_val = static_cast<uint64_t>(value);
    char     tmp[24];
    int      tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + static_cast<char>(abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    if (is_neg && idx < buffer_size - 1) buffer[idx++] = '-';

    while (tmp_idx > 0 && idx < buffer_size - 1)
        buffer[idx++] = tmp[--tmp_idx];
    buffer[idx] = '\0';

    return idx;
}

static int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1) return 0;

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char tmp[20];
    int  tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);

    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1)
        buffer[idx++] = tmp[--tmp_idx];
    buffer[idx] = '\0';

    return idx;
}

// ============================================================
// Normal path tests: decimal formatting
// ============================================================

TEST("kprintf: format positive decimal") {
    char buf[64];
    int  len = format_decimal(42, buf, sizeof(buf));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(std::string(buf), "42");
}

TEST("kprintf: format negative decimal") {
    char buf[64];
    int  len = format_decimal(-12345, buf, sizeof(buf));
    ASSERT_EQ(len, 6);
    ASSERT_EQ(std::string(buf), "-12345");
}

TEST("kprintf: format zero") {
    char buf[64];
    int  len = format_decimal(0, buf, sizeof(buf));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buf), "0");
}

TEST("kprintf: format INT64_MAX") {
    char buf[64];
    int  len = format_decimal(9223372036854775807LL, buf, sizeof(buf));
    ASSERT_EQ(len, 19);
    ASSERT_EQ(std::string(buf), "9223372036854775807");
}

// ============================================================
// Normal path tests: hex formatting
// ============================================================

TEST("kprintf: format lowercase hex") {
    char buf[64];
    int  len = format_hex(0xDEADBEEF, buf, sizeof(buf), true);
    ASSERT_EQ(len, 8);
    ASSERT_EQ(std::string(buf), "deadbeef");
}

TEST("kprintf: format uppercase hex") {
    char buf[64];
    int  len = format_hex(0xDEADBEEF, buf, sizeof(buf), false);
    ASSERT_EQ(len, 8);
    ASSERT_EQ(std::string(buf), "DEADBEEF");
}

TEST("kprintf: format hex zero") {
    char buf[64];
    int  len = format_hex(0, buf, sizeof(buf), true);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buf), "0");
}

TEST("kprintf: format hex single digit") {
    char buf[64];
    int  len = format_hex(0xA, buf, sizeof(buf), true);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buf), "a");
}

// ============================================================
// Boundary condition tests
// ============================================================

TEST("kprintf: format decimal with small buffer") {
    char buf[3];
    int  len = format_decimal(12345, buf, sizeof(buf));
    // Should truncate but not overflow
    ASSERT_TRUE(len <= 2);
}

TEST("kprintf: format hex with zero buffer") {
    char buf[1];
    int  len = format_hex(0xFF, buf, 0, true);
    ASSERT_EQ(len, 0);
}

// ============================================================
// %p specifier tests (pointer with 0x prefix, 16 hex digits)
// ============================================================

TEST("kprintf: %p produces 0x prefix + 16 hex digits") {
    // Simulate what %p does: "0x" + pad to 16 digits + hex value
    char buf[64];
    int  len = format_hex(0x1000000ULL, buf, sizeof(buf), false);

    std::string result = "0x";
    for (int i = len; i < 16; i++) result += '0';
    result += std::string(buf);

    // "0x" + 8 leading zeros + "1000000" = 2 + 8 + 7 = 17 ... let's check
    ASSERT_TRUE(result.substr(0, 2) == "0x");
    // Total length should be 2 + 16 = 18
    ASSERT_EQ(result.length(), 18u);
}

TEST("kprintf: %p with zero address") {
    char buf[64];
    int  len = format_hex(0, buf, sizeof(buf), false);

    std::string result = "0x";
    for (int i = len; i < 16; i++) result += '0';
    result += std::string(buf);

    ASSERT_EQ(result, "0x0000000000000000");
}

// ============================================================
// Mock output integration test
// ============================================================

TEST("kprintf: mock serial captures output") {
    // Verify that our MockFormatter works as expected
    MockFormatter mock;
    mock.puts("Hello");
    mock.putc('!');
    ASSERT_EQ(mock.result(), "Hello!");
}

TEST("kprintf: mock serial converts LF to CRLF") {
    MockFormatter mock;
    mock.puts("line1\nline2\n");
    ASSERT_EQ(mock.result(), "line1\r\nline2\r\n");
}

// ============================================================
// Full formatting engine tests (mock-based vprintf simulation)
// ============================================================

/**
 * @brief Simulate the vkprintf_impl format parser from kprintf.cpp
 *
 * This mirrors the actual format parsing logic so we can test
 * the complete formatting chain through a mock output function.
 */
static void mock_vprintf(MockFormatter& mock, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char buf[64];

    while (*fmt != '\0') {
        if (*fmt != '%') {
            mock.putc(*fmt++);
            continue;
        }
        fmt++;  // consume '%'

        bool zero_pad = false;
        int  width    = 0;
        if (*fmt == '0') { zero_pad = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char type = *fmt++;

        switch (type) {
        case '%': mock.putc('%'); break;
        case 'c': mock.putc(static_cast<char>(va_arg(args, int))); break;
        case 's': {
            const char* s = va_arg(args, const char*);
            if (!s) s = "(null)";
            while (*s) mock.putc(*s++);
            break;
        }
        case 'd': {
            int len = format_decimal(static_cast<int64_t>(va_arg(args, int)), buf, sizeof(buf));
            if (len < width) { char p = zero_pad ? '0' : ' '; for (int i = width - len; i > 0; i--) mock.putc(p); }
            for (int i = 0; i < len; i++) mock.putc(buf[i]);
            break;
        }
        case 'u': {
            int len = format_decimal(static_cast<int64_t>(va_arg(args, unsigned int)), buf, sizeof(buf));
            if (len < width) { char p = zero_pad ? '0' : ' '; for (int i = width - len; i > 0; i--) mock.putc(p); }
            for (int i = 0; i < len; i++) mock.putc(buf[i]);
            break;
        }
        case 'x': {
            int len = format_hex(va_arg(args, uint64_t), buf, sizeof(buf), true);
            if (len < width) { char p = zero_pad ? '0' : ' '; for (int i = width - len; i > 0; i--) mock.putc(p); }
            for (int i = 0; i < len; i++) mock.putc(buf[i]);
            break;
        }
        case 'X': {
            int len = format_hex(va_arg(args, uint64_t), buf, sizeof(buf), false);
            if (len < width) { char p = zero_pad ? '0' : ' '; for (int i = width - len; i > 0; i--) mock.putc(p); }
            for (int i = 0; i < len; i++) mock.putc(buf[i]);
            break;
        }
        case 'p': {
            mock.putc('0'); mock.putc('x');
            int len = format_hex(va_arg(args, uint64_t), buf, sizeof(buf), false);
            for (int i = len; i < 16; i++) mock.putc('0');
            for (int i = 0; i < len; i++) mock.putc(buf[i]);
            break;
        }
        default:
            mock.putc('%'); mock.putc(type); break;
        }
    }

    va_end(args);
}

// -- Full engine tests --

TEST("kprintf engine: %d positive") {
    MockFormatter m;
    mock_vprintf(m, "val=%d", 42);
    ASSERT_EQ(m.result(), "val=42");
}

TEST("kprintf engine: %d negative") {
    MockFormatter m;
    mock_vprintf(m, "%d", -99);
    ASSERT_EQ(m.result(), "-99");
}

TEST("kprintf engine: %d zero") {
    MockFormatter m;
    mock_vprintf(m, "%d", 0);
    ASSERT_EQ(m.result(), "0");
}

TEST("kprintf engine: %u large") {
    MockFormatter m;
    mock_vprintf(m, "%u", 4000000000u);
    ASSERT_EQ(m.result(), "4000000000");
}

TEST("kprintf engine: %x lowercase") {
    MockFormatter m;
    mock_vprintf(m, "0x%x", 0xDEAD);
    ASSERT_EQ(m.result(), "0xdead");
}

TEST("kprintf engine: %X uppercase") {
    MockFormatter m;
    mock_vprintf(m, "0x%X", 0xBEEF);
    ASSERT_EQ(m.result(), "0xBEEF");
}

TEST("kprintf engine: %s normal") {
    MockFormatter m;
    mock_vprintf(m, "hello %s!", "world");
    ASSERT_EQ(m.result(), "hello world!");
}

TEST("kprintf engine: %s nullptr") {
    MockFormatter m;
    mock_vprintf(m, "%s", nullptr);
    ASSERT_EQ(m.result(), "(null)");
}

TEST("kprintf engine: %s empty") {
    MockFormatter m;
    mock_vprintf(m, "[%s]", "");
    ASSERT_EQ(m.result(), "[]");
}

TEST("kprintf engine: %c character") {
    MockFormatter m;
    mock_vprintf(m, "%c%c%c", 'A', 'B', 'C');
    ASSERT_EQ(m.result(), "ABC");
}

TEST("kprintf engine: %% literal percent") {
    MockFormatter m;
    mock_vprintf(m, "100%%");
    ASSERT_EQ(m.result(), "100%");
}

TEST("kprintf engine: %p pointer") {
    MockFormatter m;
    mock_vprintf(m, "%p", 0x1000000ULL);
    ASSERT_EQ(m.result(), "0x0000000001000000");
}

TEST("kprintf engine: %p zero") {
    MockFormatter m;
    mock_vprintf(m, "%p", 0ULL);
    ASSERT_EQ(m.result(), "0x0000000000000000");
}

TEST("kprintf engine: %08x zero-pad") {
    MockFormatter m;
    mock_vprintf(m, "%08x", 0xFF);
    ASSERT_EQ(m.result(), "000000ff");
}

TEST("kprintf engine: %4d space-pad") {
    MockFormatter m;
    mock_vprintf(m, "[%4d]", 7);
    ASSERT_EQ(m.result(), "[   7]");
}

TEST("kprintf engine: mixed format") {
    MockFormatter m;
    mock_vprintf(m, "%s=%d (0x%x)", "answer", 42, 42);
    ASSERT_EQ(m.result(), "answer=42 (0x2a)");
}

TEST("kprintf engine: unknown specifier fallback") {
    MockFormatter m;
    mock_vprintf(m, "%q");
    ASSERT_EQ(m.result(), "%q");
}

// ============================================================
// Main entry point
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
