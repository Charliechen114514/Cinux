/**
 * @file test/unit/test_desktop.cpp
 * @brief Host-side unit tests for desktop icon management (033_gui_desktop)
 *
 * Tests add_desktop_icon, hit_test_icon, consume_pending_icon_action,
 * and the icon-click / desktop-click dispatch path in handle_mouse.
 * Re-implements the relevant WindowManager methods with mock Canvas/PSFFont
 * -- no kernel code linked.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>
#    include <vector>

// ============================================================
// IconAction enum (mirrors kernel/gui/desktop_icon.hpp)
// ============================================================

enum class IconAction : uint8_t {
    None           = 0,
    OpenShell      = 1,
    OpenCalculator = 2,
};

// ============================================================
// DesktopIcon struct (mirrors kernel/gui/desktop_icon.hpp)
// ============================================================

struct DesktopIcon {
    int32_t         x;
    int32_t         y;
    const uint32_t* bitmap;
    const char*     label;
    uint32_t        width;
    uint32_t        height;
    IconAction      action;

    [[nodiscard]] bool contains(int32_t mx, int32_t my) const {
        return mx >= x && mx < static_cast<int32_t>(x + width) && my >= y &&
               my < static_cast<int32_t>(y + height);
    }
};

// ============================================================
// Event types (mirrors kernel/gui/event.hpp)
// ============================================================

enum class EventType : uint8_t {
    MouseMove = 0,
    MouseDown,
    MouseUp,
    KeyDown,
    KeyUp,
};

struct MouseEvent {
    int32_t x;
    int32_t y;
    int32_t dx;
    int32_t dy;
    uint8_t buttons;
    bool    left;
    bool    right;
    bool    middle;
};

struct KeyEvent {
    char    ascii;
    uint8_t scancode;
    bool    pressed;
    bool    shift;
    bool    ctrl;
    bool    alt;
};

struct Event {
    EventType type_;
    union {
        MouseEvent mouse;
        KeyEvent   key;
    };
};

// ============================================================
// Mock Canvas (mimics kernel/drivers/canvas.hpp)
// ============================================================

class MockCanvas {
public:
    MockCanvas() = default;

    void init(uint32_t w, uint32_t h) {
        width_  = w;
        height_ = h;
        back_buf_.resize(static_cast<size_t>(w) * h, 0);
    }

    void draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
        if (x >= width_ || y >= height_)
            return;
        back_buf_[y * width_ + x] = color;
    }

    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
        for (uint32_t row = y; row < y + h && row < height_; row++) {
            for (uint32_t col = x; col < x + w && col < width_; col++) {
                back_buf_[row * width_ + col] = color;
            }
        }
    }

    void blit(int32_t dst_x, int32_t dst_y, MockCanvas& src, uint32_t sx, uint32_t sy, uint32_t w,
              uint32_t h) {
        for (uint32_t row = 0; row < h; row++) {
            uint32_t src_row = sy + row;
            int32_t  dst_row = dst_y + static_cast<int32_t>(row);
            if (dst_row < 0)
                continue;
            if (src_row >= src.height_ || dst_row >= static_cast<int32_t>(height_))
                break;

            int32_t  col_skip  = 0;
            int32_t  eff_dst_x = dst_x;
            uint32_t eff_sx    = sx;
            if (eff_dst_x < 0) {
                col_skip  = -eff_dst_x;
                eff_dst_x = 0;
                eff_sx += static_cast<uint32_t>(col_skip);
            }

            uint32_t dst_col_start = static_cast<uint32_t>(eff_dst_x);
            uint32_t col_count     = w - static_cast<uint32_t>(col_skip);

            for (uint32_t i = 0; i < col_count; i++) {
                uint32_t src_col = eff_sx + i;
                uint32_t dst_col = dst_col_start + i;
                if (src_col >= src.width_ || dst_col >= width_)
                    break;
                back_buf_[dst_row * width_ + dst_col] =
                    src.back_buf_[src_row * src.width_ + src_col];
            }
        }
    }

    void draw_text(uint32_t, uint32_t, const char*, uint32_t, class MockPSFFont&) {
        // No-op in mock -- we only care about icon logic here
    }

    void draw_bitmap(uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh, const uint32_t* data) {
        if (data == nullptr)
            return;
        for (uint32_t row = 0; row < bh; row++) {
            for (uint32_t col = 0; col < bw; col++) {
                uint32_t px = bx + col;
                uint32_t py = by + row;
                if (px < width_ && py < height_) {
                    back_buf_[py * width_ + px] = data[row * bw + col];
                }
            }
        }
    }

    void clear(uint32_t color = 0) {
        for (auto& p : back_buf_)
            p = color;
    }

    void flip() { /* no-op */ }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    uint32_t pixel(uint32_t x, uint32_t y) const {
        if (x >= width_ || y >= height_)
            return 0xDEAD;
        return back_buf_[y * width_ + x];
    }

