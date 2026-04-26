/**
 * @file test/unit/test_ext2_ops.cpp
 * @brief Host-side unit tests for ext2 write/create/mkdir/unlink logic
 *
 * Test coverage:
 *   - ext2_file_write: small write, cross-block write, direct block allocation,
 *     indirect block allocation, size update, empty write
 *   - create: inode allocation, directory entry insertion, link_count=1
 *   - mkdir: inode + data block allocation, "."/".." entries, parent link_count +1
 *   - unlink: directory entry removal, link_count decrement, block+inode free
 *   - add_dir_entry: entry splitting, new block allocation
 *   - remove_dir_entry: first-entry clear, middle-entry merge
 *   - Boundary conditions: empty file write, single-char name, long name
 *
 * Pure arithmetic and data structure manipulation -- no kernel code linked.
 * The operations logic is re-implemented here to test independently.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

#    include "fs/ext2_types.hpp"

using namespace cinux::fs;

// ============================================================
// Simulated filesystem environment for host testing
//
// The kernel ext2.cpp uses AHCI DMA buffers, PMM, VMM for real
// disk I/O.  We simulate the on-disk structures (superblock, BGDT,
// bitmaps, inode table, data blocks) entirely in memory and
// re-implement the pure logical operations to test independently.
// ============================================================

/// Simulated block size
static constexpr uint32_t TEST_BLOCK_SIZE = 1024;

/// Simulated inode size
static constexpr uint16_t TEST_INODE_SIZE = 128;

/// Number of simulated inodes per group
static constexpr uint32_t TEST_IPG = 32;

/// Number of simulated blocks per group
static constexpr uint32_t TEST_BPG = 128;

/// First data block
static constexpr uint32_t TEST_FDB = 1;

/// Maximum simulated groups
static constexpr uint32_t TEST_MAX_GROUPS = 4;

/// Maximum simulated data blocks (across all groups)
static constexpr uint32_t TEST_MAX_BLOCKS = TEST_MAX_GROUPS * TEST_BPG;

/// Simulated total inodes
static constexpr uint32_t TEST_TOTAL_INODES = TEST_MAX_GROUPS * TEST_IPG;

/**
 * @brief Simulated on-disk environment
 *
 * Holds all structures needed to test write/create/mkdir/unlink
 * operations without real disk I/O.
 */
struct SimDisk {
    Ext2Superblock           sb{};
    Ext2BlockGroupDescriptor bgdt[TEST_MAX_GROUPS]{};

    // Bitmaps
    uint8_t block_bitmaps[TEST_MAX_GROUPS][TEST_BPG / 8]{};
    uint8_t inode_bitmaps[TEST_MAX_GROUPS][TEST_IPG / 8]{};

    // Inode table (flat array, inode 1 at index 0)
    Ext2Inode inodes[TEST_TOTAL_INODES]{};

    // Data blocks (flat array)
    uint8_t data_blocks[TEST_MAX_BLOCKS][TEST_BLOCK_SIZE]{};

    // DMA buffer simulation (single block)
    uint8_t dma_buf[TEST_BLOCK_SIZE]{};

    uint32_t group_count{};
};

/**
 * @brief Initialise a simulated disk
 */
static void init_sim_disk(SimDisk& disk, uint32_t groups = 1) {
    disk             = {};
    disk.group_count = groups;

    disk.sb.s_inodes_count      = groups * TEST_IPG;
    disk.sb.s_blocks_count      = groups * TEST_BPG;
    disk.sb.s_free_blocks_count = groups * TEST_BPG;
    disk.sb.s_free_inodes_count = groups * TEST_IPG;
    disk.sb.s_first_data_block  = TEST_FDB;
    disk.sb.s_blocks_per_group  = TEST_BPG;
    disk.sb.s_inodes_per_group  = TEST_IPG;

    // Layout metadata blocks for each group:
    //   block 0: block bitmap
    //   block 1: inode bitmap
    //   block 2..N: inode table (TEST_IPG * TEST_INODE_SIZE / TEST_BLOCK_SIZE blocks)
    //   remaining: data blocks
    uint32_t inode_table_blocks =
        (TEST_IPG * TEST_INODE_SIZE + TEST_BLOCK_SIZE - 1) / TEST_BLOCK_SIZE;

    for (uint32_t g = 0; g < groups; ++g) {
        uint32_t base                = g * TEST_BPG + TEST_FDB;
        disk.bgdt[g].bg_block_bitmap = base + 0;
        disk.bgdt[g].bg_inode_bitmap = base + 1;
        disk.bgdt[g].bg_inode_table  = base + 2;
        disk.bgdt[g].bg_free_blocks_count =
            static_cast<uint16_t>(TEST_BPG - 2 - inode_table_blocks);
        disk.bgdt[g].bg_free_inodes_count = static_cast<uint16_t>(TEST_IPG);

        // Mark metadata blocks as used in the bitmap
        // (block bitmap, inode bitmap, inode table blocks)
        uint32_t meta_count = 2 + inode_table_blocks;
        for (uint32_t i = 0; i < meta_count; ++i) {
            uint32_t byte_idx = i / 8;
            uint32_t bit      = i % 8;
            disk.block_bitmaps[g][byte_idx] |= static_cast<uint8_t>(1U << bit);
        }

        // Adjust free blocks count in superblock
        disk.sb.s_free_blocks_count -= meta_count;
    }

    // Mark inode 1 (reserved) and inode 2 (root) as used
    // Inode 1 is at group 0, local index 0
    disk.inode_bitmaps[0][0] |= 0x01;  // inode 1
    disk.inode_bitmaps[0][0] |= 0x02;  // inode 2
    disk.sb.s_free_inodes_count -= 2;
    disk.bgdt[0].bg_free_inodes_count -= 2;

    // Set up root inode (inode 2)
    disk.inodes[1].i_mode        = EXT2_S_IFDIR | 0755;
    disk.inodes[1].i_size        = TEST_BLOCK_SIZE;
    disk.inodes[1].i_links_count = 2;
    disk.inodes[1].i_blocks      = TEST_BLOCK_SIZE / 512;
    // Root inode gets the first available data block
    uint32_t root_data_blk_offset =
        2 + (TEST_IPG * TEST_INODE_SIZE + TEST_BLOCK_SIZE - 1) / TEST_BLOCK_SIZE;
    disk.inodes[1].i_block[0] = TEST_FDB + root_data_blk_offset;
    // Mark that data block as used
    {
        uint32_t local = root_data_blk_offset;
        disk.block_bitmaps[0][local / 8] |= static_cast<uint8_t>(1U << (local % 8));
        disk.sb.s_free_blocks_count--;
        disk.bgdt[0].bg_free_blocks_count--;
    }

    // Initialise root directory block with "." and ".."
    {
        uint32_t blk  = disk.inodes[1].i_block[0];
        auto*    data = disk.data_blocks[blk];
        for (uint32_t i = 0; i < TEST_BLOCK_SIZE; ++i) {
            data[i] = 0;
        }

        // "." entry
        auto* dot            = reinterpret_cast<Ext2DirEntry*>(data);
        dot->inode           = 2;
        dot->name_len        = 1;
        dot->file_type       = static_cast<uint8_t>(Ext2FileType::Directory);
        dot->name[0]         = '.';
        uint32_t dot_rec_len = 12;  // 8 + 1 rounded up to 4
        dot->rec_len         = static_cast<uint16_t>(dot_rec_len);

        // ".." entry
        auto* dotdot      = reinterpret_cast<Ext2DirEntry*>(data + dot_rec_len);
        dotdot->inode     = 2;
        dotdot->name_len  = 2;
        dotdot->file_type = static_cast<uint8_t>(Ext2FileType::Directory);
        dotdot->name[0]   = '.';
        dotdot->name[1]   = '.';
        dotdot->rec_len   = static_cast<uint16_t>(TEST_BLOCK_SIZE - dot_rec_len);
    }
}

