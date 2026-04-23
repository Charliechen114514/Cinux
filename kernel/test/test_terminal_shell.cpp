/**
 * @file kernel/test/test_terminal_shell.cpp
 * @brief QEMU in-kernel tests for Terminal-Shell pipe integration (Phase 5 & 6)
 *
 * Test coverage:
 *   - Terminal set_stdin_pipe / set_stdout_pipe
 *   - Terminal poll_output reads from stdout pipe and updates screen buffer
 *   - Terminal on_key writes to stdin pipe when connected
 *   - Pipe-backed fd 0/1 round-trip through FDTable + InodeOps
 *   - try_read / try_write non-blocking operations
 *   - Terminal destructor closes pipes (EOF propagation to shell)
 *   - WM close button destroys Terminal and closes pipes (Phase 6)
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Heap initialised (for Terminal, Pipe, Inode, File construction)
 *
 * Compile condition: CINUX_GUI
 */

#include "big_kernel_test.h"

#ifdef CINUX_GUI

#include "boot/boot_info.h"
#include "kernel/drivers/canvas.hpp"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"
#include "kernel/gui/terminal.hpp"
#include "kernel/gui/window_manager.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"
#include "kernel/fs/file.hpp"
#include "kernel/fs/inode.hpp"
#include "kernel/fs/vfs_mount.hpp"

using cinux::gui::Terminal;
using cinux::ipc::Pipe;
using cinux::ipc::PipeReadOps;
using cinux::ipc::PipeWriteOps;
using cinux::fs::Inode;
using cinux::fs::InodeType;
using cinux::fs::File;
using cinux::fs::OpenFlags;

// ============================================================
// 1. Terminal stdin pipe: on_key writes to pipe
// ============================================================

/// Verify that on_key forwards characters to stdin pipe
void test_terminal_on_key_writes_to_stdin_pipe() {
    Terminal term(0, 0);
    Pipe stdin_pipe;

    term.set_stdin_pipe(&stdin_pipe);

    // Simulate a key press
    cinux::gui::KeyEvent ev;
    ev.pressed = true;
    ev.ascii = 'A';
    term.on_key(ev);

    // Verify character was written to stdin pipe
    TEST_ASSERT_EQ(stdin_pipe.available(), 1u);
    char buf[4] = {};
    stdin_pipe.try_read(buf, 1);
    TEST_ASSERT_EQ(buf[0], 'A');
}

/// Verify that on_key converts CR to LF
void test_terminal_on_key_converts_cr_to_lf() {
    Terminal term(0, 0);
    Pipe stdin_pipe;

    term.set_stdin_pipe(&stdin_pipe);

    cinux::gui::KeyEvent ev;
    ev.pressed = true;
    ev.ascii = '\r';  // Enter key sends CR
    term.on_key(ev);

    char buf[4] = {};
    stdin_pipe.try_read(buf, 1);
    TEST_ASSERT_EQ(buf[0], '\n');  // Should be converted to LF
}

