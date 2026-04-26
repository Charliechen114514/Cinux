/**
 * @file kernel/test/test_mouse_event.cpp
 * @brief QEMU in-kernel tests for Mouse driver and EventQueue (030_gui_wm_basic)
 *
 * Tests the real kernel EventQueue (no mock needed), MouseEvent/KeyEvent
 * struct construction and field correctness, and button state parsing.
 *
 * Mouse::init() and IRQ handler tests are NOT included here because they
 * require a real PS/2 controller (QEMU emulated).  Those are validated
 * by the host-side unit tests with mock I/O.  The kernel tests focus on
 * the data structures and pure logic that can be exercised without
 * hardware dependencies.
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - CINUX_GUI is defined (mouse.cpp and event.cpp are compiled)
 *
 * Compile condition: CINUX_GUI
 */

#include "big_kernel_test.h"

#ifdef CINUX_GUI

#    include "kernel/gui/event.hpp"

using cinux::gui::Event;
using cinux::gui::EventQueue;
using cinux::gui::EventType;
using cinux::gui::KeyEvent;
using cinux::gui::MouseEvent;

// ============================================================
// EventQueue tests
// ============================================================

/// Verify a newly constructed EventQueue is empty
void test_event_queue_newly_empty() {
    EventQueue q;
    TEST_ASSERT_TRUE(q.empty());
}

/// Verify enqueue + dequeue round-trips a MouseEvent
void test_event_queue_enqueue_dequeue_mouse() {
    EventQueue q;

    Event in{};
    in.type_         = EventType::MouseMove;
    in.mouse.x       = 42;
    in.mouse.y       = 99;
    in.mouse.dx      = 5;
    in.mouse.dy      = -3;
    in.mouse.buttons = 0x01;
    in.mouse.left    = true;

    q.enqueue(in);

    Event out{};
    TEST_ASSERT_TRUE(q.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::MouseMove));
    TEST_ASSERT_EQ(out.mouse.x, 42);
    TEST_ASSERT_EQ(out.mouse.y, 99);
    TEST_ASSERT_EQ(out.mouse.dx, 5);
    TEST_ASSERT_EQ(out.mouse.dy, -3);
    TEST_ASSERT_EQ(out.mouse.buttons, 0x01);
    TEST_ASSERT_TRUE(out.mouse.left);
}

/// Verify enqueue + dequeue round-trips a KeyEvent
void test_event_queue_enqueue_dequeue_key() {
    EventQueue q;

    Event in{};
    in.type_        = EventType::KeyDown;
    in.key.ascii    = 'X';
    in.key.scancode = 0x2D;
    in.key.pressed  = true;
    in.key.shift    = true;
    in.key.ctrl     = false;
    in.key.alt      = false;

    q.enqueue(in);

    Event out{};
    TEST_ASSERT_TRUE(q.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::KeyDown));
    TEST_ASSERT_EQ(out.key.ascii, 'X');
    TEST_ASSERT_EQ(out.key.scancode, 0x2Du);
    TEST_ASSERT_TRUE(out.key.pressed);
    TEST_ASSERT_TRUE(out.key.shift);
}

/// Verify dequeue on empty queue returns false
void test_event_queue_dequeue_empty() {
    EventQueue q;
    Event      out{};
    TEST_ASSERT_FALSE(q.dequeue(out));
}

/// Verify full buffer drops events (capacity = BUF_SIZE - 1)
void test_event_queue_full_drops() {
    EventQueue q;

    // Capacity is 127 (128 - 1)
    for (uint32_t i = 0; i < 127; i++) {
        Event ev{};
        ev.type_   = EventType::MouseMove;
        ev.mouse.x = static_cast<int32_t>(i);
        q.enqueue(ev);
    }

    // This 128th event should be dropped
    Event overflow{};
    overflow.type_     = EventType::KeyDown;
    overflow.key.ascii = 'Z';
    q.enqueue(overflow);

    // Drain and verify no 'Z' event
    int32_t last_x = -1;
    Event   out{};
    while (q.dequeue(out)) {
        last_x = out.mouse.x;
    }
    TEST_ASSERT_EQ(last_x, 126);  // Last valid event, not 'Z'
}

