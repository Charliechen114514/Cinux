/**
 * @file kernel/drivers/ahci/ahci_config.hpp
 * @brief AHCI hardware register definitions and constants
 *
 * Contains all AHCI HBA memory-mapped register structures
 * (HBAMem for the controller, HBAPort for each port) and
 * related constants for command framing and port control.
 *
 * Reference: Intel AHCI Specification rev 1.3
 *
 * Namespace: cinux::drivers::ahci
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers::ahci {

// ============================================================
// AHCI Hardware Constants
// ============================================================

/// Maximum number of ports reported by the HBA
constexpr uint32_t MAX_PORTS = 32;

/// Number of command slots per port (command list length)
constexpr uint32_t CMD_SLOTS = 32;

/// Command list entry size in bytes
constexpr uint32_t CMD_TBL_SIZE = 32;

/// Size of the command table (header + PRDT area)
constexpr uint32_t CMD_TBL_HDR_SIZE = 0x80;

/// Received FIS buffer size in bytes
constexpr uint32_t FIS_BUF_SIZE = 256;

/// Maximum number of Physical Region Descriptor Table entries
constexpr uint32_t MAX_PRDT_ENTRIES = 8;

/// Size of a disk sector in bytes
constexpr uint32_t SECTOR_SIZE = 512;

// ============================================================
// HBA Port Register Offsets (relative to port base)
// ============================================================

namespace PxCmd {
constexpr uint32_t ST       = (1U << 0);     ///< Start
constexpr uint32_t SUD      = (1U << 1);     ///< Spin-Up Device
constexpr uint32_t POD      = (1U << 2);     ///< Power On Device
constexpr uint32_t FRE      = (1U << 4);     ///< FIS Receive Enable
constexpr uint32_t FR       = (1U << 14);    ///< FIS Receive Running
constexpr uint32_t CR       = (1U << 15);    ///< Command List Running
constexpr uint32_t PMA      = (1U << 17);    ///< Port Multiplier Attached
constexpr uint32_t HPCP     = (1U << 18);    ///< Hot Plug Capable Port
constexpr uint32_t ICC_MASK = (0xFU << 28);  ///< Interface Communication Control
}  // namespace PxCmd

// ============================================================
// GHC (Global HBA Control) Bits
// ============================================================

namespace GhcBits {
constexpr uint32_t HBA_RESET  = (1U << 0);   ///< HBA Reset
constexpr uint32_t INT_ENABLE = (1U << 1);   ///< Interrupt Enable
constexpr uint32_t AE         = (1U << 31);  ///< AHCI Enable
}  // namespace GhcBits

// ============================================================
// ATA Command Opcodes
// ============================================================

namespace AtaCmd {
constexpr uint8_t READ_DMA_EXT  = 0x25;  ///< Read DMA with FPDMA (NCQ) or DMA
constexpr uint8_t WRITE_DMA_EXT = 0x35;  ///< Write DMA
constexpr uint8_t IDENTIFY      = 0xEC;  ///< Identify Device
}  // namespace AtaCmd

// ============================================================
// FIS Type Constants
// ============================================================

namespace FisType {
constexpr uint8_t REG_H2D = 0x27;  ///< Register Host-to-Device
}  // namespace FisType

// ============================================================
// Port Interrupt Status Bits
// ============================================================

namespace PxIs {
constexpr uint32_t DHRS = (1U << 0);   ///< Device to Host Register FIS
constexpr uint32_t PSS  = (1U << 1);   ///< PIO Setup FIS
constexpr uint32_t DSS  = (1U << 2);   ///< DMA Setup FIS
constexpr uint32_t SDBS = (1U << 3);   ///< Set Device Bits FIS
constexpr uint32_t DMPS = (1U << 23);  ///< Device Mechanical Presence
}  // namespace PxIs

// ============================================================
// Port SATA Status (PxSSTS) Detection field
// ============================================================

namespace PxSsts {
constexpr uint32_t DET_MASK   = 0x0FU;
constexpr uint32_t DET_ACTIVE = 0x03;  ///< Device present and communication active
}  // namespace PxSsts

// ============================================================
// Command Header Flags
// ============================================================

namespace CmdHdrFlags {
constexpr uint16_t WRITE     = (1U << 6);  ///< Direction: write to device
constexpr uint16_t READ      = (1U << 5);  ///< Direction: read from device
constexpr uint16_t PREFETCH  = (1U << 7);  ///< Prefetchable
constexpr uint16_t RESET     = (1U << 8);  ///< Reset
constexpr uint16_t BIST      = (1U << 9);  ///< BIST
constexpr uint16_t ATAPI     = (1U << 5);  ///< ATAPI (overloaded with READ)
constexpr uint16_t CFL_SHIFT = 0;          ///< Command FIS length in dwords, shifted
}  // namespace CmdHdrFlags

// ============================================================
// HBA Port Registers (MMIO, one per port)
// ============================================================

/**
 * @brief AHCI Port Memory-Mapped Registers
 *
 * Each port occupies 0x80 bytes in the ABAR (BAR5) MMIO region.
 * All fields are volatile since they are hardware registers.
 */
