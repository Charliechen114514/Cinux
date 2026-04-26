/**
 * @file kernel/test/test_ahci_write.cpp
 * @brief QEMU in-kernel integration tests for AHCI write and ext2 write_block (028b)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that:
 *   - AHCI::write() can write a sector to the test disk (port 0)
 *   - Written data can be read back and verified (write-then-read round-trip)
 *   - Ext2::write_block() can write a block and read it back
 *   - Block 0 write/read round-trip works
 *   - Multiple writes to the same block overwrite correctly
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised (needed for DMA buffer allocation)
 *   - VMM initialised (needed for DMA buffer mapping)
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/fs/ext2.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::ahci::AHCI;
using cinux::drivers::ahci::SECTOR_SIZE;
using cinux::fs::Ext2;
using cinux::mm::g_pmm;
using cinux::mm::g_vmm;

// ============================================================
// Helper: create an initialised AHCI for testing
// ============================================================

namespace {

/**
 * @brief Initialise PCI, find AHCI, and create an AHCI instance
 *
 * Returns a heap-allocated AHCI instance.  The caller owns it.
 * On failure, returns nullptr.
 */
AHCI* setup_ahci() {
    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return nullptr;
    }

    AHCI* ahci = new AHCI();
    ahci->init(ahci_dev);
    if (ahci->hba_mem() == nullptr) {
        delete ahci;
        return nullptr;
    }

    return ahci;
}

/// Tear down the AHCI instance
void teardown_ahci(AHCI*& ahci) {
    delete ahci;
    ahci = nullptr;
}

/**
 * @brief Allocate and map a DMA buffer for read/write testing
 *
 * Returns a struct with physical and virtual addresses.
 * On failure, phys will be 0.
 */
struct DmaBuffer {
    uint64_t phys;
    uint64_t virt;
};

DmaBuffer alloc_dma_buffer() {
    DmaBuffer buf{0, 0};

    buf.phys = g_pmm.alloc_page();
    if (buf.phys == 0) {
        return buf;
    }

    constexpr uint64_t BUF_VIRT  = 0xFFFFFFFF80300000ULL;
    constexpr uint64_t map_flags = 0x03;  // present + writable
    if (!g_vmm.map(BUF_VIRT, buf.phys, map_flags)) {
        g_pmm.free_page(buf.phys);
        buf.phys = 0;
        return buf;
    }

    buf.virt = BUF_VIRT;
    return buf;
}

void free_dma_buffer(DmaBuffer& buf) {
    if (buf.virt != 0) {
        g_vmm.unmap(buf.virt);
    }
    if (buf.phys != 0) {
        g_pmm.free_page(buf.phys);
    }
    buf.phys = 0;
    buf.virt = 0;
}

}  // anonymous namespace

// ============================================================
// Test 1: AHCI write sector 0 round-trip
// ============================================================