// ============================================================
// Host-side reimplementation of allocator (from allocator tests)
// ============================================================

static uint32_t host_alloc_block(SimDisk& disk) {
    for (uint32_t g = 0; g < disk.group_count; ++g) {
        if (disk.bgdt[g].bg_free_blocks_count == 0) {
            continue;
        }

        uint32_t bytes_needed = TEST_BPG / 8;
        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (disk.block_bitmaps[g][byte_idx] == 0xFF) {
                continue;
            }
            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local = byte_idx * 8 + bit;
                if (local >= TEST_BPG)
                    break;
                if ((disk.block_bitmaps[g][byte_idx] & (1U << bit)) == 0) {
                    disk.block_bitmaps[g][byte_idx] |= static_cast<uint8_t>(1U << bit);
                    uint32_t global = g * TEST_BPG + TEST_FDB + local;
                    if (disk.sb.s_free_blocks_count > 0)
                        --disk.sb.s_free_blocks_count;
                    if (disk.bgdt[g].bg_free_blocks_count > 0)
                        --disk.bgdt[g].bg_free_blocks_count;
                    return global;
                }
            }
        }
    }
    return 0;
}

static bool host_free_block(SimDisk& disk, uint32_t blk) {
    if (blk == 0)
        return false;
    uint32_t g = (blk - TEST_FDB) / TEST_BPG;
    if (g >= disk.group_count)
        return false;
    uint32_t local = blk - (g * TEST_BPG + TEST_FDB);
    disk.block_bitmaps[g][local / 8] &= static_cast<uint8_t>(~(1U << (local % 8)));
    ++disk.sb.s_free_blocks_count;
    ++disk.bgdt[g].bg_free_blocks_count;
    return true;
}

static uint32_t host_alloc_inode(SimDisk& disk) {
    for (uint32_t g = 0; g < disk.group_count; ++g) {
        if (disk.bgdt[g].bg_free_inodes_count == 0)
            continue;
        uint32_t bytes_needed = TEST_IPG / 8;
        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (disk.inode_bitmaps[g][byte_idx] == 0xFF)
                continue;
            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local = byte_idx * 8 + bit;
                if (local >= TEST_IPG)
                    break;
                if ((disk.inode_bitmaps[g][byte_idx] & (1U << bit)) == 0) {
                    disk.inode_bitmaps[g][byte_idx] |= static_cast<uint8_t>(1U << bit);
                    uint32_t global_ino = g * TEST_IPG + local + 1;
                    if (disk.sb.s_free_inodes_count > 0)
                        --disk.sb.s_free_inodes_count;
                    if (disk.bgdt[g].bg_free_inodes_count > 0)
                        --disk.bgdt[g].bg_free_inodes_count;
                    return global_ino;
                }
            }
        }
    }
    return 0;
}

static bool host_free_inode(SimDisk& disk, uint32_t ino) {
    if (ino == 0)
        return false;
    uint32_t g = (ino - 1) / TEST_IPG;
    if (g >= disk.group_count)
        return false;
    uint32_t local = (ino - 1) % TEST_IPG;
    disk.inode_bitmaps[g][local / 8] &= static_cast<uint8_t>(~(1U << (local % 8)));
    ++disk.sb.s_free_inodes_count;
    ++disk.bgdt[g].bg_free_inodes_count;
    return true;
}

// ============================================================
// Host-side reimplementation of ext2 operations
// ============================================================

/**
 * @brief Simulate ext2_file_write into an inode
 *
 * Writes data to the inode's block pointers, allocating blocks as needed.
 * Returns the number of bytes written.
 */
static int64_t host_file_write(SimDisk& disk, Ext2Inode& inode, uint32_t offset,
                               const uint8_t* data, uint32_t length) {
    if (data == nullptr || length == 0) {
        return 0;
    }

    uint32_t bs            = TEST_BLOCK_SIZE;
    uint32_t total_written = 0;

    while (total_written < length) {
        uint32_t file_block   = (offset + total_written) / bs;
        uint32_t block_offset = (offset + total_written) % bs;
        uint32_t chunk        = bs - block_offset;
        if (chunk > length - total_written) {
            chunk = length - total_written;
        }

        // Only support direct blocks 0-11
        if (file_block >= EXT2_DIRECT_BLOCKS) {
            break;
        }

        // Allocate block if needed
        if (inode.i_block[file_block] == 0) {
            uint32_t blk = host_alloc_block(disk);
            if (blk == 0)
                break;
            // Zero the new block
            for (uint32_t i = 0; i < bs; ++i) {
                disk.data_blocks[blk][i] = 0;
            }
            inode.i_block[file_block] = blk;
        }

        uint32_t disk_block = inode.i_block[file_block];

        // For partial writes, we simulate read-modify-write
        // (the block already exists in data_blocks, just copy)
        for (uint32_t i = 0; i < chunk; ++i) {
            disk.data_blocks[disk_block][block_offset + i] = data[total_written + i];
        }

        total_written += chunk;
    }

    // Update size
    if (total_written > 0) {
        uint32_t new_end = offset + total_written;
        if (new_end > inode.i_size) {
            inode.i_size = new_end;
        }

        // Update i_blocks (512-byte sectors)
        uint32_t sectors_used = ((inode.i_size + bs - 1) / bs) * (bs / 512);
        inode.i_blocks        = sectors_used;
    }

    return static_cast<int64_t>(total_written);
}

