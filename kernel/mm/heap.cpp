/**
 * @file kernel/mm/heap.cpp
 * @brief Kernel heap allocator implementation
 */

#include "kernel/mm/heap.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

// ============================================================
// Constants
// ============================================================

constexpr uint32_t HEAP_MAGIC    = 0xDEADBEEF;
constexpr uint32_t HEADER_SIZE   = sizeof(BlockHeader);
constexpr uint32_t MIN_SPLIT     = HEADER_SIZE + 16;   // min viable split
constexpr uint64_t EXPAND_PAGES  = 4;                   // expand by 16 KB at a time
constexpr uint64_t PAGE_FLAGS    = 0x03;                // present + writable

// ============================================================
// Global instance
// ============================================================

Heap g_heap;

// ============================================================
// Internal helpers
// ============================================================

namespace {

/** Align value up to the given power-of-2 alignment. */
uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

/** Get the BlockHeader that precedes a user payload pointer. */
BlockHeader* header_from_ptr(void* ptr) {
    return reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uintptr_t>(ptr) - HEADER_SIZE);
}

/** Zero-fill a range of bytes. */
void memzero(void* start, size_t len) {
    auto* p = static_cast<uint8_t*>(start);
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

}  // anonymous namespace

// ============================================================
// Heap::init
// ============================================================

void Heap::init(uint64_t virt_base, uint64_t initial_size) {
    // Step 1: Page-align the initial size
    uint64_t aligned_size = align_up(initial_size, cinux::arch::PAGE_SIZE);

    // Step 2: Map each page of the heap region
    for (uint64_t offset = 0; offset < aligned_size; offset += cinux::arch::PAGE_SIZE) {
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) {
            cinux::lib::kprintf("[HEAP] OOM during init at offset %u\n", offset);
            return;
        }
        g_vmm.map(virt_base + offset, phys, PAGE_FLAGS);
    }

    // Step 3: Zero the entire heap region
    memzero(reinterpret_cast<void*>(virt_base), static_cast<size_t>(aligned_size));

    // Step 4: Set up the initial free block spanning the whole region
    auto* first = reinterpret_cast<BlockHeader*>(virt_base);
    first->magic = HEAP_MAGIC;
    first->size  = static_cast<uint32_t>(aligned_size - HEADER_SIZE);
    first->free  = 1;
    first->next  = nullptr;

    // Step 5: Store heap metadata
    base_     = virt_base;
    size_     = aligned_size;
    max_size_ = cinux::arch::KMEM_HEAP_SIZE;
    used_     = 0;
    free_list_ = first;

    cinux::lib::kprintf("[HEAP] Initialised at 0x%p, size %u KB, max %u MB\n",
                        reinterpret_cast<void*>(virt_base),
                        aligned_size / 1024,
                        max_size_ / (1024 * 1024));
}

// ============================================================
// Heap::alloc_locked (internal, caller holds lock)
// ============================================================

void* Heap::alloc_locked(size_t size, size_t align) {
    if (size == 0) {
        return nullptr;
    }

    if (align < 16) {
        align = 16;
    }

    size_t needed = size + (align - 1);

    BlockHeader* prev = nullptr;
    BlockHeader* curr = free_list_;

    while (curr != nullptr) {
        if (curr->magic != HEAP_MAGIC) {
            cinux::lib::kprintf("[HEAP] Corrupt block at 0x%p (magic=0x%x)\n",
                                reinterpret_cast<void*>(curr), curr->magic);
            return nullptr;
        }

        if (curr->free && curr->size >= needed) {
            uintptr_t curr_addr = reinterpret_cast<uintptr_t>(curr);
            uintptr_t block_end = curr_addr + HEADER_SIZE + curr->size;
            uintptr_t aligned_payload = align_up(curr_addr + HEADER_SIZE, align);
            uintptr_t hdr_addr = aligned_payload - HEADER_SIZE;

            size_t usable = static_cast<size_t>(block_end - aligned_payload);
            if (usable < size) {
                prev = curr;
                curr = curr->next;
                continue;
            }

            size_t front_pad = static_cast<size_t>(hdr_addr - curr_addr);
            size_t tail_space = static_cast<size_t>(block_end - (aligned_payload + size));

            // Remove curr from free list first
            if (prev != nullptr) {
                prev->next = curr->next;
            } else {
                free_list_ = curr->next;
            }

            // If front_pad is large enough, create a small free block there
            if (front_pad >= MIN_SPLIT) {
                curr->size = static_cast<uint32_t>(front_pad - HEADER_SIZE);
                // curr stays valid as a free block, re-insert it
                curr->next = free_list_;
                free_list_ = curr;
            }
            // else: front_pad is wasted (internal fragmentation)

            // Create remainder free block after the allocation
            if (tail_space >= MIN_SPLIT) {
                auto* rem = reinterpret_cast<BlockHeader*>(aligned_payload + size);
                rem->magic = HEAP_MAGIC;
                rem->size  = static_cast<uint32_t>(tail_space - HEADER_SIZE);
                rem->free  = 1;
                rem->next  = free_list_;
                free_list_ = rem;
            }

            // Write the allocated block header at aligned_payload - HEADER_SIZE
            auto* alloc_hdr = reinterpret_cast<BlockHeader*>(hdr_addr);
            alloc_hdr->magic = HEAP_MAGIC;
            alloc_hdr->size  = static_cast<uint32_t>(size);
            alloc_hdr->free  = 0;
            alloc_hdr->next  = nullptr;

            used_ += HEADER_SIZE + size;

            memzero(reinterpret_cast<void*>(aligned_payload), size);

            return reinterpret_cast<void*>(aligned_payload);
        }

        prev = curr;
        curr = curr->next;
    }

    // No suitable block found, expand the heap
    if (!expand(size + align + HEADER_SIZE)) {
        // Expansion failed (OOM) -- do not recurse, return nullptr
        return nullptr;
    }

    // Retry allocation after successful expansion
    return alloc_locked(size, align);
}

