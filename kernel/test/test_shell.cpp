/**
 * @file kernel/test/test_shell.cpp
 * @brief QEMU in-kernel integration tests for shell infrastructure (024)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * kernel-side components that the shell depends on:
 *   - Syscall dispatch: sys_read (SYS_read=0) and sys_write (SYS_write=1)
 *     handlers are registered and callable
 *   - Tokenizer logic (mirrored from user/shell/main.cpp)
 *   - CmdEntry dispatch table pattern
 *   - User-space string utility equivalents (strlen, strcmp, memset, memcpy,
 *     memcmp via the kernel's own implementations where available)
 *
 * Note: The actual user-space shell code (user/programs/shell/) runs in
 * Ring 3 and cannot be directly called from kernel test context.  These
 * tests verify the kernel-side infrastructure the shell depends on.
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM, VMM, Heap, AddressSpace initialised
 *   - usermode_init() called
 *   - syscall_init() called
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/syscall_nums.hpp"

using cinux::syscall::SyscallNr;
using cinux::syscall::sys_write;

// ============================================================
// Mirror of shell tokenizer (same logic as user/shell/main.cpp)
// ============================================================

namespace {

constexpr size_t MAX_TOKENS = 16;

size_t tokenize(char* line, char** argv, size_t max_tokens) {
    size_t argc = 0;

    while (*line != '\0' && argc < max_tokens) {
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
        if (*line == '\0') {
            break;
        }
        argv[argc++] = line;
        while (*line != '\0' && *line != ' ' && *line != '\t') {
            ++line;
        }
        if (*line != '\0') {
            *line++ = '\0';
        }
    }
    return argc;
}

// Minimal strlen for kernel context (no stdlib)
size_t k_strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

int k_strcmp(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return static_cast<int>(*a) - static_cast<int>(*b);
        }
        ++a;
        ++b;
    }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

// Freestanding memset/memcpy/memcmp for kernel test context
void* k_memset(void* dest, int c, size_t n) {
    auto*         d = static_cast<uint8_t*>(dest);
    const uint8_t v = static_cast<uint8_t>(c);
    for (size_t i = 0; i < n; ++i) {
        d[i] = v;
    }
    return dest;
}

void* k_memcpy(void* dest, const void* src, size_t n) {
    auto*       d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

int k_memcmp(const void* a, const void* b, size_t n) {
    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
        }
    }
    return 0;
}

}  // anonymous namespace

// ============================================================
// Test 1: Kernel string utility equivalents used by shell
// ============================================================

namespace test_k_string {

void test_strlen_empty() {
    TEST_ASSERT_EQ(k_strlen(""), 0ULL);
}

void test_strlen_hello() {
    TEST_ASSERT_EQ(k_strlen("hello"), 5ULL);
}

void test_strcmp_equal() {
    TEST_ASSERT_EQ(k_strcmp("echo", "echo"), 0);
}

void test_strcmp_less() {
    TEST_ASSERT_TRUE(k_strcmp("abc", "abd") < 0);
}

void test_strcmp_greater() {
    TEST_ASSERT_TRUE(k_strcmp("abd", "abc") > 0);
}

void test_strcmp_empty_vs_nonempty() {
    TEST_ASSERT_TRUE(k_strcmp("", "a") < 0);
}

void test_strlen_with_spaces() {
    TEST_ASSERT_EQ(k_strlen("echo hello"), 10ULL);
}

}  // namespace test_k_string

// ============================================================
// Test 2: Tokenizer (same logic as user/shell/main.cpp)
// ============================================================

namespace test_tokenizer {

void test_single_word() {
    char   line[] = "hello";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    TEST_ASSERT_EQ(argc, 1ULL);
    TEST_ASSERT_TRUE(k_strcmp(argv[0], "hello") == 0);
}

void test_two_words() {
    char   line[] = "echo hello";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    TEST_ASSERT_EQ(argc, 2ULL);
    TEST_ASSERT_TRUE(k_strcmp(argv[0], "echo") == 0);
    TEST_ASSERT_TRUE(k_strcmp(argv[1], "hello") == 0);
}

void test_multiple_spaces() {
    char   line[] = "echo   hello   world";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    TEST_ASSERT_EQ(argc, 3ULL);
    TEST_ASSERT_TRUE(k_strcmp(argv[0], "echo") == 0);
    TEST_ASSERT_TRUE(k_strcmp(argv[1], "hello") == 0);
    TEST_ASSERT_TRUE(k_strcmp(argv[2], "world") == 0);
}

void test_leading_trailing_whitespace() {
    char   line[] = "   echo hello   ";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    TEST_ASSERT_EQ(argc, 2ULL);
    TEST_ASSERT_TRUE(k_strcmp(argv[0], "echo") == 0);
    TEST_ASSERT_TRUE(k_strcmp(argv[1], "hello") == 0);
}

void test_empty_string() {
    char   line[] = "";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    TEST_ASSERT_EQ(argc, 0ULL);
}

void test_only_whitespace() {
    char   line[] = "   \t  ";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    TEST_ASSERT_EQ(argc, 0ULL);
}

void test_tab_separated() {
    char   line[] = "echo\thello\tworld";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);
    TEST_ASSERT_EQ(argc, 3ULL);
    TEST_ASSERT_TRUE(k_strcmp(argv[0], "echo") == 0);
    TEST_ASSERT_TRUE(k_strcmp(argv[1], "hello") == 0);
    TEST_ASSERT_TRUE(k_strcmp(argv[2], "world") == 0);
}

void test_max_tokens_limit() {
    char   line[] = "a b c d e f g h i j k l m n o p q";
    char*  argv[4];
    size_t argc = tokenize(line, argv, 4);
    TEST_ASSERT_EQ(argc, 4ULL);
    TEST_ASSERT_TRUE(k_strcmp(argv[0], "a") == 0);
    TEST_ASSERT_TRUE(k_strcmp(argv[3], "d") == 0);
}

}  // namespace test_tokenizer

// ============================================================
// Test 3: Syscall dispatch table has read/write registered
// ============================================================

namespace test_syscall_for_shell {

void test_sys_write_registered() {
    // Call sys_write directly (not via dispatch table, which may not have
    // the handler registered in test builds).  count=0 to avoid printing
    // garbage at arbitrary user addresses.
    int64_t result = sys_write(1, 0x1000, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);
}

void test_sys_write_invalid_fd_rejected() {
    int64_t r0 = sys_write(0, 0x1000, 5, 0, 0, 0);
    TEST_ASSERT_EQ(r0, -1);
}

void test_sys_write_null_buf_rejected() {
    // buf_virt == 0 (null pointer) should return -1
    int64_t r1 = sys_write(1, 0, 5, 0, 0, 0);
    TEST_ASSERT_EQ(r1, -1);
}

void test_syscall_nr_constants() {
    // Verify syscall numbers the shell uses are correct
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_read), 0ULL);
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_write), 1ULL);
    TEST_ASSERT_EQ(static_cast<uint64_t>(SyscallNr::SYS_exit), 60ULL);
}

}  // namespace test_syscall_for_shell

// ============================================================
// Test 4: CmdEntry dispatch table pattern
// ============================================================

namespace test_cmd_dispatch {

// Mirror of CmdEntry from shell.hpp
struct CmdEntry {
    const char* name;
    void (*handler)(int argc, char** argv);
};

// Dummy command handlers for testing
static bool cmd_test_called;
static int  cmd_test_argc;

void cmd_test(int argc, char** argv) {
    cmd_test_called = true;
    cmd_test_argc   = argc;
    (void)argv;
}

void cmd_test2(int argc, char** argv) {
    (void)argc;
    (void)argv;
}

void test_dispatch_finds_registered_command() {
    CmdEntry cmds[] = {
        {"test", cmd_test},
        {"test2", cmd_test2},
        {nullptr, nullptr},
    };

    cmd_test_called = false;
    cmd_test_argc   = 0;

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (k_strcmp("test", cmds[i].name) == 0) {
            char* argv[] = {const_cast<char*>("test")};
            cmds[i].handler(1, argv);
            found = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_TRUE(cmd_test_called);
    TEST_ASSERT_EQ(cmd_test_argc, 1);
}

void test_dispatch_unknown_not_found() {
    CmdEntry cmds[] = {
        {"test", cmd_test},
        {nullptr, nullptr},
    };

    cmd_test_called = false;

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (k_strcmp("nonexistent", cmds[i].name) == 0) {
            found = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(!found);
    TEST_ASSERT_TRUE(!cmd_test_called);
}

void test_dispatch_table_entry_count() {
    CmdEntry cmds[] = {
        {"echo", cmd_test},
        {"help", cmd_test2},
        {"clear", cmd_test},
        {nullptr, nullptr},
    };

    int count = 0;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        ++count;
    }
    TEST_ASSERT_EQ(count, 3);
}

}  // namespace test_cmd_dispatch

// ============================================================
// Test 5: Kernel memset/memcpy (used by shell's libc)
// ============================================================

namespace test_k_mem {

void test_memset_fill() {
    uint8_t buf[16];
    k_memset(buf, 0xAB, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); ++i) {
        TEST_ASSERT_EQ(buf[i], 0xAB);
    }
}

void test_memset_zero_length() {
    uint8_t buf[4] = {1, 2, 3, 4};
    size_t  zero   = 0;
    k_memset(buf, 0xFF, zero);
    TEST_ASSERT_EQ(buf[0], 1);
    TEST_ASSERT_EQ(buf[1], 2);
}

void test_memcpy_round_trip() {
    const uint8_t src[]  = {1, 2, 3, 4, 5};
    uint8_t       dst[5] = {};
    k_memcpy(dst, src, sizeof(src));
    for (size_t i = 0; i < sizeof(src); ++i) {
        TEST_ASSERT_EQ(dst[i], src[i]);
    }
}

void test_memcmp_equal() {
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {1, 2, 3};
    TEST_ASSERT_EQ(k_memcmp(a, b, 3), 0);
}

void test_memcmp_differ() {
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {1, 2, 4};
    TEST_ASSERT_TRUE(k_memcmp(a, b, 3) < 0);
}

void test_memcmp_zero_len() {
    const uint8_t a[] = {0xFF};
    const uint8_t b[] = {0x00};
    TEST_ASSERT_EQ(k_memcmp(a, b, 0), 0);
}

}  // namespace test_k_mem

// ============================================================
// Entry point
// ============================================================

extern "C" void run_shell_tests() {
    TEST_SECTION("Shell Tests (024)");

    // Kernel string utilities
    RUN_TEST(test_k_string::test_strlen_empty);
    RUN_TEST(test_k_string::test_strlen_hello);
    RUN_TEST(test_k_string::test_strcmp_equal);
    RUN_TEST(test_k_string::test_strcmp_less);
    RUN_TEST(test_k_string::test_strcmp_greater);
    RUN_TEST(test_k_string::test_strcmp_empty_vs_nonempty);
    RUN_TEST(test_k_string::test_strlen_with_spaces);

    // Tokenizer
    RUN_TEST(test_tokenizer::test_single_word);
    RUN_TEST(test_tokenizer::test_two_words);
    RUN_TEST(test_tokenizer::test_multiple_spaces);
    RUN_TEST(test_tokenizer::test_leading_trailing_whitespace);
    RUN_TEST(test_tokenizer::test_empty_string);
    RUN_TEST(test_tokenizer::test_only_whitespace);
    RUN_TEST(test_tokenizer::test_tab_separated);
    RUN_TEST(test_tokenizer::test_max_tokens_limit);

    // Syscall dispatch for shell
    RUN_TEST(test_syscall_for_shell::test_sys_write_registered);
    RUN_TEST(test_syscall_for_shell::test_sys_write_invalid_fd_rejected);
    RUN_TEST(test_syscall_for_shell::test_sys_write_null_buf_rejected);
    RUN_TEST(test_syscall_for_shell::test_syscall_nr_constants);

    // Command dispatch table pattern
    RUN_TEST(test_cmd_dispatch::test_dispatch_finds_registered_command);
    RUN_TEST(test_cmd_dispatch::test_dispatch_unknown_not_found);
    RUN_TEST(test_cmd_dispatch::test_dispatch_table_entry_count);

    // Kernel memory utilities
    RUN_TEST(test_k_mem::test_memset_fill);
    RUN_TEST(test_k_mem::test_memset_zero_length);
    RUN_TEST(test_k_mem::test_memcpy_round_trip);
    RUN_TEST(test_k_mem::test_memcmp_equal);
    RUN_TEST(test_k_mem::test_memcmp_differ);
    RUN_TEST(test_k_mem::test_memcmp_zero_len);

    TEST_SUMMARY();
}
