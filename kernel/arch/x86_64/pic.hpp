/**
 * @file kernel/arch/x86_64/pic.hpp
 * @brief 8259A Programmable Interrupt Controller driver
 *
 * Encapsulates PIC remapping, masking, and End-Of-Interrupt signalling
 * in a class for clean encapsulation and future extensibility (e.g. APIC
 * migration).
 *
 * Design decisions:
 *   - Master PIC: IRQ0-7 remapped to INT 0x20-0x27
 *   - Slave  PIC: IRQ8-15 remapped to INT 0x28-0x2F
 *   - Both PICs are initialised in 8086 mode with 4 ICW words
 *   - Auto-EOI is NOT used; each handler must call send_eoi() explicitly
 *
 * Dependencies: Requires io_outb() / io_inb() / io_wait() from io.hpp.
 * Must be called AFTER GDT and IDT are initialised (IRQ handlers need
 * valid IDT entries).
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

// ============================================================
// PIC I/O Port Constants
// ============================================================

namespace PicPort {
constexpr uint16_t MASTER_CMD  = 0x20;  ///< Master PIC command / status
constexpr uint16_t MASTER_DATA = 0x21;  ///< Master PIC data (mask / ICW2-4)
constexpr uint16_t SLAVE_CMD   = 0xA0;  ///< Slave PIC command / status
constexpr uint16_t SLAVE_DATA  = 0xA1;  ///< Slave PIC data (mask / ICW2-4)
}  // namespace PicPort

// ============================================================
// PIC ICW Constants
// ============================================================

namespace PicICW {
constexpr uint8_t ICW1_ICW4      = 0x01;  ///< ICW4 needed
constexpr uint8_t ICW1_SINGLE    = 0x02;  ///< Single (cascade) mode
constexpr uint8_t ICW1_INTERVAL4 = 0x04;  ///< Call address interval 4 (8086)
constexpr uint8_t ICW1_LEVEL     = 0x08;  ///< Level triggered (edge) mode
constexpr uint8_t ICW1_INIT      = 0x10;  ///< Initialization

constexpr uint8_t ICW4_8086       = 0x01;  ///< 8086/88 (MCS-80/85) mode
constexpr uint8_t ICW4_AUTO_EOI   = 0x02;  ///< Auto End-Of-Interrupt
constexpr uint8_t ICW4_BUF_MASTER = 0x04;  ///< Buffered mode master
constexpr uint8_t ICW4_BUF_SLAVE  = 0x00;  ///< Buffered mode slave
constexpr uint8_t ICW4_SFNM       = 0x10;  ///< Special Fully Nested Mode
}  // namespace PicICW

// ============================================================
// PIC Class
// ============================================================

/**
 * @brief 8259A Programmable Interrupt Controller driver
 *
 * Provides PIC initialisation with configurable vector offsets,
 * per-IRQ mask/unmask, global disable, and EOI signalling.
 * All methods are static because there is exactly one pair of
 * PIC chips in the system.
 */
class PIC {
public:
    /**
     * @brief Initialise and remap both 8259A PICs
     *
     * Sends ICW1-ICW4 to both master and slave PICs to:
     *   - Remap master IRQ0-7 to master_offset..master_offset+7
     *   - Remap slave  IRQ8-15 to slave_offset..slave_offset+7
     *   - Configure cascade (slave on master IRQ2)
     *   - Set 8086 mode, manual EOI
     *
     * After init, all IRQs are masked (disabled).  Call unmask()
     * for the specific IRQs you need before enabling interrupts.
     *
     * @param master_offset  INT vector base for master PIC (default: 0x20)
     * @param slave_offset   INT vector base for slave PIC (default: 0x28)
     *
     * @note Must be called before any IRQ unmask / EOI operations.
     * @note Uses io_wait() between I/O writes for ISA timing.
     */
    static void init(uint8_t master_offset = 0x20, uint8_t slave_offset = 0x28);

    /**
     * @brief Send End-Of-Interrupt to the PIC(s)
     *
     * Must be called at the end of every IRQ handler before IRETQ.
     * For IRQs on the slave PIC (IRQ8-15), EOI is sent to both
     * slave and master PICs.
     *
     * @param irq  The hardware IRQ number (0-15), NOT the INT vector
     *
     * @note Sending EOI for an unhandled IRQ will cause spurious
     *       interrupts.  Always match EOI with the actual IRQ.
     */
    static void send_eoi(uint8_t irq);

    /**
     * @brief Mask (disable) a specific IRQ line
     *
     * Sets the corresponding bit in the PIC's Interrupt Mask Register
     * (IMR).  The IRQ will not be forwarded to the CPU until unmasked.
     *
     * @param irq  The hardware IRQ number (0-15)
     */
    static void mask(uint8_t irq);

    /**
     * @brief Unmask (enable) a specific IRQ line
     *
     * Clears the corresponding bit in the PIC's IMR.  The IRQ will
     * be forwarded to the CPU (provided IF is set via sti).
     *
     * @param irq  The hardware IRQ number (0-15)
     */
    static void unmask(uint8_t irq);

    /**
     * @brief Mask all IRQ lines on both PICs
     *
     * Writes 0xFF to both master and slave data registers.
     * Typically called during early init or when shutting down
     * interrupt handling.
     */
    static void disable_all();

    /**
     * @brief Get the master PIC vector offset
     * @return The INT vector base for master IRQs
     */
    static uint8_t master_offset();

    /**
     * @brief Get the slave PIC vector offset
     * @return The INT vector base for slave IRQs
     */
    static uint8_t slave_offset();

private:
    /// Stored offsets (set by init, read by send_eoi / helpers)
    static uint8_t master_offset_;
    static uint8_t slave_offset_;
};

}  // namespace cinux::arch
