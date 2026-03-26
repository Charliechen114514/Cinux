/* ==============================================================
 * Cinux Mini Kernel - PMM Tests
 * ============================================================== */

#include "kernel_test.h"
#include "../mm/pmm.h"
#include "../../../boot/boot_info.h"

using namespace cinux::mini::mm::pmm;

// ============================================================
// Mock BootInfo for testing
// ============================================================
namespace test_mock {
    static BootInfo s_test_boot_info;
    static MemoryMapEntry s_test_mmap[4];

    BootInfo* create_test_bootinfo() {
        // QEMU typical memory layout: 128MB
        // Entry 0: Low 640KB (usable)
        s_test_mmap[0].base = 0x00000000;
        s_test_mmap[0].length = 0x000A0000;
        s_test_mmap[0].type = 1;  // Usable
        s_test_mmap[0].acpi = 0;

        // Entry 1: 640KB-1MB (reserved)
        s_test_mmap[1].base = 0x000A0000;
        s_test_mmap[1].length = 0x00060000;
        s_test_mmap[1].type = 2;  // Reserved
        s_test_mmap[1].acpi = 0;

        // Entry 2: Main memory 1MB-128MB
        s_test_mmap[2].base = 0x00100000;
        s_test_mmap[2].length = 127 * 1024 * 1024;
        s_test_mmap[2].type = 1;  // Usable
        s_test_mmap[2].acpi = 0;

        s_test_boot_info.mmap_count = 3;
        s_test_boot_info.entry_point = 0xFFFFFFFF80020000;
        s_test_boot_info.kernel_phys_base = 0x20000;
        s_test_boot_info.kernel_size = 0x40000;  // 256KB

        for (uint32_t i = 0; i < 3; i++) {
            s_test_boot_info.mmap[i] = s_test_mmap[i];
        }

        return &s_test_boot_info;
    }

    BootInfo* create_small_bootinfo() {
        // Small memory map for OOM test (2MB only)
        s_test_boot_info.mmap_count = 1;
        s_test_boot_info.mmap[0].base = 0x00100000;
        s_test_boot_info.mmap[0].length = 2 * 1024 * 1024;
        s_test_boot_info.mmap[0].type = 1;
        s_test_boot_info.mmap[0].acpi = 0;

        return &s_test_boot_info;
    }
}

// ============================================================
// Test 1: Linker Symbol Access (006-01)
// ============================================================
namespace test_linker_symbols {
    extern "C" {
        extern char __kernel_size;
        extern char __mini_kernel_end;
        extern char __bss_start;
        extern char __bss_end;
    }

    void test_linker_symbol_access() {
        // 正确访问链接器符号：使用 & 取地址
        uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
        uint64_t kernel_end = reinterpret_cast<uint64_t>(&__mini_kernel_end);
        uint64_t bss_start = reinterpret_cast<uint64_t>(&__bss_start);
        uint64_t bss_end = reinterpret_cast<uint64_t>(&__bss_end);

        kprintf("  kernel_size=0x%x, kernel_end=0x%x\n", kernel_size, kernel_end);
        kprintf("  bss_start=0x%x, bss_end=0x%x\n", bss_start, bss_end);

        TEST_ASSERT_GT(kernel_size, 0);
        TEST_ASSERT_GE(kernel_end, bss_end);  // __mini_kernel_end 在 BSS 末尾或之后
        TEST_ASSERT_GT(bss_end, bss_start);
        TEST_ASSERT((bss_end - bss_start) < 1024 * 1024);  // BSS < 1MB
    }
}

// ============================================================
// Test 2: PMM Initialization
// ============================================================
namespace test_pmm_init {
    void test_initialization() {
        init(test_mock::create_test_bootinfo());

        uint64_t total = total_page_count();
        uint64_t free = free_page_count();

        kprintf("  Total: %u pages (%u MB), Free: %u pages\n",
                total, (total * PAGE_SIZE) / (1024 * 1024), free);

        TEST_ASSERT_GT(total, 0);
        TEST_ASSERT_GT(free, 0);
        TEST_ASSERT_LT(free, total);
    }
}

// ============================================================
// Test 3: Single Page Allocation
// ============================================================
namespace test_pmm_alloc {
    void test_single_allocation() {
        init(test_mock::create_test_bootinfo());

        uint64_t free_before = free_page_count();
        uint64_t page = alloc_page();

        TEST_ASSERT_NE(page, 0);
        TEST_ASSERT((page & (PAGE_SIZE - 1)) == 0);  // 4KB 对齐
        TEST_ASSERT_EQ(free_page_count(), free_before - 1);

        kprintf("  Allocated page at 0x%x\n", page);

        free_page(page);
        TEST_ASSERT_EQ(free_page_count(), free_before);
    }
}

// ============================================================
// Test 4: Multiple Allocations
// ============================================================
namespace test_pmm_multi {
    void test_multiple_allocations() {
        init(test_mock::create_test_bootinfo());

        constexpr int N = 10;
        uint64_t pages[N];
        uint64_t free_before = free_page_count();

        for (int i = 0; i < N; i++) {
            pages[i] = alloc_page();
            TEST_ASSERT_NE(pages[i], 0);
        }

        TEST_ASSERT_EQ(free_page_count(), free_before - N);

        for (int i = 0; i < N; i++) {
            free_page(pages[i]);
        }

        TEST_ASSERT_EQ(free_page_count(), free_before);
    }
}

// ============================================================
// Test 5: Edge Cases
// ============================================================
namespace test_pmm_edge {
    void test_edge_cases() {
        init(test_mock::create_test_bootinfo());

        uint64_t free_before = free_page_count();

        // Free page 0 (should be ignored)
        free_page(0);
        TEST_ASSERT_EQ(free_page_count(), free_before);

        // Free invalid address
        free_page(MAX_MEMORY + PAGE_SIZE);
        TEST_ASSERT_EQ(free_page_count(), free_before);

        // Double free
        uint64_t page = alloc_page();
        TEST_ASSERT_NE(page, 0);
        free_page(page);
        free_page(page);  // Double free - no effect
        TEST_ASSERT_EQ(free_page_count(), free_before);
    }
}

// ============================================================
// Test 6: OOM Handling
// ============================================================
namespace test_pmm_oom {
    void test_oom() {
        init(test_mock::create_small_bootinfo());

        uint64_t page;
        int count = 0;

        while ((page = alloc_page()) != 0) {
            count++;
        }

        kprintf("  Allocated %d pages before OOM\n", count);
        TEST_ASSERT_EQ(free_page_count(), 0);
        TEST_ASSERT_EQ(alloc_page(), 0);  // Should return 0
    }
}

// ============================================================
// Test Entry Point
// ============================================================
extern "C" void run_pmm_tests() {
    TEST_SECTION("PMM Tests");

    RUN_TEST(test_linker_symbols::test_linker_symbol_access);
    RUN_TEST(test_pmm_init::test_initialization);
    RUN_TEST(test_pmm_alloc::test_single_allocation);
    RUN_TEST(test_pmm_multi::test_multiple_allocations);
    RUN_TEST(test_pmm_edge::test_edge_cases);
    RUN_TEST(test_pmm_oom::test_oom);

    TEST_SUMMARY();
}
