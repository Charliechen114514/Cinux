/**
 * @file kernel/test/test_bitmap_icon.cpp
 * @brief QEMU in-kernel tests for bitmap icon rendering and DesktopIcon (032_gui_bitmap_icon)
 *
 * Tests draw_bitmap pixel verification, transparency skip, out-of-bounds
 * clipping, and DesktopIcon::contains hit testing.  Uses the real kernel
 * Canvas and Framebuffer for pixel verification.
 *
 * Note: icon_data.hpp uses consteval (C++20) which is not available in the
 * freestanding kernel build, so icon_data constant tests and real icon
 * rendering tests are covered in the host-side unit tests instead.
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Framebuffer available (set up by bootloader)
 *   - Heap initialised (for Canvas::init new[] allocation)
 *
 * Compile condition: CINUX_GUI
 */

#include <stdint.h>

#include "big_kernel_test.h"
#include "boot/boot_info.h"
#include "kernel/drivers/video/framebuffer.hpp"

#ifdef CINUX_GUI

#    include "kernel/drivers/canvas.hpp"
#    include "kernel/gui/desktop_icon.hpp"

using cinux::drivers::Canvas;
using cinux::drivers::Framebuffer;
using cinux::gui::DesktopIcon;
using cinux::gui::IconAction;

namespace {

Framebuffer g_fb;

/// Icon size constant (mirrors cinux::gui::icons::ICON_SIZE)
constexpr uint32_t ICON_SIZE   = 32;
constexpr uint32_t ICON_PIXELS = ICON_SIZE * ICON_SIZE;

/// Palette colours matching kernel/gui/data/icon_data.hpp::palette
namespace palette {
constexpr uint32_t BLACK       = 0x00000000;
constexpr uint32_t DARK_BLACK  = 0x00101010;
constexpr uint32_t WHITE       = 0x00FFFFFF;
constexpr uint32_t GREY_DARK   = 0x00404040;
constexpr uint32_t GREY_MID    = 0x00707070;
constexpr uint32_t BUTTON_GREY = 0x00909090;
constexpr uint32_t DISPLAY_BG  = 0x00C8DFC8;
constexpr uint32_t ORANGE      = 0x00FF8C00;
}  // namespace palette

/// Build a simple 32x32 test icon with a frame and cross pattern.
/// Border pixels are GREY_DARK, centre cross is WHITE, rest is DARK_BLACK.
/// Corners are transparent (BLACK).
void build_test_icon(uint32_t pixels[ICON_PIXELS]) {
    for (uint32_t r = 0; r < ICON_SIZE; r++) {
        for (uint32_t c = 0; c < ICON_SIZE; c++) {
            uint32_t idx = r * ICON_SIZE + c;

            // Transparent corners (2x2)
            if ((r < 2 && c < 2) || (r < 2 && c >= 30) || (r >= 30 && c < 2) ||
                (r >= 30 && c >= 30)) {
                pixels[idx] = palette::BLACK;
            }
            // Border (row 0, row 31, col 0, col 31)
            else if (r == 0 || r == 31 || c == 0 || c == 31) {
                pixels[idx] = palette::GREY_DARK;
            }
            // Centre cross (row 16 or col 16)
            else if (r == 16 || c == 16) {
                pixels[idx] = palette::WHITE;
            }
            // Body fill
            else {
                pixels[idx] = palette::DARK_BLACK;
            }
        }
    }
}

/// Build a second test icon (calculator-like) with different colours.
void build_test_icon2(uint32_t pixels[ICON_PIXELS]) {
    for (uint32_t r = 0; r < ICON_SIZE; r++) {
        for (uint32_t c = 0; c < ICON_SIZE; c++) {
            uint32_t idx = r * ICON_SIZE + c;

            // Transparent corners (2x2)
            if ((r < 2 && c < 2) || (r < 2 && c >= 30) || (r >= 30 && c < 2) ||
                (r >= 30 && c >= 30)) {
                pixels[idx] = palette::BLACK;
            }
            // Border
            else if (r == 0 || r == 31 || c == 0 || c == 31) {
                pixels[idx] = palette::GREY_DARK;
            }
            // Display area (rows 2-4, cols 2-29)
            else if (r >= 2 && r <= 4 && c >= 2 && c <= 29) {
                pixels[idx] = palette::DISPLAY_BG;
            }
            // Button grid (alternating BUTTON_GREY and ORANGE)
            else if (r >= 6 && r <= 28 && c >= 2 && c <= 29) {
                if ((r % 4 == 2) && (c == 28)) {
                    pixels[idx] = palette::ORANGE;  // equals column
                } else {
                    pixels[idx] = palette::BUTTON_GREY;
                }
            }
            // Body fill
            else {
                pixels[idx] = palette::GREY_MID;
            }
        }
    }
}

}  // anonymous namespace

