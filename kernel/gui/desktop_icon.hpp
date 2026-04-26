/**
 * @file kernel/gui/desktop_icon.hpp
 * @brief Desktop icon data structures and hit testing
 *
 * Defines the IconAction enum (what happens when a desktop icon is
 * activated) and the DesktopIcon struct that bundles position, bitmap,
 * label, and action.  Provides an inline hit-test method so the
 * window manager can quickly determine which icon (if any) was clicked.
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui
 */

#pragma once

#include <cstdint>

namespace cinux::gui {

// ============================================================
// Icon action enumeration
// ============================================================

/**
 * @brief Describes what action a desktop icon triggers on activation
 *
 * Used by the window manager to dispatch the correct response
 * (e.g. opening a shell window, launching the calculator).
 */
enum class IconAction : uint8_t {
    None           = 0,  ///< Placeholder / no action
    OpenShell      = 1,  ///< Open a terminal shell window
    OpenCalculator = 2,  ///< Open a calculator window
};

// ============================================================
// Desktop icon structure
// ============================================================

/**
 * @brief Represents a single clickable icon on the desktop
 *
 * Each desktop icon has a position, a pixel bitmap for rendering,
 * a text label, and an associated action that fires on double-click.
 *
 * Usage:
 *   DesktopIcon icon{
 *       .x = 10, .y = 10,
 *       .bitmap = icons::k_shell_icon.data(),
 *       .label = "Shell",
 *       .width = icons::ICON_SIZE,
 *       .height = icons::ICON_SIZE,
 *       .action = IconAction::OpenShell,
 *   };
 *   if (icon.contains(mouse_x, mouse_y)) { ... }
 */
struct DesktopIcon {
    int32_t         x;       ///< Icon left edge in screen pixels
    int32_t         y;       ///< Icon top edge in screen pixels
    const uint32_t* bitmap;  ///< Pointer to pixel data (w*h uint32_t values)
    const char*     label;   ///< Null-terminated icon label string
    uint32_t        width;   ///< Bitmap width in pixels
    uint32_t        height;  ///< Bitmap height in pixels
    IconAction      action;  ///< Action to perform on activation

    /**
     * @brief Check whether a screen coordinate falls within the icon bounds
     *
     * Tests the rectangular area defined by (x, y, width, height).
     *
     * @param mx  Mouse X in screen coordinates
     * @param my  Mouse Y in screen coordinates
     * @return    true if the point is inside the icon's bounding rectangle
     */
    [[nodiscard]] bool contains(int32_t mx, int32_t my) const {
        return mx >= x && mx < static_cast<int32_t>(x + width) && my >= y &&
               my < static_cast<int32_t>(y + height);
    }
};

}  // namespace cinux::gui
