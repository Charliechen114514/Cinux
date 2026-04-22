/**
 * @file kernel/fs/inode.hpp
 * @brief VFS inode definitions -- the core abstraction for filesystem objects
 *
 * Defines InodeType (regular file, directory, etc.), the InodeOps virtual
 * table for per-inode operations (read, write, readdir), and the Inode
 * struct itself which ties everything together.
 *
 * Each concrete filesystem (ramdisk, ext2, ...) produces Inode instances
 * whose ops pointers point at filesystem-specific implementations.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace cinux::fs {

// ============================================================
// Inode Type Enumeration
// ============================================================

/// Type of filesystem object represented by an inode
enum class InodeType : uint8_t {
    Unknown   = 0,
    Regular   = 1,
    Directory = 2,
};

// ============================================================
// Inode Operations (virtual function table)
// ============================================================

struct Inode;

/**
 * @brief Function-pointer table for inode-level operations
 *
 * Each concrete filesystem fills an InodeOps with pointers to its own
 * read / write / readdir implementations.  This avoids per-inode
 * virtual dispatch overhead (no vtable pointer) while remaining
 * extensible.
 *
 * Every function pointer may be nullptr if the operation is unsupported
 * for a given inode type (e.g. readdir on a regular file).
 */
struct InodeOps {
    /**
     * @brief Read bytes from an inode at a given offset
     *
     * @param inode   The inode to read from
     * @param offset  Byte offset within the file
     * @param buf     Destination buffer (kernel address)
     * @param count   Number of bytes to read
     * @return Number of bytes actually read, or -1 on error
     */
    int64_t (*read)(const Inode* inode, uint64_t offset,
                    void* buf, uint64_t count);

    /**
     * @brief Write bytes to an inode at a given offset
     *
     * @param inode   The inode to write to
     * @param offset  Byte offset within the file
     * @param buf     Source buffer (kernel address)
     * @param count   Number of bytes to write
     * @return Number of bytes actually written, or -1 on error
     */
    int64_t (*write)(Inode* inode, uint64_t offset,
                     const void* buf, uint64_t count);

    /**
     * @brief Read the next directory entry name
     *
     * @param inode  Directory inode
     * @param index  Entry index (0-based, incremented by caller)
     * @param name   Output buffer for the entry name
     * @param name_max  Size of the output buffer
     * @return 1 if an entry was read, 0 if no more entries, -1 on error
     */
    int64_t (*readdir)(const Inode* inode, uint64_t index,
                       char* name, uint64_t name_max);
};

// ============================================================
// Inode Structure
// ============================================================

/**
 * @brief Represents a single filesystem object (file, directory, etc.)
 *
 * Inodes are produced by concrete FileSystem backends during lookup().
 * They are owned by the producing filesystem and must not be freed by
 * the caller -- the filesystem manages their lifetime.
 */
struct Inode {
    uint64_t    ino;          ///< Inode number (filesystem-specific)
    uint64_t    size;         ///< File size in bytes
    InodeType   type;         ///< Type of this inode
    InodeOps*   ops;          ///< Operation function table (may be nullptr)
    void*       fs_private;   ///< Opaque pointer for filesystem-specific data
};

}  // namespace cinux::fs
