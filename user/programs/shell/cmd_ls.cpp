/**
 * @file user/programs/shell/cmd_ls.cpp
 * @brief Built-in 'ls' command implementation
 *
 * Lists the contents of a directory.
 * Usage: ls [path]   (defaults to '/' if no path given)
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

void cmd_ls(int argc, char** argv) {
    const char* path = (argc >= 2) ? argv[1] : "/";

    int64_t fd = sys_open(path, 0 /* O_RDONLY */);
    if (fd < 0) {
        write_str("ls: cannot open '");
        write_str(path);
        write_str("'\n");
        return;
    }

    constexpr size_t NAME_MAX = 256;
    char name[NAME_MAX];

    while (true) {
        int64_t n = sys_getdents(static_cast<int>(fd), name, NAME_MAX);
        if (n <= 0) {
            break;
        }
        // Print the name followed by a newline
        sys_write(1, name, static_cast<size_t>(n));
        sys_write(1, "\n", 1);
    }

    sys_close(static_cast<int>(fd));
}
