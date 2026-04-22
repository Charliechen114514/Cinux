/**
 * @file kernel/fs/vfs_filesystem.hpp
 * @brief Abstract FileSystem base class for the VFS layer
 *
 * Every concrete filesystem backend (ramdisk, ext2, ...) must inherit
 * from FileSystem and implement the pure-virtual mount() and lookup()
 * methods.  The VFS mount table holds FileSystem pointers and dispatches
 * path-based lookups to the appropriate backend.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include "kernel/fs/inode.hpp"

namespace cinux::fs {

/**
 * @brief Abstract base class for filesystem backends
 *
 * Provides the interface that the VFS mount layer uses to interact
 * with concrete filesystem implementations.  Each backend owns the
 * Inode objects it produces and is responsible for their lifetime.
 */
class FileSystem {
public:
    virtual ~FileSystem() = default;

    /**
     * @brief Mount (initialise) the filesystem backend
     *
     * Called once when the filesystem is added to the mount table.
     * The backend should locate its on-disk / in-memory data structures
     * and prepare for subsequent lookup() calls.
     *
     * @return true on success, false on failure
     */
    virtual bool mount() = 0;

    /**
     * @brief Look up a file by its path within this filesystem
     *
     * The path is relative to the mount point (the mount-layer strips
     * the mount prefix before calling this).
     *
     * @param path  Null-terminated path relative to the filesystem root
     * @return Pointer to the found Inode, or nullptr if not found
     */
    virtual Inode* lookup(const char* path) = 0;
};

}  // namespace cinux::fs
