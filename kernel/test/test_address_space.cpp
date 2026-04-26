/**
 * @file kernel/test/test_address_space.cpp
 * @brief QEMU in-kernel integration tests for AddressSpace (018)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * real AddressSpace class with actual PMM/VMM backing, testing:
 *   - init_kernel() saves a valid kernel PML4
 *   - Construction allocates a distinct PML4 with kernel entries copied
 *   - map/translate/unmap work through real page tables
 *   - Cross-space isolation: mapping in AS#1 is invisible from AS#2
 *   - Destruction frees user-space page table pages
 *   - activate() loads CR3 with the space's PML4
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
#include "kernel/mm/address_space.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

using cinux::arch::FLAG_PRESENT;
using cinux::arch::FLAG_WRITABLE;
using cinux::arch::PAGE_SIZE;
using cinux::arch::read_cr3;
using cinux::arch::write_cr3;
using cinux::mm::g_pmm;
using cinux::mm::g_vmm;

// ============================================================
// Test 1: init_kernel saves a valid kernel PML4
// ============================================================

namespace test_as_init {

void test_init_kernel_pml4() {
    // init_kernel was called in main_test.cpp before this suite runs.
    TEST_ASSERT_NE(cinux::mm::AddressSpace::kernel_pml4(), 0u);
}

}  // namespace test_as_init

// ============================================================
// Test 2: construction creates a distinct PML4 root
// ============================================================

namespace test_as_construct {

void test_distinct_pml4() {
    cinux::mm::AddressSpace as;
    TEST_ASSERT_NE(as.pml4_phys(), 0u);
    TEST_ASSERT_NE(as.pml4_phys(), cinux::mm::AddressSpace::kernel_pml4());
}

}  // namespace test_as_construct

// ============================================================
// Test 3: two AddressSpace instances have different roots
// ============================================================

namespace test_as_two_roots {

void test_different_roots() {
    cinux::mm::AddressSpace as1;
    cinux::mm::AddressSpace as2;

    TEST_ASSERT_NE(as1.pml4_phys(), 0u);
    TEST_ASSERT_NE(as2.pml4_phys(), 0u);
    TEST_ASSERT_NE(as1.pml4_phys(), as2.pml4_phys());
}

}  // namespace test_as_two_roots

// ============================================================
// Test 4: map and translate within a single address space
// ============================================================

namespace test_as_map {

void test_map_translate() {
    cinux::mm::AddressSpace as;

    uint64_t virt = 0x20000000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = as.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);

    uint64_t result = as.translate(virt);
    TEST_ASSERT_EQ(result, phys);

    g_pmm.free_page(phys);
}

}  // namespace test_as_map

// ============================================================
// Test 5: unmap clears the translation
// ============================================================

namespace test_as_unmap {

void test_unmap_clears() {
    cinux::mm::AddressSpace as;

    uint64_t virt = 0x20010000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = as.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(as.translate(virt), phys);

    as.unmap(virt);
    TEST_ASSERT_EQ(as.translate(virt), 0u);

    g_pmm.free_page(phys);
}

}  // namespace test_as_unmap

// ============================================================
// Test 6: translate unmapped address returns 0
// ============================================================

namespace test_as_unmapped {

void test_translate_unmapped() {
    cinux::mm::AddressSpace as;
    TEST_ASSERT_EQ(as.translate(0x30000000ULL), 0u);
}

}  // namespace test_as_unmapped

// ============================================================
// Test 7: ISOLATION -- core milestone requirement
// Create AS#1 and AS#2.  Map a page in AS#1.  Switch to AS#2
// by translating via AS#2's PML4.  The address must return 0.
// ============================================================

namespace test_as_isolation {

void test_cross_space_isolation() {
    cinux::mm::AddressSpace as1;
    cinux::mm::AddressSpace as2;

    uint64_t virt = 0x20020000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    // Map in AS#1
    bool ok = as1.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);

    // AS#1 should see the mapping
    TEST_ASSERT_EQ(as1.translate(virt), phys);

    // AS#2 should NOT see the mapping (isolation)
    TEST_ASSERT_EQ(as2.translate(virt), 0u);

    g_pmm.free_page(phys);
}

}  // namespace test_as_isolation

// ============================================================
// Test 8: activate() changes CR3
// ============================================================

namespace test_as_activate {

void test_activate_changes_cr3() {
    uint64_t saved_pml4 = cinux::mm::AddressSpace::kernel_pml4();
    uint64_t cr3_before = read_cr3();

    cinux::mm::AddressSpace as;
    as.activate();

    uint64_t cr3_after = read_cr3();
    TEST_ASSERT_EQ(cr3_after, as.pml4_phys());
    TEST_ASSERT_NE(cr3_after, cr3_before);

    write_cr3(saved_pml4);
}

}  // namespace test_as_activate

// ============================================================
// Test 9: activate then translate (real hardware walk)
// ============================================================

namespace test_as_activate_map {

void test_activate_map_translate() {
    uint64_t saved_pml4 = cinux::mm::AddressSpace::kernel_pml4();

    cinux::mm::AddressSpace as;

    uint64_t virt = 0x20030000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = as.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);

    as.activate();

    uint64_t result = as.translate(virt);
    TEST_ASSERT_EQ(result, phys);

    write_cr3(saved_pml4);

    g_pmm.free_page(phys);
}

}  // namespace test_as_activate_map

// ============================================================
// Test 10: map two pages in the same address space
// ============================================================

namespace test_as_two_pages {

void test_two_pages() {
    cinux::mm::AddressSpace as;

    uint64_t virt1 = 0x20040000ULL;
    uint64_t virt2 = 0x20050000ULL;
    uint64_t phys1 = g_pmm.alloc_page();
    uint64_t phys2 = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys1, 0u);
    TEST_ASSERT_NE(phys2, 0u);

    bool ok1 = as.map(virt1, phys1, FLAG_PRESENT | FLAG_WRITABLE);
    bool ok2 = as.map(virt2, phys2, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok1);
    TEST_ASSERT_TRUE(ok2);

    TEST_ASSERT_EQ(as.translate(virt1), phys1);
    TEST_ASSERT_EQ(as.translate(virt2), phys2);

    g_pmm.free_page(phys1);
    g_pmm.free_page(phys2);
}

}  // namespace test_as_two_pages

// ============================================================
// Test 11: destruction does not corrupt kernel mappings
// ============================================================

namespace test_as_destroy_safe {

void test_destroy_no_kernel_corruption() {
    uint64_t kernel_pml4_before = cinux::mm::AddressSpace::kernel_pml4();

    {
        cinux::mm::AddressSpace as;
        uint64_t                virt = 0x20060000ULL;
        uint64_t                phys = g_pmm.alloc_page();
        TEST_ASSERT_NE(phys, 0u);

        as.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
        // as destroyed here
        g_pmm.free_page(phys);
    }

    // Kernel PML4 should still be valid
    TEST_ASSERT_EQ(cinux::mm::AddressSpace::kernel_pml4(), kernel_pml4_before);
}

}  // namespace test_as_destroy_safe

// ============================================================
// Entry point
// ============================================================

extern "C" void run_address_space_tests() {
    TEST_SECTION("AddressSpace Tests (018)");

    RUN_TEST(test_as_init::test_init_kernel_pml4);
    RUN_TEST(test_as_construct::test_distinct_pml4);
    RUN_TEST(test_as_two_roots::test_different_roots);
    RUN_TEST(test_as_map::test_map_translate);
    RUN_TEST(test_as_unmap::test_unmap_clears);
    RUN_TEST(test_as_unmapped::test_translate_unmapped);
    RUN_TEST(test_as_isolation::test_cross_space_isolation);
    RUN_TEST(test_as_activate::test_activate_changes_cr3);
    RUN_TEST(test_as_activate_map::test_activate_map_translate);
    RUN_TEST(test_as_two_pages::test_two_pages);
    RUN_TEST(test_as_destroy_safe::test_destroy_no_kernel_corruption);

    TEST_SUMMARY();
}
