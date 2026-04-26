/**
 * @file kernel/ipc/pipe.hpp
 * @brief Byte-stream pipe with a 4 KB ring buffer for IPC
 *
 * Pipe provides a unidirectional, kernel-internal byte channel between
 * a writer and a reader.  It is the building block for anonymous pipes
 * exposed through the VFS (see PipeInodeOps).
 *
 * Blocking semantics: when the buffer is full (write) or empty (read),
 * the caller spin-waits with a bounded iteration count (SPIN_WAIT_ITERS),
 * matching the existing sys_read pattern.  True scheduler-based blocking
 * will be added in a future milestone.
 *
 * Namespace: cinux::ipc
 */

#pragma once

#include <stdint.h>

#include "kernel/proc/sync.hpp"

namespace cinux::ipc {

// ============================================================
// Constants
// ============================================================

/// Ring buffer size in bytes (one page)
static constexpr uint32_t PIPE_BUFFER_SIZE = 4096;

/// Maximum spin-wait iterations before giving up (matches sys_read)
static constexpr uint32_t PIPE_SPIN_WAIT_ITERS = 1'000'000;

// ============================================================
// Pipe -- unidirectional byte stream
// ============================================================

/**
 * @brief Unidirectional byte-stream pipe backed by a ring buffer
 *
 * A Pipe connects one writer and one reader.  Bytes written by the
 * writer are buffered in a 4 KB circular buffer and consumed in FIFO
 * order by the reader.  Closing either end signals EOF to the other
 * side (read returns 0 after writer closes, write returns -1 after
 * reader closes).
 */
class Pipe {
public:
    Pipe();

    // -- Non-copyable, non-movable --
    Pipe(const Pipe&)            = delete;
    Pipe& operator=(const Pipe&) = delete;

    // -- Public interface --------------------------------------------------

    /**
     * @brief Write bytes into the pipe
     *
     * Copies up to @p count bytes from @p data into the ring buffer.
     * If the buffer is full, spin-waits until space becomes available
     * or the reader closes.
     *
     * @param data   Source buffer
     * @param count  Number of bytes to write
     * @return Number of bytes written (>= 0), or -1 if the reader
     *         is closed or an invalid argument was passed
     */
    int64_t write(const char* data, uint64_t count);

    /**
     * @brief Read bytes from the pipe
     *
     * Copies up to @p count bytes from the ring buffer into @p buf.
     * If the buffer is empty, spin-waits until data arrives or the
     * writer closes.
     *
     * @param buf    Destination buffer
     * @param count  Maximum number of bytes to read
     * @return Number of bytes read (>= 0), or -1 if the writer is
     *         closed and the buffer is empty, or an invalid argument
     */
    int64_t read(char* buf, uint64_t count);

    /**
     * @brief Close the reader end
     *
     * After this call, reader_alive() returns false.  Any writer
     * currently spin-waiting will detect the close and return -1.
     */
    void close_reader();

    /**
     * @brief Close the writer end
     *
     * After this call, writer_alive() returns false.  Any reader
     * currently spin-waiting will detect the close and return 0 (EOF).
     */
    void close_writer();

    /**
     * @brief Check whether the reader end is still open
     * @return true if the reader has not been closed
     */
    bool reader_alive() const;

    /**
     * @brief Check whether the writer end is still open
     * @return true if the writer has not been closed
     */
    bool writer_alive() const;

    /**
     * @brief Check whether the ring buffer contains no data
     * @return true if no bytes are buffered
     */
    bool is_empty() const;

    /**
     * @brief Check whether the ring buffer has no free space
     * @return true if the buffer is completely full
     */
    bool is_full() const;

    /**
     * @brief Return the number of bytes currently available for reading
     * @return Byte count in the ring buffer
     */
    uint32_t available() const;

    /**
     * @brief Non-blocking read from the pipe
     *
     * Reads up to @p count bytes from the ring buffer into @p buf.
     * If the buffer is empty, returns 0 immediately without waiting.
     *
     * @param buf    Destination buffer
     * @param count  Maximum number of bytes to read
     * @return Number of bytes read (>= 0), or -1 on error
     */
    int64_t try_read(char* buf, uint64_t count);

    /**
     * @brief Non-blocking write to the pipe
     *
     * Writes up to @p count bytes from @p data into the ring buffer.
     * If the buffer is full, returns 0 immediately without waiting.
     *
     * @param data   Source buffer
     * @param count  Number of bytes to write
     * @return Number of bytes written (>= 0), or -1 on error
     */
    int64_t try_write(const char* data, uint64_t count);

private:
    // -- Ring buffer state -------------------------------------------------

    /// Circular byte buffer
    char buffer_[PIPE_BUFFER_SIZE];

    /// Index of the next byte to read
    uint32_t head_;

    /// Index of the next free slot to write
    uint32_t tail_;

    /// Number of bytes currently in the buffer (0 .. PIPE_BUFFER_SIZE)
    uint32_t count_;

    // -- Endpoint state ----------------------------------------------------

    bool reader_open_;
    bool writer_open_;

    // -- Synchronisation ---------------------------------------------------

    /// Protects all mutable state (head_, tail_, count_, open flags)
    cinux::proc::Spinlock lock_;
};

}  // namespace cinux::ipc
