/**
 * @file kernel/drivers/ahci/ahci.cpp
 * @brief AHCI SATA host bus adapter driver implementation
 */

#include "ahci.hpp"

#include <stdint.h>
#include <stddef.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::drivers::ahci {

// ============================================================
// Internal constants
// ============================================================

/// Virtual address region reserved for MMIO mappings
/// Using the same high-canonical range as the heap but well separated
static constexpr uint64_t MMIO_VIRT_BASE = 0xFFFF800000100000ULL;

/// Spin limit for polling operations (~1 second at ~1 GHz)
static constexpr uint32_t POLL_TIMEOUT = 100000000U;

/// Command table total size (header 128 B + 8 PRDT entries * 16 B)
static constexpr uint32_t CMD_TABLE_TOTAL = CMD_TBL_HDR_SIZE
    + MAX_PRDT_ENTRIES * sizeof(HBAPrdtEntry);

// ============================================================
// Private helpers
// ============================================================

HBAMem* AHCI::map_bar5(uint64_t bar5_phys) {
    // BAR5 is typically 4 KB for a single-port AHCI, but may be larger.
    // Map 2 pages to be safe (covers up to 8 ports).
    constexpr uint32_t BAR5_PAGES = 2;
    constexpr uint64_t BAR5_SIZE = BAR5_PAGES * cinux::arch::PAGE_SIZE;

    cinux::lib::kprintf("[AHCI] Mapping BAR5 phys=0x%p to virt=0x%p (%u pages)\n",
                        bar5_phys, MMIO_VIRT_BASE, BAR5_PAGES);

    // Map each page of BAR5 MMIO into the kernel page tables
    // Flags: present + writable + cache-disable (MMIO must be uncached)
    constexpr uint64_t mmio_flags = cinux::arch::FLAG_PRESENT
                                  | cinux::arch::FLAG_WRITABLE
                                  | cinux::arch::FLAG_PCD;

    for (uint32_t i = 0; i < BAR5_PAGES; ++i) {
        uint64_t phys = bar5_phys + i * cinux::arch::PAGE_SIZE;
        uint64_t virt = MMIO_VIRT_BASE + i * cinux::arch::PAGE_SIZE;

        if (!cinux::mm::g_vmm.map(virt, phys, mmio_flags)) {
            cinux::lib::kprintf("[AHCI] Failed to map BAR5 page %u\n", i);
            return nullptr;
        }
    }

    return reinterpret_cast<HBAMem*>(MMIO_VIRT_BASE);
}

void AHCI::reset_hba() const {
    if (hba_mem_ == nullptr) {
        return;
    }

    // Set GHC.HR (HBA Reset) bit
    hba_mem_->ghc |= GhcBits::HBA_RESET;

    // Wait for GHC.HR to clear (HBA clears it when reset completes)
    for (uint32_t i = 0; i < POLL_TIMEOUT; ++i) {
        if ((hba_mem_->ghc & GhcBits::HBA_RESET) == 0) {
            cinux::lib::kprintf("[AHCI] HBA reset complete.\n");
            return;
        }
        __asm__ volatile("pause");
    }

    cinux::lib::kprintf("[AHCI] HBA reset timeout!\n");
}

void AHCI::stop_port(HBAPort* port) {
    // Clear ST (Start) and wait for CR (Command Running) to clear
    port->cmd &= ~PxCmd::ST;

    for (uint32_t i = 0; i < POLL_TIMEOUT; ++i) {
        if ((port->cmd & PxCmd::CR) == 0) {
            break;
        }
        __asm__ volatile("pause");
    }

    // Clear FRE (FIS Receive Enable) and wait for FR to clear
    port->cmd &= ~PxCmd::FRE;

    for (uint32_t i = 0; i < POLL_TIMEOUT; ++i) {
        if ((port->cmd & PxCmd::FR) == 0) {
            break;
        }
        __asm__ volatile("pause");
    }
}

void AHCI::start_port(HBAPort* port) {
    // Enable FIS receive engine first
    port->cmd |= PxCmd::FRE;

    // Then set ST to start command processing
    port->cmd |= PxCmd::ST;
}

