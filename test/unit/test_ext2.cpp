/**
 * @file test/unit/test_ext2.cpp
 * @brief Host-side unit tests for ext2 filesystem data structures and constants
 *
 * Test coverage:
 *   - Ext2Superblock size is exactly 1024 bytes
 *   - Ext2Superblock field offsets match the on-disk layout
 *   - Ext2BlockGroupDescriptor size is exactly 32 bytes
 *   - Ext2BlockGroupDescriptor field offsets
 *   - Ext2Inode size is exactly 128 bytes
 *   - Ext2Inode field offsets
 *   - Ext2DirEntry header size and name offset
 *   - Block size computation: 1024 << s_log_block_size
 *   - Group/inode offset arithmetic formulas
 *   - ext2 constants: magic, block counts, mode masks
 *   - Ext2FileType enum class values
 *
 * Pure arithmetic and layout verification -- no kernel code linked.
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
// 1. Ext2Superblock size
// ============================================================

// The ext2 superblock must be exactly 1024 bytes to match the on-disk layout.
TEST("ext2_superblock: sizeof is 1024") {
    ASSERT_EQ(sizeof(Ext2Superblock), 1024u);
}

// ============================================================
// 2. Ext2Superblock field offsets
// ============================================================

// Verify critical field offsets within Ext2Superblock match the ext2 spec.
TEST("ext2_superblock: s_inodes_count at offset 0") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_inodes_count), 0u);
}

TEST("ext2_superblock: s_blocks_count at offset 4") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_blocks_count), 4u);
}

TEST("ext2_superblock: s_log_block_size at offset 24") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_log_block_size), 24u);
}

TEST("ext2_superblock: s_blocks_per_group at offset 32") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_blocks_per_group), 32u);
}

TEST("ext2_superblock: s_inodes_per_group at offset 40") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_inodes_per_group), 40u);
}

TEST("ext2_superblock: s_magic at offset 56") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_magic), 56u);
}

TEST("ext2_superblock: s_rev_level at offset 76") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_rev_level), 76u);
}

TEST("ext2_superblock: s_inode_size at offset 88") {
    ASSERT_EQ(offsetof(Ext2Superblock, s_inode_size), 88u);
}

// ============================================================
// 3. Ext2BlockGroupDescriptor size and offsets
// ============================================================

TEST("ext2_bgd: sizeof is 32") {
    ASSERT_EQ(sizeof(Ext2BlockGroupDescriptor), 32u);
}

TEST("ext2_bgd: bg_block_bitmap at offset 0") {
    ASSERT_EQ(offsetof(Ext2BlockGroupDescriptor, bg_block_bitmap), 0u);
}

TEST("ext2_bgd: bg_inode_bitmap at offset 4") {
    ASSERT_EQ(offsetof(Ext2BlockGroupDescriptor, bg_inode_bitmap), 4u);
}

TEST("ext2_bgd: bg_inode_table at offset 8") {
    ASSERT_EQ(offsetof(Ext2BlockGroupDescriptor, bg_inode_table), 8u);
}

TEST("ext2_bgd: bg_free_blocks_count at offset 12") {
    ASSERT_EQ(offsetof(Ext2BlockGroupDescriptor, bg_free_blocks_count), 12u);
}

TEST("ext2_bgd: bg_free_inodes_count at offset 14") {
    ASSERT_EQ(offsetof(Ext2BlockGroupDescriptor, bg_free_inodes_count), 14u);
}

TEST("ext2_bgd: bg_used_dirs_count at offset 16") {
    ASSERT_EQ(offsetof(Ext2BlockGroupDescriptor, bg_used_dirs_count), 16u);
}

// ============================================================
// 4. Ext2Inode size and offsets
// ============================================================

TEST("ext2_inode: sizeof is 128") {
    ASSERT_EQ(sizeof(Ext2Inode), 128u);
}

TEST("ext2_inode: i_mode at offset 0") {
    ASSERT_EQ(offsetof(Ext2Inode, i_mode), 0u);
}

TEST("ext2_inode: i_uid at offset 2") {
    ASSERT_EQ(offsetof(Ext2Inode, i_uid), 2u);
}

TEST("ext2_inode: i_size at offset 4") {
    ASSERT_EQ(offsetof(Ext2Inode, i_size), 4u);
}

TEST("ext2_inode: i_blocks at offset 28") {
    ASSERT_EQ(offsetof(Ext2Inode, i_blocks), 28u);
}

TEST("ext2_inode: i_block at offset 40") {
    ASSERT_EQ(offsetof(Ext2Inode, i_block), 40u);
}

// i_block array: 15 uint32_t entries = 60 bytes, starting at offset 40
TEST("ext2_inode: i_block array size is 60 bytes") {
    ASSERT_EQ(sizeof(Ext2Inode::i_block), 60u);
}

// ============================================================
// 5. Ext2DirEntry header
// ============================================================

TEST("ext2_dirent: header size is 8") {
    ASSERT_EQ(EXT2_DIR_ENTRY_HDR_SIZE, 8u);
}

TEST("ext2_dirent: name field at offset 8") {
    ASSERT_EQ(offsetof(Ext2DirEntry, name), 8u);
}

TEST("ext2_dirent: inode at offset 0") {
    ASSERT_EQ(offsetof(Ext2DirEntry, inode), 0u);
}

TEST("ext2_dirent: rec_len at offset 4") {
    ASSERT_EQ(offsetof(Ext2DirEntry, rec_len), 4u);
}

TEST("ext2_dirent: name_len at offset 6") {
    ASSERT_EQ(offsetof(Ext2DirEntry, name_len), 6u);
}

TEST("ext2_dirent: file_type at offset 7") {
    ASSERT_EQ(offsetof(Ext2DirEntry, file_type), 7u);
}

// ============================================================
// 6. Block size computation
// ============================================================

// block_size = 1024 << s_log_block_size
TEST("ext2_block_size: s_log_block_size=0 yields 1024") {
    uint32_t bs = 1024u << 0;
    ASSERT_EQ(bs, 1024u);
}

TEST("ext2_block_size: s_log_block_size=1 yields 2048") {
    uint32_t bs = 1024u << 1;
    ASSERT_EQ(bs, 2048u);
}

TEST("ext2_block_size: s_log_block_size=2 yields 4096") {
    uint32_t bs = 1024u << 2;
    ASSERT_EQ(bs, 4096u);
}

TEST("ext2_block_size: s_log_block_size=3 yields 8192") {
    uint32_t bs = 1024u << 3;
    ASSERT_EQ(bs, 8192u);
}

// ============================================================
// 7. Group/inode offset arithmetic
// ============================================================

// Group number = (ino - 1) / inodes_per_group
TEST("ext2_group: inode 1 -> group 0 when ipg=16") {
    uint32_t ino   = 1;
    uint32_t ipg   = 16;
    uint32_t group = (ino - 1) / ipg;
    ASSERT_EQ(group, 0u);
}

TEST("ext2_group: inode 16 -> group 0 when ipg=16") {
    uint32_t ino   = 16;
    uint32_t ipg   = 16;
    uint32_t group = (ino - 1) / ipg;
    ASSERT_EQ(group, 0u);
}

TEST("ext2_group: inode 17 -> group 1 when ipg=16") {
    uint32_t ino   = 17;
    uint32_t ipg   = 16;
    uint32_t group = (ino - 1) / ipg;
    ASSERT_EQ(group, 1u);
}

TEST("ext2_group: inode 32 -> group 1 when ipg=16") {
    uint32_t ino   = 32;
    uint32_t ipg   = 16;
    uint32_t group = (ino - 1) / ipg;
    ASSERT_EQ(group, 1u);
}

TEST("ext2_group: inode 33 -> group 2 when ipg=16") {
    uint32_t ino   = 33;
    uint32_t ipg   = 16;
    uint32_t group = (ino - 1) / ipg;
    ASSERT_EQ(group, 2u);
}

// Index within group = (ino - 1) % inodes_per_group
TEST("ext2_index: inode 1 -> index 0 when ipg=16") {
    uint32_t ino   = 1;
    uint32_t ipg   = 16;
    uint32_t index = (ino - 1) % ipg;
    ASSERT_EQ(index, 0u);
}

TEST("ext2_index: inode 17 -> index 0 when ipg=16") {
    uint32_t ino   = 17;
    uint32_t ipg   = 16;
    uint32_t index = (ino - 1) % ipg;
    ASSERT_EQ(index, 0u);
}

TEST("ext2_index: inode 20 -> index 3 when ipg=16") {
    uint32_t ino   = 20;
    uint32_t ipg   = 16;
    uint32_t index = (ino - 1) % ipg;
    ASSERT_EQ(index, 3u);
}

// Byte offset within inode table = index * inode_size
TEST("ext2_byte_offset: index 0, inode_size 128 -> byte_offset 0") {
    uint64_t byte_offset = 0ull * 128;
    ASSERT_EQ(byte_offset, 0ull);
}

TEST("ext2_byte_offset: index 3, inode_size 128 -> byte_offset 384") {
    uint64_t byte_offset = 3ull * 128;
    ASSERT_EQ(byte_offset, 384ull);
}

// Which block of the inode table: block_offset = byte_offset / block_size
TEST("ext2_block_offset: byte_offset 0, bs=1024 -> block 0") {
    uint32_t block_offset = 0 / 1024;
    ASSERT_EQ(block_offset, 0u);
}

TEST("ext2_block_offset: byte_offset 1024, bs=1024 -> block 1") {
    uint32_t block_offset = 1024 / 1024;
    ASSERT_EQ(block_offset, 1u);
}

TEST("ext2_block_offset: byte_offset 384, bs=1024 -> block 0") {
    uint32_t block_offset = 384 / 1024;
    ASSERT_EQ(block_offset, 0u);
}

// Within-block offset = byte_offset % block_size
TEST("ext2_within_block: byte_offset 384, bs=1024 -> 384") {
    uint32_t within = 384 % 1024;
    ASSERT_EQ(within, 384u);
}

TEST("ext2_within_block: byte_offset 1024, bs=1024 -> 0") {
    uint32_t within = 1024 % 1024;
    ASSERT_EQ(within, 0u);
}

// Group count = (blocks_count + bpg - 1) / bpg
TEST("ext2_group_count: 8192 blocks, bpg=8192 -> 1 group") {
    uint32_t gc = (8192 + 8192 - 1) / 8192;
    ASSERT_EQ(gc, 1u);
}

TEST("ext2_group_count: 8193 blocks, bpg=8192 -> 2 groups") {
    uint32_t gc = (8193 + 8192 - 1) / 8192;
    ASSERT_EQ(gc, 2u);
}

TEST("ext2_group_count: 100 blocks, bpg=8192 -> 1 group") {
    uint32_t gc = (100 + 8192 - 1) / 8192;
    ASSERT_EQ(gc, 1u);
}

// ============================================================
// 8. ext2 constants
// ============================================================

TEST("ext2_const: EXT2_SUPER_MAGIC is 0xEF53") {
    ASSERT_EQ(EXT2_SUPER_MAGIC, 0xEF53u);
}

TEST("ext2_const: EXT2_INODE_SIZE_DEFAULT is 128") {
    ASSERT_EQ(EXT2_INODE_SIZE_DEFAULT, 128u);
}

TEST("ext2_const: EXT2_DIRECT_BLOCKS is 12") {
    ASSERT_EQ(EXT2_DIRECT_BLOCKS, 12u);
}

TEST("ext2_const: EXT2_INDIRECT_BLOCK is 12") {
    ASSERT_EQ(EXT2_INDIRECT_BLOCK, 12u);
}

TEST("ext2_const: EXT2_DOUBLE_INDIRECT_BLOCK is 13") {
    ASSERT_EQ(EXT2_DOUBLE_INDIRECT_BLOCK, 13u);
}

TEST("ext2_const: EXT2_TOTAL_BLOCK_PTRS is 15") {
    ASSERT_EQ(EXT2_TOTAL_BLOCK_PTRS, 15u);
}

TEST("ext2_const: EXT2_SECTOR_SIZE is 512") {
    ASSERT_EQ(EXT2_SECTOR_SIZE, 512u);
}

TEST("ext2_const: EXT2_SUPERBLOCK_OFFSET is 1024") {
    ASSERT_EQ(EXT2_SUPERBLOCK_OFFSET, 1024ull);
}

TEST("ext2_const: EXT2_SUPERBLOCK_SIZE is 1024") {
    ASSERT_EQ(EXT2_SUPERBLOCK_SIZE, 1024u);
}

TEST("ext2_const: EXT2_S_IFMT is 0xF000") {
    ASSERT_EQ(EXT2_S_IFMT, 0xF000u);
}

TEST("ext2_const: EXT2_S_IFREG is 0x8000") {
    ASSERT_EQ(EXT2_S_IFREG, 0x8000u);
}

TEST("ext2_const: EXT2_S_IFDIR is 0x4000") {
    ASSERT_EQ(EXT2_S_IFDIR, 0x4000u);
}

// ============================================================
// 9. Ext2FileType enum class values
// ============================================================

TEST("ext2_filetype: Unknown is 0") {
    ASSERT_EQ(static_cast<uint8_t>(Ext2FileType::Unknown), 0u);
}

TEST("ext2_filetype: Regular is 1") {
    ASSERT_EQ(static_cast<uint8_t>(Ext2FileType::Regular), 1u);
}

TEST("ext2_filetype: Directory is 2") {
    ASSERT_EQ(static_cast<uint8_t>(Ext2FileType::Directory), 2u);
}

TEST("ext2_filetype: Symlink is 6") {
    ASSERT_EQ(static_cast<uint8_t>(Ext2FileType::Symlink), 6u);
}

// ============================================================
// 10. Sectors-per-block computation
// ============================================================

TEST("ext2_sectors_per_block: bs=1024 -> 2 sectors") {
    uint32_t spb = 1024 / 512;
    ASSERT_EQ(spb, 2u);
}

TEST("ext2_sectors_per_block: bs=2048 -> 4 sectors") {
    uint32_t spb = 2048 / 512;
    ASSERT_EQ(spb, 4u);
}

TEST("ext2_sectors_per_block: bs=4096 -> 8 sectors") {
    uint32_t spb = 4096 / 512;
    ASSERT_EQ(spb, 8u);
}

// ============================================================
// 11. Superblock LBA computation
// ============================================================

// Superblock at byte offset 1024, sector size 512 -> LBA 2
TEST("ext2_sb_lba: offset 1024 / 512 = 2") {
    uint64_t lba = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE;
    ASSERT_EQ(lba, 2ull);
}

// Superblock is 1024 bytes -> 2 sectors to read
TEST("ext2_sb_sectors: size 1024 / 512 = 2") {
    uint16_t sectors = EXT2_SUPERBLOCK_SIZE / EXT2_SECTOR_SIZE;
    ASSERT_EQ(sectors, 2u);
}

// ============================================================
// 12. Block pointers per indirect block
// ============================================================

TEST("ext2_indirect: bs=1024 -> 256 block pointers per indirect block") {
    uint64_t ptrs = 1024 / sizeof(uint32_t);
    ASSERT_EQ(ptrs, 256ull);
}

TEST("ext2_indirect: bs=4096 -> 1024 block pointers per indirect block") {
    uint64_t ptrs = 4096 / sizeof(uint32_t);
    ASSERT_EQ(ptrs, 1024ull);
}

// ============================================================
// 13. Max file size with direct blocks only (bs=1024)
// ============================================================

TEST("ext2_max_direct: 12 blocks * 1024 = 12288 bytes") {
    uint64_t max_direct = 12ull * 1024;
    ASSERT_EQ(max_direct, 12288ull);
}

TEST("ext2_max_direct: 12 blocks * 4096 = 49152 bytes") {
    uint64_t max_direct = 12ull * 4096;
    ASSERT_EQ(max_direct, 49152ull);
}

// ============================================================
// 14. Inode mode type extraction
// ============================================================

TEST("ext2_mode: directory mode 0x41ED -> type 0x4000") {
    uint16_t mode = 0x41ED;
    uint16_t type = mode & EXT2_S_IFMT;
    ASSERT_EQ(type, EXT2_S_IFDIR);
}

TEST("ext2_mode: regular file mode 0x81A4 -> type 0x8000") {
    uint16_t mode = 0x81A4;
    uint16_t type = mode & EXT2_S_IFMT;
    ASSERT_EQ(type, EXT2_S_IFREG);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
