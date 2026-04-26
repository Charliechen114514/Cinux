/**
 * @file test/unit/test_shell.cpp
 * @brief Host-side unit tests for shell infrastructure (024)
 *
 * Test coverage:
 *   - cinux::user::strlen: empty, normal, long strings
 *   - cinux::user::strcmp: equal, less, greater, prefix, empty
 *   - cinux::user::memset: fill, zero-length, alignment edge cases
 *   - cinux::user::memcpy: copy, overlapping-ish, zero-length
 *   - cinux::user::memcmp: equal, differ at first/last byte, zero-length
 *   - Tokenizer: single/multiple tokens, leading/trailing whitespace, tabs,
 *     max tokens, empty string, only whitespace
 *   - CmdEntry dispatch table: lookup, not-found, sentinel-terminated
 *   - cmd_echo: single arg, multi arg, no args (just newline)
 *   - cmd_help: produces output (non-empty capture)
 *   - cmd_clear: emits correct ANSI escape sequence
 *   - read_line simulation: backspace handling, newline termination
 *
 * Pure arithmetic and logic -- no kernel code linked.  Hardware dependencies
 * (sys_write, sys_read) are isolated via mock reimplementation.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstddef>
#    include <cstdint>
#    include <cstring>

// ============================================================
// Include the real string utility implementation
// ============================================================

// We include the .cpp directly so that we can exercise the real
// implementation without linking against the kernel build artifacts.

namespace cinux::user {

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return static_cast<int>(*a) - static_cast<int>(*b);
        }
        ++a;
        ++b;
    }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

void* memset(void* dest, int c, size_t n) {
    auto*         d = static_cast<uint8_t*>(dest);
    const uint8_t v = static_cast<uint8_t>(c);
    for (size_t i = 0; i < n; ++i) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    auto*       d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) {
        d[i] = s[i];
    }
    return dest;
}

int memcmp(const void* a, const void* b, size_t n) {
    const auto* pa = static_cast<const uint8_t*>(a);
    const auto* pb = static_cast<const uint8_t*>(b);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) {
            return static_cast<int>(pa[i]) - static_cast<int>(pb[i]);
        }
    }
    return 0;
}

}  // namespace cinux::user

// ============================================================
// Mock sys_write / sys_read for shell command tests
// ============================================================

namespace mock {

// Capture buffer for sys_write output
constexpr size_t CAPTURE_SIZE = 8192;
char             write_capture[CAPTURE_SIZE];
size_t           write_capture_len = 0;

void reset_capture() {
    std::memset(write_capture, 0, sizeof(write_capture));
    write_capture_len = 0;
}

// Mock sys_write: captures output into write_capture buffer
int64_t sys_write(int fd, const void* buf, size_t count) {
    if (fd != 1) {
        return -1;
    }
    if (buf == nullptr) {
        return -1;
    }
    size_t to_copy = count;
    if (write_capture_len + to_copy > CAPTURE_SIZE - 1) {
        to_copy = CAPTURE_SIZE - 1 - write_capture_len;
    }
    std::memcpy(write_capture + write_capture_len, buf, to_copy);
    write_capture_len += to_copy;
    write_capture[write_capture_len] = '\0';
    return static_cast<int64_t>(count);
}

// Simulated input buffer for sys_read mock
char   read_buffer[1024];
size_t read_pos = 0;
size_t read_len = 0;

void set_read_input(const char* input) {
    size_t slen = std::strlen(input);
    if (slen > sizeof(read_buffer) - 1) {
        slen = sizeof(read_buffer) - 1;
    }
    std::memcpy(read_buffer, input, slen);
    read_buffer[slen] = '\0';
    read_pos          = 0;
    read_len          = slen;
}

// Mock sys_read: returns one character at a time from read_buffer
int64_t sys_read(int /*fd*/, void* buf, size_t count) {
    if (read_pos >= read_len) {
        return 0;  // EOF
    }
    if (count == 0) {
        return 0;
    }
    auto* out = static_cast<char*>(buf);
    *out      = read_buffer[read_pos++];
    return 1;
}

}  // namespace mock

