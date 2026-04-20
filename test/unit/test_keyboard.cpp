/**
 * @file test/unit/test_keyboard.cpp
 * @brief Host-side unit tests for PS/2 keyboard driver logic
 *
 * Test coverage:
 *   - Scan code set 1 to ASCII lookup tables (lowercase and uppercase)
 *   - Key make/break code detection (bit 7 set = release)
 *   - Modifier key tracking (Shift, Ctrl, Alt) on press and release
 *   - ASCII translation with modifier state (shift toggles case)
 *   - Ring buffer enqueue/dequeue behaviour
 *   - Ring buffer boundary: full buffer drops events, empty returns false
 *   - Ring buffer wrap-around correctness
 *   - Extended scan code prefix (0xE0) handling
 *
 * The real keyboard driver uses x86 inline asm (io_inb/io_outb) and PIC
 * EOI which cannot execute on the host.  We replicate the pure data
 * transformation logic (scan code decoding, modifier state machine, ring
 * buffer arithmetic) and test it in isolation, mirroring the approach in
 * test_pic.cpp.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>

// ============================================================
// Replicate internal constants from keyboard.cpp for testing
// ============================================================

static constexpr uint32_t SCAN_TABLE_SIZE = 128;

/// Scan code set 1 -> lowercase ASCII (copied from keyboard.cpp)
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

/// Scan code set 1 -> uppercase/shifted ASCII (copied from keyboard.cpp)
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
// Special scan codes (internal constants from keyboard.cpp)
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
// Simulated KeyEvent (mirrors kernel struct)
// ============================================================

struct KeyEvent {
    char    ascii;
    uint8_t scancode;
    bool    pressed;
    bool    shift;
    bool    ctrl;
    bool    alt;
};

// ============================================================
// Simulated ring buffer (mirrors Keyboard ring buffer logic)
// ============================================================

static constexpr uint32_t KEY_QUEUE_SIZE = 64;

static KeyEvent queue_[KEY_QUEUE_SIZE];
static uint32_t head_ = 0;
static uint32_t tail_ = 0;

static void ring_reset() {
    head_ = 0;
    tail_ = 0;
    std::memset(queue_, 0, sizeof(queue_));
}

static bool ring_poll(KeyEvent& out) {
    if (head_ == tail_) {
        return false;
    }
    out = queue_[head_];
    head_ = (head_ + 1) % KEY_QUEUE_SIZE;
    return true;
}

static void ring_enqueue(const KeyEvent& ev) {
    uint32_t next = (tail_ + 1) % KEY_QUEUE_SIZE;
    if (next == head_) {
        return;  // drop on full
    }
    queue_[tail_] = ev;
    tail_ = next;
}

// ============================================================
// Simulated modifier state + decode logic
// ============================================================

static bool shift_held = false;
static bool ctrl_held  = false;
static bool alt_held   = false;

static void modifier_reset() {
    shift_held = false;
    ctrl_held  = false;
    alt_held   = false;
}

/**
 * @brief Decode a scan code into a KeyEvent (mirrors irq1_handler logic)
 *
 * Returns false if the scan code is an extended prefix (0xE0) and
 * should be skipped.  On success, fills @p out and returns true.
 */
static bool decode_scancode(uint8_t sc, KeyEvent& out) {
    if (sc == ScanCode::EXTENDED) {
        return false;
    }

    bool pressed   = (sc & 0x80) == 0;
    uint8_t make_code = sc & 0x7F;

    // Modifier tracking
    if (make_code == ScanCode::LSHIFT || make_code == ScanCode::RSHIFT) {
        shift_held = pressed;
    }
    if (make_code == ScanCode::LCTRL) {
        ctrl_held = pressed;
    }
    if (make_code == ScanCode::LALT) {
        alt_held = pressed;
    }

    out.scancode = sc;
    out.pressed  = pressed;
    out.shift    = shift_held;
    out.ctrl     = ctrl_held;
    out.alt      = alt_held;
    out.ascii    = 0;

    if (pressed && make_code < SCAN_TABLE_SIZE) {
        out.ascii = shift_held ? kScToUpper[make_code]
                               : kScToLower[make_code];
    }

    return true;
}

// ============================================================
// 1. Scan Code to ASCII: Lowercase (Normal Path)
// ============================================================

/// Verify 0x1E (QWERTY 'a') maps to 'a' without shift
TEST("keyboard: scancode 0x1E -> 'a' (lowercase)") {
    ASSERT_EQ(kScToLower[0x1E], 'a');
}

