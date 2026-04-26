/**
 * @file kernel/fs/ext2_types.hpp
 * @brief ext2 on-disk data structures and constants
 *
 * Defines packed structures that match the ext2 filesystem layout on disk:
 * Ext2Superblock, Ext2BlockGroupDescriptor, Ext2Inode, and Ext2DirEntry.
 * Also provides related constants (magic numbers, mask values, limits)
 * used by the ext2 driver implementation.
 *
 * Reference: The Second Extended Filesystem (ext2) documentation
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fs/inode.hpp"

namespace cinux::fs {

// ============================================================
// ext2 Constants
// ============================================================

/// Expected magic number in the ext2 superblock
static constexpr uint16_t EXT2_SUPER_MAGIC = 0xEF53;

/// Default inode size (revision 0 uses 128; good default for mkfs.ext2)
static constexpr uint16_t EXT2_INODE_SIZE_DEFAULT = 128;

/// Maximum number of direct block pointers in an inode
static constexpr uint32_t EXT2_DIRECT_BLOCKS = 12;

/// Index of the singly-indirect block pointer
static constexpr uint32_t EXT2_INDIRECT_BLOCK = 12;

/// Index of the doubly-indirect block pointer
static constexpr uint32_t EXT2_DOUBLE_INDIRECT_BLOCK = 13;

/// Maximum number of block pointers in an inode (12 direct + 1 indirect + 1 double + 1 triple)
static constexpr uint32_t EXT2_TOTAL_BLOCK_PTRS = 15;

/// Maximum directory entry name length we support
static constexpr uint32_t EXT2_NAME_MAX = 255;

/// Maximum cached inodes in the ext2 driver
static constexpr uint32_t EXT2_INODE_CACHE_SIZE = 64;

/// Disk sector size in bytes
static constexpr uint32_t EXT2_SECTOR_SIZE = 512;

/// Superblock is located at byte offset 1024 (sector 2)
static constexpr uint64_t EXT2_SUPERBLOCK_OFFSET = 1024;

/// Superblock size in bytes
static constexpr uint32_t EXT2_SUPERBLOCK_SIZE = 1024;

// ============================================================
// ext2 Inode Mode Masks
// ============================================================

/// Bitmask for the file-type field in i_mode
static constexpr uint16_t EXT2_S_IFMT = 0xF000;

/// Regular file
static constexpr uint16_t EXT2_S_IFREG = 0x8000;

/// Directory
static constexpr uint16_t EXT2_S_IFDIR = 0x4000;

// ============================================================
// ext2 Directory Entry File Types
// ============================================================

/// Type values stored in Ext2DirEntry::file_type
enum class Ext2FileType : uint8_t {
    Unknown   = 0,
    Regular   = 1,
    Directory = 2,
    Chardev   = 3,
    Blockdev  = 4,
    Fifo      = 5,
    Symlink   = 6,
    Socket    = 7,
};

// ============================================================
// ext2 Superblock Structure
// ============================================================

/**
 * @brief ext2 superblock (1024 bytes, packed)
 *
 * Located at byte offset 1024 from the start of the partition.
 * Contains global filesystem metadata.
 */
