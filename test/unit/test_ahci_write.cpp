/**
 * @file test/unit/test_ahci_write.cpp
 * @brief Host-side unit tests for AHCI write path and ext2 write_block logic
 *
 * Test coverage:
 *   - CFIS construction for WRITE DMA EXT (command 0x35)
 *   - Write command header flags: write bit = 1
 *   - Block-to-LBA arithmetic used by ext2 write_block
 *   - PRDT byte count for write operations
 *   - Ext2 write_block LBA computation for various block sizes
 *   - Edge cases: block 0, max block number, sector count overflow
 *   - Command header write direction flag
 *   - Write vs read FIS differentiation
 *
 * Pure arithmetic and layout verification -- no kernel code linked.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>

// Include kernel headers for constants and structures
#    include "drivers/ahci/ahci_config.hpp"
#    include "fs/ext2_types.hpp"

using namespace cinux::drivers::ahci;
using namespace cinux::fs;

// ============================================================
// Re-implement CFIS build logic for host testing
//
// The kernel ahci.cpp writes to volatile MMIO registers via VMM
// and PMM, which cannot execute on the host.  We extract the
// pure logic (FIS field encoding) and test it here.
// ============================================================

/**
 * @brief Build the RegH2D FIS (host-side reimplementation)
 *
 * Mirrors AHCI::build_cfis() but works on a plain buffer.
 */
static void build_cfis(uint8_t* cfis_buf, bool write_cmd, uint64_t lba, uint16_t count) {
    auto* fis = reinterpret_cast<RegH2DFIS*>(cfis_buf);

    fis->fis_type = FisType::REG_H2D;
    fis->flags    = 0x80;
    fis->command  = write_cmd ? AtaCmd::WRITE_DMA_EXT : AtaCmd::READ_DMA_EXT;
    fis->feature  = 0;

    fis->lba0   = static_cast<uint8_t>(lba & 0xFF);
    fis->lba1   = static_cast<uint8_t>((lba >> 8) & 0xFF);
    fis->lba2   = static_cast<uint8_t>((lba >> 16) & 0xFF);
    fis->device = 0x40;

    fis->lba3        = static_cast<uint8_t>((lba >> 24) & 0xFF);
    fis->lba4        = static_cast<uint8_t>((lba >> 32) & 0xFF);
    fis->lba5        = static_cast<uint8_t>((lba >> 40) & 0xFF);
    fis->feature_exp = 0;

    fis->count0 = static_cast<uint8_t>(count & 0xFF);
    fis->count1 = static_cast<uint8_t>((count >> 8) & 0xFF);

    fis->control = 0;
}

/**
 * @brief Compute the write command header flags
 *
 * For a write operation, the command header must have the write bit set.
 */
static uint16_t compute_cmd_header_flags(bool write_cmd) {
    uint16_t flags = 0;
    flags |= static_cast<uint8_t>(sizeof(RegH2DFIS) / 4);  // CFL in lower 5 bits
    if (write_cmd) {
        flags |= CmdHdrFlags::WRITE;
    }
    return flags;
}

/**
 * @brief Compute LBA from ext2 block number (mirrors Ext2::write_block logic)
 *
 * lba = block_num * sectors_per_block
 */
static uint64_t block_to_lba(uint32_t block_num, uint32_t sectors_per_block) {
    return static_cast<uint64_t>(block_num) * sectors_per_block;
}

// ============================================================
// 1. CFIS Write Command Selection
// ============================================================

/**
 * @brief Verify CFIS for WRITE of LBA 0, 1 sector uses WRITE_DMA_EXT
 */
TEST("ahci_write: CFIS write command uses WRITE_DMA_EXT") {
    uint8_t buf[64] = {};
    build_cfis(buf, true, 0, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->command, AtaCmd::WRITE_DMA_EXT);
    ASSERT_EQ(fis->command, 0x35u);
}

/**
 * @brief Verify CFIS read uses READ_DMA_EXT, write uses WRITE_DMA_EXT
 */
TEST("ahci_write: CFIS read vs write command differentiation") {
    uint8_t read_buf[64]  = {};
    uint8_t write_buf[64] = {};

    build_cfis(read_buf, false, 0, 1);
    build_cfis(write_buf, true, 0, 1);

    auto* read_fis  = reinterpret_cast<RegH2DFIS*>(read_buf);
    auto* write_fis = reinterpret_cast<RegH2DFIS*>(write_buf);

    ASSERT_EQ(read_fis->command, AtaCmd::READ_DMA_EXT);
    ASSERT_EQ(write_fis->command, AtaCmd::WRITE_DMA_EXT);
    ASSERT_NE(read_fis->command, write_fis->command);
}

