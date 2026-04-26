/**
 * @file user/programs/shell/cmd_rmdir.cpp
 * @brief Built-in 'rmdir' command implementation
 *
 * Removes an empty directory at the given path via sys_rmdir.
 * Usage: rmdir <path>
 */

#include "libc/string.hpp"
#include "libc/syscall.h"
#include "shell.hpp"

using cinux::user::strlen;

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

}  // anonymous namespace

void cmd_rmdir(int argc, char** argv) {
    if (argc < 2) {
        write_str("rmdir: missing directory operand\n");
        return;
    }

    const char* path   = argv[1];
    int64_t     result = sys_rmdir(path);

    if (result < 0) {
        write_str("rmdir: cannot remove '");
        write_str(path);
        write_str("'\n");
    }
}
