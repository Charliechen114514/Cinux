/**
 * @file kernel/syscall/sys_getdents.hpp
 * @brief sys_getdents handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Read a single directory entry name into a user buffer
 *
 * Uses the file descriptor's offset as the directory entry index.
 * On success the offset is advanced by one and the entry name length
 * is returned.  Returns 0 when no more entries remain, -1 on error.
 *
 * @param fd         File descriptor pointing to an open directory
 * @param buf_virt   User virtual address of the destination buffer
 * @param count      Size of the destination buffer
 */
int64_t sys_getdents(uint64_t fd, uint64_t buf_virt, uint64_t count,
                     uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