void AHCI::setup_port(uint8_t port_index) {
    auto* port = &hba_mem_->ports[port_index];

    // Stop the port engine before reconfiguring
    stop_port(port);

    // Allocate command list: 32 command headers * 32 bytes each = 1024 bytes
    // Needs to be 1 KB aligned (1024-byte boundary)
    uint64_t cmd_list_phys = cinux::mm::g_pmm.alloc_pages(1);  // 4 KB page is enough
    if (cmd_list_phys == 0) {
        cinux::lib::kprintf("[AHCI] Port %u: failed to alloc command list\n",
                            port_index);
        return;
    }
    cmd_list_phys_[port_index] = cmd_list_phys;

    // Zero out the command list page
    auto* cmd_list_virt = reinterpret_cast<uint8_t*>(
        cmd_list_phys + 0xFFFFFFFF80000000ULL);
    for (uint32_t i = 0; i < cinux::arch::PAGE_SIZE; ++i) {
        cmd_list_virt[i] = 0;
    }

    // Map the command list page so the kernel can access it
    uint64_t cmd_list_virt_addr = MMIO_VIRT_BASE + 0x10000
        + port_index * cinux::arch::PAGE_SIZE;
    constexpr uint64_t cmd_flags = cinux::arch::FLAG_PRESENT
                                 | cinux::arch::FLAG_WRITABLE;
    cinux::mm::g_vmm.map(cmd_list_virt_addr, cmd_list_phys, cmd_flags);

    // Allocate FIS receive buffer: 256 bytes, 256-byte aligned
    // A full page gives us the alignment
    uint64_t fis_buf_phys = cinux::mm::g_pmm.alloc_page();
    if (fis_buf_phys == 0) {
        cinux::lib::kprintf("[AHCI] Port %u: failed to alloc FIS buffer\n",
                            port_index);
        return;
    }
    fis_buf_phys_[port_index] = fis_buf_phys;

    // Zero out the FIS buffer
    auto* fis_buf_virt = reinterpret_cast<uint8_t*>(
        fis_buf_phys + 0xFFFFFFFF80000000ULL);
    for (uint32_t i = 0; i < cinux::arch::PAGE_SIZE; ++i) {
        fis_buf_virt[i] = 0;
    }

    // Map the FIS buffer page
    uint64_t fis_buf_virt_addr = MMIO_VIRT_BASE + 0x20000
        + port_index * cinux::arch::PAGE_SIZE;
    cinux::mm::g_vmm.map(fis_buf_virt_addr, fis_buf_phys, cmd_flags);

    // Write port registers
    port->clb  = static_cast<uint32_t>(cmd_list_phys & 0xFFFFFFFF);
    port->clbu = static_cast<uint32_t>(cmd_list_phys >> 32);
    port->fb   = static_cast<uint32_t>(fis_buf_phys & 0xFFFFFFFF);
    port->fbu  = static_cast<uint32_t>(fis_buf_phys >> 32);

    // Clear interrupt status
    port->is = static_cast<uint32_t>(~0U);
    // Enable interrupts
    port->ie = PxIs::DHRS | PxIs::PSS | PxIs::DSS | PxIs::SDBS;

    // Now allocate a command table for slot 0 inside the command list page.
    // We place it at offset 1024 within the same page (after the 32 headers).
    uint64_t cmd_tbl_phys = cmd_list_phys + CMD_SLOTS * sizeof(HBACommandHeader);

    // Zero the command table area
    auto* cmd_tbl_raw = reinterpret_cast<uint8_t*>(
        cmd_tbl_phys + 0xFFFFFFFF80000000ULL);
    for (uint32_t i = 0; i < CMD_TABLE_TOTAL; ++i) {
        cmd_tbl_raw[i] = 0;
    }

    // Set up command header for slot 0 to point to the command table
    auto* headers = reinterpret_cast<HBACommandHeader*>(
        cmd_list_phys + 0xFFFFFFFF80000000ULL);
    headers[0].ctba  = static_cast<uint32_t>(cmd_tbl_phys & 0xFFFFFFFF);
    headers[0].ctbau = static_cast<uint32_t>(cmd_tbl_phys >> 32);

    // Start the port engine
    start_port(port);

    cinux::lib::kprintf("[AHCI] Port %u set up: cmdlist=0x%p fis=0x%p\n",
                        port_index, cmd_list_phys, fis_buf_phys);
}

