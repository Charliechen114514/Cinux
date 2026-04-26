/**
 * @file user/programs/shell/cmd_clear.cpp
 * @brief Built-in 'clear' command implementation
 *
 * Sends the ANSI escape sequence ESC[2J ESC[H] to clear the terminal
 * screen and move the cursor to the home position.
 */

#include "libc/syscall.h"
#include "shell.hpp"

void cmd_clear(int /*argc*/, char** /*argv*/) {
    // 7 bytes: ESC[2J (clear screen) + ESC[H (cursor home)
    sys_write(1, "\033[2J\033[H", 7);
}
