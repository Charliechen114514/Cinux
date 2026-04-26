/**
 * @file kernel/test/test_window.cpp
 * @brief QEMU in-kernel tests for Window class (030_gui_wm_basic, sub-iteration B)
 *
 * Tests the real kernel Window class with off-screen Canvas and Framebuffer.
 * Uses Framebuffer::get_pixel() to verify rendered output after blit_to + flip.
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Framebuffer available (set up by bootloader)
 *   - Heap initialised (for Canvas::init new[] allocation)
 *   - PSFFont initialised (for draw_title_bar text rendering)
 *
 * Compile condition: CINUX_GUI
 */

#include "big_kernel_test.h"
#include "boot/boot_info.h"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"

#ifdef CINUX_GUI

#    include "kernel/drivers/canvas.hpp"
#    include "kernel/gui/window.hpp"

using cinux::drivers::Canvas;
using cinux::drivers::Framebuffer;
using cinux::drivers::PSFFont;
using cinux::gui::Window;

namespace {

Framebuffer g_fb;
PSFFont     g_font;

}  // anonymous namespace

// ============================================================
// Construction tests
// ============================================================

/// Verify default Window construction initialises fields correctly
void test_window_default_construction() {
    Window w;

    TEST_ASSERT_EQ(w.id(), 1u);
    TEST_ASSERT_EQ(w.x(), 0);
    TEST_ASSERT_EQ(w.y(), 0);
    TEST_ASSERT_EQ(w.width(), 320u);
    TEST_ASSERT_EQ(w.height(), 240u);
    TEST_ASSERT_TRUE(w.visible());
    TEST_ASSERT_FALSE(w.focused());
}

/// Verify custom construction sets position and size
void test_window_custom_construction() {
    Window w("My App", 50, 100, 400, 300);

    TEST_ASSERT_EQ(w.x(), 50);
    TEST_ASSERT_EQ(w.y(), 100);
    TEST_ASSERT_EQ(w.width(), 400u);
    TEST_ASSERT_EQ(w.height(), 300u);
    TEST_ASSERT_EQ(w.total_height(), 320u);
}

/// Verify multiple windows get auto-incrementing IDs
void test_window_id_auto_increment() {
    Window a("A");
    Window b("B");
    Window c("C");

    // IDs should be sequential (1, 2, 3, or continuing from earlier tests)
    TEST_ASSERT_LT(a.id(), b.id());
    TEST_ASSERT_LT(b.id(), c.id());
}

/// Verify null title is handled gracefully
void test_window_null_title() {
    Window w(nullptr, 0, 0, 100, 50);

    TEST_ASSERT_EQ(w.title()[0], '\0');
}

// ============================================================
// set_position / resize / set_title tests
// ============================================================

/// Verify set_position updates coordinates
void test_window_set_position() {
    Window w("Test", 10, 20, 100, 50);

    w.set_position(200, 300);
    TEST_ASSERT_EQ(w.x(), 200);
    TEST_ASSERT_EQ(w.y(), 300);
}

/// Verify resize updates dimensions and reallocates canvas
void test_window_resize() {
    Window w("Test", 0, 0, 100, 50);

    w.resize(200, 150);
    TEST_ASSERT_EQ(w.width(), 200u);
    TEST_ASSERT_EQ(w.height(), 150u);
    TEST_ASSERT_EQ(w.total_height(), 170u);

    Canvas& c = w.canvas();
    TEST_ASSERT_EQ(c.width(), 200u);
    TEST_ASSERT_EQ(c.height(), 170u);
}

/// Verify set_title changes the title string
void test_window_set_title() {
    Window w("Old");

    w.set_title("New Title");
    // Compare first characters
    TEST_ASSERT_EQ(w.title()[0], 'N');
    TEST_ASSERT_EQ(w.title()[1], 'e');
    TEST_ASSERT_EQ(w.title()[2], 'w');
}

// ============================================================
// set_visible / set_focused tests
// ============================================================

/// Verify set_visible toggles visibility
void test_window_set_visible() {
    Window w;
    TEST_ASSERT_TRUE(w.visible());

    w.set_visible(false);
    TEST_ASSERT_FALSE(w.visible());

    w.set_visible(true);
    TEST_ASSERT_TRUE(w.visible());
}