// ============================================================
// Heap::alloc (public, acquires lock)
// ============================================================

void* Heap::alloc(size_t size, size_t align) {
    auto g = lock_.guard();
    (void)g;
    return alloc_locked(size, align);
}

// ============================================================
// Heap::free
// ============================================================

void Heap::free(void* ptr) {
    auto g = lock_.guard();
    (void)g;

    // Step 1: Null check
    if (ptr == nullptr) {
        return;
    }

    // Step 2: Locate the block header
    auto* block = header_from_ptr(ptr);

    // Step 3: Magic validation
    if (block->magic != HEAP_MAGIC) {
        cinux::lib::kprintf("[HEAP] Double-free or corruption at 0x%p "
                            "(magic=0x%x, expected 0x%x)\n",
                            ptr, block->magic, HEAP_MAGIC);
        return;
    }

    if (block->free) {
        cinux::lib::kprintf("[HEAP] Double-free detected at 0x%p\n", ptr);
        return;
    }

    // Step 4: Mark as free and update stats
    used_ -= HEADER_SIZE + block->size;
    block->free = 1;

    // Step 5: Prepend to free list
    block->next = free_list_;
    free_list_  = block;

    // Step 6: Coalesce with adjacent free blocks
    coalesce(block);
}

// ============================================================
// Heap::expand
// ============================================================

bool Heap::expand(size_t min_bytes) {
    // Step 1: Determine how many pages to add
    uint64_t needed_bytes = align_up(
        min_bytes + HEADER_SIZE, cinux::arch::PAGE_SIZE);
    uint64_t needed_pages = needed_bytes / cinux::arch::PAGE_SIZE;

    // Always expand by at least EXPAND_PAGES
    if (needed_pages < EXPAND_PAGES) {
        needed_pages = EXPAND_PAGES;
    }

    uint64_t expand_size = needed_pages * cinux::arch::PAGE_SIZE;

    // Bounds check: do not expand past the reserved heap region
    if (size_ + expand_size > max_size_) {
        cinux::lib::kprintf("[HEAP] Expansion limit reached: %u KB / %u MB\n",
                            size_ / 1024, max_size_ / (1024 * 1024));
        return false;
    }

    // Step 2: Map new pages at the end of the current heap region
    for (uint64_t offset = 0; offset < expand_size; offset += cinux::arch::PAGE_SIZE) {
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) {
            cinux::lib::kprintf("[HEAP] OOM during expansion at offset %u\n", offset);
            return false;
        }
        g_vmm.map(base_ + size_ + offset, phys, PAGE_FLAGS);
    }

    // Step 3: Zero the new region
    memzero(reinterpret_cast<void*>(base_ + size_),
            static_cast<size_t>(expand_size));

    // Step 4: Create a new free block in the expanded region
    auto* new_block = reinterpret_cast<BlockHeader*>(base_ + size_);
    new_block->magic = HEAP_MAGIC;
    new_block->size  = static_cast<uint32_t>(expand_size - HEADER_SIZE);
    new_block->free  = 1;
    new_block->next  = free_list_;
    free_list_       = new_block;

    // Step 5: Update total heap size
    size_ += expand_size;

    cinux::lib::kprintf("[HEAP] Expanded by %u KB, total %u KB\n",
                        expand_size / 1024, size_ / 1024);
    return true;
}

// ============================================================
// Heap::coalesce
// ============================================================

void Heap::coalesce(BlockHeader* block) {
    bool changed = true;
    while (changed) {
        changed = false;
        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_;

        while (curr != nullptr) {
            if (curr == block || !curr->free) {
                prev = curr;
                curr = curr->next;
                continue;
            }

            uintptr_t curr_addr  = reinterpret_cast<uintptr_t>(curr);
            uintptr_t block_addr = reinterpret_cast<uintptr_t>(block);

            // curr is immediately before block
            if (curr_addr + HEADER_SIZE + curr->size == block_addr) {
                curr->size += HEADER_SIZE + block->size;
                if (free_list_ == block) {
                    free_list_ = block->next;
                } else {
                    auto* p = free_list_;
                    while (p && p->next != block) p = p->next;
                    if (p) p->next = block->next;
                }
                block = curr;
                changed = true;
                break;
            }

            // block is immediately before curr
            if (block_addr + HEADER_SIZE + block->size == curr_addr) {
                block->size += HEADER_SIZE + curr->size;
                if (prev) prev->next = curr->next;
                else      free_list_ = curr->next;
                changed = true;
                break;
            }

            prev = curr;
            curr = curr->next;
        }
    }
}

// ============================================================
// Heap::dump_stats
// ============================================================

void Heap::dump_stats() const {
    uint64_t free_total = 0;
    uint64_t block_count = 0;

    BlockHeader* curr = free_list_;
    while (curr != nullptr) {
        if (curr->free) {
            free_total += curr->size;
        }
        block_count++;
        curr = curr->next;
    }

    cinux::lib::kprintf("[HEAP] Stats: total=%u KB, used=%u KB, free_list=%u KB, "
                        "blocks=%u\n",
                        size_ / 1024, used_ / 1024, free_total / 1024, block_count);
}

}  // namespace cinux::mm
