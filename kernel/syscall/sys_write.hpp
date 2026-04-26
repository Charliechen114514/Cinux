/**
 * @file kernel/syscall/sys_write.hpp
 * @brief sys_write handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Write data from a user buffer to a file descriptor
 *
 * For fd=1 (stdout), outputs to serial + Console via kprintf.
 * Validates that the user buffer resides below USER_ADDR_MAX.
 *
 * @param fd       File descriptor (only fd=1 is supported)
 * @param buf_virt User virtual address of the buffer
 * @param count    Number of bytes to write
 * @return Number of bytes written, or -1 on error
 */
int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
