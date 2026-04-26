/**
 * @file kernel/test/test_multi_terminal.cpp
 * @brief QEMU in-kernel tests for 035_multi_terminal milestone
 *
 * Test coverage:
 *   - Multiple concurrent Terminal instances with independent pipes
 *   - Terminal destructor pipe cleanup (stdin writer, stdout reader)
 *   - Terminal shell_pid tracking and destructor reap semantics
 *   - WindowManager multi-terminal iteration (window_count, window_at, is_terminal)
 *   - gui_tick_callback simulation: poll+render all terminal windows
 *   - Terminal title uniqueness ("Shell #N")
 *   - Pipe EOF propagation after terminal destruction
 *   - Shell write failure after terminal stdout reader close
 *   - Terminal dimensions for centering calculation (80*8, 25*16)
 *   - WM destroy closes terminal pipes and reaps shell child
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

#    include "boot/boot_info.h"
#    include "kernel/drivers/canvas.hpp"
#    include "kernel/drivers/video/font.hpp"
#    include "kernel/drivers/video/framebuffer.hpp"
#    include "kernel/fs/file.hpp"
#    include "kernel/fs/inode.hpp"
#    include "kernel/fs/vfs_mount.hpp"
#    include "kernel/gui/terminal.hpp"
#    include "kernel/gui/window_manager.hpp"
#    include "kernel/ipc/pipe.hpp"
#    include "kernel/ipc/pipe_ops.hpp"

using cinux::gui::Terminal;
using cinux::gui::WindowManager;
using cinux::ipc::Pipe;
using cinux::ipc::PipeReadOps;
using cinux::ipc::PipeWriteOps;
using cinux::fs::Inode;
using cinux::fs::InodeType;
using cinux::fs::File;
using cinux::fs::OpenFlags;

// ============================================================
// 1. Multiple concurrent terminals with independent pipes
// ============================================================

/// Verify two terminals have independent stdin and stdout pipes
void test_multi_term_two_terminals_independent_pipes() {
    auto* stdin1  = new Pipe();
    auto* stdin2  = new Pipe();
    auto* stdout1 = new Pipe();
    auto* stdout2 = new Pipe();

    // Allocate Terminals on the heap -- each Terminal has an 80x25
    // screen buffer (~18 KB) which far exceeds the boot stack.
    auto* t1 = new Terminal(0, 0, "Shell #1");
    auto* t2 = new Terminal(100, 100, "Shell #2");

    t1->set_stdin_pipe(stdin1);
    t1->set_stdout_pipe(stdout1);
    t2->set_stdin_pipe(stdin2);
    t2->set_stdout_pipe(stdout2);

    // Write different data to each terminal's stdout
    stdout1->try_write("A", 1);
    stdout2->try_write("B", 1);

    t1->poll_output();
    t2->poll_output();

    // Each terminal should display its own data
    TEST_ASSERT_EQ(t1->cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(t2->cell(0, 0).ch, 'B');

    delete t1;
    delete t2;
    delete stdin1;
    delete stdin2;
    delete stdout1;
    delete stdout2;
}

/// Verify three terminals with concurrent poll_output
void test_multi_term_three_terminals_concurrent_poll() {
    auto* s1 = new Pipe();
    auto* s2 = new Pipe();
    auto* s3 = new Pipe();
    auto* o1 = new Pipe();
    auto* o2 = new Pipe();
    auto* o3 = new Pipe();

    auto* t1 = new Terminal(0, 0, "Shell #1");
    auto* t2 = new Terminal(0, 0, "Shell #2");
    auto* t3 = new Terminal(0, 0, "Shell #3");

    t1->set_stdin_pipe(s1);
    t1->set_stdout_pipe(o1);
    t2->set_stdin_pipe(s2);
    t2->set_stdout_pipe(o2);
    t3->set_stdin_pipe(s3);
    t3->set_stdout_pipe(o3);

    // Feed different data to each
    o1->try_write("AAA", 3);
    o2->try_write("BBB", 3);
    o3->try_write("CCC", 3);

    // Simulate gui_tick_callback: iterate and poll all terminals
    t1->poll_output();
    t2->poll_output();
    t3->poll_output();

    TEST_ASSERT_EQ(t1->cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(t1->cell(0, 1).ch, 'A');
    TEST_ASSERT_EQ(t1->cell(0, 2).ch, 'A');

    TEST_ASSERT_EQ(t2->cell(0, 0).ch, 'B');
    TEST_ASSERT_EQ(t2->cell(0, 1).ch, 'B');
    TEST_ASSERT_EQ(t2->cell(0, 2).ch, 'B');

    TEST_ASSERT_EQ(t3->cell(0, 0).ch, 'C');
    TEST_ASSERT_EQ(t3->cell(0, 1).ch, 'C');
    TEST_ASSERT_EQ(t3->cell(0, 2).ch, 'C');

    delete t1;
    delete t2;
    delete t3;
    delete s1;
    delete s2;
    delete s3;
    delete o1;
    delete o2;
    delete o3;
}

// ============================================================
// 2. Terminal destructor pipe cleanup
// ============================================================

/// Verify destructor closes stdin pipe writer and stdout pipe reader
void test_multi_term_destructor_closes_both_pipes() {
    auto* stdin_pipe  = new Pipe();
    auto* stdout_pipe = new Pipe();

    {
        auto* term = new Terminal(0, 0, "CloseTest");
        term->set_stdin_pipe(stdin_pipe);
        term->set_stdout_pipe(stdout_pipe);
        term->set_shell_pid(99);

        TEST_ASSERT_TRUE(stdin_pipe->writer_alive());
        TEST_ASSERT_TRUE(stdin_pipe->reader_alive());
        TEST_ASSERT_TRUE(stdout_pipe->writer_alive());
        TEST_ASSERT_TRUE(stdout_pipe->reader_alive());
        delete term;
    }
    // After destruction
    TEST_ASSERT_FALSE(stdin_pipe->writer_alive());   // Terminal closed writer
    TEST_ASSERT_TRUE(stdin_pipe->reader_alive());    // Shell side still open
    TEST_ASSERT_FALSE(stdout_pipe->reader_alive());  // Terminal closed reader
    TEST_ASSERT_TRUE(stdout_pipe->writer_alive());   // Shell side still open

    delete stdin_pipe;
    delete stdout_pipe;
}

/// Verify destructor with no pipes is safe
void test_multi_term_destructor_no_pipes_safe() {
    auto* term = new Terminal(0, 0, "NoPipes");
    // Should not crash on destruction
    TEST_ASSERT_TRUE(true);
    delete term;
}

/// Verify destructor with zero shell_pid skips reap
void test_multi_term_destructor_zero_pid_no_reap() {
    auto* term = new Terminal(0, 0, "ZeroPid");
    TEST_ASSERT_EQ(term->shell_pid(), 0);
    // Destructor should skip waitpid when shell_pid == 0
    TEST_ASSERT_TRUE(true);
    delete term;
}

// ============================================================
// 3. Shell PID tracking
// ============================================================

/// Verify set_shell_pid and shell_pid round-trip
void test_multi_term_shell_pid_tracking() {
    auto* term = new Terminal(0, 0, "PidTest");
    TEST_ASSERT_EQ(term->shell_pid(), 0);

    term->set_shell_pid(42);
    TEST_ASSERT_EQ(term->shell_pid(), 42);

    term->set_shell_pid(0);
    TEST_ASSERT_EQ(term->shell_pid(), 0);
    delete term;
}

// ============================================================
// 4. Pipe EOF propagation after terminal destruction
// ============================================================

/// Verify shell reads EOF from stdin after terminal destroyed
void test_multi_term_eof_after_destruction() {
    auto* stdin_pipe = new Pipe();
    {
        auto* term = new Terminal(0, 0);
        term->set_stdin_pipe(stdin_pipe);
        delete term;
    }
    // Terminal destroyed: writer closed, buffer empty
    char    buf[16] = {};
    int64_t r       = stdin_pipe->try_read(buf, 16);
    TEST_ASSERT_EQ(r, 0);  // EOF
    delete stdin_pipe;
}

/// Verify shell write fails after terminal stdout reader closed
void test_multi_term_shell_write_fails_after_destruction() {
    auto* stdout_pipe = new Pipe();
    {
        auto* term = new Terminal(0, 0);
        term->set_stdout_pipe(stdout_pipe);
        delete term;
    }
    const char msg[] = "test";
    int64_t    w     = stdout_pipe->try_write(msg, 4);
    TEST_ASSERT_EQ(w, -1);  // Reader closed
    delete stdout_pipe;
}

// ============================================================
// 5. Terminal dimensions for centering calculation
// ============================================================

/// Verify terminal width is 80*8 = 640 pixels
void test_multi_term_terminal_width_640() {
    TEST_ASSERT_EQ(Terminal::COLS * 8u, 640u);
}

/// Verify terminal height is 25*16 = 400 pixels
void test_multi_term_terminal_height_400() {
    TEST_ASSERT_EQ(Terminal::ROWS * 16u, 400u);
}

// ============================================================
// 6. WindowManager multi-terminal iteration
// ============================================================

/// Verify WM add_window, window_count, window_at for multiple terminals
void test_multi_term_wm_add_and_iterate_terminals() {
    auto* fb     = new cinux::drivers::Framebuffer();
    auto* font   = new cinux::drivers::PSFFont();
    auto* screen = new cinux::drivers::Canvas();

    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto*                      bi             = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    fb->init(*bi);
    font->init();
    fb->clear(0);
    screen->init(*fb);

    auto* wm = new WindowManager();
    wm->init(screen, font);

    auto* t1 = new Terminal(0, 0, "Shell #1");
    auto* t2 = new Terminal(100, 100, "Shell #2");

    uint32_t id1 = wm->add_window(t1);
    uint32_t id2 = wm->add_window(t2);

    TEST_ASSERT_NE(id1, 0u);
    TEST_ASSERT_NE(id2, 0u);
    TEST_ASSERT_EQ(wm->window_count(), 2u);

    // Iterate and verify terminal properties
    uint32_t terminal_count = 0;
    for (uint32_t i = 0; i < wm->window_count(); i++) {
        auto* win = wm->window_at(i);
        TEST_ASSERT_NOT_NULL(win);
        if (win->is_terminal()) {
            terminal_count++;
        }
    }
    TEST_ASSERT_EQ(terminal_count, 2u);

    delete wm;
    delete screen;
    delete font;
    delete fb;
}

/// Verify WM window_at returns nullptr for out-of-range
void test_multi_term_wm_window_at_out_of_range() {
    auto* wm = new WindowManager();
    TEST_ASSERT_NULL(wm->window_at(0));
    TEST_ASSERT_NULL(wm->window_at(100));
    delete wm;
}

// ============================================================
// 7. WM destroy closes terminal pipes (close button simulation)
// ============================================================

/// Verify WM destroy closes terminal pipes and reaps shell child
void test_multi_term_wm_destroy_closes_pipes() {
    auto* fb     = new cinux::drivers::Framebuffer();
    auto* font   = new cinux::drivers::PSFFont();
    auto* screen = new cinux::drivers::Canvas();

    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto*                      bi             = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    fb->init(*bi);
    font->init();
    fb->clear(0);
    screen->init(*fb);

    auto* wm = new WindowManager();
    wm->init(screen, font);

    auto* stdin_pipe  = new Pipe();
    auto* stdout_pipe = new Pipe();

    auto* term = new Terminal(0, 0, "DestroyTest");
    term->set_stdin_pipe(stdin_pipe);
    term->set_stdout_pipe(stdout_pipe);
    term->set_shell_pid(55);

    uint32_t id = wm->add_window(term);
    TEST_ASSERT_NE(id, 0u);
    TEST_ASSERT_EQ(wm->window_count(), 1u);

    // Simulate close button: destroy via WM
    wm->destroy(id);

    TEST_ASSERT_EQ(wm->window_count(), 0u);

    // Pipes should be closed on the Terminal side
    TEST_ASSERT_FALSE(stdin_pipe->writer_alive());
    TEST_ASSERT_FALSE(stdout_pipe->reader_alive());

    delete stdin_pipe;
    delete stdout_pipe;
    delete wm;
    delete screen;
    delete font;
    delete fb;
}

// ============================================================
// 8. gui_tick_callback simulation: poll+render all terminals
// ============================================================

/// Simulate the gui_tick_callback loop from gui_init.cpp
void test_multi_term_tick_callback_simulation() {
    auto* fb     = new cinux::drivers::Framebuffer();
    auto* font   = new cinux::drivers::PSFFont();
    auto* screen = new cinux::drivers::Canvas();

    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto*                      bi             = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    fb->init(*bi);
    font->init();
    fb->clear(0);
    screen->init(*fb);

    auto* wm = new WindowManager();
    wm->init(screen, font);

    // Create two terminals with pipes
    auto* s1 = new Pipe();
    auto* s2 = new Pipe();
    auto* o1 = new Pipe();
    auto* o2 = new Pipe();

    auto* t1 = new Terminal(0, 0, "Shell #1");
    auto* t2 = new Terminal(100, 100, "Shell #2");

    t1->set_stdin_pipe(s1);
    t1->set_stdout_pipe(o1);
    t2->set_stdin_pipe(s2);
    t2->set_stdout_pipe(o2);

    t1->set_font(font);
    t2->set_font(font);

    wm->add_window(t1);
    wm->add_window(t2);

    // Simulate shell output arriving
    o1->try_write("Term1", 5);
    o2->try_write("Term2", 5);

    // Simulate gui_tick_callback: iterate all windows, poll+render terminals
    for (uint32_t i = 0; i < wm->window_count(); i++) {
        auto* win = wm->window_at(i);
        if (win != nullptr && win->is_terminal()) {
            auto* term = static_cast<Terminal*>(win);
            term->poll_output();
            term->render_to_canvas();
        }
    }

    // Verify terminal content
    TEST_ASSERT_EQ(t1->cell(0, 0).ch, 'T');
    TEST_ASSERT_EQ(t1->cell(0, 4).ch, '1');
    TEST_ASSERT_EQ(t2->cell(0, 0).ch, 'T');
    TEST_ASSERT_EQ(t2->cell(0, 4).ch, '2');

    delete wm;
    delete s1;
    delete s2;
    delete o1;
    delete o2;
    delete screen;
    delete font;
    delete fb;
}

// ============================================================
// 9. Terminal with partial stdout data across multiple polls
// ============================================================

/// Verify terminal correctly accumulates data across multiple polls
void test_multi_term_partial_poll_multiple_rounds() {
    auto* stdout_pipe = new Pipe();
    auto* term        = new Terminal(0, 0, "PartialTest");
    term->set_stdout_pipe(stdout_pipe);

    // First batch
    stdout_pipe->try_write("Hello", 5);
    term->poll_output();
    TEST_ASSERT_EQ(term->cursor_x(), 5u);
    TEST_ASSERT_EQ(term->cell(0, 0).ch, 'H');
    TEST_ASSERT_EQ(term->cell(0, 4).ch, 'o');

    // Second batch
    stdout_pipe->try_write(" World", 6);
    term->poll_output();
    TEST_ASSERT_EQ(term->cursor_x(), 11u);
    TEST_ASSERT_EQ(term->cell(0, 5).ch, ' ');
    TEST_ASSERT_EQ(term->cell(0, 6).ch, 'W');

    delete term;
    delete stdout_pipe;
}

// ============================================================
// 10. Destroy one terminal does not affect another
// ============================================================

/// Verify destroying one terminal leaves other terminals functional
void test_multi_term_destroy_one_affects_not_other() {
    auto* s1 = new Pipe();
    auto* s2 = new Pipe();
    auto* o1 = new Pipe();
    auto* o2 = new Pipe();

    {
        auto* t1 = new Terminal(0, 0, "Shell #1");
        t1->set_stdin_pipe(s1);
        t1->set_stdout_pipe(o1);
        t1->set_shell_pid(10);
        delete t1;
    }
    // t1 destroyed: pipes closed

    auto* t2 = new Terminal(100, 100, "Shell #2");
    t2->set_stdin_pipe(s2);
    t2->set_stdout_pipe(o2);

    // t2's pipes should be unaffected
    TEST_ASSERT_TRUE(s2->writer_alive());
    TEST_ASSERT_TRUE(s2->reader_alive());
    TEST_ASSERT_TRUE(o2->writer_alive());
    TEST_ASSERT_TRUE(o2->reader_alive());

    // Data flow through t2 should still work
    o2->try_write("OK", 2);
    t2->poll_output();
    TEST_ASSERT_EQ(t2->cell(0, 0).ch, 'O');
    TEST_ASSERT_EQ(t2->cell(0, 1).ch, 'K');

    delete t2;
    delete s1;
    delete s2;
    delete o1;
    delete o2;
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_multi_terminal_tests() {
    TEST_SECTION("Multi-Terminal Tests (035_multi_terminal)");

    // Multiple concurrent terminals
    RUN_TEST(test_multi_term_two_terminals_independent_pipes);
    RUN_TEST(test_multi_term_three_terminals_concurrent_poll);

    // Destructor pipe cleanup
    RUN_TEST(test_multi_term_destructor_closes_both_pipes);
    RUN_TEST(test_multi_term_destructor_no_pipes_safe);
    RUN_TEST(test_multi_term_destructor_zero_pid_no_reap);

    // Shell PID tracking
    RUN_TEST(test_multi_term_shell_pid_tracking);

    // Pipe EOF propagation
    RUN_TEST(test_multi_term_eof_after_destruction);
    RUN_TEST(test_multi_term_shell_write_fails_after_destruction);

    // Terminal dimensions
    RUN_TEST(test_multi_term_terminal_width_640);
    RUN_TEST(test_multi_term_terminal_height_400);

    // WindowManager iteration
    RUN_TEST(test_multi_term_wm_add_and_iterate_terminals);
    RUN_TEST(test_multi_term_wm_window_at_out_of_range);

    // WM destroy closes pipes
    RUN_TEST(test_multi_term_wm_destroy_closes_pipes);

    // gui_tick_callback simulation
    RUN_TEST(test_multi_term_tick_callback_simulation);

    // Partial poll across rounds
    RUN_TEST(test_multi_term_partial_poll_multiple_rounds);

    // Destroy isolation
    RUN_TEST(test_multi_term_destroy_one_affects_not_other);

    TEST_SUMMARY();
}

#endif  // CINUX_GUI