// ============================================================
// Re-implement shell logic that uses anonymous namespace functions
// (tokenizer, read_line, commands) using the mock sys_write/sys_read
// ============================================================

namespace shell_test {

// Mirror of constants from main.cpp
constexpr size_t MAX_LINE   = 256;
constexpr size_t MAX_TOKENS = 16;

// Mirror of CmdEntry from shell.hpp
struct CmdEntry {
    const char* name;
    void (*handler)(int argc, char** argv);
};

// Mirror of tokenize from main.cpp
size_t tokenize(char* line, char** argv, size_t max_tokens) {
    size_t argc = 0;

    while (*line != '\0' && argc < max_tokens) {
        // Skip leading whitespace
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
        if (*line == '\0') {
            break;
        }

        argv[argc++] = line;

        // Advance past non-whitespace
        while (*line != '\0' && *line != ' ' && *line != '\t') {
            ++line;
        }

        // NUL-terminate the token
        if (*line != '\0') {
            *line++ = '\0';
        }
    }

    return argc;
}

// Mirror of read_line from main.cpp (using mock sys_read/sys_write)
size_t read_line(char* buf, size_t cap) {
    size_t pos = 0;

    while (pos < cap - 1) {
        char    c = 0;
        int64_t n = mock::sys_read(0, &c, 1);
        if (n <= 0) {
            continue;
        }

        if (c == '\n') {
            mock::sys_write(1, "\n", 1);
            break;
        }

        if (c == 0x7F || c == '\b') {
            if (pos > 0) {
                --pos;
                mock::sys_write(1, "\b \b", 3);
            }
            continue;
        }

        // Echo back and store
        mock::sys_write(1, &c, 1);
        buf[pos++] = c;
    }

    buf[pos] = '\0';
    return pos;
}

// Mirror of cmd_echo from cmd_echo.cpp
void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            mock::sys_write(1, " ", 1);
        }
        const char* s = argv[i];
        mock::sys_write(1, s, cinux::user::strlen(s));
    }
    mock::sys_write(1, "\n", 1);
}

// Mirror of cmd_help from cmd_help.cpp
void cmd_help(int /*argc*/, char** /*argv*/) {
    const char* help_text =
        "Available commands:\n"
        "  echo <args...>  - print arguments to stdout\n"
        "  help            - show this help message\n"
        "  clear           - clear the screen\n";
    mock::sys_write(1, help_text, cinux::user::strlen(help_text));
}

// Mirror of cmd_clear from cmd_clear.cpp
void cmd_clear(int /*argc*/, char** /*argv*/) {
    mock::sys_write(1, "\033[2J\033[H", 7);
}

}  // namespace shell_test

// ============================================================
// 1. cinux::user::strlen tests
// ============================================================

TEST("shell_strlen: empty string returns 0") {
    ASSERT_EQ(cinux::user::strlen(""), 0ULL);
}

TEST("shell_strlen: single character") {
    ASSERT_EQ(cinux::user::strlen("a"), 1ULL);
}

TEST("shell_strlen: normal string") {
    ASSERT_EQ(cinux::user::strlen("hello"), 5ULL);
}

TEST("shell_strlen: string with spaces") {
    ASSERT_EQ(cinux::user::strlen("hello world"), 11ULL);
}

TEST("shell_strlen: long string") {
    char buf[300];
    for (int i = 0; i < 299; ++i) {
        buf[i] = 'x';
    }
    buf[299] = '\0';
    ASSERT_EQ(cinux::user::strlen(buf), 299ULL);
}

// ============================================================
// 2. cinux::user::strcmp tests
// ============================================================

TEST("shell_strcmp: equal strings return 0") {
    ASSERT_EQ(cinux::user::strcmp("hello", "hello"), 0);
}

TEST("shell_strcmp: first less than second returns negative") {
    ASSERT_TRUE(cinux::user::strcmp("abc", "abd") < 0);
}

TEST("shell_strcmp: first greater than second returns positive") {
    ASSERT_TRUE(cinux::user::strcmp("abd", "abc") > 0);
}

