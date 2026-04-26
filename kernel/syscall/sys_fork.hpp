/**
 * @file kernel/syscall/sys_fork.hpp
 * @brief sys_fork handler declaration
 *
 * Creates a child process as a copy of the calling process using
 * Copy-On-Write page table semantics.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Fork the current process
 *
 * @return Child PID to the parent, 0 to the child, or -1 on error
 */
int64_t sys_fork(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