/// Verify 0x10 (QWERTY 'q') maps to 'q'
TEST("keyboard: scancode 0x10 -> 'q' (lowercase)") {
    ASSERT_EQ(kScToLower[0x10], 'q');
}

/// Verify 0x11 (QWERTY 'w') maps to 'w'
TEST("keyboard: scancode 0x11 -> 'w' (lowercase)") {
    ASSERT_EQ(kScToLower[0x11], 'w');
}

/// Verify 0x2C (QWERTY 'z') maps to 'z'
TEST("keyboard: scancode 0x2C -> 'z' (lowercase)") {
    ASSERT_EQ(kScToLower[0x2C], 'z');
}

/// Verify 0x39 (space bar) maps to ' '
TEST("keyboard: scancode 0x39 -> ' ' (space)") {
    ASSERT_EQ(kScToLower[0x39], ' ');
}

/// Verify 0x0E (backspace) maps to '\b'
TEST("keyboard: scancode 0x0E -> '\\b' (backspace)") {
    ASSERT_EQ(kScToLower[0x0E], '\b');
}

/// Verify 0x1C (enter) maps to '\n'
TEST("keyboard: scancode 0x1C -> '\\n' (enter)") {
    ASSERT_EQ(kScToLower[0x1C], '\n');
}

/// Verify 0x01 (Esc) maps to 27
TEST("keyboard: scancode 0x01 -> 27 (Esc)") {
    ASSERT_EQ(kScToLower[0x01], 27);
}

// ============================================================
// 2. Scan Code to ASCII: Uppercase / Shifted (Normal Path)
// ============================================================

/// Verify 0x1E + shift maps to 'A'
TEST("keyboard: scancode 0x1E + shift -> 'A' (uppercase)") {
    ASSERT_EQ(kScToUpper[0x1E], 'A');
}

/// Verify 0x10 + shift maps to 'Q'
TEST("keyboard: scancode 0x10 + shift -> 'Q'") {
    ASSERT_EQ(kScToUpper[0x10], 'Q');
}

/// Verify 0x2C + shift maps to 'Z'
TEST("keyboard: scancode 0x2C + shift -> 'Z'") {
    ASSERT_EQ(kScToUpper[0x2C], 'Z');
}

/// Verify 0x02 + shift maps to '!'
TEST("keyboard: scancode 0x02 -> '1' unshifted, '!' shifted") {
    ASSERT_EQ(kScToLower[0x02], '1');
    ASSERT_EQ(kScToUpper[0x02], '!');
}

/// Verify 0x03 + shift maps to '@'
TEST("keyboard: scancode 0x03 -> '2' unshifted, '@' shifted") {
    ASSERT_EQ(kScToLower[0x03], '2');
    ASSERT_EQ(kScToUpper[0x03], '@');
}

/// Verify 0x04 + shift maps to '#'
TEST("keyboard: scancode 0x04 -> '3' unshifted, '#' shifted") {
    ASSERT_EQ(kScToLower[0x04], '3');
    ASSERT_EQ(kScToUpper[0x04], '#');
}

/// Verify 0x2B + shift maps to '|' (backslash key)
TEST("keyboard: scancode 0x2B -> '\\' unshifted, '|' shifted") {
    ASSERT_EQ(kScToLower[0x2B], '\\');
    ASSERT_EQ(kScToUpper[0x2B], '|');
}

// ============================================================
// 3. Non-printable scan codes
// ============================================================

/// Verify F-keys (0x3B-0x44) have no ASCII mapping
TEST("keyboard: function key scancodes map to 0") {
    ASSERT_EQ(kScToLower[0x3B], 0);  // F1
    ASSERT_EQ(kScToLower[0x3C], 0);  // F2
    ASSERT_EQ(kScToLower[0x44], 0);  // F10
}

/// Verify scan code 0x00 has no mapping
TEST("keyboard: scancode 0x00 maps to 0") {
    ASSERT_EQ(kScToLower[0x00], 0);
    ASSERT_EQ(kScToUpper[0x00], 0);
}

/// Verify modifier key scan codes themselves have no printable ASCII
TEST("keyboard: modifier scancodes have no ASCII mapping") {
    ASSERT_EQ(kScToLower[ScanCode::LSHIFT], 0);  // 0x2A
    ASSERT_EQ(kScToLower[ScanCode::LCTRL],  0);   // 0x1D
    ASSERT_EQ(kScToLower[ScanCode::LALT],   0);   // 0x38
}

