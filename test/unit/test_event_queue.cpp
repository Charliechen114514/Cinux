/**
 * @file test/unit/test_event_queue.cpp
 * @brief Host-side unit tests for GUI EventQueue ring buffer
 *
 * Test coverage:
 *   - enqueue/dequeue basic functionality
 *   - Ring buffer full: enqueue drops event silently
 *   - Ring buffer empty: dequeue returns false
 *   - Wrap-around behaviour (fill, drain, refill)
 *   - clear() resets head and tail
 *   - empty() reflects queue state
 *   - FIFO ordering preserved across wrap-around
 *   - Mixed mouse and keyboard events in the same queue
 *
 * The EventQueue implementation is replicated from kernel/gui/event.cpp
 * to allow pure host-side testing without linking kernel code.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>

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
    int32_t  x;
    int32_t  y;
    int32_t  dx;
    int32_t  dy;
    uint8_t  buttons;
    bool     left;
    bool     right;
    bool     middle;
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

static constexpr uint32_t BUF_SIZE = 128;

struct EventQueue {
    Event    buf_[BUF_SIZE]{};
    uint32_t head_ = 0;
    uint32_t tail_ = 0;

    void enqueue(const Event& ev) {
        uint32_t next = (tail_ + 1) % BUF_SIZE;
        if (next == head_) {
            return;  // drop on full
        }
        buf_[tail_] = ev;
        tail_ = next;
    }

    bool dequeue(Event& out) {
        if (head_ == tail_) {
            return false;
        }
        out = buf_[head_];
        head_ = (head_ + 1) % BUF_SIZE;
        return true;
    }

    bool empty() const {
        return head_ == tail_;
    }

    void clear() {
        head_ = 0;
        tail_ = 0;
    }
};

// ============================================================
// Helper: create a MouseEvent
// ============================================================

static Event make_mouse_move(int32_t x, int32_t y, int32_t dx, int32_t dy) {
    Event ev{};
    ev.type_ = EventType::MouseMove;
    ev.mouse.x = x;
    ev.mouse.y = y;
    ev.mouse.dx = dx;
    ev.mouse.dy = dy;
    ev.mouse.buttons = 0;
    ev.mouse.left = false;
    ev.mouse.right = false;
    ev.mouse.middle = false;
    return ev;
}

static Event make_mouse_down(int32_t x, int32_t y, uint8_t buttons) {
    Event ev{};
    ev.type_ = EventType::MouseDown;
    ev.mouse.x = x;
    ev.mouse.y = y;
    ev.mouse.dx = 0;
    ev.mouse.dy = 0;
    ev.mouse.buttons = buttons;
    ev.mouse.left = (buttons & 0x01) != 0;
    ev.mouse.right = (buttons & 0x02) != 0;
    ev.mouse.middle = (buttons & 0x04) != 0;
    return ev;
}

static Event make_key_down(char ascii, uint8_t scancode) {
    Event ev{};
    ev.type_ = EventType::KeyDown;
    ev.key.ascii = ascii;
    ev.key.scancode = scancode;
    ev.key.pressed = true;
    ev.key.shift = false;
    ev.key.ctrl = false;
    ev.key.alt = false;
    return ev;
}

// ============================================================
// 1. Basic enqueue/dequeue
// ============================================================

/// Verify empty queue returns false on dequeue
TEST("event_queue: empty queue dequeue returns false") {
    EventQueue q;
    Event ev;
    ASSERT_FALSE(q.dequeue(ev));
}

/// Verify enqueue then dequeue retrieves the event
TEST("event_queue: enqueue then dequeue retrieves event") {
    EventQueue q;
    Event in = make_mouse_move(10, 20, 5, 3);
    q.enqueue(in);

    Event out;
    ASSERT_TRUE(q.dequeue(out));
    ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::MouseMove));
    ASSERT_EQ(out.mouse.x, 10);
    ASSERT_EQ(out.mouse.y, 20);
    ASSERT_EQ(out.mouse.dx, 5);
    ASSERT_EQ(out.mouse.dy, 3);
}

/// Verify queue reports empty after draining all events
TEST("event_queue: empty after draining all events") {
    EventQueue q;
    q.enqueue(make_mouse_move(0, 0, 1, 1));
    q.enqueue(make_mouse_move(1, 1, 2, 2));

    Event ev;
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_FALSE(q.dequeue(ev));
}

// ============================================================
// 2. FIFO ordering
// ============================================================

/// Verify FIFO order: first in, first out
TEST("event_queue: FIFO ordering") {
    EventQueue q;
    q.enqueue(make_key_down('A', 0x1E));
    q.enqueue(make_key_down('B', 0x30));
    q.enqueue(make_mouse_move(5, 10, 1, 1));

    Event ev;
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::KeyDown));
    ASSERT_EQ(ev.key.ascii, 'A');

    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::KeyDown));
    ASSERT_EQ(ev.key.ascii, 'B');

    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::MouseMove));
    ASSERT_EQ(ev.mouse.x, 5);
}

// ============================================================
// 3. Ring buffer full: enqueue drops event
// ============================================================

/// Verify full buffer drops the (BUF_SIZE)th event
TEST("event_queue: full buffer drops event") {
    EventQueue q;

    // Capacity is BUF_SIZE - 1 (one slot wasted for empty detection)
    uint32_t capacity = BUF_SIZE - 1;

    for (uint32_t i = 0; i < capacity; i++) {
        q.enqueue(make_mouse_move(static_cast<int32_t>(i), 0, 1, 0));
    }

    // Buffer is full.  Next enqueue should be dropped.
    Event overflow = make_mouse_move(999, 999, 0, 0);
    q.enqueue(overflow);

    // Drain and count
    uint32_t count = 0;
    Event ev;
    while (q.dequeue(ev)) {
        count++;
    }
    ASSERT_EQ(count, capacity);
}

/// Verify full buffer: no data corruption in existing events
TEST("event_queue: full buffer preserves existing events") {
    EventQueue q;

    uint32_t capacity = BUF_SIZE - 1;
    for (uint32_t i = 0; i < capacity; i++) {
        q.enqueue(make_mouse_move(static_cast<int32_t>(i), 0, 1, 0));
    }

    // Drop one
    q.enqueue(make_mouse_move(999, 999, 0, 0));

    // Verify all original events are intact
    Event ev;
    for (uint32_t i = 0; i < capacity; i++) {
        ASSERT_TRUE(q.dequeue(ev));
        ASSERT_EQ(ev.mouse.x, static_cast<int32_t>(i));
    }
}

// ============================================================
// 4. Wrap-around behaviour
// ============================================================

/// Verify wrap-around: fill, drain most, refill
TEST("event_queue: wrap-around fill-drain-refill") {
    EventQueue q;

    uint32_t capacity = BUF_SIZE - 1;

    // Fill to capacity
    for (uint32_t i = 0; i < capacity; i++) {
        q.enqueue(make_mouse_move(static_cast<int32_t>(i), 0, 1, 0));
    }

    // Drain all but 1 (forces head forward, tail stays at capacity-1)
    Event ev;
    for (uint32_t i = 0; i < capacity - 1; i++) {
        q.dequeue(ev);
    }

    // Enqueue several more (wraps tail around to 0)
    for (uint32_t i = 0; i < 10; i++) {
        q.enqueue(make_key_down(static_cast<char>('0' + i), 0));
    }

    // Drain everything: should get 11 total (1 remaining + 10 new)
    uint32_t count = 0;
    while (q.dequeue(ev)) {
        count++;
    }
    ASSERT_EQ(count, 11u);
}

/// Verify FIFO ordering preserved across wrap-around boundary
TEST("event_queue: FIFO preserved across wrap-around") {
    EventQueue q;

    uint32_t capacity = BUF_SIZE - 1;

    // Fill to capacity
    for (uint32_t i = 0; i < capacity; i++) {
        q.enqueue(make_mouse_move(static_cast<int32_t>(i), 0, 1, 0));
    }

    // Drain all
    Event ev;
    for (uint32_t i = 0; i < capacity; i++) {
        q.dequeue(ev);
    }

    // Enqueue new events (head and tail are both 0, fresh start)
    q.enqueue(make_key_down('X', 1));
    q.enqueue(make_key_down('Y', 2));
    q.enqueue(make_key_down('Z', 3));

    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(ev.key.ascii, 'X');
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(ev.key.ascii, 'Y');
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(ev.key.ascii, 'Z');
}

/// Verify multiple wrap-around cycles
TEST("event_queue: multiple wrap-around cycles") {
    EventQueue q;

    // Do 3 full fill-drain cycles
    for (int cycle = 0; cycle < 3; cycle++) {
        uint32_t capacity = BUF_SIZE - 1;
        for (uint32_t i = 0; i < capacity; i++) {
            q.enqueue(make_mouse_move(static_cast<int32_t>(cycle * 100 + static_cast<int>(i)), 0, 1, 0));
        }

        Event ev;
        uint32_t count = 0;
        while (q.dequeue(ev)) {
            count++;
        }
        ASSERT_EQ(count, capacity);
    }
}

// ============================================================
// 5. clear() functionality
// ============================================================

/// Verify clear() empties a non-empty queue
TEST("event_queue: clear empties non-empty queue") {
    EventQueue q;

    q.enqueue(make_mouse_move(10, 20, 1, 1));
    q.enqueue(make_mouse_move(15, 25, 5, 5));
    q.enqueue(make_key_down('A', 0x1E));

    q.clear();

    Event ev;
    ASSERT_FALSE(q.dequeue(ev));
    ASSERT_TRUE(q.empty());
}

/// Verify clear() on already empty queue is safe
TEST("event_queue: clear on empty queue is safe") {
    EventQueue q;
    q.clear();

    Event ev;
    ASSERT_FALSE(q.dequeue(ev));
    ASSERT_TRUE(q.empty());
}

/// Verify clear() then enqueue works correctly
TEST("event_queue: clear then enqueue works") {
    EventQueue q;

    q.enqueue(make_mouse_move(1, 2, 3, 4));
    q.clear();
    q.enqueue(make_key_down('Z', 0x2C));

    Event ev;
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(ev.key.ascii, 'Z');
    ASSERT_FALSE(q.dequeue(ev));
}

// ============================================================
// 6. empty() reflects queue state
// ============================================================

/// Verify empty() returns true for newly constructed queue
TEST("event_queue: newly constructed queue is empty") {
    EventQueue q;
    ASSERT_TRUE(q.empty());
}

/// Verify empty() returns false after enqueue
TEST("event_queue: not empty after enqueue") {
    EventQueue q;
    q.enqueue(make_mouse_move(0, 0, 1, 0));
    ASSERT_FALSE(q.empty());
}

/// Verify empty() returns true after dequeue of last element
TEST("event_queue: empty after dequeue of last element") {
    EventQueue q;
    q.enqueue(make_mouse_move(0, 0, 1, 0));

    Event ev;
    q.dequeue(ev);
    ASSERT_TRUE(q.empty());
}

/// Verify empty() after clear
TEST("event_queue: empty after clear") {
    EventQueue q;
    q.enqueue(make_mouse_move(0, 0, 1, 0));
    q.clear();
    ASSERT_TRUE(q.empty());
}

// ============================================================
// 7. Mixed event types in the same queue
// ============================================================

/// Verify mouse and keyboard events coexist in the queue
TEST("event_queue: mixed mouse and keyboard events") {
    EventQueue q;

    q.enqueue(make_mouse_move(10, 10, 5, 3));
    q.enqueue(make_key_down('H', 0x23));
    q.enqueue(make_mouse_down(10, 10, 0x01));
    q.enqueue(make_key_down('i', 0x17));

    Event ev;

    // MouseMove
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::MouseMove));

    // KeyDown 'H'
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::KeyDown));
    ASSERT_EQ(ev.key.ascii, 'H');

    // MouseDown
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::MouseDown));
    ASSERT_TRUE(ev.mouse.left);

    // KeyDown 'i'
    ASSERT_TRUE(q.dequeue(ev));
    ASSERT_EQ(static_cast<int>(ev.type_), static_cast<int>(EventType::KeyDown));
    ASSERT_EQ(ev.key.ascii, 'i');

    ASSERT_FALSE(q.dequeue(ev));
}

// ============================================================
// 8. Edge cases
// ============================================================

/// Verify zero-valued event can be enqueued and dequeued
TEST("event_queue: zero-valued event round-trips") {
    EventQueue q;

    Event ev{};
    // ev is all zeros: type_=MouseMove (0), all fields 0
    q.enqueue(ev);

    Event out;
    ASSERT_TRUE(q.dequeue(out));
    ASSERT_EQ(static_cast<int>(out.type_), 0);
    ASSERT_EQ(out.mouse.x, 0);
    ASSERT_EQ(out.mouse.y, 0);
}

/// Verify enqueue after full buffer then dequeue then enqueue works
TEST("event_queue: enqueue-dequeue-enqueue after full") {
    EventQueue q;

    uint32_t capacity = BUF_SIZE - 1;
    for (uint32_t i = 0; i < capacity; i++) {
        q.enqueue(make_mouse_move(static_cast<int32_t>(i), 0, 1, 0));
    }

    // Dequeue one
    Event ev;
    q.dequeue(ev);

    // Now there is room for one more
    q.enqueue(make_key_down('N', 0x31));

    // Dequeue all and verify
    uint32_t count = 0;
    while (q.dequeue(ev)) {
        count++;
    }
    ASSERT_EQ(count, capacity);  // capacity-1 remaining + 1 new = capacity
}

/// Verify large number of enqueue/dequeue cycles
TEST("event_queue: 1000 enqueue-dequeue cycles") {
    EventQueue q;

    for (int i = 0; i < 1000; i++) {
        q.enqueue(make_mouse_move(i, i, 1, 1));
        Event ev;
        ASSERT_TRUE(q.dequeue(ev));
        ASSERT_EQ(ev.mouse.x, i);
    }
    ASSERT_TRUE(q.empty());
}

// ============================================================
// Main Function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
