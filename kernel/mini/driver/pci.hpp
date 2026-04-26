/**
 * @file kernel/mini/driver/pci.hpp
 * @brief Minimal PCI Configuration Space Access
 *
 * Provides just enough PCI support to detect the IDE controller,
 * read its BAR4 (Bus Master register base), and enable Bus Master
 * DMA.  Header-only — no .cpp file needed.
 *
 * Namespace: cinux::mini::driver::pci
 */

#pragma once

#include <stdint.h>

#include "io.h"

namespace cinux::mini::driver::pci {

// ============================================================
// PCI I/O Port Definitions
// ============================================================

constexpr uint16_t PCI_CONFIG_ADDR = 0xCF8;  ///< Configuration address port
constexpr uint16_t PCI_CONFIG_DATA = 0xCFC;  ///< Configuration data port

// ============================================================
// PCI Constants
// ============================================================

constexpr uint8_t PCI_CLASS_MASS_STORAGE = 0x01;
constexpr uint8_t PCI_SUBCLASS_IDE       = 0x01;

/// Bus Master Enable bit in PCI command register
constexpr uint16_t PCI_CMD_BUS_MASTER = 0x0004;

// Config register offsets
constexpr uint8_t PCI_REG_COMMAND = 0x04;
constexpr uint8_t PCI_REG_CLASS   = 0x08;  ///< 32-bit: class|subclass|prog_if|revision
constexpr uint8_t PCI_REG_BAR4    = 0x20;  ///< Bus Master register base

// ============================================================
// PRD (Physical Region Descriptor)
// ============================================================

/**
 * @brief Physical Region Descriptor for Bus Master DMA
 *
 * Each PRD describes a contiguous physical memory buffer.
 * The DMA engine walks the PRD table sequentially.
 *
 * Layout (8 bytes):
 *   Bytes 0-3: Buffer physical address (32-bit, word-aligned)
 *   Bytes 4-5: Byte count (0 means 65536, max 65536 per PRD)
 *   Bytes 6-7: Control (bit 15 = End of Table flag)
 */
struct Prd {
    uint32_t buffer_addr;
    uint16_t byte_count;
    uint16_t flags;  // bit 15 = EOT
} __attribute__((packed));

static_assert(sizeof(Prd) == 8, "PRD must be 8 bytes");

// ============================================================
// PRD Flag Helpers
// ============================================================

constexpr uint16_t PRD_FLAG_EOT = 0x8000;  ///< End-of-table flag (bit 15)

// ============================================================
// Configuration Space Access
// ============================================================

/**
 * @brief Read a 32-bit value from PCI configuration space
 *
 * @param bus     PCI bus number
 * @param device  Device number (0-31)
 * @param func    Function number (0-7)
 * @param offset  Register offset (must be 4-byte aligned)
 * @return 32-bit value from the configuration register
 */
inline uint32_t config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t addr = (1u << 31) | (static_cast<uint32_t>(bus) << 16) |
                    (static_cast<uint32_t>(device) << 11) | (static_cast<uint32_t>(func) << 8) |
                    (offset & 0xFC);
    cinux::mini::io::outl(PCI_CONFIG_ADDR, addr);
    return cinux::mini::io::inl(PCI_CONFIG_DATA);
}

/**
 * @brief Write a 32-bit value to PCI configuration space
 */
inline void config_write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset,
                         uint32_t value) {
    uint32_t addr = (1u << 31) | (static_cast<uint32_t>(bus) << 16) |
                    (static_cast<uint32_t>(device) << 11) | (static_cast<uint32_t>(func) << 8) |
                    (offset & 0xFC);
    cinux::mini::io::outl(PCI_CONFIG_ADDR, addr);
    cinux::mini::io::outl(PCI_CONFIG_DATA, value);
}

// ============================================================
// IDE Controller Detection
// ============================================================

/**
 * @brief Scan PCI bus 0 for an IDE controller
 *
 * Searches devices 0-31, function 0, for a device with
 * class=0x01 (mass storage), subclass=0x01 (IDE).
 *
 * @param out_bus    [out] Bus number of the IDE controller
 * @param out_device [out] Device number of the IDE controller
 * @return true if found, false otherwise
 */
inline bool find_ide_controller(uint8_t& out_bus, uint8_t& out_device, uint8_t& out_func) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t vendor = config_read(0, dev, func, 0x00);
            if ((vendor & 0xFFFF) == 0xFFFF)
                continue;

            uint32_t class_reg  = config_read(0, dev, func, PCI_REG_CLASS);
            uint8_t  subclass   = static_cast<uint8_t>((class_reg >> 16) & 0xFF);
            uint8_t  class_code = static_cast<uint8_t>((class_reg >> 24) & 0xFF);

            if (class_code == PCI_CLASS_MASS_STORAGE && subclass == PCI_SUBCLASS_IDE) {
                out_bus    = 0;
                out_device = dev;
                out_func   = func;
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Read BAR4 (Bus Master register base address)
 *
 * For PIIX4 IDE, BAR4 is an I/O port range. The low 4 bits
 * are flags; the actual base address is in bits 15:4.
 *
 * @return BAR4 raw value (mask with 0xFFF0 to get base)
 */
inline uint32_t read_bar4(uint8_t bus, uint8_t device, uint8_t func) {
    return config_read(bus, device, func, PCI_REG_BAR4);
}

/**
 * @brief Enable Bus Master DMA in the PCI command register
 *
 * Sets the Bus Master Enable bit (bit 2) without clearing
 * other bits (I/O space enable, etc.).
 */
inline void enable_bus_master(uint8_t bus, uint8_t device, uint8_t func) {
    uint32_t cmd = config_read(bus, device, func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    config_write(bus, device, func, PCI_REG_COMMAND, cmd);
}

}  // namespace cinux::mini::driver::pci
