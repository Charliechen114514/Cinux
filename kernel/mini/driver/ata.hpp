/**
 * @file kernel/mini/driver/ata.hpp
 * @brief ATA PIO Mode Disk Driver for x86_64
 *
 * Provides polling-based ATA PIO (Programmed I/O) disk read operations
 * for the mini kernel. This driver talks directly to the primary ATA
 * controller's I/O ports (0x1F0-0x1F7, 0x3F6) without DMA or interrupts.
 *
 * In the Cinux boot chain, the mini kernel uses this driver to load the
 * "big kernel" ELF binary from disk into memory, then jumps to its entry
 * point. The big kernel is stored on disk starting at a known LBA offset
 * (after the mini kernel's reserved sectors).
 *
 * Limitations:
 *   - Only supports PIO mode (no DMA, no Ultra DMA)
 *   - Only supports LBA28/LBA48 addressing (no CHS)
 *   - Only the primary channel (0x1F0) is supported
 *   - Polling-based: no interrupt-driven I/O
 *   - Read-only: no write support needed for the loader
 *
 * @note Must call init() before any read operations.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::mini::driver::ata {

// ============================================================
// ATA Controller I/O Port Definitions
// ============================================================

/// Primary ATA channel base I/O port
constexpr uint16_t ATA_PRIMARY_BASE = 0x1F0;

/// Primary ATA channel control port
constexpr uint16_t ATA_PRIMARY_CTRL = 0x3F6;

// Data register offsets from base port
constexpr uint16_t ATA_REG_DATA       = 0;  ///< Data register (16-bit read/write)
constexpr uint16_t ATA_REG_ERROR      = 1;  ///< Error register (read)
constexpr uint16_t ATA_REG_FEATURES   = 1;  ///< Features register (write)
constexpr uint16_t ATA_REG_SECTOR_CNT = 2;  ///< Sector count register
constexpr uint16_t ATA_REG_LBA_LOW    = 3;  ///< LBA low byte [0:7]
constexpr uint16_t ATA_REG_LBA_MID    = 4;  ///< LBA mid byte [8:15]
constexpr uint16_t ATA_REG_LBA_HIGH   = 5;  ///< LBA high byte [16:23]
constexpr uint16_t ATA_REG_DRIVE      = 6;  ///< Drive/head register
constexpr uint16_t ATA_REG_STATUS     = 7;  ///< Status register (read)
constexpr uint16_t ATA_REG_COMMAND    = 7;  ///< Command register (write)

// ============================================================
// ATA Status Register Bits
// ============================================================
constexpr uint8_t ATA_STATUS_ERR = 0x01;  ///< Error occurred
constexpr uint8_t ATA_STATUS_DRQ = 0x08;  ///< Data request ready
constexpr uint8_t ATA_STATUS_DF  = 0x20;  ///< Drive fault
constexpr uint8_t ATA_STATUS_RDY = 0x40;  ///< Drive ready
constexpr uint8_t ATA_STATUS_BSY = 0x80;  ///< Drive busy

// ============================================================
// ATA Commands
// ============================================================
constexpr uint8_t ATA_CMD_READ_PIO     = 0x20;  ///< Read sectors (LBA28)
constexpr uint8_t ATA_CMD_READ_PIO_EXT = 0x24;  ///< Read sectors (LBA48)
constexpr uint8_t ATA_CMD_READ_DMA     = 0xC8;  ///< Read DMA (LBA28)
constexpr uint8_t ATA_CMD_READ_DMA_EXT = 0x25;  ///< Read DMA (LBA48)
constexpr uint8_t ATA_CMD_IDENTIFY     = 0xEC;  ///< Identify device

// ============================================================
// ATA Drive Selection Bits
// ============================================================
constexpr uint8_t ATA_DRIVE_MASTER = 0xE0;  ///< Select master drive with LBA bit set
constexpr uint8_t ATA_DRIVE_LBA48  = 0x40;  ///< LBA48 mode bit in sector count

// ============================================================
// Sector Size
// ============================================================
constexpr uint16_t ATA_SECTOR_SIZE = 512;  ///< Standard ATA sector size

// ============================================================
// Bus Master DMA Register Offsets (relative to BAR4 base)
// ============================================================
// Primary channel; secondary channel would be at +8
constexpr uint8_t BM_CMD    = 0x00;  ///< Bus Master Command register
constexpr uint8_t BM_STATUS = 0x02;  ///< Bus Master Status register
constexpr uint8_t BM_PRDT   = 0x04;  ///< PRD Table Address register (32-bit)

// BM_CMD bits
constexpr uint8_t BM_CMD_START     = 0x01;  ///< Bit 0: Start DMA transfer
constexpr uint8_t BM_CMD_WRITE_DIR = 0x08;  ///< Bit 3: 1=write to disk, 0=read

// BM_STATUS bits
constexpr uint8_t BM_STATUS_ACTIVE    = 0x01;  ///< Bit 0: DMA engine active
constexpr uint8_t BM_STATUS_ERROR     = 0x02;  ///< Bit 1: DMA error
constexpr uint8_t BM_STATUS_INTERRUPT = 0x04;  ///< Bit 2: DMA completion interrupt
constexpr uint8_t BM_STATUS_DMA_ERR   = 0x20;  ///< Bit 5: PCI target/master abort

// ============================================================
// Initialization
// ============================================================

/**
 * @brief Initialize the ATA controller
 *
 * Performs a software reset on the primary ATA channel and waits for
 * the master drive to report ready (BSY clear, RDY set). This must
 * be called before any read operations.
 *
 * @return true if the controller initialized successfully, false on timeout
 *
 * @note This function polls the status register with a timeout.
 *       In QEMU, initialization should be near-instant.
 */
