/**
 * @file kernel/test/test_vmm.cpp
 * @brief QEMU in-kernel integration tests for the Virtual Memory Manager (016)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that
 * VMM::init() completes successfully, that map/translate/unmap work with
 * real page tables, and that demand-paging #PF handler can resolve
 * not-present faults.
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised (g_pmm.init called)
 *   - VMM initialised (g_vmm.init called)
 */

#include <stdint.h>

#include "big_kernel_test.h"

#include "kernel/arch/x86_64/paging.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

using cinux::arch::FLAG_PRESENT;
using cinux::arch::FLAG_WRITABLE;
using cinux::arch::PAGE_SIZE;
using cinux::mm::g_pmm;
using cinux::mm::g_vmm;

// ============================================================
// Test 1: VMM initialised with valid kernel PML4
// ============================================================

namespace test_vmm_init {

void test_init_pml4() {
    TEST_ASSERT_NE(g_vmm.kernel_pml4(), 0u);
}

}  // namespace test_vmm_init

// ============================================================
// Test 2: map then translate returns correct physical address
// ============================================================

namespace test_vmm_map {

void test_map_translate() {
    uint64_t virt = 0x20000000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = g_vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);

    uint64_t result = g_vmm.translate(virt);
    TEST_ASSERT_EQ(result, phys);

    g_vmm.unmap(virt);
    g_pmm.free_page(phys);
}

}  // namespace test_vmm_map

// ============================================================
// Test 3: translate preserves in-page offset
// ============================================================

namespace test_vmm_offset {

void test_translate_offset() {
    uint64_t virt_base = 0x20010000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = g_vmm.map(virt_base, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);

    uint64_t result = g_vmm.translate(virt_base + 0x123);
    TEST_ASSERT_EQ(result, phys + 0x123);

    g_vmm.unmap(virt_base);
    g_pmm.free_page(phys);
}

}  // namespace test_vmm_offset

// ============================================================
// Test 4: unmap causes translate to return 0
// ============================================================

namespace test_vmm_unmap {

void test_unmap_clears() {
    uint64_t virt = 0x20020000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = g_vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(g_vmm.translate(virt), phys);

    g_vmm.unmap(virt);
    TEST_ASSERT_EQ(g_vmm.translate(virt), 0u);

    g_pmm.free_page(phys);
}

}  // namespace test_vmm_unmap

// ============================================================
// Test 5: translate unmapped address returns 0
// ============================================================

namespace test_vmm_unmapped {

void test_translate_unmapped() {
    uint64_t result = g_vmm.translate(0x30000000ULL);
    TEST_ASSERT_EQ(result, 0u);
}

}  // namespace test_vmm_unmapped

// ============================================================
// Test 6: map two pages and verify both
// ============================================================

namespace test_vmm_two_pages {

void test_two_pages() {
    uint64_t virt1 = 0x20030000ULL;
    uint64_t virt2 = 0x20040000ULL;
    uint64_t phys1 = g_pmm.alloc_page();
    uint64_t phys2 = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys1, 0u);
    TEST_ASSERT_NE(phys2, 0u);

    bool ok1 = g_vmm.map(virt1, phys1, FLAG_PRESENT | FLAG_WRITABLE);
    bool ok2 = g_vmm.map(virt2, phys2, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok1);
    TEST_ASSERT_TRUE(ok2);

    TEST_ASSERT_EQ(g_vmm.translate(virt1), phys1);
    TEST_ASSERT_EQ(g_vmm.translate(virt2), phys2);

    g_vmm.unmap(virt1);
    g_vmm.unmap(virt2);
    g_pmm.free_page(phys1);
    g_pmm.free_page(phys2);
}

}  // namespace test_vmm_two_pages

// ============================================================
// Test 7: remap replaces previous mapping
// ============================================================

namespace test_vmm_remap {

void test_remap() {
    uint64_t virt = 0x20050000ULL;
    uint64_t phys1 = g_pmm.alloc_page();
    uint64_t phys2 = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys1, 0u);
    TEST_ASSERT_NE(phys2, 0u);

    bool ok1 = g_vmm.map(virt, phys1, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok1);
    TEST_ASSERT_EQ(g_vmm.translate(virt), phys1);

    bool ok2 = g_vmm.map(virt, phys2, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok2);
    TEST_ASSERT_EQ(g_vmm.translate(virt), phys2);

    g_vmm.unmap(virt);
    g_pmm.free_page(phys1);
    g_pmm.free_page(phys2);
}

}  // namespace test_vmm_remap

// ============================================================
// Test 8: unmap on never-mapped address is a no-op
// ============================================================

namespace test_vmm_unmap_noop {

void test_unmap_noop() {
    g_vmm.unmap(0x20060000ULL);
    TEST_ASSERT_EQ(g_vmm.translate(0x20060000ULL), 0u);
}

}  // namespace test_vmm_unmap_noop

// ============================================================
// Test 9: map at high canonical address (kernel space)
// ============================================================

namespace test_vmm_high_addr {

void test_high_address() {
    uint64_t virt = 0xFFFFFFFF80000000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = g_vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(g_vmm.translate(virt), phys);

    g_vmm.unmap(virt);
    g_pmm.free_page(phys);
}

}  // namespace test_vmm_high_addr

// ============================================================
// Test 10: demand-paging — access unmapped address triggers #PF,
//          handler allocates a page, write is visible afterwards
// ============================================================

namespace test_vmm_demand {

void test_demand_page() {
    volatile uint64_t* ptr = reinterpret_cast<volatile uint64_t*>(0x40000000ULL);

    *ptr = 0xCAFEBABEDEADC0DEULL;

    TEST_ASSERT_EQ(*ptr, 0xCAFEBABEDEADC0DEULL);

    uint64_t phys = g_vmm.translate(0x40000000ULL);
    TEST_ASSERT_NE(phys, 0u);

    g_vmm.unmap(0x40000000ULL);
    g_pmm.free_page(phys);
}

}  // namespace test_vmm_demand

// ============================================================
// Entry point
// ============================================================

extern "C" void run_vmm_tests() {
    TEST_SECTION("VMM Tests (016)");

    RUN_TEST(test_vmm_init::test_init_pml4);
    RUN_TEST(test_vmm_map::test_map_translate);
    RUN_TEST(test_vmm_offset::test_translate_offset);
    RUN_TEST(test_vmm_unmap::test_unmap_clears);
    RUN_TEST(test_vmm_unmapped::test_translate_unmapped);
    RUN_TEST(test_vmm_two_pages::test_two_pages);
    RUN_TEST(test_vmm_remap::test_remap);
    RUN_TEST(test_vmm_unmap_noop::test_unmap_noop);
    RUN_TEST(test_vmm_high_addr::test_high_address);
    RUN_TEST(test_vmm_demand::test_demand_page);

    TEST_SUMMARY();
}
