/* ==============================================================
 * Cinux Mini Kernel - Serial Port Driver Implementation
 * ============================================================== */

#include "serial.h"

#include <stdint.h>

#include "driver/io.h"

namespace cinux::mini::serial {
// ============================================================
// Constructor - Initialize and configure serial port
// ============================================================
Serial::Serial(uint16_t port) : base_port(port) {
    init();
}

// ============================================================
// Initialize serial port to 115200 8N1 (QEMU default)
// ============================================================
void Serial::init() {
    // Disable interrupts
    io::outb(base_port + SerialReg::IER, 0x00);

    // QEMU defaults to 115200 8N1; set LCR here to ensure the configuration
    // LCR = 0x03: 8 bits, no parity, 1 stop bit
    io::outb(base_port + SerialReg::LCR, 0x03);

    // Enable FIFO, clear buffers, set 14-byte threshold
    io::outb(base_port + SerialReg::FCR, 0xC7);

    // Set Modem Control Register (RTS + DTR)
    io::outb(base_port + SerialReg::MCR, 0x03);

    // Read LSR to verify it's accessible
    io::inb(base_port + SerialReg::LSR);
}

// ============================================================
// Write a single character (blocking poll)
// ============================================================
void Serial::putc(char c) {
    // Wait for transmit buffer to be ready
    uint32_t wait_count = 0;
    while (!is_tx_ready()) {
        // Simple spin-wait
        __asm__ volatile("pause");
        wait_count++;
        if (wait_count > 100000) {
            // Timeout - serial port may be broken
            wait_count = 0;
        }
    }

    // Write character to THR
    io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}

// ============================================================
// Read a single character (blocking poll)
// ============================================================
char Serial::getc() {
    /* Mini kernel! Lets take it easy... */
    while (!is_rx_ready()) {
        // Simple spin-wait
        __asm__ volatile("pause");
    }

    return static_cast<char>(io::inb(base_port + SerialReg::RBR));
}

// ============================================================
// Write a null-terminated string
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

// ============================================================
// Global serial instance (singleton)
// ============================================================
static Serial g_serial(SERIAL_COM1);

// ============================================================
// Get global serial port instance
// ============================================================
Serial& get_initial_serial() {
    return g_serial;
}

}  // namespace cinux::mini::serial