bool init();

// ============================================================
// Disk Read Operations
// ============================================================

/**
 * @brief Read sectors from disk using ATA PIO mode
 *
 * Reads @p count consecutive sectors starting from LBA @p lba into
 * the buffer at @p buffer. Uses LBA48 addressing for LBAs above 28 bits,
 * otherwise falls back to LBA28 for compatibility.
 *
 * @param lba     Starting Logical Block Address (0-based sector index)
 * @param count   Number of 512-byte sectors to read
 * @param buffer  Destination buffer (must be at least count * 512 bytes)
 * @return true if all sectors were read successfully, false on error
 *
 * @note The buffer must be properly aligned for 16-bit inw operations.
 *       Each sector transfer involves 256 x 16-bit inw reads from the data port.
 *       This function blocks until all data is transferred (polling).
 */
bool read(uint64_t lba, uint16_t count, void* buffer);

/**
 * @brief Read a large number of sectors from disk, handling chunking
 *
 * For reads exceeding 65535 sectors (32MB), automatically splits into
 * multiple ATA PIO commands.  Each chunk is at most 65535 sectors
 * (LBA48 maximum per command).
 *
 * @param lba     Starting Logical Block Address (0-based sector index)
 * @param count   Total number of 512-byte sectors to read (32-bit)
 * @param buffer  Destination buffer (must be at least count * 512 bytes)
 * @return true if all chunks were read successfully, false on error
 */
bool read_large(uint64_t lba, uint32_t count, void* buffer);

// ============================================================
// DMA Operations
// ============================================================

/**
 * @brief Check whether DMA is available and initialized
 *
 * After init(), returns true if a PCI IDE controller with Bus Master
 * capability was detected and the DMA engine was set up.
 */
bool is_dma_available();

/**
 * @brief Read sectors using PCI Bus Master DMA
 *
 * Transfers data directly from disk to memory via DMA, without
 * CPU involvement per sector.  The buffer must reside in identity-mapped
 * physical memory below 4GB.
 *
 * @param lba     Starting LBA (48-bit max)
 * @param count   Number of 512-byte sectors (max 65535)
 * @param buffer  Physical/identity-mapped destination buffer
 * @return true on success, false on DMA or ATA error
 */
bool dma_read(uint64_t lba, uint16_t count, void* buffer);

}  // namespace cinux::mini::driver::ata
