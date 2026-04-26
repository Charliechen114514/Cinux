/**
 * @file kernel/drivers/mouse.cpp
 * @brief PS/2 mouse driver implementation
 *
 * Implements 8042 controller initialisation for the auxiliary device,
 * IRQ12 interrupt handling, 3-byte packet parsing, cursor tracking
 * with screen-bound clamping, and event dispatch to the GUI EventQueue.
 */

#include "mouse.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::io::io_inb;
using cinux::io::io_outb;
using cinux::gui::Event;
using cinux::gui::EventType;
using cinux::gui::MouseEvent;
using cinux::lib::kprintf;

namespace cinux::drivers {

// ============================================================
// PS/2 Controller Constants (reused from keyboard, kept local)
// ============================================================

namespace Ps2Port {
constexpr uint16_t DATA    = 0x60;  ///< PS/2 data register (read/write)
constexpr uint16_t STATUS  = 0x64;  ///< PS/2 status register (read)
constexpr uint16_t COMMAND = 0x64;  ///< PS/2 controller command (write)
}  // namespace Ps2Port

namespace Ps2Cmd {
constexpr uint8_t READ_CONFIG  = 0x20;
constexpr uint8_t WRITE_CONFIG = 0x60;
constexpr uint8_t ENABLE_AUX   = 0xA8;  ///< Enable auxiliary device (mouse)
constexpr uint8_t WRITE_AUX    = 0xD4;  ///< Send next byte to auxiliary device
}  // namespace Ps2Cmd

namespace Ps2Status {
constexpr uint8_t OUTPUT_FULL = 0x01;
constexpr uint8_t INPUT_FULL  = 0x02;
}  // namespace Ps2Status

namespace MouseCmd {
constexpr uint8_t ENABLE_STREAMING = 0xF4;  ///< Enable mouse streaming mode
constexpr uint8_t ACK              = 0xFA;  ///< Mouse acknowledgement byte
}  // namespace MouseCmd

// ============================================================
// Mouse packet byte 0 bit masks
// ============================================================

namespace Packet0 {
constexpr uint8_t LEFT_BTN   = 0x01;  ///< Bit 0: left button
constexpr uint8_t RIGHT_BTN  = 0x02;  ///< Bit 1: right button
constexpr uint8_t MIDDLE_BTN = 0x04;  ///< Bit 2: middle button
constexpr uint8_t ALWAYS_1   = 0x08;  ///< Bit 3: always set in standard packets
constexpr uint8_t X_SIGN     = 0x10;  ///< Bit 4: X sign (delta X negative)
constexpr uint8_t Y_SIGN     = 0x20;  ///< Bit 5: Y sign (delta Y negative)
constexpr uint8_t X_OVERFLOW = 0x40;  ///< Bit 6: X overflow
constexpr uint8_t Y_OVERFLOW = 0x80;  ///< Bit 7: Y overflow
}  // namespace Packet0

// ============================================================
// Static storage
// ============================================================

uint8_t Mouse::packet_[PACKET_SIZE] = {};
uint8_t Mouse::packet_idx_          = 0;

int32_t Mouse::mouse_x_ = 0;
int32_t Mouse::mouse_y_ = 0;
uint8_t Mouse::buttons_ = 0;

uint32_t Mouse::screen_width_  = 1024;
uint32_t Mouse::screen_height_ = 768;

uint8_t Mouse::prev_buttons_ = 0;

cinux::gui::EventQueue Mouse::g_event_queue_ = {};

// ============================================================
// Internal helpers
// ============================================================

namespace {

/**
 * @brief Wait until the PS/2 controller input buffer is empty
 *
 * Spins until bit 1 (INPUT_FULL) of the status register is clear.
 */
void wait_input_empty() {
    uint32_t timeout = 100000;
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::INPUT_FULL) != 0) {
        if (--timeout == 0) {
            return;
        }
        __asm__ volatile("pause");
    }
}

/**
 * @brief Wait until the PS/2 controller output buffer is full
 *
 * Spins until bit 0 (OUTPUT_FULL) of the status register is set.
 */
void wait_output_full() {
    uint32_t timeout = 100000;
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::OUTPUT_FULL) == 0) {
        if (--timeout == 0) {
            return;
        }
        __asm__ volatile("pause");
    }
}

}  // anonymous namespace

// ============================================================
// Mouse::init()
// ============================================================

