/**
 * @file user/libc/syscall.c
 * @brief User-space system call wrapper implementations
 *
 * Each wrapper loads the syscall number from the shared syscall_nums
 * constants and executes the SYSCALL instruction with the appropriate
 * arguments.
 */

#include "syscall.h"
#include "kernel/syscall/syscall_nums.hpp"

static inline int64_t _syscall1(uint64_t nr, uint64_t a1) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t _syscall2(uint64_t nr, uint64_t a1, uint64_t a2) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static inline int64_t _syscall3(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory"
    );
    return ret;
}

using cinux::syscall::SyscallNr;

int64_t sys_open(const char* path, int flags) {
    return _syscall2(static_cast<uint64_t>(SyscallNr::SYS_open),
                     (uint64_t)path, (uint64_t)flags);
}

int64_t sys_close(int fd) {
    return _syscall1(static_cast<uint64_t>(SyscallNr::SYS_close),
                     (uint64_t)fd);
}

int64_t sys_read(int fd, void* buf, size_t count) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_read),
                     (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int64_t sys_write(int fd, const void* buf, size_t count) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_write),
                     (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

int64_t sys_getdents(int fd, void* buf, size_t count) {
    return _syscall3(static_cast<uint64_t>(SyscallNr::SYS_getdents),
                     (uint64_t)fd, (uint64_t)buf, (uint64_t)count);
}

void sys_exit(int code) {
    _syscall1(static_cast<uint64_t>(SyscallNr::SYS_exit), (uint64_t)code);
    __builtin_unreachable();
}

void sys_yield(void) {
    _syscall1(static_cast<uint64_t>(SyscallNr::SYS_yield), 0);
}
