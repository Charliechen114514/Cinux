/**
 * @file kernel/fs/vfs_mount.hpp
 * @brief VFS mount point table and global accessor
 *
 * Maintains a fixed-size table of MountPoint entries that map path
 * prefixes to concrete FileSystem backends.  The resolve() function
 * walks the table to find the longest-prefix match and returns the
 * FileSystem and the remaining relative path.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include "kernel/fs/vfs_filesystem.hpp"

namespace cinux::fs {

// ============================================================
// Mount Point Limits
// ============================================================

/// Maximum number of simultaneous mount points
static constexpr uint32_t MOUNT_TABLE_SIZE = 8;

/// Maximum length of a mount path string (including null terminator)
static constexpr uint32_t MOUNT_PATH_MAX = 256;

// ============================================================
// Mount Point Entry
// ============================================================

/**
 * @brief A single mount point: maps a path prefix to a FileSystem
 */
struct MountPoint {
    char        path[MOUNT_PATH_MAX];  ///< Absolute path prefix (e.g. "/")
    FileSystem* fs;                     ///< Concrete filesystem backend
    bool        in_use;                 ///< Whether this slot is occupied
};

// ============================================================
// Global Mount Table
// ============================================================

/**
 * @brief Global mount point table and operations
 *
 * Provides functions to add/remove mount points and to resolve a path
 * to the appropriate FileSystem backend.
 *
 * Usage:
 *   1. Call vfs_mount_init() once during boot
 *   2. Call vfs_mount_add("/path", &fs) for each filesystem
 *   3. Call vfs_resolve(path) to find the backend for a given path
 */

/// Initialise the mount table (all slots marked unused)
void vfs_mount_init();

/**
 * @brief Add a mount point
 *
 * @param path  Absolute path prefix for the mount point
 * @param fs    Pointer to an initialised FileSystem backend
 * @return true on success, false if the table is full or args are invalid
 */
bool vfs_mount_add(const char* path, FileSystem* fs);

/**
 * @brief Remove a mount point by path
 *
 * @param path  The mount path to remove
 * @return true on success, false if not found
 */
bool vfs_mount_remove(const char* path);

/**
 * @brief Resolve a path to its FileSystem backend
 *
 * Finds the longest-prefix mount point matching the given path.
 * On success, *rel_path is set to point into `path` past the
 * mount prefix (i.e. the relative path within the filesystem).
 *
 * @param path      Absolute path to resolve
 * @param rel_path  [out] Pointer to the relative portion of `path`
 * @return Pointer to the FileSystem, or nullptr if no mount matches
 */
FileSystem* vfs_resolve(const char* path, const char** rel_path);

/**
 * @brief Get the global FDTable for the current task
 *
 * For milestone 027 we use a single global FDTable.  In a later
 * milestone this will become per-process.
 *
 * @return Reference to the global FDTable
 */
class FDTable;
FDTable& g_global_fd_table();

}  // namespace cinux::fs
