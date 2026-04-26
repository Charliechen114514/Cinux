/**
 * @file test/unit/test_kprintf_format.cpp
 * @brief kprintf Format Function Tests
 *
 * Tests the format functions extracted from kprintf.cpp.
 * These are pure algorithm tests that run in Host mode.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <limits.h>

#    include <cstdint>
#    include <string>

// Directly reference the kernel's format function implementation
#    include "mini/lib/private/format.h"

using namespace cinux::mini::lib::detail;

// ============================================================
// Decimal Format Tests
// ============================================================

/**
 * @brief Test positive decimal formatting
 */
TEST("kprintf: decimal positive") {
    char buffer[64];
    int  len = format_decimal(42, buffer, sizeof(buffer));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(std::string(buffer), "42");
}

/**
 * @brief Test negative decimal formatting
 */
TEST("kprintf: decimal negative") {
    char buffer[64];
    int  len = format_decimal(-12345, buffer, sizeof(buffer));
    ASSERT_EQ(len, 6);
    ASSERT_EQ(std::string(buffer), "-12345");
}

/**
 * @brief Test zero formatting
 */
TEST("kprintf: decimal zero") {
    char buffer[64];
    int  len = format_decimal(0, buffer, sizeof(buffer));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buffer), "0");
}

/**
 * @brief Test INT64_MIN edge case
 */
TEST("kprintf: decimal INT64_MIN") {
    char buffer[64];
    int  len = format_decimal(INT64_MIN, buffer, sizeof(buffer));
    ASSERT_EQ(len, 20);
    ASSERT_EQ(std::string(buffer), "-9223372036854775808");
}

/**
 * @brief Test INT64_MAX formatting
 */
TEST("kprintf: decimal INT64_MAX") {
    char buffer[64];
    int  len = format_decimal(INT64_MAX, buffer, sizeof(buffer));
    ASSERT_EQ(len, 19);
    ASSERT_EQ(std::string(buffer), "9223372036854775807");
}

// ============================================================
// Hexadecimal Format Tests
// ============================================================

/**
 * @brief Test lowercase hex formatting
 */
TEST("kprintf: hex lowercase") {
    char buffer[64];
    int  len = format_hex(0xDEADBEEF, buffer, sizeof(buffer), true);
    ASSERT_EQ(len, 8);
    ASSERT_EQ(std::string(buffer), "deadbeef");
}

/**
 * @brief Test uppercase hex formatting
 */
TEST("kprintf: hex uppercase") {
    char buffer[64];
    int  len = format_hex(0xDEADBEEF, buffer, sizeof(buffer), false);
    ASSERT_EQ(len, 8);
    ASSERT_EQ(std::string(buffer), "DEADBEEF");
}

/**
 * @brief Test zero hex formatting
 */
TEST("kprintf: hex zero") {
    char buffer[64];
    int  len = format_hex(0, buffer, sizeof(buffer), true);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buffer), "0");
}

/**
 * @brief Test all hex digits (0-9, a-f)
 */
TEST("kprintf: hex all digits") {
    char buffer[64];
    int  len = format_hex(0x123456789ABCDEF0ULL, buffer, sizeof(buffer), true);
    ASSERT_EQ(len, 16);
    ASSERT_EQ(std::string(buffer), "123456789abcdef0");
}

/**
 * @brief Test single digit hex
 */
TEST("kprintf: hex single digit") {
    char buffer[64];
    int  len = format_hex(0xA, buffer, sizeof(buffer), true);
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buffer), "a");
}

// ============================================================
// Binary Format Tests
// ============================================================

/**
 * @brief Test basic binary formatting
 */
TEST("kprintf: binary") {
    char buffer[64];
    int  len = format_binary(0b101010, buffer, sizeof(buffer));
    ASSERT_EQ(len, 6);
    ASSERT_EQ(std::string(buffer), "101010");
}

/**
 * @brief Test zero binary formatting
 */
TEST("kprintf: binary zero") {
    char buffer[64];
    int  len = format_binary(0, buffer, sizeof(buffer));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buffer), "0");
}

/**
 * @brief Test binary with leading zeros suppressed
 */
TEST("kprintf: binary leading zeros suppressed") {
    char buffer[64];
    int  len = format_binary(0b00101, buffer, sizeof(buffer));
    ASSERT_EQ(len, 3);
    ASSERT_EQ(std::string(buffer), "101");
}

/**
 * @brief Test max 64-bit value (all ones)
 */
TEST("kprintf: binary max 64-bit") {
    char buffer[65];  // 64 chars + '\0'
    int  len = format_binary(0xFFFFFFFFFFFFFFFFULL, buffer, sizeof(buffer));
    ASSERT_EQ(len, 64);
    // All bits should be '1'
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(buffer[i], '1');
    }
}

/**
 * @brief Test power of 2 values
 */
TEST("kprintf: binary power of 2") {
    char buffer[65];  // 64 chars + '\0'

    int len = format_binary(1ULL << 0, buffer, sizeof(buffer));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(std::string(buffer), "1");

    len = format_binary(1ULL << 3, buffer, sizeof(buffer));
    ASSERT_EQ(len, 4);
    ASSERT_EQ(std::string(buffer), "1000");

    len = format_binary(1ULL << 63, buffer, sizeof(buffer));
    ASSERT_EQ(len, 64);
    ASSERT_EQ(std::string(buffer),
              "1000000000000000000000000000000000000000000000000000000000000000");
}

// ============================================================
// Main Function
// ============================================================

/**
 * @brief Test entry point
 */
int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