/// Verify Alt scan code (0x38) entry in the lower table is 0
TEST("keyboard: scancode 0x38 (Alt) maps to 0 in lower table") {
    // Index 0x38 in kScToLower is 0 (non-printable)
    ASSERT_EQ(kScToLower[0x38], 0);
}

// ============================================================
// 4. Make/Break Code Detection
// ============================================================

/// Verify a make code (press) has bit 7 clear
TEST("keyboard: make code has bit 7 clear -> pressed=true") {
    uint8_t sc = 0x1E;  // 'a' press
    bool pressed = (sc & 0x80) == 0;
    ASSERT_TRUE(pressed);
}

/// Verify a break code (release) has bit 7 set -> pressed=false
TEST("keyboard: break code has bit 7 set -> pressed=false") {
    uint8_t sc = 0x9E;  // 'a' release = 0x1E | 0x80
    bool pressed = (sc & 0x80) == 0;
    ASSERT_FALSE(pressed);
}

/// Verify make_code extraction masks off bit 7
TEST("keyboard: break code 0x9E has make_code 0x1E") {
    uint8_t sc = 0x9E;
    uint8_t make_code = sc & 0x7F;
    ASSERT_EQ(make_code, 0x1Eu);
}

/// Verify break code for LShift (0xAA) extracts make_code 0x2A
TEST("keyboard: break code 0xAA (LShift release) -> make_code 0x2A") {
    uint8_t sc = 0xAA;  // 0x2A | 0x80
    uint8_t make_code = sc & 0x7F;
    ASSERT_EQ(make_code, 0x2Au);
}

// ============================================================
// 5. Modifier State Tracking
// ============================================================

/// Verify LShift press sets shift_held
TEST("keyboard: LShift press sets shift modifier") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::LSHIFT, ev);  // 0x2A press
    ASSERT_TRUE(shift_held);
    ASSERT_TRUE(ev.pressed);
    ASSERT_TRUE(ev.shift);
}

/// Verify LShift release clears shift_held
TEST("keyboard: LShift release clears shift modifier") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::LSHIFT, ev);  // press
    decode_scancode(0xAA, ev);              // 0x2A | 0x80 = release
    ASSERT_FALSE(shift_held);
    ASSERT_FALSE(ev.pressed);
    ASSERT_FALSE(ev.shift);
}

/// Verify RShift press also sets shift_held
TEST("keyboard: RShift press sets shift modifier") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::RSHIFT, ev);  // 0x36 press
    ASSERT_TRUE(shift_held);
}

/// Verify RShift release clears shift_held
TEST("keyboard: RShift release clears shift modifier") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::RSHIFT, ev);  // press
    decode_scancode(0xB6, ev);             // 0x36 | 0x80 = release
    ASSERT_FALSE(shift_held);
}

/// Verify Ctrl press/release tracking
TEST("keyboard: Ctrl press/release tracking") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::LCTRL, ev);  // 0x1D press
    ASSERT_TRUE(ctrl_held);
    ASSERT_TRUE(ev.ctrl);
    decode_scancode(0x9D, ev);  // 0x1D | 0x80 = release
    ASSERT_FALSE(ctrl_held);
    ASSERT_FALSE(ev.ctrl);
}

/// Verify Alt press/release tracking
TEST("keyboard: Alt press/release tracking") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::LALT, ev);  // 0x38 press
    ASSERT_TRUE(alt_held);
    ASSERT_TRUE(ev.alt);
    decode_scancode(0xB8, ev);  // 0x38 | 0x80 = release
    ASSERT_FALSE(alt_held);
    ASSERT_FALSE(ev.alt);
}

// ============================================================
// 6. ASCII Translation with Modifier State
// ============================================================

/// Verify 'a' press without shift produces 'a'
TEST("keyboard: 'a' press without shift -> ascii 'a'") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(0x1E, ev);  // 'a' press, no modifiers
    ASSERT_EQ(ev.ascii, 'a');
    ASSERT_TRUE(ev.pressed);
}

/// Verify 'a' press WITH shift produces 'A'
TEST("keyboard: 'a' press with shift -> ascii 'A'") {
    modifier_reset();
    KeyEvent ev{};
    // First press shift
    decode_scancode(ScanCode::LSHIFT, ev);
    // Then press 'a'
    decode_scancode(0x1E, ev);
    ASSERT_EQ(ev.ascii, 'A');
    ASSERT_TRUE(ev.shift);
}

