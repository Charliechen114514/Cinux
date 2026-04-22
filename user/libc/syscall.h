/**
 * @file user/libc/syscall.h
 * @brief User-space system call wrappers
 *
 * Provides C function wrappers for the SYSCALL instruction.
 * Syscall numbers are shared with the kernel via syscall_nums.hpp.
 *
 * Syscall convention (Linux x86_64):
 *   RAX = syscall number
 *   RDI = arg1, RSI = arg2, RDX = arg3
 *   R10 = arg4, R8  = arg5, R9  = arg6
 *   RAX = return value
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

int64_t sys_open(const char* path, int flags);
int64_t sys_close(int fd);
int64_t sys_read(int fd, void* buf, size_t count);
int64_t sys_write(int fd, const void* buf, size_t count);
int64_t sys_getdents(int fd, void* buf, size_t count);
void sys_exit(int code);
void sys_yield(void);
