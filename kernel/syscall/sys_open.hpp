/**
 * @file kernel/syscall/sys_open.hpp
 * @brief sys_open handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Open a file and return a file descriptor
 *
 * Resolves the path through the VFS mount table, looks up the
 * corresponding Inode in the backend filesystem, and allocates
 * a file descriptor in the global FDTable.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @param flags      Access mode (0=RDONLY, 1=WRONLY, 2=RDWR)
 * @return Non-negative file descriptor on success, or -1 on error
 */
int64_t sys_open(uint64_t path_virt, uint64_t flags, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