/// Verify set_focused toggles focus
void test_window_set_focused() {
    Window w;
    TEST_ASSERT_FALSE(w.focused());

    w.set_focused(true);
    TEST_ASSERT_TRUE(w.focused());

    w.set_focused(false);
    TEST_ASSERT_FALSE(w.focused());
}

// ============================================================
// is_close_button_hit tests
// ============================================================

/// Verify close button hit at exact top-left corner
void test_window_close_button_hit_top_left() {
    // Window at (0,0), width=100
    // cb_x = 0 + 100 - 14 - 3 = 83
    // cb_y = 0 + (20 - 14) / 2 = 3
    Window w("Test", 0, 0, 100, 50);

    TEST_ASSERT_TRUE(w.is_close_button_hit(83, 3));
}

/// Verify close button hit at bottom-right corner (exclusive boundary)
void test_window_close_button_hit_bottom_right() {
    Window w("Test", 0, 0, 100, 50);

    // cb_x=83, cb_y=3, size=14 -> bottom-right is (96,16), exclusive
    TEST_ASSERT_TRUE(w.is_close_button_hit(96, 16));
    TEST_ASSERT_FALSE(w.is_close_button_hit(97, 16));
    TEST_ASSERT_FALSE(w.is_close_button_hit(96, 17));
}

/// Verify close button not hit outside window
void test_window_close_button_miss_far_away() {
    Window w("Test", 100, 100, 200, 150);

    TEST_ASSERT_FALSE(w.is_close_button_hit(0, 0));
    TEST_ASSERT_FALSE(w.is_close_button_hit(500, 500));
}

/// Verify close button not hit in content area
void test_window_close_button_miss_content() {
    Window w("Test", 10, 10, 200, 100);

    TEST_ASSERT_FALSE(w.is_close_button_hit(50, 50));
}

/// Verify close button not hit in title bar but outside button area
void test_window_close_button_miss_title_bar() {
    Window w("Test", 0, 0, 200, 100);

    TEST_ASSERT_FALSE(w.is_close_button_hit(10, 5));
}

/// Verify close button hit with non-zero window position
void test_window_close_button_hit_offset_position() {
    Window w("Test", 50, 80, 200, 100);

    // cb_x = 50 + 200 - 14 - 3 = 233
    // cb_y = 80 + (20 - 14) / 2 = 83
    TEST_ASSERT_TRUE(w.is_close_button_hit(233, 83));
    TEST_ASSERT_TRUE(w.is_close_button_hit(246, 96));
    TEST_ASSERT_FALSE(w.is_close_button_hit(232, 83));
    TEST_ASSERT_FALSE(w.is_close_button_hit(233, 82));
}

// ============================================================
// contains tests
// ============================================================

/// Verify contains returns true for point inside window
void test_window_contains_inside() {
    Window w("Test", 10, 20, 100, 50);

    TEST_ASSERT_TRUE(w.contains(10, 20));
    TEST_ASSERT_TRUE(w.contains(50, 50));
}

/// Verify contains returns false for point outside window
void test_window_contains_outside() {
    Window w("Test", 10, 20, 100, 50);

    TEST_ASSERT_FALSE(w.contains(50, 19));   // above
    TEST_ASSERT_FALSE(w.contains(50, 90));   // below (total_height=70)
    TEST_ASSERT_FALSE(w.contains(9, 30));    // left
    TEST_ASSERT_FALSE(w.contains(110, 30));  // right
}

/// Verify contains rejects exclusive boundaries
void test_window_contains_exclusive_boundary() {
    Window w("Test", 0, 0, 100, 50);

    TEST_ASSERT_FALSE(w.contains(100, 25));  // right edge exclusive
    TEST_ASSERT_FALSE(w.contains(50, 70));   // bottom edge exclusive
}

/// Verify contains includes title bar area
void test_window_contains_title_bar() {
    Window w("Test", 0, 0, 100, 50);

    TEST_ASSERT_TRUE(w.contains(50, 10));
    TEST_ASSERT_TRUE(w.contains(50, 0));
    TEST_ASSERT_TRUE(w.contains(50, 19));
}

// ============================================================
// Canvas allocation tests
// ============================================================

/// Verify canvas dimensions include title bar
void test_window_canvas_dimensions() {
    Window w("Test", 0, 0, 200, 150);

    Canvas& c = w.canvas();
    TEST_ASSERT_EQ(c.width(), 200u);
    TEST_ASSERT_EQ(c.height(), 170u);  // 150 + 20
}

