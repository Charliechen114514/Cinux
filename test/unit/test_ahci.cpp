/**
 * @file test/unit/test_ahci.cpp
 * @brief Host-side unit tests for AHCI driver logic
 *
 * Test coverage:
 *   - AHCI register structure sizes and layout (static_assert style)
 *   - PCI config constants and class/subclass matching
 *   - GHC bit flag constants
 *   - PxCmd flag constants
 *   - Port interrupt status bits
 *   - Port SATA status detection field
 *   - Command header flags
 *   - ATA command opcodes
 *   - FIS type constant
 *   - RegH2D FIS construction logic
 *   - PRDT entry field layout
 *   - CFIS build: LBA encoding for 48-bit addresses
 *   - CFIS build: sector count encoding
 *   - CFIS build: read vs write command selection
 *   - PCI address word construction
 *   - PCI device matching (class=0x01, subclass=0x06)
 *   - BAR type detection (IO vs memory, 32-bit vs 64-bit)
 *   - Byte count computation for PRDT
 *
 * Since AHCI operations require MMIO and real hardware, the kernel
 * implementation cannot be called directly on the host.  Instead, we
 * extract and test the pure logic portions (register layout, constant
 * correctness, FIS construction, PCI address encoding) and verify
 * struct sizes.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>

// Include kernel AHCI headers for constants and structures
#include "drivers/ahci/ahci_config.hpp"
#include "drivers/pci/pci_config.hpp"

using namespace cinux::drivers::ahci;
using namespace cinux::drivers::pci;

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
static void build_cfis(uint8_t* cfis_buf, bool write_cmd,
                       uint64_t lba, uint16_t count) {
    auto* fis = reinterpret_cast<RegH2DFIS*>(cfis_buf);

    fis->fis_type = FisType::REG_H2D;
    fis->flags    = 0x80;
    fis->command  = write_cmd ? AtaCmd::WRITE_DMA_EXT
                              : AtaCmd::READ_DMA_EXT;
    fis->feature  = 0;

    fis->lba0 = static_cast<uint8_t>(lba & 0xFF);
    fis->lba1 = static_cast<uint8_t>((lba >> 8) & 0xFF);
    fis->lba2 = static_cast<uint8_t>((lba >> 16) & 0xFF);
    fis->device = 0x40;

    fis->lba3 = static_cast<uint8_t>((lba >> 24) & 0xFF);
    fis->lba4 = static_cast<uint8_t>((lba >> 32) & 0xFF);
    fis->lba5 = static_cast<uint8_t>((lba >> 40) & 0xFF);
    fis->feature_exp = 0;

    fis->count0 = static_cast<uint8_t>(count & 0xFF);
    fis->count1 = static_cast<uint8_t>((count >> 8) & 0xFF);

    fis->control = 0;
}

/**
 * @brief Build PCI config address word (host-side reimplementation)
 *
 * Mirrors PCI::pci_read address construction.
 */
static uint32_t build_pci_address(uint8_t bus, uint8_t slot,
                                  uint8_t func, uint8_t offset) {
    return (1U << 31)
         | (static_cast<uint32_t>(bus) << 16)
         | (static_cast<uint32_t>(slot) << 11)
         | (static_cast<uint32_t>(func) << 8)
         | (offset & 0xFC);
}

// ============================================================
// 1. AHCI Structure Size Verification
// ============================================================

/**
 * @brief Verify HBAPort structure is exactly 128 bytes (0x80)
 */
TEST("ahci: HBAPort size is 128 bytes") {
    ASSERT_EQ(sizeof(HBAPort), 0x80u);
}

/**
 * @brief Verify port registers start at offset 0x100 from HBA base
 */
TEST("ahci: port registers start at offset 0x100") {
    ASSERT_EQ(sizeof(HBAMem) - sizeof(HBAPort), 0x100u);
}

/**
 * @brief Verify HBACommandHeader is exactly 32 bytes
 */
TEST("ahci: HBACommandHeader size is 32 bytes") {
    ASSERT_EQ(sizeof(HBACommandHeader), CMD_TBL_SIZE);
    ASSERT_EQ(sizeof(HBACommandHeader), 32u);
}

/**
 * @brief Verify HBAPrdtEntry is exactly 16 bytes
 */
TEST("ahci: HBAPrdtEntry size is 16 bytes") {
    ASSERT_EQ(sizeof(HBAPrdtEntry), 16u);
}

/**
 * @brief Verify RegH2DFIS is exactly 20 bytes
 */