// ============================================================
// 2. CFIS Write LBA Encoding
// ============================================================

/**
 * @brief Verify CFIS for WRITE at LBA 0 (block 0 write)
 */
TEST("ahci_write: CFIS write LBA 0 count 1") {
    uint8_t buf[64] = {};
    build_cfis(buf, true, 0, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->fis_type, 0x27);
    ASSERT_EQ(fis->flags, 0x80);
    ASSERT_EQ(fis->command, AtaCmd::WRITE_DMA_EXT);
    ASSERT_EQ(fis->feature, 0);
    ASSERT_EQ(fis->lba0, 0);
    ASSERT_EQ(fis->lba1, 0);
    ASSERT_EQ(fis->lba2, 0);
    ASSERT_EQ(fis->device, 0x40);
    ASSERT_EQ(fis->lba3, 0);
    ASSERT_EQ(fis->lba4, 0);
    ASSERT_EQ(fis->lba5, 0);
    ASSERT_EQ(fis->count0, 1);
    ASSERT_EQ(fis->count1, 0);
    ASSERT_EQ(fis->control, 0);
}

/**
 * @brief Verify CFIS write with a non-zero LBA
 *
 * LBA = 100: lba0=100, lba1-5=0
 */
TEST("ahci_write: CFIS write LBA 100") {
    uint8_t buf[64] = {};
    build_cfis(buf, true, 100, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->lba0, 100);
    ASSERT_EQ(fis->lba1, 0);
    ASSERT_EQ(fis->lba2, 0);
    ASSERT_EQ(fis->lba3, 0);
}

/**
 * @brief Verify CFIS write with 48-bit LBA (high LBA for large disks)
 *
 * LBA = 0x1A2B3C4D5E6F:
 *   lba0 = 0x6F (bits 0-7)
 *   lba1 = 0x5E (bits 8-15)
 *   lba2 = 0x4D (bits 16-23)
 *   lba3 = 0x3C (bits 24-31)
 *   lba4 = 0x2B (bits 32-39)
 *   lba5 = 0x1A (bits 40-47)
 */
TEST("ahci_write: CFIS write 48-bit LBA encoding") {
    uint8_t  buf[64] = {};
    uint64_t lba     = 0x1A2B3C4D5E6FULL;
    build_cfis(buf, true, lba, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->lba0, 0x6F);
    ASSERT_EQ(fis->lba1, 0x5E);
    ASSERT_EQ(fis->lba2, 0x4D);
    ASSERT_EQ(fis->lba3, 0x3C);
    ASSERT_EQ(fis->lba4, 0x2B);
    ASSERT_EQ(fis->lba5, 0x1A);
    ASSERT_EQ(fis->device, 0x40);
}

// ============================================================
// 3. CFIS Write Sector Count
// ============================================================

/**
 * @brief Verify CFIS write sector count of 1
 */
TEST("ahci_write: CFIS write sector count 1") {
    uint8_t buf[64] = {};
    build_cfis(buf, true, 0, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->count0, 1);
    ASSERT_EQ(fis->count1, 0);
}

/**
 * @brief Verify CFIS write sector count of 8 (typical for 4K block)
 */
TEST("ahci_write: CFIS write sector count 8") {
    uint8_t buf[64] = {};
    build_cfis(buf, true, 0, 8);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->count0, 8);
    ASSERT_EQ(fis->count1, 0);
}

/**
 * @brief Verify CFIS write sector count of 256
 */
TEST("ahci_write: CFIS write sector count 256") {
    uint8_t buf[64] = {};
    build_cfis(buf, true, 0, 256);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->count0, 0);
    ASSERT_EQ(fis->count1, 1);
}

// ============================================================
// 4. Command Header Write Flag
// ============================================================

/**
 * @brief Verify command header flags for write: write bit set
 */
TEST("ahci_write: command header write bit is set") {
    uint16_t flags = compute_cmd_header_flags(true);
    ASSERT_TRUE(flags & CmdHdrFlags::WRITE);
}

/**
 * @brief Verify command header flags for read: write bit clear
 */
TEST("ahci_write: command header read has no write bit") {
    uint16_t flags = compute_cmd_header_flags(false);
    ASSERT_FALSE(flags & CmdHdrFlags::WRITE);
}

/**
 * @brief Verify CFL field in command header is 5 dwords (20 bytes / 4)
 */
TEST("ahci_write: CFL is 5 dwords in write header") {
    uint16_t flags = compute_cmd_header_flags(true);
    uint8_t  cfl   = flags & 0x1F;
    ASSERT_EQ(cfl, 5u);
}

