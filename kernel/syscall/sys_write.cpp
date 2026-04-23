/**
 * @file kernel/syscall/sys_write.cpp
 * @brief sys_write handler implementation
 *
 * For fd=1 (stdout): outputs to serial + Console via kprintf.
 * For other fds: writes through VFS (fd -> File -> Inode -> ops -> write).
 */

#include "kernel/syscall/sys_write.hpp"

#include <stdint.h>

#include "kernel/lib/kprintf.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/fs/file.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count,
                  uint64_t, uint64_t, uint64_t) {
    if (buf_virt == 0) {
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

    // Check FDTable first -- if the fd has a valid VFS entry (e.g. pipe),
    // use the VFS write path regardless of fd number.
    cinux::fs::File* file = cinux::fs::g_global_fd_table().get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        const auto* buf = reinterpret_cast<const void*>(buf_virt);
        auto g = file->offset_lock_.guard();
        (void)g;
        int64_t result = file->inode->ops->write(file->inode, file->offset, buf, count);
        if (result > 0) {
            file->offset += static_cast<uint64_t>(result);
        }
        return result;
    }

    // fd=1 (stdout): legacy kprintf output path when no VFS entry is present
    if (fd == 1) {
        const auto* buf = reinterpret_cast<const char*>(buf_virt);
        for (uint64_t i = 0; i < count; i++) {
            kprintf("%c", buf[i]);
        }

        return static_cast<int64_t>(count);
    }

    // No VFS entry and not a legacy fd -- fail
    return -1;
}

}  // namespace cinux::syscall
