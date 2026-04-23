/**
 * @file kernel/test/test_canvas.cpp
 * @brief QEMU in-kernel tests for Canvas (029_gui_canvas)
 *
 * GUI mode: draws 10 random-colour rectangles + title text "Cinux GUI"
 *           onto the canvas back buffer, then flips to the front buffer.
 *
 * CLI mode: verifies Console still works normally (no canvas compiled).
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Framebuffer available (set up by bootloader via test_video)
 */

#include "big_kernel_test.h"

#include "boot/boot_info.h"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"

#ifdef CINUX_GUI
#include "kernel/drivers/canvas.hpp"
#endif

using cinux::drivers::Framebuffer;
using cinux::drivers::PSFFont;

static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

namespace {

Framebuffer g_fb;
PSFFont g_font;

#ifdef CINUX_GUI

/**
 * @brief Simple LCG pseudo-random number generator
 *
 * Deterministic PRNG so test output is reproducible across runs.
 */
struct LCG {
    uint32_t state = 12345;

    uint32_t next() {
        state = state * 1103515245u + 12345u;
        return (state >> 16) & 0x7FFF;
    }

    uint32_t next_color() {
        // Generate a visible colour: each channel 0x40-0xFF
        uint32_t r = 0x40 + (next() % 0xC0);
        uint32_t g = 0x40 + (next() % 0xC0);
        uint32_t b = 0x40 + (next() % 0xC0);
        return 0x00000000 | (r << 16) | (g << 8) | b;
    }
};

#endif

}  // anonymous namespace

// ============================================================
// CLI mode tests (always compiled)
// ============================================================

void test_canvas_cli_fb_available() {
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);

    TEST_ASSERT_GT(g_fb.width(), 0u);
    TEST_ASSERT_GT(g_fb.height(), 0u);
    TEST_ASSERT_GT(g_fb.pitch(), 0u);
}

void test_canvas_cli_font_available() {
    g_font.init();

    TEST_ASSERT_GT(g_font.width(), 0u);
    TEST_ASSERT_GT(g_font.height(), 0u);
}

void test_canvas_cli_framebuffer_intact() {
    // Verify framebuffer is still usable for direct pixel access (CLI path)
    g_fb.clear(0x00224466);
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00224466u);
    TEST_ASSERT_EQ(g_fb.get_pixel(100, 100), 0x00224466u);

    // Restore to black
    g_fb.clear(0);
}

// ============================================================
// GUI mode tests (only compiled when CINUX_GUI is defined)
// ============================================================

#ifdef CINUX_GUI

using cinux::drivers::Canvas;

void test_canvas_init() {
    Canvas canvas;
    canvas.init(g_fb);

    TEST_ASSERT_EQ(canvas.width(), g_fb.width());
    TEST_ASSERT_EQ(canvas.height(), g_fb.height());
    TEST_ASSERT_EQ(canvas.pitch(), g_fb.pitch());
}

void test_canvas_draw_rect() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);
    canvas.draw_rect(10, 10, 50, 30, 0x00FF0000);
    canvas.flip();

    // Verify the front buffer received the red rectangle
    TEST_ASSERT_EQ(g_fb.get_pixel(10, 10), 0x00FF0000u);
    TEST_ASSERT_EQ(g_fb.get_pixel(35, 25), 0x00FF0000u);
}

void test_canvas_clear() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.draw_rect(0, 0, 100, 100, 0x00FF0000);
    canvas.clear(0x0000FF00);
    canvas.flip();

    // After clear + flip, entire front buffer should be green
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x0000FF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 50), 0x0000FF00u);
}

void test_canvas_draw_text() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0x00112233);
    canvas.draw_text(10, 10, "Test", 0x00FFFFFF, g_font);
    canvas.flip();

    // "Test" at (10,10) -- check that the background colour is present
    // and at least one foreground pixel was rendered
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00112233u);

    bool has_fg = false;
    for (uint32_t row = 10; row < 10 + g_font.height() && !has_fg; row++) {
        for (uint32_t col = 10; col < 10 + 4 * g_font.width() && !has_fg; col++) {
            if (g_fb.get_pixel(col, row) == 0x00FFFFFF) {
                has_fg = true;
            }
        }
    }
    TEST_ASSERT_TRUE(has_fg);
}

