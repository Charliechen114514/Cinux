/**
 * @file kernel/gui/gui_init.cpp
 * @brief GUI subsystem initialisation implementation
 *
 * Encapsulates all GUI setup (mouse driver, window manager, PIT
 * tick callback) behind the gui_init() / gui_start() interface so
 * that kernel_main and kernel_init_thread remain GUI-agnostic.
 */

#include "gui_init.hpp"

#include "kernel/drivers/canvas.hpp"
#include "kernel/drivers/mouse.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/video/font.hpp"
#include "kernel/gui/event.hpp"
#include "kernel/gui/terminal.hpp"
#include "kernel/gui/window_manager.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::gui {

// ============================================================
// Module-internal state
// ============================================================

namespace {
cinux::drivers::Canvas*	 g_screen = nullptr;
cinux::drivers::PSFFont* g_font	  = nullptr;
}  // anonymous namespace

// ============================================================
// gui_init() -- one-time GUI setup from kernel_main
// ============================================================

void gui_init(cinux::drivers::Canvas& screen, cinux::drivers::PSFFont& font) {
	cinux::lib::kprintf("[GUI] Initialising GUI subsystem...\n");

	// Store pointers for later use in gui_start()
	g_screen = &screen;
	g_font	 = &font;

	// Initialise the window manager with the screen canvas and font
	WindowManager::instance().init(&screen, &font);

	// Draw the GUI demo: dark background + random-coloured rectangles + title
	screen.clear(0x001A1A2E);
	uint32_t rng_state = 12345;
	auto	 lcg_next  = [&rng_state]() {
		rng_state = rng_state * 1103515245u + 12345u;
		return (rng_state >> 16) & 0x7FFF;
	};
	for (int i = 0; i < 10; i++) {
		uint32_t x	   = lcg_next() % (screen.width() - 100);
		uint32_t y	   = lcg_next() % (screen.height() - 60);
		uint32_t w	   = 40 + (lcg_next() % 120);
		uint32_t h	   = 30 + (lcg_next() % 80);
		uint32_t r	   = 0x40 + (lcg_next() % 0xC0);
		uint32_t g	   = 0x40 + (lcg_next() % 0xC0);
		uint32_t b	   = 0x40 + (lcg_next() % 0xC0);
		uint32_t color = (r << 16) | (g << 8) | b;
		screen.draw_rect(x, y, w, h, color);
	}
	const char* title  = "Cinux GUI";
	uint32_t	text_w = 9 * font.width();
	uint32_t	text_x = (screen.width() - text_w) / 2;
	screen.draw_text(text_x, 10, title, 0x00FFFFFF, font);
	screen.flip();

	cinux::lib::kprintf("[GUI] Demo rendered to framebuffer.\n");
}

// ============================================================
// PIT tick callback: process events + composite
// ============================================================

namespace {

/**
 * @brief Called on every PIT tick to drain input and refresh the screen
 *
 * @param ctx  Unused context pointer
 */
void gui_tick_callback(void* /*ctx*/) {
	using cinux::drivers::Mouse;
	using cinux::gui::Event;
	using cinux::gui::EventType;

	static uint32_t tick_count	 = 0;
	static uint32_t total_events = 0;
	static uint32_t mouse_events = 0;
	tick_count++;

	auto& wm = WindowManager::instance();
	auto& eq = Mouse::event_queue();

	// Drain all pending events from the queue
	Event ev;
	while (eq.dequeue(ev)) {
		total_events++;

		switch (ev.type_) {
		case EventType::MouseMove:
		case EventType::MouseDown:
		case EventType::MouseUp:
			mouse_events++;
			wm.handle_mouse(ev);
			break;
		case EventType::KeyDown:
		case EventType::KeyUp:
			wm.handle_key(ev);
			break;
		}
	}

	// Poll the focused terminal for shell output (if it has a stdout pipe)
	auto* focused = wm.focused();
	if (focused != nullptr) {
		auto* term = static_cast<Terminal*>(focused);
		term->poll_output();
		term->render_to_canvas();
	}

	// Composite all windows onto the screen
	wm.composite();
}

}  // anonymous namespace

// ============================================================
// gui_start() -- activate the WM tick loop from kernel_init_thread
// ============================================================

Terminal* gui_start() {
	cinux::lib::kprintf("[GUI] ===== Milestone 030: GUI Window Manager =====\n");

	// Initialise PS/2 mouse driver
	cinux::drivers::Mouse::init();

	// Configure mouse screen bounds to match the canvas
	if (g_screen != nullptr) {
		cinux::drivers::Mouse::set_screen_bounds(g_screen->width(), g_screen->height());
	}

	// Create a Terminal window for the shell
	uint32_t term_w = Terminal::COLS * 8;	// 80 * 8 = 640
	uint32_t term_h = Terminal::ROWS * 16;	// 25 * 16 = 400

	// Centre the terminal on screen if possible
	uint32_t term_x = 80;
	uint32_t term_y = 60;

	if (g_screen != nullptr) {
		uint32_t sw = g_screen->width();
		uint32_t sh = g_screen->height();
		if (term_w + 80 < sw) {
			term_x = (sw - term_w) / 2;
		}
		if (term_h + 60 < sh) {
			term_y = (sh - term_h) / 2;
		}
	}

	auto* term = new Terminal(term_x, term_y, "Cinux Terminal");
	term->set_font(g_font);
	WindowManager::instance().add_window(term);

	cinux::lib::kprintf("[GUI] WindowManager initialised with Terminal window.\n");

	// Register the GUI tick callback for event processing + compositing
	cinux::drivers::PIT::set_tick_callback(gui_tick_callback, nullptr);
	cinux::lib::kprintf("[GUI] GUI tick callback registered on PIT.\n");

	return term;
}

}  // namespace cinux::gui
