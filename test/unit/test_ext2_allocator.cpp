/**
 * @file test/unit/test_ext2_allocator.cpp
 * @brief Host-side unit tests for ext2 block/inode allocator logic
 *
 * Test coverage:
 *   - alloc_block bitmap scanning: first free bit, skip full bytes
 *   - alloc_block returns 0 when all groups full
 *   - alloc_block bitmap cross-group: group 0 full, finds in group 1
 *   - alloc_block last group with fewer blocks
 *   - free_block bit clearing and count updates
 *   - free_block rejects block 0
 *   - alloc_inode bitmap scanning: first free bit, 1-based numbering
 *   - alloc_inode returns 0 when all groups full
 *   - free_inode bit clearing and count updates
 *   - free_inode rejects inode 0
 *   - alloc/free round-trip: free then realloc yields same or new slot
 *   - Consecutive allocations advance through bitmap
 *   - Counters (s_free_blocks_count, bg_free_blocks_count) update correctly
 *
 * Pure arithmetic and bitmap manipulation -- no kernel code linked.
 * The allocator logic is re-implemented here to test independently.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstddef>
#    include <cstdint>
#    include <cstring>

#    include "fs/ext2_types.hpp"

using namespace cinux::fs;

// ============================================================
// Re-implement allocator logic for host testing
//
// The kernel ext2.cpp uses AHCI DMA buffers and VMM/PMM for disk
// I/O.  We extract the pure bitmap scanning algorithm and test it
// here against in-memory buffers that simulate on-disk bitmaps.
// ============================================================

/// Maximum block groups supported (matches kernel)
static constexpr uint32_t TEST_MAX_GROUPS = 128;

/// Simulated filesystem parameters for testing
struct TestFsParams {
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t first_data_block;
    uint32_t group_count;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t block_size;
    uint16_t inode_size;

    // Mutable state
    Ext2Superblock           sb{};
    Ext2BlockGroupDescriptor bgdt[TEST_MAX_GROUPS]{};

    // In-memory bitmap storage (one block-size buffer per group for blocks,
    // one for inodes).  We use a flat array for simplicity.
    uint8_t block_bitmaps[TEST_MAX_GROUPS][512];  // 4096 bits = 512 bytes
    uint8_t inode_bitmaps[TEST_MAX_GROUPS][512];
};

/**
 * @brief Initialise a test filesystem with given parameters
 *
 * Sets up superblock, BGDT, and zeroed bitmaps.
 */
static void init_test_fs(TestFsParams& fs, uint32_t groups = 1, uint32_t bpg = 8192,
                         uint32_t ipg = 16, uint32_t total_blocks = 8192,
                         uint32_t total_inodes = 16, uint32_t block_size = 1024,
                         uint32_t first_data_block = 1) {
    fs = {};

    fs.blocks_per_group = bpg;
    fs.inodes_per_group = ipg;
    fs.first_data_block = first_data_block;
    fs.group_count      = groups;
    fs.blocks_count     = total_blocks;
    fs.inodes_count     = total_inodes;
    fs.block_size       = block_size;
    fs.inode_size       = 128;

    fs.sb.s_blocks_count     = total_blocks;
    fs.sb.s_inodes_count     = total_inodes;
    fs.sb.s_blocks_per_group = bpg;
    fs.sb.s_inodes_per_group = ipg;
    fs.sb.s_first_data_block = first_data_block;

    // Compute free counts based on total - used
    // Initially all blocks/inodes are "free" except those used by metadata.
    // For testing, we just set free = total and bitmaps all-zero.
    fs.sb.s_free_blocks_count = total_blocks;
    fs.sb.s_free_inodes_count = total_inodes;

    for (uint32_t g = 0; g < groups; ++g) {
        fs.bgdt[g].bg_block_bitmap = g * 10 + 3;  // arbitrary block numbers
        fs.bgdt[g].bg_inode_bitmap = g * 10 + 4;
        fs.bgdt[g].bg_inode_table  = g * 10 + 5;
        fs.bgdt[g].bg_free_blocks_count =
            static_cast<uint16_t>((g < groups - 1) ? bpg : (total_blocks - g * bpg));
        fs.bgdt[g].bg_free_inodes_count =
            static_cast<uint16_t>((g < groups - 1) ? ipg : (total_inodes - g * ipg));
    }

    // Bitmaps are already zero (all bits clear = all free)
}

