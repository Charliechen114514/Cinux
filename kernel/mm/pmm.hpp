/**
 * @file kernel/mm/pmm.hpp
 * @brief Physical Memory Manager -- bitmap allocator
 *
 * Manages physical page allocation using a one-bit-per-page bitmap
 * placed immediately after the kernel image + stack.  Supports single
 * and contiguous multi-page allocation with 64-bit accelerated scanning.
 */

#pragma once

#include <stdint.h>

#include "boot/boot_info.h"
#include "kernel/proc/sync.hpp"

namespace cinux::mm {

/// A usable physical memory region extracted from the E820 map.
struct MemoryRegion {
    uint64_t base;
    uint64_t length;
};

/**
 * @brief Extract usable memory regions from the BIOS memory map
 *
 * Filters for type-1 (usable) entries, removes anything below 1 MB,
 * and aligns each region to 4 KB boundaries.
 *
 * @param info          Boot information from the bootloader
 * @param regions       Output array for extracted regions
 * @param max_regions   Capacity of the regions array
 * @return Number of regions written
 */
uint32_t parse_memory_map(const BootInfo& info, MemoryRegion* regions, uint32_t max_regions);

/**
 * @brief Physical Memory Manager using a bitmap allocator
 *
 * Bitmap is placed at __kernel_stack_top (virtual), aligned to a page
 * boundary.  Allocation uses __builtin_ctzll for fast 64-bit-group
 * scanning.
 */
class PMM {
public:
    /** Initialise from the bootloader-provided memory map. */
    void init(const BootInfo& info);

    /** Allocate a single 4 KB page.  Returns physical address, 0 on OOM. */
    uint64_t alloc_page();

    /** Free a single page (no-op if phys is 0 or already free). */
    void free_page(uint64_t phys);

    /** Allocate @p count contiguous pages.  Returns base phys addr, 0 on OOM. */
    uint64_t alloc_pages(uint64_t count);

    /** Free @p count contiguous pages starting at @p phys. */
    void free_pages(uint64_t phys, uint64_t count);

    /** Current number of free pages. */
    uint64_t free_page_count() const;

    /** Total number of pages managed. */
    uint64_t total_page_count() const;

    /**
     * @brief Lock-free page allocation (caller must guarantee exclusion)
     *
     * Does NOT acquire the internal spinlock.  Safe only from contexts
     * where interrupts are disabled and no concurrent PMM access is
     * possible (e.g. page fault handler under Interrupt gate).
     */
    uint64_t alloc_page_locked();

    /** Lock-free page free (caller must guarantee exclusion). */
    void free_page_locked(uint64_t phys);

private:
    void mark_region_used(uint64_t phys, uint64_t length);
    void mark_region_free(uint64_t phys, uint64_t length);

    cinux::proc::Spinlock lock_;
    uint8_t*              bitmap_{};
    uint64_t              total_pages_{};
    uint64_t              free_pages_{};
    uint64_t              highest_page_{};
    uint64_t              bitmap_size_{};
};

/// Global PMM instance.
extern PMM g_pmm;

}  // namespace cinux::mm
