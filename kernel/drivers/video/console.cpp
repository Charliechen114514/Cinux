/**
 * @file kernel/drivers/video/console.cpp
 * @brief Text console driver implementation
 */

#include "console.hpp"

#include <stdint.h>

namespace cinux::drivers {

void Console::init(Framebuffer& fb, PSFFont& font, uint32_t fg, uint32_t bg) {
    fb_   = &fb;
    font_ = &font;
    fg_   = fg;
    bg_   = bg;
    col_  = 0;
    row_  = 0;

    cols_ = fb.width() / font.width();
    rows_ = fb.height() / font.height();

    clear();
}

void Console::putc(char c) {
    if (fb_ == nullptr || font_ == nullptr)
        return;

    // ---- ANSI escape sequence state machine ----
    switch (ansi_state_) {
    case AnsiState::Normal:
        if (c == '\x1B') {
            ansi_state_ = AnsiState::Esc;
            return;
        }
        break;

    case AnsiState::Esc:
        if (c == '[') {
            ansi_state_ = AnsiState::Bracket;
            ansi_pos_   = 0;
            return;
        }
        // Not a CSI sequence -- discard ESC and process character normally
        ansi_state_ = AnsiState::Normal;
        break;

    case AnsiState::Bracket:
        // Collect parameter bytes (0x30-0x3F) and intermediate bytes (0x20-0x2F)
        if (ansi_pos_ < sizeof(ansi_params_) - 1 &&
            ((c >= 0x30 && c <= 0x3F) || (c >= 0x20 && c <= 0x2F))) {
            ansi_params_[ansi_pos_++] = c;
            return;
        }
        // Final byte (0x40-0x7E) dispatches the CSI command
        if (c >= 0x40 && c <= 0x7E) {
            ansi_params_[ansi_pos_] = '\0';
            handle_ansi_csi(c);
            ansi_state_ = AnsiState::Normal;
            return;
        }
        // Malformed sequence -- reset
        ansi_state_ = AnsiState::Normal;
        return;
    }

    // ---- Normal character processing ----
    switch (c) {
    case '\n':
        new_line();
        break;
    case '\r':
        col_ = 0;
        break;
    case '\b':
        if (col_ > 0) {
            col_--;
        } else if (row_ > 0) {
            row_--;
            col_ = cols_ - 1;
        }
        break;
    default:
        if (col_ >= cols_) {
            new_line();
        }
        font_->render_char(*fb_, static_cast<uint8_t>(c), col_ * font_->width(),
                           row_ * font_->height(), fg_, bg_);
        col_++;
        break;
    }
}

void Console::clear() {
    if (fb_)
        fb_->clear(bg_);
    col_ = 0;
    row_ = 0;
}

void Console::set_color(uint32_t fg, uint32_t bg) {
    fg_ = fg;
    bg_ = bg;
}

void Console::console_sink_adapter(char c, void* ctx) {
    auto* con = static_cast<Console*>(ctx);
    if (con)
        con->putc(c);
}

void Console::new_line() {
    col_ = 0;
    if (row_ + 1 >= rows_) {
        scroll();
    } else {
        row_++;
    }
}

void Console::scroll() {
    if (fb_ == nullptr || font_ == nullptr)
        return;
    uint32_t line_height = font_->height();
    fb_->scroll_up(line_height, line_height, bg_);
}

void Console::handle_ansi_csi(char final_byte) {
    // Supported CSI sequences (minimal set for cmd_clear):
    //   ESC[2J  -- erase entire screen
    //   ESC[H   -- cursor home (row 1, col 1)
    switch (final_byte) {
    case 'J': {
        // Erase in Display: parameter "2" = erase entire screen
        int param = 0;
        for (uint8_t i = 0; i < ansi_pos_; i++) {
            if (ansi_params_[i] >= '0' && ansi_params_[i] <= '9') {
                param = param * 10 + (ansi_params_[i] - '0');
            }
        }
        if (param == 2) {
            clear();
        }
        break;
    }
    case 'H': {
        // Cursor Position: move to (1,1) i.e. home
        col_ = 0;
        row_ = 0;
        break;
    }
    default:
        // Unrecognised CSI sequence -- ignore silently
        break;
    }
}

}  // namespace cinux::drivers
