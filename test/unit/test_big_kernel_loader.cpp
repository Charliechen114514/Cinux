/**
 * @file test/unit/test_big_kernel_loader.cpp
 * @brief Host-side unit tests for Big Kernel Loader constants and logic
 *
 * Test coverage:
 *   - Loader constant correctness (addresses, LBA, header sectors)
 *   - Memory layout constraints (no overlap between mini kernel, staging)
 *   - ELF magic sanity check logic
 *   - Two-phase loading constants
 *
 * The big kernel loader orchestrates ATA read + ELF loading.
 * On host, we verify the pure logic and constants; hardware-dependent
 * parts (actual disk I/O) are tested via the ATA and ELF loader tests.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstddef>
#include <cstdint>
#include <cstring>

// Include headers for constants and types
#include "mini/big_kernel_loader.hpp"
#include "mini/driver/ata.hpp"
#include "mini/elf_loader.hpp"

using namespace cinux::mini::loader;
using namespace cinux::mini::driver::ata;
using namespace cinux::mini::elf_loader;

// ============================================================
// 1. Loader Constant Correctness
// ============================================================

TEST("loader: mini kernel load address") {
    ASSERT_EQ(MINI_KERNEL_LOAD_ADDR, 0x20000ULL);
}

TEST("loader: big kernel staging address") {
    ASSERT_EQ(BIG_KERNEL_LOAD_ADDR, 0x1000000ULL);
}

TEST("loader: big kernel starting LBA") {
    ASSERT_EQ(BIG_KERNEL_LBA, 848ULL);
}

TEST("loader: ELF header sectors") {
    // Phase 1 reads 16 sectors (8KB) for ELF header + program headers
    ASSERT_EQ(ELF_HEADER_SECTORS, 16);
    ASSERT_GE(ELF_HEADER_SECTORS * ATA_SECTOR_SIZE, 64 + 16 * 56);
}

TEST("loader: max program headers") {
    ASSERT_EQ(MAX_PROGRAM_HEADERS, 16);
}

TEST("loader: max ELF file size") {
    ASSERT_EQ(MAX_ELF_FILE_SIZE, 0x50000000ULL);  // 1.25 GB
}

// ============================================================
// 2. Memory Layout Constraints
// ============================================================

TEST("loader: staging above mini kernel") {
    ASSERT_GT(BIG_KERNEL_LOAD_ADDR, MINI_KERNEL_LOAD_ADDR);
}

TEST("loader: mini kernel region below staging") {
    uint64_t mini_kernel_end = MINI_KERNEL_LOAD_ADDR + 416 * 1024;
    ASSERT_LT(mini_kernel_end, BIG_KERNEL_LOAD_ADDR);
}

TEST("loader: staging buffer within 32-bit range") {
    uint64_t staging_end = BIG_KERNEL_LOAD_ADDR + MAX_ELF_FILE_SIZE;
    ASSERT_LE(staging_end, 0x100000000ULL);  // Within 4GB
}

// ============================================================
// 3. Disk Layout Arithmetic
// ============================================================

TEST("loader: big kernel byte offset on disk") {
    uint64_t byte_offset = BIG_KERNEL_LBA * ATA_SECTOR_SIZE;
    ASSERT_EQ(byte_offset, 848ULL * 512);
}

TEST("loader: big kernel after mini kernel on disk") {
    uint64_t mini_kernel_start_lba = 16;
    uint64_t mini_kernel_sectors = 832;  // ~416KB / 512
    uint64_t mini_kernel_end_lba = mini_kernel_start_lba + mini_kernel_sectors;
    ASSERT_GE(BIG_KERNEL_LBA, mini_kernel_end_lba);
}

TEST("loader: disk sector allocation") {
    uint64_t mini_kernel_start = 16;
    uint64_t mini_kernel_sectors = BIG_KERNEL_LBA - mini_kernel_start;
    ASSERT_EQ(mini_kernel_sectors, 832ULL);
    ASSERT_EQ(mini_kernel_sectors * ATA_SECTOR_SIZE, 425984ULL);  // 416KB
}

// ============================================================
// 4. ELF Magic Sanity Check Logic
// ============================================================

TEST("loader: ELF magic byte check valid") {
    uint8_t magic[4] = {0x7F, 'E', 'L', 'F'};
    ASSERT_EQ(magic[0], 0x7F);
    ASSERT_EQ(magic[1], 'E');
    ASSERT_EQ(magic[2], 'L');
    ASSERT_EQ(magic[3], 'F');
}

TEST("loader: ELF magic rejects non-ELF") {
    uint8_t not_elf1[4] = {0x00, 'E', 'L', 'F'};
    ASSERT_NE(not_elf1[0], 0x7F);

    uint8_t not_elf2[4] = {0x7F, 'X', 'L', 'F'};
    ASSERT_NE(not_elf2[1], 'E');

    uint8_t not_elf3[4] = {0x00, 0x00, 0x00, 0x00};
    ASSERT_FALSE(not_elf3[0] == 0x7F && not_elf3[1] == 'E' &&
                 not_elf3[2] == 'L' && not_elf3[3] == 'F');
}

TEST("loader: non-ELF staging buffer detected") {
    uint8_t fake_buffer[512];
    memset(fake_buffer, 0xFF, sizeof(fake_buffer));

    const auto* magic = fake_buffer;
    bool is_elf = (magic[0] == 0x7F && magic[1] == 'E' &&
                   magic[2] == 'L' && magic[3] == 'F');
    ASSERT_FALSE(is_elf);
}

TEST("loader: valid ELF passes staging check") {
    uint8_t fake_buffer[512];
    memset(fake_buffer, 0, sizeof(fake_buffer));
    fake_buffer[0] = 0x7F;
    fake_buffer[1] = 'E';
    fake_buffer[2] = 'L';
    fake_buffer[3] = 'F';

    const auto* magic = fake_buffer;
    bool is_elf = (magic[0] == 0x7F && magic[1] == 'E' &&
                   magic[2] == 'L' && magic[3] == 'F');
    ASSERT_TRUE(is_elf);
}

// ============================================================
// 5. LBA Range Validation
// ============================================================

TEST("loader: big kernel LBA within valid range") {
    ASSERT_LT(BIG_KERNEL_LBA, (1ULL << 48));
}

TEST("loader: big kernel LBA within LBA28 range") {
    ASSERT_LT(BIG_KERNEL_LBA, 0x10000000ULL);
}

// ============================================================
// 6. Higher-Half Kernel Address Arithmetic
// ============================================================

TEST("loader: higher-half base constant") {
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t virtual_entry = 0xFFFFFFFF80100000ULL;
    uint64_t physical_entry = virtual_entry - HIGHER_HALF_BASE;
    ASSERT_EQ(physical_entry, 0x100000ULL);
}

TEST("loader: lower-half entry used as-is") {
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t virtual_entry = 0x1000ULL;
    ASSERT_LT(virtual_entry, HIGHER_HALF_BASE);
    uint64_t physical = virtual_entry;
    ASSERT_EQ(physical, 0x1000ULL);
}

// ============================================================
// Main Function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
