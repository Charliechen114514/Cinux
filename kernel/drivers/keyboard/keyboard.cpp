/**
 * @file kernel/drivers/keyboard/keyboard.cpp
 * @brief PS/2 keyboard driver implementation
 *
 * Implements PS/2 controller initialisation, scan code set 1 decoding,
 * modifier tracking, ASCII translation, and a ring-buffer event queue.
 */

#include "keyboard.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::io::io_inb;
using cinux::io::io_outb;
using cinux::io::io_wait;
using cinux::lib::kprintf;

namespace cinux::drivers {

// ============================================================
// PS/2 Controller Constants (internal)
// ============================================================

namespace Ps2Port {
constexpr uint16_t DATA    = 0x60;  ///< PS/2 data register (read/write)
constexpr uint16_t STATUS  = 0x64;  ///< PS/2 status register (read)
constexpr uint16_t COMMAND = 0x64;  ///< PS/2 controller command (write)
}  // namespace Ps2Port

namespace Ps2Cmd {
constexpr uint8_t READ_CONFIG   = 0x20;
constexpr uint8_t WRITE_CONFIG  = 0x60;
constexpr uint8_t DISABLE_PORT2 = 0xA7;
constexpr uint8_t ENABLE_PORT2  = 0xA8;
constexpr uint8_t DISABLE_PORT1 = 0xAD;
constexpr uint8_t ENABLE_PORT1  = 0xAE;
constexpr uint8_t SELF_TEST     = 0xAA;
}  // namespace Ps2Cmd

namespace Ps2Status {
constexpr uint8_t OUTPUT_FULL = 0x01;
constexpr uint8_t INPUT_FULL  = 0x02;
}  // namespace Ps2Status

// ============================================================
// Scan Code Set 1 Special Keys (internal)
// ============================================================

namespace ScanCode {
constexpr uint8_t LSHIFT   = 0x2A;
constexpr uint8_t RSHIFT   = 0x36;
constexpr uint8_t LCTRL    = 0x1D;
constexpr uint8_t LALT     = 0x38;
constexpr uint8_t CAPS     = 0x3A;
constexpr uint8_t EXTENDED = 0xE0;
}  // namespace ScanCode

// ============================================================
// Ring Buffer Constants (internal)
// ============================================================

static constexpr uint32_t KEY_QUEUE_SIZE     = 64;
static constexpr uint32_t SCAN_TABLE_SIZE    = 128;

// ============================================================
// Scan Code Set 1 -> ASCII Lookup Tables (data-driven)
// ============================================================

/**
 * @brief Scan code set 1 to lowercase ASCII translation table
 *
 * Indexed by make code.  0 means non-printable or no mapping.
 */
static constexpr char kScToLower[SCAN_TABLE_SIZE] = {
    0,   27,  '1', '2', '3', '4', '5', '6',  // 0x00-0x07
    '7', '8', '9', '0', '-', '=', '\b', 0,    // 0x08-0x0F
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',  // 0x10-0x17
    'o', 'p', '[', ']', '\n', 0,   'a', 's',  // 0x18-0x1F
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  // 0x20-0x27
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', // 0x28-0x2F
    'b', 'n', 'm', ',', '.', '/', 0,   '*',   // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,    // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',  // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',  // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,    // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x58-0x5F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x60-0x67
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x78-0x7F
};

/**
 * @brief Scan code set 1 to uppercase / shifted ASCII translation table
 */
static constexpr char kScToUpper[SCAN_TABLE_SIZE] = {
    0,   27,  '!', '@', '#', '$', '%', '^',  // 0x00-0x07
    '&', '*', '(', ')', '_', '+', '\b', 0,   // 0x08-0x0F
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', // 0x10-0x17
    'O', 'P', '{', '}', '\n', 0,   'A', 'S', // 0x18-0x1F
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', // 0x20-0x27
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',  // 0x28-0x2F
    'B', 'N', 'M', '<', '>', '?', 0,   '*',  // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,    // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',  // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',  // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,    // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x58-0x5F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x60-0x67
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x78-0x7F
};

// ============================================================
// Static storage
// ============================================================

KeyEvent Keyboard::queue_[KEY_QUEUE_SIZE] = {};
uint32_t Keyboard::head_ = 0;
uint32_t Keyboard::tail_ = 0;

bool Keyboard::shift_held_ = false;
bool Keyboard::ctrl_held_  = false;
bool Keyboard::alt_held_   = false;

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
 * @brief Send a command byte to the PS/2 controller
 *
 * Waits for input buffer to be empty, then writes to command port 0x64.
 *
 * @param cmd  The command byte to send
 */
void send_command(uint8_t cmd) {
    wait_input_empty();
    io_outb(Ps2Port::COMMAND, cmd);
}

}  // anonymous namespace

