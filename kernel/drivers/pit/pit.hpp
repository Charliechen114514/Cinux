/**
 * @file kernel/drivers/pit/pit.hpp
 * @brief Programmable Interval Timer (Intel 8254) driver
 *
 * Configures PIT channel 0 to generate periodic interrupts at a
 * configurable frequency.  The IRQ0 handler tracks a global tick
 * counter and periodically prints uptime information to serial.
 *
 * Hardware details:
 *   - Base clock: 1193182 Hz (roughly 1.193182 MHz)
 *   - Channel 0: I/O port 0x40, tied to IRQ0
 *   - Channel 1: I/O port 0x41 (RAM refresh, do not touch)
 *   - Channel 2: I/O port 0x42 (PC speaker)
 *   - Command register: I/O port 0x43
 *
 * Dependencies:
 *   - PIC must be initialised and IRQ0 unmasked before ticks arrive
 *   - IDT must have vector 0x20 (IRQ0) registered
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>
#include <atomic>

// Forward declaration -- InterruptFrame is defined in idt.hpp
namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers {

// ============================================================
// PIT Hardware Constants
// ============================================================

namespace PitHW {
constexpr uint16_t CHANNEL_0    = 0x40;  ///< Channel 0 data port
constexpr uint16_t CHANNEL_1    = 0x41;  ///< Channel 1 data port (unused)
constexpr uint16_t CHANNEL_2    = 0x42;  ///< Channel 2 data port (speaker)
constexpr uint16_t COMMAND      = 0x43;  ///< Mode/Command register

constexpr uint32_t BASE_FREQ    = 1193182;  ///< Input clock frequency (Hz)

// Command byte bits
constexpr uint8_t CMD_BINARY     = 0x00;  ///< Binary counter mode
constexpr uint8_t CMD_BCD        = 0x01;  ///< BCD counter mode
constexpr uint8_t CMD_MODE_0     = 0x00;  ///< Interrupt on terminal count
constexpr uint8_t CMD_MODE_2     = 0x04;  ///< Rate generator
constexpr uint8_t CMD_MODE_3     = 0x06;  ///< Square wave generator
constexpr uint8_t CMD_LATCH      = 0x00;  ///< Latch count value
constexpr uint8_t CMD_LSB_ONLY   = 0x10;  ///< LSB only
constexpr uint8_t CMD_MSB_ONLY   = 0x20;  ///< MSB only
constexpr uint8_t CMD_LSB_MSB    = 0x30;  ///< LSB then MSB
constexpr uint8_t CMD_CHANNEL_0  = 0x00;  ///< Select channel 0
}  // namespace PitHW

// ============================================================
// PIT Class
// ============================================================

/**
 * @brief Intel 8254 PIT driver (channel 0 only)
 *
 * Configures PIT channel 0 as a square-wave generator at a given
 * frequency.  Maintains a global tick counter used for uptime
 * tracking.
 *
 * All methods are static because there is exactly one PIT in the
 * system and the tick counter is a singleton.
 */
class PIT {
public:
    /**
     * @brief Initialise PIT channel 0 with the given frequency
     *
     * Sends command 0x36 (channel 0, LSB-then-MSB, square wave mode)
     * to the command register, then writes the 16-bit divisor as
     * low byte followed by high byte to channel 0's data port.
     *
     * The divisor is calculated as: 1193182 / freq_hz
     * Minimum usable frequency: ~19 Hz (divisor = 65535)
     * Maximum usable frequency: 1193182 Hz (divisor = 1)
     *
     * @param freq_hz  Desired interrupt frequency in Hz (default: 100 Hz,
     *                 yielding 10 ms per tick)
     *
     * @note This only configures the hardware.  The IRQ0 handler must
     *       be registered in the IDT and IRQ0 must be unmasked via PIC
     *       before interrupts will actually arrive.
     */
    static void init(uint32_t freq_hz = 100);

    /**
     * @brief IRQ0 interrupt handler (called from ISR stub)
     *
     * Increments the global tick counter.  Every freq_hz ticks
     * (i.e. once per second), prints "[TICK] uptime: Ns" to serial
     * via kprintf.  Always sends EOI to the PIC before returning.
     *
     * @param frame  Pointer to the interrupt stack frame (unused but
     *               required by the ISR stub calling convention)
     *
     * @note This function is called from assembly (irq0_stub in irq.S)
     *       and has C-linkage.  It must NOT be name-mangled.
     */
    static void irq0_handler(cinux::arch::InterruptFrame* frame);

    /**
     * @brief Get the current tick count
     *
     * Each tick represents 1/freq_hz seconds.
     *
     * @return Number of PIT interrupts since init()
     */
    static uint64_t get_ticks();

    /**
     * @brief Get system uptime in milliseconds
     *
     * Calculated as: (tick_count * 1000) / freq_hz
     *
     * @return Uptime in milliseconds since PIT was initialised
     */
    static uint64_t get_uptime_ms();

    /**
     * @brief Get the configured frequency
     * @return The PIT frequency in Hz
     */
    static uint32_t freq_hz();

private:
    /// Global tick counter, incremented once per IRQ0
    static std::atomic<uint64_t> tick_count_;

    /// Configured frequency (Hz), set by init()
    static uint32_t freq_hz_;
};

}  // namespace cinux::drivers
