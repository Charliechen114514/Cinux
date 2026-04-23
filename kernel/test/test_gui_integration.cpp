/**
 * @file kernel/test/test_gui_integration.cpp
 * @brief QEMU in-kernel integration tests for GUI subsystem wiring (030_gui_wm_basic, sub-iteration D)
 *
 * Tests the integration points introduced in sub-iteration D:
 *   - gui_init() initialises WindowManager singleton with Canvas + Font
 *   - Keyboard dual-path dispatch: events appear in both Keyboard::poll()
 *     and Mouse::event_queue() (the GUI EventQueue)
 *   - PIT tick callback logic: drain EventQueue, dispatch to WindowManager,
 *     then composite the frame
 *   - Mouse event flow: EventQueue -> WindowManager.handle_mouse()
 *
 * These tests exercise the wiring between subsystems that sub-iteration D
 * introduced, using real kernel objects (no mocks).
 *
 * Preconditions (set up by main_test.cpp):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Framebuffer available (set up by bootloader)
 *   - Heap initialised (for Canvas::init and Window allocations)
 *   - PSFFont initialised
 *
 * Compile condition: CINUX_GUI
 */

#include "big_kernel_test.h"

#include "boot/boot_info.h"
#include "kernel/drivers/video/font.hpp"
#include "kernel/drivers/video/framebuffer.hpp"

#ifdef CINUX_GUI

#include "kernel/drivers/canvas.hpp"
#include "kernel/drivers/mouse.hpp"
#include "kernel/gui/event.hpp"
#include "kernel/gui/gui_init.hpp"
#include "kernel/gui/window_manager.hpp"

using cinux::drivers::Canvas;
using cinux::drivers::Framebuffer;
using cinux::drivers::Mouse;
using cinux::drivers::PSFFont;
using cinux::gui::Event;
using cinux::gui::EventType;
using cinux::gui::WindowManager;

namespace {

Framebuffer g_fb;
PSFFont g_font;
Canvas g_screen;

}  // anonymous namespace

// ============================================================
// gui_init integration tests
// ============================================================

/// Verify gui_init stores screen and font pointers in WindowManager singleton
void test_gui_init_wm_has_screen_and_font() {
    // Use a fresh WindowManager by directly calling init on the singleton
    WindowManager::instance().init(&g_screen, &g_font);

    TEST_ASSERT_EQ(WindowManager::instance().window_count(), 0u);
    TEST_ASSERT_NULL(WindowManager::instance().focused());

    // Create a window to verify the screen/font pointers are usable
    uint32_t id = WindowManager::instance().create("Test", 100, 50);
    TEST_ASSERT_NE(id, 0u);
    TEST_ASSERT_EQ(WindowManager::instance().window_count(), 1u);
    TEST_ASSERT_NOT_NULL(WindowManager::instance().focused());

    // Clean up for subsequent tests
    WindowManager::instance().destroy(id);
}

/// Verify gui_init does not crash when called multiple times (idempotent re-init)
void test_gui_init_idempotent() {
    WindowManager::instance().init(&g_screen, &g_font);
    WindowManager::instance().create("First", 100, 50);

    // Re-init should be safe (even if it resets state)
    WindowManager::instance().init(&g_screen, &g_font);

    // After re-init, window count should be reset to 0
    TEST_ASSERT_EQ(WindowManager::instance().window_count(), 0u);
}

// ============================================================
// Keyboard dual-path dispatch tests
// ============================================================

/// Verify keyboard events are forwarded to the GUI EventQueue
///
/// Simulates the dual-path dispatch by manually enqueuing into both
/// Keyboard's queue and Mouse::event_queue(), then verifying both
/// are readable.  The actual irq1_handler does this in hardware mode.
void test_keyboard_dual_path_to_event_queue() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    // Simulate what irq1_handler does: enqueue into the GUI EventQueue
    Event gui_ev{};
    gui_ev.type_ = EventType::KeyDown;
    gui_ev.key.ascii = 'A';
    gui_ev.key.scancode = 0x1E;
    gui_ev.key.pressed = true;
    gui_ev.key.shift = false;
    gui_ev.key.ctrl = false;
    gui_ev.key.alt = false;
    eq.enqueue(gui_ev);

    // Simulate a second event (key release)
    Event gui_ev_up{};
    gui_ev_up.type_ = EventType::KeyUp;
    gui_ev_up.key.ascii = 0;
    gui_ev_up.key.scancode = 0x9E;  // break code for 'A'
    gui_ev_up.key.pressed = false;
    gui_ev_up.key.shift = false;
    gui_ev_up.key.ctrl = false;
    gui_ev_up.key.alt = false;
    eq.enqueue(gui_ev_up);

    // Verify both events are in the EventQueue
    Event out1{};
    TEST_ASSERT_TRUE(eq.dequeue(out1));
    TEST_ASSERT_EQ(static_cast<int>(out1.type_), static_cast<int>(EventType::KeyDown));
    TEST_ASSERT_EQ(out1.key.ascii, 'A');
    TEST_ASSERT_TRUE(out1.key.pressed);

    Event out2{};
    TEST_ASSERT_TRUE(eq.dequeue(out2));
    TEST_ASSERT_EQ(static_cast<int>(out2.type_), static_cast<int>(EventType::KeyUp));
    TEST_ASSERT_FALSE(out2.key.pressed);

    // Queue should now be empty
    TEST_ASSERT_TRUE(eq.empty());
}

