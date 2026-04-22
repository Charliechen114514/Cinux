/**
 * @file user/programs/shell/shell.hpp
 * @brief Shared declarations for the Cinux shell commands and dispatch
 *
 * Defines the CmdEntry dispatch-table struct and declares all built-in
 * command handler functions.  Each command lives in its own .cpp file
 * for easy extension.
 *
 * The dispatch table itself is defined in main.cpp so the linker can
 * see all handler symbols.
 */

#pragma once

#include <cstddef>

// ============================================================
// Command dispatch table entry
// ============================================================

/// Maps a command name to a handler function.
/// Commands that need argc/argv take both; those that don't take none.
struct CmdEntry {
    const char* name;
    void (*handler)(int argc, char** argv);
};

// ============================================================
// Built-in command handlers (one per .cpp file)
// ============================================================

/** @brief echo: print arguments separated by spaces (cmd_echo.cpp) */
void cmd_echo(int argc, char** argv);

/** @brief help: print list of available commands (cmd_help.cpp) */
void cmd_help(int argc, char** argv);

/** @brief clear: send ANSI escape to clear screen (cmd_clear.cpp) */
void cmd_clear(int argc, char** argv);

/** @brief cat: print file contents to stdout (cmd_cat.cpp) */
void cmd_cat(int argc, char** argv);

/** @brief ls: list directory contents (cmd_ls.cpp) */
void cmd_ls(int argc, char** argv);
