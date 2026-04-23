/**
 * @file kernel/gui/window_manager.cpp
 * @brief WindowManager class implementation
 */

#include "window_manager.hpp"

#include "drivers/mouse.hpp"
#include "drivers/video/font.hpp"

namespace cinux::gui {

// ============================================================
// Singleton
// ============================================================

WindowManager& WindowManager::instance() {
	static WindowManager wm;
	return wm;
}

// ============================================================
// Construction / destruction
// ============================================================

WindowManager::~WindowManager() {
	// Destroy all remaining windows
	for (uint32_t i = 0; i < count_; i++) {
		delete windows_[i];
		windows_[i] = nullptr;
	}
	count_	 = 0;
	focused_ = nullptr;
}

// ============================================================
// Lifecycle
// ============================================================

void WindowManager::init(cinux::drivers::Canvas* screen, cinux::drivers::PSFFont* font) {
	screen_	  = screen;
	font_	  = font;
	count_	  = 0;
	focused_  = nullptr;
	dragging_ = false;
	mouse_x_  = 0;
	mouse_y_  = 0;
}

// ============================================================
// Window management
// ============================================================

uint32_t WindowManager::create(const char* title, uint32_t w, uint32_t h) {
	// Reject if we have reached the maximum window count
	if (count_ >= MAX_WINDOWS) {
		return 0;
	}

	// Stagger new windows so they are not perfectly overlapping
	int32_t offset_x = static_cast<int32_t>(count_ * 30);
	int32_t offset_y = static_cast<int32_t>(count_ * 30);

	// Heap-allocate the new window at the top of Z-order
	windows_[count_] = new Window(title, offset_x, offset_y, w, h);

	// Draw the title bar and content onto the window's off-screen canvas
	if (font_ != nullptr) {
		windows_[count_]->draw_title_bar(*font_);
	}
	windows_[count_]->draw_content();

	count_++;

	// Update focus to the new top-most window
	update_focus();

	return windows_[count_ - 1]->id();
}

uint32_t WindowManager::add_window(Window* win) {
	// Reject if we have reached the maximum window count
	if (count_ >= MAX_WINDOWS || win == nullptr) {
		return 0;
	}

	// Place the window at the top of Z-order
	windows_[count_] = win;

	// Draw the title bar and content onto the window's off-screen canvas
	if (font_ != nullptr) {
		windows_[count_]->draw_title_bar(*font_);
	}
	windows_[count_]->draw_content();

	count_++;

	// Update focus to the new top-most window
	update_focus();

	return windows_[count_ - 1]->id();
}

void WindowManager::destroy(uint32_t id) {
	// Search for the window by ID
	uint32_t idx = find_index(id);
	if (idx < MAX_WINDOWS) {
		remove_at(idx);
	}
}

void WindowManager::raise(uint32_t id) {
	uint32_t idx = find_index(id);

	// Not found
	if (idx >= MAX_WINDOWS) {
		return;
	}

	// Already at the top -- still update focus (may be nullptr after desktop click)
	if (idx == count_ - 1) {
		update_focus();
		return;
	}

	// Save the pointer, shift windows above it down, place at top
	Window* win = windows_[idx];

	for (uint32_t i = idx; i < count_ - 1; i++) {
		windows_[i] = windows_[i + 1];
	}

	windows_[count_ - 1] = win;

	// Update focus to the new top-most window
	update_focus();
}

// ============================================================
// Compositing
// ============================================================

void WindowManager::composite() {
	if (screen_ == nullptr) {
		return;
	}

	// Clear the screen back buffer with the desktop background colour
	screen_->clear(DESKTOP_COLOR);

	// Blit each window from lowest Z-order (index 0) to highest
	for (uint32_t i = 0; i < count_; i++) {
		if (windows_[i]->visible()) {
			windows_[i]->blit_to(*screen_);
		}
	}

	// Draw the mouse cursor on top of everything
	draw_cursor(*screen_);

	// Present the composed frame to the hardware framebuffer
	screen_->flip();
}

// ============================================================
// Cursor rendering
// ============================================================

void WindowManager::draw_cursor(cinux::drivers::Canvas& screen) {
	int32_t cx = cinux::drivers::Mouse::x();
	int32_t cy = cinux::drivers::Mouse::y();

	for (uint32_t row = 0; row < CURSOR_SIZE; row++) {
		uint16_t bits = k_cursor_bitmap[row];
		for (uint32_t col = 0; col < CURSOR_SIZE; col++) {
			if (bits & (0x8000 >> col)) {
				int32_t px = cx + static_cast<int32_t>(col);
				int32_t py = cy + static_cast<int32_t>(row);

				// Draw black outline for visibility
				screen.draw_pixel(static_cast<uint32_t>(px - 1), static_cast<uint32_t>(py),
								  CURSOR_BLACK);
				screen.draw_pixel(static_cast<uint32_t>(px + 1), static_cast<uint32_t>(py),
								  CURSOR_BLACK);
				screen.draw_pixel(static_cast<uint32_t>(px), static_cast<uint32_t>(py - 1),
								  CURSOR_BLACK);
				screen.draw_pixel(static_cast<uint32_t>(px), static_cast<uint32_t>(py + 1),
								  CURSOR_BLACK);

				// Draw white cursor pixel
				screen.draw_pixel(static_cast<uint32_t>(px), static_cast<uint32_t>(py),
								  CURSOR_WHITE);
			}
		}
	}
}

// ============================================================
// Input handling
// ============================================================

void WindowManager::handle_mouse(Event& ev) {
	// Update tracked mouse position
	mouse_x_ = ev.mouse.x;
	mouse_y_ = ev.mouse.y;

	switch (ev.type_) {
	case EventType::MouseDown: {
		// Only handle left button press
		if (!ev.mouse.left) {
			break;
		}

		// Hit test from top-most window downward
		Window* hit = hit_test(ev.mouse.x, ev.mouse.y);

		if (hit == nullptr) {
			// Clicked on the desktop background -- clear focus
			if (focused_ != nullptr) {
				focused_->set_focused(false);
				focused_ = nullptr;
			}
			break;
		}

		int32_t local_y = ev.mouse.y - hit->y();
		int		is_title_bar =
			(local_y >= 0 && local_y < static_cast<int32_t>(Window::TITLE_BAR_HEIGHT)) ? 1 : 0;

		// Check if the close button was hit
		if (hit->is_close_button_hit(ev.mouse.x, ev.mouse.y)) {
			uint32_t dead_id = hit->id();
			destroy(dead_id);
			composite();
			break;
		}

		// Raise the clicked window to the top
		raise(hit->id());

		// Check if the click landed on the title bar
		// The title bar spans from window y_ to y_ + TITLE_BAR_HEIGHT
		if (is_title_bar) {
			// Begin dragging: record the offset from window origin
			dragging_	   = true;
			drag_offset_x_ = ev.mouse.x - hit->x();
			drag_offset_y_ = ev.mouse.y - hit->y();
		}

		composite();
		break;
	}

	case EventType::MouseMove: {
		if (dragging_ && focused_ != nullptr) {
			// Move the focused window to follow the cursor
			int32_t new_x = ev.mouse.x - drag_offset_x_;
			int32_t new_y = ev.mouse.y - drag_offset_y_;
			focused_->set_position(new_x, new_y);

			// Redraw the window (title bar + content) at the new position
			if (font_ != nullptr) {
				focused_->draw_title_bar(*font_);
			}
			focused_->draw_content();

			composite();
		}
		break;
	}

	case EventType::MouseUp: {
		if (dragging_) {
			dragging_ = false;
		}
		break;
	}

	default:
		break;
	}
}

void WindowManager::handle_key(Event& ev) {
	// Forward keyboard events to the focused window, if any
	if (focused_ != nullptr) {
		focused_->on_key(ev.key);
	}
}

// ============================================================
// Internal helpers
// ============================================================

Window* WindowManager::find_window(uint32_t id) {
	for (uint32_t i = 0; i < count_; i++) {
		if (windows_[i]->id() == id) {
			return windows_[i];
		}
	}
	return nullptr;
}

uint32_t WindowManager::find_index(uint32_t id) const {
	for (uint32_t i = 0; i < count_; i++) {
		if (windows_[i]->id() == id) {
			return i;
		}
	}
	return MAX_WINDOWS;
}

Window* WindowManager::hit_test(int32_t mx, int32_t my) {
	// Iterate from top-most (count_ - 1) down to bottom (0)
	for (uint32_t i = count_; i > 0; i--) {
		uint32_t idx = i - 1;
		if (windows_[idx]->visible() && windows_[idx]->contains(mx, my)) {
			return windows_[idx];
		}
	}
	return nullptr;
}

void WindowManager::remove_at(uint32_t idx) {
	if (idx >= count_) {
		return;
	}

	bool was_focused = (focused_ != nullptr && focused_->id() == windows_[idx]->id());

	// Free the window memory
	delete windows_[idx];
	windows_[idx] = nullptr;

	// Shift windows above idx down by one slot
	for (uint32_t i = idx; i < count_ - 1; i++) {
		windows_[i] = windows_[i + 1];
	}

	// Clear the vacated top slot
	windows_[count_ - 1] = nullptr;
	count_--;

	// If the removed window was focused, update focus
	if (was_focused) {
		update_focus();
	}
}

void WindowManager::update_focus() {
	// Clear focus on all windows
	for (uint32_t i = 0; i < count_; i++) {
		windows_[i]->set_focused(false);
	}

	// Set focus on the top-most window, if any
	if (count_ > 0) {
		focused_ = windows_[count_ - 1];
		focused_->set_focused(true);
	} else {
		focused_ = nullptr;
	}
}

}  // namespace cinux::gui
