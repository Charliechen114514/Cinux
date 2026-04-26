/**
 * @file kernel/syscall/sys_getpid.hpp
 * @brief sys_getpid handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Get the process ID of the calling task
 *
 * @return The PID of the current task, or -1 if no task is running
 */
int64_t sys_getpid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
