/**
 * @file user/programs/shell/cmd_pwd.cpp
 * @brief Built-in 'pwd' command implementation
 *
 * Prints the current working directory.
 * Usage: pwd
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

void cmd_pwd(int, char**) {
    char buf[256];
    int64_t ret = sys_getcwd(buf, sizeof(buf));
    if (ret < 0) {
        write_str("pwd: failed to get cwd\n");
        return;
    }

    write_str(buf);
    write_str("\n");
}