void test_canvas_draw_rect_outline() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);
    canvas.draw_rect_outline(50, 50, 20, 10, 0x000000FF);
    canvas.flip();

    // Top-left corner of outline should be blue
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 50), 0x000000FFu);
    // Interior (51,51) should remain black
    TEST_ASSERT_EQ(g_fb.get_pixel(51, 51), 0u);
}

void test_canvas_draw_line() {
    Canvas canvas;
    canvas.init(g_fb);

    canvas.clear(0);
    canvas.draw_line(0, 0, 30, 30, 0x00FFFF00);
    canvas.flip();

    // Start and end of the line should be yellow
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00FFFF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(30, 30), 0x00FFFF00u);
}

void test_canvas_gui_demo() {
    /**
     * Main GUI demo: draw 10 random-colour rectangles + "Cinux GUI" title.
     * This is the visual milestone verification -- what appears on screen
     * when running the kernel in QEMU with GUI mode enabled.
     */
    Canvas canvas;
    canvas.init(g_fb);

    // Dark background
    canvas.clear(0x001A1A2E);

    LCG rng;

    // Draw 10 random-colour rectangles scattered across the screen
    for (int i = 0; i < 10; i++) {
        uint32_t x = rng.next() % (canvas.width() - 100);
        uint32_t y = rng.next() % (canvas.height() - 60);
        uint32_t w = 40 + (rng.next() % 120);
        uint32_t h = 30 + (rng.next() % 80);
        uint32_t color = rng.next_color();

        canvas.draw_rect(x, y, w, h, color);
    }

    // Draw title "Cinux GUI" in white, centered near the top
    const char* title = "Cinux GUI";
    uint32_t text_w = 9 * g_font.width();  // 9 chars * glyph width
    uint32_t text_x = (canvas.width() - text_w) / 2;
    uint32_t text_y = 10;
    canvas.draw_text(text_x, text_y, title, 0x00FFFFFF, g_font);

    // Flip to front buffer -- this is what the user sees
    canvas.flip();

    // Verification: check that the background colour is present
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x001A1A2Eu);

    // Verification: check that the title text rendered at least one white pixel
    bool has_title = false;
    for (uint32_t row = text_y; row < text_y + g_font.height() && !has_title; row++) {
        for (uint32_t col = text_x; col < text_x + text_w && !has_title; col++) {
            if (g_fb.get_pixel(col, row) == 0x00FFFFFF) {
                has_title = true;
            }
        }
    }
    TEST_ASSERT_TRUE(has_title);

    cinux::lib::kprintf("[CANVAS] GUI demo rendered: 10 rects + title\n");
}

#endif

// ============================================================
// Entry point
// ============================================================

extern "C" void run_canvas_tests() {
    TEST_SECTION("Canvas Tests (029_gui_canvas)");

    // CLI mode tests -- always run
    RUN_TEST(test_canvas_cli_fb_available);
    RUN_TEST(test_canvas_cli_font_available);
    RUN_TEST(test_canvas_cli_framebuffer_intact);

#ifdef CINUX_GUI
    TEST_SECTION("Canvas GUI Tests (029_gui_canvas)");

    RUN_TEST(test_canvas_init);
    RUN_TEST(test_canvas_draw_rect);
    RUN_TEST(test_canvas_clear);
    RUN_TEST(test_canvas_draw_text);
    RUN_TEST(test_canvas_draw_rect_outline);
    RUN_TEST(test_canvas_draw_line);
    RUN_TEST(test_canvas_gui_demo);
#else
    cinux::lib::kprintf("[CANVAS] CLI mode -- GUI tests skipped.\n");
#endif

    TEST_SUMMARY();
}
