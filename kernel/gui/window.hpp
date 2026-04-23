/**
 * @file kernel/gui/window.hpp
 * @brief GUI Window class declaration
 *
 * Represents a single on-screen window with a title bar, close button,
 * and a content area backed by an off-screen canvas for double-buffered
 * rendering.  The WindowManager (sub-iteration C) will own and manage
 * collections of Window instances.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

#include <stdint.h>

#include "kernel/drivers/canvas.hpp"
#include "kernel/gui/event.hpp"

namespace cinux::gui {

// ============================================================
// Window class
// ============================================================

/**
 * @brief A single GUI window with title bar and content area
 *
 * Each Window owns an off-screen Canvas (w_ x h_) for double-buffered
 * rendering.  The title bar occupies an additional TITLE_BAR_HEIGHT
 * pixels above the content area.
 *
 * Window IDs are assigned automatically from a static counter so that
 * the WindowManager can uniquely identify windows without maintaining
 * its own ID scheme.
 */
class Window {
public:
    // ============================================================
    // Constants
    // ============================================================

    static constexpr uint32_t TITLE_BAR_HEIGHT = 20;
    static constexpr uint32_t CLOSE_BUTTON_SIZE = 14;
    static constexpr uint32_t TITLE_MAX_LEN    = 63;
    static constexpr uint32_t DEFAULT_WIDTH    = 320;
    static constexpr uint32_t DEFAULT_HEIGHT   = 240;

    // ============================================================
    // Title bar colours (0x00RRGGBB)
    // ============================================================

    static constexpr uint32_t COLOR_TITLE_BG     = 0x00336699;  // Steel blue
    static constexpr uint32_t COLOR_TITLE_TEXT   = 0x00FFFFFF;  // White
    static constexpr uint32_t COLOR_CLOSE_BUTTON = 0x00CC3333;  // Red
    static constexpr uint32_t COLOR_CONTENT_BG   = 0x00E0E0E0;  // Light grey
    static constexpr uint32_t COLOR_BORDER       = 0x00444444;  // Dark grey

    // ============================================================
    // Construction / destruction
    // ============================================================

    /**
     * @brief Construct a window with default size and title
     *
     * @param title  Window title string (truncated to TITLE_MAX_LEN chars)
     * @param x      Initial X position on screen (pixels)
     * @param y      Initial Y position on screen (pixels)
     * @param w      Content area width in pixels
     * @param h      Content area height in pixels (excludes title bar)
     */
    Window(const char* title = "Untitled",
           int32_t x = 0, int32_t y = 0,
           uint32_t w = DEFAULT_WIDTH,
           uint32_t h = DEFAULT_HEIGHT);

    virtual ~Window() = default;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // ============================================================
    // Virtual event handlers (override in subclasses)
    // ============================================================

    /**
     * @brief Handle a keyboard event
     *
     * Called by the WindowManager when this window has input focus.
     * The default implementation does nothing.
     *
     * @param ev  The keyboard event
     */
    virtual void on_key(KeyEvent& ev) { (void)ev; }

    /**
     * @brief Handle a paint request
     *
     * Called by the WindowManager when this window needs to be redrawn.
     * The default implementation does nothing.
     *
     * @param canvas  The canvas to paint onto
     */
    virtual void on_paint(cinux::drivers::Canvas& canvas) { (void)canvas; }

    // ============================================================
    // Drawing
    // ============================================================

    /**
     * @brief Draw the title bar onto the window's canvas
     *
     * Renders: blue background, white title text, red close button
     * (small square in the top-right corner).
     *
     * @param font  Reference to an initialised PSFFont for text rendering
     */
    void draw_title_bar(cinux::drivers::PSFFont& font);

    /**
     * @brief Clear the content area with the default background colour
     */
    void draw_content();

    /**
     * @brief Blit the entire window (title bar + content) onto a
     *        destination canvas at the window's screen position
     *
     * @param dst  Destination canvas (typically the screen canvas)
     */
    void blit_to(cinux::drivers::Canvas& dst);

    // ============================================================
    // Geometry
    // ============================================================

    /**
     * @brief Set the window's screen position
     * @param x  New X coordinate
     * @param y  New Y coordinate
     */
    void set_position(int32_t x, int32_t y);

    /**
     * @brief Resize the window's content area
     *
     * Reallocates the off-screen canvas.  Existing content is lost.
     *
     * @param w  New content width in pixels
     * @param h  New content height in pixels (excludes title bar)
     */
    void resize(uint32_t w, uint32_t h);

    /**
     * @brief Set the window title
     * @param title  New title string (truncated to TITLE_MAX_LEN chars)
     */
    void set_title(const char* title);

    // ============================================================
    // Hit testing
    // ============================================================

    /**
     * @brief Check whether a screen coordinate hits the close button
     *
     * The close button is located in the top-right corner of the
     * title bar.  Coordinates are in screen space.
     *
     * @param mx  Mouse X in screen coordinates
     * @param my  Mouse Y in screen coordinates
     * @return    true if the point is inside the close button area
     */
    bool is_close_button_hit(int32_t mx, int32_t my) const;

    /**
     * @brief Check whether a screen coordinate is inside the window
     *
     * @param mx  Mouse X in screen coordinates
     * @param my  Mouse Y in screen coordinates
     * @return    true if the point is inside the window bounds
     */
    bool contains(int32_t mx, int32_t my) const;

    // ============================================================
    // State accessors
    // ============================================================

    uint32_t id() const { return id_; }
    int32_t  x() const { return x_; }
    int32_t  y() const { return y_; }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    bool     visible() const { return visible_; }
    bool     focused() const { return focused_; }
    const char* title() const { return title_; }

    /**
     * @brief Access the window's off-screen canvas for custom drawing
     * @return Reference to the internal Canvas
     */
    cinux::drivers::Canvas& canvas() { return canvas_; }

    void set_visible(bool v) { visible_ = v; }
    void set_focused(bool f) { focused_ = f; }

    /**
     * @brief Total window height including the title bar
     * @return height_ + TITLE_BAR_HEIGHT
     */
    uint32_t total_height() const { return h_ + TITLE_BAR_HEIGHT; }

private:
    // ============================================================
    // Members
    // ============================================================

    static uint32_t next_id_;  ///< Auto-incrementing window ID counter

    uint32_t id_;             ///< Unique window identifier
    int32_t  x_;              ///< Screen X position
    int32_t  y_;              ///< Screen Y position
    uint32_t w_;              ///< Content area width
    uint32_t h_;              ///< Content area height
    char     title_[TITLE_MAX_LEN + 1];  ///< Null-terminated title
    cinux::drivers::Canvas canvas_;      ///< Off-screen back buffer
    bool     visible_;        ///< Whether the window should be rendered
    bool     focused_;        ///< Whether the window has input focus

    // ============================================================
    // Internal helpers
    // ============================================================

    /**
     * @brief (Re-)allocate the off-screen canvas for current w_ x h_
     */
    void allocate_canvas();
};

}  // namespace cinux::gui