/// Verify EventQueue correctly separates keyboard and mouse events
void test_event_queue_mixed_key_and_mouse() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    // Enqueue a key event
    Event key_ev{};
    key_ev.type_ = EventType::KeyDown;
    key_ev.key.ascii = 'B';
    key_ev.key.scancode = 0x30;
    key_ev.key.pressed = true;
    eq.enqueue(key_ev);

    // Enqueue a mouse event
    Event mouse_ev{};
    mouse_ev.type_ = EventType::MouseMove;
    mouse_ev.mouse.x = 100;
    mouse_ev.mouse.y = 200;
    mouse_ev.mouse.dx = 5;
    mouse_ev.mouse.dy = -3;
    eq.enqueue(mouse_ev);

    // Enqueue another key event
    Event key_ev2{};
    key_ev2.type_ = EventType::KeyUp;
    key_ev2.key.ascii = 0;
    key_ev2.key.scancode = 0xB0;
    key_ev2.key.pressed = false;
    eq.enqueue(key_ev2);

    // Drain and verify ordering
    Event out{};
    TEST_ASSERT_TRUE(eq.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::KeyDown));
    TEST_ASSERT_EQ(out.key.ascii, 'B');

    TEST_ASSERT_TRUE(eq.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::MouseMove));
    TEST_ASSERT_EQ(out.mouse.x, 100);

    TEST_ASSERT_TRUE(eq.dequeue(out));
    TEST_ASSERT_EQ(static_cast<int>(out.type_), static_cast<int>(EventType::KeyUp));
    TEST_ASSERT_FALSE(out.key.pressed);

    TEST_ASSERT_TRUE(eq.empty());
}

// ============================================================
// PIT tick callback simulation tests
// ============================================================

/// Simulate the gui_tick_callback logic: drain events and dispatch to WM
///
/// The real gui_tick_callback in gui_init.cpp does:
///   while (eq.dequeue(ev)) { switch(ev.type_) { dispatch to wm } }
///   wm.composite()
///
/// We replicate this logic here to verify the integration works.
void test_tick_callback_drains_and_dispatches_mouse() {
    // Ensure clean state
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    // Destroy any leftover windows
    while (wm.window_count() > 0) {
        // Find the first window's ID and destroy it
        uint32_t id = wm.focused() ? wm.focused()->id() : 0;
        if (id == 0) break;
        wm.destroy(id);
    }

    wm.init(&g_screen, &g_font);
    uint32_t win_id = wm.create("TickTest", 200, 150);
    TEST_ASSERT_NE(win_id, 0u);
    TEST_ASSERT_EQ(wm.focused()->id(), win_id);

    // Enqueue a mouse event into the EventQueue
    Event mouse_down{};
    mouse_down.type_ = EventType::MouseDown;
    mouse_down.mouse.x = 500;   // Desktop area (outside window)
    mouse_down.mouse.y = 500;
    mouse_down.mouse.left = true;
    mouse_down.mouse.right = false;
    mouse_down.mouse.middle = false;
    eq.enqueue(mouse_down);

    // Simulate the tick callback logic
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            wm.handle_key(ev);
            break;
        }
    }

    // After tick: click on desktop should clear focus
    TEST_ASSERT_NULL(wm.focused());

    // Window should still exist
    TEST_ASSERT_EQ(wm.window_count(), 1u);
}