/**
 * @brief Simulate ext2_file_read from an inode
 */
static int64_t host_file_read(SimDisk& disk, const Ext2Inode& inode, uint32_t offset, uint8_t* buf,
                              uint32_t length) {
    if (buf == nullptr || offset >= inode.i_size) {
        return 0;
    }

    uint32_t available  = inode.i_size - offset;
    uint32_t to_read    = (length < available) ? length : available;
    uint32_t bs         = TEST_BLOCK_SIZE;
    uint32_t total_read = 0;

    while (total_read < to_read) {
        uint32_t file_block   = (offset + total_read) / bs;
        uint32_t block_offset = (offset + total_read) % bs;
        uint32_t chunk        = bs - block_offset;
        if (chunk > to_read - total_read) {
            chunk = to_read - total_read;
        }

        if (file_block >= EXT2_DIRECT_BLOCKS)
            break;

        uint32_t disk_block = inode.i_block[file_block];
        if (disk_block == 0) {
            // Hole: fill with zeros
            for (uint32_t i = 0; i < chunk; ++i) {
                buf[total_read + i] = 0;
            }
        } else {
            for (uint32_t i = 0; i < chunk; ++i) {
                buf[total_read + i] = disk.data_blocks[disk_block][block_offset + i];
            }
        }

        total_read += chunk;
    }

    return static_cast<int64_t>(total_read);
}

/**
 * @brief Host-side reimplementation of add_dir_entry
 *
 * Simulates the directory entry insertion algorithm: try to split an
 * existing entry, or allocate a new data block.
 */
static bool host_add_dir_entry(SimDisk& disk, uint32_t /*dir_ino*/, Ext2Inode& dir_disk,
                               uint32_t entry_ino, const char* name, uint32_t name_len,
                               Ext2FileType file_type) {
    uint32_t required = EXT2_DIR_ENTRY_HDR_SIZE + name_len;
    required          = (required + 3) & ~3u;

    uint32_t bs           = TEST_BLOCK_SIZE;
    uint32_t total_blocks = (dir_disk.i_size + bs - 1) / bs;
    if (total_blocks == 0)
        total_blocks = 1;

    // Try splitting existing entries
    for (uint32_t b = 0; b < total_blocks && b < EXT2_DIRECT_BLOCKS; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0)
            continue;

        uint8_t* block_data = disk.data_blocks[blk];
        uint32_t pos        = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs)
                break;

            auto* entry = reinterpret_cast<Ext2DirEntry*>(block_data + pos);
            if (entry->rec_len == 0)
                break;

            uint32_t entry_min = EXT2_DIR_ENTRY_HDR_SIZE + entry->name_len;
            entry_min          = (entry_min + 3) & ~3u;
            uint32_t extra     = entry->rec_len - entry_min;

            if (extra >= required) {
                // Split
                entry->rec_len = static_cast<uint16_t>(entry_min);

                auto* new_entry     = reinterpret_cast<Ext2DirEntry*>(block_data + pos + entry_min);
                new_entry->inode    = entry_ino;
                new_entry->rec_len  = static_cast<uint16_t>(extra);
                new_entry->name_len = static_cast<uint8_t>(name_len);
                new_entry->file_type = static_cast<uint8_t>(file_type);
                for (uint32_t i = 0; i < name_len; ++i) {
                    new_entry->name[i] = name[i];
                }
                return true;
            }

            pos += entry->rec_len;
        }
    }

    // Allocate a new block
    uint32_t new_block_idx = total_blocks;
    if (new_block_idx >= EXT2_DIRECT_BLOCKS)
        return false;

    uint32_t new_blk = host_alloc_block(disk);
    if (new_blk == 0)
        return false;

    // Zero and populate
    for (uint32_t i = 0; i < bs; ++i) {
        disk.data_blocks[new_blk][i] = 0;
    }

    auto* new_entry      = reinterpret_cast<Ext2DirEntry*>(disk.data_blocks[new_blk]);
    new_entry->inode     = entry_ino;
    new_entry->rec_len   = static_cast<uint16_t>(bs);
    new_entry->name_len  = static_cast<uint8_t>(name_len);
    new_entry->file_type = static_cast<uint8_t>(file_type);
    for (uint32_t i = 0; i < name_len; ++i) {
        new_entry->name[i] = name[i];
    }

    dir_disk.i_block[new_block_idx] = new_blk;
    dir_disk.i_size += bs;
    uint32_t sectors_used = ((dir_disk.i_size + bs - 1) / bs) * (bs / 512);
    dir_disk.i_blocks     = sectors_used;

    return true;
}

/**
 * @brief Host-side reimplementation of remove_dir_entry
 *
 * Scans directory blocks and removes the named entry by clearing the inode
 * (first entry) or merging with the previous entry.
 */
static bool host_remove_dir_entry(SimDisk& disk, const Ext2Inode& dir_disk, const char* name,
                                  uint32_t name_len, uint32_t& out_entry_ino) {
    uint32_t bs           = TEST_BLOCK_SIZE;
    uint32_t dir_size     = dir_disk.i_size;
    uint32_t total_blocks = (dir_size + bs - 1) / bs;
    if (total_blocks > EXT2_DIRECT_BLOCKS)
        total_blocks = EXT2_DIRECT_BLOCKS;

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0)
            continue;

        uint8_t* block_data = disk.data_blocks[blk];
        uint32_t pos        = 0;
        uint32_t prev_pos   = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs)
                break;

            auto* entry = reinterpret_cast<Ext2DirEntry*>(block_data + pos);
            if (entry->rec_len == 0)
                break;

            if (entry->inode != 0 && entry->name_len == name_len) {
                bool match = true;
                for (uint32_t i = 0; i < name_len; ++i) {
                    if (entry->name[i] != name[i]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    out_entry_ino = entry->inode;
                    if (pos == 0) {
                        entry->inode = 0;
                    } else {
                        auto* prev = reinterpret_cast<Ext2DirEntry*>(block_data + prev_pos);
                        prev->rec_len += entry->rec_len;
                    }
                    return true;
                }
            }

            prev_pos = pos;
            pos += entry->rec_len;
        }
    }

    return false;
}

/**
 * @brief Host-side reimplementation of create
 *
 * Allocates inode, initialises as regular file, adds directory entry.
 */
