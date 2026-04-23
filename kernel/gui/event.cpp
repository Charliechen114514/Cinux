/**
 * @file kernel/gui/event.cpp
 * @brief Event queue ring-buffer implementation
 */

#include "event.hpp"

namespace cinux::gui {

// ============================================================
// EventQueue::enqueue
// ============================================================

void EventQueue::enqueue(const Event& ev) {
    uint32_t next = (tail_ + 1) % BUF_SIZE;

    // Drop the event if the buffer is full
    if (next == head_) {
        return;
    }

    buf_[tail_] = ev;
    tail_ = next;
}

// ============================================================
// EventQueue::dequeue
// ============================================================

bool EventQueue::dequeue(Event& out) {
    if (head_ == tail_) {
        return false;
    }

    out = buf_[head_];
    head_ = (head_ + 1) % BUF_SIZE;
    return true;
}

// ============================================================
// EventQueue::empty
// ============================================================

bool EventQueue::empty() const {
    return head_ == tail_;
}

// ============================================================
// EventQueue::clear
// ============================================================

void EventQueue::clear() {
    head_ = 0;
    tail_ = 0;
}

}  // namespace cinux::gui
