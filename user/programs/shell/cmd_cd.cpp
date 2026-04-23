/**
 * @file user/programs/shell/cmd_cd.cpp
 * @brief Built-in 'cd' command implementation
 *
 * Changes the current working directory.
 * Usage: cd <path>
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

void cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        write_str("cd: missing operand\n");
        return;
    }

    int64_t ret = sys_chdir(argv[1]);
    if (ret < 0) {
        write_str("cd: cannot change to '");
        write_str(argv[1]);
        write_str("'\n");
    }
}