/**
 * @brief Verify write header has both CFL and WRITE bit
 */
TEST("ahci_write: write header combines CFL and WRITE") {
    uint16_t flags = compute_cmd_header_flags(true);
    uint8_t  cfl   = flags & 0x1F;
    ASSERT_EQ(cfl, 5u);
    ASSERT_TRUE(flags & CmdHdrFlags::WRITE);
    // WRITE bit is bit 6, CFL is bits 0-4
    // The combined value depends on how the header packs the bit-fields.
    // In the actual HBACommandHeader, cfl (5 bits) and write (1 bit) are
    // separate bit-fields; together they form bits 0-5 of the first byte.
    // CFL=5 -> 0b00101, WRITE=1 -> bit 5 = 0b100000
    // Combined = 0b0100001 = 0x21 = 33
    // Note: CmdHdrFlags::WRITE is 1<<6 = 0x40 for the standalone constant,
    // but in the bit-field layout of HBACommandHeader, the write bit occupies
    // bit 5 of the first byte (after the 5 CFL bits).
    // Our compute_cmd_header_flags uses CmdHdrFlags::WRITE (0x40), so the
    // raw value is CFL | 0x40 = 5 | 0x40 = 0x45.
    ASSERT_EQ(flags, 0x45u);
}

// ============================================================
// 5. PRDT Byte Count for Write Operations
// ============================================================

/**
 * @brief Verify PRDT byte count for writing 1 sector
 */
TEST("ahci_write: PRDT byte count for 1 sector write") {
    uint16_t count      = 1;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    ASSERT_EQ(byte_count, 511u);
}

/**
 * @brief Verify PRDT byte count for writing 2 sectors (1K ext2 block)
 */
TEST("ahci_write: PRDT byte count for 2 sector write") {
    uint16_t count      = 2;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    ASSERT_EQ(byte_count, 1023u);
}

/**
 * @brief Verify PRDT byte count for writing 4 sectors (2K ext2 block)
 */
TEST("ahci_write: PRDT byte count for 4 sector write") {
    uint16_t count      = 4;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    ASSERT_EQ(byte_count, 2047u);
}

/**
 * @brief Verify PRDT byte count for writing 8 sectors (4K ext2 block)
 */
TEST("ahci_write: PRDT byte count for 8 sector write") {
    uint16_t count      = 8;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    ASSERT_EQ(byte_count, 4095u);
}

/**
 * @brief Verify PRDT byte count 22-bit mask for write
 */
TEST("ahci_write: PRDT byte count masked to 22 bits") {
    uint16_t count      = 1;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    uint32_t masked     = byte_count & 0x3FFFFF;
    ASSERT_EQ(masked, 511u);
}

// ============================================================
// 6. Ext2 write_block LBA Computation
// ============================================================

/**
 * @brief Verify block 0 -> LBA 0 for 1024-byte blocks (2 sectors/block)
 */
TEST("ahci_write: ext2 block 0 -> LBA 0 (bs=1024)") {
    uint32_t sectors_per_block = 1024 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(sectors_per_block, 2u);
    ASSERT_EQ(block_to_lba(0, sectors_per_block), 0ull);
}

/**
 * @brief Verify block 1 -> LBA 2 for 1024-byte blocks
 */
TEST("ahci_write: ext2 block 1 -> LBA 2 (bs=1024)") {
    uint32_t sectors_per_block = 1024 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(block_to_lba(1, sectors_per_block), 2ull);
}

/**
 * @brief Verify block 5 -> LBA 10 for 1024-byte blocks
 */
TEST("ahci_write: ext2 block 5 -> LBA 10 (bs=1024)") {
    uint32_t sectors_per_block = 1024 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(block_to_lba(5, sectors_per_block), 10ull);
}

/**
 * @brief Verify block 0 -> LBA 0 for 4096-byte blocks (8 sectors/block)
 */
TEST("ahci_write: ext2 block 0 -> LBA 0 (bs=4096)") {
    uint32_t sectors_per_block = 4096 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(sectors_per_block, 8u);
    ASSERT_EQ(block_to_lba(0, sectors_per_block), 0ull);
}

/**
 * @brief Verify block 1 -> LBA 8 for 4096-byte blocks
 */
TEST("ahci_write: ext2 block 1 -> LBA 8 (bs=4096)") {
    uint32_t sectors_per_block = 4096 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(block_to_lba(1, sectors_per_block), 8ull);
}

/**
 * @brief Verify block 100 -> LBA 800 for 4096-byte blocks
 */
