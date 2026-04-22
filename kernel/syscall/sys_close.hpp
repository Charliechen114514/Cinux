/**
 * @file kernel/syscall/sys_close.hpp
 * @brief sys_close handler declaration
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Close a file descriptor
 *
 * Releases the File entry associated with the given descriptor
 * in the global FDTable.
 *
 * @param fd  File descriptor to close
 * @return 0 on success, or -1 on error
 */
int64_t sys_close(uint64_t fd, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