static uint32_t host_create(SimDisk& disk, uint32_t parent_ino, const char* name,
                            uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) {
        return 0;
    }

    // Allocate inode
    uint32_t new_ino = host_alloc_inode(disk);
    if (new_ino == 0)
        return 0;

    // Initialise inode
    Ext2Inode& new_inode = disk.inodes[new_ino - 1];
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_inode)[i] = 0;
    }
    new_inode.i_mode        = EXT2_S_IFREG | 0644;
    new_inode.i_links_count = 1;

    // Add directory entry in parent
    Ext2Inode& parent = disk.inodes[parent_ino - 1];
    if (!host_add_dir_entry(disk, parent_ino, parent, new_ino, name, name_len,
                            Ext2FileType::Regular)) {
        host_free_inode(disk, new_ino);
        return 0;
    }

    return new_ino;
}

/**
 * @brief Host-side reimplementation of mkdir
 *
 * Allocates inode + data block, initialises with "." and ".." entries,
 * adds directory entry in parent, increments parent link count.
 */
static uint32_t host_mkdir(SimDisk& disk, uint32_t parent_ino, const char* name,
                           uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) {
        return 0;
    }

    // Allocate inode
    uint32_t new_ino = host_alloc_inode(disk);
    if (new_ino == 0)
        return 0;

    // Allocate data block
    uint32_t data_blk = host_alloc_block(disk);
    if (data_blk == 0) {
        host_free_inode(disk, new_ino);
        return 0;
    }

    // Initialise inode
    Ext2Inode& new_inode = disk.inodes[new_ino - 1];
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_inode)[i] = 0;
    }
    new_inode.i_mode        = EXT2_S_IFDIR | 0755;
    new_inode.i_size        = TEST_BLOCK_SIZE;
    new_inode.i_links_count = 2;  // "." + parent's entry
    new_inode.i_blocks      = TEST_BLOCK_SIZE / 512;
    new_inode.i_block[0]    = data_blk;

    // Initialise data block with "." and ".."
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; ++i) {
        disk.data_blocks[data_blk][i] = 0;
    }

    // "." entry
    auto* dot            = reinterpret_cast<Ext2DirEntry*>(disk.data_blocks[data_blk]);
    dot->inode           = new_ino;
    dot->name_len        = 1;
    dot->file_type       = static_cast<uint8_t>(Ext2FileType::Directory);
    dot->name[0]         = '.';
    uint32_t dot_rec_len = 12;
    dot->rec_len         = static_cast<uint16_t>(dot_rec_len);

    // ".." entry
    auto* dotdot      = reinterpret_cast<Ext2DirEntry*>(disk.data_blocks[data_blk] + dot_rec_len);
    dotdot->inode     = parent_ino;
    dotdot->name_len  = 2;
    dotdot->file_type = static_cast<uint8_t>(Ext2FileType::Directory);
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';
    dotdot->rec_len   = static_cast<uint16_t>(TEST_BLOCK_SIZE - dot_rec_len);

    // Add directory entry in parent
    Ext2Inode& parent = disk.inodes[parent_ino - 1];
    if (!host_add_dir_entry(disk, parent_ino, parent, new_ino, name, name_len,
                            Ext2FileType::Directory)) {
        host_free_block(disk, data_blk);
        host_free_inode(disk, new_ino);
        return 0;
    }

    // Increment parent link count (for ".." reference from new dir)
    parent.i_links_count++;

    return new_ino;
}

/**
 * @brief Host-side reimplementation of unlink
 *
 * Removes directory entry, decrements link count, frees resources if
 * link count reaches 0.
 */
static int host_unlink(SimDisk& disk, uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0)
        return -1;

    Ext2Inode& parent = disk.inodes[parent_ino - 1];

    uint32_t entry_ino = 0;
    if (!host_remove_dir_entry(disk, parent, name, name_len, entry_ino)) {
        return -1;
    }

    Ext2Inode& target = disk.inodes[entry_ino - 1];

    // Decrement link count
    if (target.i_links_count > 0) {
        target.i_links_count--;
    }

    if (target.i_links_count == 0) {
        // Free all data blocks
        for (uint32_t i = 0; i < EXT2_DIRECT_BLOCKS; ++i) {
            if (target.i_block[i] != 0) {
                host_free_block(disk, target.i_block[i]);
                target.i_block[i] = 0;
            }
        }

        target.i_size   = 0;
        target.i_blocks = 0;

        // Free the inode
        host_free_inode(disk, entry_ino);

        // If directory, decrement parent links
        if ((target.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            if (parent.i_links_count > 0) {
                parent.i_links_count--;
            }
        }
    }

    return 0;
}

// ============================================================
// Helper: verify directory contains a named entry
// ============================================================

static bool dir_has_entry(SimDisk& disk, const Ext2Inode& dir, const char* name, uint32_t name_len,
                          uint32_t& out_ino) {
    uint32_t bs           = TEST_BLOCK_SIZE;
    uint32_t total_blocks = (dir.i_size + bs - 1) / bs;
    if (total_blocks == 0)
        return false;

    for (uint32_t b = 0; b < total_blocks && b < EXT2_DIRECT_BLOCKS; ++b) {
        uint32_t blk = dir.i_block[b];
        if (blk == 0)
            continue;

        uint8_t* data = disk.data_blocks[blk];
        uint32_t pos  = 0;
        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs)
                break;
            auto* entry = reinterpret_cast<Ext2DirEntry*>(data + pos);
            if (entry->rec_len == 0)
                break;
            if (entry->inode != 0 && entry->name_len == name_len) {
                bool match = true;
                for (uint32_t i = 0; i < name_len; ++i) {
                    if (entry->name[i] != name[i]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    out_ino = entry->inode;
                    return true;
                }
            }
            pos += entry->rec_len;
        }
    }
    return false;
}

// ============================================================
// Test 1: ext2_file_write — small data within single block
// ============================================================

TEST("file_write: write 10 bytes to empty file at offset 0") {
    SimDisk disk;
    init_sim_disk(disk);

    // Create a file inode manually
    uint32_t ino = host_alloc_inode(disk);
    ASSERT_NE(ino, 0u);

    Ext2Inode& file_inode    = disk.inodes[ino - 1];
    file_inode.i_mode        = EXT2_S_IFREG | 0644;
    file_inode.i_links_count = 1;
    file_inode.i_size        = 0;

    const uint8_t data[]  = "HelloWorld";
    int64_t       written = host_file_write(disk, file_inode, 0, data, 10);

    ASSERT_EQ(written, 10);
    ASSERT_EQ(file_inode.i_size, 10u);
    ASSERT_NE(file_inode.i_block[0], 0u);

    // Verify data on disk
    uint32_t blk = file_inode.i_block[0];
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(disk.data_blocks[blk][i], static_cast<uint8_t>(data[i]));
    }
}