/**
 * @brief Host-side reimplementation of alloc_block bitmap logic
 *
 * Scans the block bitmap for the given group and allocates the first
 * free block.  Returns the global block number, or 0 if none free.
 */
static uint32_t host_alloc_block(TestFsParams& fs, uint32_t group) {
    if (group >= fs.group_count) {
        return 0;
    }

    if (fs.bgdt[group].bg_free_blocks_count == 0) {
        return 0;
    }

    uint32_t bpg         = fs.blocks_per_group;
    uint32_t first_block = group * bpg + fs.first_data_block;

    // Last group may have fewer blocks
    uint32_t blocks_in_group = bpg;
    if (first_block + blocks_in_group > fs.blocks_count) {
        blocks_in_group = fs.blocks_count - first_block;
    }

    uint32_t bytes_needed = (blocks_in_group + 7) / 8;

    for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
        if (fs.block_bitmaps[group][byte_idx] == 0xFF) {
            continue;
        }

        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t local_block = byte_idx * 8 + bit;
            if (local_block >= blocks_in_group) {
                break;
            }

            if ((fs.block_bitmaps[group][byte_idx] & (1U << bit)) == 0) {
                // Found a free block -- mark it used
                fs.block_bitmaps[group][byte_idx] |= static_cast<uint8_t>(1U << bit);

                uint32_t global_block = first_block + local_block;

                // Update counters
                if (fs.sb.s_free_blocks_count > 0) {
                    --fs.sb.s_free_blocks_count;
                }
                if (fs.bgdt[group].bg_free_blocks_count > 0) {
                    --fs.bgdt[group].bg_free_blocks_count;
                }

                return global_block;
            }
        }
    }

    return 0;
}

/**
 * @brief Host-side reimplementation of free_block logic
 */
static bool host_free_block(TestFsParams& fs, uint32_t block_num) {
    if (block_num == 0) {
        return false;
    }

    uint32_t group = (block_num - fs.first_data_block) / fs.blocks_per_group;
    if (group >= fs.group_count) {
        return false;
    }

    uint32_t local_block = block_num - (group * fs.blocks_per_group + fs.first_data_block);

    uint32_t byte_idx = local_block / 8;
    uint32_t bit      = local_block % 8;

    fs.block_bitmaps[group][byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    ++fs.sb.s_free_blocks_count;
    ++fs.bgdt[group].bg_free_blocks_count;

    return true;
}

/**
 * @brief Host-side reimplementation of alloc_inode logic
 */
static uint32_t host_alloc_inode(TestFsParams& fs, uint32_t group) {
    if (group >= fs.group_count) {
        return 0;
    }

    if (fs.bgdt[group].bg_free_inodes_count == 0) {
        return 0;
    }

    uint32_t ipg          = fs.inodes_per_group;
    uint32_t bytes_needed = (ipg + 7) / 8;

    for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
        if (fs.inode_bitmaps[group][byte_idx] == 0xFF) {
            continue;
        }

        for (uint32_t bit = 0; bit < 8; ++bit) {
            uint32_t local_index = byte_idx * 8 + bit;
            if (local_index >= ipg) {
                break;
            }

            if ((fs.inode_bitmaps[group][byte_idx] & (1U << bit)) == 0) {
                // Found a free inode -- mark it used
                fs.inode_bitmaps[group][byte_idx] |= static_cast<uint8_t>(1U << bit);

                // 1-based inode number
                uint32_t global_ino = group * ipg + local_index + 1;

                // Update counters
                if (fs.sb.s_free_inodes_count > 0) {
                    --fs.sb.s_free_inodes_count;
                }
                if (fs.bgdt[group].bg_free_inodes_count > 0) {
                    --fs.bgdt[group].bg_free_inodes_count;
                }

                return global_ino;
            }
        }
    }

    return 0;
}

/**
 * @brief Host-side reimplementation of free_inode logic
 */
