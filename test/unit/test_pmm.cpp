/**
 * @file test/unit/test_pmm.cpp
 * @brief Host-side unit tests for the Physical Memory Manager
 *
 * Tests parse_memory_map logic and bitmap allocator algorithm.
 * All kernel dependencies are mocked or reimplemented.
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#include <atomic>
#include <mutex>
#include <stdint.h>
#include <string.h>
#include <thread>
#include <unordered_set>
#include <vector>

#include "boot/boot_info.h"

// ============================================================
// Re-implement PMM constants and algorithms for host testing
// ============================================================

namespace {

constexpr uint64_t PAGE_SIZE        = 4096;
constexpr uint64_t LOW_MEM_BOUNDARY = 0x100000;

struct Region {
    uint64_t base;
    uint64_t length;
};

uint32_t parse_memory_map(const BootInfo& info, Region* regions,
                          uint32_t max_regions) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < info.mmap_count && count < max_regions; i++) {
        const auto& e = info.mmap[i];
        if (e.type != 1) continue;

        uint64_t base   = e.base;
        uint64_t length = e.length;

        if (base < LOW_MEM_BOUNDARY) {
            if (base + length <= LOW_MEM_BOUNDARY) continue;
            length -= LOW_MEM_BOUNDARY - base;
            base = LOW_MEM_BOUNDARY;
        }

        uint64_t aligned = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        length -= (aligned - base);
        length &= ~(PAGE_SIZE - 1);

        if (length < PAGE_SIZE) continue;
        regions[count++] = {aligned, length};
    }

    return count;
}

/// Standalone bitmap PMM that mirrors the kernel PMM's algorithm.
class TestPMM {
public:
    explicit TestPMM(uint64_t max_pages)
        : total_pages_(max_pages), free_pages_(0) {
        bm_size_ = (max_pages + 7) / 8;
        bm_ = new uint8_t[bm_size_];
        memset(bm_, 0xFF, bm_size_);
    }

    ~TestPMM() { delete[] bm_; }

    void mark_free(uint64_t start_page, uint64_t count) {
        for (uint64_t p = start_page; p < start_page + count && p < total_pages_; p++) {
            if (bm_[p / 8] & (1U << (p % 8))) {
                bm_[p / 8] &= static_cast<uint8_t>(~(1U << (p % 8)));
                free_pages_++;
            }
        }
    }

    void mark_used(uint64_t start_page, uint64_t count) {
        for (uint64_t p = start_page; p < start_page + count && p < total_pages_; p++) {
            if (!(bm_[p / 8] & (1U << (p % 8)))) {
                bm_[p / 8] |= static_cast<uint8_t>(1U << (p % 8));
                free_pages_--;
            }
        }
    }

    uint64_t alloc_page() {
        const auto* bm64 = reinterpret_cast<const uint64_t*>(bm_);
        uint64_t qw = bm_size_ / sizeof(uint64_t);

        for (uint64_t i = 0; i < qw; i++) {
            if (bm64[i] != ~0ULL) {
                int bit = __builtin_ctzll(~bm64[i]);
                uint64_t idx = i * 64 + static_cast<uint64_t>(bit);
                if (idx < total_pages_) {
                    bm_[idx / 8] |= static_cast<uint8_t>(1U << (idx % 8));
                    free_pages_--;
                    return idx * PAGE_SIZE;
                }
            }
        }

        for (uint64_t byte = qw * 8; byte < bm_size_; byte++) {
            if (bm_[byte] != 0xFF) {
                for (uint64_t bit = 0; bit < 8; bit++) {
                    uint64_t idx = byte * 8 + bit;
                    if (idx < total_pages_ && !(bm_[byte] & (1U << bit))) {
                        bm_[byte] |= static_cast<uint8_t>(1U << bit);
                        free_pages_--;
                        return idx * PAGE_SIZE;
                    }
                }
            }
        }

        return 0;
    }

    void free_page(uint64_t phys) {
        if (phys == 0) return;
        uint64_t idx = phys / PAGE_SIZE;
        if (idx >= total_pages_) return;
        if (!(bm_[idx / 8] & (1U << (idx % 8)))) return;
        bm_[idx / 8] &= static_cast<uint8_t>(~(1U << (idx % 8)));
        free_pages_++;
    }

    uint64_t alloc_pages(uint64_t count) {
        if (count == 0) return 0;
        if (count == 1) return alloc_page();

        uint64_t run = 0, start = 0;
        for (uint64_t p = 0; p < total_pages_; p++) {
            if (!(bm_[p / 8] & (1U << (p % 8)))) {
                if (run == 0) start = p;
                run++;
                if (run >= count) {
                    for (uint64_t i = start; i < start + count; i++)
                        bm_[i / 8] |= static_cast<uint8_t>(1U << (i % 8));
                    free_pages_ -= count;
                    return start * PAGE_SIZE;
                }
            } else {
                run = 0;
            }
        }

        return 0;
    }

    void free_pages(uint64_t phys, uint64_t count) {
        for (uint64_t i = 0; i < count; i++)
            free_page(phys + i * PAGE_SIZE);
    }

    uint64_t free_count() const { return free_pages_; }
    uint64_t total_count() const { return total_pages_; }

private:
    uint8_t* bm_;
    uint64_t total_pages_;
    uint64_t free_pages_;
    uint64_t bm_size_;
};

}  // anonymous namespace

// ============================================================
// parse_memory_map tests
// ============================================================

TEST("parse_memory_map: filters non-usable types") {
    BootInfo info = {};
    info.mmap_count = 3;
    info.mmap[0] = {0x100000, 0x100000, 1, 0};
    info.mmap[1] = {0x200000, 0x100000, 2, 0};
    info.mmap[2] = {0x300000, 0x100000, 3, 0};

    Region regions[8];
    ASSERT_EQ(parse_memory_map(info, regions, 8), 1u);
    ASSERT_EQ(regions[0].base, 0x100000u);
    ASSERT_EQ(regions[0].length, 0x100000u);
}

TEST("parse_memory_map: filters regions entirely below 1MB") {
    BootInfo info = {};
    info.mmap_count = 1;
    info.mmap[0] = {0x0, 0x100000, 1, 0};

    Region regions[8];
    ASSERT_EQ(parse_memory_map(info, regions, 8), 0u);
}

TEST("parse_memory_map: clips partial overlap with low 1MB") {
    BootInfo info = {};
    info.mmap_count = 1;
    // 0x80000..0x280000 crosses the 1MB boundary
    info.mmap[0] = {0x80000, 0x200000, 1, 0};

    Region regions[8];
    ASSERT_EQ(parse_memory_map(info, regions, 8), 1u);
    ASSERT_EQ(regions[0].base, 0x100000u);
    ASSERT_EQ(regions[0].length, 0x180000u);
}

TEST("parse_memory_map: aligns to 4KB boundaries") {
    BootInfo info = {};
    info.mmap_count = 1;
    info.mmap[0] = {0x100123, 0x2000, 1, 0};

    Region regions[8];
    ASSERT_EQ(parse_memory_map(info, regions, 8), 1u);
    ASSERT_EQ(regions[0].base, 0x101000u);
    // 0x2000 - (0x101000 - 0x100123) = 0x1223, aligned down = 0x1000
    ASSERT_EQ(regions[0].length, 0x1000u);
}

TEST("parse_memory_map: skips too-small aligned regions") {
    BootInfo info = {};
    info.mmap_count = 1;
    info.mmap[0] = {0x100001, 0xFFF, 1, 0};

    Region regions[8];
    ASSERT_EQ(parse_memory_map(info, regions, 8), 0u);
}

// ============================================================
// Bitmap allocator tests
// ============================================================

TEST("pmm: alloc_page returns page-aligned address") {
    // Page 0 maps to phys addr 0 (used as OOM sentinel), so start from page 1
    TestPMM pmm(1025);
    pmm.mark_free(1, 1024);

    uint64_t addr = pmm.alloc_page();
    ASSERT_NE(addr, 0u);
    ASSERT_EQ(addr % PAGE_SIZE, 0u);
}

TEST("pmm: 1000 alloc/free cycles preserve counts") {
    TestPMM pmm(2001);
    pmm.mark_free(1, 2000);
    uint64_t initial = pmm.free_count();

    uint64_t pages[1000];
    for (int i = 0; i < 1000; i++) {
        pages[i] = pmm.alloc_page();
        ASSERT_NE(pages[i], 0u);
        ASSERT_EQ(pages[i] % PAGE_SIZE, 0u);
    }

    ASSERT_EQ(pmm.free_count(), initial - 1000);

    for (int i = 0; i < 1000; i++) {
        pmm.free_page(pages[i]);
    }

    ASSERT_EQ(pmm.free_count(), initial);
}

TEST("pmm: OOM returns 0") {
    TestPMM pmm(5);
    pmm.mark_free(1, 4);

    for (int i = 0; i < 4; i++) {
        ASSERT_NE(pmm.alloc_page(), 0u);
    }

    ASSERT_EQ(pmm.alloc_page(), 0u);
}

TEST("pmm: free_page(0) is a no-op") {
    TestPMM pmm(5);
    pmm.mark_free(1, 4);
    uint64_t before = pmm.free_count();
    pmm.free_page(0);
    ASSERT_EQ(pmm.free_count(), before);
}

TEST("pmm: double free is a no-op") {
    TestPMM pmm(5);
    pmm.mark_free(1, 4);
    uint64_t p = pmm.alloc_page();
    ASSERT_NE(p, 0u);

    pmm.free_page(p);
    uint64_t after = pmm.free_count();
    pmm.free_page(p);
    ASSERT_EQ(pmm.free_count(), after);
}

TEST("pmm: alloc_pages returns contiguous block") {
    TestPMM pmm(257);
    pmm.mark_free(1, 256);

    uint64_t base = pmm.alloc_pages(4);
    ASSERT_NE(base, 0u);
    ASSERT_EQ(base % PAGE_SIZE, 0u);

    uint64_t saved_free = pmm.free_count();
    pmm.free_pages(base, 4);
    ASSERT_EQ(pmm.free_count(), saved_free + 4);
}

TEST("pmm: alloc_pages fails on fragmented memory") {
    TestPMM pmm(16);
    // Checkerboard: free only even pages
    for (uint64_t p = 0; p < 16; p += 2) {
        pmm.mark_free(p, 1);
    }

    ASSERT_EQ(pmm.free_count(), 8u);
    ASSERT_EQ(pmm.alloc_pages(2), 0u);
}

TEST("pmm: mark used/free counts are correct") {
    TestPMM pmm(100);
    ASSERT_EQ(pmm.free_count(), 0u);

    pmm.mark_free(10, 30);
    ASSERT_EQ(pmm.free_count(), 30u);

    pmm.mark_used(15, 5);
    ASSERT_EQ(pmm.free_count(), 25u);

    // Double mark_used is a no-op
    pmm.mark_used(15, 5);
    ASSERT_EQ(pmm.free_count(), 25u);

    // Double mark_free is a no-op on already-free pages
    pmm.mark_free(10, 5);
    ASSERT_EQ(pmm.free_count(), 25u);
}

// ============================================================
// Host-side Spinlock for concurrent tests
// ============================================================

namespace {

class HostSpinlock {
public:
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
        explicit Guard(HostSpinlock* lock) : lock_(lock) { lock_->acquire(); }
        ~Guard() { lock_->release(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        HostSpinlock* lock_;
    };
};

/// Thread-safe PMM wrapper using HostSpinlock.
class LockedTestPMM {
public:
    explicit LockedTestPMM(uint64_t max_pages)
        : total_pages_(max_pages), free_pages_(0) {
        bm_size_ = (max_pages + 7) / 8;
        bm_ = new uint8_t[bm_size_];
        memset(bm_, 0xFF, bm_size_);
    }

    ~LockedTestPMM() { delete[] bm_; }

    void mark_free(uint64_t start_page, uint64_t count) {
        for (uint64_t p = start_page; p < start_page + count && p < total_pages_; p++) {
            if (bm_[p / 8] & (1U << (p % 8))) {
                bm_[p / 8] &= static_cast<uint8_t>(~(1U << (p % 8)));
                free_pages_++;
            }
        }
    }

    uint64_t alloc_page() {
        auto g = lock_.guard();
        (void)g;
        const auto* bm64 = reinterpret_cast<const uint64_t*>(bm_);
        uint64_t qw = bm_size_ / sizeof(uint64_t);

        for (uint64_t i = 0; i < qw; i++) {
            if (bm64[i] != ~0ULL) {
                int bit = __builtin_ctzll(~bm64[i]);
                uint64_t idx = i * 64 + static_cast<uint64_t>(bit);
                if (idx < total_pages_) {
                    bm_[idx / 8] |= static_cast<uint8_t>(1U << (idx % 8));
                    free_pages_--;
                    return idx * PAGE_SIZE;
                }
            }
        }

        for (uint64_t byte = qw * 8; byte < bm_size_; byte++) {
            if (bm_[byte] != 0xFF) {
                for (uint64_t bit = 0; bit < 8; bit++) {
                    uint64_t idx = byte * 8 + bit;
                    if (idx < total_pages_ && !(bm_[byte] & (1U << bit))) {
                        bm_[byte] |= static_cast<uint8_t>(1U << bit);
                        free_pages_--;
                        return idx * PAGE_SIZE;
                    }
                }
            }
        }

        return 0;
    }

    void free_page(uint64_t phys) {
        auto g = lock_.guard();
        (void)g;
        if (phys == 0) return;
        uint64_t idx = phys / PAGE_SIZE;
        if (idx >= total_pages_) return;
        if (!(bm_[idx / 8] & (1U << (idx % 8)))) return;
        bm_[idx / 8] &= static_cast<uint8_t>(~(1U << (idx % 8)));
        free_pages_++;
    }

    uint64_t free_count() const { return free_pages_; }

private:
    HostSpinlock lock_;
    uint8_t* bm_;
    uint64_t total_pages_;
    uint64_t free_pages_;
    uint64_t bm_size_;
};

}  // anonymous namespace

// ============================================================
// Concurrent stress tests
// ============================================================

TEST("concurrent: no double-alloc under contention") {
    constexpr int NUM_THREADS = 4;
    constexpr int ALLOCS_PER_THREAD = 500;
    constexpr uint64_t TOTAL_PAGES = 4096;

    LockedTestPMM pmm(TOTAL_PAGES);
    pmm.mark_free(1, TOTAL_PAGES - 1);

    std::unordered_set<uint64_t> all_allocated;
    std::mutex set_mutex;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&]() {
            std::vector<uint64_t> local;
            local.reserve(ALLOCS_PER_THREAD);
            for (int i = 0; i < ALLOCS_PER_THREAD; i++) {
                uint64_t page = pmm.alloc_page();
                ASSERT_NE(page, 0u);
                local.push_back(page);
            }
            {
                std::lock_guard<std::mutex> lk(set_mutex);
                for (uint64_t p : local) {
                    auto [it, inserted] = all_allocated.insert(p);
                    ASSERT_TRUE(inserted);
                }
            }
        });
    }

    for (auto& th : threads) th.join();

    ASSERT_EQ(all_allocated.size(),
              static_cast<size_t>(NUM_THREADS * ALLOCS_PER_THREAD));
}

TEST("concurrent: alloc/free cycles preserve free count") {
    constexpr int NUM_THREADS = 4;
    constexpr int CYCLES = 200;
    constexpr uint64_t TOTAL_PAGES = 4096;

    LockedTestPMM pmm(TOTAL_PAGES);
    pmm.mark_free(1, TOTAL_PAGES - 1);
    uint64_t initial_free = pmm.free_count();

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < CYCLES; i++) {
                uint64_t page = pmm.alloc_page();
                ASSERT_NE(page, 0u);
                pmm.free_page(page);
            }
        });
    }

    for (auto& th : threads) th.join();

    ASSERT_EQ(pmm.free_count(), initial_free);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
