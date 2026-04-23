/**
 * @file kernel/syscall/sys_creat.cpp
 * @brief sys_creat handler implementation
 *
 * Creates a new regular file by resolving the parent directory through
 * the VFS and calling InodeOps::create() on it.
 */

#include "kernel/syscall/sys_creat.hpp"

#include <stdint.h>

#include "kernel/fs/path.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/syscall/path_util.hpp"

namespace cinux::syscall {

namespace {

using cinux::lib::kprintf;

/**
 * @brief Split a path into parent directory path and leaf name
 *
 * For example, "foo/bar/baz" -> parent="foo/bar", name="baz".
 * Edge case: "baz" -> parent="", name="baz" (create in root).
 *
 * @param path       Full path (null-terminated, relative to FS root)
 * @param parent_out Buffer to receive the parent path (at least PATH_MAX bytes)
 * @param name_out   Pointer set to the start of the leaf name within `path`
 * @param namelen_out Pointer to receive the length of the leaf name
 * @return true on success, false if path is empty or ends with '/'
 */
bool split_pathname(const char* path, char* parent_out,
                    const char** name_out, uint32_t* namelen_out) {
    uint32_t len = static_cast<uint32_t>(strlen(path));

    if (len == 0) {
        return false;
    }

    // Trailing slash is ambiguous for create/mkdir/unlink
    if (path[len - 1] == '/') {
        return false;
    }

    // Find the last '/' separator
    int32_t last_sep = -1;
    for (uint32_t i = 0; i < len; ++i) {
        if (path[i] == '/') {
            last_sep = static_cast<int32_t>(i);
        }
    }

    if (last_sep < 0) {
        // No separator: parent is root (empty string)
        parent_out[0] = '\0';
        *name_out = path;
        *namelen_out = len;
    } else {
        // Copy parent portion
        uint32_t parent_len = static_cast<uint32_t>(last_sep);
        memcpy(parent_out, path, parent_len);
        parent_out[parent_len] = '\0';

        *name_out = path + last_sep + 1;
        *namelen_out = len - parent_len - 1;
    }

    if (*namelen_out == 0) {
        return false;
    }

    return true;
}

}  // anonymous namespace

int64_t sys_creat(uint64_t path_virt, uint64_t, uint64_t,
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
        kprintf("[SYS_CREAT] No filesystem mounted for '%s'\n", resolved);
        return -1;
    }

    // Step 3: Split relative path into parent dir and leaf name
    char parent_buf[cinux::fs::PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t name_len = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        kprintf("[SYS_CREAT] Invalid path: '%s'\n", resolved);
        return -1;
    }

    // Step 4: Look up the parent directory inode
    cinux::fs::Inode* parent = fs->lookup(parent_buf);

    if (parent == nullptr) {
        kprintf("[SYS_CREAT] Parent directory not found for '%s'\n", resolved);
        return -1;
    }

    if (parent->ops == nullptr) {
        kprintf("[SYS_CREAT] Parent inode has no ops\n");
        return -1;
    }

    // Step 5: Try to create the file
    cinux::fs::Inode* new_inode = parent->ops->create(parent, leaf_name, name_len);

    if (new_inode != nullptr) {
        return 0;  // new file created successfully
    }

    // Step 6: create() returned nullptr -- file may already exist.
    // Truncate it to 0 bytes (POSIX creat semantics).
    cinux::fs::Inode* existing = fs->lookup(rel_path);
    if (existing == nullptr || existing->ops == nullptr) {
        kprintf("[SYS_CREAT] Failed to create or truncate '%s'\n", resolved);
        return -1;
    }

    if (existing->size > 0) {
        existing->size = 0;
    }

    return 0;
}

}  // namespace cinux::syscall
