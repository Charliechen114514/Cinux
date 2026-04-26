/**
 * @file kernel/syscall/sys_waitpid.hpp
 * @brief sys_waitpid handler declaration
 *
 * Waits for a child process to change state and collects its exit status.
 * Reaps zombie child processes to prevent resource leaks.
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Wait for a child process to change state
 *
 * If pid > 0, waits for the specific child with that PID.
 * If pid == -1, waits for any child process.
 *
 * @param pid       PID of child to wait for, or -1 for any child
 * @param status    User virtual address of int to store exit status
 * @return Child PID on success, 0 if child not yet exited, or negative errno
 */
int64_t sys_waitpid(uint64_t pid, uint64_t status, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