struct [[gnu::packed]] HBAPort {
    volatile uint32_t clb;        ///< 0x00: Command List Base Address (low 32)
    volatile uint32_t clbu;       ///< 0x04: Command List Base Address (upper 32)
    volatile uint32_t fb;         ///< 0x08: FIS Base Address (low 32)
    volatile uint32_t fbu;        ///< 0x0C: FIS Base Address (upper 32)
    volatile uint32_t is;         ///< 0x10: Interrupt Status
    volatile uint32_t ie;         ///< 0x14: Interrupt Enable
    volatile uint32_t cmd;        ///< 0x18: Command and Status
    volatile uint32_t rsv0;       ///< 0x1C: Reserved
    volatile uint32_t tfd;        ///< 0x20: Task File Data
    volatile uint32_t sig;        ///< 0x24: Signature
    volatile uint32_t ssts;       ///< 0x28: SATA Status (SCR0: SStatus)
    volatile uint32_t sctl;       ///< 0x2C: SATA Control (SCR2: SControl)
    volatile uint32_t serr;       ///< 0x30: SATA Error (SCR1: SError)
    volatile uint32_t sact;       ///< 0x34: SATA Active (SCR3: SActive)
    volatile uint32_t ci;         ///< 0x38: Command Issue
    volatile uint32_t sntf;       ///< 0x3C: SNotification (SCR4)
    volatile uint32_t fbs;        ///< 0x40: FIS-Based Switching Control
    volatile uint32_t rsv1[11];   ///< 0x44-0x6F: Reserved
    volatile uint32_t vendor[4];  ///< 0x70-0x7F: Vendor Specific
};

static_assert(sizeof(HBAPort) == 0x80, "HBAPort must be 128 bytes");

// ============================================================
// HBA Controller Memory-Mapped Registers
// ============================================================

/**
 * @brief AHCI Controller (HBA) Memory-Mapped Registers
 *
 * Starts with generic host control registers, followed by
 * per-port register sets.  The port registers begin at
 * offset 0x100.
 */
struct [[gnu::packed]] HBAMem {
    volatile uint32_t cap;         ///< 0x00: Host Capabilities
    volatile uint32_t ghc;         ///< 0x04: Global HBA Control
    volatile uint32_t is;          ///< 0x08: Interrupt Status (port bitmap)
    volatile uint32_t pi;          ///< 0x0C: Port Implemented (bitmap)
    volatile uint32_t vs;          ///< 0x10: AHCI Version
    volatile uint32_t ccc_ctl;     ///< 0x14: Command Completion Coalescing Control
    volatile uint32_t ccc_pts;     ///< 0x18: CCC Ports
    volatile uint32_t em_loc;      ///< 0x1C: Enclosure Management Location
    volatile uint32_t em_ctl;      ///< 0x20: Enclosure Management Control
    volatile uint32_t cap2;        ///< 0x24: Host Capabilities Extended
    volatile uint32_t bohc;        ///< 0x28: BIOS/OS Handoff Control
    volatile uint8_t  rsv[116];    ///< 0x2C-0x9F: Reserved
    volatile uint8_t  vendor[96];  ///< 0xA0-0xFF: Vendor Specific
    HBAPort           ports[1];    ///< 0x100+: Port registers (variable count)
};

