/**
 * @file kernel/drivers/serial.cpp
 * @brief Serial port (UART 16550) driver implementation
 */

#include "kernel/drivers/serial.hpp"

#include <stdint.h>

// Pull in the I/O primitives (io_inb / io_outb)
#include "kernel/arch/x86_64/io.hpp"

using cinux::io::io_inb;
using cinux::io::io_outb;

namespace cinux::drivers {

// ============================================================
// Constructor
// ============================================================

Serial::Serial(uint16_t port)
    : base_port_(port) {
    // Caller calls init() explicitly after construction.
}

// ============================================================
// init() -- configure the UART
// ============================================================

void Serial::init(uint16_t /*port*/, uint32_t /*baud*/) {
    // Disable interrupts, set 8N1, enable FIFO, set MCR, verify LSR
    io_outb(base_port_ + SerialReg::IER, 0x00);
    io_outb(base_port_ + SerialReg::LCR, 0x03);
    io_outb(base_port_ + SerialReg::FCR, 0xC7);
    io_outb(base_port_ + SerialReg::MCR, 0x03);
    io_inb(base_port_ + SerialReg::LSR);
}

// ============================================================
// is_tx_ready() -- check THR empty
// ============================================================

bool Serial::is_tx_ready() const {
    return (io_inb(base_port_ + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
}

// ============================================================
// is_ready() -- public check for TX readiness
// ============================================================

bool Serial::is_ready() const {
    return is_tx_ready();
}

// ============================================================
// putc() -- write one character (blocking)
// ============================================================

void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }

    io_outb(base_port_ + SerialReg::THR, static_cast<uint8_t>(c));
}

// ============================================================
// puts() -- write a null-terminated string
// ============================================================

void Serial::puts(const char* s) {
    if (s == nullptr) {
        return;
    }

    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}

}  // namespace cinux::drivers