namespace test_ahci_write {

/**
 * @brief Write a known pattern to sector 0, read it back, verify
 *
 * This test writes to port 0 (the AHCI test disk, not the ext2 disk).
 * Steps:
 *   1. Read sector 0 (save original content)
 *   2. Write a test pattern to sector 0
 *   3. Read sector 0 again
 *   4. Verify the test pattern matches
 *   5. Restore original content
 */
void test_write_sector0_roundtrip() {
    AHCI* ahci = setup_ahci();
    TEST_ASSERT_NOT_NULL(ahci);

    DmaBuffer buf = alloc_dma_buffer();
    TEST_ASSERT_NE(buf.phys, 0u);

    auto* data = reinterpret_cast<uint8_t*>(buf.virt);

    // Step 1: Read original sector 0
    bool ok = ahci->read(0, 0, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Save original content
    uint8_t original[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        original[i] = data[i];
    }

    // Step 2: Write a test pattern
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        data[i] = static_cast<uint8_t>(i & 0xFF);
    }
    ok = ahci->write(0, 0, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Step 3: Read back sector 0
    ok = ahci->read(0, 0, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Step 4: Verify the test pattern
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        TEST_ASSERT_EQ(data[i], static_cast<uint8_t>(i & 0xFF));
    }

    // Step 5: Restore original content
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        data[i] = original[i];
    }
    ok = ahci->write(0, 0, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    cinux::lib::kprintf("[AHCI_WRITE] Sector 0 write-read roundtrip OK\n");

    free_dma_buffer(buf);
    teardown_ahci(ahci);
}

/**
 * @brief Write to a higher LBA to verify non-zero LBA writes work
 *
 * Use LBA 100 (well beyond the MBR area) to avoid any critical data.
 */
void test_write_higher_lba() {
    AHCI* ahci = setup_ahci();
    TEST_ASSERT_NOT_NULL(ahci);

    DmaBuffer buf = alloc_dma_buffer();
    TEST_ASSERT_NE(buf.phys, 0u);

    auto* data = reinterpret_cast<uint8_t*>(buf.virt);

    // Save original
    bool ok = ahci->read(0, 100, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    uint8_t original[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        original[i] = data[i];
    }

    // Write pattern: 0xAA, 0x55 alternating
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        data[i] = static_cast<uint8_t>((i & 1) ? 0x55 : 0xAA);
    }
    ok = ahci->write(0, 100, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Read back
    ok = ahci->read(0, 100, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Verify
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        uint8_t expected = static_cast<uint8_t>((i & 1) ? 0x55 : 0xAA);
        TEST_ASSERT_EQ(data[i], expected);
    }

    // Restore
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        data[i] = original[i];
    }
    ahci->write(0, 100, 1, buf.phys);

    cinux::lib::kprintf("[AHCI_WRITE] LBA 100 write-read roundtrip OK\n");

    free_dma_buffer(buf);
    teardown_ahci(ahci);
}

/**
 * @brief Write multiple sectors at once
 *
 * Write 2 sectors (LBA 200-201) to test multi-sector writes.
 */
void test_write_multiple_sectors() {
    AHCI* ahci = setup_ahci();
    TEST_ASSERT_NOT_NULL(ahci);

    DmaBuffer buf = alloc_dma_buffer();
    TEST_ASSERT_NE(buf.phys, 0u);

    auto* data = reinterpret_cast<uint8_t*>(buf.virt);

    // Save original (2 sectors)
    bool ok = ahci->read(0, 200, 2, buf.phys);
    TEST_ASSERT_TRUE(ok);

    uint8_t original[SECTOR_SIZE * 2];
    for (uint32_t i = 0; i < SECTOR_SIZE * 2; ++i) {
        original[i] = data[i];
    }

    // Write pattern to 2 sectors
    for (uint32_t i = 0; i < SECTOR_SIZE * 2; ++i) {
        data[i] = static_cast<uint8_t>((i >> 8) ^ (i & 0xFF));
    }
    ok = ahci->write(0, 200, 2, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Read back
    ok = ahci->read(0, 200, 2, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Verify
    for (uint32_t i = 0; i < SECTOR_SIZE * 2; ++i) {
        uint8_t expected = static_cast<uint8_t>((i >> 8) ^ (i & 0xFF));
        TEST_ASSERT_EQ(data[i], expected);
    }

    // Restore
    for (uint32_t i = 0; i < SECTOR_SIZE * 2; ++i) {
        data[i] = original[i];
    }
    ahci->write(0, 200, 2, buf.phys);

    cinux::lib::kprintf("[AHCI_WRITE] Multi-sector write-read OK\n");

    free_dma_buffer(buf);
    teardown_ahci(ahci);
}

/**
 * @brief Verify that overwriting a sector replaces previous content
 */
void test_write_overwrites_previous() {
    AHCI* ahci = setup_ahci();
    TEST_ASSERT_NOT_NULL(ahci);

    DmaBuffer buf = alloc_dma_buffer();
    TEST_ASSERT_NE(buf.phys, 0u);

    auto* data = reinterpret_cast<uint8_t*>(buf.virt);

    // Save original
    bool ok = ahci->read(0, 300, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    uint8_t original[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        original[i] = data[i];
    }

    // First write: all 0x11
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        data[i] = 0x11;
    }
    ok = ahci->write(0, 300, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Second write: all 0x22
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        data[i] = 0x22;
    }
    ok = ahci->write(0, 300, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    // Read back: should be 0x22, NOT 0x11
    ok = ahci->read(0, 300, 1, buf.phys);
    TEST_ASSERT_TRUE(ok);

    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        TEST_ASSERT_EQ(data[i], 0x22u);
    }

    // Restore
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        data[i] = original[i];
    }
    ahci->write(0, 300, 1, buf.phys);

    cinux::lib::kprintf("[AHCI_WRITE] Overwrite test OK\n");

    free_dma_buffer(buf);
    teardown_ahci(ahci);
}

}  // namespace test_ahci_write

// ============================================================
// Test 2: Ext2 write_block round-trip
// ============================================================

namespace test_ext2_write_block {

/**
 * @brief Helper: set up AHCI + Ext2 on port 1 (ext2 disk)
 */
struct Ext2Pair {
    AHCI* ahci;
    Ext2* ext2;
};

Ext2Pair setup_ext2() {
    Ext2Pair result{nullptr, nullptr};

    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return result;
    }

    result.ahci = new AHCI();
    result.ahci->init(ahci_dev);
    if (result.ahci->hba_mem() == nullptr) {
        return result;
    }

    // Port 1 is the ext2 test disk
    result.ext2 = new Ext2(*result.ahci, 1);
    result.ext2->mount();

    return result;
}

void teardown_ext2(Ext2Pair& pair) {
    delete pair.ext2;
    delete pair.ahci;
    pair.ext2 = nullptr;
    pair.ahci = nullptr;
}

/**
 * @brief Test ext2 write_block on a high-numbered block (safe to modify)
 *
 * Use a block number well beyond the filesystem metadata area.
 * For a small ext2 image (~4MB), block 1000 should be in the data area.
 *
 * Steps:
 *   1. read_block(1000) -- get original content
 *   2. Modify the DMA buffer with a test pattern
 *   3. write_block(1000) -- write modified data
 *   4. read_block(1000) -- read back
 *   5. Verify the test pattern
 *   6. Restore original content
 */
void test_ext2_write_block_roundtrip() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Use a block number in the data area (well past metadata)
    uint32_t test_block = 100;

    // Step 1: Read original block
    bool ok = pair.ext2->read_block(test_block);
    TEST_ASSERT_TRUE(ok);

    auto*    dma = reinterpret_cast<uint8_t*>(pair.ext2->dma_buf_virt());
    uint32_t bs  = pair.ext2->block_size();

    // Save original
    uint8_t* original = new uint8_t[bs];
    TEST_ASSERT_NOT_NULL(original);
    for (uint32_t i = 0; i < bs; ++i) {
        original[i] = dma[i];
    }

    // Step 2: Write test pattern
    for (uint32_t i = 0; i < bs; ++i) {
        dma[i] = static_cast<uint8_t>(i & 0xFF);
    }

    // Step 3: Write block
    ok = pair.ext2->write_block(test_block);
    TEST_ASSERT_TRUE(ok);

    // Step 4: Read back
    ok = pair.ext2->read_block(test_block);
    TEST_ASSERT_TRUE(ok);

    // Step 5: Verify
    for (uint32_t i = 0; i < bs; ++i) {
        TEST_ASSERT_EQ(dma[i], static_cast<uint8_t>(i & 0xFF));
    }

    // Step 6: Restore original
    for (uint32_t i = 0; i < bs; ++i) {
        dma[i] = original[i];
    }
    pair.ext2->write_block(test_block);

    cinux::lib::kprintf("[EXT2_WRITE] Block %u write-read roundtrip OK\n", test_block);

    delete[] original;
    teardown_ext2(pair);
}

/**
 * @brief Test ext2 write_block with block 0
 *
 * Block 0 is the first data block.  For bs=1024 this is the boot block.
 * For bs=4096 it contains the superblock at offset 1024.
 * We save/restore to avoid corrupting the filesystem.
 */
void test_ext2_write_block_zero() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Read block 0
    bool ok = pair.ext2->read_block(0);
    TEST_ASSERT_TRUE(ok);

