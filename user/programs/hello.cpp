#include "libc/syscall.h"

extern "C" void _start() {
    const char msg[] = "[USER] Hello from Ring 3!\n";
    sys_write(1, msg, 26);
    sys_exit(0);
}