TEST("ahci_write: ext2 block 100 -> LBA 800 (bs=4096)") {
    uint32_t sectors_per_block = 4096 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(block_to_lba(100, sectors_per_block), 800ull);
}

// ============================================================
// 7. Boundary Conditions for write_block
// ============================================================

/**
 * @brief Verify maximum block number does not overflow LBA (bs=1024)
 *
 * With 1024-byte blocks (2 sectors/block), max block number before
 * 48-bit LBA overflow is 2^47 / 2 = 2^46.
 * Test a large but safe value: block 0x00100000 -> LBA 0x00200000
 */
TEST("ahci_write: large block number LBA (bs=1024)") {
    uint32_t sectors_per_block = 1024 / EXT2_SECTOR_SIZE;
    uint32_t block_num         = 0x00100000u;
    uint64_t lba               = block_to_lba(block_num, sectors_per_block);
    ASSERT_EQ(lba, 0x00200000ull);
}

/**
 * @brief Verify near-maximum 32-bit block number (bs=4096)
 *
 * block 0xFFFFFFFF * 8 = 0x7FFFFFFF8 (fits in 48 bits)
 */
TEST("ahci_write: max uint32_t block number LBA (bs=4096)") {
    uint32_t sectors_per_block = 4096 / EXT2_SECTOR_SIZE;
    uint32_t block_num         = 0xFFFFFFFFu;
    uint64_t lba               = block_to_lba(block_num, sectors_per_block);
    ASSERT_EQ(lba, 0x7FFFFFFF8ull);
}

/**
 * @brief Verify block 0 write: LBA and count are correct
 *
 * Writing block 0 should produce LBA=0 with the appropriate sector count.
 * This is the superblock-adjacent area (or block group descriptor table
 * for bs >= 2048), so it's an important edge case.
 */
TEST("ahci_write: block 0 LBA and sector count") {
    // For bs=1024
    uint32_t spb_1k = 1024 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(block_to_lba(0, spb_1k), 0ull);
    ASSERT_EQ(spb_1k, 2u);

    // For bs=4096
    uint32_t spb_4k = 4096 / EXT2_SECTOR_SIZE;
    ASSERT_EQ(block_to_lba(0, spb_4k), 0ull);
    ASSERT_EQ(spb_4k, 8u);
}

// ============================================================
// 8. Write Follows Read Pattern (read-modify-write)
// ============================================================

/**
 * @brief Verify the read-modify-write LBA sequence for the same block
 *
 * When doing read_block(n) followed by write_block(n), both should
 * target the same LBA with the same sector count.
 */
TEST("ahci_write: read and write target same LBA for same block") {
    uint32_t block_num         = 42;
    uint32_t sectors_per_block = 1024 / EXT2_SECTOR_SIZE;

    uint64_t read_lba  = block_to_lba(block_num, sectors_per_block);
    uint64_t write_lba = block_to_lba(block_num, sectors_per_block);

    ASSERT_EQ(read_lba, write_lba);
    ASSERT_EQ(read_lba, 84ull);
}

/**
 * @brief Verify CFIS for read then write of the same block
 *
 * Both should have the same LBA and count, differing only in command.
 */
TEST("ahci_write: CFIS read-then-write same block") {
    uint32_t block_num         = 10;
    uint32_t sectors_per_block = 2;  // 1024-byte blocks
    uint64_t lba               = block_to_lba(block_num, sectors_per_block);
    uint16_t count             = static_cast<uint16_t>(sectors_per_block);

    uint8_t read_buf[64]  = {};
    uint8_t write_buf[64] = {};

    build_cfis(read_buf, false, lba, count);
    build_cfis(write_buf, true, lba, count);

    auto* read_fis  = reinterpret_cast<RegH2DFIS*>(read_buf);
    auto* write_fis = reinterpret_cast<RegH2DFIS*>(write_buf);

    // LBA fields should be identical
    ASSERT_EQ(read_fis->lba0, write_fis->lba0);
    ASSERT_EQ(read_fis->lba1, write_fis->lba1);
    ASSERT_EQ(read_fis->lba2, write_fis->lba2);
    ASSERT_EQ(read_fis->lba3, write_fis->lba3);
    ASSERT_EQ(read_fis->lba4, write_fis->lba4);
    ASSERT_EQ(read_fis->lba5, write_fis->lba5);

    // Count should be identical
    ASSERT_EQ(read_fis->count0, write_fis->count0);
    ASSERT_EQ(read_fis->count1, write_fis->count1);

    // Only command differs
    ASSERT_EQ(read_fis->command, AtaCmd::READ_DMA_EXT);
    ASSERT_EQ(write_fis->command, AtaCmd::WRITE_DMA_EXT);
}

