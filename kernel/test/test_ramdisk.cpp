/**
 * @file kernel/test/test_ramdisk.cpp
 * @brief QEMU in-kernel integration tests for the Ramdisk driver (026)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that:
 *   - The embedded initrd archive is non-empty and has a valid base pointer
 *   - Ramdisk::mount() successfully parses the ustar entries
 *   - At least one file entry is found (hello.txt expected in test archive)
 *   - octal_to_uint() correctly parses ustar size fields
 *   - The UstarHeader struct is exactly 512 bytes
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 */

#include <stdint.h>
#include <stddef.h>

#include "big_kernel_test.h"

#include "kernel/fs/ramdisk.hpp"
#include "kernel/fs/ramdisk_config.hpp"

using cinux::fs::Ramdisk;
using cinux::fs::UstarHeader;
using cinux::fs::octal_to_uint;

// ============================================================
// Test 1: UstarHeader struct size is exactly 512 bytes
// ============================================================

namespace test_ramdisk_struct {

void test_ustar_header_size() {
    TEST_ASSERT_EQ(sizeof(UstarHeader), 512u);
}

}  // namespace test_ramdisk_struct

// ============================================================
// Test 2: octal_to_uint correctly parses octal ASCII strings
// ============================================================

namespace test_ramdisk_octal {

void test_octal_zero() {
    // "00000000000" -> 0
    TEST_ASSERT_EQ(octal_to_uint("00000000000", 11), 0ull);
}

void test_octal_small_value() {
    // "00000000012" -> 10 (octal 12 = decimal 10)
    TEST_ASSERT_EQ(octal_to_uint("00000000012", 11), 10ull);
}

void test_octal_with_null_terminator() {
    // String "144\0" -> octal 144 = decimal 100
    char buf[8] = {'1', '4', '4', '\0', '7', '7', '7', '7'};
    TEST_ASSERT_EQ(octal_to_uint(buf, 8), 100ull);
}

void test_octal_with_space_padding() {
    // "1234  \0" -> octal 1234 = decimal 668
    // octal_to_uint stops at the first space, so trailing spaces terminate
    char buf[8] = {'1', '2', '3', '4', ' ', ' ', '\0', ' '};
    TEST_ASSERT_EQ(octal_to_uint(buf, 8), 668ull);
}

void test_octal_block_size() {
    // "00000001000" -> octal 1000 = decimal 512
    TEST_ASSERT_EQ(octal_to_uint("00000001000", 11), 512ull);
}

}  // namespace test_ramdisk_octal

// ============================================================
// Test 3: Ramdisk mount succeeds with embedded initrd
// ============================================================

namespace test_ramdisk_mount {

void test_ramdisk_base_not_null() {
    Ramdisk rd;
    rd.mount();
    TEST_ASSERT_NOT_NULL(rd.base());
}

void test_ramdisk_size_nonzero() {
    Ramdisk rd;
    rd.mount();
    TEST_ASSERT_GT(rd.total_size(), 0ull);
}

void test_ramdisk_mount_finds_files() {
    Ramdisk rd;
    uint32_t count = rd.mount();
    // The test initrd.tar contains 3 files: hello.txt, readme.txt, etc/passwd
    TEST_ASSERT_EQ(count, 3u);
}

}  // namespace test_ramdisk_mount

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ramdisk_tests() {
    TEST_SECTION("Ramdisk Tests (026)");

    RUN_TEST(test_ramdisk_struct::test_ustar_header_size);

    RUN_TEST(test_ramdisk_octal::test_octal_zero);
    RUN_TEST(test_ramdisk_octal::test_octal_small_value);
    RUN_TEST(test_ramdisk_octal::test_octal_with_null_terminator);
    RUN_TEST(test_ramdisk_octal::test_octal_with_space_padding);
    RUN_TEST(test_ramdisk_octal::test_octal_block_size);

    RUN_TEST(test_ramdisk_mount::test_ramdisk_base_not_null);
    RUN_TEST(test_ramdisk_mount::test_ramdisk_size_nonzero);
    RUN_TEST(test_ramdisk_mount::test_ramdisk_mount_finds_files);

    TEST_SUMMARY();
}
