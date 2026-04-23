/**
 * @file test/unit/test_pipe.cpp
 * @brief Host-side unit tests for Pipe ring-buffer IPC (kernel/ipc/pipe.hpp)
 *
 * Test coverage:
 *   - Basic write/read: write "Hello", read it back
 *   - Multiple small writes, then one large read
 *   - One large write, then multiple small reads
 *   - Buffer full: write returns -1 when reader is closed while full
 *   - Buffer empty + writer closed: read returns 0 (EOF)
 *   - close_reader then write returns -1
 *   - close_writer then read returns 0 (EOF)
 *   - is_empty() / is_full() / available() state queries
 *   - Wrap-around: write and read across the ring buffer boundary
 *   - Null / zero-length arguments
 *   - PipeOps: PipeReadOps delegates read, write returns -1
 *   - PipeOps: PipeWriteOps delegates write, read returns -1
 *
 * Links directly with kernel/ipc/pipe.cpp, kernel/ipc/pipe_ops.cpp,
 * and kernel/fs/inode.cpp.  Uses host_spinlock.cpp for Spinlock.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>

#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/fs/inode.hpp"

using namespace cinux::ipc;
using namespace cinux::fs;

// ============================================================
// Helper: create a Pipe on the stack
// ============================================================

static Pipe make_pipe() {
    return Pipe{};
}

// ============================================================
// 1. Basic write/read
// ============================================================

// Write "Hello" and read it back byte-for-byte.
TEST("pipe: basic write and read") {
    Pipe pipe = make_pipe();

    const char msg[] = "Hello";
    int64_t w = pipe.write(msg, 5);
    ASSERT_EQ(w, 5);

    char buf[8] = {};
    int64_t r = pipe.read(buf, 5);
    ASSERT_EQ(r, 5);
    ASSERT_TRUE(memcmp(buf, "Hello", 5) == 0);
}

// ============================================================
// 2. Multiple small writes, then one large read
// ============================================================

// Three small writes followed by a single read that collects all data.
TEST("pipe: multiple small writes then one large read") {
    Pipe pipe = make_pipe();

    ASSERT_EQ(pipe.write("AB", 2), 2);
    ASSERT_EQ(pipe.write("CD", 2), 2);
    ASSERT_EQ(pipe.write("EF", 2), 2);

    char buf[8] = {};
    ASSERT_EQ(pipe.read(buf, 6), 6);
    ASSERT_TRUE(memcmp(buf, "ABCDEF", 6) == 0);
}

// ============================================================
// 3. One large write, then multiple small reads
// ============================================================

// Single write of 6 bytes, then two reads of 3 bytes each.
TEST("pipe: one large write then multiple small reads") {
    Pipe pipe = make_pipe();

    ASSERT_EQ(pipe.write("ABCDEF", 6), 6);

    char buf[4] = {};
    ASSERT_EQ(pipe.read(buf, 3), 3);
    ASSERT_TRUE(memcmp(buf, "ABC", 3) == 0);

    ASSERT_EQ(pipe.read(buf, 3), 3);
    ASSERT_TRUE(memcmp(buf, "DEF", 3) == 0);
}

// ============================================================
// 4. close_reader then write returns -1
// ============================================================

// After closing the reader, write should return -1 immediately.
TEST("pipe: write returns -1 after close_reader") {
    Pipe pipe = make_pipe();

    pipe.close_reader();
    int64_t w = pipe.write("Hello", 5);
    ASSERT_EQ(w, -1);
}

// ============================================================
// 5. close_writer then read returns 0 (EOF)
// ============================================================

// After closing the writer with an empty buffer, read returns 0.
TEST("pipe: read returns 0 after close_writer with empty buffer") {
    Pipe pipe = make_pipe();

    pipe.close_writer();
    char buf[8] = {};
    int64_t r = pipe.read(buf, 5);
    ASSERT_EQ(r, 0);
}

// After closing the writer with data still in the buffer, read
// should drain remaining data first, then return 0 on the next read.
TEST("pipe: read drains buffer then returns 0 after close_writer") {
    Pipe pipe = make_pipe();

    ASSERT_EQ(pipe.write("AB", 2), 2);
    pipe.close_writer();

    char buf[8] = {};
    ASSERT_EQ(pipe.read(buf, 2), 2);
    ASSERT_TRUE(memcmp(buf, "AB", 2) == 0);

    // Buffer is now empty and writer is closed -- EOF
    ASSERT_EQ(pipe.read(buf, 2), 0);
}

// ============================================================
// 6. is_empty() / is_full() / available() state queries
// ============================================================

// A freshly created pipe should be empty, not full, with 0 available.
TEST("pipe: fresh pipe is empty") {
    Pipe pipe = make_pipe();

    ASSERT_TRUE(pipe.is_empty());
    ASSERT_FALSE(pipe.is_full());
    ASSERT_EQ(pipe.available(), 0u);
}

// After writing, available() should reflect the byte count.
TEST("pipe: available reflects written bytes") {
    Pipe pipe = make_pipe();

    pipe.write("ABC", 3);
    ASSERT_EQ(pipe.available(), 3u);
    ASSERT_FALSE(pipe.is_empty());
}

// After reading all data, pipe should be empty again.
TEST("pipe: is_empty after draining") {
    Pipe pipe = make_pipe();

    pipe.write("X", 1);
    char c;
    pipe.read(&c, 1);
    ASSERT_TRUE(pipe.is_empty());
    ASSERT_EQ(pipe.available(), 0u);
}

// reader_alive() and writer_alive() reflect open/close state.
TEST("pipe: reader_alive and writer_alive") {
    Pipe pipe = make_pipe();

    ASSERT_TRUE(pipe.reader_alive());
    ASSERT_TRUE(pipe.writer_alive());

    pipe.close_reader();
    ASSERT_FALSE(pipe.reader_alive());
    ASSERT_TRUE(pipe.writer_alive());

    pipe.close_writer();
    ASSERT_FALSE(pipe.writer_alive());
}

// ============================================================
// 7. Wrap-around: write and read across ring buffer boundary
// ============================================================

// Write data larger than half the buffer, read half, write more.
// This forces the tail to wrap around the ring buffer.
TEST("pipe: wrap-around write and read") {
    Pipe pipe = make_pipe();

    // Write 3000 bytes (more than half of 4096)
    uint8_t src[3000];
    for (uint32_t i = 0; i < 3000; i++) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }
    ASSERT_EQ(pipe.write(reinterpret_cast<const char*>(src), 3000), 3000);
    ASSERT_EQ(pipe.available(), 3000u);

    // Read 2000 bytes
    uint8_t dst[3000];
    memset(dst, 0, sizeof(dst));
    ASSERT_EQ(pipe.read(reinterpret_cast<char*>(dst), 2000), 2000);
    ASSERT_TRUE(memcmp(dst, src, 2000) == 0);
    ASSERT_EQ(pipe.available(), 1000u);

    // Write 3096 more bytes: 3096 + 1000 = 4096 (fills the buffer exactly)
    uint8_t src2[3096];
    for (uint32_t i = 0; i < 3096; i++) {
        src2[i] = static_cast<uint8_t>((i + 100) & 0xFF);
    }
    ASSERT_EQ(pipe.write(reinterpret_cast<const char*>(src2), 3096), 3096);
    ASSERT_TRUE(pipe.is_full());
    ASSERT_EQ(pipe.available(), 4096u);

    // Read all 4096 bytes and verify correctness
    uint8_t dst2[4096];
    ASSERT_EQ(pipe.read(reinterpret_cast<char*>(dst2), 4096), 4096);

    // First 1000 bytes should be the remainder of src (bytes 2000..2999)
    ASSERT_TRUE(memcmp(dst2, src + 2000, 1000) == 0);
    // Next 3096 bytes should be src2
    ASSERT_TRUE(memcmp(dst2 + 1000, src2, 3096) == 0);

    ASSERT_TRUE(pipe.is_empty());
}

// Write exactly PIPE_BUFFER_SIZE bytes, then read them all.
TEST("pipe: fill buffer to capacity and drain") {
    Pipe pipe = make_pipe();

    char src[PIPE_BUFFER_SIZE];
    for (uint32_t i = 0; i < PIPE_BUFFER_SIZE; i++) {
        src[i] = static_cast<char>(i);
    }

    ASSERT_EQ(pipe.write(src, PIPE_BUFFER_SIZE),
              static_cast<int64_t>(PIPE_BUFFER_SIZE));
    ASSERT_TRUE(pipe.is_full());
    ASSERT_EQ(pipe.available(), PIPE_BUFFER_SIZE);

    char dst[PIPE_BUFFER_SIZE];
    ASSERT_EQ(pipe.read(dst, PIPE_BUFFER_SIZE),
              static_cast<int64_t>(PIPE_BUFFER_SIZE));
    ASSERT_TRUE(memcmp(src, dst, PIPE_BUFFER_SIZE) == 0);
    ASSERT_TRUE(pipe.is_empty());
}

// ============================================================
// 8. Null / zero-length arguments
// ============================================================

// write with nullptr data returns -1.
TEST("pipe: write nullptr returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.write(nullptr, 10), -1);
}

// write with zero count returns -1.
TEST("pipe: write zero count returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.write("Hello", 0), -1);
}

// read with nullptr buf returns -1.
TEST("pipe: read nullptr returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.read(nullptr, 10), -1);
}

// read with zero count returns -1.
TEST("pipe: read zero count returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.read(reinterpret_cast<char*>(1), 0), -1);
}

// ============================================================
// 9. Partial read (request more than available)
// ============================================================

// After closing the writer with data in the buffer, requesting more bytes
// than available returns only what is buffered (partial read), then 0 on
// the next call (EOF).  This tests that Pipe::read() drains available data
// before checking the writer-open flag.
TEST("pipe: partial read after close_writer returns available then EOF") {
    Pipe pipe = make_pipe();

    pipe.write("AB", 2);
    pipe.close_writer();

    char buf[10] = {};
    int64_t r = pipe.read(buf, 8);
    ASSERT_EQ(r, 2);
    ASSERT_TRUE(memcmp(buf, "AB", 2) == 0);

    // Next read should return 0 (EOF)
    r = pipe.read(buf, 8);
    ASSERT_EQ(r, 0);
}

// ============================================================
// 10. PipeReadOps / PipeWriteOps
// ============================================================

// PipeReadOps::read delegates to Pipe::read.
TEST("pipe_ops: PipeReadOps read delegates to pipe") {
    Pipe pipe = make_pipe();
    pipe.write("XYZ", 3);
    pipe.close_writer();

    PipeReadOps read_ops(&pipe);
    char buf[8] = {};
    int64_t r = read_ops.read(nullptr, 0, buf, 8);
    ASSERT_EQ(r, 3);
    ASSERT_TRUE(memcmp(buf, "XYZ", 3) == 0);
}

// PipeReadOps::write always returns -1 (read-only end).
TEST("pipe_ops: PipeReadOps write returns -1") {
    Pipe pipe = make_pipe();
    PipeReadOps read_ops(&pipe);

    int64_t w = read_ops.write(nullptr, 0, "data", 4);
    ASSERT_EQ(w, -1);
}

// PipeWriteOps::write delegates to Pipe::write.
TEST("pipe_ops: PipeWriteOps write delegates to pipe") {
    Pipe pipe = make_pipe();

    PipeWriteOps write_ops(&pipe);
    int64_t w = write_ops.write(nullptr, 0, "HI", 2);
    ASSERT_EQ(w, 2);

    char buf[8] = {};
    ASSERT_EQ(pipe.read(buf, 2), 2);
    ASSERT_TRUE(memcmp(buf, "HI", 2) == 0);
}

// PipeWriteOps with nullptr pipe returns -1.
TEST("pipe_ops: PipeWriteOps nullptr pipe returns -1") {
    PipeWriteOps write_ops(nullptr);
    int64_t w = write_ops.write(nullptr, 0, "HI", 2);
    ASSERT_EQ(w, -1);
}

// PipeReadOps with nullptr pipe returns -1.
TEST("pipe_ops: PipeReadOps nullptr pipe returns -1") {
    PipeReadOps read_ops(nullptr);
    char buf[8] = {};
    int64_t r = read_ops.read(nullptr, 0, buf, 8);
    ASSERT_EQ(r, -1);
}

// PipeWriteOps with nullptr buf returns -1.
TEST("pipe_ops: PipeWriteOps nullptr buf returns -1") {
    Pipe pipe = make_pipe();
    PipeWriteOps write_ops(&pipe);
    int64_t w = write_ops.write(nullptr, 0, nullptr, 4);
    ASSERT_EQ(w, -1);
}

// PipeReadOps with nullptr buf returns -1.
TEST("pipe_ops: PipeReadOps nullptr buf returns -1") {
    Pipe pipe = make_pipe();
    PipeReadOps read_ops(&pipe);
    int64_t r = read_ops.read(nullptr, 0, nullptr, 4);
    ASSERT_EQ(r, -1);
}

// ============================================================
// 11. Multiple close calls are safe
// ============================================================

// Calling close_reader / close_writer multiple times does not crash.
TEST("pipe: double close_reader is safe") {
    Pipe pipe = make_pipe();
    pipe.close_reader();
    pipe.close_reader();  // should not crash
    ASSERT_FALSE(pipe.reader_alive());
}

TEST("pipe: double close_writer is safe") {
    Pipe pipe = make_pipe();
    pipe.close_writer();
    pipe.close_writer();  // should not crash
    ASSERT_FALSE(pipe.writer_alive());
}

// ============================================================
// 12. try_read / try_write (non-blocking)
// ============================================================

// try_read on empty pipe returns 0 immediately (no blocking).
TEST("pipe: try_read on empty pipe returns 0") {
    Pipe pipe = make_pipe();

    char buf[8] = {};
    int64_t r = pipe.try_read(buf, 8);
    ASSERT_EQ(r, 0);
}

// try_write then try_read round-trip.
TEST("pipe: try_write then try_read") {
    Pipe pipe = make_pipe();

    const char msg[] = "Hello";
    int64_t w = pipe.try_write(msg, 5);
    ASSERT_EQ(w, 5);

    char buf[8] = {};
    int64_t r = pipe.try_read(buf, 8);
    ASSERT_EQ(r, 5);
    ASSERT_TRUE(memcmp(buf, "Hello", 5) == 0);
}

// try_read returns only available bytes (partial read).
TEST("pipe: try_read partial read") {
    Pipe pipe = make_pipe();

    ASSERT_EQ(pipe.try_write("ABC", 3), 3);

    char buf[8] = {};
    int64_t r = pipe.try_read(buf, 8);
    ASSERT_EQ(r, 3);
    ASSERT_TRUE(memcmp(buf, "ABC", 3) == 0);
}

// try_write on full pipe returns 0 (no blocking).
TEST("pipe: try_write on full pipe returns 0") {
    Pipe pipe = make_pipe();

    char src[PIPE_BUFFER_SIZE];
    for (uint32_t i = 0; i < PIPE_BUFFER_SIZE; i++) {
        src[i] = static_cast<char>(i);
    }
    ASSERT_EQ(pipe.try_write(src, PIPE_BUFFER_SIZE),
              static_cast<int64_t>(PIPE_BUFFER_SIZE));
    ASSERT_TRUE(pipe.is_full());

    // try_write on full buffer returns 0
    int64_t w = pipe.try_write("X", 1);
    ASSERT_EQ(w, 0);
}

// try_write on partial space writes only what fits.
TEST("pipe: try_write partial space") {
    Pipe pipe = make_pipe();

    // Fill most of the buffer
    char src[4000];
    memset(src, 'A', sizeof(src));
    ASSERT_EQ(pipe.try_write(src, 4000), 4000);

    // Try to write 200 bytes but only 96 fit
    int64_t w = pipe.try_write("BBBB", 200);
    ASSERT_EQ(w, 96);  // 4096 - 4000 = 96
}

// try_read with nullptr returns -1.
TEST("pipe: try_read nullptr returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.try_read(nullptr, 8), -1);
}

// try_read with zero count returns -1.
TEST("pipe: try_read zero count returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.try_read(reinterpret_cast<char*>(1), 0), -1);
}

// try_write with nullptr returns -1.
TEST("pipe: try_write nullptr returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.try_write(nullptr, 8), -1);
}

// try_write with zero count returns -1.
TEST("pipe: try_write zero count returns -1") {
    Pipe pipe = make_pipe();
    ASSERT_EQ(pipe.try_write("data", 0), -1);
}

// try_read after close_writer with data returns data then 0.
TEST("pipe: try_read after close_writer returns data then 0") {
    Pipe pipe = make_pipe();

    pipe.try_write("XY", 2);
    pipe.close_writer();

    char buf[8] = {};
    int64_t r = pipe.try_read(buf, 8);
    ASSERT_EQ(r, 2);
    ASSERT_TRUE(memcmp(buf, "XY", 2) == 0);

    r = pipe.try_read(buf, 8);
    ASSERT_EQ(r, 0);  // EOF
}

// try_write after close_reader returns -1.
TEST("pipe: try_write after close_reader returns -1") {
    Pipe pipe = make_pipe();
    pipe.close_reader();
    ASSERT_EQ(pipe.try_write("data", 4), -1);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