/// Verify tick callback dispatches keyboard events to WindowManager
void test_tick_callback_dispatches_key_events() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    wm.init(&g_screen, &g_font);
    wm.create("KeyTest", 100, 50);

    // Enqueue a keyboard event
    Event key_down{};
    key_down.type_ = EventType::KeyDown;
    key_down.key.ascii = 'X';
    key_down.key.scancode = 0x2D;
    key_down.key.pressed = true;
    key_down.key.shift = true;
    eq.enqueue(key_down);

    // Simulate tick callback
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            wm.handle_key(ev);
            break;
        }
    }

    // handle_key should not crash; window should still exist
    TEST_ASSERT_EQ(wm.window_count(), 1u);
    TEST_ASSERT_NOT_NULL(wm.focused());
}

/// Verify tick callback processes multiple events in one tick
void test_tick_callback_multiple_events_per_tick() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    wm.init(&g_screen, &g_font);

    // Create two windows
    uint32_t id1 = wm.create("Win1", 100, 50);
    uint32_t id2 = wm.create("Win2", 100, 50);

    // Enqueue: click on Win1 content area (should raise it)
    // Win1 is at (0,0), content starts at y=20
    Event click_win1{};
    click_win1.type_ = EventType::MouseDown;
    click_win1.mouse.x = 10;
    click_win1.mouse.y = 30;
    click_win1.mouse.left = true;
    click_win1.mouse.right = false;
    click_win1.mouse.middle = false;
    eq.enqueue(click_win1);

    // Enqueue: a key event
    Event key_ev{};
    key_ev.type_ = EventType::KeyDown;
    key_ev.key.ascii = 'T';
    key_ev.key.scancode = 0x14;
    key_ev.key.pressed = true;
    eq.enqueue(key_ev);

    // Simulate tick callback
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            wm.handle_key(ev);
            break;
        }
    }

    // After processing: Win1 should be raised (focused)
    TEST_ASSERT_EQ(wm.focused()->id(), id1);
    TEST_ASSERT_EQ(wm.window_count(), 2u);

    // Queue should be empty after tick
    TEST_ASSERT_TRUE(eq.empty());

    // Clean up
    (void)id2;
    wm.destroy(id1);
    wm.destroy(id2);
}

/// Verify tick callback with empty queue is a no-op
void test_tick_callback_empty_queue_noop() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    wm.init(&g_screen, &g_font);
    wm.create("NoopTest", 100, 50);

    uint32_t count_before = wm.window_count();

    // Simulate tick callback with empty queue
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            wm.handle_key(ev);
            break;
        }
    }

    // Nothing should have changed
    TEST_ASSERT_EQ(wm.window_count(), count_before);
    TEST_ASSERT_TRUE(eq.empty());
}

// ============================================================
// Mouse event flow: EventQueue -> WindowManager tests
// ============================================================

/// Verify mouse move event through EventQueue to WindowManager updates mouse state
void test_mouse_event_flow_move_updates_wm_state() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    wm.init(&g_screen, &g_font);

    // Enqueue mouse move
    Event move_ev{};
    move_ev.type_ = EventType::MouseMove;
    move_ev.mouse.x = 320;
    move_ev.mouse.y = 240;
    move_ev.mouse.dx = 10;
    move_ev.mouse.dy = 5;
    move_ev.mouse.left = false;
    move_ev.mouse.right = false;
    move_ev.mouse.middle = false;
    eq.enqueue(move_ev);

    // Tick: dispatch to WM
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        default:
            break;
        }
    }

    // WM should have tracked the mouse position
    TEST_ASSERT_EQ(wm.mouse_x(), 320);
    TEST_ASSERT_EQ(wm.mouse_y(), 240);
}

/// Verify mouse drag event flow: down on title bar, move, up
void test_mouse_event_flow_drag_sequence() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    wm.init(&g_screen, &g_font);

    uint32_t win_id = wm.create("DragWin", 150, 100);
    // Window at (0, 0), title bar at y=0..19
    // Drag offset = (mouse_x - win_x, mouse_y - win_y)

    // MouseDown on title bar
    Event down_ev{};
    down_ev.type_ = EventType::MouseDown;
    down_ev.mouse.x = 50;
    down_ev.mouse.y = 5;
    down_ev.mouse.left = true;
    down_ev.mouse.right = false;
    down_ev.mouse.middle = false;
    eq.enqueue(down_ev);

    // MouseMove
    Event move_ev{};
    move_ev.type_ = EventType::MouseMove;
    move_ev.mouse.x = 200;
    move_ev.mouse.y = 100;
    move_ev.mouse.left = true;
    move_ev.mouse.right = false;
    move_ev.mouse.middle = false;
    eq.enqueue(move_ev);

    // MouseUp
    Event up_ev{};
    up_ev.type_ = EventType::MouseUp;
    up_ev.mouse.x = 200;
    up_ev.mouse.y = 100;
    up_ev.mouse.left = false;
    up_ev.mouse.right = false;
    up_ev.mouse.middle = false;
    eq.enqueue(up_ev);

    // Tick: process all events
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        default:
            break;
        }
    }

    // Window should have been dragged to new position
    // drag_offset = (50 - 0, 5 - 0) = (50, 5)
    // new pos = (200 - 50, 100 - 5) = (150, 95)
    TEST_ASSERT_NOT_NULL(wm.focused());
    TEST_ASSERT_EQ(wm.focused()->id(), win_id);
    TEST_ASSERT_EQ(wm.focused()->x(), 150);
    TEST_ASSERT_EQ(wm.focused()->y(), 95);

    // Clean up
    (void)win_id;
}