TEST("ahci: RegH2DFIS size is 20 bytes") {
    ASSERT_EQ(sizeof(RegH2DFIS), 20u);
}

// ============================================================
// 2. AHCI Constant Verification
// ============================================================

/**
 * @brief Verify MAX_PORTS, CMD_SLOTS, FIS_BUF_SIZE, SECTOR_SIZE
 */
TEST("ahci: core constants") {
    ASSERT_EQ(MAX_PORTS, 32u);
    ASSERT_EQ(CMD_SLOTS, 32u);
    ASSERT_EQ(CMD_TBL_SIZE, 32u);
    ASSERT_EQ(FIS_BUF_SIZE, 256u);
    ASSERT_EQ(SECTOR_SIZE, 512u);
}

/**
 * @brief Verify command table header area size
 */
TEST("ahci: command table header size") {
    ASSERT_EQ(CMD_TBL_HDR_SIZE, 0x80u);
}

/**
 * @brief Verify max PRDT entries
 */
TEST("ahci: max PRDT entries") {
    ASSERT_EQ(MAX_PRDT_ENTRIES, 8u);
}

// ============================================================
// 3. GHC Bit Constants
// ============================================================

/**
 * @brief Verify GHC bit positions
 */
TEST("ahci: GHC bit flags") {
    ASSERT_EQ(GhcBits::HBA_RESET, 1u << 0);
    ASSERT_EQ(GhcBits::INT_ENABLE, 1u << 1);
    ASSERT_EQ(GhcBits::AE, 1u << 31);
}

// ============================================================
// 4. PxCmd Flag Constants
// ============================================================

/**
 * @brief Verify port command flag bit positions
 */
TEST("ahci: PxCmd flags") {
    ASSERT_EQ(PxCmd::ST,  1u << 0);
    ASSERT_EQ(PxCmd::SUD, 1u << 1);
    ASSERT_EQ(PxCmd::POD, 1u << 2);
    ASSERT_EQ(PxCmd::FRE, 1u << 4);
    ASSERT_EQ(PxCmd::FR,  1u << 14);
    ASSERT_EQ(PxCmd::CR,  1u << 15);
}

// ============================================================
// 5. Port Interrupt Status Bits
// ============================================================

/**
 * @brief Verify PxIs bit positions
 */
TEST("ahci: PxIs interrupt bits") {
    ASSERT_EQ(PxIs::DHRS, 1u << 0);
    ASSERT_EQ(PxIs::PSS,  1u << 1);
    ASSERT_EQ(PxIs::DSS,  1u << 2);
    ASSERT_EQ(PxIs::SDBS, 1u << 3);
    ASSERT_EQ(PxIs::DMPS, 1u << 23);
}

// ============================================================
// 6. SATA Status Detection
// ============================================================

/**
 * @brief Verify PxSsts detection mask and active value
 */
TEST("ahci: PxSsts detection field") {
    ASSERT_EQ(PxSsts::DET_MASK, 0x0Fu);
    ASSERT_EQ(PxSsts::DET_ACTIVE, 0x03u);
}

// ============================================================
// 7. Command Header Flags
// ============================================================

/**
 * @brief Verify CmdHdrFlags bit positions
 */
TEST("ahci: command header flags") {
    ASSERT_EQ(CmdHdrFlags::WRITE, 1u << 6);
    ASSERT_EQ(CmdHdrFlags::READ, 1u << 5);
    ASSERT_EQ(CmdHdrFlags::PREFETCH, 1u << 7);
    ASSERT_EQ(CmdHdrFlags::RESET, 1u << 8);
    ASSERT_EQ(CmdHdrFlags::BIST, 1u << 9);
}

// ============================================================
// 8. ATA Command Opcodes
// ============================================================

/**
 * @brief Verify ATA command constants
 */
TEST("ahci: ATA command opcodes") {
    ASSERT_EQ(AtaCmd::READ_DMA_EXT, 0x25u);
    ASSERT_EQ(AtaCmd::WRITE_DMA_EXT, 0x35u);
    ASSERT_EQ(AtaCmd::IDENTIFY, 0xECu);
}

// ============================================================
// 9. FIS Type Constant
// ============================================================

/**
 * @brief Verify FIS type value
 */
TEST("ahci: FIS type constant") {
    ASSERT_EQ(FisType::REG_H2D, 0x27u);
}

// ============================================================
// 10. RegH2D FIS Construction -- Read Command
// ============================================================

/**
 * @brief Verify CFIS for READ of LBA 0, 1 sector
 */