/// Verify break code produces ascii=0 (no translation on release)
TEST("keyboard: 'a' break code produces ascii=0") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(0x1E, ev);  // press 'a'
    decode_scancode(0x9E, ev);  // release 'a'
    ASSERT_EQ(ev.ascii, 0);
    ASSERT_FALSE(ev.pressed);
}

/// Verify shift-release restores lowercase for subsequent keys
TEST("keyboard: shift release restores lowercase for next key") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::LSHIFT, ev);  // shift press
    decode_scancode(0x1E, ev);              // 'a' press -> 'A'
    ASSERT_EQ(ev.ascii, 'A');
    decode_scancode(0xAA, ev);              // shift release
    decode_scancode(0x1E, ev);              // 'a' press -> 'a'
    ASSERT_EQ(ev.ascii, 'a');
}

/// Verify '1' with shift produces '!'
TEST("keyboard: '1' with shift -> '!'") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::LSHIFT, ev);
    decode_scancode(0x02, ev);  // '1' press with shift
    ASSERT_EQ(ev.ascii, '!');
}

/// Verify ';' with shift produces ':'
TEST("keyboard: ';' with shift -> ':'") {
    modifier_reset();
    KeyEvent ev{};
    decode_scancode(ScanCode::LSHIFT, ev);
    decode_scancode(0x27, ev);  // ';' = 0x27
    ASSERT_EQ(ev.ascii, ':');
}

/// Verify number row produces correct digits without shift
TEST("keyboard: number row 1-9,0 produces correct digits") {
    modifier_reset();
    // 0x02='1', 0x03='2', ..., 0x09='9', 0x0A='0' -- wait, 0x0B='0'
    // Actually: 0x02='1', 0x03='2', 0x04='3', 0x05='4',
    //           0x06='5', 0x07='6', 0x08='7', 0x09='8',
    //           0x0A='9', 0x0B='0'
    ASSERT_EQ(kScToLower[0x02], '1');
    ASSERT_EQ(kScToLower[0x03], '2');
    ASSERT_EQ(kScToLower[0x04], '3');
    ASSERT_EQ(kScToLower[0x05], '4');
    ASSERT_EQ(kScToLower[0x06], '5');
    ASSERT_EQ(kScToLower[0x07], '6');
    ASSERT_EQ(kScToLower[0x08], '7');
    ASSERT_EQ(kScToLower[0x09], '8');
    ASSERT_EQ(kScToLower[0x0A], '9');
    ASSERT_EQ(kScToLower[0x0B], '0');
}

// ============================================================
// 7. Extended Scan Code Prefix (0xE0)
// ============================================================

/// Verify 0xE0 is recognized as an extended prefix (decode returns false)
TEST("keyboard: extended prefix 0xE0 is skipped") {
    KeyEvent ev{};
    bool result = decode_scancode(ScanCode::EXTENDED, ev);
    ASSERT_FALSE(result);
}

// ============================================================
// 8. Ring Buffer: Basic Operations
// ============================================================

/// Verify empty buffer returns false on poll
TEST("keyboard: ring buffer empty returns false on poll") {
    ring_reset();
    KeyEvent ev{};
    ASSERT_FALSE(ring_poll(ev));
}

/// Verify enqueue then poll retrieves the event
TEST("keyboard: ring buffer enqueue then poll") {
    ring_reset();
    KeyEvent in{};
    in.ascii    = 'X';
    in.scancode = 0x2D;
    in.pressed  = true;

    ring_enqueue(in);

    KeyEvent out{};
    ASSERT_TRUE(ring_poll(out));
    ASSERT_EQ(out.ascii, 'X');
    ASSERT_EQ(out.scancode, 0x2Du);
    ASSERT_TRUE(out.pressed);
}

/// Verify poll on now-empty buffer returns false after draining
TEST("keyboard: ring buffer empty after draining single event") {
    ring_reset();
    KeyEvent ev{};
    ring_enqueue(ev);
    ring_poll(ev);
    ASSERT_FALSE(ring_poll(ev));
}

/// Verify FIFO order: first in, first out
TEST("keyboard: ring buffer preserves FIFO order") {
    ring_reset();
    KeyEvent a{};
    a.ascii = 'A';
    KeyEvent b{};
    b.ascii = 'B';
    KeyEvent c{};
    c.ascii = 'C';

    ring_enqueue(a);
    ring_enqueue(b);
    ring_enqueue(c);

    KeyEvent out{};
    ring_poll(out);
    ASSERT_EQ(out.ascii, 'A');
    ring_poll(out);
    ASSERT_EQ(out.ascii, 'B');
    ring_poll(out);
    ASSERT_EQ(out.ascii, 'C');
}

