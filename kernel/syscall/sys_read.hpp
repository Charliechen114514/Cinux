/**
 * @file kernel/syscall/sys_read.hpp
 * @brief sys_read handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Read data from a file descriptor into a user buffer
 *
 * For fd=0 (stdin), reads keyboard characters from the PS/2 ring buffer.
 * Validates that the user buffer resides below USER_ADDR_MAX.
 *
 * @param fd       File descriptor (only fd=0 is supported)
 * @param buf_virt User virtual address of the destination buffer
 * @param count    Maximum number of bytes to read
 * @return Number of bytes read, or -1 on error
 */
int64_t sys_read(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
