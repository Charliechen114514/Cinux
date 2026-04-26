/**
 * @file kernel/mini/driver/ata.cpp
 * @brief ATA Disk Driver Implementation (PIO + Bus Master DMA)
 *
 * Implements polling-based ATA PIO read operations and PCI Bus Master DMA
 * for large transfers.  DMA is detected and initialized transparently
 * during init(); read_large() automatically uses DMA when available.
 */

#include "ata.hpp"

#include <stddef.h>
#include <stdint.h>

#include "driver/io.h"
#include "driver/pci.hpp"
#include "lib/kprintf.h"

using cinux::mini::lib::kprintf;

namespace cinux::mini::driver::ata {

// ============================================================
// Internal State
// ============================================================

/// Whether the ATA controller has been successfully initialized
static bool s_initialized = false;

/// DMA state
static bool      s_dma_available = false;
static uint16_t  s_bm_base       = 0;        ///< Bus Master I/O base (from BAR4)
static uint32_t  s_prdt_phys     = 0;        ///< Physical address of PRDT page
static pci::Prd* s_prdt          = nullptr;  ///< Virtual pointer to PRDT page

/// Static PRDT buffer — avoids PMM dependency so DMA works even when
/// memory is scarce (e.g. test kernel with only 3MB RAM).
/// 512 entries × 8 bytes = 4KB, supports up to 32MB per DMA operation.
static pci::Prd s_prdt_storage[512] __attribute__((aligned(4096)));

/// Higher-half virtual base, used to convert static buffer address to physical
static constexpr uint64_t KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ULL;

namespace {

// ============================================================
// Internal I/O Helpers
// ============================================================

inline uint8_t read_reg(uint16_t reg) {
    return io::inb(ATA_PRIMARY_BASE + reg);
}

inline void write_reg(uint16_t reg, uint8_t value) {
    io::outb(ATA_PRIMARY_BASE + reg, value);
}

// ============================================================
// Status Polling Helpers
// ============================================================

bool wait_not_busy() {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = read_reg(ATA_REG_STATUS);
        if ((status & ATA_STATUS_BSY) == 0) {
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

bool wait_data_ready() {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = read_reg(ATA_REG_STATUS);

        if (status & ATA_STATUS_ERR) {
            kprintf("[ATA] ERROR: drive error, status=0x%02x, error=0x%02x\n", status,
                    read_reg(ATA_REG_ERROR));
            return false;
        }
        if (status & ATA_STATUS_DF) {
            kprintf("[ATA] ERROR: drive fault, status=0x%02x\n", status);
            return false;
        }

        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ)) {
            return true;
        }

        __asm__ volatile("pause");
    }

    kprintf("[ATA] ERROR: timeout waiting for data ready\n");
    return false;
}

void delay_400ns() {
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
}

}  // anonymous namespace

// ============================================================
// Initialization
// ============================================================

bool init() {
    kprintf("[INIT] Initializing ATA controller...\n");

    // Step 1: Software reset via control register
    io::outb(ATA_PRIMARY_CTRL, 0x04);
    delay_400ns();
    io::outb(ATA_PRIMARY_CTRL, 0x00);
    delay_400ns();

    // Step 2: Wait for drive to come out of reset
    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: drive did not come out of reset (BSY timeout)\n");
        return false;
    }

    // Step 3: Select master drive with LBA mode
    write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER);
    delay_400ns();

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: master drive not ready after selection\n");
        return false;
    }

    uint8_t status = read_reg(ATA_REG_STATUS);

    if (status == 0xFF) {
        kprintf("[ATA] ERROR: no drive detected (floating bus)\n");
        return false;
    }

    if ((status & ATA_STATUS_RDY) == 0) {
        kprintf("[ATA] ERROR: drive not ready, status=0x%02x\n", status);
        return false;
    }

    s_initialized = true;
    kprintf("[INIT] ATA controller initialized successfully (status=0x%02x).\n", status);

    // Step 4: Attempt DMA initialization
    uint8_t pci_bus, pci_dev, pci_func;
    if (pci::find_ide_controller(pci_bus, pci_dev, pci_func)) {
        kprintf("[ATA] Found PCI IDE controller at bus %u, device %u, func %u\n", pci_bus, pci_dev,
                pci_func);

        uint32_t bar4_raw = pci::read_bar4(pci_bus, pci_dev, pci_func);
        uint16_t bm_base  = static_cast<uint16_t>(bar4_raw & 0xFFF0);

        if (bm_base != 0) {
            pci::enable_bus_master(pci_bus, pci_dev, pci_func);

            // Use static PRDT buffer (no PMM dependency)
            s_prdt          = s_prdt_storage;
            s_prdt_phys     = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s_prdt_storage) -
                                                    KERNEL_VIRT_BASE);
            s_bm_base       = bm_base;
            s_dma_available = true;

            kprintf("[ATA] DMA enabled: BAR4=0x%04x, PRDT at phys 0x%08x\n", bm_base, s_prdt_phys);
        }
    } else {
        kprintf("[ATA] No PCI IDE controller found, DMA unavailable\n");
    }

    return true;
}