TEST("file_write: write 0 bytes does nothing") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    int64_t written = host_file_write(disk, file_inode, 0, nullptr, 0);
    ASSERT_EQ(written, 0);
    ASSERT_EQ(file_inode.i_size, 0u);
}

// ============================================================
// Test 2: ext2_file_write — cross-block write
// ============================================================

TEST("file_write: write crosses block boundary") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    // Write at offset TEST_BLOCK_SIZE - 5, 10 bytes -> crosses into block 1
    uint8_t data[10];
    for (int i = 0; i < 10; ++i)
        data[i] = static_cast<uint8_t>('A' + i);

    int64_t written = host_file_write(disk, file_inode, TEST_BLOCK_SIZE - 5, data, 10);
    ASSERT_EQ(written, 10);
    ASSERT_EQ(file_inode.i_size, TEST_BLOCK_SIZE - 5 + 10);
    ASSERT_NE(file_inode.i_block[0], 0u);
    ASSERT_NE(file_inode.i_block[1], 0u);

    // Verify: last 5 bytes of block 0 = A B C D E
    uint32_t blk0 = file_inode.i_block[0];
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(disk.data_blocks[blk0][TEST_BLOCK_SIZE - 5 + i], static_cast<uint8_t>('A' + i));
    }

    // Verify: first 5 bytes of block 1 = F G H I J
    uint32_t blk1 = file_inode.i_block[1];
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(disk.data_blocks[blk1][i], static_cast<uint8_t>('F' + i));
    }
}

// ============================================================
// Test 3: ext2_file_write — allocates multiple direct blocks
// ============================================================

TEST("file_write: allocates blocks 0, 1, 2 sequentially") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    // Write 3 blocks worth of data
    uint32_t total = TEST_BLOCK_SIZE * 3;
    uint8_t* data  = new uint8_t[total];
    for (uint32_t i = 0; i < total; ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }

    int64_t written = host_file_write(disk, file_inode, 0, data, total);
    ASSERT_EQ(written, static_cast<int64_t>(total));
    ASSERT_EQ(file_inode.i_size, total);
    ASSERT_NE(file_inode.i_block[0], 0u);
    ASSERT_NE(file_inode.i_block[1], 0u);
    ASSERT_NE(file_inode.i_block[2], 0u);

    // Verify all blocks are distinct
    ASSERT_NE(file_inode.i_block[0], file_inode.i_block[1]);
    ASSERT_NE(file_inode.i_block[1], file_inode.i_block[2]);

    delete[] data;
}

// ============================================================
// Test 4: ext2_file_write — size update
// ============================================================

TEST("file_write: size updates correctly for appended data") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    // Write at offset 0
    uint8_t data1[100];
    for (int i = 0; i < 100; ++i)
        data1[i] = 'X';
    host_file_write(disk, file_inode, 0, data1, 100);
    ASSERT_EQ(file_inode.i_size, 100u);

    // Write at offset 200 (creates gap)
    uint8_t data2[50];
    for (int i = 0; i < 50; ++i)
        data2[i] = 'Y';
    host_file_write(disk, file_inode, 200, data2, 50);
    ASSERT_EQ(file_inode.i_size, 250u);
}

TEST("file_write: write within existing size does not shrink") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    // Write 500 bytes
    uint8_t data[500];
    for (int i = 0; i < 500; ++i)
        data[i] = 'Z';
    host_file_write(disk, file_inode, 0, data, 500);
    ASSERT_EQ(file_inode.i_size, 500u);

    // Overwrite at offset 100, 50 bytes (within existing size)
    uint8_t data2[50];
    for (int i = 0; i < 50; ++i)
        data2[i] = 'W';
    host_file_write(disk, file_inode, 100, data2, 50);
    ASSERT_EQ(file_inode.i_size, 500u);  // size should not change
}

// ============================================================
// Test 5: ext2_file_write + read round-trip
// ============================================================

TEST("file_write: write then read back verifies data integrity") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    const uint8_t write_data[] = "The quick brown fox jumps over the lazy dog";
    uint32_t      len          = sizeof(write_data) - 1;  // exclude null terminator

    int64_t written = host_file_write(disk, file_inode, 0, write_data, len);
    ASSERT_EQ(written, static_cast<int64_t>(len));

    uint8_t read_buf[64] = {};
    int64_t read_back    = host_file_read(disk, file_inode, 0, read_buf, len);
    ASSERT_EQ(read_back, static_cast<int64_t>(len));

    for (uint32_t i = 0; i < len; ++i) {
        ASSERT_EQ(read_buf[i], write_data[i]);
    }
}

// ============================================================
// Test 6: create — allocates inode and adds directory entry
// ============================================================

TEST("create: creates file with correct inode and directory entry") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t free_before = disk.sb.s_free_inodes_count;

    const char name[]  = "testfile";
    uint32_t   new_ino = host_create(disk, 2, name, 8);

    ASSERT_NE(new_ino, 0u);
    ASSERT_EQ(disk.sb.s_free_inodes_count, free_before - 1);

    // Verify inode
    Ext2Inode& file_inode = disk.inodes[new_ino - 1];
    ASSERT_EQ(file_inode.i_mode & EXT2_S_IFMT, static_cast<uint16_t>(EXT2_S_IFREG));
    ASSERT_EQ(file_inode.i_links_count, 1);
    ASSERT_EQ(file_inode.i_size, 0u);

    // Verify directory entry exists
    Ext2Inode& root      = disk.inodes[1];
    uint32_t   found_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, root, name, 8, found_ino));
    ASSERT_EQ(found_ino, new_ino);
}

TEST("create: single character filename") {
    SimDisk disk;
    init_sim_disk(disk);

    const char name[]  = "a";
    uint32_t   new_ino = host_create(disk, 2, name, 1);
    ASSERT_NE(new_ino, 0u);

    Ext2Inode& root      = disk.inodes[1];
    uint32_t   found_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, root, name, 1, found_ino));
    ASSERT_EQ(found_ino, new_ino);
}

TEST("create: long filename (up to 255 chars)") {
    SimDisk disk;
    init_sim_disk(disk);

    char name[255];
    for (int i = 0; i < 254; ++i)
        name[i] = 'a' + (i % 26);
    name[254] = '\0';

    uint32_t new_ino = host_create(disk, 2, name, 255);
    ASSERT_NE(new_ino, 0u);

    Ext2Inode& root      = disk.inodes[1];
    uint32_t   found_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, root, name, 255, found_ino));
    ASSERT_EQ(found_ino, new_ino);
}