struct [[gnu::packed]] Ext2Superblock {
    uint32_t s_inodes_count;       ///< Total inode count
    uint32_t s_blocks_count;       ///< Total block count
    uint32_t s_r_blocks_count;     ///< Reserved blocks (for superuser)
    uint32_t s_free_blocks_count;  ///< Free block count
    uint32_t s_free_inodes_count;  ///< Free inode count
    uint32_t s_first_data_block;   ///< First data block (1 for 1K block size)
    uint32_t s_log_block_size;     ///< Block size = 1024 << s_log_block_size
    uint32_t s_log_frag_size;      ///< Fragment size (deprecated, same as block)
    uint32_t s_blocks_per_group;   ///< Blocks per group
    uint32_t s_frags_per_group;    ///< Fragments per group
    uint32_t s_inodes_per_group;   ///< Inodes per group
    uint32_t s_mtime;              ///< Mount time
    uint32_t s_wtime;              ///< Write time
    uint16_t s_mnt_count;          ///< Mount count since last fsck
    uint16_t s_max_mnt_count;      ///< Max mounts before fsck
    uint16_t s_magic;              ///< Magic signature (0xEF53)
    uint16_t s_state;              ///< Filesystem state
    uint16_t s_errors;             ///< Behaviour on error
    uint16_t s_minor_rev_level;    ///< Minor revision level
    uint32_t s_lastcheck;          ///< Time of last check
    uint32_t s_checkinterval;      ///< Max time between checks
    uint32_t s_creator_os;         ///< Creator OS
    uint32_t s_rev_level;          ///< Revision level
    uint16_t s_def_resuid;         ///< Default uid for reserved blocks
    uint16_t s_def_resgid;         ///< Default gid for reserved blocks
    // Revision 1 fields
    uint32_t s_first_ino;          ///< First non-reserved inode
    uint16_t s_inode_size;         ///< Size of inode structure (bytes)
    uint16_t s_block_group_nr;     ///< Block group number of this superblock
    uint32_t s_feature_compat;     ///< Compatible feature set
    uint32_t s_feature_incompat;   ///< Incompatible feature set
    uint32_t s_feature_ro_compat;  ///< Read-only compatible feature set
    uint8_t  s_uuid[16];           ///< 128-bit filesystem UUID
    char     s_volume_name[16];    ///< Volume name
    char     s_last_mounted[64];   ///< Directory where last mounted
    uint32_t s_algo_bitmap;        ///< Compression algorithm
    // Performance hints (revision 1)
    uint8_t  s_prealloc_blocks;      ///< Blocks to preallocate
    uint8_t  s_prealloc_dir_blocks;  ///< Blocks to preallocate for dirs
    uint16_t s_reserved_gdt_blocks;  ///< Reserved GDT blocks
    // Journaling (revision 1)
    uint8_t  s_journal_uuid[16];  ///< Journal inode UUID
    uint32_t s_journal_inum;      ///< Journal inode number
    uint32_t s_journal_dev;       ///< Journal device number
    uint32_t s_last_orphan;       ///< Head of orphan inode list
    uint32_t s_hash_seed[4];      ///< HTree hash seed
    uint8_t  s_def_hash_version;  ///< Default hash version
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;  ///< Size of group descriptor (rev 1)
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;   ///< First metablock block group
    uint32_t s_mkfs_time;       ///< Filesystem creation time
    uint32_t s_jnl_blocks[17];  ///< Journal inode backup
    // Remaining bytes to reach 1024 total
    uint32_t s_blocks_count_hi;  ///< Upper 32 bits of blocks count
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint8_t  s_encryption_level;
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;
    uint32_t s_snapshot_inum;
    uint32_t s_snapshot_id;
    uint64_t s_snapshot_r_blocks_count;
    uint32_t s_snapshot_list;
    uint32_t s_error_count;
    uint32_t s_first_error_time;
    uint32_t s_first_error_ino;
    uint64_t s_first_error_block;
    uint8_t  s_first_error_func[32];
    uint32_t s_first_error_line;
    uint32_t s_last_error_time;
    uint32_t s_last_error_ino;
    uint64_t s_last_error_block;
    uint8_t  s_last_error_func[32];
    uint32_t s_last_error_line;
    uint8_t  s_mount_opts[64];
    uint32_t s_usr_quota_inum;
    uint32_t s_grp_quota_inum;
    uint32_t s_overhead_blocks;
    uint32_t s_backup_bgs[2];
    uint8_t  s_encrypt_algos[4];
    uint8_t  s_encrypt_pw_salt[16];
    uint32_t s_lpf_ino;
    uint32_t s_prj_quota_inum;
    uint32_t s_checksum_seed;
    uint8_t  s_wtime_hi;
    uint8_t  s_mtime_hi;
    uint8_t  s_mkfs_time_hi;
    uint8_t  s_lastcheck_hi;
    uint8_t  s_first_error_time_hi;
    uint8_t  s_last_error_time_hi;
    uint8_t  s_first_error_errcode;
    uint8_t  s_last_error_errcode;
    uint8_t  s_pad[387];
};

static_assert(sizeof(Ext2Superblock) == 1024, "Ext2Superblock must be exactly 1024 bytes");

// ============================================================
// ext2 Block Group Descriptor
// ============================================================

/**
 * @brief ext2 block group descriptor (32 bytes, packed)
 *
 * The block group descriptor table starts at the block immediately
 * following the superblock block.  Each entry describes one block group.
 */
