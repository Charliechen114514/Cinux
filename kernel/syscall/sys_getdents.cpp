/**
 * @file kernel/syscall/sys_getdents.cpp
 * @brief sys_getdents handler implementation
 *
 * Reads one directory entry name per call via VFS readdir.
 * Uses file->offset as the directory entry index so that repeated
 * calls iterate through all entries.
 */

#include "kernel/syscall/sys_getdents.hpp"

#include <stdint.h>

#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::syscall {

int64_t sys_getdents(uint64_t fd, uint64_t buf_virt, uint64_t count, uint64_t, uint64_t, uint64_t) {
    // Validate user buffer address
    if (buf_virt == 0 || count == 0) {
        return -1;
    }
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -1;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -1;
    }

    // Look up the file descriptor
    cinux::fs::File* file = cinux::fs::current_fd_table().get(static_cast<int>(fd));
    if (file == nullptr || file->inode == nullptr || file->inode->ops == nullptr) {
        return -1;
    }

    auto* name_buf = reinterpret_cast<char*>(buf_virt);
    {
        auto g = file->offset_lock_.guard();
        (void)g;
        int64_t result = file->inode->ops->readdir(file->inode, file->offset, name_buf, count);

        if (result == 1) {
            file->offset++;
            uint64_t len = 0;
            while (len < count && name_buf[len] != '\0') {
                ++len;
            }
            return static_cast<int64_t>(len);
        }

        return result;
    }
}

}  // namespace cinux::syscall