// ============================================================
// Keyboard::init() -- PS/2 controller initialisation sequence
// ============================================================

void Keyboard::init() {
    // Step 1: Disable both PS/2 device ports so no data arrives
    send_command(Ps2Cmd::DISABLE_PORT1);
    send_command(Ps2Cmd::DISABLE_PORT2);

    // Step 2: Flush the output buffer (discard any stale data)
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::OUTPUT_FULL) != 0) {
        io_inb(Ps2Port::DATA);
    }

    // Step 3: Read the current controller configuration byte
    send_command(Ps2Cmd::READ_CONFIG);
    wait_input_empty();
    uint8_t config = io_inb(Ps2Port::DATA);

    // Step 4: Modify config: enable IRQ1 (bit 0), disable IRQ12 (bit 1 clear),
    //         disable mouse translation (bit 6 clear)
    config |= 0x01;   // Enable keyboard IRQ (IRQ1)
    config &= ~0x02;   // Disable mouse IRQ (IRQ12)
    config |= 0x40;   // Enable scan code set 2 → set 1 translation

    send_command(Ps2Cmd::WRITE_CONFIG);
    wait_input_empty();
    io_outb(Ps2Port::DATA, config);

    // Step 5: Controller self-test (command 0xAA, expect 0x55)
    send_command(Ps2Cmd::SELF_TEST);
    wait_input_empty();
    uint8_t result = io_inb(Ps2Port::DATA);

    if (result == 0x55) {
        kprintf("[KBD] PS/2 controller self-test passed.\n");
    } else {
        kprintf("[KBD] PS/2 controller self-test FAILED (got 0x%02X, expected 0x55)\n",
                result);
    }

    // Step 6: Re-enable the first PS/2 port (keyboard)
    send_command(Ps2Cmd::ENABLE_PORT1);

    // Step 7: Reset internal state
    head_ = 0;
    tail_ = 0;
    shift_held_ = false;
    ctrl_held_  = false;
    alt_held_   = false;

    kprintf("[KBD] Keyboard driver initialised.\n");
}

// ============================================================
// Keyboard::irq1_handler() -- called from ISR stub on every key
// ============================================================

void Keyboard::irq1_handler(cinux::arch::InterruptFrame* /*frame*/) {
    // Read the scan code from the PS/2 data port
    uint8_t sc = io_inb(Ps2Port::DATA);

    // Handle extended scan code prefix (0xE0) -- skip for now
    if (sc == ScanCode::EXTENDED) {
        PIC::send_eoi(1);
        return;
    }

    // Determine if this is a make (press) or break (release) code
    bool pressed = (sc & 0x80) == 0;
    uint8_t make_code = sc & 0x7F;

    // Track modifier keys regardless of press/release
    if (make_code == ScanCode::LSHIFT || make_code == ScanCode::RSHIFT) {
        shift_held_ = pressed;
    }

    if (make_code == ScanCode::LCTRL) {
        ctrl_held_ = pressed;
    }

    if (make_code == ScanCode::LALT) {
        alt_held_ = pressed;
    }

    // Build the event
    KeyEvent ev{};
    ev.scancode = sc;
    ev.pressed  = pressed;
    ev.shift    = shift_held_;
    ev.ctrl     = ctrl_held_;
    ev.alt      = alt_held_;
    ev.ascii    = 0;

    // Translate to ASCII only on key press and if the make code is in range
    if (pressed && make_code < SCAN_TABLE_SIZE) {
        ev.ascii = shift_held_ ? kScToUpper[make_code]
                               : kScToLower[make_code];
    }

    // Enqueue the event
    enqueue(ev);

    // Signal End-Of-Interrupt for IRQ1
    PIC::send_eoi(1);
}

// ============================================================
// Keyboard::poll() -- dequeue an event from the ring buffer
// ============================================================

bool Keyboard::poll(KeyEvent& out) {
    // Buffer is empty when head equals tail
    if (head_ == tail_) {
        return false;
    }

    // Copy the event at head and advance
    out = queue_[head_];
    head_ = (head_ + 1) % KEY_QUEUE_SIZE;
    return true;
}

// ============================================================
// Keyboard::enqueue() -- add event to the ring buffer
// ============================================================

void Keyboard::enqueue(const KeyEvent& ev) {
    uint32_t next = (tail_ + 1) % KEY_QUEUE_SIZE;

    // Drop the event if the buffer is full
    if (next == head_) {
        return;
    }

    queue_[tail_] = ev;
    tail_ = next;
}

}  // namespace cinux::drivers

// ============================================================
// C-linkage bridge: called from irq1_stub in interrupts.S
// ============================================================

extern "C" void keyboard_irq1_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::Keyboard::irq1_handler(frame);
}
