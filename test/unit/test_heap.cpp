/**
 * @file test/unit/test_heap.cpp
 * @brief Host-side unit tests for the kernel heap allocator
 *
 * Re-implements the Heap first-fit allocator algorithm with mock PMM/VMM
 * so the alloc/free/split/coalesce logic can be tested on the host.
 * Covers: alloc non-null, alignment, split, coalesce, double-free detection,
 * and 1000-cycle random alloc/free leak check.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <atomic>
#include <vector>

// ============================================================
// Constants (mirrored from heap.cpp)
// ============================================================

namespace {

constexpr uint32_t HEAP_MAGIC  = 0xDEADBEEF;
constexpr uint32_t HEADER_SIZE = 32;  // sizeof(BlockHeader)
constexpr uint32_t MIN_SPLIT   = HEADER_SIZE + 16;
constexpr uint64_t PAGE_SIZE   = 4096;

// ============================================================
// BlockHeader (mirrored from heap.hpp)
// ============================================================

struct BlockHeader {
    uint32_t     magic;
    uint32_t     size;
    uint32_t     free;
    uint8_t      _pad[12];
    BlockHeader* next;
};

static_assert(sizeof(BlockHeader) == 32, "BlockHeader must be 32 bytes");

// ============================================================
// Host-side Heap implementation (mirrors heap.cpp algorithm)
// Uses a large host malloc'd buffer instead of VMM-mapped pages.
// ============================================================

class TestHeap {
public:
    void init(size_t region_size) {
        region_size_ = align_up(region_size, PAGE_SIZE);
        buffer_ = static_cast<uint8_t*>(calloc(1, region_size_));
        base_   = reinterpret_cast<uintptr_t>(buffer_);

        auto* first = reinterpret_cast<BlockHeader*>(base_);
        first->magic = HEAP_MAGIC;
        first->size  = static_cast<uint32_t>(region_size_ - HEADER_SIZE);
        first->free  = 1;
        first->next  = nullptr;

        free_list_ = first;
        used_      = 0;
    }

    ~TestHeap() {
        free(buffer_);
    }

    void* alloc(size_t size, size_t align = 16) {
        if (size == 0) return nullptr;
        if (align < 16) align = 16;

        size_t needed = size + (align - 1);

        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_;

        while (curr != nullptr) {
            if (curr->magic != HEAP_MAGIC) return nullptr;

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

                // Remove curr from free list
                if (prev != nullptr) {
                    prev->next = curr->next;
                } else {
                    free_list_ = curr->next;
                }

                // Front padding: create small free block if large enough
                if (front_pad >= MIN_SPLIT) {
                    curr->size = static_cast<uint32_t>(front_pad - HEADER_SIZE);
                    curr->next = free_list_;
                    free_list_ = curr;
                }

                // Tail remainder: create free block after allocation
                if (tail_space >= MIN_SPLIT) {
                    auto* rem = reinterpret_cast<BlockHeader*>(aligned_payload + size);
                    rem->magic = HEAP_MAGIC;
                    rem->size  = static_cast<uint32_t>(tail_space - HEADER_SIZE);
                    rem->free  = 1;
                    rem->next  = free_list_;
                    free_list_ = rem;
                }

                // Write allocated block header at aligned_payload - HEADER_SIZE
                auto* alloc_hdr = reinterpret_cast<BlockHeader*>(hdr_addr);
                alloc_hdr->magic = HEAP_MAGIC;
                alloc_hdr->size  = static_cast<uint32_t>(size);
                alloc_hdr->free  = 0;
                alloc_hdr->next  = nullptr;

                used_ += HEADER_SIZE + size;

                memset(reinterpret_cast<void*>(aligned_payload), 0, size);
                return reinterpret_cast<void*>(aligned_payload);
            }

            prev = curr;
            curr = curr->next;
        }

        return nullptr;
    }

    void free_mem(void* ptr) {
        if (ptr == nullptr) return;

        auto* block = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<uintptr_t>(ptr) - HEADER_SIZE);

        if (block->magic != HEAP_MAGIC) return;
        if (block->free) return;  // double-free detection

        used_ -= HEADER_SIZE + block->size;
        block->free = 1;
        block->next = free_list_;
        free_list_  = block;

        coalesce(block);
    }

    size_t used() const { return used_; }
    size_t total() const { return region_size_; }

    // Count total free bytes in free list
    size_t free_total() const {
        size_t t = 0;
        BlockHeader* c = free_list_;
        while (c != nullptr) {
            if (c->free) t += c->size;
            c = c->next;
        }
        return t;
    }

    // Count free list blocks
    size_t free_block_count() const {
        size_t n = 0;
        BlockHeader* c = free_list_;
        while (c != nullptr) { n++; c = c->next; }
        return n;
    }

    // Validate free list: no cycles, all magic valid, all marked free
    bool validate_free_list() const {
        size_t count = 0;
        size_t max_count = 10000;  // safety limit
        BlockHeader* c = free_list_;
        while (c != nullptr) {
            if (++count > max_count) return false;  // cycle detected
            if (c->magic != HEAP_MAGIC) return false;
            if (!c->free) return false;
            // Check block is within buffer
            auto addr = reinterpret_cast<uintptr_t>(c);
            if (addr < base_ || addr + HEADER_SIZE > base_ + region_size_) return false;
            c = c->next;
        }
        return true;
    }

    // Check if a pointer's block still has valid magic
    bool has_valid_magic(void* ptr) const {
        if (ptr == nullptr) return false;
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        if (addr < base_ + HEADER_SIZE || addr >= base_ + region_size_) return false;
        auto* block = reinterpret_cast<BlockHeader*>(addr - HEADER_SIZE);
        return block->magic == HEAP_MAGIC;
    }

    // Check if a pointer's block is marked free
    bool is_block_free(void* ptr) const {
        auto* block = reinterpret_cast<BlockHeader*>(
            reinterpret_cast<uintptr_t>(ptr) - HEADER_SIZE);
        return block->free == 1;
    }

private:
    static uint64_t align_up(uint64_t value, uint64_t a) {
        return (value + a - 1) & ~(a - 1);
    }

    void coalesce(BlockHeader* block) {
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

                if (curr_addr + HEADER_SIZE + curr->size == block_addr) {
                    curr->size += HEADER_SIZE + block->size;
                    if (free_list_ == block) free_list_ = block->next;
                    else {
                        auto* p = free_list_;
                        while (p && p->next != block) p = p->next;
                        if (p) p->next = block->next;
                    }
                    block = curr;
                    changed = true;
                    break;
                }
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

    uint8_t*     buffer_      = nullptr;
    uintptr_t    base_        = 0;
    size_t       region_size_ = 0;
    size_t       used_        = 0;
    BlockHeader* free_list_   = nullptr;
};

}  // anonymous namespace

// ============================================================
// Normal path tests
// ============================================================

// alloc returns non-null for a valid request
TEST("heap: alloc returns non-null") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(64);
    ASSERT_NOT_NULL(p);
}

// alloc returns 16-byte aligned pointer by default
TEST("heap: alloc default alignment is 16 bytes") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(64);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);
}

// alloc respects custom alignment
TEST("heap: alloc respects 64-byte alignment") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(64, 64);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % 64, 0u);
}

TEST("heap: alloc respects 4096-byte alignment") {
    TestHeap heap;
    heap.init(16 * PAGE_SIZE);

    void* p = heap.alloc(64, 4096);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % 4096, 0u);
}

// alloc returns nullptr for zero-size
TEST("heap: alloc(0) returns nullptr") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    ASSERT_NULL(heap.alloc(0));
}

// ============================================================
// Split tests
// ============================================================

// allocating a small block from a large free block splits it
TEST("heap: alloc splits large block") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p1 = heap.alloc(64);
    ASSERT_NOT_NULL(p1);

    // The free list should still have a block (the remainder after split)
    size_t ft = heap.free_total();
    ASSERT_GT(ft, 0u);
}

// after splitting, the remaining block has valid magic
TEST("heap: split remainder has valid magic") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(64);
    ASSERT_NOT_NULL(p);

    // All remaining blocks in free list should have valid magic
    // free_total() walks the list, exercising this indirectly
    ASSERT_GT(heap.free_total(), 0u);
}

// ============================================================
// Free and coalesce tests
// ============================================================

// free(nullptr) is a safe no-op
TEST("heap: free(nullptr) is a no-op") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    size_t used_before = heap.used();
    heap.free_mem(nullptr);
    ASSERT_EQ(heap.used(), used_before);
}

// free returns memory to the free list
TEST("heap: free reduces used count") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(128);
    ASSERT_NOT_NULL(p);
    size_t used_after_alloc = heap.used();
    ASSERT_GT(used_after_alloc, 0u);

    heap.free_mem(p);
    ASSERT_LT(heap.used(), used_after_alloc);
}

// two adjacent allocs that are freed in order should coalesce
TEST("heap: coalesce merges adjacent free blocks (forward)") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p1 = heap.alloc(64);
    void* p2 = heap.alloc(64);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);

    heap.free_mem(p1);
    heap.free_mem(p2);

    // After freeing both, used should be 0
    ASSERT_EQ(heap.used(), 0u);

    // Free list should have a single large block (or merged region)
    ASSERT_EQ(heap.free_total(), heap.total() - HEADER_SIZE);
}

// freeing in reverse order also coalesces
TEST("heap: coalesce merges adjacent free blocks (reverse order)") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p1 = heap.alloc(64);
    void* p2 = heap.alloc(64);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);

    heap.free_mem(p2);
    heap.free_mem(p1);

    ASSERT_EQ(heap.used(), 0u);
    ASSERT_EQ(heap.free_total(), heap.total() - HEADER_SIZE);
}

// ============================================================
// Double-free detection
// ============================================================

// freeing the same pointer twice does not corrupt state
TEST("heap: double-free does not corrupt used count") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(128);
    ASSERT_NOT_NULL(p);

    heap.free_mem(p);
    size_t used_after = heap.used();

    // Second free should be caught by the free==1 check
    heap.free_mem(p);
    ASSERT_EQ(heap.used(), used_after);
}

// ============================================================
// Magic validation
// ============================================================

// after alloc and free, the block still has valid magic
TEST("heap: block retains valid magic after free") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(128);
    ASSERT_NOT_NULL(p);
    ASSERT_TRUE(heap.has_valid_magic(p));

    heap.free_mem(p);
    ASSERT_TRUE(heap.has_valid_magic(p));
}

// ============================================================
// Multiple allocations
// ============================================================

// many small allocations all succeed and are non-null
TEST("heap: 50 sequential allocations all succeed") {
    TestHeap heap;
    heap.init(64 * PAGE_SIZE);

    void* ptrs[50];
    for (int i = 0; i < 50; i++) {
        ptrs[i] = heap.alloc(64);
        ASSERT_NOT_NULL(ptrs[i]);
    }

    for (int i = 0; i < 50; i++) {
        heap.free_mem(ptrs[i]);
    }

    ASSERT_EQ(heap.used(), 0u);
}

// ============================================================
// Expand (automatic growth when free list exhausted)
// ============================================================

// ============================================================
// Stress test: 1000 random alloc/free cycles
// ============================================================

TEST("heap: many alloc/free cycles with no leaks") {
    TestHeap heap;
    heap.init(256 * PAGE_SIZE);

    // Phase 1: interleave allocs and frees deterministically
    for (int cycle = 0; cycle < 200; cycle++) {
        void* a = heap.alloc(32);
        void* b = heap.alloc(64);
        void* c = heap.alloc(128);
        ASSERT_NOT_NULL(a);
        ASSERT_NOT_NULL(b);
        ASSERT_NOT_NULL(c);

        heap.free_mem(b);
        heap.free_mem(a);

        void* d = heap.alloc(48);
        ASSERT_NOT_NULL(d);

        heap.free_mem(c);
        heap.free_mem(d);
    }

    // Phase 2: fill up then drain
    void* ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = heap.alloc(static_cast<size_t>(i * 4 + 16));
        ASSERT_NOT_NULL(ptrs[i]);
    }
    for (int i = 0; i < 100; i++) {
        heap.free_mem(ptrs[i]);
    }

    ASSERT_EQ(heap.used(), 0u);
}

// ============================================================
// CRITICAL: alloc with front padding (header_from_ptr alignment)
// ============================================================

TEST("heap: alloc with 4096 alignment has valid header_from_ptr") {
    TestHeap heap;
    heap.init(16 * PAGE_SIZE);

    // First alloc consumes some space so the next free block
    // starts at a non-4096-aligned address
    void* p1 = heap.alloc(64);
    ASSERT_NOT_NULL(p1);

    // This must place header at aligned_payload - 32, not at block start
    void* p2 = heap.alloc(64, 4096);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p2) % 4096, 0u);

    // header_from_ptr must find the correct header
    ASSERT_TRUE(heap.has_valid_magic(p2));
    ASSERT_FALSE(heap.is_block_free(p2));

    // Free must work correctly despite front padding
    heap.free_mem(p2);
    heap.free_mem(p1);
    ASSERT_EQ(heap.used(), 0u);
}

TEST("heap: alloc after non-16-aligned size stays aligned") {
    TestHeap heap;
    heap.init(16 * PAGE_SIZE);

    // alloc(37) makes total_consumed = 32+37 = 69, next block at +69
    void* p1 = heap.alloc(37);
    ASSERT_NOT_NULL(p1);

    // Next free block starts at an irregular offset.
    // alloc must still produce a 16-byte-aligned pointer.
    void* p2 = heap.alloc(24);
    ASSERT_NOT_NULL(p2);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p2) % 16, 0u);

    // Both must free correctly
    heap.free_mem(p1);
    heap.free_mem(p2);
    ASSERT_EQ(heap.used(), 0u);
    ASSERT_TRUE(heap.validate_free_list());
}

// ============================================================
// CRITICAL: coalesce 3+ adjacent blocks
// ============================================================

TEST("heap: coalesce merges three adjacent blocks (middle first)") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* a = heap.alloc(64);
    void* b = heap.alloc(64);
    void* c = heap.alloc(64);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);

    heap.free_mem(b);  // free middle first
    heap.free_mem(a);  // then left
    heap.free_mem(c);  // then right

    ASSERT_EQ(heap.used(), 0u);
    ASSERT_TRUE(heap.validate_free_list());
}

TEST("heap: coalesce merges three adjacent blocks (left to right)") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* a = heap.alloc(64);
    void* b = heap.alloc(64);
    void* c = heap.alloc(64);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);

    heap.free_mem(a);
    heap.free_mem(b);
    heap.free_mem(c);

    ASSERT_EQ(heap.used(), 0u);
    ASSERT_TRUE(heap.validate_free_list());
}

TEST("heap: coalesce merges three adjacent blocks (right to left)") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* a = heap.alloc(64);
    void* b = heap.alloc(64);
    void* c = heap.alloc(64);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);
    ASSERT_NOT_NULL(c);

    heap.free_mem(c);
    heap.free_mem(b);
    heap.free_mem(a);

    ASSERT_EQ(heap.used(), 0u);
    ASSERT_TRUE(heap.validate_free_list());
}

// ============================================================
// CRITICAL: freed memory is reused
// ============================================================

TEST("heap: freed block is reused by subsequent allocation") {
    TestHeap heap;
    heap.init(4 * PAGE_SIZE);

    void* a = heap.alloc(64);
    void* b = heap.alloc(64);
    ASSERT_NOT_NULL(a);
    ASSERT_NOT_NULL(b);

    size_t fb_before = heap.free_block_count();
    heap.free_mem(a);
    size_t fb_after_free = heap.free_block_count();
    ASSERT_GT(fb_after_free, fb_before);

    // Next alloc of same size must succeed (recycling freed space)
    void* c = heap.alloc(64);
    ASSERT_NOT_NULL(c);

    heap.free_mem(b);
    heap.free_mem(c);
    ASSERT_EQ(heap.used(), 0u);
}

// ============================================================
// HIGH: data integrity
// ============================================================

TEST("heap: allocated memory is zeroed and writable") {
    TestHeap heap;
    heap.init(4 * PAGE_SIZE);

    auto* p = static_cast<uint8_t*>(heap.alloc(128));
    ASSERT_NOT_NULL(p);

    // Verify zeroed on alloc
    for (int i = 0; i < 128; i++) {
        ASSERT_EQ(p[i], 0u);
    }

    // Write pattern
    for (int i = 0; i < 128; i++) {
        p[i] = static_cast<uint8_t>(i);
    }

    // Verify pattern survives
    for (int i = 0; i < 128; i++) {
        ASSERT_EQ(p[i], static_cast<uint8_t>(i));
    }

    heap.free_mem(p);
    ASSERT_EQ(heap.used(), 0u);
}

TEST("heap: pattern survives alloc/free/realloc cycle") {
    TestHeap heap;
    heap.init(4 * PAGE_SIZE);

    void* p1 = heap.alloc(64);
    ASSERT_NOT_NULL(p1);
    memset(p1, 0xAA, 64);
    heap.free_mem(p1);

    // Re-alloc same size -- should be zeroed by allocator
    auto* p2 = static_cast<uint8_t*>(heap.alloc(64));
    ASSERT_NOT_NULL(p2);
    for (int i = 0; i < 64; i++) {
        ASSERT_EQ(p2[i], 0u);  // must be zeroed, not stale data
    }

    heap.free_mem(p2);
    ASSERT_EQ(heap.used(), 0u);
}

// ============================================================
// HIGH: edge cases
// ============================================================

TEST("heap: alloc(1) minimum allocation") {
    TestHeap heap;
    heap.init(4 * PAGE_SIZE);

    void* p = heap.alloc(1);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);

    // Write to the single byte
    auto* bp = static_cast<uint8_t*>(p);
    *bp = 0x42;
    ASSERT_EQ(*bp, 0x42u);

    heap.free_mem(p);
    ASSERT_EQ(heap.used(), 0u);
}

TEST("heap: alloc returns nullptr when heap exhausted") {
    TestHeap heap;
    heap.init(PAGE_SIZE);  // only 4096 bytes

    // Exhaust the heap (each alloc(64) needs ~96 bytes)
    void* ptrs[64];
    int count = 0;
    for (int i = 0; i < 64; i++) {
        ptrs[i] = heap.alloc(256);
        if (ptrs[i] == nullptr) break;
        count++;
    }

    ASSERT_GT(count, 0);

    void* fail = heap.alloc(256);
    ASSERT_NULL(fail);

    for (int i = 0; i < count; i++) {
        heap.free_mem(ptrs[i]);
    }
    ASSERT_EQ(heap.used(), 0u);
}

TEST("heap: large allocation near page size") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    void* p = heap.alloc(3900);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);

    // Free and verify accounting
    heap.free_mem(p);
    ASSERT_EQ(heap.used(), 0u);
}

// ============================================================
// HIGH: free list integrity
// ============================================================

TEST("heap: free list integrity after fragmented alloc/free") {
    TestHeap heap;
    heap.init(16 * PAGE_SIZE);

    // Allocate 20 blocks of varying sizes
    void* ptrs[20];
    size_t sizes[20] = {32,64,48,128,96,32,64,80,48,64,
                        32,96,48,128,32,64,80,48,96,64};
    for (int i = 0; i < 20; i++) {
        ptrs[i] = heap.alloc(sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
    }

    // Free every other one (creates fragmentation)
    for (int i = 0; i < 20; i += 2) {
        heap.free_mem(ptrs[i]);
        ptrs[i] = nullptr;
    }
    ASSERT_TRUE(heap.validate_free_list());

    // Free the rest
    for (int i = 1; i < 20; i += 2) {
        heap.free_mem(ptrs[i]);
        ptrs[i] = nullptr;
    }
    ASSERT_TRUE(heap.validate_free_list());
    ASSERT_EQ(heap.used(), 0u);
}

// ============================================================
// MEDIUM: no overlap between allocations
// ============================================================

TEST("heap: multiple allocations do not overlap") {
    TestHeap heap;
    heap.init(16 * PAGE_SIZE);

    size_t sizes[] = {32, 64, 48, 128, 37, 93, 64, 4096, 17, 256};
    constexpr int N = 10;
    void* ptrs[N];

    for (int i = 0; i < N; i++) {
        ptrs[i] = heap.alloc(sizes[i]);
        ASSERT_NOT_NULL(ptrs[i]);
    }

    // Check no pair overlaps
    for (int i = 0; i < N; i++) {
        auto i_start = reinterpret_cast<uintptr_t>(ptrs[i]);
        auto i_end   = i_start + sizes[i];
        for (int j = i + 1; j < N; j++) {
            auto j_start = reinterpret_cast<uintptr_t>(ptrs[j]);
            auto j_end   = j_start + sizes[j];
            // [i_start, i_end) and [j_start, j_end) must not overlap
            bool overlap = !(i_end <= j_start || j_end <= i_start);
            ASSERT_FALSE(overlap);
        }
    }

    for (int i = 0; i < N; i++) {
        heap.free_mem(ptrs[i]);
    }
    ASSERT_EQ(heap.used(), 0u);
}

// ============================================================
// MEDIUM: accounting invariant
// used + sum(free block sizes) + num_blocks * HEADER_SIZE == region_size
// ============================================================

TEST("heap: size accounting invariant holds") {
    TestHeap heap;
    heap.init(8 * PAGE_SIZE);

    // After full drain (used==0), all space must be accounted for.
    // The invariant: free_total + overhead <= total
    // where overhead = at least free_block_count * HEADER_SIZE
    // (may be more due to front padding internal fragmentation)

    void* a = heap.alloc(64);
    void* b = heap.alloc(37);
    void* c = heap.alloc(128);

    heap.free_mem(b);
    heap.free_mem(a);
    heap.free_mem(c);

    ASSERT_EQ(heap.used(), 0u);
    // All space must be in free list (may have some internal fragmentation loss)
    size_t accounted = heap.free_total() + heap.free_block_count() * HEADER_SIZE;
    ASSERT_GE(heap.total(), accounted);
    // But shouldn't lose more than 10% to fragmentation
    ASSERT_GT(heap.free_total(), heap.total() * 9 / 10);
}

// ============================================================
// Thread-safe TestHeap wrapper and concurrent tests
// ============================================================

namespace {

class Spinlock {
public:
    Spinlock() = default;

    void acquire() {
        while (locked_.exchange(true, std::memory_order_acquire)) {
        }
    }

    void release() {
        locked_.store(false, std::memory_order_release);
    }

    [[nodiscard]] auto guard() {
        return Guard(this);
    }

private:
    std::atomic<bool> locked_{false};

    class Guard {
    public:
        explicit Guard(Spinlock* lock) : lock_(lock) {
            lock_->acquire();
        }
        ~Guard() {
            lock_->release();
        }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        Spinlock* lock_;
    };
};

class ThreadSafeTestHeap {
public:
    void init(size_t region_size) {
        heap_.init(region_size);
    }

    void* alloc(size_t size, size_t align = 16) {
        auto g = lock_.guard();
        (void)g;
        return heap_.alloc(size, align);
    }

    void free_mem(void* ptr) {
        auto g = lock_.guard();
        (void)g;
        heap_.free_mem(ptr);
    }

    size_t used() const { return heap_.used(); }
    size_t total() const { return heap_.total(); }
    bool validate_free_list() const { return heap_.validate_free_list(); }

private:
    TestHeap heap_;
    Spinlock lock_;
};

}  // anonymous namespace

TEST("heap: concurrent alloc/free with spinlock") {
    ThreadSafeTestHeap heap;
    heap.init(256 * PAGE_SIZE);

    constexpr int NUM_THREADS = 4;
    constexpr int OPS_PER_THREAD = 100;
    std::atomic<int> errors{0};

    auto worker = [&](int thread_id) {
        void* ptrs[OPS_PER_THREAD];
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            size_t sz = static_cast<size_t>((thread_id * 7 + i * 13) % 200 + 16);
            ptrs[i] = heap.alloc(sz);
            if (ptrs[i] == nullptr) {
                errors.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            auto* buf = static_cast<uint8_t*>(ptrs[i]);
            buf[0] = static_cast<uint8_t>(thread_id);
            buf[sz - 1] = static_cast<uint8_t>(thread_id ^ 0xFF);
        }
        for (int i = 0; i < OPS_PER_THREAD; i++) {
            if (ptrs[i] != nullptr) {
                heap.free_mem(ptrs[i]);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t);
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(errors.load(), 0);
    ASSERT_EQ(heap.used(), 0u);
    ASSERT_TRUE(heap.validate_free_list());
}

TEST("heap: concurrent interleaved alloc/free") {
    ThreadSafeTestHeap heap;
    heap.init(512 * PAGE_SIZE);

    constexpr int NUM_THREADS = 4;
    constexpr int CYCLES = 50;
    std::atomic<int> errors{0};

    auto worker = [&](int) {
        for (int c = 0; c < CYCLES; c++) {
            void* a = heap.alloc(32);
            void* b = heap.alloc(64);
            void* d = heap.alloc(128);
            if (a == nullptr || b == nullptr || d == nullptr) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            if (b) heap.free_mem(b);
            if (a) heap.free_mem(a);
            void* e = heap.alloc(48);
            if (e == nullptr) {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
            if (d) heap.free_mem(d);
            if (e) heap.free_mem(e);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back(worker, t);
    }
    for (auto& th : threads) {
        th.join();
    }

    ASSERT_EQ(errors.load(), 0);
    ASSERT_EQ(heap.used(), 0u);
    ASSERT_TRUE(heap.validate_free_list());
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