// Port registers start at offset 0x100 from the HBA base.
// Generic host area: 4+4+4+4+4+4+4+4+4+4+4 = 44 bytes of named fields
// + rsv[116] + vendor[96] = 256 = 0x100 bytes total before ports[].
static_assert(sizeof(HBAMem) - sizeof(HBAPort) == 0x100,
              "Port registers must start at offset 0x100");

// ============================================================
// Command Header (in the Command List)
// ============================================================

/**
 * @brief AHCI Command Header entry (one per slot in the Command List)
 *
 * Each port's command list contains CMD_SLOTS (32) of these
 * entries.  The command table (containing the FIS and PRDT)
 * is pointed to by ctba/ctbau.
 */
struct [[gnu::packed]] HBACommandHeader {
    uint8_t           cfl : 5;       ///< Command FIS length (in dwords)
    uint8_t           atapi : 1;     ///< ATAPI command flag
    uint8_t           write : 1;     ///< Write direction flag
    uint8_t           prefetch : 1;  ///< Prefetchable
    uint8_t           reset : 1;     ///< Reset
    uint8_t           bist : 1;      ///< BIST
    uint8_t           rsv0 : 1;      ///< Reserved
    uint8_t           pmp : 4;       ///< Port Multiplier Port
    uint16_t          prdtl;         ///< PRDT entry count
    volatile uint32_t prdbc;         ///< PRDT byte count (written by HBA)
    uint32_t          ctba;          ///< Command Table Base Address (low 32)
    uint32_t          ctbau;         ///< Command Table Base Address (upper 32)
    uint32_t          rsv1[4];       ///< Reserved
};

static_assert(sizeof(HBACommandHeader) == CMD_TBL_SIZE, "HBACommandHeader must be 32 bytes");

// ============================================================
// Command Table (FIS + PRDT)
// ============================================================

/**
 * @brief Physical Region Descriptor (scatter-gather entry)
 *
 * Each PRD describes one contiguous physical buffer for the DMA
 * transfer.  Byte count must not cross a 4 GB boundary.
 */
struct [[gnu::packed]] HBAPrdtEntry {
    uint32_t dba;       ///< Data Byte Address (low 32 bits)
    uint32_t dbau;      ///< Data Byte Address (upper 32 bits)
    uint32_t rsv0;      ///< Reserved
    uint32_t dbc : 22;  ///< Data Byte Count (byte count - 1)
    uint32_t rsv1 : 9;  ///< Reserved
    uint32_t i : 1;     ///< Interrupt on completion
};

static_assert(sizeof(HBAPrdtEntry) == 16, "HBAPrdtEntry must be 16 bytes");

/**
 * @brief Command Table (FIS followed by PRDT entries)
 *
 * The CFIS (Command FIS) is an array of dwords that encodes
 * the ATA command.  PRDT entries immediately follow the
 * 128-byte header area.
 */
struct [[gnu::packed]] HBACommandTable {
    uint8_t      cfis[64];                ///< Command FIS (up to 64 bytes)
    uint8_t      acmd[16];                ///< ATAPI command (unused for SATA)
    uint8_t      rsv[48];                 ///< Reserved
    HBAPrdtEntry prdt[MAX_PRDT_ENTRIES];  ///< PRDT entries
};

// ============================================================
// Register Host-to-Device FIS (Command Frame)
// ============================================================

/**
 * @brief Register Host-to-Device FIS layout (CFIS)
 *
 * Constructed in the first 5 dwords of cfis[] in HBACommandTable.
 * Byte 0: FIS type (0x27)
 * Byte 1: Flags (bit 6 = command bit, must be set)
 * Byte 2: Command register
 * Byte 3: Features
 * Bytes 4-7: LBA low + mid + high + device
 * Bytes 8-11: LBA exp low + exp mid + exp high + features exp
 * Bytes 12-13: Sector count (low + high)
 * Bytes 14-15: Reserved
 * Bytes 16-19: Control / auxiliary
 */
struct [[gnu::packed]] RegH2DFIS {
    uint8_t fis_type;
    uint8_t flags;
    uint8_t command;
    uint8_t feature;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t feature_exp;
    uint8_t count0;
    uint8_t count1;
    uint8_t rsv0;
    uint8_t rsv1;
    uint8_t control;
    uint8_t rsv2[3];
};

static_assert(sizeof(RegH2DFIS) == 20, "RegH2DFIS must be 20 bytes");

}  // namespace cinux::drivers::ahci
