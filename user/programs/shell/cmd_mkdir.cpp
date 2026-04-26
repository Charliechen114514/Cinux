/**
 * @file user/programs/shell/cmd_mkdir.cpp
 * @brief Built-in 'mkdir' command implementation
 *
 * Creates a new directory at the given path via sys_mkdir.
 * Usage: mkdir <path>
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

void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        write_str("mkdir: missing directory operand\n");
        return;
    }

    const char* path   = argv[1];
    int64_t     result = sys_mkdir(path);

    if (result < 0) {
        write_str("mkdir: cannot create directory '");
        write_str(path);
        write_str("'\n");
    }
}
