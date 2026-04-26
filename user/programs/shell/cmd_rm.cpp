/**
 * @file user/programs/shell/cmd_rm.cpp
 * @brief Built-in 'rm' command implementation
 *
 * Removes a file at the given path via sys_unlink.
 * Usage: rm <path>
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

void cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        write_str("rm: missing file operand\n");
        return;
    }

    const char* path   = argv[1];
    int64_t     result = sys_unlink(path);

    if (result < 0) {
        write_str("rm: cannot remove '");
        write_str(path);
        write_str("'\n");
    }
}
