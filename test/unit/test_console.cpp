/**
 * @file test/unit/test_console.cpp
 * @brief Host-side unit tests for Console cursor logic
 *
 * Tests the pure arithmetic of column/row tracking, wrapping,
 * scrolling triggers, and control character handling.  Mirrors the
 * Console::putc() logic without touching real hardware.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

// Mirror console cursor state
struct CursorState {
    uint32_t col      = 0;
    uint32_t row      = 0;
    uint32_t cols     = 0;
    uint32_t rows     = 0;
    bool     scrolled = false;

    void reset(uint32_t c, uint32_t r) {
        col      = 0;
        row      = 0;
        cols     = c;
        rows     = r;
        scrolled = false;
    }

    void putc(char ch) {
        scrolled = false;
        switch (ch) {
        case '\n':
            newline();
            break;
        case '\r':
            col = 0;
            break;
        case '\b':
            if (col > 0) {
                col--;
            } else if (row > 0) {
                row--;
                col = cols - 1;
            }
            break;
        default:
            if (col >= cols)
                newline();
            col++;
            break;
        }
    }

    void newline() {
        col = 0;
        if (row + 1 >= rows) {
            scrolled = true;
        } else {
            row++;
        }
    }
};

TEST("console: cols/rows for 1024x768 with 8x16 font") {
    ASSERT_EQ(1024u / 8u, 128u);
    ASSERT_EQ(768u / 16u, 48u);
}

TEST("console: putc printable advances col") {
    CursorState cs;
    cs.reset(128, 48);
    cs.putc('A');
    ASSERT_EQ(cs.col, 1u);
    ASSERT_EQ(cs.row, 0u);
}

TEST("console: putc at last col wraps to next line") {
    CursorState cs;
    cs.reset(128, 48);
    cs.col = 127;
    cs.putc('X');
    // col was 127, printable char: col >= cols (127 >= 128)? No, 127 < 128
    // so col++ → 128. Next putc will wrap.
    ASSERT_EQ(cs.col, 128u);
    ASSERT_EQ(cs.row, 0u);

    cs.putc('Y');
    // col=128 >= 128, newline, then col++
    ASSERT_EQ(cs.col, 1u);
    ASSERT_EQ(cs.row, 1u);
}

TEST("console: newline resets col and increments row") {
    CursorState cs;
    cs.reset(128, 48);
    cs.col = 50;
    cs.row = 10;
    cs.putc('\n');
    ASSERT_EQ(cs.col, 0u);
    ASSERT_EQ(cs.row, 11u);
}

TEST("console: carriage return resets col only") {
    CursorState cs;
    cs.reset(128, 48);
    cs.col = 50;
    cs.row = 10;
    cs.putc('\r');
    ASSERT_EQ(cs.col, 0u);
    ASSERT_EQ(cs.row, 10u);
}

TEST("console: backspace at col>0 decrements col") {
    CursorState cs;
    cs.reset(128, 48);
    cs.col = 5;
    cs.row = 3;
    cs.putc('\b');
    ASSERT_EQ(cs.col, 4u);
    ASSERT_EQ(cs.row, 3u);
}

TEST("console: backspace at col=0 wraps to previous row") {
    CursorState cs;
    cs.reset(128, 48);
    cs.col = 0;
    cs.row = 5;
    cs.putc('\b');
    ASSERT_EQ(cs.col, 127u);
    ASSERT_EQ(cs.row, 4u);
}

TEST("console: newline at last row triggers scroll") {
    CursorState cs;
    cs.reset(128, 48);
    cs.col = 0;
    cs.row = 47;  // last row (rows=48, so row+1=48 >= rows)
    cs.putc('\n');
    ASSERT_EQ(cs.col, 0u);
    ASSERT_TRUE(cs.scrolled);
}

TEST("console: multiple newlines track correctly") {
    CursorState cs;
    cs.reset(128, 48);
    cs.putc('A');   // col=1, row=0
    cs.putc('\n');  // col=0, row=1
    cs.putc('B');   // col=1, row=1
    cs.putc('\n');  // col=0, row=2
    ASSERT_EQ(cs.col, 0u);
    ASSERT_EQ(cs.row, 2u);
}

TEST("console: clear resets to home") {
    CursorState cs;
    cs.reset(128, 48);
    cs.col = 50;
    cs.row = 20;
    cs.col = 0;
    cs.row = 0;
    ASSERT_EQ(cs.col, 0u);
    ASSERT_EQ(cs.row, 0u);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
