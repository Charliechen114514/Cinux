/**
 * @file kernel/gui/data/icon_data.hpp
 * @brief Constexpr 32x32 pixel icon data for desktop applications
 *
 * Contains the raw compile-time bitmap data for application icons.
 * Each icon is a std::array of 1024 uint32_t values in 0x00RRGGBB
 * format (row-major, top-to-bottom).  A pixel value of 0x00000000
 * is treated as transparent by Canvas::draw_bitmap().
 *
 * This header is only compiled when CINUX_GUI is defined.
 *
 * Namespace: cinux::gui::icons::data
 */

#pragma once

#include <array>
#include <cstdint>

namespace cinux::gui::icons::data {

// ============================================================
// Colour palette
// ============================================================

namespace palette {
constexpr uint32_t BLACK       = 0x00000000;  // Transparent (skipped by draw_bitmap)
constexpr uint32_t DARK_BLACK  = 0x00101010;  // Near-black (opaque)
constexpr uint32_t WHITE       = 0x00FFFFFF;
constexpr uint32_t GREEN       = 0x0033CC33;  // Terminal prompt green
constexpr uint32_t GREY_DARK   = 0x00404040;  // Calculator body dark
constexpr uint32_t GREY_MID    = 0x00707070;  // Calculator body mid
constexpr uint32_t GREY_LIGHT  = 0x00AAAAAA;  // Calculator body light
constexpr uint32_t BUTTON_GREY = 0x00909090;  // Calculator button
constexpr uint32_t DISPLAY_BG  = 0x00C8DFC8;  // Calculator display (greenish LCD)
constexpr uint32_t DISPLAY_TXT = 0x00222222;  // Calculator display text
constexpr uint32_t ORANGE      = 0x00FF8C00;  // Calculator equals button
}  // namespace palette

// ============================================================
// Compile-time icon builder
// ============================================================

namespace detail {

/**
 * @brief Map a single hex character to a 4-bit nibble (0-15)
 *
 * @param c  ASCII hex digit ('0'-'9', 'a'-'f', 'A'-'F')
 * @return   Nibble value, or 0 for invalid characters
 */
constexpr uint32_t hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return static_cast<uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<uint32_t>(c - 'a') + 10;
    if (c >= 'A' && c <= 'F')
        return static_cast<uint32_t>(c - 'A') + 10;
    return 0;
}

/**
 * @brief Look up a palette colour by nibble index
 *
 * @param pal     16-entry colour table
 * @param nibble  Index (0-15)
 * @return        32-bit colour from the palette
 */
constexpr uint32_t palette_lookup(const uint32_t (&pal)[16], uint32_t nibble) {
    return pal[nibble];
}

/**
 * @brief Build a full 32x32 icon from 32 row hex strings and a palette
 *
 * Each row string must be exactly 32 characters.  Characters are mapped
 * through the palette to produce 32-bit pixel values.
 *
 * @tparam Rows  Number of row strings (must be 32)
 * @param palette  16-entry colour lookup table
 * @param rows     Array of 32 string literal pointers, each 32 chars + NUL
 * @return         1024-element pixel array
 */
template <uint32_t Rows>
constexpr std::array<uint32_t, 1024> build_icon(const uint32_t (&palette)[16],
                                                const char* const (&rows)[Rows]) {
    static_assert(Rows == 32, "Icon must have exactly 32 rows");

    std::array<uint32_t, 1024> pixels{};
    for (uint32_t r = 0; r < 32; r++) {
        for (uint32_t c = 0; c < 32; c++) {
            uint32_t nibble    = hex_nibble(rows[r][c]);
            pixels[r * 32 + c] = palette_lookup(palette, nibble);
        }
    }
    return pixels;
}

}  // namespace detail

// ============================================================
// Shell icon — black terminal with ">_" prompt
// ============================================================