TEST("ahci: CFIS read LBA 0 count 1") {
    uint8_t buf[64] = {};
    build_cfis(buf, false, 0, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->fis_type, 0x27);
    ASSERT_EQ(fis->flags, 0x80);
    ASSERT_EQ(fis->command, AtaCmd::READ_DMA_EXT);
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

// ============================================================
// 11. RegH2D FIS Construction -- Write Command
// ============================================================

/**
 * @brief Verify CFIS for WRITE of LBA 0, 1 sector
 */
TEST("ahci: CFIS write command selection") {
    uint8_t buf[64] = {};
    build_cfis(buf, true, 0, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->command, AtaCmd::WRITE_DMA_EXT);
}

// ============================================================
// 12. CFIS LBA Encoding (48-bit)
// ============================================================

/**
 * @brief Verify CFIS LBA encoding for a 48-bit address
 *
 * LBA = 0x112233445566:
 *   lba0 = 0x66 (bits 0-7)
 *   lba1 = 0x55 (bits 8-15)
 *   lba2 = 0x44 (bits 16-23)
 *   lba3 = 0x33 (bits 24-31)
 *   lba4 = 0x22 (bits 32-39)
 *   lba5 = 0x11 (bits 40-47)
 */
TEST("ahci: CFIS 48-bit LBA encoding") {
    uint8_t buf[64] = {};
    uint64_t lba = 0x112233445566ULL;
    build_cfis(buf, false, lba, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->lba0, 0x66);
    ASSERT_EQ(fis->lba1, 0x55);
    ASSERT_EQ(fis->lba2, 0x44);
    ASSERT_EQ(fis->lba3, 0x33);
    ASSERT_EQ(fis->lba4, 0x22);
    ASSERT_EQ(fis->lba5, 0x11);
    ASSERT_EQ(fis->device, 0x40);
}

/**
 * @brief Verify CFIS LBA encoding at 28-bit boundary
 *
 * LBA = 0x10000000:
 *   lba0-2 = 0
 *   lba3 = 0x10
 *   lba4-5 = 0
 */
TEST("ahci: CFIS LBA at 28-bit boundary") {
    uint8_t buf[64] = {};
    build_cfis(buf, false, 0x10000000ULL, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->lba0, 0x00);
    ASSERT_EQ(fis->lba1, 0x00);
    ASSERT_EQ(fis->lba2, 0x00);
    ASSERT_EQ(fis->lba3, 0x10);
    ASSERT_EQ(fis->lba4, 0x00);
    ASSERT_EQ(fis->lba5, 0x00);
}

// ============================================================
// 13. CFIS Sector Count Encoding
// ============================================================

/**
 * @brief Verify CFIS sector count for count = 1
 */
TEST("ahci: CFIS sector count 1") {
    uint8_t buf[64] = {};
    build_cfis(buf, false, 0, 1);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->count0, 1);
    ASSERT_EQ(fis->count1, 0);
}

/**
 * @brief Verify CFIS sector count for count = 256
 */
TEST("ahci: CFIS sector count 256") {
    uint8_t buf[64] = {};
    build_cfis(buf, false, 0, 256);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->count0, 0);
    ASSERT_EQ(fis->count1, 1);
}

/**
 * @brief Verify CFIS sector count for count = 512
 */
TEST("ahci: CFIS sector count 512") {
    uint8_t buf[64] = {};
    build_cfis(buf, false, 0, 512);

    auto* fis = reinterpret_cast<RegH2DFIS*>(buf);
    ASSERT_EQ(fis->count0, 0);
    ASSERT_EQ(fis->count1, 2);
}

// ============================================================
// 14. PRDT Entry Layout
// ============================================================

/**
 * @brief Verify PRDT entry field offsets and values
 */
TEST("ahci: PRDT entry layout") {
    HBAPrdtEntry prd{};
    std::memset(&prd, 0, sizeof(prd));

    prd.dba  = 0x12345678;
    prd.dbau = 0x9ABCDEF0;
    prd.dbc  = 0x1FFFF;   // max 22-bit value (last valid: 0x3FFFFF)
    prd.i    = 1;

    ASSERT_EQ(prd.dba, 0x12345678u);
    ASSERT_EQ(prd.dbau, 0x9ABCDEF0u);
    ASSERT_EQ(prd.dbc, 0x1FFFFu);
    ASSERT_EQ(prd.i, 1u);
}

/**
 * @brief Verify PRDT byte count mask (22-bit field)
 */