private:
    std::vector<uint32_t> back_buf_;
    uint32_t              width_  = 0;
    uint32_t              height_ = 0;
};

// ============================================================
// Mock PSFFont (mimics kernel/drivers/video/font.hpp)
// ============================================================

class MockPSFFont {
public:
    void init(uint32_t w, uint32_t h) {
        width_  = w;
        height_ = h;
    }
    void           set_glyph(uint8_t, const uint8_t*, uint32_t) { /* no-op */ }
    const uint8_t* glyph(uint8_t) const { return nullptr; }
    uint32_t       width() const { return width_; }
    uint32_t       height() const { return height_; }

private:
    uint32_t width_  = 0;
    uint32_t height_ = 0;
};

// ============================================================
// Mock Window (minimal -- enough for handle_mouse interaction)
// ============================================================

class MockWindow {
public:
    static constexpr uint32_t TITLE_BAR_HEIGHT  = 20;
    static constexpr uint32_t CLOSE_BUTTON_SIZE = 14;
    static constexpr uint32_t TITLE_MAX_LEN     = 63;

    static uint32_t next_id_;

    MockWindow(const char* title = "Untitled", int32_t x = 0, int32_t y = 0, uint32_t w = 320,
               uint32_t h = 240)
        : id_(next_id_++), x_(x), y_(y), w_(w), h_(h), visible_(true), focused_(false) {
        for (uint32_t i = 0; i <= TITLE_MAX_LEN; i++)
            title_[i] = '\0';
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
        return mx >= cb_x && mx < cb_x + static_cast<int32_t>(CLOSE_BUTTON_SIZE) && my >= cb_y &&
               my < cb_y + static_cast<int32_t>(CLOSE_BUTTON_SIZE);
    }

    bool contains(int32_t mx, int32_t my) const {
        return mx >= x_ && mx < x_ + static_cast<int32_t>(w_) && my >= y_ &&
               my < y_ + static_cast<int32_t>(h_ + TITLE_BAR_HEIGHT);
    }

    uint32_t id() const { return id_; }
    int32_t  x() const { return x_; }
    int32_t  y() const { return y_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    bool     visible() const { return visible_; }
    bool     focused() const { return focused_; }
    void     set_visible(bool v) { visible_ = v; }
    void     set_focused(bool f) { focused_ = f; }
    void     set_position(int32_t x, int32_t y) {
        x_ = x;
        y_ = y;
    }

    static void reset_id() { next_id_ = 1; }

private:
    uint32_t id_;
    int32_t  x_;
    int32_t  y_;
    uint32_t w_;
    uint32_t h_;
    char     title_[TITLE_MAX_LEN + 1];
    bool     visible_;
    bool     focused_;
};

uint32_t MockWindow::next_id_ = 1;

// ============================================================
// Re-implemented WindowManager with desktop icon support
// (mirrors kernel/gui/window_manager.hpp/.cpp)
// ============================================================

class MockWindowManager {
public:
    static constexpr uint32_t MAX_WINDOWS   = 64;
    static constexpr uint32_t MAX_ICONS     = 16;
    static constexpr uint32_t DESKTOP_COLOR = 0x00224466;

    MockWindowManager() = default;

    ~MockWindowManager() {
        for (uint32_t i = 0; i < count_; i++) {
            delete windows_[i];
            windows_[i] = nullptr;
        }
        count_   = 0;
        focused_ = nullptr;
    }

    MockWindowManager(const MockWindowManager&)            = delete;
    MockWindowManager& operator=(const MockWindowManager&) = delete;

    void init(MockCanvas* screen, MockPSFFont* font) {
        screen_              = screen;
        font_                = font;
        count_               = 0;
        focused_             = nullptr;
        dragging_            = false;
        mouse_x_             = 0;
        mouse_y_             = 0;
        icon_count_          = 0;
        pending_icon_action_ = IconAction::None;
    }