/**
 * @brief Shell/terminal icon palette
 *
 * Digit mapping:
 *   0 = transparent   1 = DARK_BLACK (body)
 *   2 = GREY_DARK (title bar)  3 = WHITE (text)
 *   4 = GREEN (cursor)  5 = red dot  6 = yellow dot  7 = green dot
 */
inline constexpr uint32_t k_shell_palette[16] = {
    palette::BLACK,       // 0 - transparent
    palette::DARK_BLACK,  // 1 - terminal body
    palette::GREY_DARK,   // 2 - title bar
    palette::WHITE,       // 3 - text
    palette::GREEN,       // 4 - cursor green
    0x00CC3333,           // 5 - close dot red
    0x00CCCC33,           // 6 - minimise dot yellow
    0x0033CC33,           // 7 - maximise dot green
};

/**
 * @brief 32x32 shell icon pixel data
 *
 * Visual: dark rounded-corner terminal body with three traffic-light
 * dots in the title bar and a white ">_" command prompt.
 */
inline constexpr std::array<uint32_t, 1024> k_shell_icon = detail::build_icon(
    k_shell_palette, {
                         "00222222222222222222222222222220", "02255672222222222222222222222220",
                         "02222222222222222222222222222220", "02111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01331111111111111111111111111110",
                         "01134111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "01111111111111111111111111111110",
                         "01111111111111111111111111111110", "00222222222222222222222222222220",
                     });

// ============================================================
// Calculator icon — grey body with LCD display and button grid
// ============================================================

/**
 * @brief Calculator icon palette
 *
 * Digit mapping:
 *   0 = transparent   1 = GREY_MID (body)
 *   2 = GREY_DARK (border)  3 = BUTTON_GREY
 *   4 = DISPLAY_BG  5 = DISPLAY_TXT
 *   6 = ORANGE (equals)  7 = GREY_LIGHT (button highlight)
 */
inline constexpr uint32_t k_calc_palette[16] = {
    palette::BLACK,        // 0 - transparent
    palette::GREY_MID,     // 1 - body
    palette::GREY_DARK,    // 2 - border / grid lines
    palette::BUTTON_GREY,  // 3 - buttons
    palette::DISPLAY_BG,   // 4 - display background
    palette::DISPLAY_TXT,  // 5 - display text
    palette::ORANGE,       // 6 - equals button
    palette::GREY_LIGHT,   // 7 - button highlight
};

/**
 * @brief 32x32 calculator icon pixel data
 *
 * Visual: rounded grey body, greenish LCD display showing "123",
 * 4-column button grid (C, +/-, %, /, 7-9, *, 4-6, -, 1-3, +, 0, ., =).
 */
inline constexpr std::array<uint32_t, 1024> k_calc_icon = detail::build_icon(
    k_calc_palette, {
                        "00222222222222222222222222222220", "02111111111111111111111111111120",
                        "01244444444444444444444444444110", "01245551111111111111111111111110",
                        "01244444444444444444444444444110", "01222222222222222222222222222210",
                        "01273273273273273273273273273210", "01233233233233233233233233233210",
                        "01273273273273273273273273273210", "01233233233233233233233233233210",
                        "01273273273273273273273273273210", "01233233233233233233233233233210",
                        "01273273273273273273273273273210", "01233233233233233233233233233210",
                        "01273333333333333333333333673210", "01233333333333333333333333363210",
                        "01222222222222222222222222222210", "02111111111111111111111111111120",
                        "00222222222222222222222222222220", "00000000000000000000000000000000",
                        "00000000000000000000000000000000", "00000000000000000000000000000000",
                        "00000000000000000000000000000000", "00000000000000000000000000000000",
                        "00000000000000000000000000000000", "00000000000000000000000000000000",
                        "00000000000000000000000000000000", "00000000000000000000000000000000",
                        "00000000000000000000000000000000", "00000000000000000000000000000000",
                        "00000000000000000000000000000000", "00000000000000000000000000000000",
                    });

}  // namespace cinux::gui::icons::data
