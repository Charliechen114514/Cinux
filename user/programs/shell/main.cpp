/**
 * @file user/programs/shell/main.cpp
 * @brief Shell REPL main loop, line reader, tokenizer, and dispatch
 *
 * Entry point for the Cinux user-space shell.  Runs the main loop:
 *   print_prompt -> read_line -> tokenize -> dispatch -> repeat
 *
 * The builtin command table is defined here; each command handler lives
 * in its own .cpp file (cmd_echo.cpp, cmd_help.cpp, cmd_clear.cpp).
 */

#include "shell.hpp"
#include "libc/string.hpp"
#include "libc/syscall.h"

using cinux::user::strcmp;
using cinux::user::strlen;

// ============================================================
// Constants
// ============================================================

/// Maximum input line length (including NUL terminator)
constexpr size_t MAX_LINE = 256;

/// Maximum number of tokens after splitting
constexpr size_t MAX_TOKENS = 16;

/// Shell prompt string
constexpr const char PROMPT[] = "cinux> ";

// ============================================================
// I/O helpers
// ============================================================

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

void write_buf(const char* buf, size_t len) {
    sys_write(1, buf, len);
}

}  // anonymous namespace

// ============================================================
// Line reading
// ============================================================

namespace {

/**
 * @brief Read one line from stdin into @p buf
 *
 * Reads characters one at a time via sys_read(0, ...).
 * Backspace (0x7F) / '\\b' erases the previous character.
 * Enter ('\\n') terminates the line.  The buffer is NUL-terminated;
 * the trailing newline is NOT stored.
 *
 * @param buf Destination buffer
 * @param cap Buffer capacity (including NUL)
 * @return Number of characters stored (excluding NUL)
 */
size_t read_line(char* buf, size_t cap) {
    size_t pos = 0;

    while (pos < cap - 1) {
        char c = 0;
        int64_t n = sys_read(0, &c, 1);
        if (n <= 0) {
            continue;
        }

        if (c == '\n') {
            write_buf("\n", 1);
            break;
        }

        if (c == 0x7F || c == '\b') {
            if (pos > 0) {
                --pos;
                write_buf("\b \b", 3);
            }
            continue;
        }

        // Echo back and store
        write_buf(&c, 1);
        buf[pos++] = c;
    }

    buf[pos] = '\0';
    return pos;
}

}  // anonymous namespace

// ============================================================
// Tokenizer
// ============================================================

namespace {

/**
 * @brief Tokenize a line by whitespace, in-place
 *
 * Splits @p line on space (' ') and tab ('\\t').
 * Token pointers are stored in @p argv.  The string @p line
 * is modified (whitespace replaced with NUL).
 *
 * @param line        Mutable input line
 * @param argv        Output array of token pointers
 * @param max_tokens  Maximum number of tokens to store
 * @return Number of tokens produced (argc)
 */
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

}  // anonymous namespace

// ============================================================
// Command dispatch table (data-driven)
// ============================================================

namespace {

/// Built-in command dispatch table.
/// To add a new command: implement cmd_xxx in cmd_xxx.cpp, declare
/// it in shell.hpp, and add an entry here.
constexpr CmdEntry builtin_cmds[] = {
    {"echo",  cmd_echo},
    {"help",  cmd_help},
    {"clear", cmd_clear},
    {"cat",   cmd_cat},
    {"ls",    cmd_ls},
    {"touch", cmd_touch},
    {"mkdir", cmd_mkdir},
    {"rm",    cmd_rm},
    {"rmdir", cmd_rmdir},
    {"cd",    cmd_cd},
    {"pwd",   cmd_pwd},
    {"stat",  cmd_stat},
    {nullptr, nullptr},
};

}  // anonymous namespace

// ============================================================
// Shell main loop
// ============================================================

static void shell_main() {
    char  line[MAX_LINE];
    char* argv[MAX_TOKENS];

    write_str("Cinux shell - type 'help' for commands\n");

    while (true) {
        // Print prompt
        write_str(PROMPT);

        // Read input line
        size_t len = read_line(line, MAX_LINE);
        if (len == 0) {
            continue;
        }

        // Tokenize
        size_t argc = tokenize(line, argv, MAX_TOKENS);
        if (argc == 0) {
            continue;
        }

        // Dispatch through builtin command table
        bool found = false;
        for (size_t i = 0; builtin_cmds[i].name != nullptr; ++i) {
            if (strcmp(argv[0], builtin_cmds[i].name) == 0) {
                builtin_cmds[i].handler(static_cast<int>(argc), argv);
                found = true;
                break;
            }
        }

        if (!found) {
            write_str(argv[0]);
            write_str(": command not found\n");
        }
    }
}

// ============================================================
// ELF entry point
// ============================================================

extern "C" {

void _start() {
    shell_main();
    sys_exit(0);
}

}  // extern "C"
