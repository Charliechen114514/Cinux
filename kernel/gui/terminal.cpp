/**
 * @file kernel/gui/terminal.cpp
 * @brief Terminal window implementation
 */

#include "terminal.hpp"

#include "kernel/drivers/video/font.hpp"
#include "kernel/ipc/pipe.hpp"

namespace cinux::gui {

// ============================================================
// Construction
// ============================================================

Terminal::Terminal(uint32_t x, uint32_t y, const char* title)
    : Window(title,
             static_cast<int32_t>(x),
             static_cast<int32_t>(y),
             COLS * 8,   // 80 chars * 8px per char (approximate)
             ROWS * 16)  // 25 rows * 16px per char (approximate)
{

    // Initialise all cells to default (space, white on black)
    for (uint32_t r = 0; r < ROWS; r++) {
        for (uint32_t c = 0; c < COLS; c++) {
            screen_[r][c] = TerminalCell{};
        }
    }
}

// ============================================================
// Destruction
// ============================================================

Terminal::~Terminal() {
    // Close stdin pipe write end: shell's read() will return 0 (EOF)
    // This signals to the shell that the terminal is gone.
    if (stdin_pipe_ != nullptr) {
        stdin_pipe_->close_writer();
        stdin_pipe_ = nullptr;
    }

    // Close stdout pipe read end: shell's write() will return -1
    // This prevents the shell from blocking on output after the
    // terminal has been destroyed.
    if (stdout_pipe_ != nullptr) {
        stdout_pipe_->close_reader();
        stdout_pipe_ = nullptr;
    }
}

// ============================================================
// Window virtual overrides
// ============================================================

void Terminal::on_key(KeyEvent& ev) {
    // Only handle key press events, not releases
    if (!ev.pressed) {
        return;
    }

    // Only process printable characters
    if (ev.ascii == 0) {
        return;
    }

    // When a stdin pipe is connected, forward the character to the pipe
    // instead of writing to the screen buffer (the shell echoes via stdout)
    if (stdin_pipe_ != nullptr) {
        char ch = ev.ascii;
        // Convert CR to LF for the shell
        if (ch == '\r') {
            ch = '\n';
        }
        stdin_pipe_->try_write(&ch, 1);
        return;
    }

    // No pipe connected -- write directly to the screen buffer
    process_char(ev.ascii);
}

void Terminal::on_paint(cinux::drivers::Canvas& /*canvas*/) {
    if (font_ != nullptr) {
        render_to_canvas();
    }
}

// ============================================================
// External write interface
// ============================================================

void Terminal::write(const char* str, uint64_t len) {
    uint64_t pos = 0;

    while (pos < len) {
        char ch = str[pos];

        // Check for ANSI escape sequence
        if (is_escape(ch)) {
            handle_ansi(str, len, pos);
            continue;
        }

        // Process the character
        switch (ch) {
        case '\n':
            newline();
            break;
        case '\r':
            cursor_x_ = 0;
            break;
        case '\b':
            backspace();
            break;
        case '\t':
            tab();
            break;
        default:
            process_char(ch);
            break;
        }

        pos++;
    }
}

// ============================================================
// Query
// ============================================================

const TerminalCell& Terminal::cell(uint32_t row, uint32_t col) const {
    return screen_[row][col];
}

uint32_t Terminal::cursor_x() const {
    return cursor_x_;
}

uint32_t Terminal::cursor_y() const {
    return cursor_y_;
}

// ============================================================
// Pipe fd
// ============================================================

void Terminal::set_pipe_write_fd(int fd) {
    pipe_write_fd_ = fd;
}

int Terminal::pipe_write_fd() const {
    return pipe_write_fd_;
}

// ============================================================
// Stdout pipe polling
// ============================================================

void Terminal::set_stdin_pipe(ipc::Pipe* pipe) {
    stdin_pipe_ = pipe;
}

void Terminal::set_stdout_pipe(ipc::Pipe* pipe) {
    stdout_pipe_ = pipe;
}

void Terminal::poll_output() {
    if (stdout_pipe_ == nullptr) {
        return;
    }

    // Read available data in chunks from the stdout pipe
    char buf[256];
    while (true) {
        int64_t n = stdout_pipe_->try_read(buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        write(buf, static_cast<uint64_t>(n));
    }
}

// ============================================================
// Clear
// ============================================================

void Terminal::clear() {
    for (uint32_t r = 0; r < ROWS; r++) {
        for (uint32_t c = 0; c < COLS; c++) {
            screen_[r][c] = TerminalCell{};
        }
    }
    cursor_x_ = 0;
    cursor_y_ = 0;
}

// ============================================================
// Internal helpers
// ============================================================

void Terminal::process_char(char ch) {
    // Only process printable ASCII characters
    if (static_cast<uint8_t>(ch) < 0x20 || static_cast<uint8_t>(ch) > 0x7E) {
        return;
    }

    // Write character at current cursor position
    screen_[cursor_y_][cursor_x_].ch = ch;
    screen_[cursor_y_][cursor_x_].fg = fg_;
    screen_[cursor_y_][cursor_x_].bg = bg_;

    // Advance cursor
    cursor_x_++;

    // Wrap at end of line
    if (cursor_x_ >= COLS) {
        cursor_x_ = 0;
        newline();
    }
}

void Terminal::scroll_up() {
    // Move rows 1..ROWS-1 up by one row
    for (uint32_t r = 0; r < ROWS - 1; r++) {
        for (uint32_t c = 0; c < COLS; c++) {
            screen_[r][c] = screen_[r + 1][c];
        }
    }

    // Clear the last row
    for (uint32_t c = 0; c < COLS; c++) {
        screen_[ROWS - 1][c] = TerminalCell{};
    }
}

void Terminal::newline() {
    cursor_x_ = 0;
    cursor_y_++;

    // Scroll if at the bottom
    if (cursor_y_ >= ROWS) {
        cursor_y_ = ROWS - 1;
        scroll_up();
    }
}

void Terminal::backspace() {
    if (cursor_x_ > 0) {
        cursor_x_--;
        screen_[cursor_y_][cursor_x_] = TerminalCell{};
    } else if (cursor_y_ > 0) {
        // Move to end of previous line
        cursor_y_--;
        cursor_x_ = COLS - 1;
        screen_[cursor_y_][cursor_x_] = TerminalCell{};
    }
}

void Terminal::tab() {
    // Advance to next 8-column tab stop
    uint32_t next_tab = (cursor_x_ / 8 + 1) * 8;
    if (next_tab >= COLS) {
        cursor_x_ = COLS - 1;
    } else {
        cursor_x_ = next_tab;
    }
}

// ============================================================
// ANSI escape sequence handling
// ============================================================

bool Terminal::is_escape(char ch) {
    return ch == '\033';
}

void Terminal::handle_ansi(const char* str, uint64_t len, uint64_t& pos) {
    // Expect ESC followed by '[' (CSI sequence)
    if (pos + 1 >= len || str[pos + 1] != '[') {
        // Not a CSI sequence, skip the ESC character
        pos++;
        return;
    }

    // Skip ESC and '['
    pos += 2;

    // Collect parameter bytes (digits and semicolons)
    // and the final byte (letter)
    uint32_t param = 0;

    while (pos < len) {
        char ch = str[pos];

        if (ch >= '0' && ch <= '9') {
            // Build numeric parameter
            param = param * 10 + static_cast<uint32_t>(ch - '0');
            pos++;
        } else if (ch == ';') {
            // Skip separator
            pos++;
        } else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            // Final byte -- dispatch
            pos++;

            switch (ch) {
            case 'J':
                // ESC[J or ESC[2J: clear screen
                if (param == 2) {
                    clear();
                }
                return;

            case 'H':
                // ESC[H: cursor home (row 1, col 1 in ANSI -> row 0, col 0 here)
                cursor_x_ = 0;
                cursor_y_ = 0;
                return;

            case 'K':
                // ESC[K: clear from cursor to end of line
                for (uint32_t c = cursor_x_; c < COLS; c++) {
                    screen_[cursor_y_][c] = TerminalCell{};
                }
                return;

            case 'm':
                // ESC[m: SGR (Select Graphic Rendition)
                // For now, just ignore (no colour support in basic terminal)
                return;

            default:
                // Unknown sequence, stop parsing
                return;
            }
        } else {
            // Unexpected character, stop parsing
            return;
        }
    }
}

// ============================================================
// Font / rendering
// ============================================================

void Terminal::set_font(cinux::drivers::PSFFont* font) {
    font_ = font;
}

void Terminal::render_to_canvas() {
    if (font_ == nullptr) {
        return;
    }

    auto& cvs = canvas();
    uint32_t gw = font_->width();
    uint32_t gh = font_->height();

    for (uint32_t row = 0; row < ROWS; row++) {
        for (uint32_t col = 0; col < COLS; col++) {
            const TerminalCell& cell = screen_[row][col];
            uint32_t px = col * gw;
            uint32_t py = TITLE_BAR_HEIGHT + row * gh;

            cvs.draw_rect(px, py, gw, gh, cell.bg);

            if (cell.ch > ' ') {
                const uint8_t* g = font_->glyph(static_cast<uint8_t>(cell.ch));
                if (g != nullptr) {
                    for (uint32_t gr = 0; gr < gh; gr++) {
                        uint8_t bits = g[gr];
                        for (uint32_t gc = 0; gc < gw; gc++) {
                            if ((bits >> (7 - gc)) & 1) {
                                cvs.draw_pixel(px + gc, py + gr, cell.fg);
                            }
                        }
                    }
                }
            }
        }
    }

    if (cursor_visible_) {
        uint32_t cx = cursor_x_ * gw;
        uint32_t cy = TITLE_BAR_HEIGHT + cursor_y_ * gh;
        const TerminalCell& cc = screen_[cursor_y_][cursor_x_];
        cvs.draw_rect(cx, cy, gw, gh, cc.fg);

        if (cc.ch > ' ') {
            const uint8_t* g = font_->glyph(static_cast<uint8_t>(cc.ch));
            if (g != nullptr) {
                for (uint32_t gr = 0; gr < gh; gr++) {
                    uint8_t bits = g[gr];
                    for (uint32_t gc = 0; gc < gw; gc++) {
                        if ((bits >> (7 - gc)) & 1) {
                            cvs.draw_pixel(cx + gc, cy + gr, cc.bg);
                        }
                    }
                }
            }
        }
    }
}

}  // namespace cinux::gui