struct [[gnu::packed]] Ext2BlockGroupDescriptor {
    uint32_t bg_block_bitmap;       ///< Block number of block bitmap
    uint32_t bg_inode_bitmap;       ///< Block number of inode bitmap
    uint32_t bg_inode_table;        ///< Block number of inode table start
    uint16_t bg_free_blocks_count;  ///< Free blocks in this group
    uint16_t bg_free_inodes_count;  ///< Free inodes in this group
    uint16_t bg_used_dirs_count;    ///< Number of directories in this group
    uint16_t bg_pad;                ///< Padding
    uint32_t bg_reserved[3];        ///< Reserved
};

static_assert(sizeof(Ext2BlockGroupDescriptor) == 32,
              "Ext2BlockGroupDescriptor must be exactly 32 bytes");

// ============================================================
// ext2 Inode Structure
// ============================================================

/**
 * @brief ext2 on-disk inode (128 bytes, revision 0)
 *
 * Represents a single file, directory, or other filesystem object.
 * The i_block array holds 12 direct block pointers, plus one
 * singly-indirect, one doubly-indirect, and one triply-indirect.
 */
struct [[gnu::packed]] Ext2Inode {
    uint16_t i_mode;                          ///< File mode (type + permissions)
    uint16_t i_uid;                           ///< Owner UID (low 16 bits)
    uint32_t i_size;                          ///< File size in bytes (low 32 bits)
    uint32_t i_atime;                         ///< Access time
    uint32_t i_ctime;                         ///< Creation time
    uint32_t i_mtime;                         ///< Modification time
    uint32_t i_dtime;                         ///< Deletion time
    uint16_t i_gid;                           ///< Group GID (low 16 bits)
    uint16_t i_links_count;                   ///< Hard link count
    uint32_t i_blocks;                        ///< Block count (in 512-byte sectors)
    uint32_t i_flags;                         ///< Inode flags
    uint32_t i_osd1;                          ///< OS-specific field 1
    uint32_t i_block[EXT2_TOTAL_BLOCK_PTRS];  ///< Block pointers
    uint32_t i_generation;                    ///< File version (for NFS)
    uint32_t i_file_acl;                      ///< File ACL block
    uint32_t i_dir_acl;                       ///< Directory ACL / i_size_high
    uint32_t i_faddr;                         ///< Fragment address
    uint8_t  i_osd2[12];                      ///< OS-specific field 2
};

static_assert(sizeof(Ext2Inode) == 128, "Ext2Inode must be exactly 128 bytes");

// ============================================================
// ext2 Directory Entry
// ============================================================

/**
 * @brief ext2 directory entry (variable length, packed)
 *
 * Directory data blocks contain a linked list of these entries.
 * The rec_len field is used to step to the next entry.  The actual
 * name is name_len bytes long (NOT null-terminated on disk).
 *
 * Minimum entry size is 8 bytes (header) + name must be 4-byte aligned.
 */
struct [[gnu::packed]] Ext2DirEntry {
    uint32_t inode;      ///< Inode number (0 = unused entry)
    uint16_t rec_len;    ///< Total entry size (including name)
    uint8_t  name_len;   ///< Length of the entry name
    uint8_t  file_type;  ///< File type (Ext2FileType value)
    char     name[];     ///< Entry name (variable length, NOT null-terminated)
};

/// Minimum size of an ext2 directory entry (header only, no name)
static constexpr uint32_t EXT2_DIR_ENTRY_HDR_SIZE = 8;

static_assert(offsetof(Ext2DirEntry, name) == EXT2_DIR_ENTRY_HDR_SIZE,
              "Ext2DirEntry name must start at offset 8");

// ============================================================
// Inode cache entry for the ext2 driver
// ============================================================

/**
 * @brief Cached inode with VFS integration
 *
 * Combines the on-disk Ext2Inode with the VFS Inode and marks
 * whether the cache slot is occupied.
 */
struct Ext2CachedInode {
    Ext2Inode disk_inode;  ///< Copy of the on-disk inode
    Inode     vfs_inode;   ///< VFS-facing inode
    uint32_t  ino;         ///< Inode number
    bool      in_use;      ///< Whether this cache slot is occupied
};

}  // namespace cinux::fs
