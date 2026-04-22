/**
 * @file kernel/test/test_ahci.cpp
 * @brief QEMU in-kernel integration tests for the AHCI driver (025)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that:
 *   - PCI enumeration finds an AHCI controller (class=0x01, subclass=0x06)
 *   - AHCI::init() successfully maps BAR5 and probes ports
 *   - Sector 0 can be read and contains a valid MBR signature (0x55, 0xAA)
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised (g_pmm.init called)
 *   - VMM initialised (g_vmm.init called)
 */

#include <stdint.h>
#include <stddef.h>

#include "big_kernel_test.h"

#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::ahci::AHCI;
using cinux::drivers::ahci::SECTOR_SIZE;
using cinux::mm::g_pmm;
using cinux::mm::g_vmm;

// ============================================================
// Test 1: PCI enumeration finds an AHCI controller
// ============================================================

namespace test_ahci_pci {

void test_find_ahci() {
    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    bool found = pci.find_ahci(ahci_dev);

    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQ(ahci_dev.class_code, 0x01u);
    TEST_ASSERT_EQ(ahci_dev.subclass, 0x06u);
    TEST_ASSERT_NE(ahci_dev.bar[5], 0u);
}

}  // namespace test_ahci_pci

// ============================================================
// Test 2: AHCI init maps BAR5 and reports non-null hba_mem
// ============================================================

namespace test_ahci_init {

void test_ahci_init_maps_bar5() {
    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    bool found = pci.find_ahci(ahci_dev);
    TEST_ASSERT_TRUE(found);

    AHCI ahci;
    ahci.init(ahci_dev);

    TEST_ASSERT_NOT_NULL(ahci.hba_mem());
}

}  // namespace test_ahci_init

// ============================================================
// Test 3: Read sector 0 and verify MBR boot signature
// ============================================================

namespace test_ahci_read {

void test_read_sector0_mbr_signature() {
    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    bool found = pci.find_ahci(ahci_dev);
    TEST_ASSERT_TRUE(found);

    AHCI ahci;
    ahci.init(ahci_dev);
    TEST_ASSERT_NOT_NULL(ahci.hba_mem());

    // Allocate a physically-contiguous page for the DMA read buffer
    uint64_t buf_phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(buf_phys, 0u);

    // Map the buffer into kernel virtual space so we can inspect it
    constexpr uint64_t BUF_VIRT = 0xFFFFFFFF80200000ULL;
    constexpr uint64_t map_flags = 0x03;  // present + writable
    bool mapped = g_vmm.map(BUF_VIRT, buf_phys, map_flags);
    TEST_ASSERT_TRUE(mapped);

    // Zero the buffer before reading
    auto* buf = reinterpret_cast<uint8_t*>(BUF_VIRT);
    for (uint32_t i = 0; i < SECTOR_SIZE; ++i) {
        buf[i] = 0;
    }

    // Read sector 0 (LBA 0, 1 sector) from port 0
    bool ok = ahci.read(0, 0, 1, buf_phys);
    TEST_ASSERT_TRUE(ok);

    // Verify MBR signature at offset 510-511
    uint8_t sig0 = buf[510];
    uint8_t sig1 = buf[511];

    cinux::lib::kprintf("[AHCI] Read sector 0: %02X %02X\n", sig0, sig1);

    TEST_ASSERT_EQ(sig0, 0x55u);
    TEST_ASSERT_EQ(sig1, 0xAAu);

    // Clean up
    g_vmm.unmap(BUF_VIRT);
    g_pmm.free_page(buf_phys);
}

}  // namespace test_ahci_read

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ahci_tests() {
    TEST_SECTION("AHCI Tests (025)");

    RUN_TEST(test_ahci_pci::test_find_ahci);
    RUN_TEST(test_ahci_init::test_ahci_init_maps_bar5);
    RUN_TEST(test_ahci_read::test_read_sector0_mbr_signature);

    TEST_SUMMARY();
}
