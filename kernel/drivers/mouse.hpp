/**
 * @file kernel/drivers/mouse.hpp
 * @brief PS/2 mouse driver (IRQ12)
 *
 * Provides interrupt-driven mouse input via the PS/2 controller.
 * The driver initialises the auxiliary device port, configures
 * IRQ12, and parses the standard 3-byte mouse packet format.
 *
 * The poll() interface returns MouseEvent structs containing the
 * absolute cursor position (clamped to screen bounds) and button
 * state.  Internally the driver also pushes events into the
 * GUI EventQueue so the window manager can consume them.
 *
 * This driver is only compiled when CINUX_GUI is defined.
 *
 * Dependencies:
 *   - PIC must be initialised and IRQ12 unmasked after init()
 *   - IDT must have vector 0x2C (IRQ12) registered
 *   - irq_handlers.cpp must route irq12_stub to mouse_irq12_handler
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include "kernel/gui/event.hpp"

// Forward declaration -- InterruptFrame is defined in idt.hpp
namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers {

// ============================================================
// Mouse class
// ============================================================

/**
 * @brief PS/2 mouse driver with IRQ12 interrupt handling
 *
 * All methods are static because there is exactly one PS/2 mouse
 * in the system.  The IRQ12 handler accumulates 3-byte packets,
 * updates the internal cursor position, and enqueues MouseEvent
 * structs into the GUI EventQueue.
 */
class Mouse {
public:
    /**
     * @brief Initialise the PS/2 mouse via the 8042 controller
     *
     * Sequence:
     *   1. Send CMD 0xA8 -- enable auxiliary device (mouse port)
     *   2. Read config (CMD 0x20), set bit 1 (IRQ12 enable), write back (CMD 0x60)
     *   3. Send CMD 0xD4 + 0xF4 -- activate the mouse (enable streaming)
     *
     * @note Caller must still call PIC::unmask(12) after this.
     */
    static void init();

    /**
     * @brief IRQ12 interrupt handler (called from ISR stub)
     *
     * Reads one byte from port 0x60 and accumulates it into a
     * 3-byte packet buffer.  Once a complete packet is received,
     * it is decoded into a MouseEvent and enqueued into the
     * global EventQueue.  Sends EOI to the PIC before returning.
     *
     * @param frame  Interrupt stack frame (unused)
     */
    static void irq12_handler(cinux::arch::InterruptFrame* frame);

    /**
     * @brief Dequeue a mouse event from the global EventQueue
     *
     * @param out  Reference to a MouseEvent to fill
     * @return     true if a mouse event was dequeued, false if empty
     */
    static bool poll(cinux::gui::MouseEvent& out);

    /**
     * @brief Get the current absolute cursor X position
     * @return Cursor X in pixels (0 .. screen_width - 1)
     */
    static int32_t x();

    /**
     * @brief Get the current absolute cursor Y position
     * @return Cursor Y in pixels (0 .. screen_height - 1)
     */
    static int32_t y();

    /**
     * @brief Set the screen dimensions used for cursor clamping
     *
     * @param width   Screen width in pixels
     * @param height  Screen height in pixels
     */
    static void set_screen_bounds(uint32_t width, uint32_t height);

    /**
     * @brief Get a reference to the global GUI event queue
     *
     * The window manager polls this queue for all input events
     * (mouse and keyboard).
     *
     * @return Reference to the shared EventQueue
     */
    static cinux::gui::EventQueue& event_queue();

private:
    /**
     * @brief Accumulate a byte from the mouse data port
     *
     * Builds up a 3-byte packet (byte0 = buttons + flags,
     * byte1 = dx, byte2 = dy).  When a full packet is assembled,
     * calls decode_packet().
     *
     * @param byte  One byte read from port 0x60
     */
    static void process_byte(uint8_t byte);

    /**
     * @brief Decode a complete 3-byte mouse packet
     *
     * Extracts dx, dy, button state, updates the absolute cursor
     * position (clamped to screen bounds), and enqueues the
     * appropriate MouseEvent(s) into the EventQueue.
     *
     * @param b0  Packet byte 0 (buttons + overflow flags)
     * @param b1  Packet byte 1 (delta X, 9-bit signed)
     * @param b2  Packet byte 2 (delta Y, 9-bit signed)
     */
    static void decode_packet(uint8_t b0, uint8_t b1, uint8_t b2);

    // Packet assembly state
    static constexpr uint8_t PACKET_SIZE = 3;

    static uint8_t  packet_[PACKET_SIZE];
    static uint8_t  packet_idx_;

    // Cursor state
    static int32_t  mouse_x_;
    static int32_t  mouse_y_;
    static uint8_t  buttons_;

    // Screen bounds for clamping
    static uint32_t screen_width_;
    static uint32_t screen_height_;

    // Previous button state for edge detection (press/release)
    static uint8_t  prev_buttons_;

    // Global event queue
    static cinux::gui::EventQueue g_event_queue_;
};

}  // namespace cinux::drivers
