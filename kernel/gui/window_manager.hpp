/**
 * @file kernel/gui/window_manager.hpp
 * @brief Window manager with Z-ordering, compositing, and drag support
 *
 * Owns a fixed-capacity array of heap-allocated Window pointers and
 * provides the core operations expected of a basic window manager:
 * creation, destruction, raise-to-front, Z-ordered compositing onto
 * the screen canvas, and interactive mouse-driven drag.
 *
 * Windows are stored as pointers so that Z-order reordering (raise,
 * remove) only requires shuffling pointers, not moving non-copyable
 * Window objects.  The WindowManager owns all Window lifetimes.
 *
 * Hit testing is performed from the highest Z-order window downward so
 * that the topmost visible window always receives input first.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

#include <stdint.h>

#include "kernel/drivers/canvas.hpp"
#include "kernel/gui/event.hpp"
#include "kernel/gui/window.hpp"

namespace cinux::gui {

// ============================================================
// WindowManager class
// ============================================================

/**
 * @brief Manages a collection of Window instances with compositing and input routing
 *
 * The window manager owns up to MAX_WINDOWS heap-allocated Window
 * objects referenced via a pointer array.  Index 0 is the bottom-most
 * window; index (count_ - 1) is the top-most.  All screen-space hit
 * testing iterates from the top down.
 *
 * A pointer to the screen Canvas is supplied via init() so that
 * composite() can blit every visible window and flip the result
 * to the hardware framebuffer.
 */
class WindowManager {
public:
    // ============================================================
    // Constants
    // ============================================================

    static constexpr uint32_t MAX_WINDOWS    = 64;
    static constexpr uint32_t DESKTOP_COLOR  = 0x00224466;  // Dark teal desktop

    // ============================================================
    // Singleton access
    // ============================================================

    /**
     * @brief Get the singleton WindowManager instance
     *
     * There is exactly one window manager in the system.  This returns
     * a reference to the static instance, which is zero-initialised at
     * program start and explicitly initialised via init().
     *
     * @return Reference to the global WindowManager
     */
    static WindowManager& instance();

    // ============================================================
    // Construction / destruction
    // ============================================================

    WindowManager() = default;

    /**
     * @brief Destructor -- destroys all remaining windows
     */
    ~WindowManager();

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    // ============================================================
    // Lifecycle
    // ============================================================

    /**
     * @brief Initialise the window manager with a screen canvas and font
     *
     * The screen canvas is used for compositing (blit + flip).
     * The font is used when drawing title bars for new windows.
     *
     * @param screen  Pointer to the screen canvas (must outlive this object)
     * @param font    Pointer to an initialised PSFFont for title bar text
     */
    void init(cinux::drivers::Canvas* screen, cinux::drivers::PSFFont* font);

    // ============================================================
    // Window management
    // ============================================================

    /**
     * @brief Create a new top-level window and place it at the top of Z-order
     *
     * The new window is drawn (title bar + content) immediately and
     * given input focus.
     *
     * @param title  Window title (truncated to Window::TITLE_MAX_LEN)
     * @param w      Content area width in pixels
     * @param h      Content area height in pixels (excludes title bar)
     * @return       ID of the created window, or 0 on failure (max reached)
     */
    uint32_t create(const char* title, uint32_t w, uint32_t h);

    /**
     * @brief Destroy a window by its ID
     *
     * Removes the window from the internal array, shifting subsequent
     * entries down to preserve Z-order.  If the destroyed window was
     * focused, focus transfers to the new top-most window.
     *
     * @param id  Window ID to destroy
     */
    void destroy(uint32_t id);

    /**
     * @brief Raise a window to the top of the Z-order and give it focus
     *
     * @param id  Window ID to raise
     */
    void raise(uint32_t id);

    // ============================================================
    // Compositing
    // ============================================================

    /**
     * @brief Composite all visible windows onto the screen and flip
     *
     * Clears the screen back buffer with the desktop colour, then blits
     * each window from lowest Z-order to highest.  Finally calls
     * flip() on the screen canvas to present the frame.
     */
    void composite();

    // ============================================================
    // Input handling
    // ============================================================

    /**
     * @brief Process a mouse event
     *
     * Handles:
     *   - MouseDown on close button -> destroy the window
     *   - MouseDown on title bar -> begin drag
     *   - MouseDown on content area -> raise the window
     *   - MouseMove while dragging -> update focused window position
     *   - MouseUp -> end drag
     *
     * @param ev  The mouse event to process
     */
    void handle_mouse(Event& ev);

