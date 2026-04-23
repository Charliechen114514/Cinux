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
int64_t sys_creat(const char* path);
int64_t sys_mkdir(const char* path);
int64_t sys_unlink(const char* path);
int64_t sys_rmdir(const char* path);
int64_t sys_chdir(const char* path);
int64_t sys_getcwd(char* buf, size_t size);
void sys_exit(int code);
void sys_yield(void);

struct sys_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

int64_t sys_stat(const char* path, struct sys_stat* st);
int64_t sys_fstat(int fd, struct sys_stat* st);