void AHCI::build_cfis(HBACommandTable* cmd_tbl, bool write_cmd,
                      uint64_t lba, uint16_t count) {
    // Build the Register H2D FIS in the first 20 bytes of cfis[]
    auto* fis = reinterpret_cast<RegH2DFIS*>(cmd_tbl->cfis);

    fis->fis_type = FisType::REG_H2D;
    fis->flags    = 0x80;  // Bit 6 set = command, bit 7 = reserved (set to 1 by convention)
    fis->command  = write_cmd ? AtaCmd::WRITE_DMA_EXT
                              : AtaCmd::READ_DMA_EXT;
    fis->feature  = 0;     // Features (low 8 bits)

    // 48-bit LBA: LBA0-LBA2 hold low 24 bits, LBA3-LBA5 hold upper 24 bits
    fis->lba0 = static_cast<uint8_t>(lba & 0xFF);
    fis->lba1 = static_cast<uint8_t>((lba >> 8) & 0xFF);
    fis->lba2 = static_cast<uint8_t>((lba >> 16) & 0xFF);
    fis->device = 0x40;  // LBA mode bit (bit 6) set

    fis->lba3 = static_cast<uint8_t>((lba >> 24) & 0xFF);
    fis->lba4 = static_cast<uint8_t>((lba >> 32) & 0xFF);
    fis->lba5 = static_cast<uint8_t>((lba >> 40) & 0xFF);
    fis->feature_exp = 0;

    // Sector count (16-bit, split into low and high byte)
    fis->count0 = static_cast<uint8_t>(count & 0xFF);
    fis->count1 = static_cast<uint8_t>((count >> 8) & 0xFF);

    fis->control = 0;
}

bool AHCI::execute_command(uint8_t port_index, uint8_t slot,
                           bool write_cmd, uint64_t lba,
                           uint16_t count, uint64_t buf_phys) {
    auto* port = &hba_mem_->ports[port_index];

    // Get command list (we stored the physical address; convert to virt)
    uint64_t cmd_list_virt = cmd_list_phys_[port_index]
        + 0xFFFFFFFF80000000ULL;
    auto* headers = reinterpret_cast<HBACommandHeader*>(cmd_list_virt);

    // Get command table for this slot (placed after 32 headers)
    uint64_t cmd_tbl_phys = cmd_list_phys_[port_index]
        + slot * CMD_TABLE_TOTAL;

    // If using slot > 0, we would need separate allocation.
    // For simplicity we reuse slot 0's command table for single-command ops.
    // For slot 0, cmd_tbl_phys = cmd_list_phys + 32 * 32 = cmd_list_phys + 0x400
    cmd_tbl_phys = cmd_list_phys_[port_index] + CMD_SLOTS * sizeof(HBACommandHeader);

    auto* cmd_tbl_phys_ptr = reinterpret_cast<uint8_t*>(
        cmd_tbl_phys + 0xFFFFFFFF80000000ULL);
    auto* cmd_tbl = reinterpret_cast<HBACommandTable*>(cmd_tbl_phys_ptr);

    // Zero the command table
    for (uint32_t i = 0; i < CMD_TABLE_TOTAL; ++i) {
        reinterpret_cast<uint8_t*>(cmd_tbl)[i] = 0;
    }

    // Build the Command FIS
    build_cfis(cmd_tbl, write_cmd, lba, count);

    // Set up the PRDT (single entry for contiguous buffer)
    uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
    cmd_tbl->prdt[0].dba  = static_cast<uint32_t>(buf_phys & 0xFFFFFFFF);
    cmd_tbl->prdt[0].dbau = static_cast<uint32_t>(buf_phys >> 32);
    cmd_tbl->prdt[0].dbc  = byte_count & 0x3FFFFF;  // 22-bit max
    cmd_tbl->prdt[0].i    = 1;  // Interrupt on completion

    // Configure the command header
    headers[slot].cfl    = sizeof(RegH2DFIS) / 4;  // FIS length in dwords
    headers[slot].prdtl  = 1;   // One PRDT entry
    headers[slot].write  = write_cmd ? 1 : 0;
    headers[slot].ctba   = static_cast<uint32_t>(cmd_tbl_phys & 0xFFFFFFFF);
    headers[slot].ctbau  = static_cast<uint32_t>(cmd_tbl_phys >> 32);
    headers[slot].prdbc  = 0;

    // Clear pending interrupt status
    port->is = static_cast<uint32_t>(~0U);

    // Issue the command by setting the bit in CI (Command Issue)
    port->ci = (1U << slot);

    // Poll for completion: wait for CI bit to clear and IS.DHRS to set
    for (uint32_t i = 0; i < POLL_TIMEOUT; ++i) {
        // Command completed when CI bit clears
        if ((port->ci & (1U << slot)) == 0) {
            // Check task file data for errors (TFD.ERR should be 0)
            uint32_t tfd = port->tfd;
            if ((tfd & 0x01) != 0) {
                cinux::lib::kprintf("[AHCI] Port %u: command error TFD=0x%x\n",
                                    port_index, tfd);
                return false;
            }
            return true;
        }
        __asm__ volatile("pause");
    }

    cinux::lib::kprintf("[AHCI] Port %u: command timeout\n", port_index);
    return false;
}

