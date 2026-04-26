/**
 * @file kernel/test/test_desktop.cpp
 * @brief QEMU in-kernel tests for desktop icon + window manager integration (033_gui_desktop)
 *
 * Tests the full desktop icon lifecycle through the WindowManager:
 *   - WM initialisation with screen canvas and font
 *   - add_desktop_icon: registration, capacity boundary, duplicate icons
 *   - hit_test_icon: direct hit, miss, Z-priority (later icons win), overlap
 *   - consume_pending_icon_action: click-through handle_mouse, reset on consume
 *   - composite: icons rendered behind windows, no crash with zero or many icons
 *   - handle_mouse: icon click vs window click vs desktop click disambiguation
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

#include <stdint.h>

#include "big_kernel_test.h"
#include "boot/boot_info.h"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"

#ifdef CINUX_GUI

#    include "kernel/drivers/canvas.hpp"
#    include "kernel/gui/desktop_icon.hpp"
#    include "kernel/gui/event.hpp"
#    include "kernel/gui/window.hpp"
#    include "kernel/gui/window_manager.hpp"

using cinux::drivers::Canvas;
using cinux::drivers::Framebuffer;
using cinux::drivers::PSFFont;
using cinux::gui::DesktopIcon;
using cinux::gui::Event;
using cinux::gui::EventType;
using cinux::gui::IconAction;
using cinux::gui::Window;
using cinux::gui::WindowManager;

namespace {

Framebuffer g_fb;
PSFFont     g_font;
Canvas      g_screen;

/// Icon size constant (mirrors cinux::gui::icons::ICON_SIZE)
constexpr uint32_t ICON_SIZE   = 32;
constexpr uint32_t ICON_PIXELS = ICON_SIZE * ICON_SIZE;

/// Solid-colour pixel buffer for test icons
uint32_t g_icon_shell[ICON_PIXELS];
uint32_t g_icon_calc[ICON_PIXELS];

/// Fill a pixel buffer with a solid colour (all opaque).
void fill_icon_solid(uint32_t pixels[ICON_PIXELS], uint32_t colour) {
    for (uint32_t i = 0; i < ICON_PIXELS; i++) {
        pixels[i] = colour;
    }
}

/// Build a DesktopIcon for the "Shell" shortcut at the given position.
DesktopIcon make_shell_icon(int32_t x, int32_t y) {
    return DesktopIcon{
        .x      = x,
        .y      = y,
        .bitmap = g_icon_shell,
        .label  = "Shell",
        .width  = ICON_SIZE,
        .height = ICON_SIZE,
        .action = IconAction::OpenShell,
    };
}

/// Build a DesktopIcon for the "Calculator" shortcut at the given position.
DesktopIcon make_calc_icon(int32_t x, int32_t y) {
    return DesktopIcon{
        .x      = x,
        .y      = y,
        .bitmap = g_icon_calc,
        .label  = "Calc",
        .width  = ICON_SIZE,
        .height = ICON_SIZE,
        .action = IconAction::OpenCalculator,
    };
}

}  // anonymous namespace

// ============================================================
// WM init + add_desktop_icon basic tests
// ============================================================

/// Verify WM init succeeds and accepts desktop icon registration
void test_desktop_init_and_add_icon() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    TEST_ASSERT_EQ(wm.window_count(), 0u);

    bool ok = wm.add_desktop_icon(make_shell_icon(10, 10));
    TEST_ASSERT_TRUE(ok);

    // hit_test_icon should find the icon we just added
    const DesktopIcon* hit = wm.hit_test_icon(20, 20);
    TEST_ASSERT_NOT_NULL(hit);
    TEST_ASSERT_EQ(hit->action, IconAction::OpenShell);
}

/// Verify add_desktop_icon returns false when capacity is reached
void test_desktop_icon_capacity_limit() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    for (uint32_t i = 0; i < WindowManager::MAX_ICONS; i++) {
        bool ok = wm.add_desktop_icon(make_shell_icon(static_cast<int32_t>(i * 40), 10));
        TEST_ASSERT_TRUE(ok);
    }

    // The next one should fail
    bool overflow = wm.add_desktop_icon(make_shell_icon(0, 0));
    TEST_ASSERT_FALSE(overflow);
}

/// Verify adding multiple icons at different positions
void test_desktop_add_multiple_icons() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.add_desktop_icon(make_shell_icon(10, 10));
    wm.add_desktop_icon(make_calc_icon(60, 10));

    // Hit inside first icon
    const DesktopIcon* hit1 = wm.hit_test_icon(20, 20);
    TEST_ASSERT_NOT_NULL(hit1);
    TEST_ASSERT_EQ(hit1->action, IconAction::OpenShell);

    // Hit inside second icon
    const DesktopIcon* hit2 = wm.hit_test_icon(70, 20);
    TEST_ASSERT_NOT_NULL(hit2);
    TEST_ASSERT_EQ(hit2->action, IconAction::OpenCalculator);

    // Hit outside both
    const DesktopIcon* miss = wm.hit_test_icon(200, 200);
    TEST_ASSERT_NULL(miss);
}

// ============================================================
// hit_test_icon tests
// ============================================================

/// Verify hit_test_icon returns nullptr when no icons are registered
void test_desktop_hit_test_empty() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    TEST_ASSERT_NULL(wm.hit_test_icon(0, 0));
    TEST_ASSERT_NULL(wm.hit_test_icon(100, 100));
}

/// Verify hit_test_icon on icon boundaries (edge cases)
void test_desktop_hit_test_boundaries() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.add_desktop_icon(make_shell_icon(10, 20));

    // Top-left corner (inside)
    TEST_ASSERT_NOT_NULL(wm.hit_test_icon(10, 20));
    // Last pixel (inside): x + width - 1, y + height - 1
    TEST_ASSERT_NOT_NULL(wm.hit_test_icon(41, 51));
    // One pixel outside (right)
    TEST_ASSERT_NULL(wm.hit_test_icon(42, 20));
    // One pixel outside (bottom)
    TEST_ASSERT_NULL(wm.hit_test_icon(10, 52));
    // One pixel outside (left)
    TEST_ASSERT_NULL(wm.hit_test_icon(9, 20));
    // One pixel outside (top)
    TEST_ASSERT_NULL(wm.hit_test_icon(10, 19));
}

/// Verify later-registered icon takes priority on overlap
void test_desktop_hit_test_z_priority() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    // Place shell at (0, 0), calc at (10, 10) -- they overlap in (10,10)-(31,31)
    wm.add_desktop_icon(make_shell_icon(0, 0));
    wm.add_desktop_icon(make_calc_icon(10, 10));

    // In the overlap region, calc (later) should win
    const DesktopIcon* hit = wm.hit_test_icon(15, 15);
    TEST_ASSERT_NOT_NULL(hit);
    TEST_ASSERT_EQ(hit->action, IconAction::OpenCalculator);

    // In the shell-only region (0,0)-(9,9), shell should win
    const DesktopIcon* hit_shell = wm.hit_test_icon(5, 5);
    TEST_ASSERT_NOT_NULL(hit_shell);
    TEST_ASSERT_EQ(hit_shell->action, IconAction::OpenShell);
}

// ============================================================
// consume_pending_icon_action tests
// ============================================================

/// Verify consume_pending_icon_action returns None when no click happened
void test_desktop_consume_no_pending() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    TEST_ASSERT_EQ(static_cast<uint8_t>(wm.consume_pending_icon_action()),
                   static_cast<uint8_t>(IconAction::None));
}

/// Verify clicking an icon sets pending action, consume returns it, then resets
void test_desktop_click_sets_and_consumes_action() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.add_desktop_icon(make_shell_icon(10, 10));

    // Simulate mouse click on the icon
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 20;
    ev.mouse.y      = 20;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;

    g_fb.clear(0);
    wm.handle_mouse(ev);

    // consume should return OpenShell
    IconAction action = wm.consume_pending_icon_action();
    TEST_ASSERT_EQ(static_cast<uint8_t>(action), static_cast<uint8_t>(IconAction::OpenShell));

    // Second consume should return None (already consumed)
    IconAction action2 = wm.consume_pending_icon_action();
    TEST_ASSERT_EQ(static_cast<uint8_t>(action2), static_cast<uint8_t>(IconAction::None));
}

/// Verify clicking desktop (no icon, no window) clears focus, no icon action
void test_desktop_click_no_icon_no_action() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    // Create a window so focus is set
    wm.create("A", 100, 50);
    TEST_ASSERT_NOT_NULL(wm.focused());

    // Click on desktop (far from icon)
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 500;
    ev.mouse.y      = 500;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;

    wm.handle_mouse(ev);

    // Focus should be cleared
    TEST_ASSERT_NULL(wm.focused());

    // No icon action
    TEST_ASSERT_EQ(static_cast<uint8_t>(wm.consume_pending_icon_action()),
                   static_cast<uint8_t>(IconAction::None));
}

/// Verify clicking a window (not on icon) does not set icon action
void test_desktop_click_window_no_icon_action() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.add_desktop_icon(make_shell_icon(200, 200));

    // Create a window at (0,0), size 100x50
    wm.create("A", 100, 50);

    // Click on the window's content area (not on icon)
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 50;
    ev.mouse.y      = 30;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;

    g_fb.clear(0);
    wm.handle_mouse(ev);

    // No icon action should be pending (click was on the window)
    TEST_ASSERT_EQ(static_cast<uint8_t>(wm.consume_pending_icon_action()),
                   static_cast<uint8_t>(IconAction::None));
}

// ============================================================
// composite with desktop icons tests
// ============================================================

/// Verify composite with icons and no windows does not crash
void test_desktop_composite_icons_only() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.add_desktop_icon(make_shell_icon(10, 10));
    wm.add_desktop_icon(make_calc_icon(60, 10));

    g_fb.clear(0);
    wm.composite();

    // Desktop colour at a pixel far from icons
    TEST_ASSERT_EQ(g_fb.get_pixel(400, 400), WindowManager::DESKTOP_COLOR);
}

/// Verify composite with icons and windows does not crash
void test_desktop_composite_icons_and_windows() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.add_desktop_icon(make_shell_icon(10, 10));
    wm.create("A", 100, 50);

    g_fb.clear(0);
    wm.composite();

    // Desktop colour at a pixel not covered by icon or window
    TEST_ASSERT_EQ(g_fb.get_pixel(400, 400), WindowManager::DESKTOP_COLOR);
}

/// Verify composite with no icons and no windows does not crash
void test_desktop_composite_empty() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    g_fb.clear(0);
    wm.composite();

    // Desktop colour (cursor may overlap at (0,0), so check far away)
    TEST_ASSERT_EQ(g_fb.get_pixel(400, 300), WindowManager::DESKTOP_COLOR);
}

/// Verify icons are rendered behind windows (icon pixel overwritten by window)
void test_desktop_composite_icons_behind_windows() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    // Place an icon at (0, 0)
    wm.add_desktop_icon(make_shell_icon(0, 0));

    // Create a window at (0, 0) that overlaps the icon
    // Stagger offset for first window: (0, 0)
    wm.create("A", 100, 50);

    g_fb.clear(0);
    wm.composite();

    // At (5, 25): inside window content area (y = 0 + 20 = 20, so y=25 is content)
    // Window content should overwrite icon pixels
    TEST_ASSERT_EQ(g_fb.get_pixel(5, 25), Window::COLOR_CONTENT_BG);
}

// ============================================================
// Full desktop scenario: icons + windows + click disambiguation
// ============================================================

/// Verify a full desktop scenario: icons, windows, icon click, window raise
void test_desktop_full_scenario() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    // Add two desktop icons below the window area so they are not occluded.
    // Window A at (0,0) covers y=0..69; Window B at (30,30) covers y=30..99.
    // Icons at y=200 are safely outside all windows.
    wm.add_desktop_icon(make_shell_icon(10, 200));
    wm.add_desktop_icon(make_calc_icon(60, 200));

    // Create two windows
    uint32_t id1 = wm.create("A", 100, 50);
    uint32_t id2 = wm.create("B", 100, 50);

    (void)id2;

    // Window B is on top (created last)
    TEST_ASSERT_EQ(wm.focused()->id(), id2);

    // Click on shell icon (should set pending action, not raise window)
    Event icon_click{};
    icon_click.type_        = EventType::MouseDown;
    icon_click.mouse.x      = 20;
    icon_click.mouse.y      = 210;
    icon_click.mouse.left   = true;
    icon_click.mouse.right  = false;
    icon_click.mouse.middle = false;

    g_fb.clear(0);
    wm.handle_mouse(icon_click);

    IconAction action = wm.consume_pending_icon_action();
    TEST_ASSERT_EQ(static_cast<uint8_t>(action), static_cast<uint8_t>(IconAction::OpenShell));

    // Icon click goes through the "desktop click" path in handle_mouse,
    // which clears focus.
    TEST_ASSERT_NULL(wm.focused());

    // Click on window A title bar to raise it
    // Window A at (0,0), B at (30,30). Click (10,10) is A-only territory.
    Event win_click{};
    win_click.type_        = EventType::MouseDown;
    win_click.mouse.x      = 10;
    win_click.mouse.y      = 10;
    win_click.mouse.left   = true;
    win_click.mouse.right  = false;
    win_click.mouse.middle = false;

    wm.handle_mouse(win_click);

    TEST_ASSERT_NOT_NULL(wm.focused());
    TEST_ASSERT_EQ(wm.focused()->id(), id1);

    // Composite should not crash
    g_fb.clear(0);
    wm.composite();

    TEST_ASSERT_EQ(g_fb.get_pixel(400, 400), WindowManager::DESKTOP_COLOR);
}

/// Verify init resets icon state (idempotent re-init)
void test_desktop_init_resets_icons() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    wm.add_desktop_icon(make_shell_icon(10, 10));
    TEST_ASSERT_NOT_NULL(wm.hit_test_icon(20, 20));

    // Re-init should clear icon state
    wm.init(&g_screen, &g_font);

    // No icons should be registered after re-init
    TEST_ASSERT_NULL(wm.hit_test_icon(20, 20));
}

/// Verify hit_test_icon with zero-size icon does not match any point
void test_desktop_hit_test_zero_size_icon() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);

    DesktopIcon tiny{
        .x      = 100,
        .y      = 100,
        .bitmap = g_icon_shell,
        .label  = "Tiny",
        .width  = 0,
        .height = 0,
        .action = IconAction::None,
    };

    wm.add_desktop_icon(tiny);

    // Zero-size icon should never hit
    TEST_ASSERT_NULL(wm.hit_test_icon(100, 100));
    TEST_ASSERT_NULL(wm.hit_test_icon(99, 99));
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_desktop_tests() {
    TEST_SECTION("Desktop Tests (033_gui_desktop)");

    // Initialise framebuffer, font, and off-screen canvas
    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto*                      bi             = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    g_font.init();
    g_fb.clear(0);
    g_screen.init(g_fb);

    // Prepare solid-colour icon pixel buffers
    fill_icon_solid(g_icon_shell, 0x00FFFFFF);  // White shell icon
    fill_icon_solid(g_icon_calc, 0x00FF8C00);   // Orange calc icon

    // WM init + add_desktop_icon
    RUN_TEST(test_desktop_init_and_add_icon);
    RUN_TEST(test_desktop_icon_capacity_limit);
    RUN_TEST(test_desktop_add_multiple_icons);

    // hit_test_icon
    RUN_TEST(test_desktop_hit_test_empty);
    RUN_TEST(test_desktop_hit_test_boundaries);
    RUN_TEST(test_desktop_hit_test_z_priority);

    // consume_pending_icon_action
    RUN_TEST(test_desktop_consume_no_pending);
    RUN_TEST(test_desktop_click_sets_and_consumes_action);
    RUN_TEST(test_desktop_click_no_icon_no_action);
    RUN_TEST(test_desktop_click_window_no_icon_action);

    // composite with icons
    RUN_TEST(test_desktop_composite_icons_only);
    RUN_TEST(test_desktop_composite_icons_and_windows);
    RUN_TEST(test_desktop_composite_empty);
    RUN_TEST(test_desktop_composite_icons_behind_windows);

    // Full scenario
    RUN_TEST(test_desktop_full_scenario);
    RUN_TEST(test_desktop_init_resets_icons);
    RUN_TEST(test_desktop_hit_test_zero_size_icon);

    TEST_SUMMARY();
}

#else  // !CINUX_GUI

// CLI mode stub: no GUI tests to run
extern "C" void run_desktop_tests() {
    using cinux::lib::kprintf;
    kprintf("[DESKTOP] CLI mode -- GUI tests skipped.\n");
}

#endif  // CINUX_GUI
