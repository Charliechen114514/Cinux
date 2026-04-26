/**
 * @file kernel/arch/x86_64/pic.cpp
 * @brief 8259A PIC driver implementation
 *
 * Implements PIC initialisation (ICW1-ICW4 sequence with io_wait()),
 * mask/unmask via IMR read-modify-write, and EOI signalling.
 *
 * The 8259A datasheet requires a delay between consecutive I/O writes
 * to the same PIC chip.  We use io_wait() (port 0x80 write) which
 * provides ~1 us delay, sufficient for ISA timing requirements.
 */

#include "kernel/arch/x86_64/pic.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"

using cinux::io::io_inb;
using cinux::io::io_outb;
using cinux::io::io_wait;

namespace cinux::arch {

// ============================================================
// Static storage for PIC offsets
// ============================================================

uint8_t PIC::master_offset_ = 0x20;
uint8_t PIC::slave_offset_  = 0x28;

// ============================================================
// PIC::init() -- full ICW1-ICW4 initialisation sequence
// ============================================================

void PIC::init(uint8_t master_offset, uint8_t slave_offset) {
    // Store offsets for later use in send_eoi()
    master_offset_ = master_offset;
    slave_offset_  = slave_offset;

    // Save current masks so we can restore after init
    uint8_t master_mask = io_inb(PicPort::MASTER_DATA);
    uint8_t slave_mask  = io_inb(PicPort::SLAVE_DATA);

    // ICW1: start initialisation in cascade mode, ICW4 needed
    io_outb(PicPort::MASTER_CMD, PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();
    io_outb(PicPort::SLAVE_CMD, PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();

    // ICW2: vector offsets
    io_outb(PicPort::MASTER_DATA, master_offset);
    io_wait();
    io_outb(PicPort::SLAVE_DATA, slave_offset);
    io_wait();

    // ICW3: cascade wiring -- master has slave on IRQ2 (bit 2),
    //        slave reports its cascade identity as 2
    io_outb(PicPort::MASTER_DATA, 0x04);
    io_wait();
    io_outb(PicPort::SLAVE_DATA, 0x02);
    io_wait();

    // ICW4: 8086 mode, no auto-EOI, no buffered mode, no SFNM
    io_outb(PicPort::MASTER_DATA, PicICW::ICW4_8086);
    io_wait();
    io_outb(PicPort::SLAVE_DATA, PicICW::ICW4_8086);
    io_wait();

    // Restore saved masks (keeps previous masking state)
    io_outb(PicPort::MASTER_DATA, master_mask);
    io_outb(PicPort::SLAVE_DATA, slave_mask);
}

// ============================================================
// PIC::send_eoi() -- signal end of interrupt
// ============================================================

void PIC::send_eoi(uint8_t irq) {
    // If the IRQ came from the slave PIC (IRQ8-15), we must send
    // EOI to both slave and master, because the master received
    // the interrupt via its IRQ2 cascade line.
    if (irq >= 8) {
        io_outb(PicPort::SLAVE_CMD, 0x20);
    }

    // Always send EOI to master PIC
    io_outb(PicPort::MASTER_CMD, 0x20);
}

// ============================================================
// PIC::mask() -- disable a specific IRQ
// ============================================================

void PIC::mask(uint8_t irq) {
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        // Master PIC: read current mask, set bit, write back
        port  = PicPort::MASTER_DATA;
        value = io_inb(port) | (1u << irq);
    } else {
        // Slave PIC: subtract 8 to get bit position
        port  = PicPort::SLAVE_DATA;
        value = io_inb(port) | (1u << (irq - 8));
    }

    io_outb(port, value);
}

// ============================================================
// PIC::unmask() -- enable a specific IRQ
// ============================================================

void PIC::unmask(uint8_t irq) {
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        // Master PIC: read current mask, clear bit, write back
        port  = PicPort::MASTER_DATA;
        value = io_inb(port) & ~(1u << irq);
    } else {
        // Slave PIC: subtract 8 to get bit position
        port  = PicPort::SLAVE_DATA;
        value = io_inb(port) & ~(1u << (irq - 8));
    }

    io_outb(port, value);
}

// ============================================================
// PIC::disable_all() -- mask all IRQs on both PICs
// ============================================================

void PIC::disable_all() {
    io_outb(PicPort::MASTER_DATA, 0xFF);
    io_outb(PicPort::SLAVE_DATA, 0xFF);
}

// ============================================================
// Accessors
// ============================================================

uint8_t PIC::master_offset() {
    return master_offset_;
}

uint8_t PIC::slave_offset() {
    return slave_offset_;
}

}  // namespace cinux::arch