    /**
     * @brief Process a keyboard event (reserved for future use)
     *
     * @param ev  The keyboard event to process
     */
    void handle_key(Event& ev);

    // ============================================================
    // State accessors
    // ============================================================

    uint32_t window_count() const { return count_; }

    /**
     * @brief Get the currently focused window, or nullptr if none
     */
    Window* focused() const { return focused_; }

    int32_t mouse_x() const { return mouse_x_; }
    int32_t mouse_y() const { return mouse_y_; }

private:
    // ============================================================
    // Internal helpers
    // ============================================================

    /**
     * @brief Find a window by its ID
     *
     * @param id  Window ID to search for
     * @return    Pointer to the window, or nullptr if not found
     */
    Window* find_window(uint32_t id);

    /**
     * @brief Find the index of a window by its ID
     *
     * @param id  Window ID to search for
     * @return    Array index, or MAX_WINDOWS if not found
     */
    uint32_t find_index(uint32_t id) const;

    /**
     * @brief Find the top-most window that contains the given screen point
     *
     * Iterates from the highest Z-order (count_ - 1) downward.
     *
     * @param mx  Screen X coordinate
     * @param my  Screen Y coordinate
     * @return    Pointer to the hit window, or nullptr if none
     */
    Window* hit_test(int32_t mx, int32_t my);

    /**
     * @brief Remove a window at a given array index, shifting later entries down
     *
     * @param idx  Array index of the window to remove
     */
    void remove_at(uint32_t idx);

    /**
     * @brief Update the focused window pointer to the top-most visible window
     */
    void update_focus();

    /**
     * @brief Draw the mouse cursor onto the screen canvas
     *
     * Renders a 16x16 classic arrow cursor at the current mouse position
     * (Mouse::x(), Mouse::y()).  Pixels outside the screen bounds are
     * automatically clipped by Canvas::draw_pixel.
     *
     * @param screen  The screen canvas to draw onto
     */
    void draw_cursor(cinux::drivers::Canvas& screen);

    // ============================================================
    // Cursor bitmap
    // ============================================================

    static constexpr uint32_t CURSOR_SIZE    = 16;
    static constexpr uint32_t CURSOR_WHITE   = 0x00FFFFFF;
    static constexpr uint32_t CURSOR_BLACK   = 0x00000000;

    /**
     * 16x16 classic arrow cursor bitmap (MSB-first per row).
     *
     * Each uint16_t encodes one row: bit 15 = leftmost pixel.
     *   1 = white fill pixel
     *   0 = transparent (skip)
     */
    static constexpr uint16_t k_cursor_bitmap[CURSOR_SIZE] = {
        0x8000,  // X . . . . . . . . . . . . . . .
        0xC000,  // X X . . . . . . . . . . . . . .
        0xE000,  // X X X . . . . . . . . . . . . .
        0xF000,  // X X X X . . . . . . . . . . . .
        0xF800,  // X X X X X . . . . . . . . . . .
        0xFC00,  // X X X X X X . . . . . . . . . .
        0xFE00,  // X X X X X X X . . . . . . . . .
        0xFF00,  // X X X X X X X X . . . . . . . .
        0xFF00,  // X X X X X X X X . . . . . . . .
        0xF800,  // X X X X X . . . . . . . . . . .
        0xE000,  // X X X . . . . . . . . . . . . .
        0xC000,  // X X . . . . . . . . . . . . . .
        0x8800,  // X . X . . . . . . . . . . . . .
        0x0800,  // . . X . . . . . . . . . . . . .
        0x0000,  // . . . . . . . . . . . . . . . .
        0x0000,  // . . . . . . . . . . . . . . . .
    };

    // ============================================================
    // Members
    // ============================================================

    Window* windows_[MAX_WINDOWS] = {};  ///< Z-ordered window pointer array
    uint32_t count_ = 0;                ///< Number of active windows
    Window* focused_ = nullptr;         ///< Currently focused window

    int32_t mouse_x_ = 0;              ///< Last known mouse X
    int32_t mouse_y_ = 0;              ///< Last known mouse Y

    // Drag state
    bool    dragging_      = false;    ///< True while a title-bar drag is active
    int32_t drag_offset_x_ = 0;        ///< Mouse X offset from window origin at drag start
    int32_t drag_offset_y_ = 0;        ///< Mouse Y offset from window origin at drag start

    // External dependencies (not owned)
    cinux::drivers::Canvas*  screen_ = nullptr;  ///< Screen canvas for compositing
    cinux::drivers::PSFFont* font_   = nullptr;  ///< Font for title bar rendering
};

}  // namespace cinux::gui
