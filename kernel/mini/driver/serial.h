/* ==============================================================
 * Cinux Mini Kernel - Serial Port Driver
 * ==============================================================
 *
 * Serial (UART) driver for x86_64 using x86 I/O ports.
 * Supports polling-based I/O (no interrupts).
 * Default configuration: 115200 8N1 (QEMU default)
 *
 * Hardware: NS16550A / 8250 UART compatible
 * I/O Ports:
 *   COM1: 0x3F8 (base) - 0x3FF
 *   COM2: 0x2F8 (base) - 0x2FF
 *   COM3: 0x3E8 (base) - 0x3EF
 *   COM4: 0x2E8 (base) - 0x2EF
 *
 * Register Offsets (from base):
 *   0: RBR (read) / THR (write) - Receive/Transmit buffer
 *   1: IER - Interrupt Enable (we disable interrupts)
 *   2: FCR - FIFO Control (optional)
 *   3: LCR - Line Control (8N1 = 0x03)
 *   4: MCR - Modem Control (RTS/DTR)
 *   5: LSR - Line Status (bit 0 = RX ready, bit 5 = TX ready)
 *   6: MSR - Modem Status
 *   7: SCR - Scratch
 */

#pragma once

#include <stdint.h>

#include "io.h"

namespace cinux::mini::serial {
// ============================================================
// Serial Port Base Addresses (x86 standard)
// ============================================================
constexpr uint16_t SERIAL_COM1 = 0x03F8;
constexpr uint16_t SERIAL_COM2 = 0x02F8;
constexpr uint16_t SERIAL_COM3 = 0x03E8;
constexpr uint16_t SERIAL_COM4 = 0x02E8;

// ============================================================
// UART Register Offsets
// ============================================================
namespace SerialReg {
constexpr uint8_t RBR = 0;  // Receive Buffer Register (read)
constexpr uint8_t THR = 0;  // Transmit Holding Register (write)
constexpr uint8_t IER = 1;  // Interrupt Enable Register
constexpr uint8_t FCR = 2;  // FIFO Control Register
constexpr uint8_t LCR = 3;  // Line Control Register
constexpr uint8_t MCR = 4;  // Modem Control Register
constexpr uint8_t LSR = 5;  // Line Status Register
constexpr uint8_t MSR = 6;  // Modem Status Register
constexpr uint8_t SCR = 7;  // Scratch Register
}  // namespace SerialReg

// ============================================================
// Line Status Register Bits
// ============================================================
namespace SerialLSR {
constexpr uint8_t RX_READY = 0x01;  // Data available in RBR
constexpr uint8_t TX_READY = 0x20;  // THR empty (ready to transmit)
}  // namespace SerialLSR

// ============================================================
// Serial Class
// ============================================================
class Serial {
private:
    uint16_t base_port;  // Base I/O port (e.g., 0x3F8 for COM1)

    // Check if transmit buffer is empty (ready to send)
    bool is_tx_ready() const {
        return (io::inb(base_port + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
    }

    // Check if receive buffer has data (ready to read)
    bool is_rx_ready() const {
        return (io::inb(base_port + SerialReg::LSR) & SerialLSR::RX_READY) != 0;
    }

public:
    // Constructor - initializes the serial port
    explicit Serial(uint16_t port = SERIAL_COM1);

    // Write a single character (blocking poll)
    void putc(char c);

    // Read a single character (blocking poll)
    char getc();

    // Check if character is available (non-blocking)
    bool has_data() const { return is_rx_ready(); }

    // Write a null-terminated string
    void puts(const char* s);

    // Initialize the serial port to 115200 8N1 (QEMU default)
    void init();
};

Serial& get_initial_serial();

}  // namespace cinux::mini::serial
