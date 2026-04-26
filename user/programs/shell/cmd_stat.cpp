/**
 * @file user/programs/shell/cmd_stat.cpp
 * @brief Built-in 'stat' command implementation
 *
 * Displays file status information.
 * Usage: stat <path>
 */

#include "libc/string.hpp"
#include "libc/syscall.h"
#include "shell.hpp"

using cinux::user::strlen;

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

void write_uint64(uint64_t val) {
    char buf[21];
    int  pos = 0;

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        uint64_t tmp = val;
        while (tmp > 0) {
            buf[pos++] = '0' + static_cast<char>(tmp % 10);
            tmp /= 10;
        }
    }

    // Reverse
    for (int i = 0; i < pos / 2; ++i) {
        char t           = buf[i];
        buf[i]           = buf[pos - 1 - i];
        buf[pos - 1 - i] = t;
    }

    buf[pos] = '\0';
    write_str(buf);
}

void write_octal(uint32_t val) {
    char buf[12];
    int  pos = 0;

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        uint32_t tmp = val;
        while (tmp > 0) {
            buf[pos++] = '0' + static_cast<char>(tmp & 7);
            tmp >>= 3;
        }
    }

    // Reverse
    for (int i = 0; i < pos / 2; ++i) {
        char t           = buf[i];
        buf[i]           = buf[pos - 1 - i];
        buf[pos - 1 - i] = t;
    }

    buf[pos] = '\0';
    write_str(buf);
}

}  // anonymous namespace

void cmd_stat(int argc, char** argv) {
    if (argc < 2) {
        write_str("stat: missing operand\n");
        return;
    }

    struct sys_stat st;
    int64_t         ret = sys_stat(argv[1], &st);
    if (ret < 0) {
        write_str("stat: cannot stat '");
        write_str(argv[1]);
        write_str("'\n");
        return;
    }

    write_str("  File: ");
    write_str(argv[1]);
    write_str("\n");

    write_str("  Size: ");
    write_uint64(static_cast<uint64_t>(st.st_size));
    write_str("\tBlocks: ");
    write_uint64(st.st_blocks);
    write_str("\n");

    write_str("  Inode: ");
    write_uint64(st.st_ino);
    write_str("\tLinks: ");
    write_uint64(st.st_nlink);
    write_str("\n");

    write_str("  Mode: ");
    write_octal(st.st_mode);
    write_str("\tUid: ");
    write_uint64(st.st_uid);
    write_str("\tGid: ");
    write_uint64(st.st_gid);
    write_str("\n");
}
