/**
 * @file user/programs/shell/cmd_touch.cpp
 * @brief Built-in 'touch' command implementation
 *
 * Creates a new empty file at the given path via sys_creat.
 * Usage: touch <path>
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

void cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        write_str("touch: missing file operand\n");
        return;
    }

    const char* path   = argv[1];
    int64_t     result = sys_creat(path);

    if (result < 0) {
        write_str("touch: cannot create '");
        write_str(path);
        write_str("'\n");
    }
}
