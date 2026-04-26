/**
 * @file kernel/drivers/video/framebuffer.cpp
 * @brief Linear framebuffer driver implementation
 */

#include "framebuffer.hpp"

#include <stdint.h>

#include "boot/boot_info.h"
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::drivers {

void Framebuffer::init(const BootInfo& bi) {
    uint64_t fb_phys = bi.fb_addr;
    width_           = bi.fb_width;
    height_          = bi.fb_height;
    pitch_           = bi.fb_pitch;
    bpp_             = bi.fb_bpp;

    // Map the framebuffer into higher-half kernel page tables via 2 MB
    // huge pages, so fork can safely drop PML4[0] identity mappings.
    uint64_t           fb_size    = static_cast<uint64_t>(pitch_) * height_;
    constexpr uint64_t PAGE_2MB   = 0x200000ULL;
    uint64_t           num_pages  = (fb_size + PAGE_2MB - 1) / PAGE_2MB;
    constexpr uint64_t mmio_flags = arch::FLAG_PRESENT | arch::FLAG_WRITABLE;

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = (fb_phys & ~(PAGE_2MB - 1)) + i * PAGE_2MB;
        uint64_t virt = arch::KMEM_FB_BASE + i * PAGE_2MB;
        mm::g_vmm.map_2mb(virt, phys, mmio_flags);
    }

    // Adjust for the difference between 2MB-aligned base and actual fb_phys
    uint64_t offset = fb_phys & (PAGE_2MB - 1);
    addr_           = reinterpret_cast<volatile uint32_t*>(arch::KMEM_FB_BASE + offset);
}

void Framebuffer::put_pixel(uint32_t x, uint32_t y, uint32_t argb) {
    if (x >= width_ || y >= height_)
        return;
    addr_[y * (pitch_ / 4) + x] = argb;
}

uint32_t Framebuffer::get_pixel(uint32_t x, uint32_t y) const {
    if (x >= width_ || y >= height_)
        return 0;
    return addr_[y * (pitch_ / 4) + x];
}

void Framebuffer::fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb) {
    for (uint32_t row = y; row < y + h && row < height_; row++) {
        for (uint32_t col = x; col < x + w && col < width_; col++) {
            addr_[row * (pitch_ / 4) + col] = argb;
        }
    }
}

void Framebuffer::scroll_up(uint32_t lines, uint32_t line_height, uint32_t bg) {
    if (lines >= height_) {
        clear(bg);
        return;
    }

    // Move rows up using memmove (byte-level copy on the linear buffer)
    auto*                   buf        = reinterpret_cast<volatile uint8_t*>(addr_);
    const volatile uint8_t* src        = buf + static_cast<uint32_t>(pitch_) * lines;
    volatile uint8_t*       dst        = buf;
    uint32_t                move_bytes = (height_ - lines) * pitch_;
    for (uint32_t i = 0; i < move_bytes; i++) {
        dst[i] = src[i];
    }

    // Clear the bottom band
    uint32_t clear_y = height_ - line_height;
    fill_rect(0, clear_y, width_, line_height, bg);
}

void Framebuffer::clear(uint32_t argb) {
    for (uint32_t y = 0; y < height_; y++) {
        for (uint32_t x = 0; x < width_; x++) {
            addr_[y * (pitch_ / 4) + x] = argb;
        }
    }
}

}  // namespace cinux::drivers
