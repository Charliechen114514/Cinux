/**
 * @file kernel/syscall/sys_stat.cpp
 * @brief sys_stat and sys_fstat handler implementations
 *
 * sys_stat resolves a path through the VFS, calls InodeOps::stat(),
 * and copies the result to the user buffer.
 * sys_fstat looks up an FD in the global FD table and does the same.
 */

#include "kernel/syscall/sys_stat.hpp"

#include <stdint.h>

#include "kernel/fs/file.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/stat.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(st_virt)) {
        return -1;
    }

    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }

    // Step 2: Resolve through the VFS mount table
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_STAT] No filesystem mounted for '%s'\n", resolved);
        return -1;
    }

    // Step 3: Look up the inode
    cinux::fs::Inode* inode = fs->lookup(rel_path);

    if (inode == nullptr) {
        kprintf("[SYS_STAT] File not found: '%s'\n", resolved);
        return -1;
    }

    // Step 4: Call stat()
    if (inode->ops == nullptr) {
        return -1;
    }

    cinux::fs::stat kst;
    int64_t         ret = inode->ops->stat(inode, &kst);
    if (ret < 0) {
        return -1;
    }

    // Step 5: Copy to user buffer
    auto* user_st = reinterpret_cast<cinux::fs::stat*>(st_virt);
    memcpy(user_st, &kst, sizeof(cinux::fs::stat));

    return 0;
}

int64_t sys_fstat(uint64_t fd, uint64_t st_virt, uint64_t, uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(st_virt)) {
        return -1;
    }

    // Step 1: Look up the FD
    cinux::fs::File* file = cinux::fs::current_fd_table().get(static_cast<int>(fd));

    if (file == nullptr || file->inode == nullptr) {
        return -1;
    }

    cinux::fs::Inode* inode = file->inode;

    // Step 2: Call stat()
    if (inode->ops == nullptr) {
        return -1;
    }

    cinux::fs::stat kst;
    int64_t         ret = inode->ops->stat(inode, &kst);
    if (ret < 0) {
        return -1;
    }

    // Step 3: Copy to user buffer
    auto* user_st = reinterpret_cast<cinux::fs::stat*>(st_virt);
    memcpy(user_st, &kst, sizeof(cinux::fs::stat));

    return 0;
}

}  // namespace cinux::syscall