static bool host_free_inode(TestFsParams& fs, uint32_t ino) {
    if (ino == 0) {
        return false;
    }

    uint32_t group = (ino - 1) / fs.inodes_per_group;
    if (group >= fs.group_count) {
        return false;
    }

    uint32_t local_index = (ino - 1) % fs.inodes_per_group;

    uint32_t byte_idx = local_index / 8;
    uint32_t bit      = local_index % 8;

    fs.inode_bitmaps[group][byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    ++fs.sb.s_free_inodes_count;
    ++fs.bgdt[group].bg_free_inodes_count;

    return true;
}

// ============================================================
// 1. alloc_block: normal allocation
// ============================================================

// With all bitmaps zero, alloc_block should return the first block
// in group 0: first_data_block + 0.
TEST("alloc_block: first alloc returns first_data_block") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t blk = host_alloc_block(fs, 0);
    ASSERT_EQ(blk, 1u);  // first_data_block=1, local_block=0
}

TEST("alloc_block: first alloc with first_data_block=0") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 0);

    uint32_t blk = host_alloc_block(fs, 0);
    ASSERT_EQ(blk, 0u);  // first_data_block=0, local_block=0
}

// ============================================================
// 2. alloc_block: consecutive allocations advance through bitmap
// ============================================================

TEST("alloc_block: consecutive allocs return sequential blocks") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t b1 = host_alloc_block(fs, 0);
    uint32_t b2 = host_alloc_block(fs, 0);
    uint32_t b3 = host_alloc_block(fs, 0);

    ASSERT_EQ(b1, 1u);
    ASSERT_EQ(b2, 2u);
    ASSERT_EQ(b3, 3u);
}

TEST("alloc_block: consecutive allocs update free count") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t initial_free = fs.sb.s_free_blocks_count;
    ASSERT_EQ(initial_free, 8192u);

    host_alloc_block(fs, 0);
    ASSERT_EQ(fs.sb.s_free_blocks_count, 8191u);

    host_alloc_block(fs, 0);
    ASSERT_EQ(fs.sb.s_free_blocks_count, 8190u);

    host_alloc_block(fs, 0);
    ASSERT_EQ(fs.sb.s_free_blocks_count, 8189u);
}

// ============================================================
// 3. alloc_block: skip full bytes in bitmap
// ============================================================

TEST("alloc_block: skips full bytes (0xFF)") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    // Mark the first byte of the bitmap as full (8 blocks used)
    fs.block_bitmaps[0][0] = 0xFF;

    // Next alloc should skip to byte index 1, bit 0 -> local_block=8
    uint32_t blk = host_alloc_block(fs, 0);
    ASSERT_EQ(blk, 1u + 8u);  // first_data_block + 8
}

TEST("alloc_block: skips multiple full bytes") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    // Mark first 4 bytes full (32 blocks used)
    fs.block_bitmaps[0][0] = 0xFF;
    fs.block_bitmaps[0][1] = 0xFF;
    fs.block_bitmaps[0][2] = 0xFF;
    fs.block_bitmaps[0][3] = 0xFF;

    // Next alloc should return local_block=32
    uint32_t blk = host_alloc_block(fs, 0);
    ASSERT_EQ(blk, 1u + 32u);
}

// ============================================================
// 4. alloc_block: bitmap cross-group (group 0 full, finds in group 1)
// ============================================================

TEST("alloc_block: group 0 full, allocates from group 1") {
    TestFsParams fs;
    init_test_fs(fs, 2, 8, 16, 16, 32, 1024, 1);
    // 2 groups, 8 blocks each, total 16 blocks
    // Group 0: blocks 1-8, Group 1: blocks 9-16

    // Mark all bits in group 0 bitmap as used
    fs.block_bitmaps[0][0]          = 0xFF;  // 8 blocks -> 1 byte
    fs.bgdt[0].bg_free_blocks_count = 0;

    // Alloc from group 0 should fail
    uint32_t blk0 = host_alloc_block(fs, 0);
    ASSERT_EQ(blk0, 0u);

    // Alloc from group 1 should succeed
    uint32_t blk1 = host_alloc_block(fs, 1);
    ASSERT_EQ(blk1, 9u);  // group 1 starts at 1 + 8 = 9
}

// ============================================================
// 5. alloc_block: returns 0 when all groups full
// ============================================================

