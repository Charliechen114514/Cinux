/**
 * @file kernel/gui/event.hpp
 * @brief GUI event types, structures, and ring-buffer event queue
 *
 * Defines the unified event system used by the GUI subsystem.
 * Mouse and keyboard events are dispatched through a single
 * EventQueue (ring buffer) so that the window manager can poll
 * all input from one place.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

#include <stdint.h>

namespace cinux::gui {

// ============================================================
// Event type enumeration
// ============================================================

/**
 * @brief Discriminant for the Event union
 *
 * Each variant corresponds to a category of user input.
 */
enum class EventType : uint8_t {
    MouseMove = 0,  ///< Mouse cursor moved
    MouseDown,      ///< Mouse button pressed
    MouseUp,        ///< Mouse button released
    KeyDown,        ///< Keyboard key pressed
    KeyUp,          ///< Keyboard key released
};

// ============================================================
// Mouse event data
// ============================================================

/**
 * @brief Decoded mouse event
 *
 * Carries the absolute cursor position after movement, a bitmask
 * of currently-pressed buttons, and individual convenience booleans
 * for the three standard PS/2 buttons.
 */
struct MouseEvent {
    int32_t  x;        ///< Absolute cursor X (pixels, clamped to screen)
    int32_t  y;        ///< Absolute cursor Y (pixels, clamped to screen)
    int32_t  dx;       ///< Relative X movement since last packet
    int32_t  dy;       ///< Relative Y movement since last packet (positive = down)
    uint8_t  buttons;  ///< Raw button bitmask (bit0=left, bit1=right, bit2=middle)
    bool     left;     ///< true if left button is pressed
    bool     right;    ///< true if right button is pressed
    bool     middle;   ///< true if middle button is pressed
};

// ============================================================
// Keyboard event data
// ============================================================

/**
 * @brief Decoded keyboard event
 *
 * Re-exports the same layout as drivers::KeyEvent so that the
 * window manager can consume key events from the unified queue
 * without depending on the driver layer.
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
// Unified event
// ============================================================

/**
 * @brief Single input event that may carry mouse or keyboard data
 *
 * The @c type_ field determines which union member is valid.
 */
struct Event {
    EventType type_;

    union {
        MouseEvent mouse;
        KeyEvent   key;
    };
};

// ============================================================
// Ring-buffer event queue
// ============================================================

/**
 * @brief Lock-free single-producer / single-consumer ring buffer
 *
 * The IRQ handler (producer) enqueues events; the window manager
 * (consumer) dequeues them via poll().  Because the kernel runs
 * cooperatively with IRQs masked during dequeue, no additional
 * synchronisation is needed beyond InterruptGuard.
 */
class EventQueue {
public:
    EventQueue() = default;

    /**
     * @brief Enqueue an event into the ring buffer
     *
     * If the buffer is full the event is silently dropped.
     *
     * @param ev  The event to enqueue
     */
    void enqueue(const Event& ev);

    /**
     * @brief Dequeue the next event from the ring buffer
     *
     * @param out  Reference to receive the dequeued event
     * @return     true if an event was available, false if empty
     */
    bool dequeue(Event& out);

    /**
     * @brief Check whether the queue is empty
     * @return true if no events are pending
     */
    bool empty() const;

    /**
     * @brief Discard all pending events
     */
    void clear();

private:
    static constexpr uint32_t BUF_SIZE = 128;

    Event   buf_[BUF_SIZE]{};
    uint32_t head_ = 0;
    uint32_t tail_ = 0;
};

}  // namespace cinux::gui