/// Verify canvas is usable for drawing after construction
void test_window_canvas_usable() {
    Window w("Test", 0, 0, 100, 50);

    // draw_content should not crash
    w.draw_content();
    // If we got here without crashing, canvas is usable
    TEST_ASSERT_TRUE(true);
}

// ============================================================
// Pixel verification tests (via blit_to + flip + get_pixel)
// ============================================================

/// Verify draw_content fills content area with light grey
void test_window_draw_content_pixels() {
    Window w("Test", 0, 0, 100, 50);
    w.draw_content();

    Canvas dst;
    dst.init(g_fb);
    w.blit_to(dst);
    dst.flip();

    // Content area at screen position (0, 20) should be light grey
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 20), Window::COLOR_CONTENT_BG);
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 40), Window::COLOR_CONTENT_BG);
    TEST_ASSERT_EQ(g_fb.get_pixel(99, 69), Window::COLOR_CONTENT_BG);
}

/// Verify draw_title_bar renders blue background and red close button
void test_window_draw_title_bar_pixels() {
    Window w("Test", 0, 0, 100, 50);

    Canvas dst;
    dst.init(g_fb);
    w.draw_title_bar(g_font);
    w.blit_to(dst);
    dst.flip();

    // Title bar background at (0, 0) should be steel blue
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), Window::COLOR_TITLE_BG);
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 5), Window::COLOR_TITLE_BG);

    // Close button at (83, 3) should be red
    TEST_ASSERT_EQ(g_fb.get_pixel(83, 3), Window::COLOR_CLOSE_BUTTON);

    // Border at bottom of title bar (row 19)
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 19), Window::COLOR_BORDER);
}

/// Verify blit_to places window at correct screen position
void test_window_blit_to_position() {
    Window w("Test", 50, 60, 40, 20);
    w.draw_content();

    Canvas dst;
    dst.init(g_fb);
    w.blit_to(dst);
    dst.flip();

    // Content at screen position (50, 60+20=80)
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 80), Window::COLOR_CONTENT_BG);

    // Pixel before window should be black (cleared canvas)
    TEST_ASSERT_EQ(g_fb.get_pixel(49, 80), 0u);
}

/// Verify blit_to does nothing when window is invisible
void test_window_blit_to_invisible() {
    Window w("Test", 0, 0, 50, 30);
    w.draw_content();
    w.set_visible(false);

    Canvas dst;
    dst.init(g_fb);
    w.blit_to(dst);
    dst.flip();

    // Destination should remain black (canvas was cleared by init)
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 0), 0u);
    TEST_ASSERT_EQ(g_fb.get_pixel(25, 10), 0u);
}

// ============================================================
// Multiple window tests
// ============================================================

/// Verify multiple windows have independent state
void test_window_multiple_independent() {
    Window a("A", 0, 0, 100, 100);
    Window b("B", 200, 200, 150, 80);

    TEST_ASSERT_LT(a.id(), b.id());
    TEST_ASSERT_EQ(a.x(), 0);
    TEST_ASSERT_EQ(b.x(), 200);
    TEST_ASSERT_EQ(a.title()[0], 'A');
    TEST_ASSERT_EQ(b.title()[0], 'B');

    a.set_position(10, 10);
    a.set_title("Modified");
    TEST_ASSERT_EQ(a.x(), 10);
    TEST_ASSERT_EQ(b.x(), 200);
    TEST_ASSERT_EQ(b.title()[0], 'B');
}

/// Verify multiple windows can blit independently to same destination
void test_window_multiple_blit() {
    Window a("A", 0, 0, 50, 30);
    Window b("B", 60, 0, 50, 30);

    a.draw_content();
    b.draw_content();

    Canvas dst;
    dst.init(g_fb);
    a.blit_to(dst);
    b.blit_to(dst);
    dst.flip();

    // Both windows' content areas should be visible
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 20), Window::COLOR_CONTENT_BG);
    TEST_ASSERT_EQ(g_fb.get_pixel(60, 20), Window::COLOR_CONTENT_BG);

    // Gap between windows should be black
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 20), 0u);
    TEST_ASSERT_EQ(g_fb.get_pixel(55, 20), 0u);
}

/// Verify multiple windows have auto-incrementing IDs
void test_window_id_sequence() {
    // Create two windows and verify IDs are consecutive
    Window x("X");
    Window y("Y");
    TEST_ASSERT_EQ(y.id(), x.id() + 1);
}