// ============================================================
// 9. Ext2 Superblock Block Write Protection Arithmetic
// ============================================================

/**
 * @brief Verify superblock block number for different block sizes
 *
 * For bs=1024: superblock is in block 1 (at byte offset 1024)
 * For bs>=2048: superblock is in block 0 (at byte offset 1024 within block 0)
 */
TEST("ahci_write: superblock block number for bs=1024") {
    uint32_t bs       = 1024;
    uint32_t sb_block = static_cast<uint32_t>(EXT2_SUPERBLOCK_OFFSET / bs);
    ASSERT_EQ(sb_block, 1u);
}

TEST("ahci_write: superblock block number for bs=4096") {
    uint32_t bs       = 4096;
    uint32_t sb_block = static_cast<uint32_t>(EXT2_SUPERBLOCK_OFFSET / bs);
    ASSERT_EQ(sb_block, 0u);
}

/**
 * @brief Verify BGDT block number for bs=1024 (starts at block 2)
 */
TEST("ahci_write: BGDT block for bs=1024") {
    uint32_t bs         = 1024;
    uint32_t bgdt_block = (bs == 1024) ? 2 : 1;
    ASSERT_EQ(bgdt_block, 2u);
}

/**
 * @brief Verify BGDT block number for bs=4096 (starts at block 1)
 */
TEST("ahci_write: BGDT block for bs=4096") {
    uint32_t bs         = 4096;
    uint32_t bgdt_block = (bs == 1024) ? 2 : 1;
    ASSERT_EQ(bgdt_block, 1u);
}

// ============================================================
// 10. Write DMA EXT Opcode Verification
// ============================================================

/**
 * @brief Verify WRITE_DMA_EXT opcode value
 */
TEST("ahci_write: WRITE_DMA_EXT opcode is 0x35") {
    ASSERT_EQ(AtaCmd::WRITE_DMA_EXT, 0x35u);
}

/**
 * @brief Verify WRITE and READ opcodes are distinct
 */
TEST("ahci_write: WRITE and READ opcodes are different") {
    ASSERT_NE(AtaCmd::WRITE_DMA_EXT, AtaCmd::READ_DMA_EXT);
}

/**
 * @brief Verify command header write bit position
 */
TEST("ahci_write: CmdHdrFlags::WRITE is bit 6") {
    ASSERT_EQ(CmdHdrFlags::WRITE, 1u << 6);
}

// ============================================================
// 11. PRDT Setup for Write
// ============================================================

/**
 * @brief Verify PRDT entry setup for a write operation
 */
TEST("ahci_write: PRDT entry fields for write") {
    HBAPrdtEntry prd{};
    std::memset(&prd, 0, sizeof(prd));

    uint64_t buf_phys   = 0x12345000ULL;
    uint16_t count      = 2;  // 2 sectors = 1024 bytes
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;

    prd.dba  = static_cast<uint32_t>(buf_phys & 0xFFFFFFFF);
    prd.dbau = static_cast<uint32_t>(buf_phys >> 32);
    prd.dbc  = byte_count & 0x3FFFFF;
    prd.i    = 1;

    ASSERT_EQ(prd.dba, 0x12345000u);
    ASSERT_EQ(prd.dbau, 0x0u);
    ASSERT_EQ(prd.dbc, 1023u);
    ASSERT_EQ(prd.i, 1u);
}

/**
 * @brief Verify PRDT interrupt-on-completion flag is set for writes
 */
TEST("ahci_write: PRDT interrupt flag set") {
    HBAPrdtEntry prd{};
    std::memset(&prd, 0, sizeof(prd));

    prd.i = 1;  // Interrupt on completion
    ASSERT_EQ(prd.i, 1u);
}

// ============================================================
// 12. Port Index Validation for Write
// ============================================================

/**
 * @brief Verify MAX_PORTS limit is respected
 */
TEST("ahci_write: MAX_PORTS is 32") {
    ASSERT_EQ(MAX_PORTS, 32u);
}

/**
 * @brief Verify port index bounds: valid range is 0-31
 */
TEST("ahci_write: valid port index range") {
    uint8_t valid_port = 0;
    ASSERT_TRUE(valid_port < MAX_PORTS);

    valid_port = 31;
    ASSERT_TRUE(valid_port < MAX_PORTS);
}

/**
 * @brief Verify port index 32 is out of bounds
 */
TEST("ahci_write: port index 32 is out of bounds") {
    uint8_t invalid_port = 32;
    ASSERT_FALSE(invalid_port < MAX_PORTS);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
