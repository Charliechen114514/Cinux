/**
 * @file kernel/ipc/pipe.cpp
 * @brief Pipe implementation -- ring-buffer read/write with spin-wait blocking
 */

#include "kernel/ipc/pipe.hpp"

namespace cinux::ipc {

// ============================================================
// Constructor
// ============================================================

Pipe::Pipe()
    : head_(0)
    , tail_(0)
    , count_(0)
    , reader_open_(true)
    , writer_open_(true) {}

// ============================================================
// Write
// ============================================================

int64_t Pipe::write(const char* data, uint64_t count) {
    if (data == nullptr || count == 0) {
        return -1;
    }

    uint64_t written = 0;

    while (written < count) {
        lock_.acquire();

        // Reader has closed -- no point writing further
        if (!reader_open_) {
            lock_.release();
            return (written > 0) ? static_cast<int64_t>(written) : -1;
        }

        // Spin-wait if the buffer is completely full.
        // Release the lock so the reader can make progress.
        if (count_ == PIPE_BUFFER_SIZE) {
            lock_.release();

            for (uint32_t i = 0; i < PIPE_SPIN_WAIT_ITERS; i++) {
                __asm__ volatile("sti; hlt; cli");

                // Re-acquire briefly to check state, then release again
                lock_.acquire();
                bool still_full = (count_ == PIPE_BUFFER_SIZE);
                bool reader_gone = !reader_open_;
                lock_.release();

                if (!still_full || reader_gone) {
                    break;
                }
            }
            continue;
        }

        // Compute how many contiguous bytes we can write from tail_
        uint32_t space = PIPE_BUFFER_SIZE - count_;
        uint64_t chunk = count - written;
        if (chunk > space) {
            chunk = space;
        }

        // Handle wrap-around: first segment from tail_ to end of buffer
        uint32_t first = PIPE_BUFFER_SIZE - tail_;
        if (first > chunk) {
            first = static_cast<uint32_t>(chunk);
        }

        for (uint32_t i = 0; i < first; i++) {
            buffer_[tail_ + i] = data[written + i];
        }
        tail_ = (tail_ + first) % PIPE_BUFFER_SIZE;
        count_ += first;
        written += first;

        // Second segment (wraps to beginning of buffer)
        uint32_t second = static_cast<uint32_t>(chunk) - first;
        if (second > 0) {
            for (uint32_t i = 0; i < second; i++) {
                buffer_[tail_ + i] = data[written + i];
            }
            tail_ = (tail_ + second) % PIPE_BUFFER_SIZE;
            count_ += second;
            written += second;
        }

        lock_.release();
    }

    return static_cast<int64_t>(written);
}

// ============================================================
// Read
// ============================================================

int64_t Pipe::read(char* buf, uint64_t count) {
    if (buf == nullptr || count == 0) {
        return -1;
    }

    uint64_t total_read = 0;

    while (total_read < count) {
        lock_.acquire();

        // Writer closed and buffer drained -- EOF
        if (!writer_open_ && count_ == 0) {
            lock_.release();
            return (total_read > 0) ? static_cast<int64_t>(total_read) : 0;
        }

        // Spin-wait if the buffer is empty.
        // Release the lock so the writer can make progress.
        if (count_ == 0) {
            lock_.release();

            for (uint32_t i = 0; i < PIPE_SPIN_WAIT_ITERS; i++) {
                __asm__ volatile("sti; hlt; cli");

                lock_.acquire();
                bool still_empty = (count_ == 0);
                bool writer_gone = !writer_open_;
                lock_.release();

                if (!still_empty || writer_gone) {
                    break;
                }
            }
            continue;
        }

        // Compute how many contiguous bytes we can read from head_
        uint64_t chunk = count - total_read;
        if (chunk > count_) {
            chunk = count_;
        }

        // First segment: head_ to end of buffer
        uint32_t first = PIPE_BUFFER_SIZE - head_;
        if (first > chunk) {
            first = static_cast<uint32_t>(chunk);
        }

        for (uint32_t i = 0; i < first; i++) {
            buf[total_read + i] = buffer_[head_ + i];
        }
        head_ = (head_ + first) % PIPE_BUFFER_SIZE;
        count_ -= first;
        total_read += first;

        // Second segment (wraps to beginning of buffer)
        uint32_t second = static_cast<uint32_t>(chunk) - first;
        if (second > 0) {
            for (uint32_t i = 0; i < second; i++) {
                buf[total_read + i] = buffer_[i];
            }
            head_ = (head_ + second) % PIPE_BUFFER_SIZE;
            count_ -= second;
            total_read += second;
        }

        lock_.release();
    }

    return static_cast<int64_t>(total_read);
}

// ============================================================
// Close endpoints
// ============================================================

void Pipe::close_reader() {
    auto guard = lock_.guard();
    reader_open_ = false;
}

void Pipe::close_writer() {
    auto guard = lock_.guard();
    writer_open_ = false;
}

// ============================================================
// State queries (lock-free -- for diagnostics / fast-path checks)
// ============================================================

bool Pipe::reader_alive() const {
    return reader_open_;
}

bool Pipe::writer_alive() const {
    return writer_open_;
}

bool Pipe::is_empty() const {
    return count_ == 0;
}

bool Pipe::is_full() const {
    return count_ == PIPE_BUFFER_SIZE;
}

uint32_t Pipe::available() const {
    return count_;
}

// ============================================================
// Non-blocking try_read / try_write
// ============================================================

int64_t Pipe::try_read(char* buf, uint64_t count) {
    if (buf == nullptr || count == 0) {
        return -1;
    }

    auto guard = lock_.guard();

    // Writer closed and buffer drained -- EOF
    if (!writer_open_ && count_ == 0) {
        return 0;
    }

    // Buffer empty -- return 0 immediately (no spin-wait)
    if (count_ == 0) {
        return 0;
    }

    // Compute how many contiguous bytes we can read from head_
    uint64_t chunk = count;
    if (chunk > count_) {
        chunk = count_;
    }

    // First segment: head_ to end of buffer
    uint32_t first = PIPE_BUFFER_SIZE - head_;
    if (first > chunk) {
        first = static_cast<uint32_t>(chunk);
    }

    for (uint32_t i = 0; i < first; i++) {
        buf[i] = buffer_[head_ + i];
    }
    head_ = (head_ + first) % PIPE_BUFFER_SIZE;
    count_ -= first;

    // Second segment (wraps to beginning of buffer)
    uint32_t second = static_cast<uint32_t>(chunk) - first;
    if (second > 0) {
        for (uint32_t i = 0; i < second; i++) {
            buf[first + i] = buffer_[i];
        }
        head_ = (head_ + second) % PIPE_BUFFER_SIZE;
        count_ -= second;
    }

    return static_cast<int64_t>(chunk);
}

int64_t Pipe::try_write(const char* data, uint64_t count) {
    if (data == nullptr || count == 0) {
        return -1;
    }

    auto guard = lock_.guard();

    // Reader has closed -- cannot write
    if (!reader_open_) {
        return -1;
    }

    // Buffer full -- return 0 immediately (no spin-wait)
    if (count_ == PIPE_BUFFER_SIZE) {
        return 0;
    }

    // Compute how many contiguous bytes we can write from tail_
    uint32_t space = PIPE_BUFFER_SIZE - count_;
    uint64_t chunk = count;
    if (chunk > space) {
        chunk = space;
    }

    // First segment: tail_ to end of buffer
    uint32_t first = PIPE_BUFFER_SIZE - tail_;
    if (first > chunk) {
        first = static_cast<uint32_t>(chunk);
    }

    for (uint32_t i = 0; i < first; i++) {
        buffer_[tail_ + i] = data[i];
    }
    tail_ = (tail_ + first) % PIPE_BUFFER_SIZE;
    count_ += first;

    // Second segment (wraps to beginning of buffer)
    uint32_t second = static_cast<uint32_t>(chunk) - first;
    if (second > 0) {
        for (uint32_t i = 0; i < second; i++) {
            buffer_[i] = data[first + i];
        }
        tail_ = (tail_ + second) % PIPE_BUFFER_SIZE;
        count_ += second;
    }

    return static_cast<int64_t>(chunk);
}

}  // namespace cinux::ipc