// ============================================================
// Edge case tests
// ============================================================

/// Verify zero-height content area window works
void test_window_zero_height() {
    Window w("Test", 0, 0, 50, 0);

    TEST_ASSERT_EQ(w.height(), 0u);
    TEST_ASSERT_EQ(w.total_height(), 20u);

    Canvas& c = w.canvas();
    TEST_ASSERT_EQ(c.width(), 50u);
    TEST_ASSERT_EQ(c.height(), 20u);
}

/// Verify negative position values work correctly
void test_window_negative_position() {
    Window w("Test", -10, -5, 100, 50);

    TEST_ASSERT_EQ(w.x(), -10);
    TEST_ASSERT_EQ(w.y(), -5);
    TEST_ASSERT_TRUE(w.contains(-10, -5));
    TEST_ASSERT_FALSE(w.contains(-11, -5));
}

/// Verify window constants match expected values
void test_window_constants() {
    TEST_ASSERT_EQ(Window::TITLE_BAR_HEIGHT, 20u);
    TEST_ASSERT_EQ(Window::CLOSE_BUTTON_SIZE, 14u);
    TEST_ASSERT_EQ(Window::TITLE_MAX_LEN, 63u);
    TEST_ASSERT_EQ(Window::DEFAULT_WIDTH, 320u);
    TEST_ASSERT_EQ(Window::DEFAULT_HEIGHT, 240u);

    TEST_ASSERT_EQ(Window::COLOR_TITLE_BG, 0x00336699u);
    TEST_ASSERT_EQ(Window::COLOR_TITLE_TEXT, 0x00FFFFFFu);
    TEST_ASSERT_EQ(Window::COLOR_CLOSE_BUTTON, 0x00CC3333u);
    TEST_ASSERT_EQ(Window::COLOR_CONTENT_BG, 0x00E0E0E0u);
    TEST_ASSERT_EQ(Window::COLOR_BORDER, 0x00444444u);
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_window_tests() {
    TEST_SECTION("Window Tests (030_gui_wm_basic, sub-iteration B)");

    // Initialise framebuffer and font for pixel-verification tests
    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto*                      bi             = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    g_font.init();
    g_fb.clear(0);

    // Construction tests
    RUN_TEST(test_window_default_construction);
    RUN_TEST(test_window_custom_construction);
    RUN_TEST(test_window_id_auto_increment);
    RUN_TEST(test_window_null_title);

    // set_position / resize / set_title tests
    RUN_TEST(test_window_set_position);
    RUN_TEST(test_window_resize);
    RUN_TEST(test_window_set_title);

    // set_visible / set_focused tests
    RUN_TEST(test_window_set_visible);
    RUN_TEST(test_window_set_focused);

    // is_close_button_hit tests
    RUN_TEST(test_window_close_button_hit_top_left);
    RUN_TEST(test_window_close_button_hit_bottom_right);
    RUN_TEST(test_window_close_button_miss_far_away);
    RUN_TEST(test_window_close_button_miss_content);
    RUN_TEST(test_window_close_button_miss_title_bar);
    RUN_TEST(test_window_close_button_hit_offset_position);

    // contains tests
    RUN_TEST(test_window_contains_inside);
    RUN_TEST(test_window_contains_outside);
    RUN_TEST(test_window_contains_exclusive_boundary);
    RUN_TEST(test_window_contains_title_bar);

    // Canvas allocation tests
    RUN_TEST(test_window_canvas_dimensions);
    RUN_TEST(test_window_canvas_usable);

    // Pixel verification tests
    RUN_TEST(test_window_draw_content_pixels);
    RUN_TEST(test_window_draw_title_bar_pixels);
    RUN_TEST(test_window_blit_to_position);
    RUN_TEST(test_window_blit_to_invisible);

    // Multiple window tests
    RUN_TEST(test_window_multiple_independent);
    RUN_TEST(test_window_multiple_blit);
    RUN_TEST(test_window_id_sequence);

    // Edge case tests
    RUN_TEST(test_window_zero_height);
    RUN_TEST(test_window_negative_position);
    RUN_TEST(test_window_constants);

    TEST_SUMMARY();
}

#else  // !CINUX_GUI

// CLI mode stub: no GUI tests to run
extern "C" void run_window_tests() {
    using cinux::lib::kprintf;
    kprintf("[WINDOW] CLI mode -- GUI tests skipped.\n");
}

#endif  // CINUX_GUI
