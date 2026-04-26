/**
 * @file kernel/mini/arch/x86_64/paging.hpp
 * @brief Minimal paging helpers for the mini kernel
 *
 * Provides C++ functions to extend the page table mappings set up
 * by the bootloader.
 *
 * Page table layout (set up by boot/common/long_mode.S):
 *   PML4 @ 0x1000 -> PDPT @ 0x2000 -> PD @ 0x3000
 *   PD entries use 2MB huge pages (PS bit = 1)
 *   PDPT entries can use 1GB huge pages for addresses >= 1GB
 *
 * Both identity-mapped (PML4[0]) and higher-half (PML4[511]) entries
 * point to the same PDPT/PD, so adding a PD/PDPT entry automatically maps
 * the region at both 0x00000000xxxxxxxx and 0xFFFFFFFF80xxxxxxxx.
 *
 * For addresses beyond the PD's 1GB range (512 x 2MB), this module
 * populates additional PDPT entries as 1GB huge pages.  This requires
 * CPU support for 1GB pages (CPUID.80000001H:EDX[26], PDPE1GB).
 *
 * Namespace: cinux::mini::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::arch {

// ============================================================
// Page Table Constants
// ============================================================

/// Page directory entry flags: Present + Read/Write + Huge (2MB page)
static constexpr uint64_t PD_HUGE_PAGE_FLAGS = 0x83;

/// PDPT entry flags: Present + Read/Write + Page Size (1GB page)
static constexpr uint64_t PDPT_1GB_PAGE_FLAGS = 0x83;

/// Size of a 2MB huge page
static constexpr uint64_t PAGE_2MB_SIZE = 0x200000;

/// Size of a 1GB huge page
static constexpr uint64_t PAGE_1GB_SIZE = 0x40000000ULL;

/// Number of entries in a Page Directory (or any page table level)
static constexpr uint32_t PT_ENTRIES = 512;

/// Higher-half virtual address of the Page Directory
/// (matches PD_PHYS_ADDR = 0x3000 from the bootloader)
static constexpr uint64_t PD_VIRT_ADDR = 0xFFFFFFFF80003000ULL;

/// Higher-half virtual address of the Page Directory Pointer Table
/// (matches PDPT_PHYS_ADDR = 0x2000 from the bootloader)
static constexpr uint64_t PDPT_VIRT_ADDR = 0xFFFFFFFF80002000ULL;

/// PDPT[0] points to PD (bootloader-set, do not overwrite)
static constexpr uint32_t PDPT_PD_ENTRY = 0;

/// PDPT[510] is used for higher-half mapping (bootloader-set, do not overwrite)
/// long_mode.S: PML4[510] -> PDPT (recursive), PDPT[510] -> PD
static constexpr uint32_t PDPT_HIGHER_HALF_ENTRY = 510;

// ============================================================
// Internal Helpers
// ============================================================

namespace detail {

/// Whether the CPU supports 1GB pages (CPUID.80000001H:EDX[26])
/// Cached on first call to avoid repeated CPUID.
inline bool has_1gb_pages() {
    static bool checked   = false;
    static bool supported = false;
    if (checked)
        return supported;

    uint32_t eax = 0x80000001;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    supported = (edx & (1u << 26)) != 0;  // PDPE1GB bit
    checked   = true;
    return supported;
}

/// Reload CR3 to flush the entire TLB (non-global entries).
/// Required after modifying PDPT entries because invlpg cannot
/// invalidate 1GB page TLB entries (Intel SDM Vol.3 4.10.4).
inline void reload_cr3() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

}  // namespace detail

// ============================================================
// Public API
// ============================================================

/**
 * @brief Ensure a physical address range is identity-mapped
 *
 * Extends the page table with:
 *   - PD entries (2MB huge pages) for the first 1GB
 *   - PDPT entries (1GB huge pages) for addresses >= 1GB
 *
 * @param end_addr  Physical address that must be mapped (exclusive).
 *                  The function maps up to the next 2MB (or 1GB) boundary.
 */
inline void identity_map_up_to(uint64_t end_addr) {
    auto* pd   = reinterpret_cast<volatile uint64_t*>(PD_VIRT_ADDR);
    auto* pdpt = reinterpret_cast<volatile uint64_t*>(PDPT_VIRT_ADDR);

    // ---- Part 1: Fill PD entries (0-1GB, 2MB huge pages) ----
    uint32_t needed_2mb = static_cast<uint32_t>((end_addr + PAGE_2MB_SIZE - 1) / PAGE_2MB_SIZE);
    if (needed_2mb > PT_ENTRIES) {
        needed_2mb = PT_ENTRIES;  // PD can map at most 1GB
    }

    for (uint32_t i = 0; i < needed_2mb; i++) {
        if (pd[i] == 0) {
            uint64_t phys_base = static_cast<uint64_t>(i) * PAGE_2MB_SIZE;
            pd[i]              = phys_base | PD_HUGE_PAGE_FLAGS;

            // Invalidate TLB for the newly mapped 2MB region
            uint64_t virt_addr = static_cast<uint64_t>(i) * PAGE_2MB_SIZE;
            __asm__ volatile("invlpg (%0)" : : "r"(virt_addr));
        }
    }

    // ---- Part 2: Fill PDPT entries (>= 1GB, 1GB huge pages) ----
    uint32_t needed_1gb = static_cast<uint32_t>((end_addr + PAGE_1GB_SIZE - 1) / PAGE_1GB_SIZE);

    if (needed_1gb > 1 && detail::has_1gb_pages()) {
        for (uint32_t n = 1; n < needed_1gb; n++) {
            // Skip entries reserved by the bootloader
            if (n == PDPT_PD_ENTRY || n == PDPT_HIGHER_HALF_ENTRY) {
                continue;
            }
            if (pdpt[n] == 0) {
                uint64_t phys_base = static_cast<uint64_t>(n) * PAGE_1GB_SIZE;
                pdpt[n]            = phys_base | PDPT_1GB_PAGE_FLAGS;
            }
        }
        // Must reload CR3 to flush 1GB page TLB entries;
        // invlpg cannot invalidate PDPT-level mappings.
        detail::reload_cr3();
    }
}

}  // namespace cinux::mini::arch
