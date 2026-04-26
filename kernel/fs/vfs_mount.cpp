/**
 * @file kernel/fs/vfs_mount.cpp
 * @brief VFS mount point table implementation
 *
 * Maintains a fixed-size array of MountPoint entries that map path
 * prefixes to concrete FileSystem backends.  Provides init / add /
 * remove / resolve operations plus the global FDTable accessor.
 *
 * Namespace: cinux::fs
 */

#include "kernel/fs/vfs_mount.hpp"

#include "kernel/fs/file.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/sync.hpp"

namespace cinux::fs {

// ============================================================
// Global Mount Table
// ============================================================

static MountPoint            g_mount_table[MOUNT_TABLE_SIZE];
static cinux::proc::Spinlock g_mount_lock;

// ============================================================
// Init
// ============================================================

void vfs_mount_init() {
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        g_mount_table[i].path[0] = '\0';
        g_mount_table[i].fs      = nullptr;
        g_mount_table[i].in_use  = false;
    }
}

// ============================================================
// Add
// ============================================================

bool vfs_mount_add(const char* path, FileSystem* fs) {
    if (path == nullptr || fs == nullptr) {
        return false;
    }

    auto g = g_mount_lock.guard();
    (void)g;

    // Find a free slot
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (!g_mount_table[i].in_use) {
            // Check path length (including null terminator)
            uint32_t len = 0;
            while (path[len] != '\0') {
                ++len;
            }
            if (len == 0 || len >= MOUNT_PATH_MAX) {
                return false;
            }

            memcpy(g_mount_table[i].path, path, len + 1);
            g_mount_table[i].fs     = fs;
            g_mount_table[i].in_use = true;
            return true;
        }
    }

    // Table is full
    return false;
}

// ============================================================
// Remove
// ============================================================

bool vfs_mount_remove(const char* path) {
    if (path == nullptr) {
        return false;
    }

    auto g = g_mount_lock.guard();
    (void)g;

    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (g_mount_table[i].in_use && strncmp(g_mount_table[i].path, path, MOUNT_PATH_MAX) == 0) {
            g_mount_table[i].in_use = false;
            return true;
        }
    }

    return false;
}

// ============================================================
// Resolve (longest-prefix match)
// ============================================================

FileSystem* vfs_resolve(const char* path, const char** rel_path) {
    if (path == nullptr || rel_path == nullptr) {
        return nullptr;
    }

    auto g = g_mount_lock.guard();
    (void)g;

    FileSystem* best_fs  = nullptr;
    uint32_t    best_len = 0;

    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (!g_mount_table[i].in_use) {
            continue;
        }

        const char* mpath = g_mount_table[i].path;
        uint32_t    mlen  = 0;
        while (mpath[mlen] != '\0') {
            ++mlen;
        }

        // Check that path starts with the mount prefix
        if (strncmp(mpath, path, mlen) != 0) {
            continue;
        }

        // Ensure the prefix match is at a path component boundary.
        // If the mount path ends with '/' (e.g. "/"), the prefix itself
        // is the boundary.  Otherwise the next char in path must be '\0'
        // or '/'.
        char last_mount_char = mpath[mlen - 1];
        if (last_mount_char != '/') {
            if (path[mlen] != '\0' && path[mlen] != '/') {
                continue;
            }
        }

        if (mlen > best_len) {
            best_len = mlen;
            best_fs  = g_mount_table[i].fs;
        }
    }

    if (best_fs != nullptr) {
        *rel_path = path + best_len;
    }

    return best_fs;
}

// ============================================================
// Global FDTable accessor
// ============================================================

FDTable& g_global_fd_table() {
    static FDTable s_global_fd_table;
    return s_global_fd_table;
}

FDTable& current_fd_table() {
#ifndef CINUX_HOST_TEST
    auto* task = cinux::proc::Scheduler::current();
    if (task != nullptr && task->fd_table != nullptr) {
        return *task->fd_table;
    }
#endif
    return g_global_fd_table();
}

}  // namespace cinux::fs
