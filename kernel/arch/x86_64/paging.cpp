/**
 * @file kernel/arch/x86_64/paging.cpp
 * @brief Minimal paging implementation for the big kernel
 *
 * Manipulates the page tables at their known virtual addresses
 * (set up by the bootloader and extended by the mini kernel) to
 * map MMIO regions such as the framebuffer.
 */

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"

#include <stdint.h>

namespace cinux::arch {

namespace {

constexpr uint64_t PD_HUGE_PAGE_FLAGS = FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE;
constexpr uint64_t PDPT_1GB_PAGE_FLAGS = FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE;

constexpr uint64_t PAGE_2MB_SIZE = 0x200000;
constexpr uint64_t PAGE_1GB_SIZE = 0x40000000ULL;

constexpr uint64_t PD_VIRT_ADDR   = 0xFFFFFFFF80003000ULL;
constexpr uint64_t PDPT_VIRT_ADDR = 0xFFFFFFFF80002000ULL;

// PDPT[0] points to PD -- do not overwrite
constexpr uint32_t PDPT_PD_ENTRY = 0;

bool has_1gb_pages() {
    uint32_t eax = 0x80000001;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    return (edx & (1u << 26)) != 0;
}

}  // anonymous namespace

void map_mmio(uint64_t phys, uint64_t size) {
    auto* pd = reinterpret_cast<volatile uint64_t*>(PD_VIRT_ADDR);
    auto* pdpt = reinterpret_cast<volatile uint64_t*>(PDPT_VIRT_ADDR);

    uint64_t end = phys + size;

    // Part 1: PD entries for range within first 1GB (2MB pages)
    uint64_t cur = phys & ~(PAGE_2MB_SIZE - 1);
    while (cur < end && cur < PAGE_1GB_SIZE) {
        uint32_t idx = static_cast<uint32_t>(cur / PAGE_2MB_SIZE);
        if (idx < PT_ENTRIES && pd[idx] == 0) {
            pd[idx] = cur | PD_HUGE_PAGE_FLAGS;
            flush_tlb(cur);
        }
        cur += PAGE_2MB_SIZE;
    }

    // Part 2: PDPT entries for range >= 1GB (1GB pages)
    if (end > PAGE_1GB_SIZE && has_1gb_pages()) {
        uint64_t cur1g = phys & ~(PAGE_1GB_SIZE - 1);
        if (cur1g < PAGE_1GB_SIZE) cur1g = PAGE_1GB_SIZE;

        while (cur1g < end) {
            uint32_t n = static_cast<uint32_t>(cur1g / PAGE_1GB_SIZE);
            if (n < PT_ENTRIES && n != PDPT_PD_ENTRY && pdpt[n] == 0) {
                pdpt[n] = cur1g | PDPT_1GB_PAGE_FLAGS;
            }
            cur1g += PAGE_1GB_SIZE;
        }
        flush_tlb_all();
    }
}

}  // namespace cinux::arch