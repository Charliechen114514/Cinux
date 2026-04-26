/**
 * @file kernel/drivers/pci/pci.cpp
 * @brief PCI configuration space access and device enumeration
 */

#include "pci.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::io::io_inl;
using cinux::io::io_outl;

namespace cinux::drivers::pci {

// ============================================================
// Configuration Space Access
// ============================================================

uint32_t PCI::pci_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    // Build the 32-bit address: enable bit 31, then bus(23:16),
    // slot(15:11), func(10:8), offset(7:0, must be dword-aligned)
    uint32_t address = (1U << 31) | (static_cast<uint32_t>(bus) << 16) |
                       (static_cast<uint32_t>(slot) << 11) | (static_cast<uint32_t>(func) << 8) |
                       (offset & 0xFC);

    io_outl(PciPort::CONFIG_ADDRESS, address);

    uint32_t value = io_inl(PciPort::CONFIG_DATA);

    return value;
}

void PCI::pci_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    // Same address format as pci_read
    uint32_t address = (1U << 31) | (static_cast<uint32_t>(bus) << 16) |
                       (static_cast<uint32_t>(slot) << 11) | (static_cast<uint32_t>(func) << 8) |
                       (offset & 0xFC);

    io_outl(PciPort::CONFIG_ADDRESS, address);
    io_outl(PciPort::CONFIG_DATA, value);
}

// ============================================================
// Internal Helpers
// ============================================================

bool PCI::scan_function(uint8_t bus, uint8_t slot, uint8_t func, PCIDevice& dev) {
    // Read vendor ID first -- 0xFFFF means no device present
    uint32_t vendev = pci_read(bus, slot, func, PciReg::VENDOR_ID);
    uint16_t vendor = static_cast<uint16_t>(vendev & 0xFFFF);

    if (vendor == VENDOR_INVALID) {
        return false;
    }

    uint16_t device = static_cast<uint16_t>((vendev >> 16) & 0xFFFF);

    // Read class/subclass/prog_if in one dword at offset 0x08
    uint32_t class_rev  = pci_read(bus, slot, func, 0x08);
    uint8_t  prog_if    = static_cast<uint8_t>((class_rev >> 8) & 0xFF);
    uint8_t  subclass   = static_cast<uint8_t>((class_rev >> 16) & 0xFF);
    uint8_t  class_code = static_cast<uint8_t>((class_rev >> 24) & 0xFF);

    // Read header type at offset 0x0C (low byte of high word)
    uint32_t hdr         = pci_read(bus, slot, func, PciReg::HEADER_TYPE);
    uint8_t  header_type = static_cast<uint8_t>((hdr >> 16) & 0xFF);

    dev.bus         = bus;
    dev.slot        = slot;
    dev.func        = func;
    dev.vendor_id   = vendor;
    dev.device_id   = device;
    dev.class_code  = class_code;
    dev.subclass    = subclass;
    dev.prog_if     = prog_if;
    dev.header_type = header_type;

    return true;
}

void PCI::read_bars(PCIDevice& dev) {
    // BAR register offsets in order
    constexpr uint8_t bar_offsets[BAR_COUNT] = {PciReg::BAR0, PciReg::BAR1, PciReg::BAR2,
                                                PciReg::BAR3, PciReg::BAR4, PciReg::BAR5};

    for (uint8_t i = 0; i < BAR_COUNT; ++i) {
        uint32_t raw = pci_read(dev.bus, dev.slot, dev.func, bar_offsets[i]);

        if ((raw & BAR_IO_SPACE) != 0) {
            // I/O space BAR: address is in bits 15:2
            dev.bar[i] = raw & 0xFFFFFFFC;
        } else {
            // Memory space BAR
            dev.bar[i] = raw & BAR_ADDR_MASK_32;

            // If this is a 64-bit BAR, consume the next BAR register
            // as the upper 32 bits and skip the next index
            if ((raw & BAR_TYPE_MASK) == BAR_TYPE_64 && (i + 1) < BAR_COUNT) {
                uint32_t high  = pci_read(dev.bus, dev.slot, dev.func, bar_offsets[i + 1]);
                dev.bar[i]     = (static_cast<uint64_t>(high) << 32) | (raw & BAR_ADDR_MASK_32);
                dev.bar[i + 1] = 0;
                ++i;  // Skip the next BAR index
            }
        }
    }
}

// ============================================================
// Public Interface
// ============================================================

void PCI::init() {
    cinux::lib::kprintf("[PCI] Scanning PCI bus...\n");

    uint32_t device_count = 0;

    for (uint8_t bus = 0; bus < MAX_BUS; ++bus) {
        for (uint8_t slot = 0; slot < MAX_SLOT; ++slot) {
            for (uint8_t func = 0; func < MAX_FUNC; ++func) {
                PCIDevice dev{};
                if (!scan_function(bus, slot, func, dev)) {
                    // Func 0 empty means no device at this slot at all
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                ++device_count;
                cinux::lib::kprintf(
                    "[PCI] %02x:%02x.%x %04x:%04x "
                    "class=%02x sub=%02x\n",
                    bus, slot, func, dev.vendor_id, dev.device_id, dev.class_code, dev.subclass);
            }
        }
    }

    cinux::lib::kprintf("[PCI] Found %u PCI devices.\n", device_count);
}

bool PCI::find_ahci(PCIDevice& out) const {
    for (uint8_t bus = 0; bus < MAX_BUS; ++bus) {
        for (uint8_t slot = 0; slot < MAX_SLOT; ++slot) {
            for (uint8_t func = 0; func < MAX_FUNC; ++func) {
                PCIDevice dev{};
                if (!scan_function(bus, slot, func, dev)) {
                    if (func == 0) {
                        break;
                    }
                    continue;
                }

                if (dev.class_code == PciClass::MASS_STORAGE &&
                    dev.subclass == PciClass::AHCI_SUBCLASS) {
                    // Found an AHCI controller -- read BARs
                    read_bars(dev);
                    out = dev;

                    cinux::lib::kprintf(
                        "[PCI] AHCI found: %02x:%02x.%x "
                        "BAR5=0x%p\n",
                        dev.bus, dev.slot, dev.func, static_cast<uint64_t>(dev.bar[5]));
                    return true;
                }
            }
        }
    }

    cinux::lib::kprintf("[PCI] No AHCI controller found.\n");
    return false;
}

}  // namespace cinux::drivers::pci
