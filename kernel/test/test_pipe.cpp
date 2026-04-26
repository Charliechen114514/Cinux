/**
 * @file kernel/test/test_pipe.cpp
 * @brief QEMU in-kernel tests for Pipe and PipeOps (031_gui_native_app, Phase 1)
 *
 * Test coverage:
 *   - Single-thread write/read round-trip
 *   - close_reader then write returns -1
 *   - close_writer then read returns 0 (EOF)
 *   - close_writer after drain: read returns 0
 *   - is_empty() / is_full() / available() state queries
 *   - PipeReadOps: read delegates, write returns -1
 *   - PipeWriteOps: write delegates, read returns -1
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Heap initialised (for any future heap-based tests)
 */

#include "big_kernel_test.h"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"

using cinux::ipc::Pipe;
using cinux::ipc::PipeReadOps;
using cinux::ipc::PipeWriteOps;

// ============================================================
// 1. Single-thread write/read
// ============================================================

void test_pipe_basic_write_read() {
    auto* pipe = new Pipe();

    const char msg[] = "Hello";
    int64_t    w     = pipe->write(msg, 5);
    TEST_ASSERT_EQ(w, 5);

    char    buf[8] = {};
    int64_t r      = pipe->read(buf, 5);
    TEST_ASSERT_EQ(r, 5);

    // Verify content
    TEST_ASSERT_EQ(buf[0], 'H');
    TEST_ASSERT_EQ(buf[1], 'e');
    TEST_ASSERT_EQ(buf[2], 'l');
    TEST_ASSERT_EQ(buf[3], 'l');
    TEST_ASSERT_EQ(buf[4], 'o');

    delete pipe;
}

// ============================================================
// 2. close_reader then write returns -1
// ============================================================

void test_pipe_write_after_close_reader() {
    auto* pipe = new Pipe();

    pipe->close_reader();
    int64_t w = pipe->write("data", 4);
    TEST_ASSERT_EQ(w, -1);

    delete pipe;
}

// ============================================================
// 3. close_writer then read returns 0 (EOF)
// ============================================================

void test_pipe_read_eof_after_close_writer() {
    auto* pipe = new Pipe();

    pipe->close_writer();
    char    buf[8] = {};
    int64_t r      = pipe->read(buf, 4);
    TEST_ASSERT_EQ(r, 0);

    delete pipe;
}

// Write data, close writer, drain, then read should return 0.
void test_pipe_drain_then_eof() {
    auto* pipe = new Pipe();

    pipe->write("AB", 2);
    pipe->close_writer();

    char    buf[8] = {};
    int64_t r      = pipe->read(buf, 2);
    TEST_ASSERT_EQ(r, 2);
    TEST_ASSERT_EQ(buf[0], 'A');
    TEST_ASSERT_EQ(buf[1], 'B');

    // Buffer empty, writer closed -- EOF
    r = pipe->read(buf, 2);
    TEST_ASSERT_EQ(r, 0);

    delete pipe;
}

// ============================================================
// 4. State queries
// ============================================================

void test_pipe_fresh_state() {
    auto* pipe = new Pipe();

    TEST_ASSERT_TRUE(pipe->is_empty());
    TEST_ASSERT_FALSE(pipe->is_full());
    TEST_ASSERT_EQ(pipe->available(), 0u);
    TEST_ASSERT_TRUE(pipe->reader_alive());
    TEST_ASSERT_TRUE(pipe->writer_alive());

    delete pipe;
}

void test_pipe_available_after_write() {
    auto* pipe = new Pipe();

    pipe->write("ABC", 3);
    TEST_ASSERT_EQ(pipe->available(), 3u);
    TEST_ASSERT_FALSE(pipe->is_empty());

    char buf[4] = {};
    pipe->read(buf, 3);
    TEST_ASSERT_TRUE(pipe->is_empty());
    TEST_ASSERT_EQ(pipe->available(), 0u);

    delete pipe;
}

// ============================================================
// 5. PipeReadOps / PipeWriteOps
// ============================================================

