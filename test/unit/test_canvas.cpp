/**
 * @file test/unit/test_canvas.cpp
 * @brief Host-side unit tests for Canvas double-buffered drawing
 *
 * Tests draw_pixel, draw_rect, draw_rect_outline, clear, and flip
 * by re-implementing the Canvas algorithms and verifying pixel output.
 * This follows the same pure-arithmetic test pattern as test_framebuffer.cpp.
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
// Mock Framebuffer (mimics kernel/drivers/video/framebuffer.hpp)
// ============================================================

class MockFramebuffer {
public:
    void init(uint32_t w, uint32_t h, uint32_t p) {
        width_  = w;
        height_ = h;
        pitch_  = p;
        buf_.resize(static_cast<size_t>(p) * h / 4, 0);
    }

    uint32_t get_pixel(uint32_t x, uint32_t y) const {
        if (x >= width_ || y >= height_) return 0;
        return buf_[y * (pitch_ / 4) + x];
    }

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pitch()  const { return pitch_; }

    uint32_t* data() { return buf_.data(); }

private:
    std::vector<uint32_t> buf_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t pitch_  = 0;
};

// ============================================================
// Mock PSFFont (mimics kernel/drivers/video/font.hpp for host tests)
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
// Mock Canvas (mirrors kernel/drivers/canvas.hpp/.cpp logic)
// ============================================================

class MockCanvas {
public:
    void init(MockFramebuffer& fb) {
        front_buf_ = &fb;
        width_     = fb.width();
        height_    = fb.height();
        pitch_     = fb.pitch();

        uint32_t total_pixels = width_ * height_;
        back_buf_.resize(total_pixels, 0);
    }

    void draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= width_ || y >= height_) return;
        uint32_t pixels_per_row = pitch_ / 4;
        back_buf_[y * pixels_per_row + x] = color;
    }

    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
        uint32_t pixels_per_row = pitch_ / 4;
        for (uint32_t row = y; row < y + h && row < height_; row++) {
            for (uint32_t col = x; col < x + w && col < width_; col++) {
                back_buf_[row * pixels_per_row + col] = color;
            }
        }
    }

    void draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
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

    void flip() {
        if (front_buf_ == nullptr) return;
        auto* dst = front_buf_->data();
        for (uint32_t row = 0; row < height_; row++) {
            uint32_t row_offset = row * (pitch_ / 4);
            for (uint32_t col = 0; col < width_; col++) {
                dst[row_offset + col] = back_buf_[row_offset + col];
            }
        }
    }

    void clear(uint32_t color = 0) {
        for (auto& px : back_buf_) {
            px = color;
        }
    }

    void draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                   uint32_t color) {
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

    void draw_text(uint32_t x, uint32_t y, const char* str, uint32_t color,
                   MockPSFFont& font) {
        if (str == nullptr) return;

        uint32_t glyph_w = font.width();
        uint32_t glyph_h = font.height();

        if (glyph_w == 0 || glyph_h == 0) return;

        uint32_t cursor_x = x;
        uint32_t cursor_y = y;

        for (uint32_t i = 0; str[i] != '\0'; i++) {
            if (str[i] == '\n') {
                cursor_x = x;
                cursor_y += glyph_h;
                continue;
            }

            const uint8_t* g = font.glyph(static_cast<uint8_t>(str[i]));
            if (g == nullptr) continue;

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

    void blit(uint32_t dst_x, uint32_t dst_y, MockCanvas& src,
              uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
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

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pitch()  const { return pitch_; }

    // Expose back buffer for direct inspection
    uint32_t back_pixel(uint32_t x, uint32_t y) const {
        if (x >= width_ || y >= height_) return 0xDEAD;
        return back_buf_[y * (pitch_ / 4) + x];
    }

private:
    MockFramebuffer* front_buf_ = nullptr;
    std::vector<uint32_t> back_buf_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t pitch_  = 0;
};

// ============================================================
// Helpers
// ============================================================

static MockFramebuffer make_fb(uint32_t w = 4, uint32_t h = 3) {
    MockFramebuffer fb;
    fb.init(w, h, w * 4);
    return fb;
}

static MockCanvas make_canvas(MockFramebuffer& fb) {
    MockCanvas c;
    c.init(fb);
    return c;
}

// ============================================================
// draw_pixel tests
// ============================================================

TEST("canvas: draw_pixel sets back buffer pixel") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_pixel(2, 1, 0x00FF0000);

    ASSERT_EQ(canvas.back_pixel(2, 1), 0x00FF0000u);
    // Neighboring pixels remain 0
    ASSERT_EQ(canvas.back_pixel(1, 1), 0u);
    ASSERT_EQ(canvas.back_pixel(2, 0), 0u);
}

TEST("canvas: draw_pixel out of bounds is ignored") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_pixel(4, 0, 0x00FF0000);     // x out of range
    canvas.draw_pixel(0, 3, 0x00FF0000);     // y out of range
    canvas.draw_pixel(999, 999, 0x00FF0000); // both out of range

    // Front buffer still all zeros
    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(fb.get_pixel(x, y), 0u);
        }
    }
}

TEST("canvas: draw_pixel at origin") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_pixel(0, 0, 0x00ABCDEF);
    canvas.flip();

    ASSERT_EQ(fb.get_pixel(0, 0), 0x00ABCDEFu);
}

// ============================================================
// draw_rect tests
// ============================================================

TEST("canvas: draw_rect fills entire area") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_rect(1, 0, 2, 2, 0x00FF0000);
    canvas.flip();

    // Pixels (1,0), (2,0), (1,1), (2,1) should be red
    ASSERT_EQ(fb.get_pixel(1, 0), 0x00FF0000u);
    ASSERT_EQ(fb.get_pixel(2, 0), 0x00FF0000u);
    ASSERT_EQ(fb.get_pixel(1, 1), 0x00FF0000u);
    ASSERT_EQ(fb.get_pixel(2, 1), 0x00FF0000u);

    // Other pixels remain black
    ASSERT_EQ(fb.get_pixel(0, 0), 0u);
    ASSERT_EQ(fb.get_pixel(3, 0), 0u);
    ASSERT_EQ(fb.get_pixel(0, 2), 0u);
}

TEST("canvas: draw_rect clamped to canvas bounds") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    // Rect extends beyond canvas: (2,1) w=5 h=5 -> clamped to (2,1) w=2 h=2
    canvas.draw_rect(2, 1, 5, 5, 0x0000FF00);
    canvas.flip();

    ASSERT_EQ(fb.get_pixel(2, 1), 0x0000FF00u);
    ASSERT_EQ(fb.get_pixel(3, 1), 0x0000FF00u);
    ASSERT_EQ(fb.get_pixel(2, 2), 0x0000FF00u);
    ASSERT_EQ(fb.get_pixel(3, 2), 0x0000FF00u);
    // Out-of-bounds pixels not written
    ASSERT_EQ(fb.get_pixel(0, 0), 0u);
}

// ============================================================
// draw_rect_outline tests
// ============================================================

TEST("canvas: draw_rect_outline draws border only") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_rect_outline(1, 0, 2, 2, 0x00FFFFFF);
    canvas.flip();

    // For 2x2 outline, all 4 pixels are border (no interior)
    ASSERT_EQ(fb.get_pixel(1, 0), 0x00FFFFFFu);  // top-left
    ASSERT_EQ(fb.get_pixel(2, 0), 0x00FFFFFFu);  // top-right
    ASSERT_EQ(fb.get_pixel(1, 1), 0x00FFFFFFu);  // bottom-left
    ASSERT_EQ(fb.get_pixel(2, 1), 0x00FFFFFFu);  // bottom-right
}

TEST("canvas: draw_rect_outline 3x3 has hollow center") {
    MockFramebuffer fb = make_fb(5, 5);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_rect_outline(1, 1, 3, 3, 0x00FFFFFF);
    canvas.flip();

    // Border pixels
    ASSERT_EQ(fb.get_pixel(1, 1), 0x00FFFFFFu);  // top-left
    ASSERT_EQ(fb.get_pixel(2, 1), 0x00FFFFFFu);  // top-center
    ASSERT_EQ(fb.get_pixel(3, 1), 0x00FFFFFFu);  // top-right
    ASSERT_EQ(fb.get_pixel(1, 2), 0x00FFFFFFu);  // middle-left
    ASSERT_EQ(fb.get_pixel(3, 2), 0x00FFFFFFu);  // middle-right
    ASSERT_EQ(fb.get_pixel(1, 3), 0x00FFFFFFu);  // bottom-left
    ASSERT_EQ(fb.get_pixel(2, 3), 0x00FFFFFFu);  // bottom-center
    ASSERT_EQ(fb.get_pixel(3, 3), 0x00FFFFFFu);  // bottom-right

    // Center must be empty (black)
    ASSERT_EQ(fb.get_pixel(2, 2), 0u);
}

TEST("canvas: draw_rect_outline 1x1 is a single pixel") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_rect_outline(2, 1, 1, 1, 0x00FFFF00);
    canvas.flip();

    // 1x1 outline = single pixel at (2,1)
    ASSERT_EQ(fb.get_pixel(2, 1), 0x00FFFF00u);
    // Adjacent pixels untouched
    ASSERT_EQ(fb.get_pixel(1, 1), 0u);
    ASSERT_EQ(fb.get_pixel(3, 1), 0u);
}

// ============================================================
// clear tests
// ============================================================

TEST("canvas: clear fills back buffer with color") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_rect(0, 0, 4, 3, 0x00FF0000);
    canvas.clear(0x000000FF);
    canvas.flip();

    // All pixels should be blue
    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(fb.get_pixel(x, y), 0x000000FFu);
        }
    }
}

TEST("canvas: clear default is black") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_rect(0, 0, 4, 3, 0x00FFFFFF);
    canvas.clear();
    canvas.flip();

    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(fb.get_pixel(x, y), 0u);
        }
    }
}

// ============================================================
// flip tests
// ============================================================

TEST("canvas: flip copies back buffer to front buffer") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_pixel(0, 0, 0x00AA0000);
    canvas.draw_pixel(3, 2, 0x0000AA00);

    // Front buffer should still be empty before flip
    ASSERT_EQ(fb.get_pixel(0, 0), 0u);
    ASSERT_EQ(fb.get_pixel(3, 2), 0u);

    canvas.flip();

    ASSERT_EQ(fb.get_pixel(0, 0), 0x00AA0000u);
    ASSERT_EQ(fb.get_pixel(3, 2), 0x0000AA00u);
}

TEST("canvas: flip overwrites front buffer completely") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    // First frame: draw red
    canvas.draw_rect(0, 0, 4, 3, 0x00FF0000);
    canvas.flip();
    ASSERT_EQ(fb.get_pixel(0, 0), 0x00FF0000u);

    // Second frame: draw green (overwrite entire back buffer)
    canvas.draw_rect(0, 0, 4, 3, 0x0000FF00);
    canvas.flip();
    ASSERT_EQ(fb.get_pixel(0, 0), 0x0000FF00u);
    ASSERT_EQ(fb.get_pixel(3, 2), 0x0000FF00u);
}

TEST("canvas: flip does not affect unmodified back buffer pixels") {
    MockFramebuffer fb = make_fb(4, 3);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_pixel(0, 0, 0x00FF0000);
    canvas.flip();

    ASSERT_EQ(fb.get_pixel(0, 0), 0x00FF0000u);
    ASSERT_EQ(fb.get_pixel(1, 0), 0u);  // untouched
    ASSERT_EQ(fb.get_pixel(0, 1), 0u);  // untouched
}

// ============================================================
// Accessor tests
// ============================================================

TEST("canvas: width/height/pitch match framebuffer") {
    MockFramebuffer fb;
    fb.init(1024, 768, 4096);
    MockCanvas canvas;
    canvas.init(fb);

    ASSERT_EQ(canvas.width(), 1024u);
    ASSERT_EQ(canvas.height(), 768u);
    ASSERT_EQ(canvas.pitch(), 4096u);
}

// ============================================================
// Pitch > width * 4 (padding) test
// ============================================================

TEST("canvas: respects pitch with padding") {
    // 4 pixels wide but pitch = 24 bytes (6 uint32 per row, 2 padding)
    MockFramebuffer fb;
    fb.init(4, 2, 24);
    MockCanvas canvas;
    canvas.init(fb);

    canvas.draw_pixel(3, 1, 0x00FF00FF);
    canvas.flip();

    // Pixel at (3, 1) with pitch 24: offset = 1 * (24/4) + 3 = 9
    ASSERT_EQ(fb.get_pixel(3, 1), 0x00FF00FFu);

    // Padding pixels (columns 4,5) should be zero
    ASSERT_EQ(fb.get_pixel(4, 0), 0u);
    ASSERT_EQ(fb.get_pixel(5, 0), 0u);
}

// ============================================================
// draw_line tests
// ============================================================

TEST("canvas: draw_line horizontal") {
    MockFramebuffer fb = make_fb(8, 4);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_line(1, 2, 5, 2, 0x00FF0000);

    // Horizontal line from (1,2) to (5,2)
    for (uint32_t x = 1; x <= 5; x++) {
        ASSERT_EQ(canvas.back_pixel(x, 2), 0x00FF0000u);
    }
    // Pixels above and below should be untouched
    ASSERT_EQ(canvas.back_pixel(3, 1), 0u);
    ASSERT_EQ(canvas.back_pixel(3, 3), 0u);
}

TEST("canvas: draw_line vertical") {
    MockFramebuffer fb = make_fb(8, 8);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_line(3, 1, 3, 6, 0x0000FF00);

    // Vertical line from (3,1) to (3,6)
    for (uint32_t y = 1; y <= 6; y++) {
        ASSERT_EQ(canvas.back_pixel(3, y), 0x0000FF00u);
    }
    // Adjacent columns untouched
    ASSERT_EQ(canvas.back_pixel(2, 3), 0u);
    ASSERT_EQ(canvas.back_pixel(4, 3), 0u);
}

TEST("canvas: draw_line diagonal 45 degrees") {
    MockFramebuffer fb = make_fb(8, 8);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_line(0, 0, 4, 4, 0x00FFFFFF);

    // Diagonal from (0,0) to (4,4)
    for (uint32_t i = 0; i <= 4; i++) {
        ASSERT_EQ(canvas.back_pixel(i, i), 0x00FFFFFFu);
    }
    // Off-diagonal pixels untouched
    ASSERT_EQ(canvas.back_pixel(1, 0), 0u);
    ASSERT_EQ(canvas.back_pixel(0, 1), 0u);
}

TEST("canvas: draw_line steep slope") {
    MockFramebuffer fb = make_fb(8, 8);
    MockCanvas canvas = make_canvas(fb);

    // Nearly vertical: dx=1, dy=5
    canvas.draw_line(2, 0, 3, 5, 0x00FFFF00);

    // Both endpoints must be set
    ASSERT_EQ(canvas.back_pixel(2, 0), 0x00FFFF00u);
    ASSERT_EQ(canvas.back_pixel(3, 5), 0x00FFFF00u);
    // No stray pixels outside expected range
    ASSERT_EQ(canvas.back_pixel(1, 0), 0u);
}

TEST("canvas: draw_line single point") {
    MockFramebuffer fb = make_fb(8, 8);
    MockCanvas canvas = make_canvas(fb);

    canvas.draw_line(3, 3, 3, 3, 0x00FF00FF);

    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FF00FFu);
    ASSERT_EQ(canvas.back_pixel(2, 3), 0u);
    ASSERT_EQ(canvas.back_pixel(3, 2), 0u);
}

// ============================================================
// draw_text tests
// ============================================================

static MockPSFFont make_test_font() {
    MockPSFFont font;
    // 3-pixel-wide, 3-pixel-tall font for testing
    font.init(3, 3);

    // Glyph for 'A': cross pattern (all bits set in middle row, left/right on top/bottom)
    // Row 0: .X. = 0b01000000 -> bits 0x40
    // Row 1: XXX = 0b11100000 -> bits 0xE0
    // Row 2: X.X = 0b10100000 -> bits 0xA0
    {
        uint8_t glyph_a[] = {0x40, 0xE0, 0xA0};
        font.set_glyph('A', glyph_a, 3);
    }

    // Glyph for 'B': full block
    // Row 0: XXX = 0xE0
    // Row 1: XXX = 0xE0
    // Row 2: XXX = 0xE0
    {
        uint8_t glyph_b[] = {0xE0, 0xE0, 0xE0};
        font.set_glyph('B', glyph_b, 3);
    }

    // Glyph for 'C': single pixel top-left
    // Row 0: X.. = 0x80
    // Row 1: ...
    // Row 2: ...
    {
        uint8_t glyph_c[] = {0x80, 0x00, 0x00};
        font.set_glyph('C', glyph_c, 3);
    }

    return font;
}

TEST("canvas: draw_text renders single character") {
    MockFramebuffer fb = make_fb(10, 10);
    MockCanvas canvas = make_canvas(fb);
    MockPSFFont font = make_test_font();

    canvas.draw_text(1, 1, "A", 0x00FFFFFF, font);

    // 'A' at (1,1): 3x3 glyph
    // Row 0: .X. -> (2,1)
    ASSERT_EQ(canvas.back_pixel(2, 1), 0x00FFFFFFu);
    ASSERT_EQ(canvas.back_pixel(1, 1), 0u);
    ASSERT_EQ(canvas.back_pixel(3, 1), 0u);

    // Row 1: XXX -> (1,2), (2,2), (3,2)
    ASSERT_EQ(canvas.back_pixel(1, 2), 0x00FFFFFFu);
    ASSERT_EQ(canvas.back_pixel(2, 2), 0x00FFFFFFu);
    ASSERT_EQ(canvas.back_pixel(3, 2), 0x00FFFFFFu);

    // Row 2: X.X -> (1,3), (3,3)
    ASSERT_EQ(canvas.back_pixel(1, 3), 0x00FFFFFFu);
    ASSERT_EQ(canvas.back_pixel(2, 3), 0u);
    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FFFFFFu);
}

TEST("canvas: draw_text renders two characters side by side") {
    MockFramebuffer fb = make_fb(16, 10);
    MockCanvas canvas = make_canvas(fb);
    MockPSFFont font = make_test_font();

    canvas.draw_text(0, 0, "AB", 0x00FF0000, font);

    // 'A' at (0,0): top row has pixel at (1,0)
    ASSERT_EQ(canvas.back_pixel(1, 0), 0x00FF0000u);

    // 'B' at (3,0) (glyph_w=3, so next char starts at x=3): full block
    // (3,0) should be set (first pixel of B top row)
    ASSERT_EQ(canvas.back_pixel(3, 0), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(4, 0), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(5, 0), 0x00FF0000u);
}

TEST("canvas: draw_text empty string renders nothing") {
    MockFramebuffer fb = make_fb(8, 8);
    MockCanvas canvas = make_canvas(fb);
    MockPSFFont font = make_test_font();

    canvas.draw_text(0, 0, "", 0x00FFFFFF, font);

    // All pixels should remain zero
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            ASSERT_EQ(canvas.back_pixel(x, y), 0u);
        }
    }
}

TEST("canvas: draw_text handles newline") {
    MockFramebuffer fb = make_fb(10, 10);
    MockCanvas canvas = make_canvas(fb);
    MockPSFFont font = make_test_font();

    // 'C' followed by newline then 'C' again
    canvas.draw_text(1, 1, "C\nC", 0x00FFFFFF, font);

    // First 'C' at (1,1): single pixel at (1,1)
    ASSERT_EQ(canvas.back_pixel(1, 1), 0x00FFFFFFu);

    // Second 'C' at (1,4) (cursor_x reset to 1, cursor_y = 1+3=4)
    ASSERT_EQ(canvas.back_pixel(1, 4), 0x00FFFFFFu);

    // No pixel at (1,2) (not a new glyph start)
    ASSERT_EQ(canvas.back_pixel(1, 2), 0u);
}

TEST("canvas: draw_text clips to canvas bounds") {
    MockFramebuffer fb = make_fb(4, 4);
    MockCanvas canvas = make_canvas(fb);
    MockPSFFont font = make_test_font();

    // Draw 'B' (full 3x3 block) at (2,2) — partially out of bounds
    canvas.draw_text(2, 2, "B", 0x00FF0000, font);

    // Pixels within bounds should be drawn
    ASSERT_EQ(canvas.back_pixel(2, 2), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(3, 2), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(2, 3), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FF0000u);

    // (4,2), (4,3) etc are out of bounds — not drawn (no crash)
}

// ============================================================
// blit tests
// ============================================================

TEST("canvas: blit copies region from source to destination") {
    MockFramebuffer fb1 = make_fb(8, 8);
    MockFramebuffer fb2 = make_fb(8, 8);
    MockCanvas src = make_canvas(fb1);
    MockCanvas dst = make_canvas(fb2);

    // Draw a 3x2 red rectangle on source at (1,1)
    src.draw_rect(1, 1, 3, 2, 0x00FF0000);

    // Blit from src(1,1) 3x2 to dst(4,3)
    dst.blit(4, 3, src, 1, 1, 3, 2);

    // Destination should have red pixels at (4,3)-(6,4)
    ASSERT_EQ(dst.back_pixel(4, 3), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(5, 3), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(6, 3), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(4, 4), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(5, 4), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(6, 4), 0x00FF0000u);

    // Surrounding pixels untouched
    ASSERT_EQ(dst.back_pixel(3, 3), 0u);
    ASSERT_EQ(dst.back_pixel(4, 2), 0u);
    ASSERT_EQ(dst.back_pixel(7, 3), 0u);
}

TEST("canvas: blit clamps to destination bounds") {
    MockFramebuffer fb1 = make_fb(4, 4);
    MockFramebuffer fb2 = make_fb(4, 4);
    MockCanvas src = make_canvas(fb1);
    MockCanvas dst = make_canvas(fb2);

    // Fill source entirely with green
    src.draw_rect(0, 0, 4, 4, 0x0000FF00);

    // Blit 4x4 from src(0,0) to dst(2,2) — partially out of bounds
    dst.blit(2, 2, src, 0, 0, 4, 4);

    // Only the 2x2 region that fits should be copied
    ASSERT_EQ(dst.back_pixel(2, 2), 0x0000FF00u);
    ASSERT_EQ(dst.back_pixel(3, 2), 0x0000FF00u);
    ASSERT_EQ(dst.back_pixel(2, 3), 0x0000FF00u);
    ASSERT_EQ(dst.back_pixel(3, 3), 0x0000FF00u);

    // Row 0 untouched
    ASSERT_EQ(dst.back_pixel(0, 0), 0u);
}

TEST("canvas: blit clamps to source bounds") {
    MockFramebuffer fb1 = make_fb(4, 4);
    MockFramebuffer fb2 = make_fb(8, 8);
    MockCanvas src = make_canvas(fb1);
    MockCanvas dst = make_canvas(fb2);

    // Draw red in top-left 2x2 of source
    src.draw_rect(0, 0, 2, 2, 0x00FF0000);

    // Blit 4x4 from src(0,0) to dst(0,0) — source only has 4x4, so full copy
    dst.blit(0, 0, src, 0, 0, 4, 4);

    ASSERT_EQ(dst.back_pixel(0, 0), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(1, 0), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(0, 1), 0x00FF0000u);
    ASSERT_EQ(dst.back_pixel(1, 1), 0x00FF0000u);
    // Rest of source was black (clear default)
    ASSERT_EQ(dst.back_pixel(2, 2), 0u);
}

TEST("canvas: blit zero size copies nothing") {
    MockFramebuffer fb1 = make_fb(4, 4);
    MockFramebuffer fb2 = make_fb(4, 4);
    MockCanvas src = make_canvas(fb1);
    MockCanvas dst = make_canvas(fb2);

    src.draw_rect(0, 0, 4, 4, 0x00FF0000);

    dst.blit(0, 0, src, 0, 0, 0, 3);

    // Nothing should be copied
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(dst.back_pixel(x, y), 0u);
        }
    }
}

// ============================================================
// Main entry point
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