/// Verify that on_key does not write to screen when stdin pipe is set
void test_terminal_on_key_no_screen_write_with_pipe() {
    Terminal term(0, 0);
    Pipe stdin_pipe;

    term.set_stdin_pipe(&stdin_pipe);

    cinux::gui::KeyEvent ev;
    ev.pressed = true;
    ev.ascii = 'X';
    term.on_key(ev);

    // Screen buffer should still be empty (echo comes from stdout pipe)
    TEST_ASSERT_EQ(term.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(term.cursor_x(), 0u);
}

// ============================================================
// 2. Terminal stdout pipe: poll_output reads from pipe
// ============================================================

/// Verify that poll_output reads data from stdout pipe into screen buffer
void test_terminal_poll_output_reads_stdout() {
    Terminal term(0, 0);
    Pipe stdout_pipe;

    term.set_stdout_pipe(&stdout_pipe);

    // Write shell output to stdout pipe
    const char msg[] = "Hi";
    stdout_pipe.try_write(msg, 2);

    // Poll should transfer data to screen buffer
    term.poll_output();

    TEST_ASSERT_EQ(term.cell(0, 0).ch, 'H');
    TEST_ASSERT_EQ(term.cell(0, 1).ch, 'i');
    TEST_ASSERT_EQ(term.cursor_x(), 2u);
    TEST_ASSERT_EQ(term.cursor_y(), 0u);
}

/// Verify that poll_output on empty pipe does nothing
void test_terminal_poll_output_empty_pipe() {
    Terminal term(0, 0);
    Pipe stdout_pipe;

    term.set_stdout_pipe(&stdout_pipe);

    // Poll empty pipe -- should not crash or change state
    term.poll_output();

    TEST_ASSERT_EQ(term.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(term.cursor_x(), 0u);
}

/// Verify that poll_output handles newline correctly
void test_terminal_poll_output_with_newline() {
    Terminal term(0, 0);
    Pipe stdout_pipe;

    term.set_stdout_pipe(&stdout_pipe);

    const char msg[] = "AB\nCD";
    stdout_pipe.try_write(msg, 5);

    term.poll_output();

    TEST_ASSERT_EQ(term.cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(term.cell(0, 1).ch, 'B');
    TEST_ASSERT_EQ(term.cell(1, 0).ch, 'C');
    TEST_ASSERT_EQ(term.cell(1, 1).ch, 'D');
    TEST_ASSERT_EQ(term.cursor_x(), 2u);
    TEST_ASSERT_EQ(term.cursor_y(), 1u);
}

/// Verify poll_output with no stdout pipe set does nothing
void test_terminal_poll_output_no_pipe() {
    Terminal term(0, 0);

    // No pipe set -- should not crash
    term.poll_output();

    TEST_ASSERT_EQ(term.cursor_x(), 0u);
    TEST_ASSERT_EQ(term.cursor_y(), 0u);
}

// ============================================================
// 3. Full round-trip: on_key -> stdin pipe -> read -> write -> stdout pipe -> poll
// ============================================================

/// Simulate full Terminal-Shell data flow using pipes
void test_terminal_shell_full_roundtrip() {
    Terminal term(0, 0);
    Pipe stdin_pipe;
    Pipe stdout_pipe;

    term.set_stdin_pipe(&stdin_pipe);
    term.set_stdout_pipe(&stdout_pipe);

    // Simulate: Terminal receives key 'H'
    cinux::gui::KeyEvent ev;
    ev.pressed = true;
    ev.ascii = 'H';
    term.on_key(ev);

    // Shell reads from stdin pipe
    char read_buf[16] = {};
    int64_t r = stdin_pipe.try_read(read_buf, 16);
    TEST_ASSERT_EQ(r, 1);
    TEST_ASSERT_EQ(read_buf[0], 'H');

    // Shell echoes back to stdout pipe
    stdout_pipe.try_write("H", 1);

    // Terminal polls stdout pipe
    term.poll_output();

    // Screen should show 'H'
    TEST_ASSERT_EQ(term.cell(0, 0).ch, 'H');
    TEST_ASSERT_EQ(term.cursor_x(), 1u);
}

// ============================================================
// 4. Pipe-backed fd 0/1 through FDTable
// ============================================================

/// Verify fd 0 and fd 1 can be bound to pipe-backed Inodes
void test_terminal_pipe_fd_table_binding() {
    cinux::fs::vfs_mount_init();

    Pipe stdin_pipe;
    Pipe stdout_pipe;

    auto* stdin_ops = new PipeReadOps(&stdin_pipe);
    auto* stdout_ops = new PipeWriteOps(&stdout_pipe);

    auto* stdin_inode = new Inode();
    stdin_inode->ops = stdin_ops;
    stdin_inode->type = InodeType::Regular;

    auto* stdout_inode = new Inode();
    stdout_inode->ops = stdout_ops;
    stdout_inode->type = InodeType::Regular;

    auto* stdin_file = new File(stdin_inode, 0, OpenFlags::RDONLY);
    auto* stdout_file = new File(stdout_inode, 0, OpenFlags::WRONLY);

    cinux::fs::g_global_fd_table().set(0, stdin_file);
    cinux::fs::g_global_fd_table().set(1, stdout_file);

    // Verify fd table entries
    File* f0 = cinux::fs::g_global_fd_table().get(0);
    File* f1 = cinux::fs::g_global_fd_table().get(1);

    TEST_ASSERT_NOT_NULL(f0);
    TEST_ASSERT_NOT_NULL(f1);
    TEST_ASSERT_EQ(f0->flags, OpenFlags::RDONLY);
    TEST_ASSERT_EQ(f1->flags, OpenFlags::WRONLY);

    // Write through stdout fd
    const char msg[] = "ok";
    int64_t w = f1->inode->ops->write(f1->inode, 0, msg, 2);
    TEST_ASSERT_EQ(w, 2);

    // Read back from stdout pipe directly
    char buf[8] = {};
    int64_t r = stdout_pipe.try_read(buf, 8);
    TEST_ASSERT_EQ(r, 2);
    TEST_ASSERT_EQ(buf[0], 'o');
    TEST_ASSERT_EQ(buf[1], 'k');

    // Cleanup
    cinux::fs::g_global_fd_table().close(0);
    cinux::fs::g_global_fd_table().close(1);
    delete stdout_file;
    delete stdin_file;
    delete stdout_inode;
    delete stdin_inode;
    delete stdout_ops;
    delete stdin_ops;
}

// ============================================================
// 5. Terminal with both pipes: on_key does not echo, poll does
// ============================================================

/// Verify that when both pipes are connected, on_key does not echo
/// and poll_output is needed to display shell output
void test_terminal_both_pipes_no_direct_echo() {
    Terminal term(0, 0);
    Pipe stdin_pipe;
    Pipe stdout_pipe;

    term.set_stdin_pipe(&stdin_pipe);
    term.set_stdout_pipe(&stdout_pipe);

    // Simulate typing "AB"
    cinux::gui::KeyEvent ev;
    ev.pressed = true;
    ev.ascii = 'A';
    term.on_key(ev);
    ev.ascii = 'B';
    term.on_key(ev);

    // Screen should still be empty (no direct echo)
    TEST_ASSERT_EQ(term.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(term.cursor_x(), 0u);

    // Read from stdin pipe and echo back via stdout pipe (as shell would)
    char in_buf[8] = {};
    stdin_pipe.try_read(in_buf, 2);

    stdout_pipe.try_write("AB", 2);

    // Now poll_output should display the echoed text
    term.poll_output();
    TEST_ASSERT_EQ(term.cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(term.cell(0, 1).ch, 'B');
    TEST_ASSERT_EQ(term.cursor_x(), 2u);
}

// ============================================================
// 6. Close button / EOF propagation (Phase 6)
// ============================================================

/// Verify Terminal destructor closes stdin pipe write end (EOF to shell)
void test_terminal_destructor_closes_stdin_pipe() {
    Pipe stdin_pipe;
    Pipe stdout_pipe;

    {
        Terminal term(0, 0);
        term.set_stdin_pipe(&stdin_pipe);
        term.set_stdout_pipe(&stdout_pipe);

        // Pipes should be alive before destruction
        TEST_ASSERT_TRUE(stdin_pipe.writer_alive());
        TEST_ASSERT_TRUE(stdout_pipe.reader_alive());
    }
    // Terminal destroyed -- pipes should be closed on the Terminal side
    TEST_ASSERT_FALSE(stdin_pipe.writer_alive());
    TEST_ASSERT_FALSE(stdout_pipe.reader_alive());

    // Reader end of stdin pipe should still be open (shell side)
    TEST_ASSERT_TRUE(stdin_pipe.reader_alive());
    // Writer end of stdout pipe should still be open (shell side)
    TEST_ASSERT_TRUE(stdout_pipe.writer_alive());
}

/// Verify that after Terminal destruction, shell read returns 0 (EOF)
void test_terminal_eof_propagation_to_shell() {
    Pipe stdin_pipe;
    Pipe stdout_pipe;

    {
        Terminal term(0, 0);
        term.set_stdin_pipe(&stdin_pipe);
        term.set_stdout_pipe(&stdout_pipe);
    }

    // Shell tries to read from stdin -- should get 0 (EOF) immediately
    char buf[16] = {};
    int64_t r = stdin_pipe.try_read(buf, 16);
    TEST_ASSERT_EQ(r, 0);  // EOF: writer closed, buffer empty
}

/// Verify that after Terminal destruction, shell write to stdout returns -1
void test_terminal_destructor_shell_write_fails() {
    Pipe stdin_pipe;
    Pipe stdout_pipe;

    {
        Terminal term(0, 0);
        term.set_stdin_pipe(&stdin_pipe);
        term.set_stdout_pipe(&stdout_pipe);
    }

    // Shell tries to write to stdout -- reader is closed, should return -1
    const char msg[] = "test";
    int64_t w = stdout_pipe.try_write(msg, 4);
    TEST_ASSERT_EQ(w, -1);  // Reader closed
}

/// Verify Terminal destructor with no pipes set does not crash
void test_terminal_destructor_no_pipes_no_crash() {
    Terminal term(0, 0);
    // No pipes set -- destructor should be a safe no-op (besides default cleanup)
    // This test just verifies no crash
    TEST_ASSERT_TRUE(true);
}

/// Verify close button flow: WM destroy -> Terminal dtor -> pipes closed
void test_wm_close_button_closes_terminal_pipes() {
    cinux::drivers::Framebuffer fb;
    cinux::drivers::PSFFont font;
    cinux::drivers::Canvas screen;

    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    fb.init(*bi);
    font.init();
    fb.clear(0);
    screen.init(fb);

    cinux::gui::WindowManager wm;
    wm.init(&screen, &font);

    // Create pipes for the terminal
    auto* stdin_pipe = new Pipe();
    auto* stdout_pipe = new Pipe();

    // Create Terminal and connect pipes
    auto* term = new cinux::gui::Terminal(0, 0, "TestClose");
    term->set_stdin_pipe(stdin_pipe);
    term->set_stdout_pipe(stdout_pipe);

    uint32_t id = wm.add_window(term);
    TEST_ASSERT_NE(id, 0u);
    TEST_ASSERT_EQ(wm.window_count(), 1u);

    // Simulate close button click
    wm.destroy(id);

    // Window should be destroyed
    TEST_ASSERT_EQ(wm.window_count(), 0u);
    TEST_ASSERT_NULL(wm.focused());

    // Pipes should be closed on the Terminal side
    TEST_ASSERT_FALSE(stdin_pipe->writer_alive());
    TEST_ASSERT_FALSE(stdout_pipe->reader_alive());

    // Cleanup
    delete stdin_pipe;
    delete stdout_pipe;
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_terminal_shell_tests() {
    TEST_SECTION("Terminal-Shell Integration Tests (Phase 5 & 6)");

    // Phase 5: pipe forwarding
    RUN_TEST(test_terminal_on_key_writes_to_stdin_pipe);
    RUN_TEST(test_terminal_on_key_converts_cr_to_lf);
    RUN_TEST(test_terminal_on_key_no_screen_write_with_pipe);
    RUN_TEST(test_terminal_poll_output_reads_stdout);
    RUN_TEST(test_terminal_poll_output_empty_pipe);
    RUN_TEST(test_terminal_poll_output_with_newline);
    RUN_TEST(test_terminal_poll_output_no_pipe);
    RUN_TEST(test_terminal_shell_full_roundtrip);
    RUN_TEST(test_terminal_pipe_fd_table_binding);
    RUN_TEST(test_terminal_both_pipes_no_direct_echo);

    // Phase 6: close button / EOF propagation
    RUN_TEST(test_terminal_destructor_closes_stdin_pipe);
    RUN_TEST(test_terminal_eof_propagation_to_shell);
    RUN_TEST(test_terminal_destructor_shell_write_fails);
    RUN_TEST(test_terminal_destructor_no_pipes_no_crash);
    RUN_TEST(test_wm_close_button_closes_terminal_pipes);

    TEST_SUMMARY();
}

#endif  // CINUX_GUI
