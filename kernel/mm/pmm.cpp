/**
 * @file kernel/mm/pmm.cpp
 * @brief Physical Memory Manager -- bitmap allocator implementation
 */

#include "kernel/mm/pmm.hpp"

#include <stddef.h>

#include "kernel/lib/kprintf.hpp"

namespace cinux::mm {

// ============================================================
// Constants
// ============================================================

constexpr uint64_t PAGE_SIZE        = 4096;
constexpr uint64_t LOW_MEM_BOUNDARY = 0x100000;               // 1 MB
constexpr uint64_t KERNEL_VMA       = 0xFFFFFFFF80000000ULL;

// ============================================================
// Linker symbols
// ============================================================

extern "C" {
extern char __kernel_end;
extern char __kernel_stack_top;
}

// ============================================================
// Global instance
// ============================================================

PMM g_pmm;

// ============================================================
// Internal bitmap helpers
// ============================================================

namespace {

void bm_set(uint8_t* bm, uint64_t idx) {
    bm[idx / 8] |= static_cast<uint8_t>(1U << (idx % 8));
}

void bm_clear(uint8_t* bm, uint64_t idx) {
    bm[idx / 8] &= static_cast<uint8_t>(~(1U << (idx % 8)));
}

bool bm_test(const uint8_t* bm, uint64_t idx) {
    return (bm[idx / 8] & (1U << (idx % 8))) != 0;
}

/// Scan 64 bits at a time using __builtin_ctzll for the first free bit.
int64_t bm_find_first_free(const uint8_t* bm, uint64_t highest_page,
                           uint64_t bitmap_size) {
    const auto* bm64 = reinterpret_cast<const uint64_t*>(bm);
    uint64_t qword_count = bitmap_size / sizeof(uint64_t);

    for (uint64_t i = 0; i < qword_count; i++) {
        if (bm64[i] != ~0ULL) {
            int bit = __builtin_ctzll(~bm64[i]);
            uint64_t idx = i * 64 + static_cast<uint64_t>(bit);
            if (idx < highest_page) return static_cast<int64_t>(idx);
        }
    }

    // Handle tail bytes not covered by the qword scan
    for (uint64_t byte = qword_count * 8; byte < bitmap_size; byte++) {
        if (bm[byte] != 0xFF) {
            for (uint64_t bit = 0; bit < 8; bit++) {
                uint64_t idx = byte * 8 + bit;
                if (idx < highest_page && !(bm[byte] & (1U << bit))) {
                    return static_cast<int64_t>(idx);
                }
            }
        }
    }

    return -1;
}

}  // anonymous namespace

// ============================================================
// parse_memory_map
// ============================================================

uint32_t parse_memory_map(const BootInfo& info,
                          MemoryRegion* regions,
                          uint32_t max_regions) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < info.mmap_count && count < max_regions; i++) {
        const auto& entry = info.mmap[i];
        if (entry.type != 1) continue;

        uint64_t base   = entry.base;
        uint64_t length = entry.length;

        // Filter: everything below 1 MB is reserved
        if (base < LOW_MEM_BOUNDARY) {
            if (base + length <= LOW_MEM_BOUNDARY) continue;
            length -= LOW_MEM_BOUNDARY - base;
            base = LOW_MEM_BOUNDARY;
        }

        // Align base up, length down to 4 KB
        uint64_t aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        length -= (aligned_base - base);
        length &= ~(PAGE_SIZE - 1);

        if (length < PAGE_SIZE) continue;
        regions[count++] = {aligned_base, length};
    }

    return count;
}

// ============================================================
// PMM private helpers
// ============================================================

void PMM::mark_region_used(uint64_t phys, uint64_t length) {
    uint64_t start = phys / PAGE_SIZE;
    uint64_t end   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = start; p < end && p < highest_page_; p++) {
        if (!bm_test(bitmap_, p)) {
            bm_set(bitmap_, p);
            free_pages_--;
        }
    }
}

void PMM::mark_region_free(uint64_t phys, uint64_t length) {
    uint64_t start = phys / PAGE_SIZE;
    uint64_t end   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = start; p < end && p < highest_page_; p++) {
        if (bm_test(bitmap_, p)) {
            bm_clear(bitmap_, p);
            free_pages_++;
        }
    }
}

// ============================================================
// PMM::init
// ============================================================