    // ---- Window management (minimal) ----

    uint32_t create(const char* title, uint32_t w, uint32_t h) {
        if (count_ >= MAX_WINDOWS)
            return 0;
        int32_t ox       = static_cast<int32_t>(count_ * 30);
        int32_t oy       = static_cast<int32_t>(count_ * 30);
        windows_[count_] = new MockWindow(title, ox, oy, w, h);
        count_++;
        update_focus();
        return windows_[count_ - 1]->id();
    }

    void destroy(uint32_t id) {
        uint32_t idx = find_index(id);
        if (idx < MAX_WINDOWS)
            remove_at(idx);
    }

    void raise(uint32_t id) {
        uint32_t idx = find_index(id);
        if (idx >= MAX_WINDOWS)
            return;
        if (idx == count_ - 1) {
            update_focus();
            return;
        }
        MockWindow* win = windows_[idx];
        for (uint32_t i = idx; i < count_ - 1; i++) {
            windows_[i] = windows_[i + 1];
        }
        windows_[count_ - 1] = win;
        update_focus();
    }

    // ---- Desktop icon management ----

    bool add_desktop_icon(const DesktopIcon& icon) {
        if (icon_count_ >= MAX_ICONS)
            return false;
        icons_[icon_count_] = icon;
        icon_count_++;
        return true;
    }

    const DesktopIcon* hit_test_icon(int32_t mx, int32_t my) const {
        for (uint32_t i = icon_count_; i > 0; i--) {
            uint32_t idx = i - 1;
            if (icons_[idx].contains(mx, my)) {
                return &icons_[idx];
            }
        }
        return nullptr;
    }

    IconAction consume_pending_icon_action() {
        IconAction action    = pending_icon_action_;
        pending_icon_action_ = IconAction::None;
        return action;
    }

    // ---- Compositing ----

    void composite() {
        if (screen_ == nullptr)
            return;
        screen_->clear(DESKTOP_COLOR);
        draw_desktop_icons(*screen_);
        for (uint32_t i = 0; i < count_; i++) {
            // blit windows -- simplified: just mark presence
            (void)windows_[i];
        }
        screen_->flip();
    }

    // ---- Input handling ----

    void handle_mouse(Event& ev) {
        mouse_x_ = ev.mouse.x;
        mouse_y_ = ev.mouse.y;

        switch (ev.type_) {
        case EventType::MouseDown: {
            if (!ev.mouse.left)
                break;

            MockWindow* hit = hit_test(ev.mouse.x, ev.mouse.y);

            if (hit == nullptr) {
                // No window hit -- check desktop icons
                const DesktopIcon* icon_hit = hit_test_icon(ev.mouse.x, ev.mouse.y);
                if (icon_hit != nullptr) {
                    pending_icon_action_ = icon_hit->action;
                } else {
                    // Clicked on desktop background -- clear focus
                    if (focused_ != nullptr) {
                        focused_->set_focused(false);
                        focused_ = nullptr;
                    }
                }
                break;
            }

            if (hit->is_close_button_hit(ev.mouse.x, ev.mouse.y)) {
                uint32_t dead_id = hit->id();
                destroy(dead_id);
                composite();
                break;
            }

            raise(hit->id());

            int32_t local_y = ev.mouse.y - hit->y();
            if (local_y >= 0 && local_y < static_cast<int32_t>(MockWindow::TITLE_BAR_HEIGHT)) {
                dragging_      = true;
                drag_offset_x_ = ev.mouse.x - hit->x();
                drag_offset_y_ = ev.mouse.y - hit->y();
            }

            composite();
            break;
        }

        case EventType::MouseMove: {
            if (dragging_ && focused_ != nullptr) {
                focused_->set_position(ev.mouse.x - drag_offset_x_, ev.mouse.y - drag_offset_y_);
                composite();
            }
            break;
        }

        case EventType::MouseUp: {
            if (dragging_)
                dragging_ = false;
            break;
        }

        default:
            break;
        }
    }

    void handle_key(Event& ev) {
        if (focused_ != nullptr) {
            (void)ev;  // No-op: MockWindow does not have on_key
        }
    }

    // ---- State accessors ----