// ============================================================
// draw_bitmap: pixel rendering tests
// ============================================================

/// Verify draw_bitmap renders opaque pixels at the correct position
void test_bitmap_render_opaque() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);
    uint32_t pixels[] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF};
    canvas.draw_bitmap(10, 10, 2, 2, pixels);
    canvas.flip();

    TEST_ASSERT_EQ(g_fb.get_pixel(10, 10), 0x00FF0000u);
    TEST_ASSERT_EQ(g_fb.get_pixel(11, 10), 0x0000FF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(10, 11), 0x000000FFu);
    TEST_ASSERT_EQ(g_fb.get_pixel(11, 11), 0x00FFFFFFu);
}

/// Verify draw_bitmap at origin (0,0)
void test_bitmap_render_at_origin() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);
    uint32_t pixel = 0x00AABBCC;
    canvas.draw_bitmap(0, 0, 1, 1, &pixel);
    canvas.flip();

    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00AABBCCu);
}

/// Verify draw_bitmap renders a larger bitmap filling the expected area
void test_bitmap_render_larger() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);

    // 8x8 bitmap, all yellow
    constexpr uint32_t W = 8, H = 8;
    uint32_t           pixels[W * H];
    for (uint32_t i = 0; i < W * H; i++)
        pixels[i] = 0x00FFFF00;

    canvas.draw_bitmap(5, 5, W, H, pixels);
    canvas.flip();

    // Check corners of the rendered area
    TEST_ASSERT_EQ(g_fb.get_pixel(5, 5), 0x00FFFF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(12, 5), 0x00FFFF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(5, 12), 0x00FFFF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(12, 12), 0x00FFFF00u);

    // Pixel just outside should be black
    TEST_ASSERT_EQ(g_fb.get_pixel(4, 5), 0u);
    TEST_ASSERT_EQ(g_fb.get_pixel(13, 5), 0u);
}

// ============================================================
// draw_bitmap: transparency tests
// ============================================================

/// Verify 0x00000000 pixels are skipped (transparent)
void test_bitmap_transparent_skip() {
    Canvas canvas;
    canvas.init(g_fb);

    // Pre-fill with blue
    canvas.clear(0x000000FF);

    // 2x2 bitmap with mixed transparency
    uint32_t pixels[] = {0x00000000, 0x00FF0000, 0x0000FF00, 0x00000000};
    canvas.draw_bitmap(10, 10, 2, 2, pixels);
    canvas.flip();

    // Transparent pixels should preserve the blue background
    TEST_ASSERT_EQ(g_fb.get_pixel(10, 10), 0x000000FFu);
    TEST_ASSERT_EQ(g_fb.get_pixel(11, 11), 0x000000FFu);
    // Opaque pixels overwrite
    TEST_ASSERT_EQ(g_fb.get_pixel(11, 10), 0x00FF0000u);
    TEST_ASSERT_EQ(g_fb.get_pixel(10, 11), 0x0000FF00u);
}

/// Verify all-transparent bitmap is a no-op
void test_bitmap_all_transparent() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0x00FF0000);

    uint32_t pixels[] = {0x00000000, 0x00000000, 0x00000000, 0x00000000};
    canvas.draw_bitmap(0, 0, 2, 2, pixels);
    canvas.flip();

    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00FF0000u);
    TEST_ASSERT_EQ(g_fb.get_pixel(1, 1), 0x00FF0000u);
}

// ============================================================
// draw_bitmap: clipping tests
// ============================================================

/// Verify draw_bitmap clips at the right edge of the canvas
void test_bitmap_clip_right() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);
    uint32_t pixels[] = {0x00FF0000, 0x00FF0000, 0x00FF0000, 0x00FF0000};

    // Place 4-pixel-wide bitmap at x = width-2 so only 2 fit
    uint32_t start_x = canvas.width() - 2;
    canvas.draw_bitmap(start_x, 0, 4, 1, pixels);
    canvas.flip();

    TEST_ASSERT_EQ(g_fb.get_pixel(start_x, 0), 0x00FF0000u);
    TEST_ASSERT_EQ(g_fb.get_pixel(start_x + 1, 0), 0x00FF0000u);
}

/// Verify draw_bitmap clips at the bottom edge of the canvas
void test_bitmap_clip_bottom() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);
    uint32_t pixels[] = {0x0000FF00, 0x0000FF00, 0x0000FF00, 0x0000FF00};

    // Place 4-pixel-tall bitmap at y = height-1 so only 1 fits
    uint32_t start_y = canvas.height() - 1;
    canvas.draw_bitmap(0, start_y, 1, 4, pixels);
    canvas.flip();

    TEST_ASSERT_EQ(g_fb.get_pixel(0, start_y), 0x0000FF00u);
}

