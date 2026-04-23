/**
 * @file kernel/drivers/canvas.cpp
 * @brief Double-buffered canvas implementation
 */

#include "canvas.hpp"

#include <cstdint>

#include "drivers/video/font.hpp"
#include "drivers/video/framebuffer.hpp"

namespace cinux::drivers {

// ============================================================
// Internal helpers
// ============================================================

namespace {

/**
 * @brief Copy bytes from source to a potentially volatile destination
 *
 * Used instead of libc memcpy since the kernel is freestanding.
 * Accepts volatile destination to support writing to MMIO framebuffers.
 */
void memcopy(volatile void* dst, const void* src, uint32_t size) {
    auto*       d = static_cast<volatile uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (uint32_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

/**
 * @brief Fill a memory region with a repeated 32-bit value
 *
 * @param dst    Destination pointer
 * @param value  32-bit fill pattern
 * @param count  Number of 32-bit words to write
 */
void memfill32(uint32_t* dst, uint32_t value, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = value;
    }
}

}  // anonymous namespace

// ============================================================
// Public interface
// ============================================================

void Canvas::init(Framebuffer& fb) {
    front_buf_ = &fb;
    width_     = fb.width();
    height_    = fb.height();
    pitch_     = fb.pitch();

    // Allocate back buffer: width * height * 4 bytes (32-bit pixels)
    uint32_t total_pixels = width_ * height_;
    back_buf_ = new uint32_t[total_pixels];

    // Clear back buffer to black
    memfill32(back_buf_, 0, total_pixels);
}

void Canvas::init(uint32_t w, uint32_t h) {
    // Free any previously allocated back buffer
    if (back_buf_ != nullptr) {
        delete[] back_buf_;
        back_buf_ = nullptr;
    }

    front_buf_ = nullptr;
    width_     = w;
    height_    = h;
    pitch_     = w * 4;  // 4 bytes per pixel, no alignment padding

    // Allocate back buffer
    uint32_t total_pixels = width_ * height_;
    back_buf_ = new uint32_t[total_pixels];

    // Clear to black
    memfill32(back_buf_, 0, total_pixels);
}

void Canvas::draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width_ || y >= height_)
        return;

    // Compute index using pitch (same formula as Framebuffer)
    uint32_t pixels_per_row = pitch_ / 4;
    back_buf_[y * pixels_per_row + x] = color;
}

void Canvas::draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t pixels_per_row = pitch_ / 4;

    for (uint32_t row = y; row < y + h && row < height_; row++) {
        for (uint32_t col = x; col < x + w && col < width_; col++) {
            back_buf_[row * pixels_per_row + col] = color;
        }
    }
}

void Canvas::draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    // Top edge
    for (uint32_t col = x; col < x + w && col < width_; col++) {
        draw_pixel(col, y, color);
    }

    // Bottom edge
    if (y + h > 0) {
        uint32_t bottom = y + h - 1;
        if (bottom < height_) {
            for (uint32_t col = x; col < x + w && col < width_; col++) {
                draw_pixel(col, bottom, color);
            }
        }
    }

    // Left edge
    for (uint32_t row = y; row < y + h && row < height_; row++) {
        draw_pixel(x, row, color);
    }

    // Right edge
    if (x + w > 0) {
        uint32_t right = x + w - 1;
        if (right < width_) {
            for (uint32_t row = y; row < y + h && row < height_; row++) {
                draw_pixel(right, row, color);
            }
        }
    }
}

void Canvas::flip() {
    if (front_buf_ == nullptr || back_buf_ == nullptr)
        return;

    // Copy back buffer to front buffer row by row (respecting pitch)
    auto* dst = reinterpret_cast<volatile uint8_t*>(front_buf_->data());
    auto* src = reinterpret_cast<const uint8_t*>(back_buf_);

    for (uint32_t row = 0; row < height_; row++) {
        memcopy(dst + static_cast<uintptr_t>(row) * pitch_,
                src + static_cast<uintptr_t>(row) * pitch_,
                width_ * 4);
    }
}

void Canvas::clear(uint32_t color) {
    if (back_buf_ == nullptr)
        return;

    uint32_t total_pixels = width_ * height_;
    memfill32(back_buf_, color, total_pixels);
}

void Canvas::draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                       uint32_t color) {
    if (back_buf_ == nullptr)
        return;

    // Bresenham's line algorithm (handles all octants)
    int32_t dx = static_cast<int32_t>(x1) - static_cast<int32_t>(x0);
    int32_t dy = static_cast<int32_t>(y1) - static_cast<int32_t>(y0);

    int32_t step_x = (dx >= 0) ? 1 : -1;
    int32_t step_y = (dy >= 0) ? 1 : -1;

    dx = (dx >= 0) ? dx : -dx;
    dy = (dy >= 0) ? dy : -dy;

    int32_t err = dx - dy;

    uint32_t cx = x0;
    uint32_t cy = y0;

    while (true) {
        draw_pixel(cx, cy, color);

        if (cx == x1 && cy == y1)
            break;

        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            cx = static_cast<uint32_t>(static_cast<int32_t>(cx) + step_x);
        }
        if (e2 < dx) {
            err += dx;
            cy = static_cast<uint32_t>(static_cast<int32_t>(cy) + step_y);
        }
    }
}

void Canvas::draw_text(uint32_t x, uint32_t y, const char* str, uint32_t color,
                       PSFFont& font) {
    if (back_buf_ == nullptr || str == nullptr)
        return;

    uint32_t glyph_w = font.width();
    uint32_t glyph_h = font.height();

    if (glyph_w == 0 || glyph_h == 0)
        return;

    uint32_t cursor_x = x;
    uint32_t cursor_y = y;

    for (uint32_t i = 0; str[i] != '\0'; i++) {
        // Handle newline
        if (str[i] == '\n') {
            cursor_x = x;
            cursor_y += glyph_h;
            continue;
        }

        const uint8_t* g = font.glyph(static_cast<uint8_t>(str[i]));
        if (g == nullptr)
            continue;

        // Render glyph to back buffer
        for (uint32_t row = 0; row < glyph_h; row++) {
            uint8_t bits = g[row];
            for (uint32_t col = 0; col < glyph_w; col++) {
                if ((bits >> (7 - col)) & 1) {
                    draw_pixel(cursor_x + col, cursor_y + row, color);
                }
            }
        }

        cursor_x += glyph_w;
    }
}

void Canvas::blit(uint32_t dst_x, uint32_t dst_y, Canvas& src,
                  uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
    if (back_buf_ == nullptr || src.back_buf_ == nullptr)
        return;

    uint32_t dst_pixels_per_row = pitch_ / 4;
    uint32_t src_pixels_per_row = src.pitch_ / 4;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t src_row = sy + row;
        uint32_t dst_row = dst_y + row;

        if (src_row >= src.height_ || dst_row >= height_)
            break;

        for (uint32_t col = 0; col < w; col++) {
            uint32_t src_col = sx + col;
            uint32_t dst_col = dst_x + col;

            if (src_col >= src.width_ || dst_col >= width_)
                break;

            back_buf_[dst_row * dst_pixels_per_row + dst_col] =
                src.back_buf_[src_row * src_pixels_per_row + src_col];
        }
    }
}

}  // namespace cinux::drivers