TEST("alloc_block: returns 0 when all groups full") {
    TestFsParams fs;
    init_test_fs(fs, 2, 8, 16, 16, 32, 1024, 1);

    // Mark everything used
    fs.block_bitmaps[0][0]          = 0xFF;
    fs.block_bitmaps[1][0]          = 0xFF;
    fs.bgdt[0].bg_free_blocks_count = 0;
    fs.bgdt[1].bg_free_blocks_count = 0;

    uint32_t blk = host_alloc_block(fs, 0);
    ASSERT_EQ(blk, 0u);

    blk = host_alloc_block(fs, 1);
    ASSERT_EQ(blk, 0u);
}

// ============================================================
// 6. alloc_block: last group with fewer blocks
// ============================================================

TEST("alloc_block: last group has fewer blocks") {
    TestFsParams fs;
    init_test_fs(fs, 2, 8, 16, 15, 32, 1024, 1);
    // 2 groups, bpg=8, total=15
    // Group 0: first_block=1, blocks_in_group=min(8,15-1)=8 -> blocks 1-8
    // Group 1: first_block=9, blocks_in_group=min(8,15-9)=6 -> blocks 9-14

    // Mark group 0 full
    fs.block_bitmaps[0][0]          = 0xFF;
    fs.bgdt[0].bg_free_blocks_count = 0;

    // Group 1 has 6 blocks (9-14), allocate 3 then verify exhaustion
    uint32_t b1 = host_alloc_block(fs, 1);
    uint32_t b2 = host_alloc_block(fs, 1);
    uint32_t b3 = host_alloc_block(fs, 1);

    ASSERT_EQ(b1, 9u);   // first block in group 1
    ASSERT_EQ(b2, 10u);  // second block in group 1
    ASSERT_EQ(b3, 11u);  // third block in group 1
}

// ============================================================
// 7. alloc_block: bit-level allocation within a byte
// ============================================================

TEST("alloc_block: finds bit 3 free in a partially used byte") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    // Mark bits 0,1,2 used, bit 3 free
    fs.block_bitmaps[0][0] = 0x07;  // 0b00000111

    uint32_t blk = host_alloc_block(fs, 0);
    ASSERT_EQ(blk, 1u + 3u);  // first_data_block + bit 3
}

TEST("alloc_block: finds highest free bit in a byte") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8, 16, 9, 16, 1024, 1);
    // total=9, bpg=8, fdb=1 -> group 0: first_block=1, blocks_in_group=min(8,9-1)=8

    // Mark bits 0-6 used, only bit 7 free
    fs.block_bitmaps[0][0] = 0x7F;  // 0b01111111

    uint32_t blk = host_alloc_block(fs, 0);
    ASSERT_EQ(blk, 1u + 7u);  // first_data_block + bit 7 = local_block 7
}

// ============================================================
// 8. free_block: basic free and count update
// ============================================================

TEST("free_block: clears bit and increments free count") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    // Allocate 3 blocks
    host_alloc_block(fs, 0);
    host_alloc_block(fs, 0);
    host_alloc_block(fs, 0);
    ASSERT_EQ(fs.sb.s_free_blocks_count, 8189u);

    // Free block 2 (first_data_block=1, local_block=1)
    bool ok = host_free_block(fs, 2);
    ASSERT_TRUE(ok);
    ASSERT_EQ(fs.sb.s_free_blocks_count, 8190u);
    ASSERT_EQ(fs.bgdt[0].bg_free_blocks_count, 8190u);

    // Verify the bit is cleared
    // Block 2: local_block = 2 - 1 = 1, byte 0, bit 1
    ASSERT_FALSE(fs.block_bitmaps[0][0] & (1U << 1));
}

TEST("free_block: rejects block 0") {
    TestFsParams fs;
    init_test_fs(fs);

    bool ok = host_free_block(fs, 0);
    ASSERT_FALSE(ok);
}

TEST("free_block: rejects out-of-range group") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    // Block number that would be in group 1 (but only group 0 exists)
    uint32_t bad_block = 1 + 8192 + 5;
    bool     ok        = host_free_block(fs, bad_block);
    ASSERT_FALSE(ok);
}

// ============================================================
// 9. free_block: freed block can be re-allocated
// ============================================================

