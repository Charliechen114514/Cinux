/**
 * @file kernel/fs/ext2.hpp
 * @brief ext2 filesystem driver (inherits from FileSystem)
 *
 * Implements the VFS FileSystem interface for the ext2 filesystem.
 * Reads blocks from disk via the AHCI driver and DMA buffers
 * allocated through PMM/VMM.  Supports mount(), lookup(), and
 * InodeOps (read, readdir) for files and directories.
 *
 * Usage:
 *   cinux::fs::Ext2 ext2(ahci, port_index);
 *   ext2.mount();
 *   vfs_mount_add("/", &ext2);
 *   Inode* ino = ext2.lookup("etc/motd");
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "fs/ext2_types.hpp"
#include "fs/inode.hpp"
#include "fs/vfs_filesystem.hpp"

namespace cinux::drivers::ahci {
class AHCI;
}

namespace cinux::fs {

// ============================================================
// Maximum number of block group descriptors
// ============================================================

/// Maximum block groups supported (covers up to ~8 GB with 4K blocks)
static constexpr uint32_t EXT2_MAX_GROUPS = 128;

// ============================================================
// Ext2 InodeOps subclasses (forward-declared, defined in ext2.cpp)
// ============================================================

class Ext2;

/**
 * @brief InodeOps for ext2 regular files
 *
 * Overrides read() and write(); all other operations use the
 * InodeOps defaults (return -1 / nullptr).
 */
class Ext2FileOps : public InodeOps {
public:
    explicit Ext2FileOps(Ext2& ext2);

    int64_t read(const Inode* inode, uint64_t offset,
                 void* buf, uint64_t count) override;
    int64_t write(Inode* inode, uint64_t offset,
                  const void* buf, uint64_t count) override;
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

    int64_t readdir(const Inode* inode, uint64_t index,
                    char* name, uint64_t name_max) override;
    Inode* create(Inode* dir, const char* name, uint32_t namelen) override;
    Inode* mkdir(Inode* dir, const char* name, uint32_t namelen) override;
    int64_t unlink(Inode* dir, const char* name, uint32_t namelen) override;
    int64_t stat(const Inode* inode, struct stat* st) override;

private:
    Ext2& ext2_;
};

// ============================================================
// Ext2 Filesystem Driver Class
// ============================================================

/**
 * @brief ext2 filesystem driver (read-only)
 *
 * Reads the ext2 superblock and block group descriptor table from
 * disk during mount(), then provides path-based lookup() and
 * InodeOps for reading files and listing directories.
 *
 * Block I/O is performed through the AHCI driver using DMA buffers
 * allocated from the PMM and mapped into kernel virtual address space.
 */
class Ext2 : public FileSystem {
public:
    /**
     * @brief Construct an ext2 driver bound to an AHCI port
     *
     * @param ahci         Reference to the initialised AHCI controller
     * @param port_index   AHCI port number where the ext2 disk resides
     */
    Ext2(cinux::drivers::ahci::AHCI& ahci, uint8_t port_index);

    /**
     * @brief Mount the ext2 filesystem
     *
     * Reads and validates the superblock, computes block_size,
     * reads the block group descriptor table, and prepares the
     * root directory inode for VFS lookup.
     *
     * @return true on success, false if superblock is invalid or I/O fails
     */
    bool mount() override;

    /**
     * @brief Look up a file or directory by path
     *
     * The path is relative to the filesystem root (the mount layer
     * strips the mount prefix).  Performs component-by-component
     * traversal through directory entries.
     *
     * @param path  Null-terminated path relative to filesystem root
     * @return Pointer to a cached Inode, or nullptr if not found
     */
    Inode* lookup(const char* path) override;

    /**
     * @brief Get the resolved block size in bytes
     * @return Block size (1024, 2048, or 4096)
     */
    uint32_t block_size() const;

    /**
     * @brief Check whether the filesystem has been mounted
     * @return true if mount() succeeded
     */
    bool is_mounted() const;

    /**
     * @brief Get the virtual address of the DMA buffer (read-only access)
     *
     * Used by InodeOps callbacks to access block data after read_block().
     * @return Virtual address of the DMA buffer
     */
    uint64_t dma_buf_virt() const;

    /**
     * @brief Read an ext2 block from disk into the DMA buffer
     *
     * Public wrapper used by InodeOps callbacks.
     *
     * @param block_num  ext2 block number (0-based)
     * @return true on success, false on I/O error
     */
    bool read_block(uint32_t block_num);

    /**
     * @brief Write the DMA buffer contents back to an ext2 block on disk
     *
     * The caller should first populate the DMA buffer (via dma_buf_virt())
     * with the modified block data, then call write_block() to flush it
     * to disk.  This is the counterpart to read_block().
     *
     * @param block_num  ext2 block number (0-based)
     * @return true on success, false on I/O error
     */
    bool write_block(uint32_t block_num);

