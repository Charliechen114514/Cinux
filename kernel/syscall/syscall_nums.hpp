/**
 * @file kernel/syscall/syscall_nums.hpp
 * @brief System call number constants
 *
 * Defines the syscall numbers used by user-space programs to request
 * kernel services via the SYSCALL instruction.  These numbers are
 * shared between kernel dispatch and the user-space libc wrapper.
 *
 * Convention: numbers match Linux x86_64 where practical to simplify
 * future porting of user programs.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

namespace cinux::syscall {

enum class SyscallNr : uint64_t {
    SYS_read     = 0,
    SYS_write    = 1,
    SYS_open     = 2,
    SYS_close    = 3,
    SYS_stat     = 4,
    SYS_fstat    = 5,
    SYS_chdir    = 12,
    SYS_exit     = 60,
    SYS_yield    = 24,
    SYS_getcwd   = 79,
    SYS_getdents = 78,
    SYS_mkdir    = 83,
    SYS_rmdir    = 84,
    SYS_creat    = 85,
    SYS_unlink   = 87,
    SYS_pipe     = 22,
    SYS_getpid   = 39,
    SYS_getppid  = 110,
    SYS_fork     = 57,
    SYS_execve   = 59,
    SYS_waitpid  = 61,
};

constexpr uint64_t SYSCALL_TABLE_SIZE = 256;

}  // namespace cinux::syscall