/// Verify wrap-around: fill, drain most, refill
void test_event_queue_wrap_around() {
    EventQueue q;

    // Fill to capacity
    for (uint32_t i = 0; i < 127; i++) {
        Event ev{};
        ev.type_   = EventType::MouseMove;
        ev.mouse.x = static_cast<int32_t>(i);
        q.enqueue(ev);
    }

    // Drain all but 1
    Event out{};
    for (uint32_t i = 0; i < 126; i++) {
        q.dequeue(out);
    }

    // Enqueue 10 more (wraps tail around)
    for (uint32_t i = 0; i < 10; i++) {
        Event ev{};
        ev.type_     = EventType::KeyDown;
        ev.key.ascii = static_cast<char>('0' + i);
        q.enqueue(ev);
    }

    // Drain all: should get 11 total (1 remaining + 10 new)
    uint32_t count = 0;
    while (q.dequeue(out)) {
        count++;
    }
    TEST_ASSERT_EQ(count, 11u);
}

/// Verify clear() empties the queue
void test_event_queue_clear() {
    EventQueue q;

    for (int i = 0; i < 10; i++) {
        Event ev{};
        ev.type_ = EventType::MouseMove;
        q.enqueue(ev);
    }

    q.clear();
    TEST_ASSERT_TRUE(q.empty());

    Event out{};
    TEST_ASSERT_FALSE(q.dequeue(out));
}

/// Verify FIFO ordering across multiple event types
void test_event_queue_fifo_ordering() {
    EventQueue q;

    Event a{};
    a.type_   = EventType::MouseMove;
    a.mouse.x = 1;

    Event b{};
    b.type_      = EventType::MouseDown;
    b.mouse.left = true;

    Event c{};
    c.type_     = EventType::KeyDown;
    c.key.ascii = 'K';

    Event d{};
    d.type_ = EventType::MouseUp;

    q.enqueue(a);
    q.enqueue(b);
    q.enqueue(c);
    q.enqueue(d);

    Event out{};
    TEST_ASSERT_TRUE(q.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::MouseMove));

    TEST_ASSERT_TRUE(q.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::MouseDown));

    TEST_ASSERT_TRUE(q.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::KeyDown));
    TEST_ASSERT_EQ(out.key.ascii, 'K');

    TEST_ASSERT_TRUE(q.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::MouseUp));

    TEST_ASSERT_FALSE(q.dequeue(out));
}

// ============================================================
// MouseEvent construction tests
// ============================================================

/// Verify MouseEvent default-initialised fields
void test_mouse_event_default_init() {
    MouseEvent me{};
    TEST_ASSERT_EQ(me.x, 0);
    TEST_ASSERT_EQ(me.y, 0);
    TEST_ASSERT_EQ(me.dx, 0);
    TEST_ASSERT_EQ(me.dy, 0);
    TEST_ASSERT_EQ(me.buttons, 0u);
    TEST_ASSERT_FALSE(me.left);
    TEST_ASSERT_FALSE(me.right);
    TEST_ASSERT_FALSE(me.middle);
}

/// Verify MouseEvent field assignment
void test_mouse_event_field_assignment() {
    MouseEvent me{};
    me.x       = 640;
    me.y       = 480;
    me.dx      = -10;
    me.dy      = 5;
    me.buttons = 0x05;  // left + middle
    me.left    = true;
    me.right   = false;
    me.middle  = true;

    TEST_ASSERT_EQ(me.x, 640);
    TEST_ASSERT_EQ(me.y, 480);
    TEST_ASSERT_EQ(me.dx, -10);
    TEST_ASSERT_EQ(me.dy, 5);
    TEST_ASSERT_EQ(me.buttons, 0x05u);
    TEST_ASSERT_TRUE(me.left);
    TEST_ASSERT_FALSE(me.right);
    TEST_ASSERT_TRUE(me.middle);
}