    uint32_t    window_count() const { return count_; }
    MockWindow* focused() const { return focused_; }
    int32_t     mouse_x() const { return mouse_x_; }
    int32_t     mouse_y() const { return mouse_y_; }
    uint32_t    icon_count() const { return icon_count_; }
    bool        dragging() const { return dragging_; }

    // Expose pending_icon_action_ for direct inspection
    IconAction pending_icon_action() const { return pending_icon_action_; }

private:
    void on_key(KeyEvent&) { /* no-op base */ }

    MockWindow* find_window(uint32_t id) {
        for (uint32_t i = 0; i < count_; i++) {
            if (windows_[i]->id() == id)
                return windows_[i];
        }
        return nullptr;
    }

    uint32_t find_index(uint32_t id) const {
        for (uint32_t i = 0; i < count_; i++) {
            if (windows_[i]->id() == id)
                return i;
        }
        return MAX_WINDOWS;
    }

    MockWindow* hit_test(int32_t mx, int32_t my) {
        for (uint32_t i = count_; i > 0; i--) {
            uint32_t idx = i - 1;
            if (windows_[idx]->visible() && windows_[idx]->contains(mx, my)) {
                return windows_[idx];
            }
        }
        return nullptr;
    }

    void remove_at(uint32_t idx) {
        if (idx >= count_)
            return;
        bool was_focused = (focused_ != nullptr && focused_->id() == windows_[idx]->id());
        delete windows_[idx];
        windows_[idx] = nullptr;
        for (uint32_t i = idx; i < count_ - 1; i++) {
            windows_[i] = windows_[i + 1];
        }
        windows_[count_ - 1] = nullptr;
        count_--;
        if (was_focused)
            update_focus();
    }

    void update_focus() {
        for (uint32_t i = 0; i < count_; i++) {
            windows_[i]->set_focused(false);
        }
        if (count_ > 0) {
            focused_ = windows_[count_ - 1];
            focused_->set_focused(true);
        } else {
            focused_ = nullptr;
        }
    }

    void draw_desktop_icons(MockCanvas& screen) {
        for (uint32_t i = 0; i < icon_count_; i++) {
            const DesktopIcon& icon = icons_[i];
            if (icon.bitmap != nullptr) {
                screen.draw_bitmap(static_cast<uint32_t>(icon.x), static_cast<uint32_t>(icon.y),
                                   icon.width, icon.height, icon.bitmap);
            }
        }
    }

    MockWindow* windows_[MAX_WINDOWS] = {};
    uint32_t    count_                = 0;
    MockWindow* focused_              = nullptr;

    int32_t mouse_x_ = 0;
    int32_t mouse_y_ = 0;

    bool    dragging_      = false;
    int32_t drag_offset_x_ = 0;
    int32_t drag_offset_y_ = 0;

    DesktopIcon icons_[MAX_ICONS]    = {};
    uint32_t    icon_count_          = 0;
    IconAction  pending_icon_action_ = IconAction::None;

    MockCanvas*  screen_ = nullptr;
    MockPSFFont* font_   = nullptr;
};

// ============================================================
// Helpers
// ============================================================

static MockPSFFont make_test_font() {
    MockPSFFont font;
    font.init(3, 3);
    return font;
}

static void init_wm(MockWindowManager& wm, MockCanvas* screen, MockPSFFont* font) {
    MockWindow::reset_id();
    wm.init(screen, font);
}

/// Helper to build a 32x32 solid-color bitmap on the heap
static std::vector<uint32_t> make_solid_bitmap(uint32_t size, uint32_t color) {
    return std::vector<uint32_t>(size * size, color);
}

/// Helper to create a DesktopIcon at a given position with a solid bitmap
static DesktopIcon make_icon(int32_t x, int32_t y, uint32_t size, IconAction action,
                             const char* label, std::vector<uint32_t>& storage) {
    storage = make_solid_bitmap(size, 0xFF00FF00);  // Green bitmap
    return DesktopIcon{
        .x      = x,
        .y      = y,
        .bitmap = storage.data(),
        .label  = label,
        .width  = size,
        .height = size,
        .action = action,
    };
}

// ============================================================
// add_desktop_icon tests
// ============================================================

// Verify basic add_desktop_icon increments icon_count
TEST("desktop: add_desktop_icon increments icon count") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    ASSERT_EQ(wm.icon_count(), 0u);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(10, 10, 32, IconAction::OpenShell, "Shell", bmp);

