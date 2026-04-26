/**
 * @file kernel/drivers/video/framebuffer.hpp
 * @brief Linear framebuffer driver for VBE/VESA graphics output
 *
 * Provides basic drawing primitives (put_pixel, fill_rect, scroll_up,
 * clear) on a linear framebuffer whose physical address, dimensions,
 * and pixel format are supplied by the bootloader via BootInfo.
 *
 * The framebuffer is mapped into the virtual address space during init
 * using map_mmio().  Pixel format is assumed to be 32-bit ARGB/XRGB
 * (8 bits per channel, 4 bytes per pixel).
 *
 * Usage:
 *   Framebuffer fb;
 *   fb.init(boot_info);
 *   fb.clear(0x00336699);   // fill with dark blue
 *   fb.put_pixel(100, 50, 0x00FFFFFF);  // white dot
 *
 * Namespace: cinux::drivers
 */

#pragma once

#include <stdint.h>

#include "boot/boot_info.h"

namespace cinux::drivers {

class Framebuffer {
public:
    /**
     * @brief Initialise the framebuffer from bootloader info
     *
     * Maps the physical framebuffer address into the virtual address
     * space and stores dimensions for subsequent drawing operations.
     *
     * @param bi  BootInfo from bootloader (contains fb_addr, dimensions)
     */
    void init(const BootInfo& bi);

    /**
     * @brief Write a single pixel to the framebuffer
     *
     * @param x      Column (0 = left)
     * @param y      Row (0 = top)
     * @param argb   Pixel colour in 0x00RRGGBB format
     */
    void put_pixel(uint32_t x, uint32_t y, uint32_t argb);

    /**
     * @brief Fill a rectangular area with a solid colour
     *
     * @param x      Left edge
     * @param y      Top edge
     * @param w      Width in pixels
     * @param h      Height in pixels
     * @param argb   Fill colour in 0x00RRGGBB format
     */
    void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);

    /**
     * @brief Scroll the framebuffer contents upward by the given number
     *        of pixel lines
     *
     * Moves all rows up by @p lines pixels.  The bottom @p lines rows
     * are cleared to @p bg colour.
     *
     * @param lines       Number of pixel rows to scroll up
     * @param line_height Height of the cleared band at the bottom
     * @param bg          Background colour for the cleared area
     */
    void scroll_up(uint32_t lines, uint32_t line_height, uint32_t bg);

    /**
     * @brief Clear the entire framebuffer to a solid colour
     *
     * @param argb  Fill colour (default: black = 0)
     */
    void clear(uint32_t argb = 0);

    /**
     * @brief Read a single pixel from the framebuffer
     *
     * @param x      Column (0 = left)
     * @param y      Row (0 = top)
     * @return       Pixel colour in 0x00RRGGBB format, or 0 if out of bounds
     */
    uint32_t get_pixel(uint32_t x, uint32_t y) const;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t pitch() const { return pitch_; }

    /**
     * @brief Access the raw framebuffer memory pointer
     *
     * Returns the base address of the linear framebuffer mapped into
     * virtual address space.  Used by Canvas::flip() for bulk transfer.
     *
     * @return Pointer to the first pixel (volatile uint32_t*)
     */
    volatile uint32_t* data() const { return addr_; }

private:
    volatile uint32_t* addr_   = nullptr;
    uint32_t           width_  = 0;
    uint32_t           height_ = 0;
    uint32_t           pitch_  = 0;  // bytes per scan line
    uint32_t           bpp_    = 0;
};

}  // namespace cinux::drivers