/// Verify close button event flow destroys window
void test_mouse_event_flow_close_button() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    wm.init(&g_screen, &g_font);

    wm.create("CloseWin", 100, 50);
    TEST_ASSERT_EQ(wm.window_count(), 1u);

    // Window at (0, 0), width=100.
    // Close button: x = 0 + 100 - 14 - 3 = 83, y = (20-14)/2 = 3
    Event close_ev{};
    close_ev.type_ = EventType::MouseDown;
    close_ev.mouse.x = 83;
    close_ev.mouse.y = 3;
    close_ev.mouse.left = true;
    close_ev.mouse.right = false;
    close_ev.mouse.middle = false;
    eq.enqueue(close_ev);

    // Tick: dispatch
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        default:
            break;
        }
    }

    // Window should be destroyed
    TEST_ASSERT_EQ(wm.window_count(), 0u);
    TEST_ASSERT_NULL(wm.focused());
}

// ============================================================
// Composite integration test
// ============================================================

/// Verify composite after tick callback updates framebuffer
void test_tick_callback_composite_renders() {
    auto& eq = Mouse::event_queue();
    eq.clear();

    WindowManager& wm = WindowManager::instance();
    wm.init(&g_screen, &g_font);

    wm.create("CompositeTest", 100, 50);

    // Clear framebuffer
    g_fb.clear(0);

    // Simulate tick: drain (empty) + composite
    Event ev;
    while (eq.dequeue(ev)) {
        // nothing
    }
    wm.composite();

    // Desktop should be visible at a pixel not covered by any window
    TEST_ASSERT_EQ(g_fb.get_pixel(400, 400), WindowManager::DESKTOP_COLOR);

    // Window content area should be visible at its position
    // Window at (0,0), content starts at y=20
    TEST_ASSERT_EQ(g_fb.get_pixel(50, 25), cinux::gui::Window::COLOR_CONTENT_BG);
}

// ============================================================
// Entry point
// ============================================================

extern "C" void run_gui_integration_tests() {
    TEST_SECTION("GUI Integration Tests (030_gui_wm_basic, sub-iteration D)");

    // Initialise framebuffer, font, and off-screen canvas
    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    g_font.init();
    g_fb.clear(0);
    g_screen.init(g_fb);

    // gui_init integration
    RUN_TEST(test_gui_init_wm_has_screen_and_font);
    RUN_TEST(test_gui_init_idempotent);

    // Keyboard dual-path dispatch
    RUN_TEST(test_keyboard_dual_path_to_event_queue);
    RUN_TEST(test_event_queue_mixed_key_and_mouse);

    // PIT tick callback simulation
    RUN_TEST(test_tick_callback_drains_and_dispatches_mouse);
    RUN_TEST(test_tick_callback_dispatches_key_events);
    RUN_TEST(test_tick_callback_multiple_events_per_tick);
    RUN_TEST(test_tick_callback_empty_queue_noop);

    // Mouse event flow
    RUN_TEST(test_mouse_event_flow_move_updates_wm_state);
    RUN_TEST(test_mouse_event_flow_drag_sequence);
    RUN_TEST(test_mouse_event_flow_close_button);

    // Composite integration
    RUN_TEST(test_tick_callback_composite_renders);

    TEST_SUMMARY();
}

#else  // !CINUX_GUI

// CLI mode stub: no GUI integration tests to run
extern "C" void run_gui_integration_tests() {
    using cinux::lib::kprintf;
    kprintf("[GUI_INTEGRATION] CLI mode -- GUI integration tests skipped.\n");
}

#endif  // CINUX_GUI
