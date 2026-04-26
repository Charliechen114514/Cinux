/**
 * @file kernel/syscall/sys_getppid.hpp
 * @brief sys_getppid handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Get the parent process ID of the calling task
 *
 * @return The PPID of the current task, or -1 if no task is running
 */
int64_t sys_getppid(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
