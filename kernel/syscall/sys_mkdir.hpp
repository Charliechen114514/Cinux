/**
 * @file kernel/syscall/sys_mkdir.hpp
 * @brief sys_mkdir handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Create a new directory at the given path
 *
 * Resolves the path through the VFS mount table, splits it into
 * parent directory and leaf name, then invokes InodeOps::mkdir()
 * on the parent directory.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @return 0 on success, or -1 on error
 */
int64_t sys_mkdir(uint64_t path_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
