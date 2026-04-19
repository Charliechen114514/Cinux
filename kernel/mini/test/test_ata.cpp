/**
 * @file kernel/mini/test/test_ata.cpp
 * @brief QEMU in-kernel integration tests for ATA PIO driver
 *
 * Runs inside QEMU, verifying that ATA PIO reads work correctly
 * against the virtual hard disk.
 *
 * Test coverage:
 *   - ATA driver initialization succeeds
 *   - MBR sector (LBA 0) contains valid boot signature 0xAA55
 *   - Reading multiple consecutive sectors succeeds
 *   - Read data is non-trivial (not all zeros)
 */

#include "kernel_test.h"
#include "../driver/ata.hpp"
#include "../lib/string.h"

using namespace cinux::mini::driver::ata;

// ============================================================
// Shared read buffer (one sector)
// ============================================================
static uint8_t g_sector_buf[ATA_SECTOR_SIZE] __attribute__((aligned(16)));

// ============================================================
// Test 1: ATA initialization
// ============================================================
namespace test_ata_init {

    void test_init_no_crash() {
        // init() performs software reset and drive selection.
        // In QEMU this should succeed without hanging.
        init();
        // If we reach here, init completed without triple fault.
    }
}

// ============================================================
// Test 2: Read MBR and verify boot signature
// ============================================================
namespace test_ata_mbr {

    void test_read_mbr_signature() {
        memset(g_sector_buf, 0, ATA_SECTOR_SIZE);
        read(0, 1, g_sector_buf);

        // MBR boot signature is at offset 510-511
        uint8_t sig_lo = g_sector_buf[510];
        uint8_t sig_hi = g_sector_buf[511];
        uint16_t signature = static_cast<uint16_t>(sig_lo) |
                             (static_cast<uint16_t>(sig_hi) << 8);

        kprintf("  MBR signature: 0x%x\n", signature);
        TEST_ASSERT_EQ(signature, 0xAA55);
    }
}

// ============================================================
// Test 3: Read data is non-trivial
// ============================================================
namespace test_ata_data {

    void test_read_non_zero() {
        memset(g_sector_buf, 0, ATA_SECTOR_SIZE);

        // Read stage2 area (LBA 1)
        read(1, 1, g_sector_buf);

        // Verify at least some bytes are non-zero
        bool has_non_zero = false;
        for (int i = 0; i < ATA_SECTOR_SIZE; i++) {
            if (g_sector_buf[i] != 0) {
                has_non_zero = true;
                break;
            }
        }
        TEST_ASSERT_TRUE(has_non_zero);
    }
}

// ============================================================
// Test 4: Read multiple sectors
// ============================================================
namespace test_ata_multi {

    void test_read_multi_sector() {
        memset(g_sector_buf, 0, ATA_SECTOR_SIZE);

        // Read 4 sectors starting from mini kernel LBA (16)
        read(16, 4, g_sector_buf);

        // First few bytes should contain kernel code (non-zero)
        bool has_non_zero = false;
        for (int i = 0; i < 64; i++) {
            if (g_sector_buf[i] != 0) {
                has_non_zero = true;
                break;
            }
        }
        TEST_ASSERT_TRUE(has_non_zero);
    }
}

// ============================================================
// Test 5: DMA availability
// ============================================================
namespace test_ata_dma {

    void test_dma_available() {
        // QEMU's PIIX4 IDE controller supports Bus Master DMA.
        // Using static PRDT buffer so DMA works regardless of PMM state.
        TEST_ASSERT_TRUE(is_dma_available());
    }
}

// ============================================================
// Test Entry Point
// ============================================================
extern "C" void run_ata_tests() {
    TEST_SECTION("ATA PIO Driver Tests (008)");

    RUN_TEST(test_ata_init::test_init_no_crash);
    RUN_TEST(test_ata_mbr::test_read_mbr_signature);
    RUN_TEST(test_ata_data::test_read_non_zero);
    RUN_TEST(test_ata_multi::test_read_multi_sector);
    RUN_TEST(test_ata_dma::test_dma_available);

    TEST_SUMMARY();
}
