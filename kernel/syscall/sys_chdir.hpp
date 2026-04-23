/**
 * @file kernel/syscall/sys_chdir.hpp
 * @brief sys_chdir handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Change the current working directory
 *
 * Resolves the path through the VFS, verifies it is a directory,
 * and updates the current task's cwd.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @return 0 on success, or -1 on error
 */
int64_t sys_chdir(uint64_t path_virt, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
