/**
 * @file user/programs/shell/cmd_echo.cpp
 * @brief Built-in 'echo' command implementation
 *
 * Prints all arguments separated by single spaces, followed by a newline.
 * Supports output redirection: echo <text> > <path>
 * When '>' is found among arguments, the text before '>' is written
 * to the file specified after '>'.
 */

#include "libc/string.hpp"
#include "libc/syscall.h"
#include "shell.hpp"

using cinux::user::strlen;
using cinux::user::strcmp;

namespace {

void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

}  // anonymous namespace

void cmd_echo(int argc, char** argv) {
    // Search for the '>' redirect token among arguments
    int redirect_idx = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], ">") == 0) {
            redirect_idx = i;
            break;
        }
    }

    if (redirect_idx > 0 && redirect_idx + 1 < argc) {
        // Redirect mode: echo <text...> > <path>
        const char* path = argv[redirect_idx + 1];

        // Create the file first
        int64_t creat_result = sys_creat(path);
        if (creat_result < 0) {
            write_str("echo: cannot create '");
            write_str(path);
            write_str("'\n");
            return;
        }

        // Open the file for writing (O_WRONLY = 1)
        int64_t fd = sys_open(path, 1);
        if (fd < 0) {
            write_str("echo: cannot open '");
            write_str(path);
            write_str("'\n");
            return;
        }

        // Write arguments before '>' to the file
        for (int i = 1; i < redirect_idx; ++i) {
            if (i > 1) {
                const char space[] = " ";
                sys_write(static_cast<int>(fd), space, 1);
            }
            sys_write(static_cast<int>(fd), argv[i], strlen(argv[i]));
        }

        // Write trailing newline
        const char nl[] = "\n";
        sys_write(static_cast<int>(fd), nl, 1);

        sys_close(static_cast<int>(fd));
        return;
    }

    // Normal mode: print to stdout
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            sys_write(1, " ", 1);
        }
        write_str(argv[i]);
    }
    sys_write(1, "\n", 1);
}