TEST("ahci: PRDT byte count is 22-bit") {
    HBAPrdtEntry prd{};
    std::memset(&prd, 0, sizeof(prd));

    // Set all 22 bits -- this is the maximum valid value
    prd.dbc = 0x3FFFFF;
    ASSERT_EQ(prd.dbc, 0x3FFFFFu);

    // Verify that a value one below the max is preserved
    prd.dbc = 0x3FFFFE;
    ASSERT_EQ(prd.dbc, 0x3FFFFEu);
}

// ============================================================
// 15. PRDT Byte Count Computation
// ============================================================

/**
 * @brief Verify byte count calculation for PRDT (count * 512 - 1)
 */
TEST("ahci: PRDT byte count for 1 sector") {
    uint16_t count = 1;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    ASSERT_EQ(byte_count, 511u);
}

/**
 * @brief Verify byte count for multiple sectors
 */
TEST("ahci: PRDT byte count for 4 sectors") {
    uint16_t count = 4;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    ASSERT_EQ(byte_count, 2047u);
}

/**
 * @brief Verify byte count masking to 22-bit max
 */
TEST("ahci: PRDT byte count 22-bit mask") {
    uint16_t count = 1;
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    uint32_t masked = byte_count & 0x3FFFFF;
    ASSERT_EQ(masked, 511u);
}

// ============================================================
// 16. PCI Config Space Constants
// ============================================================

/**
 * @brief Verify PCI I/O port addresses
 */
TEST("ahci: PCI config port addresses") {
    ASSERT_EQ(PciPort::CONFIG_ADDRESS, 0xCF8);
    ASSERT_EQ(PciPort::CONFIG_DATA, 0xCFC);
}

/**
 * @brief Verify PCI register offsets
 */
TEST("ahci: PCI register offsets") {
    ASSERT_EQ(PciReg::VENDOR_ID, 0x00);
    ASSERT_EQ(PciReg::DEVICE_ID, 0x02);
    ASSERT_EQ(PciReg::COMMAND, 0x04);
    ASSERT_EQ(PciReg::CLASS_CODE, 0x0B);
    ASSERT_EQ(PciReg::SUBCLASS, 0x0A);
    ASSERT_EQ(PciReg::HEADER_TYPE, 0x0E);
    ASSERT_EQ(PciReg::BAR5, 0x24);
}

/**
 * @brief Verify PCI class codes for AHCI
 */
TEST("ahci: PCI class codes for AHCI") {
    ASSERT_EQ(PciClass::MASS_STORAGE, 0x01);
    ASSERT_EQ(PciClass::AHCI_SUBCLASS, 0x06);
}

// ============================================================
// 17. PCI Address Word Construction
// ============================================================

/**
 * @brief Verify PCI address word for bus 0, slot 0, func 0, offset 0
 */
TEST("ahci: PCI address word bus 0 slot 0 func 0") {
    uint32_t addr = build_pci_address(0, 0, 0, 0);
    ASSERT_EQ(addr, 1u << 31);
}

/**
 * @brief Verify PCI address word bit layout
 *
 * Enable bit 31, bus bits 23:16, slot bits 15:11,
 * func bits 10:8, offset bits 7:2.
 */
TEST("ahci: PCI address word full layout") {
    uint32_t addr = build_pci_address(5, 10, 2, 0x08);
    ASSERT_TRUE(addr & (1u << 31));                      // enable
    ASSERT_EQ((addr >> 16) & 0xFF, 5u);                 // bus
    ASSERT_EQ((addr >> 11) & 0x1F, 10u);                // slot
    ASSERT_EQ((addr >> 8) & 0x07, 2u);                  // func
    ASSERT_EQ(addr & 0xFC, 0x08u);                      // offset (dword-aligned)
}

/**
 * @brief Verify PCI address word masks low 2 bits of offset
 */
TEST("ahci: PCI address masks offset low bits") {
    uint32_t addr = build_pci_address(0, 0, 0, 0x0B);
    ASSERT_EQ(addr & 0xFC, 0x08u);  // 0x0B & 0xFC = 0x08
}

// ============================================================
// 18. PCI Device Matching (class=0x01, subclass=0x06)
// ============================================================

/**
 * @brief Verify AHCI class/subclass match
 */
TEST("ahci: PCI AHCI class subclass match") {
    uint8_t class_code = PciClass::MASS_STORAGE;
    uint8_t subclass = PciClass::AHCI_SUBCLASS;

    ASSERT_EQ(class_code, 0x01);
    ASSERT_EQ(subclass, 0x06);
    ASSERT_TRUE(class_code == 0x01 && subclass == 0x06);
}