    bool ok = wm.add_desktop_icon(icon);
    ASSERT_TRUE(ok);
    ASSERT_EQ(wm.icon_count(), 1u);
}

// Verify adding multiple icons works and increments count
TEST("desktop: add multiple desktop icons") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp1, bmp2, bmp3;
    DesktopIcon           icon1 = make_icon(10, 10, 32, IconAction::OpenShell, "Shell", bmp1);
    DesktopIcon icon2 = make_icon(60, 10, 32, IconAction::OpenCalculator, "Calculator", bmp2);
    DesktopIcon icon3 = make_icon(110, 10, 32, IconAction::None, "Info", bmp3);

    ASSERT_TRUE(wm.add_desktop_icon(icon1));
    ASSERT_EQ(wm.icon_count(), 1u);

    ASSERT_TRUE(wm.add_desktop_icon(icon2));
    ASSERT_EQ(wm.icon_count(), 2u);

    ASSERT_TRUE(wm.add_desktop_icon(icon3));
    ASSERT_EQ(wm.icon_count(), 3u);
}

// Verify add_desktop_icon returns false when MAX_ICONS is reached
TEST("desktop: add_desktop_icon returns false at MAX_ICONS") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(10, 10, 16, IconAction::None, "X", bmp);

    for (uint32_t i = 0; i < MockWindowManager::MAX_ICONS; i++) {
        bool ok = wm.add_desktop_icon(icon);
        ASSERT_TRUE(ok);
    }
    ASSERT_EQ(wm.icon_count(), MockWindowManager::MAX_ICONS);

    // This one should fail
    bool overflow = wm.add_desktop_icon(icon);
    ASSERT_FALSE(overflow);
    ASSERT_EQ(wm.icon_count(), MockWindowManager::MAX_ICONS);
}

// Verify init resets icon_count and pending_icon_action
TEST("desktop: init resets icon state") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(10, 10, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);
    ASSERT_EQ(wm.icon_count(), 1u);

    // Re-init should reset everything
    wm.init(&screen, &font);
    ASSERT_EQ(wm.icon_count(), 0u);
    ASSERT_EQ(wm.pending_icon_action(), IconAction::None);
}

// ============================================================
// hit_test_icon tests
// ============================================================

// Verify hit_test_icon returns the correct icon when clicking inside
TEST("desktop: hit_test_icon returns icon on hit") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(50, 50, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    // Click at (60, 60) -- inside the 32x32 icon at (50,50)
    const DesktopIcon* hit = wm.hit_test_icon(60, 60);
    ASSERT_NOT_NULL(hit);
    ASSERT_EQ(hit->action, IconAction::OpenShell);
}

// Verify hit_test_icon returns nullptr when clicking outside
TEST("desktop: hit_test_icon returns nullptr on miss") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(50, 50, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    // Click far away from the icon
    const DesktopIcon* hit = wm.hit_test_icon(500, 500);
    ASSERT_NULL(hit);
}

// Verify hit_test_icon returns nullptr when no icons registered
TEST("desktop: hit_test_icon returns nullptr with no icons") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    const DesktopIcon* hit = wm.hit_test_icon(100, 100);
    ASSERT_NULL(hit);
}

// Verify hit_test_icon gives priority to later-registered icons on overlap
TEST("desktop: hit_test_icon later icon takes priority on overlap") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp1, bmp2;
    // Two icons overlapping at (50,50)-(81,81)
    DesktopIcon           icon1 = make_icon(50, 50, 32, IconAction::OpenShell, "Shell", bmp1);
    DesktopIcon icon2 = make_icon(60, 60, 32, IconAction::OpenCalculator, "Calculator", bmp2);
    wm.add_desktop_icon(icon1);
    wm.add_desktop_icon(icon2);

    // Click at (70, 70) -- inside both icons, icon2 registered later
    const DesktopIcon* hit = wm.hit_test_icon(70, 70);
    ASSERT_NOT_NULL(hit);
    ASSERT_EQ(hit->action, IconAction::OpenCalculator);
}

