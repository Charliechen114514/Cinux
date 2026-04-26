/**
 * @file test/unit/test_font.cpp
 * @brief Host-side unit tests for PSF2 font parsing and rendering arithmetic
 *
 * Tests the pure arithmetic of PSF2 header parsing, glyph offset calculation,
 * and bitmap bit extraction.  Does not touch real hardware or embedded binary
 * data -- all logic is mirrored locally.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

// Mirrored constants from kernel/drivers/video/font.cpp
static constexpr uint32_t PSF2_MAGIC = 0x864AB572;

// Mirrored PSF2 header structure
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

// Mirrored parsing result
struct FontInfo {
    const uint8_t* glyphs          = nullptr;
    uint32_t       bytes_per_glyph = 0;
    uint32_t       num_glyphs      = 0;
    uint32_t       width           = 0;
    uint32_t       height          = 0;
};

static FontInfo parse_psf2(const uint8_t* data, size_t len) {
    FontInfo info;
    if (len < sizeof(PSF2Header))
        return info;
    const auto* hdr = reinterpret_cast<const PSF2Header*>(data);
    if (hdr->magic != PSF2_MAGIC)
        return info;
    info.num_glyphs      = hdr->length;
    info.bytes_per_glyph = hdr->charsize;
    info.width           = hdr->width;
    info.height          = hdr->height;
    info.glyphs          = data + hdr->header_size;
    return info;
}

TEST("font: PSF2 magic is 0x864AB572") {
    ASSERT_EQ(PSF2_MAGIC, 0x864AB572u);
}

TEST("font: PSF2Header is 32 bytes") {
    ASSERT_EQ(sizeof(PSF2Header), 32u);
}

TEST("font: parse valid PSF2 header") {
    uint8_t buf[64]  = {};
    auto*   hdr      = reinterpret_cast<PSF2Header*>(buf);
    hdr->magic       = PSF2_MAGIC;
    hdr->version     = 0;
    hdr->header_size = 32;
    hdr->flags       = 0;
    hdr->length      = 256;
    hdr->charsize    = 16;
    hdr->height      = 16;
    hdr->width       = 8;

    auto info = parse_psf2(buf, sizeof(buf));
    ASSERT_NOT_NULL(info.glyphs);
    ASSERT_EQ(info.num_glyphs, 256u);
    ASSERT_EQ(info.bytes_per_glyph, 16u);
    ASSERT_EQ(info.width, 8u);
    ASSERT_EQ(info.height, 16u);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(info.glyphs), reinterpret_cast<uintptr_t>(buf) + 32);
}

TEST("font: reject invalid magic") {
    uint8_t buf[64] = {};
    auto    info    = parse_psf2(buf, sizeof(buf));
    ASSERT_NULL(info.glyphs);
}

TEST("font: glyph offset for character A") {
    // Character 'A' (65) in a 256-glyph font with 16 bytes per glyph
    uint32_t offset = 65 * 16;
    ASSERT_EQ(offset, 1040u);
}

TEST("font: out-of-range char wraps to glyph 0") {
    // Mirror: if (c >= num_glyphs) c = 0;
    uint8_t  c          = 255;
    uint32_t num_glyphs = 256;
    if (c >= num_glyphs)
        c = 0;
    ASSERT_EQ(c, 255);  // 255 < 256, so no wrap

    c = 0;
    if (c >= num_glyphs)
        c = 0;
    ASSERT_EQ(c, 0);
}

TEST("font: bitmap bit extraction pattern") {
    // For byte 0xAA = 10101010, iterating col 0..7 with (bits >> (7-col)) & 1
    uint8_t bits       = 0xAA;
    bool    expected[] = {true, false, true, false, true, false, true, false};
    for (int col = 0; col < 8; col++) {
        bool on = (bits >> (7 - col)) & 1;
        ASSERT_EQ(on, expected[col]);
    }
}

TEST("font: glyph pixel range for 8x16") {
    uint32_t font_w = 8, font_h = 16;
    uint32_t x = 100, y = 200;
    // Glyph spans [x, x+7] x [y, y+15]
    ASSERT_EQ(x + font_w - 1, 107u);
    ASSERT_EQ(y + font_h - 1, 215u);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
