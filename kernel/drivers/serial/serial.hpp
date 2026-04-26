/**
 * @file kernel/drivers/serial/serial.hpp
 * @brief Serial port (UART 16550) driver for the big kernel
 *
 * Polling-based serial I/O driver.  No interrupt-driven receive --
 * we simply spin-wait on the Line Status Register (LSR) before
 * each transmit or receive.
 *
 * Default configuration: 115200 baud, 8 data bits, no parity, 1 stop
 * bit (8N1).  QEMU's virtual UART works with these defaults.
 *
 * Usage:
 *   Serial com1(SERIAL_COM1);
 *   com1.init();
 *   com1.puts("Hello from big kernel!\n");
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"

namespace cinux::drivers {

// ============================================================
// Serial Port Base Addresses (x86 standard)
// ============================================================
constexpr uint16_t SERIAL_COM1 = 0x03F8;
constexpr uint16_t SERIAL_COM2 = 0x02F8;
constexpr uint16_t SERIAL_COM3 = 0x03E8;
constexpr uint16_t SERIAL_COM4 = 0x2E8;

// ============================================================
// UART Register Offsets (from base port)
// ============================================================
namespace SerialReg {
constexpr uint8_t RBR = 0;  ///< Receive Buffer Register  (read)
constexpr uint8_t THR = 0;  ///< Transmit Holding Register (write)
constexpr uint8_t IER = 1;  ///< Interrupt Enable Register
constexpr uint8_t FCR = 2;  ///< FIFO Control Register
constexpr uint8_t LCR = 3;  ///< Line Control Register
constexpr uint8_t MCR = 4;  ///< Modem Control Register
constexpr uint8_t LSR = 5;  ///< Line Status Register
constexpr uint8_t MSR = 6;  ///< Modem Status Register
constexpr uint8_t SCR = 7;  ///< Scratch Register
}  // namespace SerialReg

// ============================================================
// Line Status Register Bits
// ============================================================
namespace SerialLSR {
constexpr uint8_t RX_READY = 0x01;  ///< Data available in RBR
constexpr uint8_t TX_READY = 0x20;  ///< THR empty, safe to write
}  // namespace SerialLSR

// ============================================================
// Serial Class
// ============================================================

/**
 * @brief UART 16550 serial port driver (polling mode)
 *
 * Wraps a single COM port.  The caller constructs with a base port
 * address and calls init() before any I/O.
 */
class Serial {
public:
    /**
     * @brief Construct a Serial wrapper for the given port
     *
     * @param port  Base I/O port (default: COM1 = 0x3F8)
     *
     * @note Does NOT configure the hardware -- call init() separately.
     */
    explicit Serial(uint16_t port = SERIAL_COM1);

    /**
     * @brief Initialise the UART to 115200 8N1
     *
     * Disables interrupts, sets line control (8N1), enables FIFO,
     * and sets MCR (RTS + DTR).
     *
     * @param port  (unused in current design -- set at construction)
     * @param baud  (unused -- QEMU defaults to 115200)
     */
    void init(uint16_t port = SERIAL_COM1, uint32_t baud = 115200);

    /**
     * @brief Write a single character (blocking poll on TX ready)
     *
     * @param c  The character to transmit
     */
    void putc(char c);

    /**
     * @brief Write a null-terminated string
     *
     * Converts '\\n' to "\\r\\n" automatically.
     *
     * @param s  The string to write (nullptr is a no-op)
     */
    void puts(const char* s);

    /**
     * @brief Check whether the UART is ready to accept a new byte
     *
     * @return true if the transmit holding register is empty
     */
    bool is_ready() const;

private:
    /// Base I/O port for this UART instance
    uint16_t base_port_;

    /**
     * @brief Check TX ready (internal helper)
     * @return true if THR is empty
     */
    bool is_tx_ready() const;
};

}  // namespace cinux::drivers
