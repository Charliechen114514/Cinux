/**
 * @file kernel/drivers/pci/pci.hpp
 * @brief PCI configuration space access and device enumeration
 *
 * Provides a PCI class that encapsulates configuration space
 * read/write via the 0xCF8/0xCFC address-data register pair,
 * and a high-level enumeration method to locate AHCI host bus
 * adapters (class 0x01, subclass 0x06).
 *
 * Usage:
 *   PCI pci;
 *   pci.init();
 *   PCIDevice ahci_dev;
 *   if (pci.find_ahci(ahci_dev)) { ... }
 *
 * Namespace: cinux::drivers::pci
 */

#pragma once

#include <stdint.h>

#include "pci_config.hpp"

namespace cinux::drivers::pci {

// ============================================================
// PCI Device Descriptor
// ============================================================

/**
 * @brief Decoded PCI device/function descriptor
 *
 * Bundles the Bus/Slot/Func addressing triple and the decoded
 * header fields (vendor, device, class, subclass, prog_if,
 * and up to six BAR values) for a single PCI function.
 */
struct PCIDevice {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint32_t bar[BAR_COUNT];
};

// ============================================================
// PCI Class
// ============================================================

/**
 * @brief PCI configuration space access and device enumeration
 *
 * Encapsulates the 0xCF8/0xCFC address-data register pair for
 * reading and writing PCI configuration space.  Provides device
 * enumeration and BAR decoding methods.
 */
class PCI {
public:
    /**
     * @brief Initialise the PCI subsystem
     *
     * Scans all bus/slot/function combinations and logs a summary
     * of found devices to the kernel console.
     */
    void init();

    /**
     * @brief Read a 32-bit dword from PCI configuration space
     *
     * Constructs the 32-bit address word (enable bit | bus | slot |
     * func | register offset) and writes it to port 0xCF8, then
     * reads the result from port 0xCFC.
     *
     * @param bus      PCI bus number (0-255)
     * @param slot     Device/slot number on the bus (0-31)
     * @param func     Function number (0-7)
     * @param offset   Register offset (must be dword-aligned)
     * @return         The 32-bit value read from the configuration register
     */
    static uint32_t pci_read(uint8_t bus, uint8_t slot, uint8_t func,
                             uint8_t offset);

    /**
     * @brief Write a 32-bit dword to PCI configuration space
     *
     * @param bus      PCI bus number
     * @param slot     Device/slot number
     * @param func     Function number
     * @param offset   Register offset (must be dword-aligned)
     * @param value    The 32-bit value to write
     */
    static void pci_write(uint8_t bus, uint8_t slot, uint8_t func,
                          uint8_t offset, uint32_t value);

    /**
     * @brief Enumerate PCI buses and locate an AHCI HBA
     *
     * Scans all bus/slot/function combinations looking for a device
     * with class_code == 0x01 (mass storage) and subclass == 0x06
     * (SATA / AHCI).  The first match is written to @p out.
     *
     * @param out  Reference to a PCIDevice to fill with the match
     * @return     true if an AHCI device was found, false otherwise
     */
    bool find_ahci(PCIDevice& out) const;

    /**
     * @brief Read all six BAR values from a PCI device
     *
     * Handles both 32-bit and 64-bit memory BARs: when a 64-bit
     * BAR is encountered, the next BAR register is consumed as the
     * upper 32 bits and the following BAR index is skipped.
     *
     * @param dev  The PCI device (bus/slot/func) to read BARs from
     */
    static void read_bars(PCIDevice& dev);

private:
    /**
     * @brief Scan a single PCI function and fill in the device descriptor
     *
     * Reads vendor/device/class/subclass/prog_if/header_type from
     * the configuration space of the given bus/slot/func.  Returns
     * false if the vendor ID is 0xFFFF (empty slot).
     *
     * @param bus   Bus number
     * @param slot  Slot number
     * @param func  Function number
     * @param dev   Output device descriptor
     * @return      true if a device was found at this location
     */
    static bool scan_function(uint8_t bus, uint8_t slot, uint8_t func,
                              PCIDevice& dev);
};

}  // namespace cinux::drivers::pci
