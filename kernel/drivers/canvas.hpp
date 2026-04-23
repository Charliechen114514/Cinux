/**
 * @file kernel/drivers/canvas.hpp
 * @brief Double-buffered canvas for GUI rendering
 *
 * Provides a back-buffer abstraction over a hardware Framebuffer.
 * All drawing operations target the back buffer; flip() copies the
 * completed frame to the front (hardware) buffer in one pass.
 *
 * This class is only compiled when CINUX_GUI is defined (controlled
 * by the CINUX_GUI CMake option).
 *
 * Usage:
 *   Canvas canvas;
 *   canvas.init(fb);
 *   canvas.draw_rect(10, 20, 100, 50, 0x00FF0000);
 *   canvas.flip();
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

namespace cinux::drivers {

class Framebuffer;
class PSFFont;

class Canvas {
public:
    Canvas() = default;

    ~Canvas() {
        if (back_buf_ != nullptr) {
            delete[] back_buf_;
            back_buf_ = nullptr;
        }
    }

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    /**
     * @brief Initialise the canvas from a hardware framebuffer
     *
     * Stores a reference to the front buffer and allocates a
     * back buffer of the same pixel dimensions.
     *
     * @param fb  Reference to an initialised Framebuffer
     */
    void init(Framebuffer& fb);

    /**
     * @brief Initialise a standalone canvas without a hardware framebuffer
     *
     * Allocates a back buffer of the specified dimensions.  The front
     * buffer is left as nullptr, so flip() is a no-op.  Useful for
     * off-screen rendering (e.g. window content areas) that are later
     * blitted onto a screen canvas.
     *
     * If a back buffer was previously allocated it is freed first.
     *
     * @param w  Canvas width in pixels
     * @param h  Canvas height in pixels
     */
    void init(uint32_t w, uint32_t h);

    /**
     * @brief Write a single pixel to the back buffer
     *
     * @param x      Column (0 = left)
     * @param y      Row (0 = top)
     * @param color  Pixel colour in 0x00RRGGBB format
     */
    void draw_pixel(uint32_t x, uint32_t y, uint32_t color);

    /**
     * @brief Fill a rectangular area on the back buffer
     *
     * @param x      Left edge
     * @param y      Top edge
     * @param w      Width in pixels
     * @param h      Height in pixels
     * @param color  Fill colour in 0x00RRGGBB format
     */
    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

    /**
     * @brief Draw a rectangular outline on the back buffer
     *
     * Draws four 1-pixel-wide lines forming the rectangle border.
     *
     * @param x      Left edge
     * @param y      Top edge
     * @param w      Width in pixels
     * @param h      Height in pixels
     * @param color  Border colour in 0x00RRGGBB format
     */
    void draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

    /**
     * @brief Copy the back buffer to the front (hardware) buffer
     *
     * Performs a row-by-row memcpy from back_buf to the framebuffer,
     * respecting the pitch (bytes per scan line).
     */
    void flip();

    /**
     * @brief Clear the entire back buffer to a solid colour
     *
     * @param color  Fill colour (default: black = 0)
     */
    void clear(uint32_t color = 0);

    /**
     * @brief Draw a line between two points using Bresenham's algorithm
     *
     * @param x0     Start X coordinate
     * @param y0     Start Y coordinate
     * @param x1     End X coordinate
     * @param y1     End Y coordinate
     * @param color  Line colour in 0x00RRGGBB format
     */
    void draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                   uint32_t color);

    /**
     * @brief Render a null-terminated string to the back buffer
     *
     * Uses PSFFont glyph data to render each character via draw_pixel.
     * Characters that fall outside the canvas bounds are clipped.
     *
     * @param x      Left edge of the first character
     * @param y      Top edge of the first character
     * @param str    Null-terminated string to render
     * @param color  Text colour in 0x00RRGGBB format
     * @param font   Reference to an initialised PSFFont
     */
    void draw_text(uint32_t x, uint32_t y, const char* str, uint32_t color,
                   PSFFont& font);

    /**
     * @brief Copy a rectangular region from another canvas to this canvas
     *
     * Copies a sub-region of the source canvas to a position on this canvas.
     * Both source and destination coordinates are clamped to their respective
     * canvas bounds.
     *
     * @param dst_x  Destination X coordinate on this canvas
     * @param dst_y  Destination Y coordinate on this canvas
     * @param src    Source canvas to copy from
     * @param sx     Source region left edge
     * @param sy     Source region top edge
     * @param w      Width of the region to copy
     * @param h      Height of the region to copy
     */
    void blit(uint32_t dst_x, uint32_t dst_y, Canvas& src,
              uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pitch() const { return pitch_; }

private:
    Framebuffer* front_buf_ = nullptr;
    uint32_t*    back_buf_  = nullptr;
    uint32_t     width_     = 0;
    uint32_t     height_    = 0;
    uint32_t     pitch_     = 0;  // bytes per scan line
};

}  // namespace cinux::drivers
