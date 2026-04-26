/**
 * @file kernel/mm/heap.hpp
 * @brief Kernel heap allocator with first-fit, splitting, and coalescing
 *
 * Provides a linked-list-based heap allocator for dynamic memory allocation
 * inside the kernel.  Each block is preceded by a BlockHeader containing a
 * magic number for corruption detection.  Allocation uses a first-fit
 * strategy with block splitting, and freeing coalesces with adjacent free
 * blocks.  When the free list is exhausted, the heap is expanded
 * automatically via VMM::map().
 *
 * The global operator new / delete are redirected to Heap::alloc / free
 * so that C++ code can use the standard allocation syntax.
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/proc/sync.hpp"

namespace cinux::mm {

// ============================================================
// Block header placed before every heap block
// ============================================================

/**
 * @brief Per-block metadata prepended to each heap allocation
 *
 * Placed immediately before the user-accessible region.  The magic field
 * is checked on free() to detect corruption or double-free.
 */
struct [[gnu::packed]] BlockHeader {
    uint32_t     magic;
    uint32_t     size;      ///< Payload size (bytes, excluding this header)
    uint32_t     free;      ///< 1 = free, 0 = in use
    uint8_t      _pad[12];  ///< Padding to reach 32 bytes total
    BlockHeader* next;      ///< Next block in the free list
};

static_assert(sizeof(BlockHeader) == 32, "BlockHeader must be 32 bytes");

// ============================================================
// Heap class
// ============================================================

/**
 * @brief Kernel heap allocator
 *
 * Manages a region of virtual memory as a free-list heap.  Supports
 * first-fit allocation with block splitting, free with coalescing,
 * and automatic expansion via VMM when the free list is exhausted.
 */
class Heap {
public:
    /**
     * @brief Initialise the heap region
     *
     * Maps initial_size bytes of virtual memory starting at virt_base
     * using VMM::map() and PMM, then sets up the initial free block.
     *
     * @param virt_base    Starting virtual address for the heap region
     * @param initial_size Initial size in bytes (will be page-aligned up)
     */
    void init(uint64_t virt_base, uint64_t initial_size);

    /**
     * @brief Allocate a block of memory
     *
     * Uses first-fit search over the free list.  If the found block is
     * significantly larger than requested, it is split.  If no suitable
     * block is found, the heap is expanded via VMM.
     *
     * @param size   Requested payload size in bytes
     * @param align  Alignment requirement in bytes (must be power of 2)
     * @return Pointer to the allocated payload, or nullptr on failure
     */
    void* alloc(size_t size, size_t align = 16);

    /**
     * @brief Free a previously allocated block
     *
     * Validates the magic number, marks the block as free, and
     * coalesces with adjacent free blocks to reduce fragmentation.
     *
     * @param ptr  Pointer previously returned by alloc()
     */
    void free(void* ptr);

    /**
     * @brief Print heap statistics to the kernel log
     *
     * Outputs total size, used size, free size, and block count.
     */
    void dump_stats() const;

private:
    /**
     * @brief Internal allocation logic (caller must hold lock_)
     */
    void* alloc_locked(size_t size, size_t align);

    /**
     * @brief Expand the heap by mapping additional pages
     *
     * Called internally when no free block can satisfy a request.
     * Allocates physical pages via PMM and maps them via VMM.
     *
     * @param min_bytes  Minimum number of additional bytes needed
     * @return true if expansion succeeded, false if OOM
     */
    bool expand(size_t min_bytes);

    /**
     * @brief Coalesce a free block with its successor if also free
     *
     * @param block  The block that was just freed
     */
    void coalesce(BlockHeader* block);

    uint64_t              base_{};
    uint64_t              size_{};
    uint64_t              max_size_{};
    uint64_t              used_{};
    BlockHeader*          free_list_{};
    cinux::proc::Spinlock lock_;
};

/// Global Heap instance.
extern Heap g_heap;

}  // namespace cinux::mm