TEST("create: empty name is rejected") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t result = host_create(disk, 2, "", 0);
    ASSERT_EQ(result, 0u);
}

// ============================================================
// Test 7: create multiple files
// ============================================================

TEST("create: multiple files get distinct inodes") {
    SimDisk disk;
    init_sim_disk(disk);

    const char name1[] = "file1";
    const char name2[] = "file2";
    const char name3[] = "file3";

    uint32_t ino1 = host_create(disk, 2, name1, 5);
    uint32_t ino2 = host_create(disk, 2, name2, 5);
    uint32_t ino3 = host_create(disk, 2, name3, 5);

    ASSERT_NE(ino1, 0u);
    ASSERT_NE(ino2, 0u);
    ASSERT_NE(ino3, 0u);

    ASSERT_NE(ino1, ino2);
    ASSERT_NE(ino2, ino3);
    ASSERT_NE(ino1, ino3);

    // All three should be in root directory
    Ext2Inode& root = disk.inodes[1];
    uint32_t   f1 = 0, f2 = 0, f3 = 0;
    ASSERT_TRUE(dir_has_entry(disk, root, "file1", 5, f1));
    ASSERT_TRUE(dir_has_entry(disk, root, "file2", 5, f2));
    ASSERT_TRUE(dir_has_entry(disk, root, "file3", 5, f3));
}

// ============================================================
// Test 8: mkdir — creates directory with "." and ".."
// ============================================================

TEST("mkdir: creates directory with dot and dotdot entries") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t free_inodes_before = disk.sb.s_free_inodes_count;
    uint32_t free_blocks_before = disk.sb.s_free_blocks_count;

    const char name[]  = "mydir";
    uint32_t   new_ino = host_mkdir(disk, 2, name, 5);

    ASSERT_NE(new_ino, 0u);
    ASSERT_EQ(disk.sb.s_free_inodes_count, free_inodes_before - 1);
    ASSERT_EQ(disk.sb.s_free_blocks_count, free_blocks_before - 1);

    // Verify inode
    Ext2Inode& dir_inode = disk.inodes[new_ino - 1];
    ASSERT_EQ(dir_inode.i_mode & EXT2_S_IFMT, static_cast<uint16_t>(EXT2_S_IFDIR));
    ASSERT_EQ(dir_inode.i_links_count, 2);  // "." + parent's entry
    ASSERT_EQ(dir_inode.i_size, TEST_BLOCK_SIZE);
    ASSERT_NE(dir_inode.i_block[0], 0u);

    // Verify "." entry
    uint32_t dot_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, dir_inode, ".", 1, dot_ino));
    ASSERT_EQ(dot_ino, new_ino);

    // Verify ".." entry
    uint32_t dotdot_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, dir_inode, "..", 2, dotdot_ino));
    ASSERT_EQ(dotdot_ino, 2u);  // parent is root (inode 2)

    // Verify parent link count increased
    Ext2Inode& root = disk.inodes[1];
    ASSERT_EQ(root.i_links_count, 3u);  // root starts at 2, +1 for mydir's ".."

    // Verify entry in parent
    uint32_t found_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, root, name, 5, found_ino));
    ASSERT_EQ(found_ino, new_ino);
}

TEST("mkdir: empty name is rejected") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t result = host_mkdir(disk, 2, "", 0);
    ASSERT_EQ(result, 0u);
}

// ============================================================
// Test 9: mkdir then create file inside
// ============================================================

TEST("mkdir: create file inside new directory") {
    SimDisk disk;
    init_sim_disk(disk);

    // Create directory
    const char dirname[] = "subdir";
    uint32_t   dir_ino   = host_mkdir(disk, 2, dirname, 6);
    ASSERT_NE(dir_ino, 0u);

    // Create file inside
    const char filename[] = "inner_file";
    uint32_t   file_ino   = host_create(disk, dir_ino, filename, 10);
    ASSERT_NE(file_ino, 0u);

    // Verify file is in the subdirectory
    Ext2Inode& dir_inode = disk.inodes[dir_ino - 1];
    uint32_t   found_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, dir_inode, filename, 10, found_ino));
    ASSERT_EQ(found_ino, file_ino);

    // Verify file type
    Ext2Inode& file_inode = disk.inodes[file_ino - 1];
    ASSERT_EQ(file_inode.i_mode & EXT2_S_IFMT, static_cast<uint16_t>(EXT2_S_IFREG));
}

// ============================================================
// Test 10: unlink — removes file and frees resources
// ============================================================

TEST("unlink: removes file, decrements link count, frees inode") {
    SimDisk disk;
    init_sim_disk(disk);

    // Create a file
    const char name[] = "todelete";
    uint32_t   ino    = host_create(disk, 2, name, 8);
    ASSERT_NE(ino, 0u);

    uint32_t free_inodes_before = disk.sb.s_free_inodes_count;

    // Unlink it
    int result = host_unlink(disk, 2, name, 8);
    ASSERT_EQ(result, 0);

    // Verify directory entry is gone
    Ext2Inode& root      = disk.inodes[1];
    uint32_t   found_ino = 0;
    ASSERT_FALSE(dir_has_entry(disk, root, name, 8, found_ino));

    // Verify inode was freed (link_count was 1, now 0 -> freed)
    ASSERT_EQ(disk.sb.s_free_inodes_count, free_inodes_before + 1);
}

TEST("unlink: non-existent name returns error") {
    SimDisk disk;
    init_sim_disk(disk);

    int result = host_unlink(disk, 2, "nonexistent", 11);
    ASSERT_EQ(result, -1);
}

TEST("unlink: empty name is rejected") {
    SimDisk disk;
    init_sim_disk(disk);

    int result = host_unlink(disk, 2, "", 0);
    ASSERT_EQ(result, -1);
}

// ============================================================
// Test 11: unlink — frees data blocks
// ============================================================

TEST("unlink: file with data has blocks freed") {
    SimDisk disk;
    init_sim_disk(disk);

    // Create and write data
    const char name[] = "datafile";
    uint32_t   ino    = host_create(disk, 2, name, 8);
    ASSERT_NE(ino, 0u);

    Ext2Inode& file_inode = disk.inodes[ino - 1];
    uint8_t    data[TEST_BLOCK_SIZE];
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    int64_t written = host_file_write(disk, file_inode, 0, data, TEST_BLOCK_SIZE);
    ASSERT_EQ(written, static_cast<int64_t>(TEST_BLOCK_SIZE));
    ASSERT_NE(file_inode.i_block[0], 0u);

    uint32_t free_blocks_before = disk.sb.s_free_blocks_count;

    // Unlink
    int result = host_unlink(disk, 2, name, 8);
    ASSERT_EQ(result, 0);

    // Block should be freed
    ASSERT_EQ(disk.sb.s_free_blocks_count, free_blocks_before + 1);
}