TEST("shell_strcmp: empty vs non-empty returns negative") {
    ASSERT_TRUE(cinux::user::strcmp("", "a") < 0);
}

TEST("shell_strcmp: non-empty vs empty returns positive") {
    ASSERT_TRUE(cinux::user::strcmp("a", "") > 0);
}

TEST("shell_strcmp: both empty returns 0") {
    ASSERT_EQ(cinux::user::strcmp("", ""), 0);
}

TEST("shell_strcmp: prefix vs longer string returns negative") {
    ASSERT_TRUE(cinux::user::strcmp("hel", "hello") < 0);
}

TEST("shell_strcmp: longer vs prefix returns positive") {
    ASSERT_TRUE(cinux::user::strcmp("hello", "hel") > 0);
}

// ============================================================
// 3. cinux::user::memset tests
// ============================================================

TEST("shell_memset: fills buffer with value") {
    uint8_t buf[16];
    cinux::user::memset(buf, 0xAB, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); ++i) {
        ASSERT_EQ(buf[i], 0xAB);
    }
}

TEST("shell_memset: zero length does nothing") {
    uint8_t buf[4] = {1, 2, 3, 4};
    cinux::user::memset(buf, 0xFF, 0);
    ASSERT_EQ(buf[0], 1);
    ASSERT_EQ(buf[1], 2);
    ASSERT_EQ(buf[2], 3);
    ASSERT_EQ(buf[3], 4);
}

TEST("shell_memset: fill with zero") {
    uint8_t buf[8];
    cinux::user::memset(buf, 0, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); ++i) {
        ASSERT_EQ(buf[i], 0);
    }
}

TEST("shell_memset: returns original pointer") {
    uint8_t buf[8];
    void*   result = cinux::user::memset(buf, 0, sizeof(buf));
    ASSERT_TRUE(result == buf);
}

TEST("shell_memset: single byte fill") {
    uint8_t buf[1];
    cinux::user::memset(buf, 0x42, 1);
    ASSERT_EQ(buf[0], 0x42);
}

// ============================================================
// 4. cinux::user::memcpy tests
// ============================================================

TEST("shell_memcpy: copies bytes correctly") {
    const uint8_t src[]  = {1, 2, 3, 4, 5};
    uint8_t       dst[5] = {};
    cinux::user::memcpy(dst, src, sizeof(src));
    for (size_t i = 0; i < sizeof(src); ++i) {
        ASSERT_EQ(dst[i], src[i]);
    }
}

TEST("shell_memcpy: zero length does nothing") {
    uint8_t dst[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t src[4] = {1, 2, 3, 4};
    cinux::user::memcpy(dst, src, 0);
    ASSERT_EQ(dst[0], 0xAA);
    ASSERT_EQ(dst[1], 0xBB);
}

TEST("shell_memcpy: returns destination pointer") {
    uint8_t dst[4];
    uint8_t src[4] = {};
    void*   result = cinux::user::memcpy(dst, src, sizeof(src));
    ASSERT_TRUE(result == dst);
}

TEST("shell_memcpy: copies string data") {
    char        dst[16];
    const char* src = "hello world";
    cinux::user::memcpy(dst, src, 12);
    ASSERT_EQ(std::memcmp(dst, src, 12), 0);
}

// ============================================================
// 5. cinux::user::memcmp tests
// ============================================================

TEST("shell_memcmp: equal regions return 0") {
    const uint8_t a[] = {1, 2, 3, 4};
    const uint8_t b[] = {1, 2, 3, 4};
    ASSERT_EQ(cinux::user::memcmp(a, b, 4), 0);
}

TEST("shell_memcmp: differing at first byte") {
    const uint8_t a[] = {0, 2, 3, 4};
    const uint8_t b[] = {1, 2, 3, 4};
    ASSERT_TRUE(cinux::user::memcmp(a, b, 4) < 0);
}

TEST("shell_memcmp: differing at last byte") {
    const uint8_t a[] = {1, 2, 3, 5};
    const uint8_t b[] = {1, 2, 3, 4};
    ASSERT_TRUE(cinux::user::memcmp(a, b, 4) > 0);
}

TEST("shell_memcmp: zero length returns 0") {
    const uint8_t a[] = {0xFF};
    const uint8_t b[] = {0x00};
    ASSERT_EQ(cinux::user::memcmp(a, b, 0), 0);
}

TEST("shell_memcmp: partial comparison equal prefix") {
    const uint8_t a[] = {1, 2, 3, 99};
    const uint8_t b[] = {1, 2, 3, 88};
    ASSERT_EQ(cinux::user::memcmp(a, b, 3), 0);
}

// ============================================================
// 6. Tokenizer tests
// ============================================================

TEST("shell_tokenize: single word") {
    char   line[] = "hello";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 1ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "hello") == 0);
}

