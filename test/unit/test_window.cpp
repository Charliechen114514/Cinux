/**
 * @file test/unit/test_window.cpp
 * @brief Host-side unit tests for Window class (030_gui_wm_basic, sub-iteration B)
 *
 * Tests Window construction, ID auto-increment, draw_title_bar, draw_content,
 * is_close_button_hit, contains, set_position, resize, set_title, and blit_to
 * by re-implementing Window with mock Canvas/PSFFont.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>
#include <vector>

// ============================================================
// Mock PSFFont (mimics kernel/drivers/video/font.hpp)
// ============================================================

class MockPSFFont {
public:
    void init(uint32_t w, uint32_t h) {
        width_  = w;
        height_ = h;
    }

    void set_glyph(uint8_t c, const uint8_t* rows, uint32_t num_rows) {
        glyphs_[c].assign(rows, rows + num_rows);
    }

    const uint8_t* glyph(uint8_t c) const {
        if (glyphs_[c].empty()) return nullptr;
        return glyphs_[c].data();
    }

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

private:
    std::vector<uint8_t> glyphs_[256];
    uint32_t width_  = 0;
    uint32_t height_ = 0;
};

// ============================================================
// Mock Canvas (mimics kernel/drivers/canvas.hpp/.cpp logic)
// ============================================================

class MockCanvas {
public:
    MockCanvas() = default;

    void init(uint32_t w, uint32_t h) {
        if (!back_buf_.empty()) back_buf_.clear();
        width_     = w;
        height_    = h;
        pitch_     = w * 4;
        back_buf_.resize(static_cast<size_t>(w) * h, 0);
    }

    void draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= width_ || y >= height_) return;
        back_buf_[y * width_ + x] = color;
    }

    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
        for (uint32_t row = y; row < y + h && row < height_; row++) {
            for (uint32_t col = x; col < x + w && col < width_; col++) {
                back_buf_[row * width_ + col] = color;
            }
        }
    }

    void blit(uint32_t dst_x, uint32_t dst_y, MockCanvas& src,
              uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
        for (uint32_t row = 0; row < h; row++) {
            uint32_t src_row = sy + row;
            uint32_t dst_row = dst_y + row;
            if (src_row >= src.height_ || dst_row >= height_) break;
            for (uint32_t col = 0; col < w; col++) {
                uint32_t src_col = sx + col;
                uint32_t dst_col = dst_x + col;
                if (src_col >= src.width_ || dst_col >= width_) break;
                back_buf_[dst_row * width_ + dst_col] =
                    src.back_buf_[src_row * src.width_ + src_col];
            }
        }
    }

    void draw_text(uint32_t x, uint32_t y, const char* str, uint32_t color,
                   MockPSFFont& font) {
        if (str == nullptr) return;
        uint32_t gw = font.width();
        uint32_t gh = font.height();
        if (gw == 0 || gh == 0) return;
        uint32_t cx = x;
        for (uint32_t i = 0; str[i] != '\0'; i++) {
            const uint8_t* g = font.glyph(static_cast<uint8_t>(str[i]));
            if (g == nullptr) continue;
            for (uint32_t row = 0; row < gh; row++) {
                uint8_t bits = g[row];
                for (uint32_t col = 0; col < gw; col++) {
                    if ((bits >> (7 - col)) & 1) {
                        draw_pixel(cx + col, y + row, color);
                    }
                }
            }
            cx += gw;
        }
    }

    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

    uint32_t pixel(uint32_t x, uint32_t y) const {
        if (x >= width_ || y >= height_) return 0xDEAD;
        return back_buf_[y * width_ + x];
    }

private:
    std::vector<uint32_t> back_buf_;
    uint32_t width_  = 0;
    uint32_t height_ = 0;
    uint32_t pitch_  = 0;
};

// ============================================================
// Re-implemented Window (mirrors kernel/gui/window.hpp/.cpp logic)
// ============================================================

class MockWindow {
public:
    static constexpr uint32_t TITLE_BAR_HEIGHT = 20;
    static constexpr uint32_t CLOSE_BUTTON_SIZE = 14;
    static constexpr uint32_t TITLE_MAX_LEN    = 63;
    static constexpr uint32_t DEFAULT_WIDTH    = 320;
    static constexpr uint32_t DEFAULT_HEIGHT   = 240;

    static constexpr uint32_t COLOR_TITLE_BG     = 0x00336699;
    static constexpr uint32_t COLOR_TITLE_TEXT   = 0x00FFFFFF;
    static constexpr uint32_t COLOR_CLOSE_BUTTON = 0x00CC3333;
    static constexpr uint32_t COLOR_CONTENT_BG   = 0x00E0E0E0;
    static constexpr uint32_t COLOR_BORDER       = 0x00444444;

    static uint32_t next_id_;

    MockWindow(const char* title = "Untitled",
               int32_t x = 0, int32_t y = 0,
               uint32_t w = DEFAULT_WIDTH,
               uint32_t h = DEFAULT_HEIGHT)
        : id_(next_id_++), x_(x), y_(y), w_(w), h_(h),
          visible_(true), focused_(false) {
        for (uint32_t i = 0; i <= TITLE_MAX_LEN; i++) title_[i] = '\0';
        if (title != nullptr) {
            uint32_t i = 0;
            while (i < TITLE_MAX_LEN && title[i] != '\0') {
                title_[i] = title[i];
                i++;
            }
        }
        allocate_canvas();
    }

    void draw_title_bar(MockPSFFont& font) {
        canvas_.draw_rect(0, 0, w_, TITLE_BAR_HEIGHT, COLOR_TITLE_BG);
        canvas_.draw_rect(0, TITLE_BAR_HEIGHT - 1, w_, 1, COLOR_BORDER);
        uint32_t text_y = (TITLE_BAR_HEIGHT - font.height()) / 2;
        canvas_.draw_text(4, text_y, title_, COLOR_TITLE_TEXT, font);
        uint32_t cb_x = w_ - CLOSE_BUTTON_SIZE - 3;
        uint32_t cb_y = (TITLE_BAR_HEIGHT - CLOSE_BUTTON_SIZE) / 2;
        canvas_.draw_rect(cb_x, cb_y, CLOSE_BUTTON_SIZE, CLOSE_BUTTON_SIZE,
                          COLOR_CLOSE_BUTTON);
    }

    void draw_content() {
        canvas_.draw_rect(0, TITLE_BAR_HEIGHT, w_, h_, COLOR_CONTENT_BG);
    }

    void blit_to(MockCanvas& dst) {
        if (!visible_) return;
        dst.blit(static_cast<uint32_t>(x_), static_cast<uint32_t>(y_),
                 canvas_, 0, 0, w_, total_height());
    }

    void set_position(int32_t x, int32_t y) { x_ = x; y_ = y; }
    void resize(uint32_t w, uint32_t h) { w_ = w; h_ = h; allocate_canvas(); }
    void set_title(const char* title) {
        for (uint32_t i = 0; i <= TITLE_MAX_LEN; i++) title_[i] = '\0';
        if (title != nullptr) {
            uint32_t i = 0;
            while (i < TITLE_MAX_LEN && title[i] != '\0') {
                title_[i] = title[i];
                i++;
            }
        }
    }

    bool is_close_button_hit(int32_t mx, int32_t my) const {
        int32_t cb_x = x_ + static_cast<int32_t>(w_) - CLOSE_BUTTON_SIZE - 3;
        int32_t cb_y = y_ + static_cast<int32_t>((TITLE_BAR_HEIGHT - CLOSE_BUTTON_SIZE) / 2);
        return mx >= cb_x && mx < cb_x + static_cast<int32_t>(CLOSE_BUTTON_SIZE)
            && my >= cb_y && my < cb_y + static_cast<int32_t>(CLOSE_BUTTON_SIZE);
    }

    bool contains(int32_t mx, int32_t my) const {
        return mx >= x_
            && mx < x_ + static_cast<int32_t>(w_)
            && my >= y_
            && my < y_ + static_cast<int32_t>(total_height());
    }

    uint32_t id() const { return id_; }
    int32_t  x() const { return x_; }
    int32_t  y() const { return y_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    bool     visible() const { return visible_; }
    bool     focused() const { return focused_; }
    const char* title() const { return title_; }
    uint32_t total_height() const { return h_ + TITLE_BAR_HEIGHT; }

    void set_visible(bool v) { visible_ = v; }
    void set_focused(bool f) { focused_ = f; }

    MockCanvas& canvas() { return canvas_; }

    static void reset_id() { next_id_ = 1; }

private:
    void allocate_canvas() {
        uint32_t total_h = h_ + TITLE_BAR_HEIGHT;
        canvas_.init(w_, total_h);
    }

    uint32_t id_;
    int32_t  x_;
    int32_t  y_;
    uint32_t w_;
    uint32_t h_;
    char     title_[TITLE_MAX_LEN + 1];
    MockCanvas canvas_;
    bool     visible_;
    bool     focused_;
};

uint32_t MockWindow::next_id_ = 1;

// ============================================================
// Helpers
// ============================================================

static MockPSFFont make_test_font() {
    MockPSFFont font;
    // 3-pixel-wide, 3-pixel-tall font for testing
    font.init(3, 3);

    // Glyph for 'H': simple pattern
    // Row 0: X.X = 0xA0
    // Row 1: XXX = 0xE0
    // Row 2: X.X = 0xA0
    {
        uint8_t glyph_h[] = {0xA0, 0xE0, 0xA0};
        font.set_glyph('H', glyph_h, 3);
    }

    // Glyph for 'i': single pixel top, column middle
    // Row 0: .X. = 0x40
    // Row 1: .X. = 0x40
    // Row 2: .X. = 0x40
    {
        uint8_t glyph_i[] = {0x40, 0x40, 0x40};
        font.set_glyph('i', glyph_i, 3);
    }

    // Glyph for 'U':
    // Row 0: X.X = 0xA0
    // Row 1: X.X = 0xA0
    // Row 2: XXX = 0xE0
    {
        uint8_t glyph_u[] = {0xA0, 0xA0, 0xE0};
        font.set_glyph('U', glyph_u, 3);
    }

    return font;
}

// ============================================================
// Construction tests
// ============================================================

TEST("window: default construction initializes fields") {
    MockWindow::reset_id();
    MockWindow w;

    ASSERT_EQ(w.id(), 1u);
    ASSERT_EQ(w.x(), 0);
    ASSERT_EQ(w.y(), 0);
    ASSERT_EQ(w.width(), 320u);
    ASSERT_EQ(w.height(), 240u);
    ASSERT_TRUE(w.visible());
    ASSERT_FALSE(w.focused());
    ASSERT_TRUE(strcmp(w.title(), "Untitled") == 0);
    ASSERT_EQ(w.total_height(), 260u);
}

TEST("window: construction with custom parameters") {
    MockWindow::reset_id();
    MockWindow w("My App", 50, 100, 400, 300);

    ASSERT_EQ(w.id(), 1u);
    ASSERT_EQ(w.x(), 50);
    ASSERT_EQ(w.y(), 100);
    ASSERT_EQ(w.width(), 400u);
    ASSERT_EQ(w.height(), 300u);
    ASSERT_TRUE(strcmp(w.title(), "My App") == 0);
    ASSERT_EQ(w.total_height(), 320u);
}

TEST("window: id auto-increments across multiple windows") {
    MockWindow::reset_id();
    MockWindow a("A");
    MockWindow b("B");
    MockWindow c("C");

    ASSERT_EQ(a.id(), 1u);
    ASSERT_EQ(b.id(), 2u);
    ASSERT_EQ(c.id(), 3u);
}

TEST("window: null title defaults gracefully") {
    MockWindow::reset_id();
    MockWindow w(nullptr, 0, 0, 100, 100);

    ASSERT_EQ(w.title()[0], '\0');
}

TEST("window: title truncated to TITLE_MAX_LEN") {
    MockWindow::reset_id();
    // TITLE_MAX_LEN = 63, so a 70-char title should be truncated to 63 chars
    char long_title[70];
    for (int i = 0; i < 69; i++) long_title[i] = 'X';
    long_title[69] = '\0';

    MockWindow w(long_title, 0, 0, 100, 100);
    ASSERT_EQ(strlen(w.title()), 63u);
}

// ============================================================
// set_position / resize / set_title tests
// ============================================================

TEST("window: set_position updates coordinates") {
    MockWindow::reset_id();
    MockWindow w("Test", 10, 20, 100, 50);

    w.set_position(200, 300);
    ASSERT_EQ(w.x(), 200);
    ASSERT_EQ(w.y(), 300);
}

TEST("window: resize updates dimensions and reallocates canvas") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);

    w.resize(200, 150);
    ASSERT_EQ(w.width(), 200u);
    ASSERT_EQ(w.height(), 150u);
    ASSERT_EQ(w.total_height(), 170u);

    // Canvas should have the new dimensions
    MockCanvas& c = w.canvas();
    ASSERT_EQ(c.width(), 200u);
    ASSERT_EQ(c.height(), 170u);
}

TEST("window: set_title changes title") {
    MockWindow::reset_id();
    MockWindow w("Old");

    w.set_title("New Title");
    ASSERT_TRUE(strcmp(w.title(), "New Title") == 0);
}

TEST("window: set_title with null clears title") {
    MockWindow::reset_id();
    MockWindow w("Something");

    w.set_title(nullptr);
    ASSERT_EQ(w.title()[0], '\0');
}

TEST("window: set_title truncates long title") {
    MockWindow::reset_id();
    MockWindow w("Short");

    char long_title[70];
    for (int i = 0; i < 69; i++) long_title[i] = 'Y';
    long_title[69] = '\0';
    w.set_title(long_title);
    ASSERT_EQ(strlen(w.title()), 63u);
}

// ============================================================
// set_visible / set_focused tests
// ============================================================

TEST("window: set_visible toggles visibility") {
    MockWindow::reset_id();
    MockWindow w;
    ASSERT_TRUE(w.visible());

    w.set_visible(false);
    ASSERT_FALSE(w.visible());

    w.set_visible(true);
    ASSERT_TRUE(w.visible());
}

TEST("window: set_focused toggles focus") {
    MockWindow::reset_id();
    MockWindow w;
    ASSERT_FALSE(w.focused());

    w.set_focused(true);
    ASSERT_TRUE(w.focused());

    w.set_focused(false);
    ASSERT_FALSE(w.focused());
}

// ============================================================
// is_close_button_hit tests
// ============================================================

TEST("window: close button hit at exact top-left corner") {
    MockWindow::reset_id();
    // Window at (0,0), width=100. Close button at (100-14-3, (20-14)/2) = (83, 3)
    MockWindow w("Test", 0, 0, 100, 50);

    // cb_x = 0 + 100 - 14 - 3 = 83
    // cb_y = 0 + (20 - 14) / 2 = 3
    ASSERT_TRUE(w.is_close_button_hit(83, 3));
}

TEST("window: close button hit at bottom-right corner (exclusive)") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);

    // cb_x=83, cb_y=3, size=14 -> bottom-right is (96,16), exclusive
    ASSERT_TRUE(w.is_close_button_hit(96, 16));
    // Just outside (exclusive boundary)
    ASSERT_FALSE(w.is_close_button_hit(97, 16));
    ASSERT_FALSE(w.is_close_button_hit(96, 17));
}

TEST("window: close button not hit outside window") {
    MockWindow::reset_id();
    MockWindow w("Test", 100, 100, 200, 150);

    // Click far away from the window
    ASSERT_FALSE(w.is_close_button_hit(0, 0));
    ASSERT_FALSE(w.is_close_button_hit(500, 500));
}

TEST("window: close button not hit in content area") {
    MockWindow::reset_id();
    MockWindow w("Test", 10, 10, 200, 100);

    // Click in the content area (below title bar)
    ASSERT_FALSE(w.is_close_button_hit(50, 50));
}

TEST("window: close button not hit in title bar but outside button") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 200, 100);

    // Title bar left area (far from close button)
    ASSERT_FALSE(w.is_close_button_hit(10, 5));
}

TEST("window: close button hit with non-zero position") {
    MockWindow::reset_id();
    MockWindow w("Test", 50, 80, 200, 100);

    // cb_x = 50 + 200 - 14 - 3 = 233
    // cb_y = 80 + (20 - 14) / 2 = 83
    ASSERT_TRUE(w.is_close_button_hit(233, 83));
    ASSERT_TRUE(w.is_close_button_hit(246, 96));  // bottom-right corner
    ASSERT_FALSE(w.is_close_button_hit(232, 83)); // one pixel left of button
    ASSERT_FALSE(w.is_close_button_hit(233, 82)); // one pixel above button
}

// ============================================================
// contains tests
// ============================================================

TEST("window: contains point inside window") {
    MockWindow::reset_id();
    MockWindow w("Test", 10, 20, 100, 50);

    // Top-left corner
    ASSERT_TRUE(w.contains(10, 20));
    // Middle of content area
    ASSERT_TRUE(w.contains(50, 50));
}

TEST("window: contains rejects point outside window") {
    MockWindow::reset_id();
    MockWindow w("Test", 10, 20, 100, 50);

    // Above window
    ASSERT_FALSE(w.contains(50, 19));
    // Below window (total_height = 50 + 20 = 70, so y >= 90 is outside)
    ASSERT_FALSE(w.contains(50, 90));
    // Left of window
    ASSERT_FALSE(w.contains(9, 30));
    // Right of window (x >= 110 is outside)
    ASSERT_FALSE(w.contains(110, 30));
}

TEST("window: contains rejects exclusive boundaries") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);

    // x = 100 is exactly on the right edge (exclusive)
    ASSERT_FALSE(w.contains(100, 25));
    // y = 70 is exactly on the bottom edge (exclusive, total_height=70)
    ASSERT_FALSE(w.contains(50, 70));
}

TEST("window: contains includes title bar area") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);

    // Inside title bar
    ASSERT_TRUE(w.contains(50, 10));
    ASSERT_TRUE(w.contains(50, 0));
    // Bottom row of title bar
    ASSERT_TRUE(w.contains(50, 19));
}

// ============================================================
// draw_title_bar tests
// ============================================================

TEST("window: draw_title_bar fills title bar with steel blue") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);
    MockPSFFont font = make_test_font();

    w.draw_title_bar(font);

    MockCanvas& c = w.canvas();
    // Check title bar background pixels
    ASSERT_EQ(c.pixel(0, 0), MockWindow::COLOR_TITLE_BG);
    ASSERT_EQ(c.pixel(50, 5), MockWindow::COLOR_TITLE_BG);
    ASSERT_EQ(c.pixel(99, 0), MockWindow::COLOR_TITLE_BG);
}

TEST("window: draw_title_bar draws border at bottom of title bar") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);
    MockPSFFont font = make_test_font();

    w.draw_title_bar(font);

    MockCanvas& c = w.canvas();
    // Row 19 (TITLE_BAR_HEIGHT - 1) should be border color
    ASSERT_EQ(c.pixel(0, 19), MockWindow::COLOR_BORDER);
    ASSERT_EQ(c.pixel(50, 19), MockWindow::COLOR_BORDER);
    ASSERT_EQ(c.pixel(99, 19), MockWindow::COLOR_BORDER);
}

TEST("window: draw_title_bar draws close button in red") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);
    MockPSFFont font = make_test_font();

    w.draw_title_bar(font);

    MockCanvas& c = w.canvas();
    // Close button at (100-14-3, (20-14)/2) = (83, 3), size 14x14
    ASSERT_EQ(c.pixel(83, 3), MockWindow::COLOR_CLOSE_BUTTON);
    ASSERT_EQ(c.pixel(90, 10), MockWindow::COLOR_CLOSE_BUTTON);
    ASSERT_EQ(c.pixel(96, 16), MockWindow::COLOR_CLOSE_BUTTON);
}

TEST("window: draw_title_bar does not draw in content area") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);
    MockPSFFont font = make_test_font();

    w.draw_title_bar(font);

    MockCanvas& c = w.canvas();
    // Content area should remain untouched (0)
    ASSERT_EQ(c.pixel(0, 20), 0u);
    ASSERT_EQ(c.pixel(50, 40), 0u);
}

// ============================================================
// draw_content tests
// ============================================================

TEST("window: draw_content fills content area with light grey") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);

    w.draw_content();

    MockCanvas& c = w.canvas();
    ASSERT_EQ(c.pixel(0, 20), MockWindow::COLOR_CONTENT_BG);
    ASSERT_EQ(c.pixel(50, 40), MockWindow::COLOR_CONTENT_BG);
    ASSERT_EQ(c.pixel(99, 69), MockWindow::COLOR_CONTENT_BG);
}

TEST("window: draw_content does not overwrite title bar") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);

    w.draw_content();

    MockCanvas& c = w.canvas();
    // Title bar area should remain untouched (0)
    ASSERT_EQ(c.pixel(0, 0), 0u);
    ASSERT_EQ(c.pixel(50, 10), 0u);
    ASSERT_EQ(c.pixel(0, 19), 0u);
}

// ============================================================
// blit_to tests
// ============================================================

TEST("window: blit_to copies window to destination canvas") {
    MockWindow::reset_id();
    MockWindow w("Test", 10, 10, 50, 30);
    MockPSFFont font = make_test_font();

    w.draw_title_bar(font);
    w.draw_content();

    MockCanvas dst;
    dst.init(200, 200);

    w.blit_to(dst);

    // Title bar blue background at screen position (10,10)
    ASSERT_EQ(dst.pixel(10, 10), MockWindow::COLOR_TITLE_BG);
    // Content area grey at screen position (10, 30)
    ASSERT_EQ(dst.pixel(10, 30), MockWindow::COLOR_CONTENT_BG);
}

TEST("window: blit_to does nothing when invisible") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 50, 30);
    MockPSFFont font = make_test_font();

    w.draw_title_bar(font);
    w.set_visible(false);

    MockCanvas dst;
    dst.init(100, 100);

    w.blit_to(dst);

    // Destination should remain all zeros
    ASSERT_EQ(dst.pixel(0, 0), 0u);
    ASSERT_EQ(dst.pixel(25, 10), 0u);
}

TEST("window: blit_to places window at correct screen position") {
    MockWindow::reset_id();
    MockWindow w("Test", 50, 60, 40, 20);
    MockPSFFont font = make_test_font();

    w.draw_content();

    MockCanvas dst;
    dst.init(200, 200);

    w.blit_to(dst);

    // Content starts at y=60+TITLE_BAR_HEIGHT=80 in screen coords
    ASSERT_EQ(dst.pixel(50, 80), MockWindow::COLOR_CONTENT_BG);
    // Pixel before window should be zero
    ASSERT_EQ(dst.pixel(49, 80), 0u);
    ASSERT_EQ(dst.pixel(50, 79), 0u);
}

// ============================================================
// Canvas allocation tests
// ============================================================

TEST("window: canvas dimensions match window size including title bar") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 200, 150);

    MockCanvas& c = w.canvas();
    ASSERT_EQ(c.width(), 200u);
    ASSERT_EQ(c.height(), 170u);  // 150 + 20 title bar
}

TEST("window: canvas reallocated after resize") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 100, 50);

    w.draw_content();
    // Verify content is drawn at old size
    MockCanvas& c1 = w.canvas();
    ASSERT_EQ(c1.pixel(0, 20), MockWindow::COLOR_CONTENT_BG);

    w.resize(200, 100);
    MockCanvas& c2 = w.canvas();
    // New canvas should be fresh (zeros)
    ASSERT_EQ(c2.width(), 200u);
    ASSERT_EQ(c2.height(), 120u);
    ASSERT_EQ(c2.pixel(0, 20), 0u);
}

// ============================================================
// Multiple window independence tests
// ============================================================

TEST("window: multiple windows have independent state") {
    MockWindow::reset_id();
    MockWindow a("A", 0, 0, 100, 100);
    MockWindow b("B", 200, 200, 150, 80);

    ASSERT_EQ(a.id(), 1u);
    ASSERT_EQ(b.id(), 2u);
    ASSERT_EQ(a.x(), 0);
    ASSERT_EQ(b.x(), 200);
    ASSERT_TRUE(strcmp(a.title(), "A") == 0);
    ASSERT_TRUE(strcmp(b.title(), "B") == 0);

    // Modify a, verify b is unaffected
    a.set_position(10, 10);
    a.set_title("Modified");
    ASSERT_EQ(a.x(), 10);
    ASSERT_EQ(b.x(), 200);
    ASSERT_TRUE(strcmp(b.title(), "B") == 0);
}

TEST("window: multiple windows have independent canvases") {
    MockWindow::reset_id();
    MockWindow a("A", 0, 0, 100, 100);
    MockWindow b("B", 0, 0, 100, 100);

    MockPSFFont font = make_test_font();
    a.draw_content();
    b.draw_title_bar(font);

    // a's canvas should have content area filled but title bar untouched
    MockCanvas& ca = a.canvas();
    ASSERT_EQ(ca.pixel(0, 20), MockWindow::COLOR_CONTENT_BG);
    ASSERT_EQ(ca.pixel(0, 0), 0u);

    // b's canvas should have title bar drawn but content untouched
    MockCanvas& cb = b.canvas();
    ASSERT_EQ(cb.pixel(0, 0), MockWindow::COLOR_TITLE_BG);
    ASSERT_EQ(cb.pixel(0, 20), 0u);
}

// ============================================================
// Edge case tests
// ============================================================

TEST("window: zero-size content area") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 50, 0);

    ASSERT_EQ(w.height(), 0u);
    ASSERT_EQ(w.total_height(), 20u);

    MockCanvas& c = w.canvas();
    ASSERT_EQ(c.width(), 50u);
    ASSERT_EQ(c.height(), 20u);
}

TEST("window: negative position values") {
    MockWindow::reset_id();
    MockWindow w("Test", -10, -5, 100, 50);

    ASSERT_EQ(w.x(), -10);
    ASSERT_EQ(w.y(), -5);

    // contains should still work correctly with negative positions
    ASSERT_TRUE(w.contains(-10, -5));
    ASSERT_FALSE(w.contains(-11, -5));
    ASSERT_FALSE(w.contains(-10, -6));
}

TEST("window: large window size") {
    MockWindow::reset_id();
    MockWindow w("Test", 0, 0, 1024, 768);

    ASSERT_EQ(w.width(), 1024u);
    ASSERT_EQ(w.height(), 768u);
    ASSERT_EQ(w.total_height(), 788u);

    MockCanvas& c = w.canvas();
    ASSERT_EQ(c.width(), 1024u);
    ASSERT_EQ(c.height(), 788u);
}

// ============================================================
// Main entry point
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