TEST("free_block: freed block is re-allocatable") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t b1 = host_alloc_block(fs, 0);
    ASSERT_EQ(b1, 1u);

    uint32_t b2 = host_alloc_block(fs, 0);
    ASSERT_EQ(b2, 2u);

    // Free block 1
    host_free_block(fs, 1);

    // Next alloc should get block 1 again (lowest free bit)
    uint32_t b3 = host_alloc_block(fs, 0);
    ASSERT_EQ(b3, 1u);
}

// ============================================================
// 10. alloc_inode: normal allocation, 1-based numbering
// ============================================================

TEST("alloc_inode: first alloc returns inode 1 (1-based)") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t ino = host_alloc_inode(fs, 0);
    ASSERT_EQ(ino, 1u);
}

TEST("alloc_inode: consecutive allocs return 1, 2, 3") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t i1 = host_alloc_inode(fs, 0);
    uint32_t i2 = host_alloc_inode(fs, 0);
    uint32_t i3 = host_alloc_inode(fs, 0);

    ASSERT_EQ(i1, 1u);
    ASSERT_EQ(i2, 2u);
    ASSERT_EQ(i3, 3u);
}

TEST("alloc_inode: updates free inode count") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    ASSERT_EQ(fs.sb.s_free_inodes_count, 16u);

    host_alloc_inode(fs, 0);
    ASSERT_EQ(fs.sb.s_free_inodes_count, 15u);

    host_alloc_inode(fs, 0);
    ASSERT_EQ(fs.sb.s_free_inodes_count, 14u);
}

// ============================================================
// 11. alloc_inode: skip full bytes in bitmap
// ============================================================

TEST("alloc_inode: skips full bytes") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    // Mark first byte full (inodes 1-8 used)
    fs.inode_bitmaps[0][0] = 0xFF;

    // Next alloc should give inode 9 (local_index=8, +1 for 1-based)
    uint32_t ino = host_alloc_inode(fs, 0);
    ASSERT_EQ(ino, 9u);
}

// ============================================================
// 12. alloc_inode: returns 0 when all groups full
// ============================================================

TEST("alloc_inode: returns 0 when all groups full") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);
    // ipg=16, so 2 bytes of bitmap

    fs.inode_bitmaps[0][0]          = 0xFF;
    fs.inode_bitmaps[0][1]          = 0xFF;
    fs.bgdt[0].bg_free_inodes_count = 0;

    uint32_t ino = host_alloc_inode(fs, 0);
    ASSERT_EQ(ino, 0u);
}

TEST("alloc_inode: returns 0 when group free count is 0") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    fs.bgdt[0].bg_free_inodes_count = 0;

    uint32_t ino = host_alloc_inode(fs, 0);
    ASSERT_EQ(ino, 0u);
}

// ============================================================
// 13. alloc_inode: cross-group allocation
// ============================================================

TEST("alloc_inode: group 0 full, allocates from group 1") {
    TestFsParams fs;
    init_test_fs(fs, 2, 8192, 8, 16384, 16, 1024, 1);
    // 2 groups, ipg=8, total_inodes=16
    // Group 0: inodes 1-8, Group 1: inodes 9-16

    // Mark group 0 full
    fs.inode_bitmaps[0][0]          = 0xFF;
    fs.bgdt[0].bg_free_inodes_count = 0;

    // Alloc from group 0 fails
    uint32_t ino0 = host_alloc_inode(fs, 0);
    ASSERT_EQ(ino0, 0u);

    // Alloc from group 1 succeeds -> inode 9 (group*ipg + 0 + 1)
    uint32_t ino1 = host_alloc_inode(fs, 1);
    ASSERT_EQ(ino1, 9u);
}

// ============================================================
// 14. free_inode: basic free and count update
// ============================================================

TEST("free_inode: clears bit and increments free count") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    host_alloc_inode(fs, 0);
    host_alloc_inode(fs, 0);
    ASSERT_EQ(fs.sb.s_free_inodes_count, 14u);

    // Free inode 1
    bool ok = host_free_inode(fs, 1);
    ASSERT_TRUE(ok);
    ASSERT_EQ(fs.sb.s_free_inodes_count, 15u);

    // Verify bit cleared: inode 1 -> local_index 0 -> byte 0 bit 0
    ASSERT_FALSE(fs.inode_bitmaps[0][0] & (1U << 0));
}