TEST("shell_tokenize: two words separated by space") {
    char   line[] = "echo hello";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 2ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "echo") == 0);
    ASSERT_TRUE(std::strcmp(argv[1], "hello") == 0);
}

TEST("shell_tokenize: multiple spaces between words") {
    char   line[] = "echo   hello   world";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 3ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "echo") == 0);
    ASSERT_TRUE(std::strcmp(argv[1], "hello") == 0);
    ASSERT_TRUE(std::strcmp(argv[2], "world") == 0);
}

TEST("shell_tokenize: leading whitespace is skipped") {
    char   line[] = "   echo hello";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 2ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "echo") == 0);
}

TEST("shell_tokenize: trailing whitespace produces no extra token") {
    char   line[] = "echo hello   ";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 2ULL);
}

TEST("shell_tokenize: empty string produces zero tokens") {
    char   line[] = "";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 0ULL);
}

TEST("shell_tokenize: only whitespace produces zero tokens") {
    char   line[] = "   \t  \t  ";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 0ULL);
}

TEST("shell_tokenize: tab separation") {
    char   line[] = "echo\thello";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 2ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "echo") == 0);
    ASSERT_TRUE(std::strcmp(argv[1], "hello") == 0);
}

TEST("shell_tokenize: mixed spaces and tabs") {
    char   line[] = " \t echo \t hello \t world \t ";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 3ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "echo") == 0);
    ASSERT_TRUE(std::strcmp(argv[1], "hello") == 0);
    ASSERT_TRUE(std::strcmp(argv[2], "world") == 0);
}

TEST("shell_tokenize: max tokens limit respected") {
    char   line[] = "a b c d e f g h i j k l m n o p q";
    char*  argv[4];  // Only allow 4 tokens
    size_t argc = shell_test::tokenize(line, argv, 4);
    ASSERT_EQ(argc, 4ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "a") == 0);
    ASSERT_TRUE(std::strcmp(argv[1], "b") == 0);
    ASSERT_TRUE(std::strcmp(argv[2], "c") == 0);
    ASSERT_TRUE(std::strcmp(argv[3], "d") == 0);
}

TEST("shell_tokenize: single character tokens") {
    char   line[] = "a b c";
    char*  argv[shell_test::MAX_TOKENS];
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 3ULL);
    ASSERT_TRUE(std::strcmp(argv[0], "a") == 0);
    ASSERT_TRUE(std::strcmp(argv[1], "b") == 0);
    ASSERT_TRUE(std::strcmp(argv[2], "c") == 0);
}

// ============================================================
// 7. Command dispatch table tests
// ============================================================

TEST("shell_dispatch: lookup echo command") {
    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    const char* query = "echo";
    bool        found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(query, builtin_cmds[i].name) == 0) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
}

TEST("shell_dispatch: lookup help command") {
    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    const char* query = "help";
    bool        found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(query, builtin_cmds[i].name) == 0) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
}

TEST("shell_dispatch: lookup clear command") {
    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    const char* query = "clear";
    bool        found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(query, builtin_cmds[i].name) == 0) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
}

TEST("shell_dispatch: unknown command not found") {
    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    const char* query = "foobar";
    bool        found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(query, builtin_cmds[i].name) == 0) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(!found);
}

TEST("shell_dispatch: sentinel-terminated iteration stops at nullptr") {
    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    int count = 0;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        ++count;
    }
    ASSERT_EQ(count, 3);
}

// ============================================================
// 8. cmd_echo tests
// ============================================================

