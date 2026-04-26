/**
 * @file kernel/syscall/sys_exit.hpp
 * @brief sys_exit handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Terminate the current task with an exit code
 *
 * Marks the current task as Dead and yields to the scheduler.
 * Does not return.
 *
 * @param code  Exit code (0 = success)
 * @return Should not return; scheduler picks the next task
 */
int64_t sys_exit(uint64_t code, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