void PMM::init(const BootInfo& info) {
    // Step 1: Extract usable memory regions
    MemoryRegion regions[32];
    uint32_t region_count = parse_memory_map(info, regions, 32);

    // Step 2: Determine highest physical address -> bitmap size
    uint64_t max_addr = 0;
    for (uint32_t i = 0; i < region_count; i++) {
        uint64_t end = regions[i].base + regions[i].length;
        if (end > max_addr) max_addr = end;
    }

    highest_page_ = max_addr / PAGE_SIZE;
    total_pages_  = highest_page_;
    bitmap_size_  = (highest_page_ + 7) / 8;

    // Step 3: Place bitmap after kernel stack, page-aligned
    uintptr_t stack_top_virt = reinterpret_cast<uintptr_t>(&__kernel_stack_top);
    uintptr_t bm_virt = (stack_top_virt + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    bitmap_ = reinterpret_cast<uint8_t*>(bm_virt);

    // Step 4: Initialise bitmap -- all pages marked as used
    for (uint64_t i = 0; i < bitmap_size_; i++) {
        bitmap_[i] = 0xFF;
    }
    free_pages_ = 0;

    // Step 5: Clear bits for each usable region
    for (uint32_t i = 0; i < region_count; i++) {
        mark_region_free(regions[i].base, regions[i].length);
    }

    // Step 6: Re-mark kernel image + stack as used
    uint64_t used_phys_start = info.kernel_phys_base;
    uint64_t used_phys_end   = bm_virt - KERNEL_VMA;
    mark_region_used(used_phys_start, used_phys_end - used_phys_start);

    // Step 7: Mark bitmap itself as used
    uint64_t bm_phys       = bm_virt - KERNEL_VMA;
    uint64_t bm_pages_bytes = ((bitmap_size_ + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    mark_region_used(bm_phys, bm_pages_bytes);

    // Step 8: Print statistics
    uint64_t total_mb = total_pages_ * PAGE_SIZE / (1024 * 1024);
    uint64_t free_mb  = free_pages_ * PAGE_SIZE / (1024 * 1024);
    cinux::lib::kprintf("[PMM] Total: %uMB, Free: %uMB\n", total_mb, free_mb);
}

// ============================================================
// PMM::alloc_page_locked / free_page_locked (no lock)
// ============================================================

uint64_t PMM::alloc_page_locked() {
    int64_t idx = bm_find_first_free(bitmap_, highest_page_, bitmap_size_);
    if (idx < 0) return 0;

    bm_set(bitmap_, static_cast<uint64_t>(idx));
    free_pages_--;
    return static_cast<uint64_t>(idx) * PAGE_SIZE;
}

void PMM::free_page_locked(uint64_t phys) {
    if (phys == 0) return;
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= highest_page_) return;
    if (!bm_test(bitmap_, idx)) return;

    bm_clear(bitmap_, idx);
    free_pages_++;
}

// ============================================================
// PMM::alloc_page / free_page (public, locked)
// ============================================================

uint64_t PMM::alloc_page() {
    auto g = lock_.guard();
    (void)g;
    return alloc_page_locked();
}

void PMM::free_page(uint64_t phys) {
    auto g = lock_.guard();
    (void)g;
    free_page_locked(phys);
}

// ============================================================
// PMM::alloc_pages / free_pages (public, locked)
// ============================================================

uint64_t PMM::alloc_pages(uint64_t count) {
    if (count == 0) return 0;

    auto g = lock_.guard();
    (void)g;

    if (count == 1) return alloc_page_locked();

    uint64_t run   = 0;
    uint64_t start = 0;

    for (uint64_t p = 0; p < highest_page_; p++) {
        if (!bm_test(bitmap_, p)) {
            if (run == 0) start = p;
            run++;
            if (run >= count) {
                for (uint64_t i = start; i < start + count; i++) {
                    bm_set(bitmap_, i);
                }
                free_pages_ -= count;
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }

    return 0;
}

void PMM::free_pages(uint64_t phys, uint64_t count) {
    auto g = lock_.guard();
    (void)g;
    for (uint64_t i = 0; i < count; i++) {
        free_page_locked(phys + i * PAGE_SIZE);
    }
}

// ============================================================
// PMM statistics
// ============================================================

uint64_t PMM::free_page_count() const { return free_pages_; }
uint64_t PMM::total_page_count() const { return total_pages_; }

}  // namespace cinux::mm