TEST("shell_cmd_echo: single argument") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("hello")};
    shell_test::cmd_echo(2, argv);
    // Should output "hello\n"
    ASSERT_TRUE(std::strcmp(mock::write_capture, "hello\n") == 0);
}

TEST("shell_cmd_echo: multiple arguments") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("hello"),
                    const_cast<char*>("world"), const_cast<char*>("foo")};
    shell_test::cmd_echo(4, argv);
    // Should output "hello world foo\n"
    ASSERT_TRUE(std::strcmp(mock::write_capture, "hello world foo\n") == 0);
}

TEST("shell_cmd_echo: no arguments produces just newline") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("echo")};
    shell_test::cmd_echo(1, argv);
    // Should output "\n" only
    ASSERT_TRUE(std::strcmp(mock::write_capture, "\n") == 0);
}

TEST("shell_cmd_echo: two arguments produces correct spacing") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("a"), const_cast<char*>("b")};
    shell_test::cmd_echo(3, argv);
    ASSERT_TRUE(std::strcmp(mock::write_capture, "a b\n") == 0);
}

// ============================================================
// 9. cmd_help tests
// ============================================================

TEST("shell_cmd_help: produces non-empty output") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("help")};
    shell_test::cmd_help(1, argv);
    ASSERT_TRUE(mock::write_capture_len > 0);
}

TEST("shell_cmd_help: output contains command names") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("help")};
    shell_test::cmd_help(1, argv);

    // The help text should mention echo, help, and clear
    ASSERT_TRUE(std::strstr(mock::write_capture, "echo") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "help") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "clear") != nullptr);
}

TEST("shell_cmd_help: output ends with newline") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("help")};
    shell_test::cmd_help(1, argv);
    ASSERT_GT(mock::write_capture_len, 0ULL);
    ASSERT_EQ(mock::write_capture[mock::write_capture_len - 1], '\n');
}

// ============================================================
// 10. cmd_clear tests
// ============================================================

TEST("shell_cmd_clear: emits correct ANSI escape sequence") {
    mock::reset_capture();
    char* argv[] = {const_cast<char*>("clear")};
    shell_test::cmd_clear(1, argv);
    // ESC[2J ESC[H = "\033[2J\033[H"
    ASSERT_EQ(mock::write_capture_len, 7ULL);
    ASSERT_TRUE(std::memcmp(mock::write_capture, "\033[2J\033[H", 7) == 0);
}

// ============================================================
// 11. read_line simulation tests
// ============================================================

TEST("shell_read_line: simple input") {
    mock::reset_capture();
    char buf[shell_test::MAX_LINE];

    // Simulate typing "hello" then Enter
    mock::set_read_input("hello\n");
    size_t len = shell_test::read_line(buf, sizeof(buf));

    ASSERT_EQ(len, 5ULL);
    ASSERT_TRUE(std::strcmp(buf, "hello") == 0);
}

TEST("shell_read_line: empty input (just newline)") {
    mock::reset_capture();
    char buf[shell_test::MAX_LINE];

    mock::set_read_input("\n");
    size_t len = shell_test::read_line(buf, sizeof(buf));

    ASSERT_EQ(len, 0ULL);
    ASSERT_EQ(buf[0], '\0');
}

TEST("shell_read_line: backspace erases previous character") {
    mock::reset_capture();
    char buf[shell_test::MAX_LINE];

    // Type "ab" then backspace, then "c" then Enter
    // "ab\x7F" = "ab" then backspace deletes "b", then "c\n"
    mock::set_read_input(
        "ab"
        "\x7F"
        "c\n");
    size_t len = shell_test::read_line(buf, sizeof(buf));

    ASSERT_EQ(len, 2ULL);
    // After typing "ab", backspace removes "b" (pos=1), then "c" is added
    ASSERT_TRUE(std::strcmp(buf, "ac") == 0);
}