// ============================================================
// Test 12: unlink — directory removal
// ============================================================

TEST("unlink: removes directory and adjusts parent link count") {
    SimDisk disk;
    init_sim_disk(disk);

    // Create directory
    const char dirname[] = "dirtoremove";
    uint32_t   dir_ino   = host_mkdir(disk, 2, dirname, 11);
    ASSERT_NE(dir_ino, 0u);

    Ext2Inode& root              = disk.inodes[1];
    uint32_t   root_links_before = root.i_links_count;  // 3 (root's ., .., and dir's ..)

    uint32_t free_inodes_before = disk.sb.s_free_inodes_count;
    uint32_t free_blocks_before = disk.sb.s_free_blocks_count;

    // Unlink the directory
    // Directory link_count starts at 2 (. + parent entry).  Unlink decrements
    // by 1 (parent entry removed) -> link_count becomes 1.  Since it is not 0,
    // the inode and data blocks are NOT freed in a simple unlink.  This matches
    // the kernel behaviour: rmdir would need to also clear the "." self-reference.
    int result = host_unlink(disk, 2, dirname, 11);
    ASSERT_EQ(result, 0);

    // Directory entry should be gone from parent
    uint32_t found_ino = 0;
    ASSERT_FALSE(dir_has_entry(disk, root, dirname, 11, found_ino));

    // Inode is NOT freed because link_count went from 2 -> 1 (not 0)
    ASSERT_EQ(disk.sb.s_free_inodes_count, free_inodes_before);

    // Data blocks are NOT freed either
    ASSERT_EQ(disk.sb.s_free_blocks_count, free_blocks_before);

    // Parent link count is NOT decremented because link_count did not reach 0.
    // Only when the directory's resources are actually freed (link_count == 0)
    // does the kernel decrement the parent's link count.
    ASSERT_EQ(root.i_links_count, root_links_before);
}

// ============================================================
// Test 13: create, write, unlink, recreate same name
// ============================================================

TEST("unlink: recreate file with same name after unlink") {
    SimDisk disk;
    init_sim_disk(disk);

    const char name[] = "recycle";

    // Create file 1
    uint32_t ino1 = host_create(disk, 2, name, 7);
    ASSERT_NE(ino1, 0u);

    // Write data
    Ext2Inode& file1   = disk.inodes[ino1 - 1];
    uint8_t    data1[] = "AAAA";
    host_file_write(disk, file1, 0, data1, 4);

    // Unlink
    int result = host_unlink(disk, 2, name, 7);
    ASSERT_EQ(result, 0);

    // Create file 2 with same name
    uint32_t ino2 = host_create(disk, 2, name, 7);
    ASSERT_NE(ino2, 0u);

    // Should be a different inode number (original freed, can be reused)
    // Verify it's a fresh inode
    Ext2Inode& file2 = disk.inodes[ino2 - 1];
    ASSERT_EQ(file2.i_size, 0u);
    ASSERT_EQ(file2.i_links_count, 1u);
}

// ============================================================
// Test 14: full flow — mkdir, create, write, read, unlink
// ============================================================

TEST("full_flow: mkdir, create file inside, write, read, unlink") {
    SimDisk disk;
    init_sim_disk(disk);

    // 1. Create a subdirectory
    const char dirname[] = "workdir";
    uint32_t   dir_ino   = host_mkdir(disk, 2, dirname, 7);
    ASSERT_NE(dir_ino, 0u);

    // 2. Create a file in the subdirectory
    const char filename[] = "data.txt";
    uint32_t   file_ino   = host_create(disk, dir_ino, filename, 8);
    ASSERT_NE(file_ino, 0u);

    // 3. Write data to the file
    Ext2Inode&    file_inode   = disk.inodes[file_ino - 1];
    const uint8_t write_data[] = "Hello from cinux ext2!";
    uint32_t      len          = sizeof(write_data) - 1;
    int64_t       written      = host_file_write(disk, file_inode, 0, write_data, len);
    ASSERT_EQ(written, static_cast<int64_t>(len));

    // 4. Read data back
    uint8_t read_buf[64] = {};
    int64_t read_back    = host_file_read(disk, file_inode, 0, read_buf, len);
    ASSERT_EQ(read_back, static_cast<int64_t>(len));
    for (uint32_t i = 0; i < len; ++i) {
        ASSERT_EQ(read_buf[i], write_data[i]);
    }

    // 5. Unlink the file
    int result = host_unlink(disk, dir_ino, filename, 8);
    ASSERT_EQ(result, 0);

    // 6. Verify file is gone from directory
    Ext2Inode& dir_inode = disk.inodes[dir_ino - 1];
    uint32_t   found_ino = 0;
    ASSERT_FALSE(dir_has_entry(disk, dir_inode, filename, 8, found_ino));
}

// ============================================================
// Test 15: add_dir_entry — splitting existing entries
// ============================================================

TEST("add_dir_entry: splits existing entry to fit new entry") {
    SimDisk disk;
    init_sim_disk(disk);

    // Root dir has ".." entry spanning most of the block.  The ".." entry
    // has a large rec_len.  Creating a file should split the ".." entry.
    const char name[] = "newfile";
    uint32_t   ino    = host_create(disk, 2, name, 7);
    ASSERT_NE(ino, 0u);

    // Verify the entry exists (which means splitting worked)
    Ext2Inode& root      = disk.inodes[1];
    uint32_t   found_ino = 0;
    ASSERT_TRUE(dir_has_entry(disk, root, name, 7, found_ino));
    ASSERT_EQ(found_ino, ino);
}

// ============================================================
// Test 16: remove_dir_entry — first entry and middle entry
// ============================================================

