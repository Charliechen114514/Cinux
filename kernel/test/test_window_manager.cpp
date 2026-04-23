/**
 * @file kernel/test/test_window_manager.cpp
 * @brief QEMU in-kernel tests for WindowManager class (030_gui_wm_basic, sub-iteration C)
 *
 * Tests the real kernel WindowManager class with off-screen Canvas and
 * Framebuffer.  Uses Framebuffer::get_pixel() to verify rendered output
 * after composite, and verifies create/destroy/raise/handle_mouse semantics.
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

#include "kernel/drivers/canvas.hpp"
#include "kernel/gui/event.hpp"
#include "kernel/gui/window.hpp"
#include "kernel/gui/window_manager.hpp"

using cinux::drivers::Canvas;
using cinux::drivers::Framebuffer;
using cinux::drivers::PSFFont;
using cinux::gui::Event;
using cinux::gui::EventType;
using cinux::gui::Window;
using cinux::gui::WindowManager;

namespace {

Framebuffer g_fb;
PSFFont g_font;
Canvas g_screen;

}  // anonymous namespace

// ============================================================
// init / create / destroy / raise basic tests
// ============================================================

/// Verify init accepts screen canvas pointer
void test_wm_init_stores_pointers() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    TEST_ASSERT_EQ(wm.window_count(), 0u);
    TEST_ASSERT_NULL(wm.focused());
}

/// Verify create returns incrementing IDs
void test_wm_create_increments_ids() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id1 = wm.create("A", 100, 50);
    uint32_t id2 = wm.create("B", 100, 50);
    uint32_t id3 = wm.create("C", 100, 50);

    TEST_ASSERT_NE(id1, 0u);
    TEST_ASSERT_NE(id2, 0u);
    TEST_ASSERT_NE(id3, 0u);
    TEST_ASSERT_LT(id1, id2);
    TEST_ASSERT_LT(id2, id3);
}

/// Verify create increments window count
void test_wm_create_increments_count() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    TEST_ASSERT_EQ(wm.window_count(), 0u);
    wm.create("A", 100, 50);
    TEST_ASSERT_EQ(wm.window_count(), 1u);
    wm.create("B", 100, 50);
    TEST_ASSERT_EQ(wm.window_count(), 2u);
}

/// Verify create sets focus on new window
void test_wm_create_sets_focus() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    TEST_ASSERT_NULL(wm.focused());

    uint32_t id1 = wm.create("A", 100, 50);
    TEST_ASSERT_NOT_NULL(wm.focused());
    TEST_ASSERT_EQ(wm.focused()->id(), id1);

    uint32_t id2 = wm.create("B", 100, 50);
    TEST_ASSERT_NOT_NULL(wm.focused());
    TEST_ASSERT_EQ(wm.focused()->id(), id2);
}

// ============================================================
// destroy tests
// ============================================================

/// Verify destroy removes window and decrements count
void test_wm_destroy_removes_window() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id1 = wm.create("A", 100, 50);
    uint32_t id2 = wm.create("B", 100, 50);

    (void)id1;

    TEST_ASSERT_EQ(wm.window_count(), 2u);
    wm.destroy(id2);
    TEST_ASSERT_EQ(wm.window_count(), 1u);
}

/// Verify destroying focused window transfers focus
void test_wm_destroy_focused_transfers_focus() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id1 = wm.create("A", 100, 50);
    uint32_t id2 = wm.create("B", 100, 50);

    TEST_ASSERT_EQ(wm.focused()->id(), id2);

    wm.destroy(id2);
    TEST_ASSERT_EQ(wm.window_count(), 1u);
    TEST_ASSERT_NOT_NULL(wm.focused());
    TEST_ASSERT_EQ(wm.focused()->id(), id1);
}

/// Verify destroy non-existent ID does not crash
void test_wm_destroy_nonexistent_no_crash() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.create("A", 100, 50);
    wm.destroy(999);
    TEST_ASSERT_EQ(wm.window_count(), 1u);
}

/// Verify destroy all windows leaves empty state
void test_wm_destroy_all_leaves_empty() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id1 = wm.create("A", 100, 50);
    uint32_t id2 = wm.create("B", 100, 50);

    wm.destroy(id1);
    wm.destroy(id2);

    TEST_ASSERT_EQ(wm.window_count(), 0u);
    TEST_ASSERT_NULL(wm.focused());
}

// ============================================================
// raise tests
// ============================================================

/// Verify raise moves window to top of Z-order
void test_wm_raise_moves_to_top() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id1 = wm.create("A", 100, 50);
    uint32_t id2 = wm.create("B", 100, 50);
    uint32_t id3 = wm.create("C", 100, 50);

    (void)id2;
    (void)id3;

    wm.raise(id1);

    TEST_ASSERT_NOT_NULL(wm.focused());
    TEST_ASSERT_EQ(wm.focused()->id(), id1);
}

/// Verify raise non-existent ID does not crash
void test_wm_raise_nonexistent_no_crash() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.create("A", 100, 50);
    wm.raise(999);
    TEST_ASSERT_EQ(wm.window_count(), 1u);
}

/// Verify raise already-top window is no-op
void test_wm_raise_already_top_noop() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id1 = wm.create("A", 100, 50);
    uint32_t id2 = wm.create("B", 100, 50);

    (void)id1;

    TEST_ASSERT_EQ(wm.focused()->id(), id2);
    wm.raise(id2);
    TEST_ASSERT_EQ(wm.focused()->id(), id2);
    TEST_ASSERT_EQ(wm.window_count(), 2u);
}

// ============================================================
// composite pixel verification tests
// ============================================================

/// Verify composite with multiple windows renders in Z-order
void test_wm_composite_multiple_windows() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    // Window A at stagger (0, 0), size 100x50
    // Window B at stagger (30, 30), size 100x50
    // They overlap at (30,30)-(99,69)
    wm.create("A", 100, 50);
    wm.create("B", 100, 50);

    g_fb.clear(0);
    wm.composite();

    // Window B is on top. At (50, 40) -- B's title bar.
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 40), Window::COLOR_TITLE_BG);

    // Desktop colour at a pixel not covered by any window
    TEST_ASSERT_EQ(g_fb.get_pixel(400, 400), WindowManager::DESKTOP_COLOR);
}

/// Verify composite after raise updates Z-order in rendered output
void test_wm_composite_after_raise() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id_a = wm.create("A", 100, 50);
    wm.create("B", 100, 50);

    g_fb.clear(0);

    // Raise A to top
    wm.raise(id_a);
    wm.composite();

    // Now A is on top. At (50, 40):
    // A's content area starts at y=20. (50, 40) is in A's content area.
    // B's title bar was at y=30..49, but A overwrites it.
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 40), Window::COLOR_CONTENT_BG);
}

/// Verify composite after destroy removes window from output
void test_wm_composite_after_destroy() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id_a = wm.create("A", 100, 50);
    wm.create("B", 100, 50);

    // Destroy A
    wm.destroy(id_a);

    g_fb.clear(0);
    wm.composite();

    // A was at (0,0). At (0, 20): A's content area.
    // After destroy, should be desktop colour.
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 20), WindowManager::DESKTOP_COLOR);

    // B at (30, 30) should still be visible.
    // B's content starts at y = 30 + 20 = 50.
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 50), Window::COLOR_CONTENT_BG);
}

/// Verify composite with no windows just clears screen (with cursor overlay)
void test_wm_composite_no_windows() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    g_fb.clear(0);
    wm.composite();

    // Cursor is drawn at Mouse::x()/Mouse::y() (default 0,0),
    // so (0,0) will be white.  Check pixels outside the 16x16 cursor area.
    TEST_ASSERT_EQ(g_fb.get_pixel(400, 300), WindowManager::DESKTOP_COLOR);
    TEST_ASSERT_EQ(g_fb.get_pixel(200, 100), WindowManager::DESKTOP_COLOR);
}

// ============================================================
// Drag pixel verification tests
// ============================================================

/// Verify dragging a window updates its position in composite output
void test_wm_drag_updates_composite_position() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.create("A", 100, 50);

    // Start drag on title bar at (50, 5), window at (0, 0)
    Event ev_down;
    ev_down.type_ = EventType::MouseDown;
    ev_down.mouse.x = 50;
    ev_down.mouse.y = 5;
    ev_down.mouse.left = true;
    ev_down.mouse.right = false;
    ev_down.mouse.middle = false;

    wm.handle_mouse(ev_down);

    // Drag to (200, 100)
    // drag_offset = (50 - 0, 5 - 0) = (50, 5)
    // new position = (200 - 50, 100 - 5) = (150, 95)
    Event ev_move;
    ev_move.type_ = EventType::MouseMove;
    ev_move.mouse.x = 200;
    ev_move.mouse.y = 100;
    ev_move.mouse.left = true;
    ev_move.mouse.right = false;
    ev_move.mouse.middle = false;

    g_fb.clear(0);
    wm.handle_mouse(ev_move);

    // Content area should now start at y = 95 + 20 = 115
    TEST_ASSERT_EQ(g_fb.get_pixel(150, 115), Window::COLOR_CONTENT_BG);

    // Old position should be desktop colour
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 20), WindowManager::DESKTOP_COLOR);

    // Release mouse
    Event ev_up;
    ev_up.type_ = EventType::MouseUp;
    ev_up.mouse.x = 200;
    ev_up.mouse.y = 100;
    ev_up.mouse.left = false;
    ev_up.mouse.right = false;
    ev_up.mouse.middle = false;

    wm.handle_mouse(ev_up);
}

// ============================================================
// Close button pixel verification test
// ============================================================

/// Verify clicking close button destroys window and re-composites
void test_wm_close_button_destroys_and_composites() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id_a = wm.create("A", 100, 50);
    wm.create("B", 100, 50);

    (void)id_a;

    TEST_ASSERT_EQ(wm.window_count(), 2u);

    // Window A at (0, 0), width=100.
    // Close button at x = 0 + 100 - 14 - 3 = 83, y = 0 + (20-14)/2 = 3
    Event ev;
    ev.type_ = EventType::MouseDown;
    ev.mouse.x = 83;
    ev.mouse.y = 3;
    ev.mouse.left = true;
    ev.mouse.right = false;
    ev.mouse.middle = false;

    g_fb.clear(0);
    wm.handle_mouse(ev);

    // Window A should be destroyed
    TEST_ASSERT_EQ(wm.window_count(), 1u);

    // After composite (called by handle_mouse), A's old position should be desktop
    TEST_ASSERT_EQ(g_fb.get_pixel(0, 20), WindowManager::DESKTOP_COLOR);

    // B should still be visible at (30, 30)
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 50), Window::COLOR_CONTENT_BG);
}

// ============================================================
// Content area click raise test
// ============================================================

/// Verify clicking content area raises window (visible via composite)
void test_wm_content_click_raises_and_composites() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id_a = wm.create("A", 100, 50);
    uint32_t id_b = wm.create("B", 100, 50);

    // A at (0,0), B at (30,30). B is on top.
    TEST_ASSERT_EQ(wm.focused()->id(), id_b);

    // Click on A's content area at (10, 50) -- A's content starts at y=20
    Event ev;
    ev.type_ = EventType::MouseDown;
    ev.mouse.x = 10;
    ev.mouse.y = 50;
    ev.mouse.left = true;
    ev.mouse.right = false;
    ev.mouse.middle = false;

    g_fb.clear(0);
    wm.handle_mouse(ev);

    // A should be raised to top and focused
    TEST_ASSERT_EQ(wm.focused()->id(), id_a);

    // After composite, at overlap point (50, 40):
    // A is now on top, A's content area (grey) overwrites B's title bar (blue)
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 40), Window::COLOR_CONTENT_BG);
}

// ============================================================
// Desktop click clears focus test
// ============================================================

/// Verify clicking desktop clears focus
void test_wm_desktop_click_clears_focus() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.create("A", 100, 50);
    TEST_ASSERT_NOT_NULL(wm.focused());

    Event ev;
    ev.type_ = EventType::MouseDown;
    ev.mouse.x = 500;
    ev.mouse.y = 500;
    ev.mouse.left = true;
    ev.mouse.right = false;
    ev.mouse.middle = false;

    wm.handle_mouse(ev);

    TEST_ASSERT_NULL(wm.focused());
}

// ============================================================
// handle_key test
// ============================================================

/// Verify handle_key does not crash
void test_wm_handle_key_no_crash() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.create("A", 100, 50);

    Event ev;
    ev.type_ = EventType::KeyDown;
    ev.key.ascii = 'a';
    ev.key.scancode = 0x1E;
    ev.key.pressed = true;
    ev.key.shift = false;
    ev.key.ctrl = false;
    ev.key.alt = false;

    wm.handle_key(ev);

    TEST_ASSERT_EQ(wm.window_count(), 1u);
}

// ============================================================
// Z-order consistency tests
// ============================================================

/// Verify Z-order after multiple raises followed by composite
void test_wm_zorder_multiple_raises_composite() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id_a = wm.create("A", 100, 50);
    uint32_t id_b = wm.create("B", 100, 50);
    uint32_t id_c = wm.create("C", 100, 50);

    // Raise A to top
    wm.raise(id_a);
    g_fb.clear(0);
    wm.composite();

    // At overlap (50, 40): A on top, A's content grey
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 40), Window::COLOR_CONTENT_BG);

    // Raise B to top
    wm.raise(id_b);
    g_fb.clear(0);
    wm.composite();

    // At overlap (50, 40): B on top, B's title bar blue (y=30..49)
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 40), Window::COLOR_TITLE_BG);

    // All windows still exist
    TEST_ASSERT_EQ(wm.window_count(), 3u);
    TEST_ASSERT_EQ(wm.focused()->id(), id_b);

    (void)id_c;
}

/// Verify Z-order after destroy and recreate
void test_wm_zorder_destroy_recreate() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    uint32_t id_a = wm.create("A", 100, 50);
    uint32_t id_b = wm.create("B", 100, 50);

    (void)id_a;

    // Destroy B (top), A remains
    wm.destroy(id_b);
    TEST_ASSERT_EQ(wm.window_count(), 1u);

    // Create C -- should be on top
    uint32_t id_c = wm.create("C", 100, 50);
    TEST_ASSERT_EQ(wm.window_count(), 2u);
    TEST_ASSERT_EQ(wm.focused()->id(), id_c);

    g_fb.clear(0);
    wm.composite();

    // C at stagger (30, 30). C's title bar at y=30..49.
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 40), Window::COLOR_TITLE_BG);
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_window_manager_tests() {
    TEST_SECTION("WindowManager Tests (030_gui_wm_basic, sub-iteration C)");

    // Initialise framebuffer, font, and off-screen canvas for pixel tests
    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    g_font.init();
    g_fb.clear(0);

    // Screen canvas uses the real framebuffer for pixel verification
    g_screen.init(g_fb);

    // init / create tests
    RUN_TEST(test_wm_init_stores_pointers);
    RUN_TEST(test_wm_create_increments_ids);
    RUN_TEST(test_wm_create_increments_count);
    RUN_TEST(test_wm_create_sets_focus);

    // destroy tests
    RUN_TEST(test_wm_destroy_removes_window);
    RUN_TEST(test_wm_destroy_focused_transfers_focus);
    RUN_TEST(test_wm_destroy_nonexistent_no_crash);
    RUN_TEST(test_wm_destroy_all_leaves_empty);

    // raise tests
    RUN_TEST(test_wm_raise_moves_to_top);
    RUN_TEST(test_wm_raise_nonexistent_no_crash);
    RUN_TEST(test_wm_raise_already_top_noop);

    // composite pixel verification
    RUN_TEST(test_wm_composite_multiple_windows);
    RUN_TEST(test_wm_composite_after_raise);
    RUN_TEST(test_wm_composite_after_destroy);
    RUN_TEST(test_wm_composite_no_windows);

    // drag pixel verification
    RUN_TEST(test_wm_drag_updates_composite_position);

    // close button pixel verification
    RUN_TEST(test_wm_close_button_destroys_and_composites);

    // content area click raise
    RUN_TEST(test_wm_content_click_raises_and_composites);

    // desktop click
    RUN_TEST(test_wm_desktop_click_clears_focus);

    // handle_key
    RUN_TEST(test_wm_handle_key_no_crash);

    // Z-order consistency
    RUN_TEST(test_wm_zorder_multiple_raises_composite);
    RUN_TEST(test_wm_zorder_destroy_recreate);

    TEST_SUMMARY();
}

#else  // !CINUX_GUI

// CLI mode stub: no GUI tests to run
extern "C" void run_window_manager_tests() {
    using cinux::lib::kprintf;
    kprintf("[WINDOW_MANAGER] CLI mode -- GUI tests skipped.\n");
}

#endif  // CINUX_GUI