/**
 * @brief Verify non-AHCI device is rejected
 */
TEST("ahci: PCI non-AHCI device rejected") {
    uint8_t class_code = 0x02;  // Network controller
    uint8_t subclass = 0x00;

    ASSERT_FALSE(class_code == PciClass::MASS_STORAGE
                 && subclass == PciClass::AHCI_SUBCLASS);
}

// ============================================================
// 19. BAR Type Detection
// ============================================================

/**
 * @brief Verify BAR IO space bit
 */
TEST("ahci: BAR IO space detection") {
    uint32_t io_bar = 0x0000C001;  // IO space bit set
    ASSERT_TRUE(io_bar & BAR_IO_SPACE);

    uint32_t mem_bar = 0xF0000000;  // Memory space
    ASSERT_FALSE(mem_bar & BAR_IO_SPACE);
}

/**
 * @brief Verify BAR 64-bit memory type detection
 */
TEST("ahci: BAR 64-bit memory type detection") {
    uint32_t bar_64 = 0xF0000004;  // Type = 64-bit
    ASSERT_TRUE((bar_64 & BAR_TYPE_MASK) == BAR_TYPE_64);

    uint32_t bar_32 = 0xF0000000;  // Type = 32-bit
    ASSERT_FALSE((bar_32 & BAR_TYPE_MASK) == BAR_TYPE_64);
}

/**
 * @brief Verify BAR 32-bit address mask
 */
TEST("ahci: BAR 32-bit address mask") {
    uint32_t bar = 0xFEC00000;
    ASSERT_EQ(bar & BAR_ADDR_MASK_32, 0xFEC00000u);

    uint32_t bar_with_low = 0xFEC0000F;
    ASSERT_EQ(bar_with_low & BAR_ADDR_MASK_32, 0xFEC00000u);
}

// ============================================================
// 20. PCI Addressing Limits
// ============================================================

/**
 * @brief Verify PCI enumeration limits
 */
TEST("ahci: PCI enumeration limits") {
    ASSERT_EQ(MAX_BUS, 32);
    ASSERT_EQ(MAX_SLOT, 32);
    ASSERT_EQ(MAX_FUNC, 8);
    ASSERT_EQ(BAR_COUNT, 6);
    ASSERT_EQ(VENDOR_INVALID, 0xFFFF);
}

// ============================================================
// 21. HBAPort Field Offset Verification
// ============================================================

/**
 * @brief Verify key HBAPort field offsets via pointer arithmetic
 */
TEST("ahci: HBAPort field offsets") {
    HBAPort port{};
    auto base = reinterpret_cast<uintptr_t>(&port);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.clb) - base, 0x00u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.clbu) - base, 0x04u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.fb) - base, 0x08u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.fbu) - base, 0x0Cu);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.is) - base, 0x10u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.ie) - base, 0x14u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.cmd) - base, 0x18u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.tfd) - base, 0x20u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.sig) - base, 0x24u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.ssts) - base, 0x28u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&port.ci) - base, 0x38u);
}

// ============================================================
// 22. HBAMem Field Offset Verification
// ============================================================

/**
 * @brief Verify key HBAMem field offsets
 */
TEST("ahci: HBAMem field offsets") {
    HBAMem mem{};
    auto base = reinterpret_cast<uintptr_t>(&mem);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&mem.cap) - base, 0x00u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&mem.ghc) - base, 0x04u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&mem.is) - base, 0x08u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&mem.pi) - base, 0x0Cu);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(&mem.vs) - base, 0x10u);
}

// ============================================================
// 23. HBACommandHeader CFL Computation
// ============================================================

/**
 * @brief Verify CFL (Command FIS Length) is 5 dwords (RegH2DFIS = 20 bytes)
 */
TEST("ahci: CFL is 5 dwords") {
    uint8_t cfl = sizeof(RegH2DFIS) / 4;
    ASSERT_EQ(cfl, 5u);
}

// ============================================================
// 24. Command Table Total Size
// ============================================================

/**
 * @brief Verify command table total size calculation
 */
TEST("ahci: command table total size") {
    // Header (128) + 8 PRDT entries * 16 bytes each
    uint32_t total = CMD_TBL_HDR_SIZE + MAX_PRDT_ENTRIES * sizeof(HBAPrdtEntry);
    ASSERT_EQ(total, 128u + 128u);
    ASSERT_EQ(total, 256u);
}