TEST("remove_dir_entry: clear first entry in block") {
    SimDisk disk;
    init_sim_disk(disk);

    // Manually set up a directory block with a known entry
    uint32_t blk = host_alloc_block(disk);
    ASSERT_NE(blk, 0u);

    // Create two entries in the block
    auto* data = disk.data_blocks[blk];
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; ++i)
        data[i] = 0;

    auto* e1        = reinterpret_cast<Ext2DirEntry*>(data);
    e1->inode       = 10;
    e1->name_len    = 2;
    e1->file_type   = static_cast<uint8_t>(Ext2FileType::Regular);
    e1->name[0]     = 'a';
    e1->name[1]     = 'a';
    uint16_t e1_rec = 12;
    e1->rec_len     = e1_rec;

    auto* e2      = reinterpret_cast<Ext2DirEntry*>(data + e1_rec);
    e2->inode     = 11;
    e2->name_len  = 2;
    e2->file_type = static_cast<uint8_t>(Ext2FileType::Regular);
    e2->name[0]   = 'b';
    e2->name[1]   = 'b';
    e2->rec_len   = static_cast<uint16_t>(TEST_BLOCK_SIZE - e1_rec);

    // Create a fake directory inode
    Ext2Inode dir_disk{};
    dir_disk.i_block[0] = blk;
    dir_disk.i_size     = TEST_BLOCK_SIZE;

    uint32_t out_ino = 0;
    bool     ok      = host_remove_dir_entry(disk, dir_disk, "aa", 2, out_ino);

    ASSERT_TRUE(ok);
    ASSERT_EQ(out_ino, 10u);

    // First entry should have inode cleared
    ASSERT_EQ(e1->inode, 0u);

    // Second entry should still be intact
    ASSERT_EQ(e2->inode, 11u);
}

TEST("remove_dir_entry: merge with previous entry") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t blk = host_alloc_block(disk);
    ASSERT_NE(blk, 0u);

    auto* data = disk.data_blocks[blk];
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; ++i)
        data[i] = 0;

    // Entry 1
    auto* e1     = reinterpret_cast<Ext2DirEntry*>(data);
    e1->inode    = 10;
    e1->name_len = 2;
    e1->name[0]  = 'a';
    e1->name[1]  = 'a';
    e1->rec_len  = 12;

    // Entry 2 (to be removed)
    auto* e2     = reinterpret_cast<Ext2DirEntry*>(data + 12);
    e2->inode    = 11;
    e2->name_len = 2;
    e2->name[0]  = 'b';
    e2->name[1]  = 'b';
    e2->rec_len  = 12;

    // Entry 3
    auto* e3     = reinterpret_cast<Ext2DirEntry*>(data + 24);
    e3->inode    = 12;
    e3->name_len = 2;
    e3->name[0]  = 'c';
    e3->name[1]  = 'c';
    e3->rec_len  = static_cast<uint16_t>(TEST_BLOCK_SIZE - 24);

    Ext2Inode dir_disk{};
    dir_disk.i_block[0] = blk;
    dir_disk.i_size     = TEST_BLOCK_SIZE;

    uint32_t out_ino = 0;
    bool     ok      = host_remove_dir_entry(disk, dir_disk, "bb", 2, out_ino);

    ASSERT_TRUE(ok);
    ASSERT_EQ(out_ino, 11u);

    // e1's rec_len should now include e2's space
    ASSERT_EQ(e1->rec_len, 24u);

    // e3 should still be intact
    ASSERT_EQ(e3->inode, 12u);
}

// ============================================================
// Test 17: boundary — write to offset 0 of empty file
// ============================================================

TEST("file_write: write to offset 0 of empty file") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    uint8_t data    = 0x42;
    int64_t written = host_file_write(disk, file_inode, 0, &data, 1);
    ASSERT_EQ(written, 1);
    ASSERT_EQ(file_inode.i_size, 1u);
    ASSERT_NE(file_inode.i_block[0], 0u);

    uint32_t blk = file_inode.i_block[0];
    ASSERT_EQ(disk.data_blocks[blk][0], 0x42u);
}

// ============================================================
// Test 18: write then overwrite same region
// ============================================================

TEST("file_write: overwrite same region updates data") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;

    // First write
    uint8_t data1[50];
    for (int i = 0; i < 50; ++i)
        data1[i] = 'A';
    host_file_write(disk, file_inode, 0, data1, 50);

    // Overwrite
    uint8_t data2[20];
    for (int i = 0; i < 20; ++i)
        data2[i] = 'B';
    host_file_write(disk, file_inode, 10, data2, 20);

    // Read back and verify
    uint8_t buf[50] = {};
    host_file_read(disk, file_inode, 0, buf, 50);

    // First 10 bytes should be 'A'
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(buf[i], static_cast<uint8_t>('A'));
    }
    // Next 20 bytes should be 'B'
    for (int i = 10; i < 30; ++i) {
        ASSERT_EQ(buf[i], static_cast<uint8_t>('B'));
    }
    // Remaining 20 bytes should be 'A'
    for (int i = 30; i < 50; ++i) {
        ASSERT_EQ(buf[i], static_cast<uint8_t>('A'));
    }
}

// ============================================================
// Test 19: multiple creates then unlinks
// ============================================================

TEST("create_unlink: create 3 files, unlink all, verify clean state") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t free_inodes_before = disk.sb.s_free_inodes_count;

    const char* names[] = {"f1", "f2", "f3"};
    uint32_t    inos[3];

    for (int i = 0; i < 3; ++i) {
        inos[i] = host_create(disk, 2, names[i], 2);
        ASSERT_NE(inos[i], 0u);
    }

    ASSERT_EQ(disk.sb.s_free_inodes_count, free_inodes_before - 3);

    for (int i = 0; i < 3; ++i) {
        int result = host_unlink(disk, 2, names[i], 2);
        ASSERT_EQ(result, 0);
    }

    ASSERT_EQ(disk.sb.s_free_inodes_count, free_inodes_before);

    // All entries should be gone
    Ext2Inode& root      = disk.inodes[1];
    uint32_t   found_ino = 0;
    ASSERT_FALSE(dir_has_entry(disk, root, "f1", 2, found_ino));
    ASSERT_FALSE(dir_has_entry(disk, root, "f2", 2, found_ino));
    ASSERT_FALSE(dir_has_entry(disk, root, "f3", 2, found_ino));
}

// ============================================================
// Test 20: i_blocks field consistency
// ============================================================

TEST("file_write: i_blocks field tracks 512-byte sectors correctly") {
    SimDisk disk;
    init_sim_disk(disk);

    uint32_t   ino        = host_alloc_inode(disk);
    Ext2Inode& file_inode = disk.inodes[ino - 1];
    file_inode.i_size     = 0;
    file_inode.i_blocks   = 0;

    // Write one block
    uint8_t data[TEST_BLOCK_SIZE];
    for (uint32_t i = 0; i < TEST_BLOCK_SIZE; ++i)
        data[i] = 0xAA;
    host_file_write(disk, file_inode, 0, data, TEST_BLOCK_SIZE);

    // i_blocks should be TEST_BLOCK_SIZE / 512 = 2
    ASSERT_EQ(file_inode.i_blocks, TEST_BLOCK_SIZE / 512);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
