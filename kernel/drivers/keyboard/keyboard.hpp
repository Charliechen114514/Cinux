/**
 * @file kernel/drivers/keyboard/keyboard.hpp
 * @brief PS/2 keyboard driver (scan code set 1)
 *
 * Provides interrupt-driven keyboard input with modifier tracking and
 * a ring-buffer event queue.  The IRQ1 handler enqueues raw scan codes;
 * the poll interface dequeues fully-decoded KeyEvent structs with ASCII
 * translation and modifier state.
 *
 * Dependencies:
 *   - PIC must be initialised and IRQ1 unmasked before keys arrive
 *   - IDT must have vector 0x21 (IRQ1) registered
 *
 * Usage:
 *   Keyboard::init();
 *   PIC::unmask(1);
 *   // ... enable interrupts ...
 *   KeyEvent ev;
 *   if (Keyboard::poll(ev)) {
 *       // ev.ascii, ev.pressed, ev.shift, etc.
 *   }
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

// Forward declaration -- InterruptFrame is defined in idt.hpp
namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers {

// ============================================================
// Key Event Structure
// ============================================================

/**
 * @brief Decoded keyboard event
 *
 * Contains the ASCII translation (0 if non-printable), the raw scan code,
 * the pressed/released state, and the current modifier key state at the
 * time of the event.
 */
struct KeyEvent {
    char    ascii;     ///< ASCII character (0 if non-printable or key release)
    uint8_t scancode;  ///< Raw scan code set 1 value
    bool    pressed;   ///< true = key press (make), false = key release (break)
    bool    shift;     ///< true if either Shift is held
    bool    ctrl;      ///< true if either Ctrl is held
    bool    alt;       ///< true if either Alt is held
};

// ============================================================
// Keyboard Class
// ============================================================

/**
 * @brief PS/2 keyboard driver with ring buffer and modifier tracking
 *
 * All methods are static because there is exactly one PS/2 keyboard
 * controller in the system.  The IRQ1 handler enqueues events into
 * an internal ring buffer; callers use poll() to dequeue.
 */
class Keyboard {
public:
    /**
     * @brief Initialise the PS/2 keyboard controller
     *
     * Performs the following sequence:
     *   1. Disable both PS/2 device ports
     *   2. Flush the output buffer
     *   3. Read, modify (enable IRQ1), and write config byte
     *   4. Run controller self-test (expect 0x55 response)
     *   5. Re-enable the first PS/2 port (keyboard)
     *   6. Reset the ring buffer and modifier state
     */
    static void init();

    /**
     * @brief IRQ1 interrupt handler (called from ISR stub)
     *
     * Reads the scan code from port 0x60, decodes modifier state,
     * translates to ASCII, and enqueues a KeyEvent into the ring buffer.
     * Sends EOI to the PIC before returning.
     *
     * @param frame  Interrupt stack frame (unused)
     */
    static void irq1_handler(cinux::arch::InterruptFrame* frame);

    /**
     * @brief Dequeue a keyboard event from the ring buffer
     *
     * @param out  Reference to a KeyEvent to fill
     * @return     true if an event was dequeued, false if buffer is empty
     */
    static bool poll(KeyEvent& out);

private:
    static constexpr uint32_t KEY_QUEUE_SIZE = 64;

    static void enqueue(const KeyEvent& ev);

    // Ring buffer storage
    static KeyEvent queue_[KEY_QUEUE_SIZE];
    static uint32_t head_;
    static uint32_t tail_;

    // Modifier tracking state
    static bool shift_held_;
    static bool ctrl_held_;
    static bool alt_held_;
};

}  // namespace cinux::drivers