// ============================================================
// 9. Ring Buffer: Full Buffer Drops Events
// ============================================================

/// Verify a full buffer drops the (KEY_QUEUE_SIZE)th event
TEST("keyboard: ring buffer full drops event") {
    ring_reset();

    // The ring buffer can hold KEY_QUEUE_SIZE - 1 items (one slot wasted)
    // because tail+1 == head means full.
    uint32_t capacity = KEY_QUEUE_SIZE - 1;

    KeyEvent dummy{};
    dummy.ascii = 'D';

    for (uint32_t i = 0; i < capacity; i++) {
        ring_enqueue(dummy);
    }

    // Buffer is now full.  The next enqueue should be dropped.
    KeyEvent overflow{};
    overflow.ascii = 'Z';
    ring_enqueue(overflow);

    // Drain all events -- there should be exactly `capacity` events
    uint32_t count = 0;
    KeyEvent out{};
    while (ring_poll(out)) {
        count++;
    }
    ASSERT_EQ(count, capacity);
}

// ============================================================
// 10. Ring Buffer: Wrap-Around
// ============================================================

/// Verify ring buffer wraps around correctly after reaching end of array
TEST("keyboard: ring buffer wrap-around correctness") {
    ring_reset();

    // Fill to capacity, drain most of it, then fill again.
    // This forces head_ and tail_ to wrap around the array boundary.
    uint32_t capacity = KEY_QUEUE_SIZE - 1;

    KeyEvent dummy{};
    dummy.ascii = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        ring_enqueue(dummy);
    }

    // Drain all but 1
    KeyEvent out{};
    for (uint32_t i = 0; i < capacity - 1; i++) {
        ring_poll(out);
    }

    // Now enqueue several more (these will wrap around)
    for (uint32_t i = 0; i < 10; i++) {
        dummy.ascii = static_cast<char>('0' + i);
        ring_enqueue(dummy);
    }

    // Drain everything and verify no corruption (we should get 11 total)
    uint32_t count = 0;
    while (ring_poll(out)) {
        count++;
    }
    ASSERT_EQ(count, 11u);  // 1 remaining + 10 new
}

// ============================================================
// 11. Ring Buffer: Reset Clears State
// ============================================================

/// Verify ring_reset() clears the buffer completely
TEST("keyboard: ring_reset clears buffer") {
    ring_reset();
    KeyEvent ev{};
    ring_enqueue(ev);
    ring_enqueue(ev);
    ring_reset();
    ASSERT_FALSE(ring_poll(ev));
}

// ============================================================
// 12. Full Decode Sequence (Shift + Key + Release)
// ============================================================

/// Simulate a full Shift+A (capital A) keystroke sequence
TEST("keyboard: full Shift+A sequence decodes correctly") {
    modifier_reset();
    ring_reset();

    KeyEvent ev{};

    // Press LShift
    decode_scancode(ScanCode::LSHIFT, ev);
    ring_enqueue(ev);

    // Press 'a' key
    decode_scancode(0x1E, ev);
    ring_enqueue(ev);

    // Release 'a' key
    decode_scancode(0x9E, ev);
    ring_enqueue(ev);

    // Release LShift
    decode_scancode(0xAA, ev);
    ring_enqueue(ev);

    // Dequeue and verify
    KeyEvent out{};

    // Event 1: LShift press
    ring_poll(out);
    ASSERT_EQ(out.scancode, ScanCode::LSHIFT);
    ASSERT_TRUE(out.pressed);
    ASSERT_TRUE(out.shift);

    // Event 2: 'A' press (shifted)
    ring_poll(out);
    ASSERT_EQ(out.ascii, 'A');
    ASSERT_TRUE(out.pressed);
    ASSERT_TRUE(out.shift);

    // Event 3: 'a' release (no ASCII on release)
    ring_poll(out);
    ASSERT_EQ(out.ascii, 0);
    ASSERT_FALSE(out.pressed);
    ASSERT_TRUE(out.shift);  // shift still held at time of release

    // Event 4: LShift release
    ring_poll(out);
    ASSERT_EQ(out.scancode, 0xAAu);
    ASSERT_FALSE(out.pressed);
    ASSERT_FALSE(out.shift);

    // Buffer should now be empty
    ASSERT_FALSE(ring_poll(out));
}

// ============================================================
// Main Function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
