/**
 * @file kernel/syscall/sys_open.cpp
 * @brief sys_open handler implementation
 *
 * Resolves a path through the VFS, looks up the Inode, and allocates
 * a file descriptor in the global FDTable.
 */

#include "kernel/syscall/sys_open.hpp"

#include <stdint.h>

#include "kernel/fs/vfs_mount.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::syscall {

int64_t sys_open(uint64_t path_virt, uint64_t flags, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    // Reject null and non-canonical addresses.
    // x86_64 canonical: bits 48-63 must all equal bit 47.
    if (path_virt == 0) {
        return -1;
    }
    uint64_t bit47 = (path_virt >> 47) & 1;
    uint64_t upper = path_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -1;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -1;
    }

    if (path[0] == '\0') {
        return -1;
    }

    // Step 1: Resolve through the VFS mount table
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(path, &rel_path);

    if (fs == nullptr) {
        cinux::lib::kprintf("[SYS_OPEN] No filesystem mounted for '%s'\n", path);
        return -1;
    }

    // Step 2: Look up the Inode in the backend filesystem
    cinux::fs::Inode* inode = fs->lookup(rel_path);

    if (inode == nullptr) {
        cinux::lib::kprintf("[SYS_OPEN] File not found: '%s'\n", path);
        return -1;
    }

    // Step 3: Convert flags to OpenFlags
    cinux::fs::OpenFlags open_flags;
    switch (flags) {
        case 0:  open_flags = cinux::fs::OpenFlags::RDONLY; break;
        case 1:  open_flags = cinux::fs::OpenFlags::WRONLY; break;
        case 2:  open_flags = cinux::fs::OpenFlags::RDWR;   break;
        default: open_flags = cinux::fs::OpenFlags::RDONLY; break;
    }

    // Step 4: Allocate a file descriptor
    int fd = cinux::fs::g_global_fd_table().alloc(inode, open_flags);

    if (fd == cinux::fs::FD_NONE) {
        cinux::lib::kprintf("[SYS_OPEN] FD table full, cannot open '%s'\n", path);
        return -1;
    }

    return static_cast<int64_t>(fd);
}

}  // namespace cinux::syscall
