/**
 * @file kernel/arch/x86_64/paging_config.hpp
 * @brief x86_64 paging configuration constants
 *
 * Page size, shift amounts, index extraction macros, and address masks
 * used throughout the paging subsystem.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

constexpr uint64_t PAGE_SIZE  = 4096;
constexpr uint32_t PAGE_SHIFT = 12;
constexpr uint32_t PT_ENTRIES = 512;

// Bits to shift for each paging level index
constexpr uint32_t PT_SHIFT   = 12;
constexpr uint32_t PD_SHIFT   = 21;
constexpr uint32_t PDPT_SHIFT = 30;
constexpr uint32_t PML4_SHIFT = 39;

// Address and flag masks for page table entries
constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t FLAG_MASK = 0xFFF0000000000FFFULL;

// Page table flag bits
constexpr uint64_t FLAG_PRESENT  = 1ULL << 0;
constexpr uint64_t FLAG_WRITABLE = 1ULL << 1;
constexpr uint64_t FLAG_USER     = 1ULL << 2;
constexpr uint64_t FLAG_PWT      = 1ULL << 3;
constexpr uint64_t FLAG_PCD      = 1ULL << 4;
constexpr uint64_t FLAG_ACCESSED = 1ULL << 5;
constexpr uint64_t FLAG_DIRTY    = 1ULL << 6;
constexpr uint64_t FLAG_HUGE     = 1ULL << 7;
constexpr uint64_t FLAG_GLOBAL   = 1ULL << 8;
constexpr uint64_t FLAG_COW      = 1ULL << 9;  // Available bit 9: Copy-On-Write marker
constexpr uint64_t FLAG_NX       = 1ULL << 63;

// Index extraction for each paging level
constexpr uint64_t PML4_INDEX(uint64_t virt) {
    return (virt >> PML4_SHIFT) & 0x1FF;
}
constexpr uint64_t PDPT_INDEX(uint64_t virt) {
    return (virt >> PDPT_SHIFT) & 0x1FF;
}
constexpr uint64_t PD_INDEX(uint64_t virt) {
    return (virt >> PD_SHIFT) & 0x1FF;
}
constexpr uint64_t PT_INDEX(uint64_t virt) {
    return (virt >> PT_SHIFT) & 0x1FF;
}

// True if the virtual address falls in the canonical lower half (user space).
// x86_48 user space: bit 47 = 0, i.e. 0x0000000000000000 .. 0x00007FFFFFFFFFFF.
constexpr bool is_user_vaddr(uint64_t virt) {
    return !(virt & (1ULL << 47));
}

}  // namespace cinux::arch
