/**
 * @file kernel/test/test_terminal.cpp
 * @brief QEMU in-kernel tests for Terminal class (031 Terminal Window)
 *
 * Tests the real kernel Terminal class with character buffer, cursor
 * management, write, ANSI escape sequences, scroll, backspace, tab,
 * clear, and on_key.
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Heap initialised (for Terminal construction)
 *
 * Compile condition: CINUX_GUI
 */

#include "big_kernel_test.h"

#ifdef CINUX_GUI

#include "kernel/gui/terminal.hpp"

using cinux::gui::Terminal;

// ============================================================
// Construction tests
// ============================================================

/// Verify Terminal construction initialises screen to spaces
void test_terminal_construction() {
    Terminal t(0, 0);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(t.cell(12, 34).ch, ' ');
    TEST_ASSERT_EQ(t.cell(24, 79).ch, ' ');
    TEST_ASSERT_EQ(t.cell(0, 0).fg, 0x00FFFFFFu);
    TEST_ASSERT_EQ(t.cell(0, 0).bg, 0x00000000u);
    TEST_ASSERT_EQ(t.cursor_x(), 0u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

/// Verify Terminal sets window size from COLS/ROWS
void test_terminal_window_size() {
    Terminal t(100, 200);

    TEST_ASSERT_EQ(t.width(), 640u);   // 80 * 8
    TEST_ASSERT_EQ(t.height(), 400u);  // 25 * 16
    TEST_ASSERT_EQ(t.x(), 100);
    TEST_ASSERT_EQ(t.y(), 200);
}

/// Verify pipe_write_fd defaults to -1
void test_terminal_pipe_fd_default() {
    Terminal t(0, 0);

    TEST_ASSERT_EQ(t.pipe_write_fd(), -1);
}

// ============================================================
// write tests
// ============================================================

/// Verify write places characters in the buffer
void test_terminal_write_hello() {
    Terminal t(0, 0);

    t.write("Hello", 5);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'H');
    TEST_ASSERT_EQ(t.cell(0, 1).ch, 'e');
    TEST_ASSERT_EQ(t.cell(0, 2).ch, 'l');
    TEST_ASSERT_EQ(t.cell(0, 3).ch, 'l');
    TEST_ASSERT_EQ(t.cell(0, 4).ch, 'o');
    TEST_ASSERT_EQ(t.cursor_x(), 5u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

/// Verify write newline moves cursor to next line
void test_terminal_write_newline() {
    Terminal t(0, 0);

    t.write("AB\nCD", 5);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(t.cell(0, 1).ch, 'B');
    TEST_ASSERT_EQ(t.cell(1, 0).ch, 'C');
    TEST_ASSERT_EQ(t.cell(1, 1).ch, 'D');
    TEST_ASSERT_EQ(t.cursor_x(), 2u);
    TEST_ASSERT_EQ(t.cursor_y(), 1u);
}

/// Verify write carriage return resets column
void test_terminal_write_cr() {
    Terminal t(0, 0);

    t.write("AB\rX", 4);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'X');
    TEST_ASSERT_EQ(t.cell(0, 1).ch, 'B');
    TEST_ASSERT_EQ(t.cursor_x(), 1u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

/// Verify write wraps at end of line
void test_terminal_write_wrap() {
    Terminal t(0, 0);

    // Write exactly COLS characters
    char line[81];
    for (int i = 0; i < 80; i++) {
        line[i] = 'A' + static_cast<char>(i % 26);
    }
    line[80] = '\0';
    t.write(line, 80);

    TEST_ASSERT_EQ(t.cursor_x(), 0u);
    TEST_ASSERT_EQ(t.cursor_y(), 1u);
}

/// Verify write scrolls when reaching bottom
void test_terminal_write_scroll() {
    Terminal t(0, 0);

    // Fill all 25 rows and trigger scroll (two scrolls total)
    for (uint32_t row = 0; row < 26; row++) {
        char marker = static_cast<char>('A' + (row % 26));
        t.write(&marker, 1);
        t.write("\n", 1);
    }

    // After two scrolls, row 0 should have 'C' (original row 2)
    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'C');
    TEST_ASSERT_EQ(t.cursor_y(), 24u);
}

// ============================================================
// backspace tests
// ============================================================

/// Verify backspace deletes previous character
void test_terminal_backspace() {
    Terminal t(0, 0);

    t.write("ABC\b", 4);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(t.cell(0, 1).ch, 'B');
    TEST_ASSERT_EQ(t.cell(0, 2).ch, ' ');
    TEST_ASSERT_EQ(t.cursor_x(), 2u);
}

/// Verify backspace at beginning of line wraps to previous line
void test_terminal_backspace_wrap() {
    Terminal t(0, 0);

    t.write("AB\n", 3);
    t.write("\b", 1);

    TEST_ASSERT_EQ(t.cursor_x(), 79u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

// ============================================================
// tab tests
// ============================================================

/// Verify tab advances to next 8-column stop
void test_terminal_tab() {
    Terminal t(0, 0);

    t.write("A\tB", 3);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(t.cell(0, 8).ch, 'B');
    TEST_ASSERT_EQ(t.cursor_x(), 9u);
}

// ============================================================
// clear tests
// ============================================================

/// Verify clear resets all cells and cursor
void test_terminal_clear() {
    Terminal t(0, 0);

    t.write("Hello World", 11);
    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'H');

    t.clear();

    TEST_ASSERT_EQ(t.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(t.cell(0, 5).ch, ' ');
    TEST_ASSERT_EQ(t.cursor_x(), 0u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

// ============================================================
// on_key tests
// ============================================================

/// Verify on_key writes printable character
void test_terminal_on_key_printable() {
    Terminal t(0, 0);

    cinux::gui::KeyEvent ev{};
    ev.ascii = 'Z';
    ev.pressed = true;
    ev.scancode = 0;
    ev.shift = false;
    ev.ctrl = false;
    ev.alt = false;

    t.on_key(ev);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'Z');
    TEST_ASSERT_EQ(t.cursor_x(), 1u);
}

/// Verify on_key ignores key release
void test_terminal_on_key_release() {
    Terminal t(0, 0);

    cinux::gui::KeyEvent ev{};
    ev.ascii = 'Z';
    ev.pressed = false;

    t.on_key(ev);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(t.cursor_x(), 0u);
}

// ============================================================
// ANSI escape sequence tests
// ============================================================

/// Verify ESC[2J clears screen
void test_terminal_ansi_clear_screen() {
    Terminal t(0, 0);

    t.write("Hello", 5);
    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'H');

    t.write("\033[2J", 4);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(t.cursor_x(), 0u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

/// Verify ESC[H moves cursor home
void test_terminal_ansi_cursor_home() {
    Terminal t(0, 0);

    t.write("ABC\nDE", 6);
    TEST_ASSERT_EQ(t.cursor_y(), 1u);

    t.write("\033[H", 3);

    TEST_ASSERT_EQ(t.cursor_x(), 0u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

/// Verify ESC[K clears to end of line
void test_terminal_ansi_clear_eol() {
    Terminal t(0, 0);

    t.write("ABCDE", 5);

    // Move cursor back
    t.write("\b\b", 2);
    TEST_ASSERT_EQ(t.cursor_x(), 3u);

    t.write("\033[K", 3);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, 'A');
    TEST_ASSERT_EQ(t.cell(0, 1).ch, 'B');
    TEST_ASSERT_EQ(t.cell(0, 2).ch, 'C');  // not cleared (before cursor)
    TEST_ASSERT_EQ(t.cell(0, 3).ch, ' ');  // cleared (at cursor)
    TEST_ASSERT_EQ(t.cell(0, 4).ch, ' ');  // cleared
}

/// Verify combined ESC[2J ESC[H
void test_terminal_ansi_combined() {
    Terminal t(0, 0);

    t.write("Line1\nLine2\nLine3", 17);

    t.write("\033[2J\033[H", 7);

    TEST_ASSERT_EQ(t.cell(0, 0).ch, ' ');
    TEST_ASSERT_EQ(t.cell(1, 0).ch, ' ');
    TEST_ASSERT_EQ(t.cursor_x(), 0u);
    TEST_ASSERT_EQ(t.cursor_y(), 0u);
}

// ============================================================
// Constants test
// ============================================================

/// Verify Terminal constants
void test_terminal_constants() {
    TEST_ASSERT_EQ(Terminal::COLS, 80u);
    TEST_ASSERT_EQ(Terminal::ROWS, 25u);
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_terminal_tests() {
    TEST_SECTION("Terminal Tests (031 Terminal Window)");

    // Construction
    RUN_TEST(test_terminal_construction);
    RUN_TEST(test_terminal_window_size);
    RUN_TEST(test_terminal_pipe_fd_default);

    // write
    RUN_TEST(test_terminal_write_hello);
    RUN_TEST(test_terminal_write_newline);
    RUN_TEST(test_terminal_write_cr);
    RUN_TEST(test_terminal_write_wrap);
    RUN_TEST(test_terminal_write_scroll);

    // backspace
    RUN_TEST(test_terminal_backspace);
    RUN_TEST(test_terminal_backspace_wrap);

    // tab
    RUN_TEST(test_terminal_tab);

    // clear
    RUN_TEST(test_terminal_clear);

    // on_key
    RUN_TEST(test_terminal_on_key_printable);
    RUN_TEST(test_terminal_on_key_release);

    // ANSI
    RUN_TEST(test_terminal_ansi_clear_screen);
    RUN_TEST(test_terminal_ansi_cursor_home);
    RUN_TEST(test_terminal_ansi_clear_eol);
    RUN_TEST(test_terminal_ansi_combined);

    // Constants
    RUN_TEST(test_terminal_constants);

    TEST_SUMMARY();
}

#else  // !CINUX_GUI

// CLI mode stub: no Terminal tests to run
extern "C" void run_terminal_tests() {
    using cinux::lib::kprintf;
    kprintf("[TERMINAL] CLI mode -- Terminal tests skipped.\n");
}

#endif  // CINUX_GUI
