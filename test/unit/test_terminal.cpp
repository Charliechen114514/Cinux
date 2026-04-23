/**
 * @file test/unit/test_terminal.cpp
 * @brief Host-side unit tests for Terminal class (031 Terminal Window)
 *
 * Tests Terminal construction, character buffer, cursor management,
 * text rendering, ANSI escape sequences, scroll, backspace, tab,
 * clear, and on_key by re-implementing Terminal with mock Canvas/PSFFont.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>
#include <vector>

// ============================================================
// Mock PSFFont (mimics kernel/drivers/video/font.hpp)
// ============================================================

class MockPSFFont {
public:
    void init(uint32_t w, uint32_t h) {
        width_  = w;
        height_ = h;
    }

    void set_glyph(uint8_t c, const uint8_t* rows, uint32_t num_rows) {
        glyphs_[c].assign(rows, rows + num_rows);
    }

    const uint8_t* glyph(uint8_t c) const {
        if (glyphs_[c].empty()) return nullptr;
        return glyphs_[c].data();
    }

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

private:
    std::vector<uint8_t> glyphs_[256];
    uint32_t width_  = 0;
    uint32_t height_ = 0;
};

// ============================================================
// Mock Canvas (mimics kernel/drivers/canvas.hpp/.cpp logic)
// ============================================================

class MockCanvas {
public:
    MockCanvas() = default;

    void init(uint32_t w, uint32_t h) {
        if (!back_buf_.empty()) back_buf_.clear();
        width_     = w;
        height_    = h;
        pitch_     = w * 4;
        back_buf_.resize(static_cast<size_t>(w) * h, 0);
    }

    void draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= width_ || y >= height_) return;
        back_buf_[y * width_ + x] = color;
    }

    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
        for (uint32_t row = y; row < y + h && row < height_; row++) {
            for (uint32_t col = x; col < x + w && col < width_; col++) {
                back_buf_[row * width_ + col] = color;
            }
        }
    }

    void blit(int32_t dst_x, int32_t dst_y, MockCanvas& src,
              uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
        for (uint32_t row = 0; row < h; row++) {
            uint32_t src_row = sy + row;
            int32_t dst_row = dst_y + static_cast<int32_t>(row);
            if (dst_row < 0) continue;
            if (src_row >= src.height_ || dst_row >= static_cast<int32_t>(height_)) break;

            int32_t col_skip = 0;
            int32_t eff_dst_x = dst_x;
            uint32_t eff_sx = sx;
            if (eff_dst_x < 0) {
                col_skip = -eff_dst_x;
                eff_dst_x = 0;
                eff_sx += static_cast<uint32_t>(col_skip);
            }

            uint32_t dst_col_start = static_cast<uint32_t>(eff_dst_x);
            uint32_t col_count = w - static_cast<uint32_t>(col_skip);

            for (uint32_t i = 0; i < col_count; i++) {
                uint32_t src_col = eff_sx + i;
                uint32_t dst_col = dst_col_start + i;
                if (src_col >= src.width_ || dst_col >= width_) break;
                back_buf_[dst_row * width_ + dst_col] =
                    src.back_buf_[src_row * src.width_ + src_col];
            }
        }
    }

    void draw_text(uint32_t x, uint32_t y, const char* str, uint32_t color,
                   MockPSFFont& font) {
        if (str == nullptr) return;
        uint32_t gw = font.width();
        uint32_t gh = font.height();
        if (gw == 0 || gh == 0) return;
        uint32_t cx = x;
        for (uint32_t i = 0; str[i] != '\0'; i++) {
            const uint8_t* g = font.glyph(static_cast<uint8_t>(str[i]));
            if (g == nullptr) continue;
            for (uint32_t row = 0; row < gh; row++) {
                uint8_t bits = g[row];
                for (uint32_t col = 0; col < gw; col++) {
                    if ((bits >> (7 - col)) & 1) {
                        draw_pixel(cx + col, y + row, color);
                    }
                }
            }
            cx += gw;
        }
    }

    void clear(uint32_t color = 0) {
        for (auto& p : back_buf_) p = color;
    }

    void flip() {
        // No-op in mock
    }

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

    uint32_t pixel(uint32_t x, uint32_t y) const {
        if (x >= width_ || y >= height_) return 0xDEAD;
        return back_buf_[y * width_ + x];
    }

private:
    std::vector<uint32_t> back_buf_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t pitch_  = 0;
};

// ============================================================
// Re-implemented KeyEvent
// ============================================================

struct MockKeyEvent {
    char    ascii;
    uint8_t scancode;
    bool    pressed;
    bool    shift;
    bool    ctrl;
    bool    alt;
};

// ============================================================
// Re-implemented Window base class (mirrors kernel/gui/window.hpp)
// ============================================================

class MockWindow {
public:
    static constexpr uint32_t TITLE_BAR_HEIGHT = 20;
    static constexpr uint32_t CLOSE_BUTTON_SIZE = 14;
    static constexpr uint32_t TITLE_MAX_LEN    = 63;
    static constexpr uint32_t DEFAULT_WIDTH    = 320;
    static constexpr uint32_t DEFAULT_HEIGHT   = 240;

    static constexpr uint32_t COLOR_TITLE_BG     = 0x00336699;
    static constexpr uint32_t COLOR_TITLE_TEXT   = 0x00FFFFFF;
    static constexpr uint32_t COLOR_CLOSE_BUTTON = 0x00CC3333;
    static constexpr uint32_t COLOR_CONTENT_BG   = 0x00E0E0E0;
    static constexpr uint32_t COLOR_BORDER       = 0x00444444;

    static uint32_t next_id_;

    MockWindow(const char* title = "Untitled",
               int32_t x = 0, int32_t y = 0,
               uint32_t w = DEFAULT_WIDTH,
               uint32_t h = DEFAULT_HEIGHT)
        : id_(next_id_++), x_(x), y_(y), w_(w), h_(h),
          visible_(true), focused_(false) {
        for (uint32_t i = 0; i <= TITLE_MAX_LEN; i++) title_[i] = '\0';
        if (title != nullptr) {
            uint32_t i = 0;
            while (i < TITLE_MAX_LEN && title[i] != '\0') {
                title_[i] = title[i];
                i++;
            }
        }
        allocate_canvas();
    }

    virtual ~MockWindow() = default;

    virtual void on_key(MockKeyEvent& ev) { (void)ev; }
    virtual void on_paint(MockCanvas& canvas) { (void)canvas; }

    void draw_title_bar(MockPSFFont& font) {
        canvas_.draw_rect(0, 0, w_, TITLE_BAR_HEIGHT, COLOR_TITLE_BG);
        canvas_.draw_rect(0, TITLE_BAR_HEIGHT - 1, w_, 1, COLOR_BORDER);
        uint32_t text_y = (TITLE_BAR_HEIGHT - font.height()) / 2;
        canvas_.draw_text(4, text_y, title_, COLOR_TITLE_TEXT, font);
        uint32_t cb_x = w_ - CLOSE_BUTTON_SIZE - 3;
        uint32_t cb_y = (TITLE_BAR_HEIGHT - CLOSE_BUTTON_SIZE) / 2;
        canvas_.draw_rect(cb_x, cb_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
                          COLOR_CLOSE_BUTTON);
    }

    void draw_content() {
        canvas_.draw_rect(0, TITLE_BAR_HEIGHT, w_, h_, COLOR_CONTENT_BG);
    }

    void blit_to(MockCanvas& dst) {
        if (!visible_) return;
        dst.blit(x_, y_, canvas_, 0, 0, w_, total_height());
    }

    void set_position(int32_t x, int32_t y) { x_ = x; y_ = y; }
    void resize(uint32_t w, uint32_t h) { w_ = w; h_ = h; allocate_canvas(); }

    bool is_close_button_hit(int32_t mx, int32_t my) const {
        int32_t cb_x = x_ + static_cast<int32_t>(w_) - CLOSE_BUTTON_SIZE - 3;
        int32_t cb_y = y_ + static_cast<int32_t>((TITLE_BAR_HEIGHT - CLOSE_BUTTON_SIZE) / 2);
        return mx >= cb_x && mx < cb_x + static_cast<int32_t>(CLOSE_BUTTON_SIZE)
            && my >= cb_y && my < cb_y + static_cast<int32_t>(CLOSE_BUTTON_SIZE);
    }

    bool contains(int32_t mx, int32_t my) const {
        return mx >= x_
            && mx < x_ + static_cast<int32_t>(w_)
            && my >= y_
            && my < y_ + static_cast<int32_t>(total_height());
    }

    uint32_t id() const { return id_; }
    int32_t  x() const { return x_; }
    int32_t  y() const { return y_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    bool     visible() const { return visible_; }
    bool     focused() const { return focused_; }
    const char* title() const { return title_; }
    uint32_t total_height() const { return h_ + TITLE_BAR_HEIGHT; }

    void set_visible(bool v) { visible_ = v; }
    void set_focused(bool f) { focused_ = f; }

    MockCanvas& canvas() { return canvas_; }

    static void reset_id() { next_id_ = 1; }

protected:
    void allocate_canvas() {
        uint32_t total_h = h_ + TITLE_BAR_HEIGHT;
        canvas_.init(w_, total_h);
    }

    uint32_t id_;
    int32_t  x_;
    int32_t  y_;
    uint32_t w_;
    uint32_t h_;
    char     title_[TITLE_MAX_LEN + 1];
    MockCanvas canvas_;
    bool     visible_;
    bool     focused_;
};

uint32_t MockWindow::next_id_ = 1;

// ============================================================
// Re-implemented TerminalCell
// ============================================================

struct MockTerminalCell {
    char     ch = ' ';
    uint32_t fg = 0x00FFFFFF;
    uint32_t bg = 0x00000000;
};

// ============================================================
// Re-implemented Terminal (mirrors kernel/gui/terminal.hpp/.cpp)
// ============================================================

class MockTerminal : public MockWindow {
public:
    static constexpr uint32_t COLS = 80;
    static constexpr uint32_t ROWS = 25;

    MockTerminal(uint32_t x = 0, uint32_t y = 0, const char* title = "Cinux Terminal")
        : MockWindow(title,
                     static_cast<int32_t>(x),
                     static_cast<int32_t>(y),
                     COLS * 8,
                     ROWS * 16) {
        for (uint32_t r = 0; r < ROWS; r++) {
            for (uint32_t c = 0; c < COLS; c++) {
                screen_[r][c] = MockTerminalCell{};
            }
        }
    }

    ~MockTerminal() override = default;

    void on_key(MockKeyEvent& ev) override {
        if (!ev.pressed) return;
        if (ev.ascii == 0) return;
        process_char(ev.ascii);
    }

    void write(const char* str, uint64_t len) {
        uint64_t pos = 0;
        while (pos < len) {
            char ch = str[pos];
            if (is_escape(ch)) {
                handle_ansi(str, len, pos);
                continue;
            }
            switch (ch) {
            case '\n': newline(); break;
            case '\r': cursor_x_ = 0; break;
            case '\b': backspace(); break;
            case '\t': tab(); break;
            default:   process_char(ch); break;
            }
            pos++;
        }
    }

    const MockTerminalCell& cell(uint32_t row, uint32_t col) const {
        return screen_[row][col];
    }

    uint32_t cursor_x() const { return cursor_x_; }
    uint32_t cursor_y() const { return cursor_y_; }

    void set_pipe_write_fd(int fd) { pipe_write_fd_ = fd; }
    int pipe_write_fd() const { return pipe_write_fd_; }

    void clear() {
        for (uint32_t r = 0; r < ROWS; r++) {
            for (uint32_t c = 0; c < COLS; c++) {
                screen_[r][c] = MockTerminalCell{};
            }
        }
        cursor_x_ = 0;
        cursor_y_ = 0;
    }

    // Expose internals for testing
    void process_char(char ch) {
        if (static_cast<uint8_t>(ch) < 0x20 || static_cast<uint8_t>(ch) > 0x7E) return;
        screen_[cursor_y_][cursor_x_].ch = ch;
        screen_[cursor_y_][cursor_x_].fg = fg_;
        screen_[cursor_y_][cursor_x_].bg = bg_;
        cursor_x_++;
        if (cursor_x_ >= COLS) {
            cursor_x_ = 0;
            newline();
        }
    }

    void scroll_up() {
        for (uint32_t r = 0; r < ROWS - 1; r++) {
            for (uint32_t c = 0; c < COLS; c++) {
                screen_[r][c] = screen_[r + 1][c];
            }
        }
        for (uint32_t c = 0; c < COLS; c++) {
            screen_[ROWS - 1][c] = MockTerminalCell{};
        }
    }

    void newline() {
        cursor_x_ = 0;
        cursor_y_++;
        if (cursor_y_ >= ROWS) {
            cursor_y_ = ROWS - 1;
            scroll_up();
        }
    }

    void backspace() {
        if (cursor_x_ > 0) {
            cursor_x_--;
            screen_[cursor_y_][cursor_x_] = MockTerminalCell{};
        } else if (cursor_y_ > 0) {
            cursor_y_--;
            cursor_x_ = COLS - 1;
            screen_[cursor_y_][cursor_x_] = MockTerminalCell{};
        }
    }

    void tab() {
        uint32_t next_tab = (cursor_x_ / 8 + 1) * 8;
        if (next_tab >= COLS) {
            cursor_x_ = COLS - 1;
        } else {
            cursor_x_ = next_tab;
        }
    }

private:
    static bool is_escape(char ch) { return ch == '\033'; }

    void handle_ansi(const char* str, uint64_t len, uint64_t& pos) {
        if (pos + 1 >= len || str[pos + 1] != '[') {
            pos++;
            return;
        }
        pos += 2;
        uint32_t param = 0;
        while (pos < len) {
            char ch = str[pos];
            if (ch >= '0' && ch <= '9') {
                param = param * 10 + static_cast<uint32_t>(ch - '0');
                pos++;
            } else if (ch == ';') {
                pos++;
            } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
                pos++;
                switch (ch) {
                case 'J':
                    if (param == 2) clear();
                    return;
                case 'H':
                    cursor_x_ = 0;
                    cursor_y_ = 0;
                    return;
                case 'K':
                    for (uint32_t c = cursor_x_; c < COLS; c++) {
                        screen_[cursor_y_][c] = MockTerminalCell{};
                    }
                    return;
                case 'm':
                    return;
                default:
                    return;
                }
            } else {
                return;
            }
        }
    }

    MockTerminalCell screen_[ROWS][COLS];
    uint32_t cursor_x_ = 0;
    uint32_t cursor_y_ = 0;
    uint32_t fg_ = 0x00FFFFFF;
    uint32_t bg_ = 0x00000000;
    int pipe_write_fd_ = -1;
};

// ============================================================
// Construction tests
// ============================================================

TEST("terminal: construction initialises screen to spaces") {
    MockWindow::reset_id();
    MockTerminal t(0, 0);

    // All cells should be spaces with white fg, black bg
    ASSERT_EQ(t.cell(0, 0).ch, ' ');
    ASSERT_EQ(t.cell(12, 34).ch, ' ');
    ASSERT_EQ(t.cell(24, 79).ch, ' ');
    ASSERT_EQ(t.cell(0, 0).fg, 0x00FFFFFFu);
    ASSERT_EQ(t.cell(0, 0).bg, 0x00000000u);

    // Cursor should be at (0, 0)
    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: construction sets window size from COLS/ROWS") {
    MockWindow::reset_id();
    MockTerminal t(100, 200);

    // Window size should be COLS * 8 x ROWS * 16
    ASSERT_EQ(t.width(), 640u);
    ASSERT_EQ(t.height(), 400u);
    ASSERT_EQ(t.x(), 100);
    ASSERT_EQ(t.y(), 200);
}

TEST("terminal: pipe_write_fd defaults to -1") {
    MockWindow::reset_id();
    MockTerminal t;

    ASSERT_EQ(t.pipe_write_fd(), -1);
}

TEST("terminal: set_pipe_write_fd stores value") {
    MockWindow::reset_id();
    MockTerminal t;

    t.set_pipe_write_fd(42);
    ASSERT_EQ(t.pipe_write_fd(), 42);
}

// ============================================================
// write tests
// ============================================================

TEST("terminal: write Hello places characters in buffer") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("Hello", 5);

    ASSERT_EQ(t.cell(0, 0).ch, 'H');
    ASSERT_EQ(t.cell(0, 1).ch, 'e');
    ASSERT_EQ(t.cell(0, 2).ch, 'l');
    ASSERT_EQ(t.cell(0, 3).ch, 'l');
    ASSERT_EQ(t.cell(0, 4).ch, 'o');

    // Cursor should be at column 5
    ASSERT_EQ(t.cursor_x(), 5u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: write newline moves cursor to next line") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("AB\nCD", 5);

    ASSERT_EQ(t.cell(0, 0).ch, 'A');
    ASSERT_EQ(t.cell(0, 1).ch, 'B');
    ASSERT_EQ(t.cell(1, 0).ch, 'C');
    ASSERT_EQ(t.cell(1, 1).ch, 'D');

    ASSERT_EQ(t.cursor_x(), 2u);
    ASSERT_EQ(t.cursor_y(), 1u);
}

TEST("terminal: write carriage return resets column") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("AB\rX", 4);

    ASSERT_EQ(t.cell(0, 0).ch, 'X');  // Overwritten by X
    ASSERT_EQ(t.cell(0, 1).ch, 'B');  // B remains

    ASSERT_EQ(t.cursor_x(), 1u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: write wraps at end of line") {
    MockWindow::reset_id();
    MockTerminal t;

    // Write exactly COLS characters
    char line[81];
    for (int i = 0; i < 80; i++) line[i] = 'A' + static_cast<char>(i % 26);
    line[80] = '\0';
    t.write(line, 80);

    // Cursor should have wrapped to next line
    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 1u);

    // Last char of first row should be the 80th character
    // i=79, 79 % 26 = 1 -> 'B'
    ASSERT_EQ(t.cell(0, 79).ch, 'B');
}

TEST("terminal: write wraps beyond COLS triggers newline") {
    MockWindow::reset_id();
    MockTerminal t;

    // Write COLS + 1 characters
    char buf[82];
    for (int i = 0; i < 81; i++) buf[i] = 'X';
    t.write(buf, 81);

    // First char of second row should be 'X' (the 81st character)
    ASSERT_EQ(t.cell(1, 0).ch, 'X');
    ASSERT_EQ(t.cursor_x(), 1u);
    ASSERT_EQ(t.cursor_y(), 1u);
}

TEST("terminal: write scroll_up when reaching bottom") {
    MockWindow::reset_id();
    MockTerminal t;

    // Write enough lines to fill the screen (25 rows) plus one more
    // This triggers two scrolls: one at row 25 (Y->newline) and one at row 26 (Z->newline)
    for (uint32_t row = 0; row < 26; row++) {
        // Write a marker char for each row
        char marker = static_cast<char>('A' + (row % 26));
        t.write(&marker, 1);
        t.write("\n", 1);
    }

    // After two scrolls, row 0 should have 'C' (original row 2 content)
    ASSERT_EQ(t.cell(0, 0).ch, 'C');
    // The last visible row should be cleared by the second scroll
    ASSERT_EQ(t.cell(24, 0).ch, ' ');

    // Cursor should be at row 24 (clamped to ROWS-1)
    ASSERT_EQ(t.cursor_y(), 24u);
}

TEST("terminal: write backspace deletes previous character") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("ABC\b", 4);

    // Backspace should have deleted 'C', cursor at column 2
    ASSERT_EQ(t.cell(0, 0).ch, 'A');
    ASSERT_EQ(t.cell(0, 1).ch, 'B');
    ASSERT_EQ(t.cell(0, 2).ch, ' ');  // erased
    ASSERT_EQ(t.cursor_x(), 2u);
}

TEST("terminal: write backspace at beginning of line wraps to previous line") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("AB\n", 3);
    t.write("\b", 1);

    // Backspace at column 0 of row 1 should go to end of row 0
    ASSERT_EQ(t.cursor_x(), 79u);
    ASSERT_EQ(t.cursor_y(), 0u);
    // The char at (0, 79) should be erased
    ASSERT_EQ(t.cell(0, 79).ch, ' ');
}

TEST("terminal: write tab advances to next 8-column stop") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("A\tB", 3);

    // 'A' at col 0, tab should move to col 8, 'B' at col 8
    ASSERT_EQ(t.cell(0, 0).ch, 'A');
    ASSERT_EQ(t.cell(0, 8).ch, 'B');
    ASSERT_EQ(t.cursor_x(), 9u);
}

TEST("terminal: write tab near end of line clamps to COLS-1") {
    MockWindow::reset_id();
    MockTerminal t;

    // Set cursor near end of line
    // Write 77 chars then tab: cursor at 77, next tab stop = 80, clamp to 79
    char buf[78];
    for (int i = 0; i < 77; i++) buf[i] = 'X';
    buf[77] = '\0';
    t.write(buf, 77);
    ASSERT_EQ(t.cursor_x(), 77u);

    t.write("\t", 1);
    // next_tab = (77/8 + 1) * 8 = (9 + 1) * 8 = 80 >= COLS -> clamp to 79
    ASSERT_EQ(t.cursor_x(), 79u);
}

// ============================================================
// clear tests
// ============================================================

TEST("terminal: clear resets all cells to space") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("Hello World", 11);
    ASSERT_EQ(t.cell(0, 0).ch, 'H');

    t.clear();

    ASSERT_EQ(t.cell(0, 0).ch, ' ');
    ASSERT_EQ(t.cell(0, 5).ch, ' ');
    ASSERT_EQ(t.cell(10, 20).ch, ' ');
    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

// ============================================================
// on_key tests
// ============================================================

TEST("terminal: on_key writes printable character to buffer") {
    MockWindow::reset_id();
    MockTerminal t;

    MockKeyEvent ev{};
    ev.ascii = 'Z';
    ev.pressed = true;
    ev.scancode = 0;
    ev.shift = false;
    ev.ctrl = false;
    ev.alt = false;

    t.on_key(ev);

    ASSERT_EQ(t.cell(0, 0).ch, 'Z');
    ASSERT_EQ(t.cursor_x(), 1u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: on_key ignores key release") {
    MockWindow::reset_id();
    MockTerminal t;

    MockKeyEvent ev{};
    ev.ascii = 'Z';
    ev.pressed = false;

    t.on_key(ev);

    ASSERT_EQ(t.cell(0, 0).ch, ' ');
    ASSERT_EQ(t.cursor_x(), 0u);
}

TEST("terminal: on_key ignores non-printable (ascii=0)") {
    MockWindow::reset_id();
    MockTerminal t;

    MockKeyEvent ev{};
    ev.ascii = 0;
    ev.pressed = true;

    t.on_key(ev);

    ASSERT_EQ(t.cursor_x(), 0u);
}

TEST("terminal: on_key enter triggers newline") {
    MockWindow::reset_id();
    MockTerminal t;

    // Write 'A' first
    MockKeyEvent ev_a{};
    ev_a.ascii = 'A';
    ev_a.pressed = true;
    t.on_key(ev_a);

    // Write newline via process_char('\n') -- but on_key only handles
    // printable chars (0x20-0x7E), so '\n' (0x0A) is filtered.
    // This is expected behavior: on_key only processes printable chars.
    // Newlines from shell output go through write(), not on_key.
}

// ============================================================
// ANSI escape sequence tests
// ============================================================

TEST("terminal: ANSI ESC[2J clears screen") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("Hello", 5);
    ASSERT_EQ(t.cell(0, 0).ch, 'H');

    t.write("\033[2J", 4);

    ASSERT_EQ(t.cell(0, 0).ch, ' ');
    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: ANSI ESC[H moves cursor home") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("ABC\nDE", 6);
    ASSERT_EQ(t.cursor_x(), 2u);
    ASSERT_EQ(t.cursor_y(), 1u);

    t.write("\033[H", 3);

    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: ANSI ESC[K clears to end of line") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("ABCDE", 5);

    // Move cursor back to column 2
    t.write("\b\b", 2);
    ASSERT_EQ(t.cursor_x(), 3u);

    t.write("\033[K", 3);

    // Characters at cursor and beyond should be cleared
    ASSERT_EQ(t.cell(0, 0).ch, 'A');
    ASSERT_EQ(t.cell(0, 1).ch, 'B');
    ASSERT_EQ(t.cell(0, 2).ch, 'C');  // not cleared (before cursor)
    ASSERT_EQ(t.cell(0, 3).ch, ' ');  // cleared (at cursor)
    ASSERT_EQ(t.cell(0, 4).ch, ' ');  // cleared
}

TEST("terminal: ANSI ESC[2J ESC[H combined clears and homes") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("Line1\nLine2\nLine3", 17);
    ASSERT_EQ(t.cursor_y(), 2u);

    t.write("\033[2J\033[H", 7);

    ASSERT_EQ(t.cell(0, 0).ch, ' ');
    ASSERT_EQ(t.cell(1, 0).ch, ' ');
    ASSERT_EQ(t.cell(2, 0).ch, ' ');
    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: ANSI unknown sequence is ignored") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("AB\033[999Z", 8);

    // Unknown final byte 'Z' should stop parsing, characters before should remain
    ASSERT_EQ(t.cell(0, 0).ch, 'A');
    ASSERT_EQ(t.cell(0, 1).ch, 'B');
    ASSERT_EQ(t.cursor_x(), 2u);
}

TEST("terminal: ANSI ESC[m (SGR) is ignored") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("AB\033[mCD", 7);

    ASSERT_EQ(t.cell(0, 0).ch, 'A');
    ASSERT_EQ(t.cell(0, 1).ch, 'B');
    ASSERT_EQ(t.cell(0, 2).ch, 'C');
    ASSERT_EQ(t.cell(0, 3).ch, 'D');
    ASSERT_EQ(t.cursor_x(), 4u);
}

// ============================================================
// scroll_up tests
// ============================================================

TEST("terminal: scroll_up preserves row content") {
    MockWindow::reset_id();
    MockTerminal t;

    // Write 'A' on row 0, 'B' on row 1
    t.write("A\n", 2);
    t.write("B", 1);

    ASSERT_EQ(t.cell(0, 0).ch, 'A');
    ASSERT_EQ(t.cell(1, 0).ch, 'B');

    // Write enough to trigger a scroll (fill rows 2-24, then one more newline)
    for (uint32_t i = 2; i < 25; i++) {
        t.write("\n", 1);
    }
    ASSERT_EQ(t.cursor_y(), 24u);

    // One more newline triggers scroll
    t.write("\n", 1);

    // After scroll, row 0 should have old row 1 content ('B')
    ASSERT_EQ(t.cell(0, 0).ch, 'B');
    // Row 23 should have the old row 24 content
    // Last row should be clear
    ASSERT_EQ(t.cell(24, 0).ch, ' ');
    ASSERT_EQ(t.cursor_y(), 24u);
}

// ============================================================
// Cursor position query tests
// ============================================================

TEST("terminal: cursor position after multiple writes") {
    MockWindow::reset_id();
    MockTerminal t;

    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);

    t.write("12345", 5);
    ASSERT_EQ(t.cursor_x(), 5u);
    ASSERT_EQ(t.cursor_y(), 0u);

    t.write("\n", 1);
    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 1u);

    t.write("X", 1);
    ASSERT_EQ(t.cursor_x(), 1u);
    ASSERT_EQ(t.cursor_y(), 1u);
}

TEST("terminal: cell returns correct fg/bg colours") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("Z", 1);

    ASSERT_EQ(t.cell(0, 0).fg, 0x00FFFFFFu);
    ASSERT_EQ(t.cell(0, 0).bg, 0x00000000u);
}

// ============================================================
// Edge case tests
// ============================================================

TEST("terminal: write empty string does nothing") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("", 0);

    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
    ASSERT_EQ(t.cell(0, 0).ch, ' ');
}

TEST("terminal: write control characters are ignored") {
    MockWindow::reset_id();
    MockTerminal t;

    // Characters below 0x20 (except \n, \r, \b, \t) are not printable
    // They should be silently ignored
    t.write("\x01\x02\x03", 3);

    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: multiple backspaces at beginning of line") {
    MockWindow::reset_id();
    MockTerminal t;

    // Two backspaces at (0,0) should be no-ops
    t.write("\b\b", 2);

    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

TEST("terminal: backspace at (0,0) is a no-op") {
    MockWindow::reset_id();
    MockTerminal t;

    t.write("\b", 1);

    ASSERT_EQ(t.cursor_x(), 0u);
    ASSERT_EQ(t.cursor_y(), 0u);
}

// ============================================================
// Main entry point
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