/// Verify draw_bitmap completely outside canvas is a no-op
void test_bitmap_outside_canvas() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0x00FFFFFF);

    uint32_t pixels[] = {0x00FF0000, 0x00FF0000, 0x00FF0000, 0x00FF0000};
    canvas.draw_bitmap(canvas.width() + 10, 0, 2, 2, pixels);
    canvas.draw_bitmap(0, canvas.height() + 10, 2, 2, pixels);
    canvas.flip();

    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00FFFFFFu);
}

/// Verify draw_bitmap null pixels pointer is a no-op
void test_bitmap_null_pixels() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0x00FF0000);

    canvas.draw_bitmap(0, 0, 2, 2, nullptr);
    canvas.flip();

    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00FF0000u);
}

// ============================================================
// draw_bitmap: 32x32 icon rendering tests
// ============================================================

/// Verify rendering a 32x32 icon with transparent corners, border, and cross
void test_bitmap_render_test_icon() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);

    uint32_t icon[ICON_PIXELS];
    build_test_icon(icon);

    canvas.draw_bitmap(0, 0, ICON_SIZE, ICON_SIZE, icon);
    canvas.flip();

    // Top-left corner should be transparent (black)
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0u);
    TEST_ASSERT_EQ(g_fb.get_pixel(1, 1), 0u);

    // Border at (2, 0) should be GREY_DARK
    TEST_ASSERT_EQ(g_fb.get_pixel(2, 0), palette::GREY_DARK);

    // Body at (3, 3) should be DARK_BLACK
    TEST_ASSERT_EQ(g_fb.get_pixel(3, 3), palette::DARK_BLACK);

    // Centre cross at (16, 5) should be WHITE
    TEST_ASSERT_EQ(g_fb.get_pixel(16, 5), palette::WHITE);
    TEST_ASSERT_EQ(g_fb.get_pixel(5, 16), palette::WHITE);
}

/// Verify rendering a second test icon with display area and button grid
void test_bitmap_render_test_icon2() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);

    uint32_t icon[ICON_PIXELS];
    build_test_icon2(icon);

    canvas.draw_bitmap(0, 0, ICON_SIZE, ICON_SIZE, icon);
    canvas.flip();

    // Top-left corner should be transparent
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0u);

    // Border at (2, 0) should be GREY_DARK
    TEST_ASSERT_EQ(g_fb.get_pixel(2, 0), palette::GREY_DARK);

    // Body at (1, 5) should be GREY_MID
    TEST_ASSERT_EQ(g_fb.get_pixel(1, 5), palette::GREY_MID);

    // Display area at (5, 3) should be DISPLAY_BG
    TEST_ASSERT_EQ(g_fb.get_pixel(5, 3), palette::DISPLAY_BG);

    // Button area at (5, 10) should be BUTTON_GREY
    TEST_ASSERT_EQ(g_fb.get_pixel(5, 10), palette::BUTTON_GREY);

    // Equals button at (28, 10) should be ORANGE
    TEST_ASSERT_EQ(g_fb.get_pixel(28, 10), palette::ORANGE);
}

/// Verify two icons rendered side by side with a gap
void test_bitmap_render_two_icons() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);

    uint32_t icon1[ICON_PIXELS];
    uint32_t icon2[ICON_PIXELS];
    build_test_icon(icon1);
    build_test_icon2(icon2);

    uint32_t gap = 16;
    canvas.draw_bitmap(0, 0, ICON_SIZE, ICON_SIZE, icon1);
    canvas.draw_bitmap(ICON_SIZE + gap, 0, ICON_SIZE, ICON_SIZE, icon2);
    canvas.flip();

    // Icon 1 body at (3, 3) should be DARK_BLACK
    TEST_ASSERT_EQ(g_fb.get_pixel(3, 3), palette::DARK_BLACK);

    // Gap area should be transparent (black)
    TEST_ASSERT_EQ(g_fb.get_pixel(ICON_SIZE + gap / 2, 5), 0u);

    // Icon 2 body at (ICON_SIZE + gap + 1, 5) should be GREY_MID
    TEST_ASSERT_EQ(g_fb.get_pixel(ICON_SIZE + gap + 1, 5), palette::GREY_MID);

    // Icon 2 display at (ICON_SIZE + gap + 5, 3) should be DISPLAY_BG
    TEST_ASSERT_EQ(g_fb.get_pixel(ICON_SIZE + gap + 5, 3), palette::DISPLAY_BG);
}

// ============================================================
// DesktopIcon::contains tests
// ============================================================