    // ============================================================
    // File / directory mutation
    // ============================================================

    /**
     * @brief Create a new regular file inside a directory
     *
     * Allocates a new inode, initialises it as a regular file (mode REG,
     * links_count = 1), adds a directory entry in the parent, and writes
     * all modified metadata back to disk.
     *
     * @param parent_ino  Inode number of the parent directory
     * @param name        Name of the new file (NOT null-terminated)
     * @param name_len    Length of the name
     * @return Pointer to the new VFS Inode on success, nullptr on failure
     */
    Inode* create(uint32_t parent_ino, const char* name, uint32_t name_len);

    /**
     * @brief Create a new subdirectory inside a directory
     *
     * Allocates a new inode, initialises it as a directory (mode DIR,
     * links_count = 2), allocates one data block, writes "." and ".."
     * entries, adds a directory entry in the parent, and writes all
     * modified metadata back to disk.
     *
     * @param parent_ino  Inode number of the parent directory
     * @param name        Name of the new directory (NOT null-terminated)
     * @param name_len    Length of the name
     * @return Pointer to the new VFS Inode on success, nullptr on failure
     */
    Inode* mkdir(uint32_t parent_ino, const char* name, uint32_t name_len);

    /**
     * @brief Remove a directory entry and, if link count reaches zero,
     *        free the associated inode and data blocks
     *
     * Scans the parent directory for the named entry, removes it, and
     * decrements the target inode's link count.  If the link count
     * becomes zero, all data blocks are freed and the inode is released.
     *
     * @param parent_ino  Inode number of the parent directory
     * @param name        Name of the entry to remove (NOT null-terminated)
     * @param name_len    Length of the name
     * @return 0 on success, -1 on failure
     */
    int unlink(uint32_t parent_ino, const char* name, uint32_t name_len);

    // ============================================================
    // Block allocator
    // ============================================================

    /**
     * @brief Allocate a free data block from the filesystem
     *
     * Scans block bitmaps across all block groups to find a free block.
     * Marks the block as used in the bitmap, updates the superblock and
     * block group descriptor free-block counts, and writes all modified
     * metadata back to disk.
     *
     * @return Allocated block number (0-based), or 0 if the filesystem is full
     */
    uint32_t alloc_block();

    /**
     * @brief Release a previously allocated data block
     *
     * Clears the block's bit in the appropriate block bitmap, updates
     * the superblock and block group descriptor free-block counts, and
     * writes all modified metadata back to disk.
     *
     * @param block_num  ext2 block number to free (must be > 0)
     * @return true on success, false on error
     */
    bool free_block(uint32_t block_num);

    // ============================================================
    // Inode allocator
    // ============================================================

    /**
     * @brief Allocate a free inode from the filesystem
     *
     * Scans inode bitmaps across all block groups to find a free inode.
     * Marks the inode as used in the bitmap, updates the superblock and
     * block group descriptor free-inode counts, and writes all modified
     * metadata back to disk.
     *
     * @return Allocated inode number (1-based), or 0 if no free inodes
     */
    uint32_t alloc_inode();

    /**
     * @brief Release a previously allocated inode
     *
     * Clears the inode's bit in the appropriate inode bitmap, updates
     * the superblock and block group descriptor free-inode counts, and
     * writes all modified metadata back to disk.
     *
     * @param ino  Inode number to free (1-based, must be > 0)
     * @return true on success, false on error
     */
    bool free_inode(uint32_t ino);

    // ============================================================
    // Disk inode read/write (public for InodeOps callback access)
    // ============================================================

    /**
     * @brief Read an on-disk inode by inode number
     *
     * @param ino          Inode number (1-based)
     * @param out_inode    Output buffer for the inode data
     * @return true on success, false on I/O error
     */
    bool read_disk_inode(uint32_t ino, Ext2Inode& out_inode);

    /**
     * @brief Write an on-disk inode back to disk
     *
     * @param ino          Inode number (1-based)
     * @param inode        The inode data to write
     * @return true on success, false on I/O error
     */
    bool write_disk_inode(uint32_t ino, const Ext2Inode& inode);

    /**
     * @brief Get or allocate a block pointer for a given file block index
     *
     * @param disk      On-disk inode (modified with new block pointers)
     * @param file_block  Logical block index within the file (0..12)
     * @return Disk block number allocated, or 0 on failure
     */
    uint32_t get_or_alloc_block(Ext2Inode& disk, uint32_t file_block);

    /**
     * @brief Add a directory entry to a parent directory
     *
     * @param dir_ino      Parent directory inode number
     * @param dir_disk     On-disk inode (modified in-memory)
     * @param entry_ino    Inode number of the new entry
     * @param name         Entry name (NOT null-terminated)
     * @param name_len     Length of the name
     * @param file_type    Ext2FileType value for the new entry
     * @return true on success, false on failure
     */
    bool add_dir_entry(uint32_t dir_ino, Ext2Inode& dir_disk,
                       uint32_t entry_ino, const char* name,
                       uint32_t name_len, Ext2FileType file_type);

