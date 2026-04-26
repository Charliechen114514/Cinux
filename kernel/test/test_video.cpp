/**
 * @file kernel/test/test_video.cpp
 * @brief QEMU in-kernel tests for framebuffer, font, and console drivers (013)
 *
 * Runs inside QEMU, testing against the real VBE framebuffer set up by the
 * bootloader.  The bootloader has already switched to VBE mode 0x118
 * (1024x768x32) and populated BootInfo at physical 0x7000.
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 */

#include "big_kernel_test.h"
#include "boot/boot_info.h"
#include "kernel/drivers/video/console.hpp"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"

using cinux::drivers::Console;
using cinux::drivers::Framebuffer;
using cinux::drivers::PSFFont;

static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;

namespace {

Framebuffer g_fb;
PSFFont     g_font;

}  // anonymous namespace

// ============================================================
// Framebuffer tests
// ============================================================

void test_fb_init_from_bootinfo() {
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);

    TEST_ASSERT_EQ(g_fb.width(), 1024u);
    TEST_ASSERT_EQ(g_fb.height(), 768u);
    TEST_ASSERT_GT(g_fb.pitch(), 0u);
    TEST_ASSERT_EQ(g_fb.pitch() % 4, 0u);
    TEST_ASSERT_GE(g_fb.pitch(), g_fb.width() * 4u);
}

void test_fb_put_pixel_readback() {
    g_fb.clear(0);
    g_fb.put_pixel(100, 100, 0x00FF0000);
    TEST_ASSERT_EQ(g_fb.get_pixel(100, 100), 0x00FF0000u);

    // Surrounding pixels should still be black
    TEST_ASSERT_EQ(g_fb.get_pixel(99, 100), 0u);
    TEST_ASSERT_EQ(g_fb.get_pixel(101, 100), 0u);
}

void test_fb_corners_readback() {
    g_fb.clear(0);
    g_fb.put_pixel(0, 0, 0x00FFFFFF);
    g_fb.put_pixel(1023, 767, 0x00FFFFFF);
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00FFFFFFu);
    TEST_ASSERT_EQ(g_fb.get_pixel(1023, 767), 0x00FFFFFFu);
}

void test_fb_fill_rect_readback() {
    g_fb.clear(0);
    g_fb.fill_rect(50, 50, 10, 10, 0x0000FF00);

    // Interior should be green
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 50), 0x0000FF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(55, 55), 0x0000FF00u);
    TEST_ASSERT_EQ(g_fb.get_pixel(59, 59), 0x0000FF00u);

    // Exterior should be black
    TEST_ASSERT_EQ(g_fb.get_pixel(49, 50), 0u);
    TEST_ASSERT_EQ(g_fb.get_pixel(60, 50), 0u);
}

void test_fb_clear_readback() {
    g_fb.clear(0x00AAAAAA);
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00AAAAAAu);
    TEST_ASSERT_EQ(g_fb.get_pixel(512, 384), 0x00AAAAAAu);
    TEST_ASSERT_EQ(g_fb.get_pixel(1023, 767), 0x00AAAAAAu);
}

void test_fb_scroll_up_readback() {
    g_fb.clear(0);
    // Paint a red marker at row 100
    for (uint32_t x = 0; x < 8; x++) {
        g_fb.put_pixel(x, 100, 0x00FF0000);
    }

    g_fb.scroll_up(16, 16, 0);

    // The marker should have moved up by 16 rows
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 84), 0x00FF0000u);

    // Original position should now be black
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 100), 0u);
}

// ============================================================
// Font tests
// ============================================================

void test_font_init() {
    g_font.init();
    TEST_ASSERT_EQ(g_font.width(), 8u);
    TEST_ASSERT_EQ(g_font.height(), 16u);
}

void test_font_render_char() {
    g_fb.clear(0);
    g_font.render_char(g_fb, static_cast<uint8_t>('A'), 0, 0, 0x00FFFFFF, 0x00000000);

    // 'A' should have at least one foreground (white) pixel in its glyph
    bool has_fg = false;
    bool has_bg = false;
    for (uint32_t row = 0; row < g_font.height(); row++) {
        for (uint32_t col = 0; col < g_font.width(); col++) {
            uint32_t px = g_fb.get_pixel(col, row);
            if (px == 0x00FFFFFF)
                has_fg = true;
            if (px == 0)
                has_bg = true;
        }
    }
    TEST_ASSERT_TRUE(has_fg);
    TEST_ASSERT_TRUE(has_bg);
}

void test_font_render_with_bg_color() {
    g_fb.clear(0);
    g_font.render_char(g_fb, static_cast<uint8_t>(' '), 0, 0, 0x00FFFFFF, 0x00FF0000);

    // Space glyph is all background -- entire 8x16 area should be red
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0x00FF0000u);
    TEST_ASSERT_EQ(g_fb.get_pixel(7, 15), 0x00FF0000u);
}

// ============================================================
// Console tests
// ============================================================

void test_console_putc_visible() {
    g_fb.clear(0);
    Console console;
    console.init(g_fb, g_font, 0x00FFFFFF, 0x00000000);

    console.putc('X');

    // 'X' at position (0,0) should have some white pixels
    bool has_fg = false;
    for (uint32_t row = 0; row < g_font.height(); row++) {
        for (uint32_t col = 0; col < g_font.width(); col++) {
            if (g_fb.get_pixel(col, row) == 0x00FFFFFF) {
                has_fg = true;
                break;
            }
        }
        if (has_fg)
            break;
    }
    TEST_ASSERT_TRUE(has_fg);
}

void test_console_clear() {
    g_fb.clear(0);
    Console console;
    console.init(g_fb, g_font, 0x00FFFFFF, 0x00000000);

    console.putc('A');
    console.clear();

    // After clear, everything should be black
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0u);
    TEST_ASSERT_EQ(g_fb.get_pixel(512, 384), 0u);
}

void test_console_multiple_chars() {
    g_fb.clear(0);
    Console console;
    console.init(g_fb, g_font, 0x00FFFFFF, 0x00000000);

    console.putc('A');
    console.putc('B');

    // 'B' should be at x=8 (second glyph position)
    bool has_b_fg = false;
    for (uint32_t row = 0; row < g_font.height(); row++) {
        for (uint32_t col = 8; col < 16; col++) {
            if (g_fb.get_pixel(col, row) == 0x00FFFFFF) {
                has_b_fg = true;
                break;
            }
        }
        if (has_b_fg)
            break;
    }
    TEST_ASSERT_TRUE(has_b_fg);
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_video_tests() {
    TEST_SECTION("Video: Framebuffer");

    RUN_TEST(test_fb_init_from_bootinfo);
    RUN_TEST(test_fb_put_pixel_readback);
    RUN_TEST(test_fb_corners_readback);
    RUN_TEST(test_fb_fill_rect_readback);
    RUN_TEST(test_fb_clear_readback);
    RUN_TEST(test_fb_scroll_up_readback);

    TEST_SECTION("Video: Font");

    RUN_TEST(test_font_init);
    RUN_TEST(test_font_render_char);
    RUN_TEST(test_font_render_with_bg_color);

    TEST_SECTION("Video: Console");

    RUN_TEST(test_console_putc_visible);
    RUN_TEST(test_console_clear);
    RUN_TEST(test_console_multiple_chars);
}