// ============================================================
// DMA Availability Query
// ============================================================

bool is_dma_available() {
    return s_dma_available;
}

// ============================================================
// DMA Read Implementation
// ============================================================

bool dma_read(uint64_t lba, uint16_t count, void* buffer) {
    // Validate
    if (!s_dma_available)
        return false;
    if (count == 0 || buffer == nullptr)
        return false;
    if (lba >= (1ULL << 48))
        return false;

    // Buffer must be in 32-bit addressable physical memory
    auto buf_addr = reinterpret_cast<uintptr_t>(buffer);
    if (buf_addr > 0xFFFFFFFFULL)
        return false;

    // Fill PRD table
    uint32_t total_bytes = static_cast<uint32_t>(count) * ATA_SECTOR_SIZE;
    uint32_t buf_phys    = static_cast<uint32_t>(buf_addr);
    uint32_t prd_count   = 0;

    while (total_bytes > 0) {
        uint32_t chunk = total_bytes;
        if (chunk > 65536)
            chunk = 65536;

        s_prdt[prd_count].buffer_addr = buf_phys;
        s_prdt[prd_count].byte_count  = static_cast<uint16_t>(chunk & 0xFFFF);
        s_prdt[prd_count].flags       = 0;

        buf_phys += chunk;
        total_bytes -= chunk;
        prd_count++;

        if (prd_count >= 512)
            break;  // PRDT page limit
    }
    s_prdt[prd_count - 1].flags = pci::PRD_FLAG_EOT;

    // Wait for drive to not be busy
    if (!wait_not_busy()) {
        kprintf("[ATA DMA] ERROR: drive busy\n");
        return false;
    }

    // Program Bus Master registers
    io::outl(s_bm_base + BM_PRDT, s_prdt_phys);  // Set PRDT address
    io::outb(s_bm_base + BM_STATUS, 0x06);       // Clear error + interrupt flags
    io::outb(s_bm_base + BM_CMD, 0x00);          // Stop DMA, read direction

    // Issue ATA READ DMA EXT command (always LBA48 for DMA)
    write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER | 0x40);
    delay_400ns();

    // High-order bytes
    write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>((count >> 8) & 0xFF));
    write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>((lba >> 24) & 0xFF));
    write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 32) & 0xFF));
    write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 40) & 0xFF));

    // Low-order bytes
    write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
    write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>(lba & 0xFF));
    write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 8) & 0xFF));
    write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 16) & 0xFF));

    // Send READ DMA EXT command
    write_reg(ATA_REG_COMMAND, ATA_CMD_READ_DMA_EXT);
    delay_400ns();

    // Start DMA transfer (bit 0 = start, bit 3 = 0 for read)
    io::outb(s_bm_base + BM_CMD, BM_CMD_START);

    // Poll for DMA completion
    for (uint32_t i = 0; i < 50000000; i++) {
        uint8_t bm_stat = io::inb(s_bm_base + BM_STATUS);

        if (bm_stat & (BM_STATUS_ERROR | BM_STATUS_DMA_ERR)) {
            kprintf("[ATA DMA] ERROR: BM_STATUS=0x%02x\n", bm_stat);
            io::outb(s_bm_base + BM_CMD, 0x00);  // Stop DMA
            return false;
        }

        if (bm_stat & BM_STATUS_INTERRUPT) {
            break;
        }
        __asm__ volatile("pause");
    }

    // Stop DMA engine
    io::outb(s_bm_base + BM_CMD, 0x00);

    // Check ATA status
    uint8_t ata_status = read_reg(ATA_REG_STATUS);
    if (ata_status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        kprintf("[ATA DMA] ERROR: ATA status=0x%02x after DMA\n", ata_status);
        return false;
    }

    // Clear BM_STATUS interrupt
    io::outb(s_bm_base + BM_STATUS, BM_STATUS_INTERRUPT);

    return true;
}

