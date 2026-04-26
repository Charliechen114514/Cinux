/**
 * @file kernel/gui/window.cpp
 * @brief Window class implementation
 */

#include "window.hpp"

#include "drivers/video/font.hpp"

namespace cinux::gui {

// ============================================================
// Static member
// ============================================================

uint32_t Window::next_id_ = 1;

// ============================================================
// Construction
// ============================================================

Window::Window(const char* title, int32_t x, int32_t y, uint32_t w, uint32_t h)
    : id_(next_id_++), x_(x), y_(y), w_(w), h_(h), visible_(true), focused_(false) {
    // Zero-fill and copy title
    for (uint32_t i = 0; i <= TITLE_MAX_LEN; i++) {
        title_[i] = '\0';
    }

    if (title != nullptr) {
        uint32_t i = 0;
        while (i < TITLE_MAX_LEN && title[i] != '\0') {
            title_[i] = title[i];
            i++;
        }
    }

    allocate_canvas();
}

// ============================================================
// Drawing
// ============================================================

void Window::draw_title_bar(cinux::drivers::PSFFont& font) {
    // Fill the title bar area with steel blue
    canvas_.draw_rect(0, 0, w_, TITLE_BAR_HEIGHT, COLOR_TITLE_BG);

    // Draw a 1-pixel border at the bottom of the title bar
    canvas_.draw_rect(0, TITLE_BAR_HEIGHT - 1, w_, 1, COLOR_BORDER);

    // Draw the title text (white), offset a few pixels from the left
    uint32_t text_y = (TITLE_BAR_HEIGHT - font.height()) / 2;
    canvas_.draw_text(4, text_y, title_, COLOR_TITLE_TEXT, font);

    // Draw the close button: red square in the top-right corner
    uint32_t cb_x = w_ - CLOSE_BUTTON_SIZE - 3;
    uint32_t cb_y = (TITLE_BAR_HEIGHT - CLOSE_BUTTON_SIZE) / 2;
    canvas_.draw_rect(cb_x, cb_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE, COLOR_CLOSE_BUTTON);
}

void Window::draw_content() {
    // Clear the content area (below the title bar) with the background colour
    canvas_.draw_rect(0, TITLE_BAR_HEIGHT, w_, h_, COLOR_CONTENT_BG);

    // Give subclasses a chance to paint custom content on top
    on_paint(canvas_);
}

void Window::blit_to(cinux::drivers::Canvas& dst) {
    if (!visible_) {
        return;
    }

    // Blit the entire window canvas (title bar + content) to the destination.
    // Pass signed coordinates so the blit can handle partial off-screen windows.
    dst.blit(x_, y_, canvas_, 0, 0, w_, total_height());
}

// ============================================================
// Geometry
// ============================================================

void Window::set_position(int32_t x, int32_t y) {
    x_ = x;
    y_ = y;
}

void Window::resize(uint32_t w, uint32_t h) {
    w_ = w;
    h_ = h;
    allocate_canvas();
}

void Window::set_title(const char* title) {
    // Zero-fill title buffer
    for (uint32_t i = 0; i <= TITLE_MAX_LEN; i++) {
        title_[i] = '\0';
    }

    if (title != nullptr) {
        uint32_t i = 0;
        while (i < TITLE_MAX_LEN && title[i] != '\0') {
            title_[i] = title[i];
            i++;
        }
    }
}

// ============================================================
// Hit testing
// ============================================================

bool Window::is_close_button_hit(int32_t mx, int32_t my) const {
    // Close button position in screen coordinates
    int32_t cb_x = x_ + static_cast<int32_t>(w_) - CLOSE_BUTTON_SIZE - 3;
    int32_t cb_y = y_ + static_cast<int32_t>((TITLE_BAR_HEIGHT - CLOSE_BUTTON_SIZE) / 2);

    return mx >= cb_x && mx < cb_x + static_cast<int32_t>(CLOSE_BUTTON_SIZE) && my >= cb_y &&
           my < cb_y + static_cast<int32_t>(CLOSE_BUTTON_SIZE);
}

bool Window::contains(int32_t mx, int32_t my) const {
    return mx >= x_ && mx < x_ + static_cast<int32_t>(w_) && my >= y_ &&
           my < y_ + static_cast<int32_t>(total_height());
}

// ============================================================
// Internal helpers
// ============================================================

void Window::allocate_canvas() {
    // Total height includes the title bar
    uint32_t total_h = h_ + TITLE_BAR_HEIGHT;

    canvas_.init(w_, total_h);
}

}  // namespace cinux::gui
