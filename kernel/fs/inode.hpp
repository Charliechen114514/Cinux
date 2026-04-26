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

#include <stddef.h>
#include <stdint.h>

#include "fs/stat.hpp"

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
 * @brief Abstract base class for inode-level operations
 *
 * Each concrete filesystem provides InodeOps subclasses that implement
 * read, write, readdir, create, mkdir, and unlink.  Unsupported
 * operations fall back to the default implementations (returning -1
 * or nullptr).
 */
class InodeOps {
public:
    virtual ~InodeOps() = default;

    virtual int64_t read(const Inode* inode, uint64_t offset, void* buf, uint64_t count);
    virtual int64_t write(Inode* inode, uint64_t offset, const void* buf, uint64_t count);
    virtual int64_t readdir(const Inode* inode, uint64_t index, char* name, uint64_t name_max);
    virtual Inode*  create(Inode* dir, const char* name, uint32_t namelen);
    virtual Inode*  mkdir(Inode* dir, const char* name, uint32_t namelen);
    virtual int64_t unlink(Inode* dir, const char* name, uint32_t namelen);
    virtual int64_t stat(const Inode* inode, struct stat* st);
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
    uint64_t  ino{0};                    ///< Inode number (filesystem-specific)
    uint64_t  size{0};                   ///< File size in bytes
    InodeType type{InodeType::Regular};  ///< Type of this inode
    InodeOps* ops{nullptr};              ///< Operation function table (may be nullptr)
    void*     fs_private{nullptr};       ///< Opaque pointer for filesystem-specific data

    uint32_t mode{0};    ///< File mode (type + permissions)
    uint32_t uid{0};     ///< Owner user ID
    uint32_t gid{0};     ///< Owner group ID
    uint32_t nlink{1};   ///< Hard link count
    uint64_t atime{0};   ///< Time of last access
    uint64_t ctime{0};   ///< Time of last status change
    uint64_t mtime{0};   ///< Time of last modification
    uint64_t blocks{0};  ///< Number of 512-byte blocks allocated
};

}  // namespace cinux::fs