// ============================================================
// Public interface
// ============================================================

void AHCI::init(const pci::PCIDevice& dev) {
    cinux::lib::kprintf("[AHCI] Initialising AHCI controller...\n");

    // Step 1: Enable PCI bus master and memory space access
    uint32_t cmd_reg = pci::PCI::pci_read(dev.bus, dev.slot, dev.func,
                                          pci::PciReg::COMMAND);
    cmd_reg |= (1U << 1);   // Bus Master Enable
    cmd_reg |= (1U << 2);   // Memory Space Enable
    pci::PCI::pci_write(dev.bus, dev.slot, dev.func,
                        pci::PciReg::COMMAND, cmd_reg);

    // Step 2: Map BAR5 MMIO
    uint64_t bar5 = dev.bar[5];
    if (bar5 == 0) {
        cinux::lib::kprintf("[AHCI] BAR5 is null, cannot initialise.\n");
        return;
    }

    hba_mem_ = map_bar5(bar5);
    if (hba_mem_ == nullptr) {
        cinux::lib::kprintf("[AHCI] Failed to map BAR5.\n");
        return;
    }

    cinux::lib::kprintf("[AHCI] BAR5 mapped, PI=0x%x CAP=0x%x VER=0x%x\n",
                        hba_mem_->pi, hba_mem_->cap, hba_mem_->vs);

    // Step 3: Enable AHCI mode
    hba_mem_->ghc |= GhcBits::AE;

    // Step 4: Reset the HBA
    reset_hba();

    // Re-enable AHCI mode after reset
    hba_mem_->ghc |= GhcBits::AE;

    // Step 5: Enable interrupts globally
    hba_mem_->ghc |= GhcBits::INT_ENABLE;

    // Step 6: Probe ports indicated by the PI bitmap
    uint32_t pi = hba_mem_->pi;
    uint32_t active_count = 0;

    for (uint8_t i = 0; i < MAX_PORTS; ++i) {
        if ((pi & (1U << i)) == 0) {
            continue;
        }

        auto* port = &hba_mem_->ports[i];

        // Check SATA Status: DET field == 0x03 means device present and active
        uint32_t ssts = port->ssts;
        uint32_t det = ssts & PxSsts::DET_MASK;

        cinux::lib::kprintf("[AHCI] Port %u: SSTS=0x%x DET=%u SIG=0x%x\n",
                            i, ssts, det, port->sig);

        if (det != PxSsts::DET_ACTIVE) {
            cinux::lib::kprintf("[AHCI] Port %u: no device detected, skipping.\n", i);
            continue;
        }

        // Device is present -- set up the port
        setup_port(i);
        ++active_count;
    }

    cinux::lib::kprintf("[AHCI] %u active ports initialised.\n", active_count);
}

bool AHCI::read(uint8_t port_index, uint64_t lba, uint16_t count,
                uint64_t buf) {
    if (hba_mem_ == nullptr || port_index >= MAX_PORTS) {
        return false;
    }

    if (cmd_list_phys_[port_index] == 0) {
        cinux::lib::kprintf("[AHCI] Port %u not initialised.\n", port_index);
        return false;
    }

    return execute_command(port_index, 0, false, lba, count, buf);
}

bool AHCI::write(uint8_t port_index, uint64_t lba, uint16_t count,
                 uint64_t buf) {
    if (hba_mem_ == nullptr || port_index >= MAX_PORTS) {
        return false;
    }

    if (cmd_list_phys_[port_index] == 0) {
        cinux::lib::kprintf("[AHCI] Port %u not initialised.\n", port_index);
        return false;
    }

    return execute_command(port_index, 0, true, lba, count, buf);
}

HBAMem* AHCI::hba_mem() const {
    return hba_mem_;
}

}  // namespace cinux::drivers::ahci
