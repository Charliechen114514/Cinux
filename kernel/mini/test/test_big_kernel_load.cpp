/**
 * @file kernel/mini/test/test_big_kernel_load.cpp
 * @brief Integration test: mini kernel loads big kernel from disk
 *
 * Verifies the complete big kernel loading pipeline:
 *   L1: ELF magic + header validation (phase 1)
 *   L2: CRC32 integrity checksum
 *   L3: PT_LOAD segment loading via phase 2
 *   L4: Entry point instruction verification
 *
 * Uses the two-phase loader API to allow CRC32 checking between
 * phase 1 (header read) and phase 2 (full ELF load).
 */

#include "../arch/x86_64/paging.hpp"
#include "../big_kernel_loader.hpp"
#include "../driver/ata.hpp"
#include "../lib/crc32.h"
#include "../lib/kprintf.h"
#include "kernel_test.h"

using cinux::mini::loader::BIG_KERNEL_LOAD_ADDR;
using cinux::mini::loader::BIG_KERNEL_ENTRY_VADDR;
using cinux::mini::loader::BIG_KERNEL_LBA;
using cinux::mini::loader::BigKernelLoadState;
using cinux::mini::loader::load_big_kernel_phase1;
using cinux::mini::loader::load_big_kernel_phase2;
using cinux::mini::arch::identity_map_up_to;
using cinux::mini::arch::PAGE_2MB_SIZE;
using cinux::mini::driver::ata::read_large;
using cinux::mini::driver::ata::ATA_SECTOR_SIZE;
using cinux::mini::lib::crc32;
using cinux::mini::lib::crc32_progress;

// Shared state between tests — populated by phase1, used by later tests.
static BigKernelLoadState g_state;
static bool               g_phase1_ok     = false;
static bool               g_full_elf_read = false;
static uint64_t           g_loaded_entry  = 0;

// ============================================================
// Test 1: Phase 1 — read headers and verify ELF magic
// ============================================================

namespace test_big_kernel_elf_magic {

void test_elf_magic() {
    TEST_ASSERT_TRUE(g_phase1_ok);
    const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
    TEST_ASSERT_EQ(magic[0], 0x7F);
    TEST_ASSERT_EQ(magic[1], 'E');
    TEST_ASSERT_EQ(magic[2], 'L');
    TEST_ASSERT_EQ(magic[3], 'F');
}
}  // namespace test_big_kernel_elf_magic

// ============================================================
// Test 2: CRC32 integrity verification
// ============================================================

namespace test_big_kernel_crc32 {

void test_crc32_matches() {
    TEST_ASSERT_TRUE(g_phase1_ok);

    // Ensure the full ELF is in the staging buffer before CRC check.
    // Phase 1 only read 16 sectors (headers). We need the complete file.
    if (!g_full_elf_read) {
        kprintf("  CRC: Reading full ELF (%u sectors) into staging buffer...\n",
                g_state.total_sectors);

        // Extend paging to cover the full staging buffer
        uint64_t staging_end =
            BIG_KERNEL_LOAD_ADDR + static_cast<uint64_t>(g_state.total_sectors) * ATA_SECTOR_SIZE;
        uint64_t highest = staging_end;
        for (uint16_t i = 0; i < g_state.phnum; i++) {
            if (g_state.phdrs[i].p_type == 1 && g_state.phdrs[i].p_memsz > 0) {
                uint64_t seg_end = g_state.phdrs[i].p_paddr + g_state.phdrs[i].p_memsz;
                if (seg_end > highest)
                    highest = seg_end;
            }
        }
        highest = (highest + PAGE_2MB_SIZE - 1) & ~(PAGE_2MB_SIZE - 1);
        identity_map_up_to(highest);

        // Read the full ELF + 1 extra sector for the trailing CRC32
        // (append_crc32.py stores CRC after the ELF data)
        uint32_t sectors_to_read = g_state.total_sectors + 1;
        bool     ok              = read_large(BIG_KERNEL_LBA, sectors_to_read,
                                              reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR));
        TEST_ASSERT_TRUE(ok);
        g_full_elf_read = true;
        kprintf("  CRC: Full ELF read complete.\n");
    }

    const auto* staging = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);

    // CRC32 is stored as 4 bytes immediately after the actual ELF data
    // (raw_elf_end, before sector alignment)
    const auto* stored_crc_ptr = reinterpret_cast<const uint32_t*>(staging + g_state.raw_elf_end);
    uint32_t    stored_crc     = *stored_crc_ptr;

    kprintf("  CRC: Computing CRC32 over %u bytes...\n",
            static_cast<uint32_t>(g_state.raw_elf_end));
    // Compute CRC32 over the ELF data only (with progress for large files)
    auto crc_progress = [](size_t done, size_t total) {
        uint32_t pct = static_cast<uint32_t>((done * 100) / total);
        kprintf("  CRC progress: %u / %u bytes (%u%%)\n", static_cast<uint32_t>(done),
                static_cast<uint32_t>(total), pct);
    };
    uint32_t computed_crc = crc32_progress(staging, g_state.raw_elf_end, crc_progress,
                                           1024 * 1024);  // report every 1MB

    kprintf("  CRC32: stored=0x%x computed=0x%x (elf_end=%u)\n", stored_crc, computed_crc,
            static_cast<uint32_t>(g_state.raw_elf_end));

    TEST_ASSERT_EQ(stored_crc, computed_crc);
}
}  // namespace test_big_kernel_crc32

// ============================================================
// Test 3: Phase 2 — load ELF and verify entry point
// ============================================================

namespace test_big_kernel_load {

void test_load_elf_success() {
    TEST_ASSERT_TRUE(g_phase1_ok);
    uint64_t entry = load_big_kernel_phase2(g_state, BIG_KERNEL_LBA);
    TEST_ASSERT(entry != 0);
    kprintf("  Entry point: 0x%p\n", reinterpret_cast<void*>(entry));
    g_loaded_entry = entry;
}
}  // namespace test_big_kernel_load

// ============================================================
// Test 4: Entry point is at expected physical address
// ============================================================

namespace test_big_kernel_entry {

void test_entry_address() {
    TEST_ASSERT(g_loaded_entry != 0);
    TEST_ASSERT_EQ(g_loaded_entry, BIG_KERNEL_ENTRY_VADDR);
}
}  // namespace test_big_kernel_entry

// ============================================================
// Test 5: First instruction at entry point is 'cli' (0xFA)
// ============================================================

namespace test_big_kernel_first_insn {

void test_first_instruction_is_cli() {
    auto first_byte = *reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
    TEST_ASSERT_EQ(first_byte, 0xFA);
}
}  // namespace test_big_kernel_first_insn

// ============================================================
// Test Entry Point
// ============================================================

extern "C" uint64_t run_big_kernel_load_tests() {
    TEST_SECTION("Big Kernel Load Tests (009)");

    // Run Phase 1 to read headers — all subsequent tests depend on this
    g_phase1_ok = load_big_kernel_phase1(BIG_KERNEL_LBA, g_state);
    if (!g_phase1_ok) {
        kprintf("  FATAL: Phase 1 failed, skipping remaining tests\n");
        TEST_SUMMARY();
        return 0;
    }

    RUN_TEST(test_big_kernel_elf_magic::test_elf_magic);
    RUN_TEST(test_big_kernel_crc32::test_crc32_matches);
    RUN_TEST(test_big_kernel_load::test_load_elf_success);
    RUN_TEST(test_big_kernel_entry::test_entry_address);
    RUN_TEST(test_big_kernel_first_insn::test_first_instruction_is_cli);

    TEST_SUMMARY();
    return g_loaded_entry;
}
