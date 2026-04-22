/**
 * @file kernel/syscall/sys_close.cpp
 * @brief sys_close handler implementation
 *
 * Closes a file descriptor by releasing the File entry in the
 * global FDTable.
 */

#include "kernel/syscall/sys_close.hpp"

#include <stdint.h>

#include "kernel/fs/vfs_mount.hpp"
#include "kernel/fs/file.hpp"

namespace cinux::syscall {

int64_t sys_close(uint64_t fd, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    int result = cinux::fs::g_global_fd_table().close(static_cast<int>(fd));
    return static_cast<int64_t>(result);
}

}  // namespace cinux::syscall