TEST("free_inode: rejects inode 0") {
    TestFsParams fs;
    init_test_fs(fs);

    bool ok = host_free_inode(fs, 0);
    ASSERT_FALSE(ok);
}

TEST("free_inode: rejects out-of-range group") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    // Inode that would belong to group 1 (ipg=16, so ino 17 = group 1)
    bool ok = host_free_inode(fs, 17);
    ASSERT_FALSE(ok);
}

// ============================================================
// 15. free_inode: freed inode can be re-allocated
// ============================================================

TEST("free_inode: freed inode is re-allocatable") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t i1 = host_alloc_inode(fs, 0);
    ASSERT_EQ(i1, 1u);

    uint32_t i2 = host_alloc_inode(fs, 0);
    ASSERT_EQ(i2, 2u);

    // Free inode 1
    host_free_inode(fs, 1);

    // Next alloc should give inode 1 again
    uint32_t i3 = host_alloc_inode(fs, 0);
    ASSERT_EQ(i3, 1u);
}

// ============================================================
// 16. alloc/free round-trip: cycle of alloc and free
// ============================================================

TEST("alloc_block_free_cycle: 3 allocs, free middle, realloc") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t b1 = host_alloc_block(fs, 0);
    uint32_t b2 = host_alloc_block(fs, 0);
    uint32_t b3 = host_alloc_block(fs, 0);
    ASSERT_EQ(b1, 1u);
    ASSERT_EQ(b2, 2u);
    ASSERT_EQ(b3, 3u);

    // Free b2 (middle)
    host_free_block(fs, b2);

    // Next alloc returns b2 (lowest free)
    uint32_t b4 = host_alloc_block(fs, 0);
    ASSERT_EQ(b4, 2u);
}

TEST("alloc_inode_free_cycle: 3 allocs, free middle, realloc") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    uint32_t i1 = host_alloc_inode(fs, 0);
    uint32_t i2 = host_alloc_inode(fs, 0);
    uint32_t i3 = host_alloc_inode(fs, 0);
    ASSERT_EQ(i1, 1u);
    ASSERT_EQ(i2, 2u);
    ASSERT_EQ(i3, 3u);

    // Free i2
    host_free_inode(fs, i2);

    uint32_t i4 = host_alloc_inode(fs, 0);
    ASSERT_EQ(i4, 2u);
}

// ============================================================
// 17. Multiple alloc/free cycles (stress)
// ============================================================

TEST("alloc_block_stress: alloc all then free all") {
    TestFsParams fs;
    init_test_fs(fs, 1, 64, 16, 65, 16, 1024, 1);
    // bpg=64, total=65, fdb=1
    // Group 0: first_block=1, blocks_in_group=min(64,65-1)=64

    uint32_t allocated[64];
    for (uint32_t i = 0; i < 64; ++i) {
        allocated[i] = host_alloc_block(fs, 0);
        ASSERT_EQ(allocated[i], 1u + i);
    }

    ASSERT_EQ(fs.sb.s_free_blocks_count, 1u);  // started at 65, allocated 64

    // All allocated in group 0, next should fail
    uint32_t extra = host_alloc_block(fs, 0);
    ASSERT_EQ(extra, 0u);

    // Free all
    for (uint32_t i = 0; i < 64; ++i) {
        bool ok = host_free_block(fs, allocated[i]);
        ASSERT_TRUE(ok);
    }

    ASSERT_EQ(fs.sb.s_free_blocks_count, 65u);

    // Can allocate again
    uint32_t b = host_alloc_block(fs, 0);
    ASSERT_EQ(b, 1u);
}

TEST("alloc_inode_stress: alloc all then free all") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 32, 8192, 32, 1024, 1);
    // ipg=32, total=32 inodes

    uint32_t allocated[32];
    for (uint32_t i = 0; i < 32; ++i) {
        allocated[i] = host_alloc_inode(fs, 0);
        ASSERT_EQ(allocated[i], 1u + i);
    }

    ASSERT_EQ(fs.sb.s_free_inodes_count, 0u);

    uint32_t extra = host_alloc_inode(fs, 0);
    ASSERT_EQ(extra, 0u);

    for (uint32_t i = 0; i < 32; ++i) {
        bool ok = host_free_inode(fs, allocated[i]);
        ASSERT_TRUE(ok);
    }

    ASSERT_EQ(fs.sb.s_free_inodes_count, 32u);

    uint32_t ino = host_alloc_inode(fs, 0);
    ASSERT_EQ(ino, 1u);
}

