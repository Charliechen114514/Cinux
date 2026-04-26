/**
 * @file kernel/drivers/video/font.cpp
 * @brief PSF2 font parser and character renderer implementation
 */

#include "font.hpp"

#include <stdint.h>

#include "framebuffer.hpp"

// Symbols defined in font_data.S (.incbin embedding)
extern "C" {
extern const uint8_t  font_psf_start[];
extern const uint8_t  font_psf_end[];
extern const uint32_t font_psf_size[];
}

// PSF2 header structure (packed, little-endian)
struct PSF2Header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t length;
    uint32_t charsize;
    uint32_t height;
    uint32_t width;
} __attribute__((packed));

static constexpr uint32_t PSF2_MAGIC = 0x864AB572;

namespace cinux::drivers {

void PSFFont::init() {
    const auto* hdr = reinterpret_cast<const PSF2Header*>(font_psf_start);

    if (hdr->magic != PSF2_MAGIC)
        return;

    num_glyphs_      = hdr->length;
    bytes_per_glyph_ = hdr->charsize;
    width_           = hdr->width;
    height_          = hdr->height;
    glyphs_          = font_psf_start + hdr->header_size;
}

void PSFFont::render_char(Framebuffer& fb, uint8_t c, uint32_t x, uint32_t y, uint32_t fg,
                          uint32_t bg) {
    if (glyphs_ == nullptr)
        return;
    if (c >= num_glyphs_)
        c = 0;

    const uint8_t* glyph = glyphs_ + static_cast<uint32_t>(c) * bytes_per_glyph_;

    for (uint32_t row = 0; row < height_; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < width_; col++) {
            bool on = (bits >> (7 - col)) & 1;
            fb.put_pixel(x + col, y + row, on ? fg : bg);
        }
    }
}

const uint8_t* PSFFont::glyph(uint8_t c) const {
    if (glyphs_ == nullptr)
        return nullptr;
    if (c >= num_glyphs_)
        c = 0;
    return glyphs_ + static_cast<uint32_t>(c) * bytes_per_glyph_;
}

}  // namespace cinux::drivers