void Mouse::init() {
    // Step 1: Enable auxiliary device port (mouse) via CMD 0xA8
    wait_input_empty();
    io_outb(Ps2Port::COMMAND, Ps2Cmd::ENABLE_AUX);

    // Step 2: Read current config byte (CMD 0x20)
    wait_input_empty();
    io_outb(Ps2Port::COMMAND, Ps2Cmd::READ_CONFIG);
    wait_output_full();
    uint8_t config = io_inb(Ps2Port::DATA);

    // Set bit 1 to enable IRQ12 (mouse interrupt)
    config |= 0x02;

    // Write back the modified config (CMD 0x60)
    wait_input_empty();
    io_outb(Ps2Port::COMMAND, Ps2Cmd::WRITE_CONFIG);
    wait_input_empty();
    io_outb(Ps2Port::DATA, config);

    // Step 3: Send CMD 0xD4 followed by 0xF4 to enable mouse streaming
    wait_input_empty();
    io_outb(Ps2Port::COMMAND, Ps2Cmd::WRITE_AUX);
    wait_input_empty();
    io_outb(Ps2Port::DATA, MouseCmd::ENABLE_STREAMING);

    // Wait for ACK from the mouse
    wait_output_full();
    uint8_t ack = io_inb(Ps2Port::DATA);
    if (ack == MouseCmd::ACK) {
        kprintf("[MOUSE] Mouse enabled (ACK received).\n");
    } else {
        kprintf("[MOUSE] Warning: expected ACK 0xFA, got 0x%02X\n", ack);
    }

    // Reset internal state
    packet_idx_   = 0;
    // PS/2 is delta-only; start at center for good UX
    mouse_x_      = 0;
    mouse_y_      = 0;
    buttons_      = 0;
    prev_buttons_ = 0;

    // Clear any stale events that accumulated between PIC::unmask(12)
    // (in main.cpp) and this init call (in kernel_init_thread).
    g_event_queue_.clear();

    kprintf("[MOUSE] PS/2 mouse driver initialised.\n");
}

// ============================================================
// Mouse::irq12_handler()
// ============================================================

void Mouse::irq12_handler(cinux::arch::InterruptFrame* /*frame*/) {
    // Read one byte from the PS/2 data port
    uint8_t byte = io_inb(Ps2Port::DATA);

    // Process the byte (assembles 3-byte packets internally)
    process_byte(byte);

    // Signal End-Of-Interrupt for IRQ12 (slave PIC, cascaded on IRQ2)
    PIC::send_eoi(12);
}

// ============================================================
// Mouse::process_byte()
// ============================================================

void Mouse::process_byte(uint8_t byte) {
    // Byte 0 of a new packet must have bit 3 set (ALWAYS_1 flag).
    // If we are not in sync, resynchronise by looking for this marker.
    if (packet_idx_ == 0) {
        if ((byte & Packet0::ALWAYS_1) == 0) {
            // Not a valid packet start -- discard and wait
            return;
        }
    }

    packet_[packet_idx_] = byte;
    packet_idx_++;

    // When we have all 3 bytes, decode and dispatch
    if (packet_idx_ >= PACKET_SIZE) {
        decode_packet(packet_[0], packet_[1], packet_[2]);
        packet_idx_ = 0;
    }
}

// ============================================================
// Mouse::decode_packet()
// ============================================================