// Verify hit_test_icon boundary: clicking exactly on the edge (x + width) misses
TEST("desktop: hit_test_icon boundary edge is miss") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(50, 50, 32, IconAction::None, "X", bmp);
    wm.add_desktop_icon(icon);

    // Click at (82, 60) -- x=50+32=82, which is outside [50, 82)
    const DesktopIcon* hit = wm.hit_test_icon(82, 60);
    ASSERT_NULL(hit);

    // Click at (60, 82) -- y=50+32=82, which is outside [50, 82)
    hit = wm.hit_test_icon(60, 82);
    ASSERT_NULL(hit);

    // Click at (81, 81) -- last pixel inside the icon
    hit = wm.hit_test_icon(81, 81);
    ASSERT_NOT_NULL(hit);
}

// Verify hit_test_icon works at origin (0, 0)
TEST("desktop: hit_test_icon works at screen origin") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(0, 0, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    const DesktopIcon* hit = wm.hit_test_icon(0, 0);
    ASSERT_NOT_NULL(hit);

    hit = wm.hit_test_icon(15, 15);
    ASSERT_NOT_NULL(hit);
}

// ============================================================
// consume_pending_icon_action tests
// ============================================================

// Verify consume returns None when no action is pending
TEST("desktop: consume_pending_icon_action returns None when empty") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    IconAction action = wm.consume_pending_icon_action();
    ASSERT_EQ(static_cast<int>(action), static_cast<int>(IconAction::None));
}

// Verify consume resets the pending action to None
TEST("desktop: consume_pending_icon_action resets to None") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    // Manually set a pending action (simulating what handle_mouse does)
    // We use handle_mouse to trigger this path instead
    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(50, 50, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    // Click on the icon to set pending action
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 60;
    ev.mouse.y      = 60;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;
    wm.handle_mouse(ev);

    // First consume returns the action
    IconAction action1 = wm.consume_pending_icon_action();
    ASSERT_EQ(static_cast<int>(action1), static_cast<int>(IconAction::OpenShell));

    // Second consume returns None (action was reset)
    IconAction action2 = wm.consume_pending_icon_action();
    ASSERT_EQ(static_cast<int>(action2), static_cast<int>(IconAction::None));
}

// Verify multiple consumes without intervening clicks all return None
TEST("desktop: multiple consumes without click all return None") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    for (int i = 0; i < 5; i++) {
        IconAction a = wm.consume_pending_icon_action();
        ASSERT_EQ(static_cast<int>(a), static_cast<int>(IconAction::None));
    }
}

// ============================================================
// Icon click sets action via handle_mouse
// ============================================================

// Verify clicking a desktop icon sets pending_icon_action to the icon's action
TEST("desktop: icon click sets pending_icon_action") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(50, 50, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    // Before click, no pending action
    ASSERT_EQ(wm.pending_icon_action(), IconAction::None);

    // Click on the icon
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 60;
    ev.mouse.y      = 60;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;
    wm.handle_mouse(ev);

    // Pending action should be set to the icon's action
    ASSERT_EQ(wm.pending_icon_action(), IconAction::OpenShell);
}

// Verify clicking different icons sets different actions
TEST("desktop: different icons set different actions") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp1, bmp2;
    DesktopIcon           icon1 = make_icon(10, 10, 32, IconAction::OpenShell, "Shell", bmp1);
    DesktopIcon icon2 = make_icon(60, 10, 32, IconAction::OpenCalculator, "Calculator", bmp2);
    wm.add_desktop_icon(icon1);
    wm.add_desktop_icon(icon2);

    // Click on icon1 (Shell)
    Event ev1{};
    ev1.type_      = EventType::MouseDown;
    ev1.mouse.x    = 20;
    ev1.mouse.y    = 20;
    ev1.mouse.left = true;
    wm.handle_mouse(ev1);
    ASSERT_EQ(wm.pending_icon_action(), IconAction::OpenShell);

    // Consume it
    wm.consume_pending_icon_action();

    // Click on icon2 (Calculator)
    Event ev2{};
    ev2.type_      = EventType::MouseDown;
    ev2.mouse.x    = 70;
    ev2.mouse.y    = 20;
    ev2.mouse.left = true;
    wm.handle_mouse(ev2);
    ASSERT_EQ(wm.pending_icon_action(), IconAction::OpenCalculator);
}

