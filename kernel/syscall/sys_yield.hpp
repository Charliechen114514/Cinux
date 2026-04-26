/**
 * @file kernel/syscall/sys_yield.hpp
 * @brief sys_yield handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Yield the CPU to the next ready task
 *
 * Delegates directly to Scheduler::yield().
 *
 * @return 0 on success
 */
int64_t sys_yield(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
