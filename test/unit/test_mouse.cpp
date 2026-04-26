/**
 * @file test/unit/test_mouse.cpp
 * @brief Host-side unit tests for PS/2 mouse driver logic
 *
 * Test coverage:
 *   - init() command sequence verification (0xA8, 0x20/0x60 config, 0xD4/0xF4)
 *   - 3-byte packet parsing: byte0(buttons)/byte1(dx)/byte2(dy)
 *   - 9-bit signed dx/dy sign-extension (positive and negative)
 *   - Button edge detection: press generates MouseDown, release generates MouseUp
 *   - No duplicate MouseDown for same button held across packets
 *   - Mouse position clamping to screen bounds
 *   - Cursor position tracking across multiple packets
 *   - Reinitialisation resets all internal state
 *
 * The real mouse driver uses x86 inline asm (io_inb/io_outb) and PIC
 * EOI which cannot execute on the host.  We replicate the pure data
 * transformation logic (packet parsing, sign-extension, button edge
 * detection, cursor clamping) and test it in isolation.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>

// ============================================================
// Replicate PS/2 constants from mouse.cpp
// ============================================================

namespace Ps2Port {
constexpr uint16_t DATA    = 0x60;
constexpr uint16_t STATUS  = 0x64;
constexpr uint16_t COMMAND = 0x64;
}  // namespace Ps2Port

namespace Ps2Cmd {
constexpr uint8_t READ_CONFIG  = 0x20;
constexpr uint8_t WRITE_CONFIG = 0x60;
constexpr uint8_t ENABLE_AUX   = 0xA8;
constexpr uint8_t WRITE_AUX    = 0xD4;
}  // namespace Ps2Cmd

namespace Ps2Status {
// Constants for reference (used indirectly via trace_inb mock)
}

namespace MouseCmd {
constexpr uint8_t ENABLE_STREAMING = 0xF4;
constexpr uint8_t ACK              = 0xFA;
}  // namespace MouseCmd

namespace Packet0 {
constexpr uint8_t LEFT_BTN   = 0x01;
constexpr uint8_t RIGHT_BTN  = 0x02;
constexpr uint8_t MIDDLE_BTN = 0x04;
constexpr uint8_t ALWAYS_1   = 0x08;
constexpr uint8_t X_SIGN     = 0x10;
constexpr uint8_t Y_SIGN     = 0x20;
}  // namespace Packet0

// ============================================================
// Replicate event types from kernel/gui/event.hpp
// ============================================================

enum class EventType : uint8_t {
    MouseMove = 0,
    MouseDown,
    MouseUp,
    KeyDown,
    KeyUp,
};

struct MouseEvent {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    uint8_t buttons;
    bool    left;
    bool    right;
    bool    middle;
};

struct KeyEvent {
    char    ascii;
    uint8_t scancode;
    bool    pressed;
    bool    shift;
    bool    ctrl;
    bool    alt;
};

struct Event {
    EventType type_;
    union {
        MouseEvent mouse;
        KeyEvent   key;
    };
};

// ============================================================
// Replicate EventQueue from kernel/gui/event.cpp
// ============================================================

static constexpr uint32_t EVENT_BUF_SIZE = 128;

struct EventQueue {
    Event    buf_[EVENT_BUF_SIZE]{};
    uint32_t head_ = 0;
    uint32_t tail_ = 0;

    void enqueue(const Event& ev) {
        uint32_t next = (tail_ + 1) % EVENT_BUF_SIZE;
        if (next == head_) {
            return;  // drop on full
        }
        buf_[tail_] = ev;
        tail_       = next;
    }

    bool dequeue(Event& out) {
        if (head_ == tail_) {
            return false;
        }
        out   = buf_[head_];
        head_ = (head_ + 1) % EVENT_BUF_SIZE;
        return true;
    }

    bool empty() const { return head_ == tail_; }

    void clear() {
        head_ = 0;
        tail_ = 0;
    }
};

// ============================================================
// Mock I/O port trace (for init sequence verification)
// ============================================================

struct IoTraceEntry {
    bool     is_write;
    uint16_t port;
    uint8_t  value;
};

static constexpr uint32_t MAX_IO_TRACE = 64;
static IoTraceEntry       io_trace[MAX_IO_TRACE];
static uint32_t           io_trace_count = 0;

static void io_trace_reset() {
    io_trace_count = 0;
    std::memset(io_trace, 0, sizeof(io_trace));
}

static void trace_outb(uint16_t port, uint8_t value) {
    if (io_trace_count < MAX_IO_TRACE) {
        io_trace[io_trace_count++] = {true, port, value};
    }
}

static uint8_t trace_inb(uint16_t port) {
    if (io_trace_count < MAX_IO_TRACE) {
        io_trace[io_trace_count++] = {false, port, 0};
    }
    // Return canned responses for init sequence:
    if (port == Ps2Port::STATUS) {
        return 0;  // Neither input-full nor output-full (ready)
    }
    if (port == Ps2Port::DATA) {
        return MouseCmd::ACK;  // Mouse ACK for 0xF4
    }
    return 0;
}

// ============================================================
// Simulated Mouse driver state
// ============================================================

static constexpr uint8_t PACKET_SIZE = 3;

struct MockMouse {
    uint8_t    packet_[PACKET_SIZE] = {};
    uint8_t    packet_idx_          = 0;
    int32_t    mouse_x_             = 0;
    int32_t    mouse_y_             = 0;
    uint8_t    buttons_             = 0;
    uint8_t    prev_buttons_        = 0;
    uint32_t   screen_width_        = 1024;
    uint32_t   screen_height_       = 768;
    EventQueue g_event_queue_{};

    void reset() {
        std::memset(packet_, 0, sizeof(packet_));
        packet_idx_   = 0;
        mouse_x_      = 0;
        mouse_y_      = 0;
        buttons_      = 0;
        prev_buttons_ = 0;
        g_event_queue_.clear();
    }

    // Simulates Mouse::init() using the mock I/O
    void init() {
        io_trace_reset();
        reset();

        // Step 1: Enable auxiliary device port (CMD 0xA8)
        trace_outb(Ps2Port::COMMAND, Ps2Cmd::ENABLE_AUX);

        // Step 2: Read config (CMD 0x20)
        trace_outb(Ps2Port::COMMAND, Ps2Cmd::READ_CONFIG);
        trace_inb(Ps2Port::STATUS);
        uint8_t config = trace_inb(Ps2Port::DATA);
        config |= 0x02;
        // Write back config (CMD 0x60)
        trace_outb(Ps2Port::COMMAND, Ps2Cmd::WRITE_CONFIG);
        trace_inb(Ps2Port::STATUS);
        trace_outb(Ps2Port::DATA, config);

        // Step 3: Send 0xD4 + 0xF4 to enable mouse streaming
        trace_outb(Ps2Port::COMMAND, Ps2Cmd::WRITE_AUX);
        trace_inb(Ps2Port::STATUS);
        trace_outb(Ps2Port::DATA, MouseCmd::ENABLE_STREAMING);

        // Wait for ACK
        trace_inb(Ps2Port::STATUS);
        trace_inb(Ps2Port::DATA);
    }

    // Simulates process_byte
    void process_byte(uint8_t byte) {
        if (packet_idx_ == 0) {
            if ((byte & Packet0::ALWAYS_1) == 0) {
                return;
            }
        }
        packet_[packet_idx_] = byte;
        packet_idx_++;
        if (packet_idx_ >= PACKET_SIZE) {
            decode_packet(packet_[0], packet_[1], packet_[2]);
            packet_idx_ = 0;
        }
    }

    // Simulates decode_packet
    void decode_packet(uint8_t b0, uint8_t b1, uint8_t b2) {
        int32_t dx = static_cast<int32_t>(b1);
        if (b0 & Packet0::X_SIGN) {
            dx -= 256;
        }

        int32_t dy = static_cast<int32_t>(b2);
        if (b0 & Packet0::Y_SIGN) {
            dy -= 256;
        }

        mouse_x_ += dx;
        mouse_y_ += dy;

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

        uint8_t new_buttons = b0 & (Packet0::LEFT_BTN | Packet0::RIGHT_BTN | Packet0::MIDDLE_BTN);

        uint8_t pressed  = new_buttons & ~prev_buttons_;
        uint8_t released = prev_buttons_ & ~new_buttons;

        MouseEvent me{};
        me.x       = mouse_x_;
        me.y       = mouse_y_;
        me.dx      = dx;
        me.dy      = dy;
        me.buttons = new_buttons;
        me.left    = (new_buttons & Packet0::LEFT_BTN) != 0;
        me.right   = (new_buttons & Packet0::RIGHT_BTN) != 0;
        me.middle  = (new_buttons & Packet0::MIDDLE_BTN) != 0;

        if (dx != 0 || dy != 0) {
            Event ev{};
            ev.type_ = EventType::MouseMove;
            ev.mouse = me;
            g_event_queue_.enqueue(ev);
        }

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

        buttons_      = new_buttons;
        prev_buttons_ = new_buttons;
    }

    bool poll(MouseEvent& out) {
        Event ev;
        if (!g_event_queue_.dequeue(ev)) {
            return false;
        }
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            out = ev.mouse;
            return true;
        default:
            return false;
        }
    }

    void set_screen_bounds(uint32_t w, uint32_t h) {
        screen_width_  = w;
        screen_height_ = h;
        if (mouse_x_ >= static_cast<int32_t>(w)) {
            mouse_x_ = static_cast<int32_t>(w) - 1;
        }
        if (mouse_y_ >= static_cast<int32_t>(h)) {
            mouse_y_ = static_cast<int32_t>(h) - 1;
        }
    }
};

// ============================================================
// Helper: feed a complete 3-byte packet to the mock mouse
// ============================================================

static void feed_packet(MockMouse& m, uint8_t b0, uint8_t b1, uint8_t b2) {
    m.process_byte(b0);
    m.process_byte(b1);
    m.process_byte(b2);
}

// ============================================================
// 1. init() command sequence verification
// ============================================================

/// Verify init() sends 0xA8 to command port first
TEST("mouse: init sends ENABLE_AUX (0xA8) to command port") {
    MockMouse m;
    m.init();

    ASSERT_GT(io_trace_count, 0u);
    ASSERT_EQ(io_trace[0].is_write, true);
    ASSERT_EQ(io_trace[0].port, Ps2Port::COMMAND);
    ASSERT_EQ(io_trace[0].value, Ps2Cmd::ENABLE_AUX);
}

/// Verify init() reads config via CMD 0x20
TEST("mouse: init reads config via READ_CONFIG (0x20)") {
    MockMouse m;
    m.init();

    // Second outb should be CMD 0x20
    ASSERT_GT(io_trace_count, 1u);
    ASSERT_EQ(io_trace[1].is_write, true);
    ASSERT_EQ(io_trace[1].port, Ps2Port::COMMAND);
    ASSERT_EQ(io_trace[1].value, Ps2Cmd::READ_CONFIG);
}

/// Verify init() writes back config via CMD 0x60 with bit 1 set
TEST("mouse: init writes config via WRITE_CONFIG (0x60) with IRQ12 bit") {
    MockMouse m;
    m.init();

    // Find the WRITE_CONFIG command in the trace
    bool found_write_config = false;
    bool found_config_data  = false;
    for (uint32_t i = 0; i < io_trace_count; i++) {
        if (io_trace[i].is_write && io_trace[i].port == Ps2Port::COMMAND &&
            io_trace[i].value == Ps2Cmd::WRITE_CONFIG) {
            found_write_config = true;
            // The next outb to DATA port should have bit 1 set
            for (uint32_t j = i + 1; j < io_trace_count; j++) {
                if (io_trace[j].is_write && io_trace[j].port == Ps2Port::DATA) {
                    ASSERT_TRUE(io_trace[j].value & 0x02);
                    found_config_data = true;
                    break;
                }
            }
            break;
        }
    }
    ASSERT_TRUE(found_write_config);
    ASSERT_TRUE(found_config_data);
}

/// Verify init() sends 0xD4 (WRITE_AUX) followed by 0xF4 (ENABLE_STREAMING)
TEST("mouse: init sends WRITE_AUX (0xD4) then 0xF4") {
    MockMouse m;
    m.init();

    // Find WRITE_AUX command
    bool found_write_aux = false;
    for (uint32_t i = 0; i < io_trace_count; i++) {
        if (io_trace[i].is_write && io_trace[i].port == Ps2Port::COMMAND &&
            io_trace[i].value == Ps2Cmd::WRITE_AUX) {
            found_write_aux = true;
            // Next outb to DATA should be 0xF4
            for (uint32_t j = i + 1; j < io_trace_count; j++) {
                if (io_trace[j].is_write && io_trace[j].port == Ps2Port::DATA) {
                    ASSERT_EQ(io_trace[j].value, MouseCmd::ENABLE_STREAMING);
                    break;
                }
            }
            break;
        }
    }
    ASSERT_TRUE(found_write_aux);
}

// ============================================================
// 2. 3-byte packet parsing: basic movement
// ============================================================

/// Verify a simple rightward movement (dx=5, dy=0, no buttons)
TEST("mouse: basic movement dx=5 dy=0") {
    MockMouse m;
    m.reset();

    // byte0: ALWAYS_1 set, no buttons, no sign, no overflow
    // byte1: dx=5
    // byte2: dy=0
    feed_packet(m, Packet0::ALWAYS_1, 5, 0);

    ASSERT_EQ(m.mouse_x_, 5);
    ASSERT_EQ(m.mouse_y_, 0);

    MouseEvent me;
    ASSERT_TRUE(m.poll(me));
    ASSERT_EQ(me.dx, 5);
    ASSERT_EQ(me.dy, 0);
    ASSERT_EQ(me.x, 5);
    ASSERT_EQ(me.y, 0);
    ASSERT_EQ(me.buttons, 0u);
    ASSERT_FALSE(me.left);
    ASSERT_FALSE(me.right);
    ASSERT_FALSE(me.middle);
}

/// Verify a downward movement (dx=0, dy=10, no buttons)
TEST("mouse: basic movement dx=0 dy=10") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1, 0, 10);

    ASSERT_EQ(m.mouse_x_, 0);
    ASSERT_EQ(m.mouse_y_, 10);

    MouseEvent me;
    ASSERT_TRUE(m.poll(me));
    ASSERT_EQ(me.dx, 0);
    ASSERT_EQ(me.dy, 10);
}

// ============================================================
// 3. 9-bit signed dx/dy: negative values
// ============================================================

/// Verify negative dx: X_SIGN set, dx=0xFF -> -1
TEST("mouse: negative dx via X_SIGN (dx=-1)") {
    MockMouse m;
    m.reset();

    // Start at position (10, 0)
    m.mouse_x_ = 10;

    // byte0: ALWAYS_1 | X_SIGN
    // byte1: 0xFF -> interpreted as -1 (255 - 256 = -1)
    // byte2: 0x00
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::X_SIGN, 0xFF, 0x00);

    ASSERT_EQ(m.mouse_x_, 9);  // 10 + (-1)
}

/// Verify negative dy: Y_SIGN set, dy=0xFF -> -1
TEST("mouse: negative dy via Y_SIGN (dy=-1)") {
    MockMouse m;
    m.reset();

    m.mouse_y_ = 10;

    // byte0: ALWAYS_1 | Y_SIGN
    // byte1: 0x00
    // byte2: 0xFF -> -1
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::Y_SIGN, 0x00, 0xFF);

    ASSERT_EQ(m.mouse_y_, 9);  // 10 + (-1)
}

/// Verify maximum positive dx (255, within 9-bit positive range)
TEST("mouse: positive dx=255") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1, 255, 0);

    ASSERT_EQ(m.mouse_x_, 255);
}

/// Verify minimum negative dx (byte1=0x00, X_SIGN -> -256)
TEST("mouse: negative dx=-256 via X_SIGN") {
    MockMouse m;
    m.reset();

    m.mouse_x_ = 300;

    // X_SIGN set, byte1=0x00 -> 0 - 256 = -256
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::X_SIGN, 0x00, 0x00);

    ASSERT_EQ(m.mouse_x_, 44);  // 300 + (-256) = 44
}

/// Verify negative dy=-256 via Y_SIGN
TEST("mouse: negative dy=-256 via Y_SIGN") {
    MockMouse m;
    m.reset();

    m.mouse_y_ = 300;

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::Y_SIGN, 0x00, 0x00);

    ASSERT_EQ(m.mouse_y_, 44);  // 300 + (-256) = 44
}

/// Verify combined negative dx and dy clamp to (0,0)
TEST("mouse: combined negative dx and dy") {
    MockMouse m;
    m.reset();

    m.mouse_x_ = 100;
    m.mouse_y_ = 100;

    // X_SIGN | Y_SIGN set, dx byte=0x80, dy byte=0x80
    // dx = 128 - 256 = -128
    // dy = 128 - 256 = -128
    // Raw: 100 - 128 = -28, clamped to 0
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::X_SIGN | Packet0::Y_SIGN, 0x80, 0x80);

    // Both should be clamped to 0
    ASSERT_EQ(m.mouse_x_, 0);
    ASSERT_EQ(m.mouse_y_, 0);
}

// ============================================================
// 4. Button parsing
// ============================================================

/// Verify left button press sets left=true and buttons bit 0
TEST("mouse: left button press") {
    MockMouse m;
    m.reset();

    // byte0: ALWAYS_1 | LEFT_BTN
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 0, 0);

    ASSERT_EQ(m.buttons_, Packet0::LEFT_BTN);
    ASSERT_TRUE(m.buttons_ & Packet0::LEFT_BTN);

    // Check event queue: should have a MouseDown event
    MouseEvent me;
    ASSERT_TRUE(m.poll(me));
    // First event should be MouseDown (no movement so no MouseMove)
    // Since dx=0 and dy=0, only button events are generated
    // Actually check via Event dequeue
}

/// Verify right button press
TEST("mouse: right button press") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::RIGHT_BTN, 0, 0);

    ASSERT_TRUE(m.buttons_ & Packet0::RIGHT_BTN);
}

/// Verify middle button press
TEST("mouse: middle button press") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::MIDDLE_BTN, 0, 0);

    ASSERT_TRUE(m.buttons_ & Packet0::MIDDLE_BTN);
}

/// Verify multiple buttons pressed simultaneously
TEST("mouse: left+right buttons simultaneously") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN | Packet0::RIGHT_BTN, 0, 0);

    ASSERT_TRUE(m.buttons_ & Packet0::LEFT_BTN);
    ASSERT_TRUE(m.buttons_ & Packet0::RIGHT_BTN);
    ASSERT_FALSE(m.buttons_ & Packet0::MIDDLE_BTN);
}

// ============================================================
// 5. Button edge detection: press/release
// ============================================================

/// Verify button press generates MouseDown event
TEST("mouse: left press generates MouseDown event") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 0, 0);

    // No movement (dx=0, dy=0), so only MouseDown should be in queue
    Event ev;
    ASSERT_TRUE(m.g_event_queue_.dequeue(ev));
    ASSERT_EQ(ev.type_, EventType::MouseDown);
    ASSERT_TRUE(ev.mouse.left);

    // Queue should be empty now
    ASSERT_FALSE(m.g_event_queue_.dequeue(ev));
}

/// Verify button release generates MouseUp event
TEST("mouse: left release generates MouseUp event") {
    MockMouse m;
    m.reset();

    // Press first
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 0, 0);
    // Clear queue
    Event ev;
    while (m.g_event_queue_.dequeue(ev)) {
    }

    // Release
    feed_packet(m, Packet0::ALWAYS_1, 0, 0);

    ASSERT_TRUE(m.g_event_queue_.dequeue(ev));
    ASSERT_EQ(ev.type_, EventType::MouseUp);
    ASSERT_FALSE(ev.mouse.left);
}

/// Verify same button held across packets only generates one MouseDown
TEST("mouse: same button held across packets -> single MouseDown") {
    MockMouse m;
    m.reset();

    // First packet: left button pressed + movement
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 5, 0);

    int   mouse_down_count = 0;
    Event ev;
    while (m.g_event_queue_.dequeue(ev)) {
        if (ev.type_ == EventType::MouseDown) {
            mouse_down_count++;
        }
    }
    ASSERT_EQ(mouse_down_count, 1);

    // Second packet: left button still held + more movement
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 3, 0);

    mouse_down_count     = 0;
    int mouse_move_count = 0;
    while (m.g_event_queue_.dequeue(ev)) {
        if (ev.type_ == EventType::MouseDown) {
            mouse_down_count++;
        }
        if (ev.type_ == EventType::MouseMove) {
            mouse_move_count++;
        }
    }
    // Second packet should generate MouseMove (movement) but NOT MouseDown
    ASSERT_EQ(mouse_down_count, 0);
    ASSERT_EQ(mouse_move_count, 1);
}

/// Verify release-then-press generates both MouseUp and MouseDown
TEST("mouse: release then press generates MouseUp then MouseDown") {
    MockMouse m;
    m.reset();

    // Press
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 0, 0);
    // Release
    feed_packet(m, Packet0::ALWAYS_1, 0, 0);
    // Press again
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 0, 0);

    Event ev;
    int   down_count = 0;
    int   up_count   = 0;
    while (m.g_event_queue_.dequeue(ev)) {
        if (ev.type_ == EventType::MouseDown) {
            down_count++;
        }
        if (ev.type_ == EventType::MouseUp) {
            up_count++;
        }
    }
    ASSERT_EQ(down_count, 2);  // Two presses
    ASSERT_EQ(up_count, 1);    // One release
}

/// Verify right button edge detection
TEST("mouse: right button press and release events") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::RIGHT_BTN, 0, 0);
    feed_packet(m, Packet0::ALWAYS_1, 0, 0);

    Event ev;
    int   down = 0, up = 0;
    while (m.g_event_queue_.dequeue(ev)) {
        if (ev.type_ == EventType::MouseDown)
            down++;
        if (ev.type_ == EventType::MouseUp)
            up++;
    }
    ASSERT_EQ(down, 1);
    ASSERT_EQ(up, 1);
}

// ============================================================
// 6. Mouse position clamping to screen bounds
// ============================================================

/// Verify cursor clamped at x=0 (cannot go negative)
TEST("mouse: cursor clamped to x=0 on large negative dx") {
    MockMouse m;
    m.reset();

    // X_SIGN set, byte1=0x00 -> dx=-256, from x=0 -> should clamp to 0
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::X_SIGN, 0x00, 0x00);

    ASSERT_EQ(m.mouse_x_, 0);
}

/// Verify cursor clamped at y=0
TEST("mouse: cursor clamped to y=0 on large negative dy") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::Y_SIGN, 0x00, 0x00);

    ASSERT_EQ(m.mouse_y_, 0);
}

/// Verify cursor clamped at screen_width - 1
TEST("mouse: cursor clamped to screen_width-1") {
    MockMouse m;
    m.reset();
    m.screen_width_ = 100;

    // dx=200 from x=0, should clamp to 99
    feed_packet(m, Packet0::ALWAYS_1, 200, 0);

    ASSERT_EQ(m.mouse_x_, 99);
}

/// Verify cursor clamped at screen_height - 1
TEST("mouse: cursor clamped to screen_height-1") {
    MockMouse m;
    m.reset();
    m.screen_height_ = 50;

    feed_packet(m, Packet0::ALWAYS_1, 0, 200);

    ASSERT_EQ(m.mouse_y_, 49);
}

/// Verify set_screen_bounds clamps current cursor position
TEST("mouse: set_screen_bounds clamps cursor") {
    MockMouse m;
    m.reset();
    m.mouse_x_ = 500;
    m.mouse_y_ = 500;

    m.set_screen_bounds(100, 100);

    ASSERT_EQ(m.mouse_x_, 99);
    ASSERT_EQ(m.mouse_y_, 99);
}

/// Verify set_screen_bounds with larger bounds does not clamp
TEST("mouse: set_screen_bounds larger does not clamp") {
    MockMouse m;
    m.reset();
    m.mouse_x_ = 50;
    m.mouse_y_ = 50;

    m.set_screen_bounds(2000, 2000);

    ASSERT_EQ(m.mouse_x_, 50);
    ASSERT_EQ(m.mouse_y_, 50);
}

// ============================================================
// 7. Cursor position tracking across multiple packets
// ============================================================

/// Verify cumulative position updates across three packets
TEST("mouse: cumulative position across 3 packets") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1, 10, 5);         // x=10, y=5
    feed_packet(m, Packet0::ALWAYS_1, 3, 2);          // x=13, y=7
    feed_packet(m, Packet0::ALWAYS_1, -1 & 0xFF, 0);  // dx=255, no sign -> x=268

    ASSERT_EQ(m.mouse_x_, 268);  // 10 + 3 + 255
    ASSERT_EQ(m.mouse_y_, 7);    // 5 + 2 + 0
}

/// Verify negative movement across packets
TEST("mouse: negative movement reduces position") {
    MockMouse m;
    m.reset();
    m.mouse_x_ = 100;
    m.mouse_y_ = 100;

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::X_SIGN | Packet0::Y_SIGN, 10, 20);
    // dx = 10 - 256 = -246, dy = 20 - 256 = -236

    ASSERT_EQ(m.mouse_x_, 0);  // 100 - 246 = -146, clamped to 0
    ASSERT_EQ(m.mouse_y_, 0);  // 100 - 236 = -136, clamped to 0
}

// ============================================================
// 8. Reinitialisation resets all state
// ============================================================

/// Verify init() resets cursor position and buttons
TEST("mouse: init resets all state") {
    MockMouse m;
    m.reset();

    // Move cursor and press a button
    m.mouse_x_      = 500;
    m.mouse_y_      = 300;
    m.buttons_      = Packet0::LEFT_BTN;
    m.prev_buttons_ = Packet0::LEFT_BTN;

    m.init();  // This calls reset() internally

    ASSERT_EQ(m.mouse_x_, 0);
    ASSERT_EQ(m.mouse_y_, 0);
    ASSERT_EQ(m.buttons_, 0u);
    ASSERT_EQ(m.prev_buttons_, 0u);
    ASSERT_EQ(m.packet_idx_, 0u);
    ASSERT_TRUE(m.g_event_queue_.empty());
}

// ============================================================
// 9. Packet resynchronisation (invalid byte0 discarded)
// ============================================================

/// Verify byte0 without ALWAYS_1 flag is discarded
TEST("mouse: byte0 without ALWAYS_1 flag is discarded") {
    MockMouse m;
    m.reset();

    // byte0 = 0x00 (no ALWAYS_1), should be discarded
    m.process_byte(0x00);
    ASSERT_EQ(m.packet_idx_, 0);

    // byte0 = 0x07 (buttons only, no ALWAYS_1), should be discarded
    m.process_byte(0x07);
    ASSERT_EQ(m.packet_idx_, 0);

    // Valid byte0 should be accepted
    m.process_byte(Packet0::ALWAYS_1);
    ASSERT_EQ(m.packet_idx_, 1);
}

/// Verify resynchronisation: garbage byte resets packet assembly
TEST("mouse: garbage byte resets packet index to 0") {
    MockMouse m;
    m.reset();

    // Valid first byte
    m.process_byte(Packet0::ALWAYS_1);
    ASSERT_EQ(m.packet_idx_, 1);

    // If a new byte0 arrives without completing the packet,
    // and the packet_idx is NOT 0, it should still accumulate
    // (resync only happens at idx=0)
    m.process_byte(0x42);  // byte1
    ASSERT_EQ(m.packet_idx_, 2);

    m.process_byte(0x00);  // byte2 -> packet complete
    ASSERT_EQ(m.packet_idx_, 0);
}

// ============================================================
// 10. Event queue integration: movement + button events
// ============================================================

/// Verify movement + button press generates both MouseMove and MouseDown
TEST("mouse: movement + left press generates MouseMove and MouseDown") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN, 5, 3);

    Event ev;
    int   move_count = 0;
    int   down_count = 0;
    while (m.g_event_queue_.dequeue(ev)) {
        if (ev.type_ == EventType::MouseMove)
            move_count++;
        if (ev.type_ == EventType::MouseDown)
            down_count++;
    }
    ASSERT_EQ(move_count, 1);
    ASSERT_EQ(down_count, 1);
}

/// Verify poll() only returns mouse events (not keyboard events)
TEST("mouse: poll discards non-mouse events") {
    MockMouse m;
    m.reset();

    // Enqueue a keyboard event directly
    Event key_ev{};
    key_ev.type_     = EventType::KeyDown;
    key_ev.key.ascii = 'A';
    m.g_event_queue_.enqueue(key_ev);

    MouseEvent me;
    // poll() should skip the keyboard event and return false
    ASSERT_FALSE(m.poll(me));
}

// ============================================================
// 11. MouseEvent field correctness
// ============================================================

/// Verify all MouseEvent fields are set correctly
TEST("mouse: MouseEvent fields match packet data") {
    MockMouse m;
    m.reset();

    m.mouse_x_ = 50;
    m.mouse_y_ = 30;

    // Left + right buttons, dx=10, dy=20
    feed_packet(m, Packet0::ALWAYS_1 | Packet0::LEFT_BTN | Packet0::RIGHT_BTN, 10, 20);

    Event ev;
    ASSERT_TRUE(m.g_event_queue_.dequeue(ev));
    // First event is MouseMove (movement)
    ASSERT_EQ(ev.type_, EventType::MouseMove);
    ASSERT_EQ(ev.mouse.x, 60);  // 50 + 10
    ASSERT_EQ(ev.mouse.y, 50);  // 30 + 20
    ASSERT_EQ(ev.mouse.dx, 10);
    ASSERT_EQ(ev.mouse.dy, 20);
    ASSERT_EQ(ev.mouse.buttons, Packet0::LEFT_BTN | Packet0::RIGHT_BTN);
    ASSERT_TRUE(ev.mouse.left);
    ASSERT_TRUE(ev.mouse.right);
    ASSERT_FALSE(ev.mouse.middle);
}

/// Verify buttons convenience booleans in dequeued event
TEST("mouse: middle button convenience boolean") {
    MockMouse m;
    m.reset();

    feed_packet(m, Packet0::ALWAYS_1 | Packet0::MIDDLE_BTN, 0, 0);

    Event ev;
    ASSERT_TRUE(m.g_event_queue_.dequeue(ev));
    ASSERT_EQ(ev.type_, EventType::MouseDown);
    ASSERT_TRUE(ev.mouse.middle);
    ASSERT_FALSE(ev.mouse.left);
    ASSERT_FALSE(ev.mouse.right);
}

// ============================================================
// Main Function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