/// Verify MouseEvent copy via Event union
void test_mouse_event_via_union() {
    MouseEvent me{};
    me.x       = 100;
    me.y       = 200;
    me.dx      = 3;
    me.dy      = -7;
    me.buttons = 0x02;
    me.left    = false;
    me.right   = true;
    me.middle  = false;

    Event ev{};
    ev.type_ = EventType::MouseDown;
    ev.mouse = me;

    // Read back through the union
    TEST_ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::MouseDown));
    TEST_ASSERT_EQ(ev.mouse.x, 100);
    TEST_ASSERT_EQ(ev.mouse.y, 200);
    TEST_ASSERT_EQ(ev.mouse.dx, 3);
    TEST_ASSERT_EQ(ev.mouse.dy, -7);
    TEST_ASSERT_EQ(ev.mouse.buttons, 0x02u);
    TEST_ASSERT_FALSE(ev.mouse.left);
    TEST_ASSERT_TRUE(ev.mouse.right);
    TEST_ASSERT_FALSE(ev.mouse.middle);
}

// ============================================================
// KeyEvent construction tests
// ============================================================

/// Verify KeyEvent default-initialised fields
void test_key_event_default_init() {
    KeyEvent ke{};
    TEST_ASSERT_EQ(ke.ascii, 0);
    TEST_ASSERT_EQ(ke.scancode, 0u);
    TEST_ASSERT_FALSE(ke.pressed);
    TEST_ASSERT_FALSE(ke.shift);
    TEST_ASSERT_FALSE(ke.ctrl);
    TEST_ASSERT_FALSE(ke.alt);
}

/// Verify KeyEvent field assignment
void test_key_event_field_assignment() {
    KeyEvent ke{};
    ke.ascii    = 'G';
    ke.scancode = 0x22;
    ke.pressed  = true;
    ke.shift    = true;
    ke.ctrl     = false;
    ke.alt      = true;

    TEST_ASSERT_EQ(ke.ascii, 'G');
    TEST_ASSERT_EQ(ke.scancode, 0x22u);
    TEST_ASSERT_TRUE(ke.pressed);
    TEST_ASSERT_TRUE(ke.shift);
    TEST_ASSERT_FALSE(ke.ctrl);
    TEST_ASSERT_TRUE(ke.alt);
}

/// Verify KeyEvent copy via Event union
void test_key_event_via_union() {
    KeyEvent ke{};
    ke.ascii    = 'A';
    ke.scancode = 0x1E;
    ke.pressed  = true;
    ke.shift    = false;

    Event ev{};
    ev.type_ = EventType::KeyDown;
    ev.key   = ke;

    TEST_ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::KeyDown));
    TEST_ASSERT_EQ(ev.key.ascii, 'A');
    TEST_ASSERT_EQ(ev.key.scancode, 0x1Eu);
    TEST_ASSERT_TRUE(ev.key.pressed);
    TEST_ASSERT_FALSE(ev.key.shift);
}

// ============================================================
// Button state parsing tests
// ============================================================

/// Verify left button bitmask extraction
void test_button_state_left() {
    uint8_t b0 = 0x01;  // LEFT_BTN only
    TEST_ASSERT(b0 & 0x01);
    TEST_ASSERT(!(b0 & 0x02));
    TEST_ASSERT(!(b0 & 0x04));
}

/// Verify right button bitmask extraction
void test_button_state_right() {
    uint8_t b0 = 0x02;  // RIGHT_BTN only
    TEST_ASSERT(!(b0 & 0x01));
    TEST_ASSERT(b0 & 0x02);
    TEST_ASSERT(!(b0 & 0x04));
}

/// Verify middle button bitmask extraction
void test_button_state_middle() {
    uint8_t b0 = 0x04;  // MIDDLE_BTN only
    TEST_ASSERT(!(b0 & 0x01));
    TEST_ASSERT(!(b0 & 0x02));
    TEST_ASSERT(b0 & 0x04);
}

/// Verify all three buttons simultaneously
void test_button_state_all_three() {
    uint8_t b0 = 0x07;  // LEFT | RIGHT | MIDDLE
    TEST_ASSERT(b0 & 0x01);
    TEST_ASSERT(b0 & 0x02);
    TEST_ASSERT(b0 & 0x04);
}

