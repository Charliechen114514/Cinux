/**
 * @file kernel/syscall/sys_stat.hpp
 * @brief sys_stat and sys_fstat handler declarations
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Get file status by path
 *
 * Resolves the path through the VFS and fills a struct stat.
 *
 * @param path_virt  User virtual address of the null-terminated path string
 * @param st_virt    User virtual address of the struct stat output buffer
 * @return 0 on success, or -1 on error
 */
int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t);

/**
 * @brief Get file status by file descriptor
 *
 * Looks up the FD in the global FD table and fills a struct stat.
 *
 * @param fd         File descriptor index
 * @param st_virt    User virtual address of the struct stat output buffer
 * @return 0 on success, or -1 on error
 */
int64_t sys_fstat(uint64_t fd, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