void test_pipe_read_ops_delegates_read() {
    auto* pipe = new Pipe();
    pipe->write("OK", 2);
    pipe->close_writer();

    PipeReadOps read_ops(pipe);
    char        buf[8] = {};
    int64_t     r      = read_ops.read(nullptr, 0, buf, 8);
    TEST_ASSERT_EQ(r, 2);
    TEST_ASSERT_EQ(buf[0], 'O');
    TEST_ASSERT_EQ(buf[1], 'K');

    delete pipe;
}

void test_pipe_read_ops_write_returns_minus1() {
    auto*       pipe = new Pipe();
    PipeReadOps read_ops(pipe);

    int64_t w = read_ops.write(nullptr, 0, "data", 4);
    TEST_ASSERT_EQ(w, -1);

    delete pipe;
}

void test_pipe_write_ops_delegates_write() {
    auto* pipe = new Pipe();

    PipeWriteOps write_ops(pipe);
    int64_t      w = write_ops.write(nullptr, 0, "W", 1);
    TEST_ASSERT_EQ(w, 1);

    char    buf[8] = {};
    int64_t r      = pipe->read(buf, 1);
    TEST_ASSERT_EQ(r, 1);
    TEST_ASSERT_EQ(buf[0], 'W');

    delete pipe;
}

void test_pipe_write_ops_read_returns_minus1() {
    auto*        pipe = new Pipe();
    PipeWriteOps write_ops(pipe);

    // PipeWriteOps inherits read() from InodeOps, which returns -1
    char    buf[8] = {};
    int64_t r      = write_ops.read(nullptr, 0, buf, 8);
    TEST_ASSERT_EQ(r, -1);

    delete pipe;
}

// ============================================================
// 6. Multiple small writes then one read
// ============================================================

void test_pipe_multiple_writes_single_read() {
    auto* pipe = new Pipe();

    pipe->write("AB", 2);
    pipe->write("CD", 2);
    pipe->write("EF", 2);

    char    buf[8] = {};
    int64_t r      = pipe->read(buf, 6);
    TEST_ASSERT_EQ(r, 6);
    TEST_ASSERT_EQ(buf[0], 'A');
    TEST_ASSERT_EQ(buf[1], 'B');
    TEST_ASSERT_EQ(buf[2], 'C');
    TEST_ASSERT_EQ(buf[3], 'D');
    TEST_ASSERT_EQ(buf[4], 'E');
    TEST_ASSERT_EQ(buf[5], 'F');

    delete pipe;
}

// ============================================================
// 7. Null / zero-length arguments
// ============================================================

void test_pipe_write_nullptr() {
    auto* pipe = new Pipe();
    TEST_ASSERT_EQ(pipe->write(nullptr, 10), -1);
    delete pipe;
}

void test_pipe_write_zero_count() {
    auto* pipe = new Pipe();
    TEST_ASSERT_EQ(pipe->write("data", 0), -1);
    delete pipe;
}

void test_pipe_read_nullptr() {
    auto* pipe = new Pipe();
    TEST_ASSERT_EQ(pipe->read(nullptr, 10), -1);
    delete pipe;
}

void test_pipe_read_zero_count() {
    auto* pipe = new Pipe();
    TEST_ASSERT_EQ(pipe->read(reinterpret_cast<char*>(1), 0), -1);
    delete pipe;
}

// ============================================================
// 8. try_read / try_write (non-blocking)
// ============================================================

void test_pipe_try_read_empty() {
    auto* pipe   = new Pipe();
    char  buf[8] = {};
    TEST_ASSERT_EQ(pipe->try_read(buf, 8), 0);
    delete pipe;
}

void test_pipe_try_write_then_try_read() {
    auto*      pipe  = new Pipe();
    const char msg[] = "Hi";
    TEST_ASSERT_EQ(pipe->try_write(msg, 2), 2);

    char buf[8] = {};
    TEST_ASSERT_EQ(pipe->try_read(buf, 8), 2);
    TEST_ASSERT_EQ(buf[0], 'H');
    TEST_ASSERT_EQ(buf[1], 'i');

    delete pipe;
}