    auto*    dma = reinterpret_cast<uint8_t*>(pair.ext2->dma_buf_virt());
    uint32_t bs  = pair.ext2->block_size();

    // Save original
    uint8_t* original = new uint8_t[bs];
    TEST_ASSERT_NOT_NULL(original);
    for (uint32_t i = 0; i < bs; ++i) {
        original[i] = dma[i];
    }

    // Modify a few bytes at the beginning (avoid superblock area)
    // For safety, only modify bytes 0-3 (before the superblock at offset 1024)
    for (int i = 0; i < 4; ++i) {
        dma[i] = static_cast<uint8_t>(0xDE - i);
    }

    ok = pair.ext2->write_block(0);
    TEST_ASSERT_TRUE(ok);

    // Read back and verify only the modified bytes
    ok = pair.ext2->read_block(0);
    TEST_ASSERT_TRUE(ok);

    TEST_ASSERT_EQ(dma[0], 0xDEu);
    TEST_ASSERT_EQ(dma[1], 0xDDu);
    TEST_ASSERT_EQ(dma[2], 0xDCu);
    TEST_ASSERT_EQ(dma[3], 0xDBu);

    // Restore
    for (uint32_t i = 0; i < bs; ++i) {
        dma[i] = original[i];
    }
    pair.ext2->write_block(0);

