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
    SYS_read   = 0,
    SYS_write  = 1,
    SYS_exit   = 60,
    SYS_yield  = 24,
};

constexpr uint64_t SYSCALL_TABLE_SIZE = 256;

}  // namespace cinux::syscall
