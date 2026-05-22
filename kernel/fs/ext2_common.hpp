/**
 * @file kernel/fs/ext2_common.hpp
 * @brief Shared ext2 constants and VFS InodeOps wrappers
 *
 * Houses the Ext2FileOps and Ext2DirOps classes that bridge the VFS
 * InodeOps interface to the concrete Ext2 driver.  Also defines the
 * EXT2_MAX_GROUPS constant shared across all ext2 sub-modules.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

#include "fs/inode.hpp"

namespace cinux::fs {

class Ext2;

/// Maximum block groups supported (covers up to ~8 GB with 4K blocks)
static constexpr uint32_t EXT2_MAX_GROUPS = 128;

/**
 * @brief InodeOps for ext2 regular files
 *
 * Overrides read() and write(); all other operations use the
 * InodeOps defaults (return -1 / nullptr).
 */
class Ext2FileOps : public InodeOps {
public:
    explicit Ext2FileOps(Ext2& ext2);

    int64_t read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) override;
    int64_t write(Inode* inode, uint64_t offset, const void* buf, uint64_t count) override;
    int64_t stat(const Inode* inode, struct stat* st) override;

private:
    Ext2& ext2_;
};

/**
 * @brief InodeOps for ext2 directories
 *
 * Overrides readdir(), create(), mkdir(), and unlink();
 * read()/write() use the InodeOps defaults.
 */
class Ext2DirOps : public InodeOps {
public:
    explicit Ext2DirOps(Ext2& ext2);

    int64_t readdir(const Inode* inode, uint64_t index, char* name, uint64_t name_max) override;
    Inode*  create(Inode* dir, const char* name, uint32_t namelen) override;
    Inode*  mkdir(Inode* dir, const char* name, uint32_t namelen) override;
    int64_t unlink(Inode* dir, const char* name, uint32_t namelen) override;
    int64_t stat(const Inode* inode, struct stat* st) override;

private:
    Ext2& ext2_;
};

}  // namespace cinux::fs