/// Verify contains returns true for point inside icon
void test_desktop_icon_contains_inside() {
    DesktopIcon icon{
        .x      = 10,
        .y      = 20,
        .bitmap = nullptr,
        .label  = "Shell",
        .width  = ICON_SIZE,
        .height = ICON_SIZE,
        .action = IconAction::OpenShell,
    };

    TEST_ASSERT_TRUE(icon.contains(10, 20));  // top-left corner
    TEST_ASSERT_TRUE(icon.contains(25, 35));  // middle
    TEST_ASSERT_TRUE(icon.contains(41, 51));  // last pixel (x+w-1, y+h-1)
}

/// Verify contains returns false for point outside icon
void test_desktop_icon_contains_outside() {
    DesktopIcon icon{
        .x      = 10,
        .y      = 20,
        .bitmap = nullptr,
        .label  = "Shell",
        .width  = ICON_SIZE,
        .height = ICON_SIZE,
        .action = IconAction::OpenShell,
    };

    TEST_ASSERT_FALSE(icon.contains(9, 20));   // left of icon
    TEST_ASSERT_FALSE(icon.contains(10, 19));  // above icon
    TEST_ASSERT_FALSE(icon.contains(42, 51));  // right edge (exclusive)
    TEST_ASSERT_FALSE(icon.contains(41, 52));  // bottom edge (exclusive)
    TEST_ASSERT_FALSE(icon.contains(0, 0));    // far away
}

/// Verify contains with negative icon position
void test_desktop_icon_contains_negative_position() {
    DesktopIcon icon{
        .x      = -10,
        .y      = -5,
        .bitmap = nullptr,
        .label  = "Offscreen",
        .width  = 32,
        .height = 32,
        .action = IconAction::None,
    };

    TEST_ASSERT_TRUE(icon.contains(-10, -5));  // top-left corner
    TEST_ASSERT_TRUE(icon.contains(0, 0));     // well inside
    TEST_ASSERT_TRUE(icon.contains(21, 26));   // last pixel
    TEST_ASSERT_FALSE(icon.contains(22, 26));  // one past right
    TEST_ASSERT_FALSE(icon.contains(21, 27));  // one past bottom
}

/// Verify IconAction enum values
void test_icon_action_values() {
    TEST_ASSERT_EQ(static_cast<uint8_t>(IconAction::None), 0);
    TEST_ASSERT_EQ(static_cast<uint8_t>(IconAction::OpenShell), 1);
    TEST_ASSERT_EQ(static_cast<uint8_t>(IconAction::OpenCalculator), 2);
}

/// Verify 1x1 icon contains only its single pixel
void test_desktop_icon_1x1() {
    DesktopIcon icon{
        .x      = 50,
        .y      = 50,
        .bitmap = nullptr,
        .label  = "Dot",
        .width  = 1,
        .height = 1,
        .action = IconAction::None,
    };

    TEST_ASSERT_TRUE(icon.contains(50, 50));
    TEST_ASSERT_FALSE(icon.contains(49, 50));
    TEST_ASSERT_FALSE(icon.contains(51, 50));
    TEST_ASSERT_FALSE(icon.contains(50, 49));
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_bitmap_icon_tests() {
    TEST_SECTION("Bitmap Icon Tests (032_gui_bitmap_icon)");

    // Initialise framebuffer for pixel-verification tests
    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto*                      bi             = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    g_fb.clear(0);

    // draw_bitmap pixel rendering tests
    RUN_TEST(test_bitmap_render_opaque);
    RUN_TEST(test_bitmap_render_at_origin);
    RUN_TEST(test_bitmap_render_larger);

    // Transparency tests
    RUN_TEST(test_bitmap_transparent_skip);
    RUN_TEST(test_bitmap_all_transparent);

    // Clipping tests
    RUN_TEST(test_bitmap_clip_right);
    RUN_TEST(test_bitmap_clip_bottom);
    RUN_TEST(test_bitmap_outside_canvas);
    RUN_TEST(test_bitmap_null_pixels);

    // 32x32 icon rendering tests
    RUN_TEST(test_bitmap_render_test_icon);
    RUN_TEST(test_bitmap_render_test_icon2);
    RUN_TEST(test_bitmap_render_two_icons);

    // DesktopIcon::contains tests
    RUN_TEST(test_desktop_icon_contains_inside);
    RUN_TEST(test_desktop_icon_contains_outside);
    RUN_TEST(test_desktop_icon_contains_negative_position);
    RUN_TEST(test_icon_action_values);
    RUN_TEST(test_desktop_icon_1x1);

    TEST_SUMMARY();
}

#else  // !CINUX_GUI

// CLI mode stub: no GUI tests to run
extern "C" void run_bitmap_icon_tests() {
    using cinux::lib::kprintf;
    kprintf("[BITMAP_ICON] CLI mode -- GUI tests skipped.\n");
}

#endif  // CINUX_GUI