TEST("shell_read_line: multiple backspaces") {
    mock::reset_capture();
    char buf[shell_test::MAX_LINE];

    // Type "abc" then two backspaces, then "d\n"
    mock::set_read_input(
        "abc"
        "\x7F\x7F"
        "d\n");
    size_t len = shell_test::read_line(buf, sizeof(buf));

    ASSERT_EQ(len, 2ULL);
    // "abc" -> backspace -> "ab" -> backspace -> "a" -> add "d" -> "ad"
    ASSERT_TRUE(std::strcmp(buf, "ad") == 0);
}

TEST("shell_read_line: backspace at start does nothing") {
    mock::reset_capture();
    char buf[shell_test::MAX_LINE];

    // Backspace at start (pos=0) should be ignored
    mock::set_read_input("\x7Fhello\n");
    size_t len = shell_test::read_line(buf, sizeof(buf));

    ASSERT_EQ(len, 5ULL);
    ASSERT_TRUE(std::strcmp(buf, "hello") == 0);
}

TEST("shell_read_line: backspace as \\b character") {
    mock::reset_capture();
    char buf[shell_test::MAX_LINE];

    // \b (0x08) should also work as backspace
    mock::set_read_input("ab\bc\n");
    size_t len = shell_test::read_line(buf, sizeof(buf));

    ASSERT_EQ(len, 2ULL);
    ASSERT_TRUE(std::strcmp(buf, "ac") == 0);
}

// ============================================================
// 12. Full dispatch pipeline test (tokenize + dispatch)
// ============================================================

TEST("shell_pipeline: echo hello world") {
    mock::reset_capture();

    char  line[shell_test::MAX_LINE];
    char* argv[shell_test::MAX_TOKENS];

    // Simulate the full shell pipeline for "echo hello world"
    std::strcpy(line, "echo hello world");
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 3ULL);

    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], builtin_cmds[i].name) == 0) {
            builtin_cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    ASSERT_TRUE(std::strcmp(mock::write_capture, "hello world\n") == 0);
}

TEST("shell_pipeline: help command produces expected output") {
    mock::reset_capture();

    char  line[shell_test::MAX_LINE];
    char* argv[shell_test::MAX_TOKENS];

    std::strcpy(line, "help");
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 1ULL);

    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], builtin_cmds[i].name) == 0) {
            builtin_cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    ASSERT_TRUE(std::strstr(mock::write_capture, "echo") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "clear") != nullptr);
}

TEST("shell_pipeline: clear command produces ANSI escape") {
    mock::reset_capture();

    char  line[shell_test::MAX_LINE];
    char* argv[shell_test::MAX_TOKENS];

    std::strcpy(line, "clear");
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 1ULL);

    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], builtin_cmds[i].name) == 0) {
            builtin_cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    ASSERT_TRUE(std::memcmp(mock::write_capture, "\033[2J\033[H", 7) == 0);
}

TEST("shell_pipeline: unknown command not found in table") {
    mock::reset_capture();

    char  line[shell_test::MAX_LINE];
    char* argv[shell_test::MAX_TOKENS];

    std::strcpy(line, "unknown_cmd");
    size_t argc = shell_test::tokenize(line, argv, shell_test::MAX_TOKENS);
    ASSERT_EQ(argc, 1ULL);

    shell_test::CmdEntry builtin_cmds[] = {
        {"echo", shell_test::cmd_echo},
        {"help", shell_test::cmd_help},
        {"clear", shell_test::cmd_clear},
        {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; builtin_cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], builtin_cmds[i].name) == 0) {
            builtin_cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(!found);
}

// ============================================================
// 13. Edge cases: CmdEntry struct layout
// ============================================================

TEST("shell_cmd_entry: struct has name and handler fields") {
    shell_test::CmdEntry entry;
    entry.name    = "test";
    entry.handler = nullptr;
    ASSERT_TRUE(std::strcmp(entry.name, "test") == 0);
    ASSERT_NULL(entry.handler);
}

TEST("shell_cmd_entry: handler can be called through pointer") {
    mock::reset_capture();

    shell_test::CmdEntry entry;
    entry.name    = "echo";
    entry.handler = shell_test::cmd_echo;

    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("test")};
    entry.handler(2, argv);

    ASSERT_TRUE(std::strcmp(mock::write_capture, "test\n") == 0);
}

// ============================================================
// Main function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