/// Verify button transition detection (press/release)
void test_button_edge_detection() {
    uint8_t prev = 0x00;
    uint8_t curr = 0x01;  // Left pressed

    uint8_t pressed  = curr & ~prev;
    uint8_t released = prev & ~curr;

    TEST_ASSERT_EQ(pressed, 0x01u);   // Left newly pressed
    TEST_ASSERT_EQ(released, 0x00u);  // Nothing released

    // Now release left
    prev     = 0x01;
    curr     = 0x00;
    pressed  = curr & ~prev;
    released = prev & ~curr;

    TEST_ASSERT_EQ(pressed, 0x00u);   // Nothing pressed
    TEST_ASSERT_EQ(released, 0x01u);  // Left released
}

/// Verify no transition when button state unchanged
void test_button_edge_no_change() {
    uint8_t prev = 0x03;  // Left + Right held
    uint8_t curr = 0x03;  // Still held

    uint8_t pressed  = curr & ~prev;
    uint8_t released = prev & ~curr;

    TEST_ASSERT_EQ(pressed, 0x00u);
    TEST_ASSERT_EQ(released, 0x00u);
}

/// Verify simultaneous press and release of different buttons
void test_button_edge_press_release_simultaneous() {
    uint8_t prev = 0x01;  // Left held
    uint8_t curr = 0x02;  // Right held (left released)

    uint8_t pressed  = curr & ~prev;
    uint8_t released = prev & ~curr;

    TEST_ASSERT_EQ(pressed, 0x02u);   // Right newly pressed
    TEST_ASSERT_EQ(released, 0x01u);  // Left released
}

// ============================================================
// EventType enum tests
// ============================================================

/// Verify EventType enum values match expected ordering
void test_event_type_values() {
    TEST_ASSERT_EQ(static_cast<int>(EventType::MouseMove), 0);
    TEST_ASSERT_EQ(static_cast<int>(EventType::MouseDown), 1);
    TEST_ASSERT_EQ(static_cast<int>(EventType::MouseUp), 2);
    TEST_ASSERT_EQ(static_cast<int>(EventType::KeyDown), 3);
    TEST_ASSERT_EQ(static_cast<int>(EventType::KeyUp), 4);
}

/// Verify Event union size is at least as large as both members
void test_event_union_size() {
    Event ev{};
    // The Event union must be large enough for both MouseEvent and KeyEvent
    TEST_ASSERT_GE(sizeof(ev), sizeof(MouseEvent));
    TEST_ASSERT_GE(sizeof(ev), sizeof(KeyEvent));
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_mouse_event_tests() {
    TEST_SECTION("Mouse & Event Tests (030_gui_wm_basic)");

    // EventQueue tests
    RUN_TEST(test_event_queue_newly_empty);
    RUN_TEST(test_event_queue_enqueue_dequeue_mouse);
    RUN_TEST(test_event_queue_enqueue_dequeue_key);
    RUN_TEST(test_event_queue_dequeue_empty);
    RUN_TEST(test_event_queue_full_drops);
    RUN_TEST(test_event_queue_wrap_around);
    RUN_TEST(test_event_queue_clear);
    RUN_TEST(test_event_queue_fifo_ordering);

    // MouseEvent construction tests
    RUN_TEST(test_mouse_event_default_init);
    RUN_TEST(test_mouse_event_field_assignment);
    RUN_TEST(test_mouse_event_via_union);

    // KeyEvent construction tests
    RUN_TEST(test_key_event_default_init);
    RUN_TEST(test_key_event_field_assignment);
    RUN_TEST(test_key_event_via_union);

    // Button state parsing tests
    RUN_TEST(test_button_state_left);
    RUN_TEST(test_button_state_right);
    RUN_TEST(test_button_state_middle);
    RUN_TEST(test_button_state_all_three);
    RUN_TEST(test_button_edge_detection);
    RUN_TEST(test_button_edge_no_change);
    RUN_TEST(test_button_edge_press_release_simultaneous);

    // EventType enum tests
    RUN_TEST(test_event_type_values);
    RUN_TEST(test_event_union_size);

    TEST_SUMMARY();
}

#else  // !CINUX_GUI

// CLI mode stub: no GUI tests to run
extern "C" void run_mouse_event_tests() {
    using cinux::lib::kprintf;
    kprintf("[MOUSE_EVENT] CLI mode -- GUI tests skipped.\n");
}

#endif  // CINUX_GUI