// ============================================================
// 18. Block group descriptor counter consistency
// ============================================================

TEST("bgdt_counter: block alloc decrements group free_blocks_count") {
    TestFsParams fs;
    init_test_fs(fs, 2, 8, 16, 16, 32, 1024, 1);
    // 2 groups, bpg=8

    ASSERT_EQ(fs.bgdt[0].bg_free_blocks_count, 8u);
    ASSERT_EQ(fs.bgdt[1].bg_free_blocks_count, 8u);

    host_alloc_block(fs, 0);
    ASSERT_EQ(fs.bgdt[0].bg_free_blocks_count, 7u);

    host_alloc_block(fs, 1);
    ASSERT_EQ(fs.bgdt[1].bg_free_blocks_count, 7u);
}

TEST("bgdt_counter: block free increments group free_blocks_count") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    host_alloc_block(fs, 0);
    host_alloc_block(fs, 0);
    ASSERT_EQ(fs.bgdt[0].bg_free_blocks_count, 8190u);

    host_free_block(fs, 2);
    ASSERT_EQ(fs.bgdt[0].bg_free_blocks_count, 8191u);
}

TEST("bgdt_counter: inode alloc decrements group free_inodes_count") {
    TestFsParams fs;
    init_test_fs(fs, 2, 8192, 8, 16384, 16, 1024, 1);

    ASSERT_EQ(fs.bgdt[0].bg_free_inodes_count, 8u);
    ASSERT_EQ(fs.bgdt[1].bg_free_inodes_count, 8u);

    host_alloc_inode(fs, 0);
    ASSERT_EQ(fs.bgdt[0].bg_free_inodes_count, 7u);
}

TEST("bgdt_counter: inode free increments group free_inodes_count") {
    TestFsParams fs;
    init_test_fs(fs, 1, 8192, 16, 8192, 16, 1024, 1);

    host_alloc_inode(fs, 0);
    ASSERT_EQ(fs.bgdt[0].bg_free_inodes_count, 15u);

    host_free_inode(fs, 1);
    ASSERT_EQ(fs.bgdt[0].bg_free_inodes_count, 16u);
}

// ============================================================
// 19. Edge case: single block in last group
// ============================================================

TEST("alloc_block: single block in last group") {
    TestFsParams fs;
    init_test_fs(fs, 2, 8, 16, 10, 32, 1024, 1);
    // 2 groups, bpg=8, total=10, fdb=1
    // Group 0: first_block=1, blocks_in_group=min(8,10-1)=8 -> blocks 1-8
    // Group 1: first_block=9, blocks_in_group=min(8,10-9)=1 -> block 9 only

    uint32_t b = host_alloc_block(fs, 1);
    ASSERT_EQ(b, 9u);

    // No more in group 1
    uint32_t b2 = host_alloc_block(fs, 1);
    ASSERT_EQ(b2, 0u);
}

// ============================================================
// 20. Bitmap bit manipulation correctness
// ============================================================

TEST("bitmap_ops: set and clear a specific bit") {
    uint8_t bitmap[4] = {};

    // Set bit 10
    bitmap[1] |= static_cast<uint8_t>(1U << 2);  // byte 1, bit 2
    ASSERT_EQ(bitmap[1], 0x04u);
    ASSERT_TRUE(bitmap[1] & (1U << 2));

    // Clear bit 10
    bitmap[1] &= static_cast<uint8_t>(~(1U << 2));
    ASSERT_EQ(bitmap[1], 0x00u);
    ASSERT_FALSE(bitmap[1] & (1U << 2));
}

TEST("bitmap_ops: all bits set then all cleared") {
    uint8_t bitmap[2] = {};

    // Set all 16 bits
    bitmap[0] = 0xFF;
    bitmap[1] = 0xFF;

    // Clear all
    for (int i = 0; i < 16; ++i) {
        bitmap[i / 8] &= static_cast<uint8_t>(~(1U << (i % 8)));
    }

    ASSERT_EQ(bitmap[0], 0x00u);
    ASSERT_EQ(bitmap[1], 0x00u);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
