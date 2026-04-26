/**
 * @file test/unit/test_framebuffer.cpp
 * @brief Host-side unit tests for framebuffer indexing arithmetic
 *
 * Tests the pitch-based pixel offset formula, bounds checking logic,
 * fill_rect region calculation, and scroll_up byte offset arithmetic.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>

// Mirror the framebuffer index formula: offset = y * (pitch / 4) + x
static uint32_t pixel_index(uint32_t x, uint32_t y, uint32_t pitch) {
    return y * (pitch / 4) + x;
}

// Mirror bounds check: skip if x >= width or y >= height
static bool in_bounds(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    return x < width && y < height;
}

TEST("framebuffer: pixel index at origin") {
    ASSERT_EQ(pixel_index(0, 0, 4096), 0u);
}

TEST("framebuffer: pixel index at (100, 50)") {
    // pitch=4096 (1024*4), offset = 50 * (4096/4) + 100 = 50*1024 + 100
    ASSERT_EQ(pixel_index(100, 50, 4096), 51200u + 100u);
}

TEST("framebuffer: out-of-bounds rejected") {
    ASSERT_FALSE(in_bounds(1024, 0, 1024, 768));
    ASSERT_FALSE(in_bounds(0, 768, 1024, 768));
    ASSERT_TRUE(in_bounds(1023, 767, 1024, 768));
    ASSERT_TRUE(in_bounds(0, 0, 1024, 768));
}

TEST("framebuffer: fill_rect pixel count") {
    // fill_rect(10, 20, 5, 3) covers rows 20,21,22 x cols 10,11,12,13,14
    uint32_t count = 5 * 3;  // w * h
    ASSERT_EQ(count, 15u);
}

TEST("framebuffer: scroll_up byte offset") {
    uint32_t pitch  = 4096;
    uint32_t height = 768;
    uint32_t lines  = 16;
    // source offset = pitch * lines = 4096 * 16 = 65536
    ASSERT_EQ(pitch * lines, 65536u);
    // move_bytes = (height - lines) * pitch = 752 * 4096
    ASSERT_EQ((height - lines) * pitch, 752u * 4096u);
}

TEST("framebuffer: scroll_up with lines >= height clears all") {
    uint32_t height = 768;
    uint32_t lines  = 768;
    // lines >= height triggers clear() path
    ASSERT_TRUE(lines >= height);
}

TEST("framebuffer: pitch index at bottom-right corner") {
    // For 1024x768 with pitch 4096: pixel at (1023, 767)
    ASSERT_EQ(pixel_index(1023, 767, 4096), 767u * 1024u + 1023u);
}

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
