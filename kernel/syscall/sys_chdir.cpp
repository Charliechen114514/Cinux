/**
 * @file kernel/syscall/sys_chdir.cpp
 * @brief sys_chdir handler implementation
 *
 * Resolves the given path (relative or absolute), verifies it is a
 * directory, and updates the current task's cwd field.
 */

#include "kernel/syscall/sys_chdir.hpp"

#include <stdint.h>

#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

}  // anonymous namespace

int64_t sys_chdir(uint64_t path_virt, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }

    // Step 2: Resolve through the VFS mount table
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(resolved, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_CHDIR] No filesystem mounted for '%s'\n", resolved);
        return -1;
    }

    // Step 3: Look up the inode
    cinux::fs::Inode* inode = fs->lookup(rel_path);

    if (inode == nullptr) {
        kprintf("[SYS_CHDIR] Path not found: '%s'\n", resolved);
        return -1;
    }

    // Step 4: Verify it is a directory
    if (inode->type != cinux::fs::InodeType::Directory) {
        kprintf("[SYS_CHDIR] Not a directory: '%s'\n", resolved);
        return -1;
    }

    // Step 5: Update cwd
    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) {
        return -1;
    }

    uint32_t i = 0;
    while (resolved[i] != '\0' && i < sizeof(current->cwd) - 1) {
        current->cwd[i] = resolved[i];
        ++i;
    }
    current->cwd[i] = '\0';

    return 0;
}

}  // namespace cinux::syscall
