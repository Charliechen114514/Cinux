/**
 * @file test/unit/test_bitmap_icon.cpp
 * @brief Host-side unit tests for bitmap icon rendering and DesktopIcon hit testing
 *
 * Tests Canvas::draw_bitmap (pixel verification, transparency skip, out-of-bounds
 * clipping) and DesktopIcon::contains (boundary conditions, edge cases).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>
#    include <vector>

// Icon and desktop_icon headers (no C++20 dependency)
#    define CINUX_GUI
#    include "gui/desktop_icon.hpp"

// ============================================================
// Mock Canvas (mirrors kernel/drivers/canvas.hpp/.cpp draw_bitmap logic)
// ============================================================

class MockCanvas {
public:
    void init(uint32_t w, uint32_t h) {
        front_buf_ = nullptr;
        width_     = w;
        height_    = h;
        pitch_     = w * 4;
        back_buf_.resize(static_cast<size_t>(width_) * height_, 0);
    }

    // Mirrors kernel/drivers/canvas.cpp::draw_bitmap exactly
    void draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels) {
        if (back_buf_.data() == nullptr || pixels == nullptr)
            return;

        uint32_t pixels_per_row = pitch_ / 4;

        for (uint32_t row = 0; row < h; row++) {
            if (y + row >= height_)
                break;

            for (uint32_t col = 0; col < w; col++) {
                if (x + col >= width_)
                    break;

                uint32_t color = pixels[row * w + col];

                if (color == 0x00000000)
                    continue;

                back_buf_[(y + row) * pixels_per_row + (x + col)] = color;
            }
        }
    }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pitch() const { return pitch_; }

    uint32_t back_pixel(uint32_t x, uint32_t y) const {
        if (x >= width_ || y >= height_)
            return 0xDEAD;
        return back_buf_[y * (pitch_ / 4) + x];
    }

private:
    uint32_t*             front_buf_ = nullptr;
    std::vector<uint32_t> back_buf_;
    uint32_t              width_  = 0;
    uint32_t              height_ = 0;
    uint32_t              pitch_  = 0;
};

// ============================================================
// DesktopIcon struct (mirrors kernel/gui/desktop_icon.hpp)
// ============================================================

enum class IconAction : uint8_t {
    None           = 0,
    OpenShell      = 1,
    OpenCalculator = 2,
};

struct DesktopIcon {
    int32_t         x;
    int32_t         y;
    const uint32_t* bitmap;
    const char*     label;
    uint32_t        width;
    uint32_t        height;
    IconAction      action;

    [[nodiscard]] bool contains(int32_t mx, int32_t my) const {
        return mx >= x && mx < static_cast<int32_t>(x + width) && my >= y &&
               my < static_cast<int32_t>(y + height);
    }
};

// ============================================================
// Helpers
// ============================================================

static MockCanvas make_canvas(uint32_t w = 8, uint32_t h = 8) {
    MockCanvas c;
    c.init(w, h);
    return c;
}

// ============================================================
// draw_bitmap: normal pixel rendering
// ============================================================

TEST("bitmap: draw_bitmap renders opaque pixels correctly") {
    MockCanvas canvas = make_canvas(8, 8);

    // 2x2 bitmap, all opaque red
    uint32_t pixels[] = {0x00FF0000, 0x00FF0000, 0x00FF0000, 0x00FF0000};

    canvas.draw_bitmap(2, 2, 2, 2, pixels);

    ASSERT_EQ(canvas.back_pixel(2, 2), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(3, 2), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(2, 3), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FF0000u);

    // Surrounding pixels untouched
    ASSERT_EQ(canvas.back_pixel(1, 2), 0u);
    ASSERT_EQ(canvas.back_pixel(4, 2), 0u);
    ASSERT_EQ(canvas.back_pixel(2, 1), 0u);
}

TEST("bitmap: draw_bitmap renders multiple distinct colours") {
    MockCanvas canvas = make_canvas(8, 8);

    // 2x2 bitmap with 4 different colours
    uint32_t pixels[] = {0x00FF0000, 0x0000FF00,   // red, green
                         0x000000FF, 0x00FFFFFF};  // blue, white

    canvas.draw_bitmap(1, 1, 2, 2, pixels);

    ASSERT_EQ(canvas.back_pixel(1, 1), 0x00FF0000u);  // red
    ASSERT_EQ(canvas.back_pixel(2, 1), 0x0000FF00u);  // green
    ASSERT_EQ(canvas.back_pixel(1, 2), 0x000000FFu);  // blue
    ASSERT_EQ(canvas.back_pixel(2, 2), 0x00FFFFFFu);  // white
}

TEST("bitmap: draw_bitmap at origin (0,0)") {
    MockCanvas canvas = make_canvas(4, 4);

    uint32_t pixels[] = {0x00AABBCC, 0x00DDEEFF, 0x00112233, 0x00445566};

    canvas.draw_bitmap(0, 0, 2, 2, pixels);

    ASSERT_EQ(canvas.back_pixel(0, 0), 0x00AABBCCu);
    ASSERT_EQ(canvas.back_pixel(1, 0), 0x00DDEEFFu);
    ASSERT_EQ(canvas.back_pixel(0, 1), 0x00112233u);
    ASSERT_EQ(canvas.back_pixel(1, 1), 0x00445566u);
}

TEST("bitmap: draw_bitmap larger bitmap fills area") {
    MockCanvas canvas = make_canvas(10, 10);

    // 5x5 bitmap, all green
    uint32_t pixels[25];
    for (int i = 0; i < 25; i++)
        pixels[i] = 0x0000FF00;

    canvas.draw_bitmap(3, 3, 5, 5, pixels);

    // Verify all 25 pixels rendered
    for (uint32_t row = 3; row < 8; row++) {
        for (uint32_t col = 3; col < 8; col++) {
            ASSERT_EQ(canvas.back_pixel(col, row), 0x0000FF00u);
        }
    }

    // Border pixels untouched
    ASSERT_EQ(canvas.back_pixel(2, 3), 0u);
    ASSERT_EQ(canvas.back_pixel(8, 3), 0u);
    ASSERT_EQ(canvas.back_pixel(3, 2), 0u);
    ASSERT_EQ(canvas.back_pixel(3, 8), 0u);
}

// ============================================================
// draw_bitmap: transparency (0x00000000 skip)
// ============================================================

TEST("bitmap: draw_bitmap skips transparent pixels (0x00000000)") {
    MockCanvas canvas = make_canvas(8, 8);

    // Fill canvas with blue first using a proper pixel array
    uint32_t blue_pixels[64];
    for (int i = 0; i < 64; i++)
        blue_pixels[i] = 0x000000FF;
    canvas.draw_bitmap(0, 0, 8, 8, blue_pixels);

    // 2x2 bitmap: transparent, red, green, blue
    uint32_t pixels[] = {0x00000000, 0x00FF0000, 0x0000FF00, 0x000000FF};

    canvas.draw_bitmap(1, 1, 2, 2, pixels);

    // Transparent pixel (1,1) should retain underlying blue
    ASSERT_EQ(canvas.back_pixel(1, 1), 0x000000FFu);
    // Opaque pixels should overwrite
    ASSERT_EQ(canvas.back_pixel(2, 1), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(1, 2), 0x0000FF00u);
    ASSERT_EQ(canvas.back_pixel(2, 2), 0x000000FFu);
}

TEST("bitmap: draw_bitmap all transparent draws nothing") {
    MockCanvas canvas = make_canvas(4, 4);

    uint32_t pixels[] = {0x00000000, 0x00000000, 0x00000000, 0x00000000};

    canvas.draw_bitmap(0, 0, 2, 2, pixels);

    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(canvas.back_pixel(x, y), 0u);
        }
    }
}

TEST("bitmap: draw_bitmap checkerboard pattern") {
    MockCanvas canvas = make_canvas(8, 8);

    // 4x4 checkerboard: alternating red and transparent
    uint32_t pixels[16];
    for (int i = 0; i < 16; i++) {
        int row   = i / 4;
        int col   = i % 4;
        pixels[i] = ((row + col) % 2 == 0) ? 0x00FF0000 : 0x00000000;
    }

    canvas.draw_bitmap(0, 0, 4, 4, pixels);

    // Diagonal pattern: even-sum positions are red
    ASSERT_EQ(canvas.back_pixel(0, 0), 0x00FF0000u);  // (0+0) even
    ASSERT_EQ(canvas.back_pixel(1, 0), 0u);           // (1+0) odd
    ASSERT_EQ(canvas.back_pixel(0, 1), 0u);           // (0+1) odd
    ASSERT_EQ(canvas.back_pixel(1, 1), 0x00FF0000u);  // (1+1) even
    ASSERT_EQ(canvas.back_pixel(2, 2), 0x00FF0000u);  // (2+2) even
    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FF0000u);  // (3+3) even
    ASSERT_EQ(canvas.back_pixel(2, 3), 0u);           // (2+3) odd
}

// ============================================================
// draw_bitmap: out-of-bounds clipping
// ============================================================

TEST("bitmap: draw_bitmap clips right edge") {
    MockCanvas canvas = make_canvas(4, 4);

    // 4x1 bitmap, all red — but placed at x=2 so only 2 pixels fit
    uint32_t pixels[] = {0x00FF0000, 0x00FF0000, 0x00FF0000, 0x00FF0000};

    canvas.draw_bitmap(2, 0, 4, 1, pixels);

    ASSERT_EQ(canvas.back_pixel(2, 0), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(3, 0), 0x00FF0000u);
    // (4,0) is out of bounds — not written (no crash)
}

TEST("bitmap: draw_bitmap clips bottom edge") {
    MockCanvas canvas = make_canvas(4, 4);

    // 1x4 bitmap, all green — placed at y=3 so only 1 pixel fits
    uint32_t pixels[] = {0x0000FF00, 0x0000FF00, 0x0000FF00, 0x0000FF00};

    canvas.draw_bitmap(0, 3, 1, 4, pixels);

    ASSERT_EQ(canvas.back_pixel(0, 3), 0x0000FF00u);
    // y=4,5,6 are out of bounds — not written
}

TEST("bitmap: draw_bitmap clips both right and bottom") {
    MockCanvas canvas = make_canvas(4, 4);

    // 4x4 bitmap placed at (2,2) — only 2x2 fits
    uint32_t pixels[16];
    for (int i = 0; i < 16; i++)
        pixels[i] = 0x00FFFF00;

    canvas.draw_bitmap(2, 2, 4, 4, pixels);

    ASSERT_EQ(canvas.back_pixel(2, 2), 0x00FFFF00u);
    ASSERT_EQ(canvas.back_pixel(3, 2), 0x00FFFF00u);
    ASSERT_EQ(canvas.back_pixel(2, 3), 0x00FFFF00u);
    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FFFF00u);
    // Rest of canvas untouched
    ASSERT_EQ(canvas.back_pixel(0, 0), 0u);
    ASSERT_EQ(canvas.back_pixel(1, 1), 0u);
}

TEST("bitmap: draw_bitmap completely outside canvas is no-op") {
    MockCanvas canvas = make_canvas(4, 4);

    uint32_t pixels[] = {0x00FF0000, 0x00FF0000, 0x00FF0000, 0x00FF0000};

    // Bitmap starts entirely past the right edge
    canvas.draw_bitmap(10, 0, 2, 2, pixels);

    // Bitmap starts entirely below the bottom edge
    canvas.draw_bitmap(0, 10, 2, 2, pixels);

    // Entire canvas should remain zero
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(canvas.back_pixel(x, y), 0u);
        }
    }
}

TEST("bitmap: draw_bitmap 1x1 at exact edge boundary") {
    MockCanvas canvas = make_canvas(4, 4);

    uint32_t pixel = 0x00FF00FF;
    canvas.draw_bitmap(3, 3, 1, 1, &pixel);

    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FF00FFu);
    ASSERT_EQ(canvas.back_pixel(2, 3), 0u);
    ASSERT_EQ(canvas.back_pixel(3, 2), 0u);
}

// ============================================================
// draw_bitmap: null / error handling
// ============================================================

TEST("bitmap: draw_bitmap null pixels is no-op") {
    MockCanvas canvas = make_canvas(4, 4);

    // Pre-fill with red using a proper 4x4 pixel array
    uint32_t red_pixels[16];
    for (int i = 0; i < 16; i++)
        red_pixels[i] = 0x00FF0000;
    canvas.draw_bitmap(0, 0, 4, 4, red_pixels);

    canvas.draw_bitmap(0, 0, 2, 2, nullptr);

    // Canvas should be unchanged (all red from pre-fill)
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(canvas.back_pixel(x, y), 0x00FF0000u);
        }
    }
}

TEST("bitmap: draw_bitmap zero width is no-op") {
    MockCanvas canvas = make_canvas(4, 4);

    uint32_t pixels[] = {0x00FF0000};
    canvas.draw_bitmap(0, 0, 0, 1, pixels);

    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(canvas.back_pixel(x, y), 0u);
        }
    }
}

TEST("bitmap: draw_bitmap zero height is no-op") {
    MockCanvas canvas = make_canvas(4, 4);

    uint32_t pixels[] = {0x00FF0000};
    canvas.draw_bitmap(0, 0, 1, 0, pixels);

    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            ASSERT_EQ(canvas.back_pixel(x, y), 0u);
        }
    }
}

// ============================================================
// draw_bitmap: transparent does not overwrite existing content
// ============================================================

TEST("bitmap: draw_bitmap transparent preserves existing drawing") {
    MockCanvas canvas = make_canvas(4, 4);

    // Draw a red rectangle first using a proper pixel array
    uint32_t red_pixels[16];
    for (int i = 0; i < 16; i++)
        red_pixels[i] = 0x00FF0000;
    canvas.draw_bitmap(0, 0, 4, 4, red_pixels);

    // Overlay a 2x2 bitmap with one transparent pixel
    uint32_t pixels[] = {0x00000000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};

    canvas.draw_bitmap(1, 1, 2, 2, pixels);

    // (1,1) transparent -> preserves red underneath
    ASSERT_EQ(canvas.back_pixel(1, 1), 0x00FF0000u);
    // (2,1) green overwrites red
    ASSERT_EQ(canvas.back_pixel(2, 1), 0x0000FF00u);
    // (1,2) blue overwrites red
    ASSERT_EQ(canvas.back_pixel(1, 2), 0x000000FFu);
    // (2,2) white overwrites red
    ASSERT_EQ(canvas.back_pixel(2, 2), 0x00FFFFFFu);
    // Pixels outside the bitmap area remain red
    ASSERT_EQ(canvas.back_pixel(0, 0), 0x00FF0000u);
    ASSERT_EQ(canvas.back_pixel(3, 3), 0x00FF0000u);
}

// ============================================================
// DesktopIcon::contains: normal cases
// ============================================================

TEST("desktop_icon: contains returns true for point inside") {
    DesktopIcon icon{
        .x      = 10,
        .y      = 20,
        .bitmap = nullptr,
        .label  = "Test",
        .width  = 32,
        .height = 32,
        .action = IconAction::OpenShell,
    };

    ASSERT_TRUE(icon.contains(10, 20));  // top-left corner
    ASSERT_TRUE(icon.contains(20, 30));  // middle
    ASSERT_TRUE(icon.contains(41, 51));  // bottom-right - 1
}

TEST("desktop_icon: contains returns false for point outside") {
    DesktopIcon icon{
        .x      = 10,
        .y      = 20,
        .bitmap = nullptr,
        .label  = "Test",
        .width  = 32,
        .height = 32,
        .action = IconAction::OpenShell,
    };

    ASSERT_FALSE(icon.contains(9, 20));   // left of icon
    ASSERT_FALSE(icon.contains(10, 19));  // above icon
    ASSERT_FALSE(icon.contains(42, 51));  // right of icon (x + width)
    ASSERT_FALSE(icon.contains(41, 52));  // below icon (y + height)
}

// ============================================================
// DesktopIcon::contains: boundary conditions
// ============================================================

TEST("desktop_icon: contains boundary exact top-left corner is inside") {
    DesktopIcon icon{
        .x      = 0,
        .y      = 0,
        .bitmap = nullptr,
        .label  = "Test",
        .width  = 16,
        .height = 16,
        .action = IconAction::None,
    };

    ASSERT_TRUE(icon.contains(0, 0));
    ASSERT_TRUE(icon.contains(15, 15));  // last pixel inside
    ASSERT_FALSE(icon.contains(16, 0));  // one past right edge
    ASSERT_FALSE(icon.contains(0, 16));  // one past bottom edge
}

TEST("desktop_icon: contains at large coordinates") {
    DesktopIcon icon{
        .x      = 1000,
        .y      = 2000,
        .bitmap = nullptr,
        .label  = "Test",
        .width  = 32,
        .height = 32,
        .action = IconAction::OpenCalculator,
    };

    ASSERT_TRUE(icon.contains(1000, 2000));
    ASSERT_TRUE(icon.contains(1031, 2031));   // last pixel
    ASSERT_FALSE(icon.contains(1032, 2000));  // one past right
    ASSERT_FALSE(icon.contains(1000, 2032));  // one past bottom
}

TEST("desktop_icon: contains 1x1 icon") {
    DesktopIcon icon{
        .x      = 50,
        .y      = 50,
        .bitmap = nullptr,
        .label  = "Dot",
        .width  = 1,
        .height = 1,
        .action = IconAction::None,
    };

    ASSERT_TRUE(icon.contains(50, 50));
    ASSERT_FALSE(icon.contains(49, 50));
    ASSERT_FALSE(icon.contains(51, 50));
    ASSERT_FALSE(icon.contains(50, 49));
    ASSERT_FALSE(icon.contains(50, 51));
}

TEST("desktop_icon: contains negative icon position") {
    DesktopIcon icon{
        .x      = -10,
        .y      = -5,
        .bitmap = nullptr,
        .label  = "Offscreen",
        .width  = 32,
        .height = 32,
        .action = IconAction::None,
    };

    // Point (-10, -5) is the top-left corner — should be inside
    ASSERT_TRUE(icon.contains(-10, -5));
    ASSERT_TRUE(icon.contains(0, 0));     // well inside
    ASSERT_TRUE(icon.contains(21, 26));   // last pixel (x=-10+32-1=21, y=-5+32-1=26)
    ASSERT_FALSE(icon.contains(22, 26));  // one past right edge
    ASSERT_FALSE(icon.contains(21, 27));  // one past bottom edge
}

TEST("desktop_icon: contains with different IconAction values") {
    // Verify that IconAction values don't affect hit testing
    DesktopIcon icon1{
        .x      = 0,
        .y      = 0,
        .bitmap = nullptr,
        .label  = "None",
        .width  = 10,
        .height = 10,
        .action = IconAction::None,
    };
    DesktopIcon icon2{
        .x      = 0,
        .y      = 0,
        .bitmap = nullptr,
        .label  = "Shell",
        .width  = 10,
        .height = 10,
        .action = IconAction::OpenShell,
    };
    DesktopIcon icon3{
        .x      = 0,
        .y      = 0,
        .bitmap = nullptr,
        .label  = "Calc",
        .width  = 10,
        .height = 10,
        .action = IconAction::OpenCalculator,
    };

    ASSERT_TRUE(icon1.contains(5, 5));
    ASSERT_TRUE(icon2.contains(5, 5));
    ASSERT_TRUE(icon3.contains(5, 5));
    ASSERT_FALSE(icon1.contains(10, 10));
    ASSERT_FALSE(icon2.contains(10, 10));
    ASSERT_FALSE(icon3.contains(10, 10));
}

// ============================================================
// Main entry point
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
