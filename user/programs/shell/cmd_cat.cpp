/**
 * @file user/programs/shell/cmd_cat.cpp
 * @brief Built-in 'cat' command implementation
 *
 * Prints the contents of a file to stdout.
 * Usage: cat <path>
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

void cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        write_str("cat: missing file argument\n");
        return;
    }

    const char* path = argv[1];
    int64_t fd = sys_open(path, 0);
    if (fd < 0) {
        write_str("cat: cannot open '");
        write_str(path);
        write_str("'\n");
        return;
    }

    // Read and output in chunks
    constexpr size_t BUF_SIZE = 256;
    char buf[BUF_SIZE];

    while (true) {
        int64_t n = sys_read(static_cast<int>(fd), buf, BUF_SIZE);
        if (n <= 0) {
            break;
        }
        sys_write(1, buf, static_cast<size_t>(n));
    }

    sys_close(static_cast<int>(fd));
}