    cinux::lib::kprintf("[EXT2_WRITE] Block 0 write-read roundtrip OK\n");

    delete[] original;
    teardown_ext2(pair);
}

/**
 * @brief Test that write_block correctly calls AHCI::write (not read)
 *
 * Write a unique pattern, verify it persists through a write-then-read cycle.
 * Use a pattern that is unlikely to occur naturally.
 */
void test_ext2_write_block_persistence() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    uint32_t test_block = 200;

    // Read and save original
    bool ok = pair.ext2->read_block(test_block);
    TEST_ASSERT_TRUE(ok);

    auto*    dma = reinterpret_cast<uint8_t*>(pair.ext2->dma_buf_virt());
    uint32_t bs  = pair.ext2->block_size();

    uint8_t* original = new uint8_t[bs];
    TEST_ASSERT_NOT_NULL(original);
    for (uint32_t i = 0; i < bs; ++i) {
        original[i] = dma[i];
    }

    // Write a distinctive pattern
    const uint8_t pattern[] = {0xCA, 0xFE, 0xBA, 0xBE};
    for (uint32_t i = 0; i < bs; i += 4) {
        for (int j = 0; j < 4 && (i + j) < bs; ++j) {
            dma[i + j] = pattern[j];
        }
    }

    ok = pair.ext2->write_block(test_block);
    TEST_ASSERT_TRUE(ok);

    // Read a different block first to flush the DMA buffer
    ok = pair.ext2->read_block(test_block + 1);
    // This may or may not succeed depending on disk size; just proceed

    // Now read back the test block
    ok = pair.ext2->read_block(test_block);
    TEST_ASSERT_TRUE(ok);

    // Verify the pattern persisted
    for (uint32_t i = 0; i < bs; i += 4) {
        for (int j = 0; j < 4 && (i + j) < bs; ++j) {
            TEST_ASSERT_EQ(dma[i + j], pattern[j]);
        }
    }

    // Restore
    for (uint32_t i = 0; i < bs; ++i) {
        dma[i] = original[i];
    }
    pair.ext2->write_block(test_block);

    cinux::lib::kprintf("[EXT2_WRITE] Block persistence test OK\n");

    delete[] original;
    teardown_ext2(pair);
}

}  // namespace test_ext2_write_block

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ahci_write_tests() {
    TEST_SECTION("AHCI Write + Ext2 write_block Tests (028b)");

    // AHCI direct write tests (port 0 = test disk)
    RUN_TEST(test_ahci_write::test_write_sector0_roundtrip);
    RUN_TEST(test_ahci_write::test_write_higher_lba);
    RUN_TEST(test_ahci_write::test_write_multiple_sectors);
    RUN_TEST(test_ahci_write::test_write_overwrites_previous);

    // Ext2 write_block tests (port 1 = ext2 disk)
    RUN_TEST(test_ext2_write_block::test_ext2_write_block_roundtrip);
    RUN_TEST(test_ext2_write_block::test_ext2_write_block_zero);
    RUN_TEST(test_ext2_write_block::test_ext2_write_block_persistence);

    TEST_SUMMARY();
}
