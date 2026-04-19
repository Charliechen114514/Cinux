/**
 * @file kernel/arch/x86_64/io.hpp
 * @brief x86 I/O port access primitives (inline assembly)
 *
 * Provides thin wrappers around the `in` and `out` instructions for
 * byte, word, and dword I/O, plus an io_wait() for ISA timing.
 *
 * All functions use inline assembly with "memory" clobber to act as
 * compiler barriers -- the I/O instruction is a synchronising operation
 * and we do not want the compiler to reorder memory accesses across it.
 *
 * Namespace: cinux::io
 */

#pragma once

#include <stdint.h>

namespace cinux::io {

// ============================================================
// Byte I/O (8-bit)
// ============================================================

/**
 * @brief Read a byte from an I/O port
 *
 * @param port  The 16-bit I/O port number
 * @return      The byte value read from the port
 */
inline uint8_t io_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
}

/**
 * @brief Write a byte to an I/O port
 *
 * @param port   The 16-bit I/O port number
 * @param value  The byte value to write
 */
inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
}

// ============================================================
// Word I/O (16-bit)
// ============================================================

/**
 * @brief Read a 16-bit word from an I/O port
 *
 * @param port  The 16-bit I/O port number
 * @return      The 16-bit value read
 */
inline uint16_t io_inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile("inw %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
}

/**
 * @brief Write a 16-bit word to an I/O port
 *
 * @param port   The 16-bit I/O port number
 * @param value  The 16-bit value to write
 */
inline void io_outw(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
}

// ============================================================
// Dword I/O (32-bit)
// ============================================================

/**
 * @brief Read a 32-bit dword from an I/O port
 *
 * @param port  The 16-bit I/O port number
 * @return      The 32-bit value read
 */
inline uint32_t io_inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
}

/**
 * @brief Write a 32-bit dword to an I/O port
 *
 * @param port   The 16-bit I/O port number
 * @param value  The 32-bit value to write
 */
inline void io_outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
}

// ============================================================
// I/O Delay
// ============================================================

/**
 * @brief Short delay via I/O port 0x80
 *
 * Writing to port 0x80 (POST diagnostics port) takes ~1 us on
 * typical hardware, which is enough to satisfy ISA timing
 * requirements after certain I/O operations (e.g. PIT / PIC).
 */
inline void io_wait() {
    io_outb(0x80, 0);
}

}  // namespace cinux::io