void test_pipe_try_read_partial() {
    auto* pipe = new Pipe();
    pipe->try_write("ABC", 3);

    char buf[8] = {};
    TEST_ASSERT_EQ(pipe->try_read(buf, 8), 3);
    TEST_ASSERT_EQ(buf[0], 'A');
    TEST_ASSERT_EQ(buf[1], 'B');
    TEST_ASSERT_EQ(buf[2], 'C');

    delete pipe;
}

void test_pipe_try_write_full_returns_zero() {
    auto* pipe = new Pipe();

    // Fill the buffer
    char src[cinux::ipc::PIPE_BUFFER_SIZE];
    for (uint32_t i = 0; i < cinux::ipc::PIPE_BUFFER_SIZE; i++) {
        src[i] = static_cast<char>(i);
    }
    pipe->write(src, cinux::ipc::PIPE_BUFFER_SIZE);
    TEST_ASSERT_TRUE(pipe->is_full());

    // try_write on full buffer returns 0
    TEST_ASSERT_EQ(pipe->try_write("X", 1), 0);

    delete pipe;
}

void test_pipe_try_write_nullptr() {
    auto* pipe = new Pipe();
    TEST_ASSERT_EQ(pipe->try_write(nullptr, 8), -1);
    delete pipe;
}

void test_pipe_try_read_nullptr() {
    auto* pipe = new Pipe();
    TEST_ASSERT_EQ(pipe->try_read(nullptr, 8), -1);
    delete pipe;
}

void test_pipe_try_read_after_close_writer() {
    auto* pipe = new Pipe();
    pipe->try_write("XY", 2);
    pipe->close_writer();

    char buf[8] = {};
    TEST_ASSERT_EQ(pipe->try_read(buf, 8), 2);
    TEST_ASSERT_EQ(buf[0], 'X');
    TEST_ASSERT_EQ(buf[1], 'Y');

    TEST_ASSERT_EQ(pipe->try_read(buf, 8), 0);  // EOF

    delete pipe;
}

void test_pipe_try_write_after_close_reader() {
    auto* pipe = new Pipe();
    pipe->close_reader();
    TEST_ASSERT_EQ(pipe->try_write("data", 4), -1);
    delete pipe;
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_pipe_tests() {
    TEST_SECTION("Pipe Tests (031_gui_native_app Phase 1)");

    RUN_TEST(test_pipe_basic_write_read);
    RUN_TEST(test_pipe_write_after_close_reader);
    RUN_TEST(test_pipe_read_eof_after_close_writer);
    RUN_TEST(test_pipe_drain_then_eof);
    RUN_TEST(test_pipe_fresh_state);
    RUN_TEST(test_pipe_available_after_write);
    RUN_TEST(test_pipe_read_ops_delegates_read);
    RUN_TEST(test_pipe_read_ops_write_returns_minus1);
    RUN_TEST(test_pipe_write_ops_delegates_write);
    RUN_TEST(test_pipe_write_ops_read_returns_minus1);
    RUN_TEST(test_pipe_multiple_writes_single_read);
    RUN_TEST(test_pipe_write_nullptr);
    RUN_TEST(test_pipe_write_zero_count);
    RUN_TEST(test_pipe_read_nullptr);
    RUN_TEST(test_pipe_read_zero_count);
    RUN_TEST(test_pipe_try_read_empty);
    RUN_TEST(test_pipe_try_write_then_try_read);
    RUN_TEST(test_pipe_try_read_partial);
    RUN_TEST(test_pipe_try_write_full_returns_zero);
    RUN_TEST(test_pipe_try_write_nullptr);
    RUN_TEST(test_pipe_try_read_nullptr);
    RUN_TEST(test_pipe_try_read_after_close_writer);
    RUN_TEST(test_pipe_try_write_after_close_reader);

    TEST_SUMMARY();
}
