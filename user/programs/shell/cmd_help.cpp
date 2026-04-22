/**
 * @file user/programs/shell/cmd_help.cpp
 * @brief Built-in 'help' command implementation
 *
 * Prints a list of available shell commands.
 */

#include "shell.hpp"
#include "libc/string.hpp"
#include "libc/syscall.h"

using cinux::user::strlen;

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

}  // anonymous namespace

void cmd_help(int /*argc*/, char** /*argv*/) {
    write_str(
        "Available commands:\n"
        "  echo <args...>  - print arguments to stdout\n"
        "  help            - show this help message\n"
        "  clear           - clear the screen\n"
        "  cat <path>      - print file contents\n"
        "  ls [path]       - list directory contents\n"
    );
}
