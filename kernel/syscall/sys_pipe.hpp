/**
 * @file kernel/syscall/sys_pipe.hpp
 * @brief sys_pipe handler declaration
 *
 * Creates an anonymous unidirectional pipe and installs two file
 * descriptors in the calling process's FDTable:
 *   - pipefd[0] is the read end
 *   - pipefd[1] is the write end
 *
 * Namespace: cinux::syscall
 */

#pragma once

#include <stdint.h>
#include "kernel/arch/x86_64/syscall.hpp"

namespace cinux::syscall {

/**
 * @brief Create an anonymous pipe and return two file descriptors
 *
 * Creates a Pipe object on the kernel heap, wraps each end in an
 * Inode with the appropriate PipeReadOps / PipeWriteOps, allocates
 * two fd slots in the global FDTable, and writes the descriptor
 * numbers into the user-space int array at @p pipefd_virt.
 *
 * @param pipefd_virt  User virtual address of a 2-element int array
 *                     (pipefd[0] = read end, pipefd[1] = write end)
 * @return 0 on success, -1 on error
 */
int64_t sys_pipe(uint64_t pipefd_virt,
                 uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

}  // namespace cinux::syscall
