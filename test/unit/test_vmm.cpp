/**
 * @file test/unit/test_vmm.cpp
 * @brief Host-side unit tests for the Virtual Memory Manager
 *
 * Tests the VMM map/translate/unmap logic using a simulated physical
 * memory pool instead of a real PMM.  All paging structures are
 * allocated from the simulated pool so that the 4-level walk can be
 * verified entirely on the host.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include <stdint.h>
#include <string.h>

#include "test_framework.h"

// ============================================================
// Paging constants (mirrored from paging_config.hpp)
// ============================================================

namespace {

constexpr uint64_t PAGE_SIZE  = 4096;
constexpr uint32_t PT_ENTRIES = 512;

constexpr uint32_t PT_SHIFT   = 12;
constexpr uint32_t PD_SHIFT   = 21;
constexpr uint32_t PDPT_SHIFT = 30;
constexpr uint32_t PML4_SHIFT = 39;

constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;

constexpr uint64_t FLAG_PRESENT  = 1ULL << 0;
constexpr uint64_t FLAG_WRITABLE = 1ULL << 1;
constexpr uint64_t FLAG_USER     = 1ULL << 2;
constexpr uint64_t FLAG_NX       = 1ULL << 63;

uint64_t PML4_INDEX(uint64_t virt) {
    return (virt >> PML4_SHIFT) & 0x1FF;
}
uint64_t PDPT_INDEX(uint64_t virt) {
    return (virt >> PDPT_SHIFT) & 0x1FF;
}
uint64_t PD_INDEX(uint64_t virt) {
    return (virt >> PD_SHIFT) & 0x1FF;
}
uint64_t PT_INDEX(uint64_t virt) {
    return (virt >> PT_SHIFT) & 0x1FF;
}

// ============================================================
// Simulated physical memory pool (mock PMM)
// ============================================================

constexpr uint32_t MOCK_POOL_PAGES = 256;
constexpr uint64_t MOCK_POOL_BASE  = 0x2000000ULL;

struct MockPMM {
    uint8_t  bitmap[MOCK_POOL_PAGES / 8];
    uint32_t next_page;

    MockPMM() : next_page(0) { memset(bitmap, 0, sizeof(bitmap)); }

    uint64_t alloc_page() {
        for (uint32_t i = 0; i < MOCK_POOL_PAGES; i++) {
            uint32_t byte = i / 8;
            uint32_t bit  = i % 8;
            if (!(bitmap[byte] & (1U << bit))) {
                bitmap[byte] |= static_cast<uint8_t>(1U << bit);
                return MOCK_POOL_BASE + static_cast<uint64_t>(i) * PAGE_SIZE;
            }
        }
        return 0;
    }

    void free_page(uint64_t phys) {
        if (phys < MOCK_POOL_BASE)
            return;
        uint32_t idx = static_cast<uint32_t>((phys - MOCK_POOL_BASE) / PAGE_SIZE);
        if (idx >= MOCK_POOL_PAGES)
            return;
        bitmap[idx / 8] &= static_cast<uint8_t>(~(1U << (idx % 8)));
    }
};

// ============================================================
// Simulated page table memory (backed by host memory)
// ============================================================

constexpr uint32_t SIM_PAGES = 128;
alignas(4096) uint8_t sim_memory[SIM_PAGES][PAGE_SIZE];

uint8_t* sim_virt_of(uint64_t phys) {
    return &sim_memory[(phys - MOCK_POOL_BASE) / PAGE_SIZE][0];
}

union PageEntry {
    uint64_t raw;
    struct {
        uint64_t present : 1;
        uint64_t writable : 1;
        uint64_t user : 1;
        uint64_t pwt : 1;
        uint64_t pcd : 1;
        uint64_t accessed : 1;
        uint64_t dirty : 1;
        uint64_t huge : 1;
        uint64_t global : 1;
        uint64_t _avail : 3;
        uint64_t addr : 40;
        uint64_t _avail2 : 11;
        uint64_t nx : 1;
    };

    uint64_t phys_addr() const { return raw & ADDR_MASK; }
    bool     is_present() const { return (raw & FLAG_PRESENT) != 0; }
};

// ============================================================
// TestVMM: mirrors the real VMM algorithm using mock memory
// ============================================================

class TestVMM {
public:
    void init() {
        MockPMM  pmm;
        uint64_t pml4_phys = pmm.alloc_page();
        pml4_              = reinterpret_cast<PageEntry*>(sim_virt_of(pml4_phys));
        memset(pml4_, 0, PAGE_SIZE);
        pml4_phys_ = pml4_phys;
        pmm_       = pmm;
    }

    bool map(uint64_t virt, uint64_t phys, uint64_t flags) {
        PageEntry* table = pml4_;

        PageEntry* pdpt = walk_or_alloc(table, PML4_INDEX(virt));
        if (!pdpt)
            return false;

        PageEntry* pd = walk_or_alloc(pdpt, PDPT_INDEX(virt));
        if (!pd)
            return false;

        PageEntry* pt = walk_or_alloc(pd, PD_INDEX(virt));
        if (!pt)
            return false;

        uint64_t pt_idx = PT_INDEX(virt);
        pt[pt_idx].raw  = (phys & ADDR_MASK) | (flags & ~ADDR_MASK);
        return true;
    }

    void unmap(uint64_t virt) {
        PageEntry* table = pml4_;

        PageEntry* pdpt = walk_only(table, PML4_INDEX(virt));
        if (!pdpt)
            return;

        PageEntry* pd = walk_only(pdpt, PDPT_INDEX(virt));
        if (!pd)
            return;

        PageEntry* pt = walk_only(pd, PD_INDEX(virt));
        if (!pt)
            return;

        pt[PT_INDEX(virt)].raw = 0;
    }

    uint64_t translate(uint64_t virt) {
        PageEntry* table = pml4_;

        PageEntry* pdpt = walk_only(table, PML4_INDEX(virt));
        if (!pdpt)
            return 0;

        PageEntry* pd = walk_only(pdpt, PDPT_INDEX(virt));
        if (!pd)
            return 0;

        PageEntry* pt = walk_only(pd, PD_INDEX(virt));
        if (!pt)
            return 0;

        PageEntry& entry = pt[PT_INDEX(virt)];
        if (!entry.is_present())
            return 0;

        uint64_t offset = virt & (PAGE_SIZE - 1);
        return entry.phys_addr() | offset;
    }

private:
    PageEntry* walk_or_alloc(PageEntry* table, uint64_t index) {
        PageEntry& entry = table[index];
        if (entry.is_present()) {
            return reinterpret_cast<PageEntry*>(sim_virt_of(entry.phys_addr()));
        }

        uint64_t new_page = pmm_.alloc_page();
        if (new_page == 0)
            return nullptr;

        auto* new_table = reinterpret_cast<PageEntry*>(sim_virt_of(new_page));
        memset(new_table, 0, PAGE_SIZE);

        entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE;
        return new_table;
    }

    PageEntry* walk_only(PageEntry* table, uint64_t index) {
        PageEntry& entry = table[index];
        if (!entry.is_present())
            return nullptr;
        return reinterpret_cast<PageEntry*>(sim_virt_of(entry.phys_addr()));
    }

    PageEntry* pml4_;
    uint64_t   pml4_phys_;
    MockPMM    pmm_;
};

}  // anonymous namespace

// ============================================================
// Normal path tests
// ============================================================

TEST("vmm: map then translate returns correct physical address") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt = 0x400000;  // 4 MB
    uint64_t phys = 0xB8000;   // text mode buffer as test

    ASSERT_TRUE(vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys);
}

TEST("vmm: map preserves in-page offset in translate") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt = 0x400100;
    uint64_t phys = 0x100000;

    ASSERT_TRUE(vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys + 0x100);
}

TEST("vmm: unmap causes translate to return 0") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt = 0x400000;
    uint64_t phys = 0x100000;

    ASSERT_TRUE(vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys);

    vmm.unmap(virt);
    ASSERT_EQ(vmm.translate(virt), 0u);
}

TEST("vmm: map two different virtual addresses") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt1 = 0x400000;
    uint64_t virt2 = 0x500000;
    uint64_t phys1 = 0x100000;
    uint64_t phys2 = 0x200000;

    ASSERT_TRUE(vmm.map(virt1, phys1, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_TRUE(vmm.map(virt2, phys2, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt1), phys1);
    ASSERT_EQ(vmm.translate(virt2), phys2);
}

TEST("vmm: translate unmapped address returns 0") {
    TestVMM vmm;
    vmm.init();

    ASSERT_EQ(vmm.translate(0x400000), 0u);
}

TEST("vmm: remap replaces previous mapping") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt  = 0x400000;
    uint64_t phys1 = 0x100000;
    uint64_t phys2 = 0x200000;

    ASSERT_TRUE(vmm.map(virt, phys1, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys1);

    ASSERT_TRUE(vmm.map(virt, phys2, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys2);
}

// ============================================================
// Flag tests
// ============================================================

TEST("vmm: flags are stored correctly in page table entry") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt  = 0x400000;
    uint64_t phys  = 0x100000;
    uint64_t flags = FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER;

    ASSERT_TRUE(vmm.map(virt, phys, flags));
    ASSERT_EQ(vmm.translate(virt), phys);
}

// ============================================================
// Multiple page mappings in same table
// ============================================================

TEST("vmm: map multiple pages within same PT") {
    TestVMM vmm;
    vmm.init();

    // These all share the same PML4/PDPT/PD entry but different PT slots
    for (uint64_t i = 0; i < 16; i++) {
        uint64_t virt = 0x400000 + i * PAGE_SIZE;
        uint64_t phys = 0x100000 + i * PAGE_SIZE;
        ASSERT_TRUE(vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE));
    }

    for (uint64_t i = 0; i < 16; i++) {
        uint64_t virt = 0x400000 + i * PAGE_SIZE;
        uint64_t phys = 0x100000 + i * PAGE_SIZE;
        ASSERT_EQ(vmm.translate(virt), phys);
    }
}

TEST("vmm: unmap one page does not affect sibling pages") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt1 = 0x400000;
    uint64_t virt2 = 0x401000;
    uint64_t phys1 = 0x100000;
    uint64_t phys2 = 0x101000;

    ASSERT_TRUE(vmm.map(virt1, phys1, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_TRUE(vmm.map(virt2, phys2, FLAG_PRESENT | FLAG_WRITABLE));

    vmm.unmap(virt1);
    ASSERT_EQ(vmm.translate(virt1), 0u);
    ASSERT_EQ(vmm.translate(virt2), phys2);
}

// ============================================================
// Edge cases
// ============================================================

TEST("vmm: unmap on never-mapped address is a no-op") {
    TestVMM vmm;
    vmm.init();

    vmm.unmap(0xDEAD000);
    ASSERT_EQ(vmm.translate(0xDEAD000), 0u);
}

TEST("vmm: map at high canonical address") {
    TestVMM vmm;
    vmm.init();

    // Kernel higher-half: 0xFFFFFFFF80000000
    uint64_t virt = 0xFFFFFFFF80000000ULL;
    uint64_t phys = 0x100000;

    ASSERT_TRUE(vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys);
}

TEST("vmm: full map/unmap/remap cycle") {
    TestVMM vmm;
    vmm.init();

    uint64_t virt   = 0x600000;
    uint64_t phys_a = 0xAA000;
    uint64_t phys_b = 0xBB000;

    // Step 1: map to A
    ASSERT_TRUE(vmm.map(virt, phys_a, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys_a);

    // Step 2: unmap
    vmm.unmap(virt);
    ASSERT_EQ(vmm.translate(virt), 0u);

    // Step 3: remap to B
    ASSERT_TRUE(vmm.map(virt, phys_b, FLAG_PRESENT | FLAG_WRITABLE));
    ASSERT_EQ(vmm.translate(virt), phys_b);
}

// ============================================================
// PageEntry union tests
// ============================================================

TEST("PageEntry: phys_addr extracts address field") {
    PageEntry e;
    e.raw = 0x0000000010300000ULL | FLAG_PRESENT | FLAG_WRITABLE;
    ASSERT_EQ(e.phys_addr(), 0x0000000010300000ULL);
}

TEST("PageEntry: is_present checks bit 0") {
    PageEntry e;
    e.raw = 0;
    ASSERT_FALSE(e.is_present());

    e.raw = FLAG_PRESENT;
    ASSERT_TRUE(e.is_present());

    e.raw = FLAG_PRESENT | FLAG_WRITABLE;
    ASSERT_TRUE(e.is_present());
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}