// Verify icon click with an icon that has IconAction::None sets None
TEST("desktop: icon click with None action sets None") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(10, 10, 32, IconAction::None, "Info", bmp);
    wm.add_desktop_icon(icon);

    Event ev{};
    ev.type_      = EventType::MouseDown;
    ev.mouse.x    = 20;
    ev.mouse.y    = 20;
    ev.mouse.left = true;
    wm.handle_mouse(ev);

    ASSERT_EQ(wm.pending_icon_action(), IconAction::None);
}

// ============================================================
// Desktop blank click does not set action
// ============================================================

// Verify clicking desktop background does not set pending_icon_action
TEST("desktop: desktop blank click does not set action") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    // No icons, no windows -- just desktop
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 400;
    ev.mouse.y      = 300;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;
    wm.handle_mouse(ev);

    ASSERT_EQ(wm.pending_icon_action(), IconAction::None);
}

// Verify clicking desktop with icons present but missing them does not set action
TEST("desktop: clicking empty area near icons does not set action") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(10, 10, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    // Click between icons (far from the icon at (10,10)-(41,41))
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 400;
    ev.mouse.y      = 300;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;
    wm.handle_mouse(ev);

    ASSERT_EQ(wm.pending_icon_action(), IconAction::None);
}

// ============================================================
// Icon click with a window on top -- window gets click, not icon
// ============================================================

// Verify clicking on a window that overlaps an icon does NOT set icon action
TEST("desktop: window on top of icon prevents icon click") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    // Add an icon at (50, 50)
    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(50, 50, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    // Create a window that overlaps the icon region
    // Window at stagger (0, 0), size 100x100, so covers (0,0)-(99,119)
    wm.create("Overlap", 100, 100);

    // Click at (60, 60) -- inside both window and icon, but window is on top
    Event ev{};
    ev.type_        = EventType::MouseDown;
    ev.mouse.x      = 60;
    ev.mouse.y      = 60;
    ev.mouse.left   = true;
    ev.mouse.right  = false;
    ev.mouse.middle = false;
    wm.handle_mouse(ev);

    // Icon action should NOT be set (window got the click)
    ASSERT_EQ(wm.pending_icon_action(), IconAction::None);
}

// ============================================================
// Icon click clears window focus (icon click goes through desktop path)
// ============================================================

// Verify icon click does NOT clear window focus (only desktop blank does)
TEST("desktop: icon click preserves window focus") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    // Create a window (it will be focused)
    uint32_t win_id = wm.create("Win", 200, 200);
    (void)win_id;
    ASSERT_NOT_NULL(wm.focused());

    // Add an icon at (400, 400) -- outside the window at (0,0)
    std::vector<uint32_t> bmp;
    DesktopIcon           icon = make_icon(400, 400, 32, IconAction::OpenShell, "Shell", bmp);
    wm.add_desktop_icon(icon);

    // Click on the icon (no window hit -> icon hit path)
    Event ev{};
    ev.type_      = EventType::MouseDown;
    ev.mouse.x    = 410;
    ev.mouse.y    = 410;
    ev.mouse.left = true;
    wm.handle_mouse(ev);

    // Focus should remain on the window (icon hit does not clear focus)
    ASSERT_NOT_NULL(wm.focused());
    ASSERT_EQ(wm.pending_icon_action(), IconAction::OpenShell);
}

// ============================================================
// Composite draws icon bitmaps
// ============================================================

// Verify composite renders icon bitmap pixels onto the screen
TEST("desktop: composite renders icon bitmap") {
    MockCanvas screen;
    screen.init(800, 600);
    MockPSFFont font = make_test_font();

    MockWindowManager wm;
    init_wm(wm, &screen, &font);

    uint32_t              icon_color = 0xFF00FF00;
    std::vector<uint32_t> bmp(32 * 32, icon_color);
    DesktopIcon           icon{
                  .x      = 10,
                  .y      = 10,
                  .bitmap = bmp.data(),
                  .label  = "Test",
                  .width  = 32,
                  .height = 32,
                  .action = IconAction::None,
    };
    wm.add_desktop_icon(icon);

    wm.composite();

    // Check that a pixel inside the icon area has the icon color
    ASSERT_EQ(screen.pixel(20, 20), icon_color);

    // Check that a pixel outside the icon area has the desktop color
    ASSERT_EQ(screen.pixel(50, 50), MockWindowManager::DESKTOP_COLOR);
}

// ============================================================
// Main entry point
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