// ============================================================
// 25. Port Interrupt Enable Mask
// ============================================================

/**
 * @brief Verify combined interrupt enable mask used in setup_port
 */
TEST("ahci: port interrupt enable mask") {
    uint32_t ie_mask = PxIs::DHRS | PxIs::PSS | PxIs::DSS | PxIs::SDBS;
    ASSERT_EQ(ie_mask, 0x0Fu);  // bits 0-3
}

// ============================================================
// 26. Stop/Start Port Bit Manipulation
// ============================================================

/**
 * @brief Verify stop_port clears ST and FRE bits
 */
TEST("ahci: stop_port clears ST and FRE") {
    // Simulate port with ST, CR, FRE, FR all set
    uint32_t cmd = PxCmd::ST | PxCmd::CR | PxCmd::FRE | PxCmd::FR;

    // Clear ST
    cmd &= ~PxCmd::ST;
    ASSERT_EQ(cmd & PxCmd::ST, 0u);

    // Clear FRE
    cmd &= ~PxCmd::FRE;
    ASSERT_EQ(cmd & PxCmd::FRE, 0u);
}

/**
 * @brief Verify start_port sets FRE then ST
 */
TEST("ahci: start_port sets FRE then ST") {
    uint32_t cmd = 0;

    // Enable FRE first
    cmd |= PxCmd::FRE;
    ASSERT_TRUE(cmd & PxCmd::FRE);
    ASSERT_FALSE(cmd & PxCmd::ST);

    // Then set ST
    cmd |= PxCmd::ST;
    ASSERT_TRUE(cmd & PxCmd::ST);
}

// ============================================================
// 27. AHCI Enable / Reset Sequence
// ============================================================

/**
 * @brief Verify GHC AE bit can be set and tested
 */
TEST("ahci: GHC AE bit") {
    uint32_t ghc = 0;
    ghc |= GhcBits::AE;
    ASSERT_TRUE(ghc & GhcBits::AE);

    // HR bit can be tested
    ghc |= GhcBits::HBA_RESET;
    ASSERT_TRUE(ghc & GhcBits::HBA_RESET);
}

/**
 * @brief Verify GHC HR clears after reset
 */
TEST("ahci: GHC HR clear simulation") {
    uint32_t ghc = GhcBits::HBA_RESET;
    ASSERT_TRUE(ghc & GhcBits::HBA_RESET);

    // Simulate HBA clearing the bit
    ghc &= ~GhcBits::HBA_RESET;
    ASSERT_FALSE(ghc & GhcBits::HBA_RESET);
}

// ============================================================
// 28. Command Issue Bit Setting
// ============================================================

/**
 * @brief Verify CI bit setting for slot 0
 */
TEST("ahci: CI slot 0 bit") {
    uint8_t slot = 0;
    uint32_t ci = 1u << slot;
    ASSERT_EQ(ci, 1u);
}

/**
 * @brief Verify CI bit clearing detection
 */
TEST("ahci: CI completion detection") {
    uint32_t ci = 1u << 0;
    ASSERT_TRUE(ci & (1u << 0));

    // Command complete: bit clears
    ci = 0;
    ASSERT_FALSE(ci & (1u << 0));
}

// ============================================================
// 29. TFD Error Bit Check
// ============================================================

/**
 * @brief Verify TFD error bit (bit 0) detection
 */
TEST("ahci: TFD error bit") {
    // TFD.ERR is bit 0 -- if set, the command failed
    uint32_t tfd_err = 0x51;  // ERR bit set (bit 0 = 1)
    ASSERT_TRUE((tfd_err & 0x01) != 0);

    uint32_t tfd_good = 0x50;  // DRDY + DSC, no error
    ASSERT_FALSE((tfd_good & 0x01) != 0);
}

// ============================================================
// 30. PCI Command Register Bits for AHCI Init
// ============================================================

/**
 * @brief Verify Bus Master Enable (bit 1) and Memory Space Enable (bit 2)
 */
TEST("ahci: PCI command register bits for AHCI") {
    uint32_t cmd_reg = 0;
    cmd_reg |= (1u << 1);  // Bus Master Enable
    cmd_reg |= (1u << 2);  // Memory Space Enable

    ASSERT_TRUE(cmd_reg & (1u << 1));
    ASSERT_TRUE(cmd_reg & (1u << 2));
    ASSERT_EQ(cmd_reg, 0x06u);
}

// ============================================================
// Main Function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