    /**
     * @brief Remove a directory entry from a parent directory
     *
     * @param dir_ino      Parent directory inode number
     * @param dir_disk     On-disk inode of the parent directory
     * @param name         Entry name to remove (NOT null-terminated)
     * @param name_len     Length of the name
     * @param out_entry_ino  [out] Inode number of the removed entry
     * @return true on success, false on failure
     */
    bool remove_dir_entry(uint32_t dir_ino, const Ext2Inode& dir_disk,
                          const char* name, uint32_t name_len,
                          uint32_t& out_entry_ino);

private:
    // ============================================================
    // Internal helpers
    // ============================================================

    /**
     * @brief Ensure the DMA buffer is allocated and mapped
     *
     * @return true if the buffer is ready, false on allocation failure
     */
    bool ensure_dma_buffer();

    // ============================================================
    // Metadata write-back helpers
    // ============================================================

    /**
     * @brief Write the cached superblock back to disk
     *
     * Flushes the in-memory sb_ to the on-disk superblock location
     * (byte offset 1024).
     *
     * @return true on success, false on I/O error
     */
    bool write_superblock();

    /**
     * @brief Write a single block group descriptor back to disk
     *
     * Reads the BGDT block that contains the specified group's descriptor,
     * patches the descriptor entry, and writes the block back.
     *
     * @param group  Block group index (0-based)
     * @return true on success, false on I/O error
     */
    bool write_bgdt(uint32_t group);

    // ============================================================
    // Inode cache management
    // ============================================================

    /**
     * @brief Find or allocate a cache slot for the given inode number
     *
     * If the inode is already cached, returns its slot.  Otherwise
     * finds a free slot, reads the inode from disk, and populates
     * both the disk_inode and vfs_inode fields.
     *
     * @param ino  Inode number (1-based)
     * @return Pointer to the VFS Inode, or nullptr on failure
     */
    Inode* get_cached_inode(uint32_t ino);

    /**
     * @brief Build a VFS Inode from an on-disk ext2 inode
     *
     * Populates the vfs_inode fields (ino, size, type, ops, fs_private)
     * based on the disk inode contents.
     *
     * @param cached  Cache entry to populate
     */
    void populate_vfs_inode(Ext2CachedInode& cached);

    // ============================================================
    // Path resolution
    // ============================================================

    /**
     * @brief Look up a single component name inside a directory inode
     *
     * Scans the directory data blocks for an entry whose name matches
     * the given component.  Returns the matching entry's inode number.
     *
     * @param dir_ino      Inode number of the directory to search
     * @param name         Component name to find (null-terminated)
     * @param name_len     Length of the name
     * @return Inode number of the matching entry, or 0 if not found
     */
    uint32_t lookup_in_dir(uint32_t dir_ino, const char* name, uint32_t name_len);

    // ============================================================
    // Member data
    // ============================================================

    /// Ops instances for file and directory inodes
    Ext2FileOps file_ops_;
    Ext2DirOps  dir_ops_;

    /// Reference to the AHCI controller
    cinux::drivers::ahci::AHCI& ahci_;

    /// AHCI port index where the ext2 disk is attached
    uint8_t port_index_;

    /// Whether mount() has succeeded
    bool mounted_{};

    /// Computed block size in bytes (1024, 2048, or 4096)
    uint32_t block_size_{};

    /// Number of sectors per ext2 block
    uint32_t sectors_per_block_{};

    /// First data block number (1 for 1K blocks, 0 otherwise)
    uint32_t first_data_block_{};

    /// Inode size in bytes (from superblock)
    uint16_t inode_size_{};

    /// Number of inodes per block group
    uint32_t inodes_per_group_{};

    /// Number of blocks per block group
    uint32_t blocks_per_group_{};

    /// Total number of block groups
    uint32_t group_count_{};

    /// Superblock (cached after mount)
    Ext2Superblock sb_{};

    /// Block group descriptor table (cached after mount)
    Ext2BlockGroupDescriptor bgdt_[EXT2_MAX_GROUPS]{};

    /// Inode cache (fixed-size array)
    Ext2CachedInode inode_cache_[EXT2_INODE_CACHE_SIZE]{};

    /// Root directory VFS inode (always at cache slot 0 after mount)
    Inode root_inode_{};

    /// Physical address of the single-block DMA buffer
    uint64_t dma_buf_phys_{};

    /// Virtual address of the single-block DMA buffer
    uint64_t dma_buf_virt_{};

    /// Whether the DMA buffer has been allocated and mapped
    bool dma_ready_{};
};

}  // namespace cinux::fs