// ============================================================
// Disk Read Operations
// ============================================================

bool read(uint64_t lba, uint16_t count, void* buffer) {
    if (!s_initialized) {
        kprintf("[ATA] ERROR: driver not initialized\n");
        return false;
    }
    if (count == 0) {
        kprintf("[ATA] ERROR: zero sector count\n");
        return false;
    }
    if (buffer == nullptr) {
        kprintf("[ATA] ERROR: null buffer\n");
        return false;
    }
    if (lba >= (1ULL << 48)) {
        kprintf("[ATA] ERROR: LBA out of 48-bit range\n");
        return false;
    }

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: drive busy before read\n");
        return false;
    }

    bool use_lba48 = (lba >= 0x10000000ULL) || (count > 256);

    if (use_lba48) {
        write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER | 0x40);
        delay_400ns();

        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>((count >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>((lba >> 24) & 0xFF));
        write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 32) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 40) & 0xFF));

        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
        write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>(lba & 0xFF));
        write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 16) & 0xFF));

        write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
    } else {
        write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER | static_cast<uint8_t>((lba >> 24) & 0x0F));
        delay_400ns();

        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
        write_reg(ATA_REG_LBA_LOW, static_cast<uint8_t>(lba & 0xFF));
        write_reg(ATA_REG_LBA_MID, static_cast<uint8_t>((lba >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH, static_cast<uint8_t>((lba >> 16) & 0xFF));

        write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    }

    auto* buf = static_cast<uint16_t*>(buffer);

    for (uint16_t sector = 0; sector < count; sector++) {
        delay_400ns();
        if (!wait_data_ready()) {
            kprintf("[ATA] ERROR: failed reading sector %u (LBA %u)\n", sector,
                    static_cast<uint32_t>(lba + sector));
            return false;
        }

        {
            unsigned int words = 256;
            __asm__ volatile("rep insw"
                             : "+D"(buf), "+c"(words)
                             : "d"(static_cast<uint16_t>(ATA_PRIMARY_BASE))
                             : "memory");
        }
    }

    return true;
}
// ============================================================
// Chunked Disk Read (uses DMA when available)
// ============================================================

bool read_large(uint64_t lba, uint32_t count, void* buffer) {
    if (!s_initialized) {
        kprintf("[ATA] ERROR: driver not initialized\n");
        return false;
    }
    if (count == 0)
        return true;
    if (buffer == nullptr) {
        kprintf("[ATA] ERROR: null buffer\n");
        return false;
    }

    static constexpr uint32_t MAX_SECTORS_PER_READ = 65535;
    // Log progress every 64K sectors (~32 MB)
    static constexpr uint32_t PROGRESS_SECTORS     = 65536;

    auto*    buf         = static_cast<uint8_t*>(buffer);
    uint32_t remaining   = count;
    uint64_t current_lba = lba;
    uint32_t done        = 0;

    while (remaining > 0) {
        uint16_t chunk = static_cast<uint16_t>(
            remaining > MAX_SECTORS_PER_READ ? MAX_SECTORS_PER_READ : remaining);

        bool ok;
        if (s_dma_available) {
            ok = dma_read(current_lba, chunk, buf);
            if (!ok) {
                kprintf("[ATA] DMA failed at LBA 0x%x, falling back to PIO\n",
                        static_cast<uint32_t>(current_lba));
                ok = read(current_lba, chunk, buf);
            }
        } else {
            ok = read(current_lba, chunk, buf);
        }

        if (!ok) {
            kprintf("[ATA] ERROR: chunked read failed at LBA 0x%x (%u sectors)\n",
                    static_cast<uint32_t>(current_lba), chunk);
            return false;
        }
        buf += static_cast<size_t>(chunk) * ATA_SECTOR_SIZE;
        current_lba += chunk;
        remaining -= chunk;
        done += chunk;

        if (done >= PROGRESS_SECTORS) {
            uint32_t pct =
                static_cast<uint32_t>((static_cast<uint64_t>(count - remaining) * 100) / count);
            kprintf("[ATA] Read progress: %u MB / %u MB (%u%%)\n", (count - remaining) / 2048,
                    count / 2048, pct);
            done = 0;
        }
    }
    return true;
}

}  // namespace cinux::mini::driver::ata