void Mouse::decode_packet(uint8_t b0, uint8_t b1, uint8_t b2) {
    // Extract 9-bit signed delta X from byte1 + sign bit in byte0
    int32_t dx = static_cast<int32_t>(b1);
    if (b0 & Packet0::X_SIGN) {
        dx -= 256;  // Sign-extend from 8 to 9 bits
    }

    // Extract 9-bit signed delta Y from byte2 + sign bit in byte0
    // PS/2 Y axis: positive dy = mouse moves UP (physical),
    //              which is screen Y- direction, so subtract.
    int32_t dy = static_cast<int32_t>(b2);
    if (b0 & Packet0::Y_SIGN) {
        dy -= 256;  // Sign-extend from 8 to 9 bits
    }

    // Update absolute cursor position
    mouse_x_ += dx;
    mouse_y_ -= dy;

    // Clamp to screen bounds
    if (mouse_x_ < 0) {
        mouse_x_ = 0;
    }
    if (mouse_y_ < 0) {
        mouse_y_ = 0;
    }
    if (mouse_x_ >= static_cast<int32_t>(screen_width_)) {
        mouse_x_ = static_cast<int32_t>(screen_width_) - 1;
    }
    if (mouse_y_ >= static_cast<int32_t>(screen_height_)) {
        mouse_y_ = static_cast<int32_t>(screen_height_) - 1;
    }

    // Extract button state from byte 0
    uint8_t new_buttons = b0 & (Packet0::LEFT_BTN | Packet0::RIGHT_BTN | Packet0::MIDDLE_BTN);

    // Detect button transitions (press/release) for event generation
    uint8_t pressed  = new_buttons & ~prev_buttons_;  // Bits newly set
    uint8_t released = prev_buttons_ & ~new_buttons;  // Bits newly cleared

    // Build a MouseEvent with the current state
    MouseEvent me{};
    me.x       = mouse_x_;
    me.y       = mouse_y_;
    me.dx      = dx;
    me.dy      = -dy;  // Convert to screen-space (positive = downward)
    me.buttons = new_buttons;
    me.left    = (new_buttons & Packet0::LEFT_BTN) != 0;
    me.right   = (new_buttons & Packet0::RIGHT_BTN) != 0;
    me.middle  = (new_buttons & Packet0::MIDDLE_BTN) != 0;

    // If there was any movement, enqueue a MouseMove event
    if (dx != 0 || dy != 0) {
        Event ev{};
        ev.type_ = EventType::MouseMove;
        ev.mouse = me;
        g_event_queue_.enqueue(ev);
    }

    // Enqueue MouseDown events for newly pressed buttons
    if (pressed & Packet0::LEFT_BTN) {
        Event ev{};
        ev.type_ = EventType::MouseDown;
        ev.mouse = me;
        g_event_queue_.enqueue(ev);
    }
    if (pressed & Packet0::RIGHT_BTN) {
        Event ev{};
        ev.type_ = EventType::MouseDown;
        ev.mouse = me;
        g_event_queue_.enqueue(ev);
    }
    if (pressed & Packet0::MIDDLE_BTN) {
        Event ev{};
        ev.type_ = EventType::MouseDown;
        ev.mouse = me;
        g_event_queue_.enqueue(ev);
    }

    // Enqueue MouseUp events for newly released buttons
    if (released & Packet0::LEFT_BTN) {
        Event ev{};
        ev.type_ = EventType::MouseUp;
        ev.mouse = me;
        g_event_queue_.enqueue(ev);
    }
    if (released & Packet0::RIGHT_BTN) {
        Event ev{};
        ev.type_ = EventType::MouseUp;
        ev.mouse = me;
        g_event_queue_.enqueue(ev);
    }
    if (released & Packet0::MIDDLE_BTN) {
        Event ev{};
        ev.type_ = EventType::MouseUp;
        ev.mouse = me;
        g_event_queue_.enqueue(ev);
    }

    // Update state for next packet
    buttons_      = new_buttons;
    prev_buttons_ = new_buttons;
}

// ============================================================
// Mouse::poll()
// ============================================================

bool Mouse::poll(MouseEvent& out) {
    Event ev;
    if (!g_event_queue_.dequeue(ev)) {
        return false;
    }

    // Only return mouse events (MouseMove, MouseDown, MouseUp)
    switch (ev.type_) {
    case EventType::MouseMove:
    case EventType::MouseDown:
    case EventType::MouseUp:
        out = ev.mouse;
        return true;
    default:
        // Not a mouse event -- discard (keyboard events handled elsewhere)
        return false;
    }
}

// ============================================================
// Mouse::x() / y()
// ============================================================

int32_t Mouse::x() {
    return mouse_x_;
}

int32_t Mouse::y() {
    return mouse_y_;
}

// ============================================================
// Mouse::set_screen_bounds()
// ============================================================

void Mouse::set_screen_bounds(uint32_t width, uint32_t height) {
    screen_width_  = width;
    screen_height_ = height;

    // Clamp current cursor to new bounds
    if (mouse_x_ >= static_cast<int32_t>(width)) {
        mouse_x_ = static_cast<int32_t>(width) - 1;
    }
    if (mouse_y_ >= static_cast<int32_t>(height)) {
        mouse_y_ = static_cast<int32_t>(height) - 1;
    }
}

// ============================================================
// Mouse::event_queue()
// ============================================================

cinux::gui::EventQueue& Mouse::event_queue() {
    return g_event_queue_;
}

}  // namespace cinux::drivers

// ============================================================
// C-linkage bridge: called from irq12_stub in interrupts.S
// ============================================================

extern "C" void mouse_irq12_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::Mouse::irq12_handler(frame);
}
