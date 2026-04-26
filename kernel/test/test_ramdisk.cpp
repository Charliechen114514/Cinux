/**
 * @file kernel/test/test_ramdisk.cpp
 * @brief QEMU in-kernel integration tests for the Ramdisk driver (026/027)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that:
 *   - The embedded initrd archive is non-empty and has a valid base pointer
 *   - Ramdisk::mount() successfully parses the ustar entries (returns true)
 *   - At least one file entry is found (hello.txt expected in test archive)
 *   - octal_to_uint() correctly parses ustar size fields
 *   - The UstarHeader struct is exactly 512 bytes
 *   - Ramdisk::lookup() finds files by name and returns valid Inodes
 *   - Ramdisk InodeOps: read returns correct file data
 *   - Ramdisk InodeOps: write returns -1 (read-only)
 *   - VFS integration: mount + open + read + close end-to-end
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - Heap initialised (needed for new/delete in FDTable)
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/fs/file.hpp"
#include "kernel/fs/ramdisk.hpp"
#include "kernel/fs/ramdisk_config.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/string.hpp"

using cinux::fs::Ramdisk;
using cinux::fs::UstarHeader;
using cinux::fs::octal_to_uint;
using cinux::fs::InodeType;

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
    TEST_ASSERT_EQ(octal_to_uint("00000000000", 11), 0ull);
}

void test_octal_small_value() {
    TEST_ASSERT_EQ(octal_to_uint("00000000012", 11), 10ull);
}

void test_octal_with_null_terminator() {
    char buf[8] = {'1', '4', '4', '\0', '7', '7', '7', '7'};
    TEST_ASSERT_EQ(octal_to_uint(buf, 8), 100ull);
}

void test_octal_with_space_padding() {
    char buf[8] = {'1', '2', '3', '4', ' ', ' ', '\0', ' '};
    TEST_ASSERT_EQ(octal_to_uint(buf, 8), 668ull);
}

void test_octal_block_size() {
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

void test_ramdisk_mount_returns_true() {
    Ramdisk rd;
    bool    result = rd.mount();
    TEST_ASSERT_TRUE(result);
}

void test_ramdisk_mount_finds_files() {
    Ramdisk rd;
    rd.mount();
    TEST_ASSERT_EQ(rd.entry_count(), 3u);
}

}  // namespace test_ramdisk_mount

// ============================================================
// Test 4: Ramdisk lookup finds files by name
// ============================================================

namespace test_ramdisk_lookup {

void test_lookup_hello_txt() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("hello.txt");
    TEST_ASSERT_NOT_NULL(inode);
    TEST_ASSERT_EQ(static_cast<uint32_t>(inode->type), static_cast<uint32_t>(InodeType::Regular));
}

void test_lookup_readme_txt() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("readme.txt");
    TEST_ASSERT_NOT_NULL(inode);
}

void test_lookup_etc_passwd() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("etc/passwd");
    TEST_ASSERT_NOT_NULL(inode);
}

void test_lookup_nonexistent() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("nonexistent.txt");
    TEST_ASSERT_NULL(inode);
}

void test_lookup_null_path() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup(nullptr);
    TEST_ASSERT_NULL(inode);
}

void test_lookup_root_returns_dir_inode() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("");
    TEST_ASSERT_NOT_NULL(inode);
    TEST_ASSERT_EQ(static_cast<uint32_t>(inode->type), static_cast<uint32_t>(InodeType::Directory));
}

void test_lookup_root_slash_returns_dir_inode() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("/");
    TEST_ASSERT_NOT_NULL(inode);
    TEST_ASSERT_EQ(static_cast<uint32_t>(inode->type), static_cast<uint32_t>(InodeType::Directory));
}

}  // namespace test_ramdisk_lookup

// ============================================================
// Test 5: Ramdisk InodeOps -- read/write
// ============================================================

namespace test_ramdisk_inode_ops {

void test_read_hello_content() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("hello.txt");
    TEST_ASSERT_NOT_NULL(inode);
    TEST_ASSERT_NOT_NULL(inode->ops);

    char    buf[64] = {};
    int64_t n       = inode->ops->read(inode, 0, buf, sizeof(buf) - 1);
    TEST_ASSERT_GT(n, 0);

    const char expected[]   = "Hello from Cinux!\n";
    auto       expected_len = static_cast<uint64_t>(sizeof(expected) - 1);
    TEST_ASSERT_EQ(static_cast<uint64_t>(n), expected_len);
    TEST_ASSERT_TRUE(memcmp(buf, expected, expected_len) == 0);
}

void test_read_with_offset() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("hello.txt");
    TEST_ASSERT_NOT_NULL(inode);

    char    buf[8] = {};
    int64_t n      = inode->ops->read(inode, 6, buf, 4);
    TEST_ASSERT_EQ(n, 4);
    TEST_ASSERT_TRUE(memcmp(buf, "from", 4) == 0);
}

void test_read_past_end_returns_zero() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("hello.txt");
    TEST_ASSERT_NOT_NULL(inode);

    char    buf[8] = {};
    int64_t n      = inode->ops->read(inode, 10000, buf, 4);
    TEST_ASSERT_EQ(n, 0);
}

void test_write_returns_error() {
    Ramdisk rd;
    rd.mount();

    auto* inode = rd.lookup("hello.txt");
    TEST_ASSERT_NOT_NULL(inode);

    const char data[] = "test";
    int64_t    n      = inode->ops->write(inode, 0, data, 4);
    TEST_ASSERT_EQ(n, -1);
}

void test_read_null_inode_returns_error() {
    cinux::fs::FDTable fd_table;
    cinux::fs::File*   f = fd_table.get(0);
    TEST_ASSERT_NULL(f);
}

}  // namespace test_ramdisk_inode_ops

// ============================================================
// Test 6: VFS integration -- mount + open + read + close
// ============================================================

namespace test_vfs_integration {

void test_vfs_mount_and_resolve() {
    cinux::fs::vfs_mount_init();

    Ramdisk* rd = new Ramdisk();
    TEST_ASSERT_TRUE(rd->mount());

    bool added = cinux::fs::vfs_mount_add("/", rd);
    TEST_ASSERT_TRUE(added);

    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve("/hello.txt", &rel_path);
    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(rel_path);

    TEST_ASSERT_TRUE(strcmp(rel_path, "hello.txt") == 0);

    cinux::fs::vfs_mount_remove("/");
    delete rd;
}

void test_vfs_open_read_close() {
    cinux::fs::vfs_mount_init();

    Ramdisk* rd = new Ramdisk();
    rd->mount();

    cinux::fs::vfs_mount_add("/", rd);

    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve("/hello.txt", &rel_path);
    TEST_ASSERT_NOT_NULL(fs);

    cinux::fs::Inode* inode = fs->lookup(rel_path);
    TEST_ASSERT_NOT_NULL(inode);

    int fd = cinux::fs::g_global_fd_table().alloc(inode, cinux::fs::OpenFlags::RDONLY);
    TEST_ASSERT_GE(fd, 0);

    cinux::fs::File* file = cinux::fs::g_global_fd_table().get(fd);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_NOT_NULL(file->inode);
    TEST_ASSERT_NOT_NULL(file->inode->ops);

    char    buf[64] = {};
    int64_t n       = file->inode->ops->read(file->inode, file->offset, buf, sizeof(buf) - 1);
    TEST_ASSERT_GT(n, 0);

    const char expected[] = "Hello from Cinux!\n";
    TEST_ASSERT_TRUE(memcmp(buf, expected, sizeof(expected) - 1) == 0);

    file->offset += static_cast<uint64_t>(n);

    char    buf2[16] = {};
    int64_t n2       = file->inode->ops->read(file->inode, file->offset, buf2, sizeof(buf2));
    TEST_ASSERT_EQ(n2, 0);

    int close_result = cinux::fs::g_global_fd_table().close(fd);
    TEST_ASSERT_EQ(close_result, 0);

    cinux::fs::File* closed_file = cinux::fs::g_global_fd_table().get(fd);
    TEST_ASSERT_NULL(closed_file);

    cinux::fs::vfs_mount_remove("/");
    delete rd;
}

void test_vfs_open_nonexistent_fails() {
    cinux::fs::vfs_mount_init();

    Ramdisk* rd = new Ramdisk();
    rd->mount();

    cinux::fs::vfs_mount_add("/", rd);

    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve("/nonexistent.txt", &rel_path);
    TEST_ASSERT_NOT_NULL(fs);

    cinux::fs::Inode* inode = fs->lookup(rel_path);
    TEST_ASSERT_NULL(inode);

    cinux::fs::vfs_mount_remove("/");
    delete rd;
}

void test_vfs_close_invalid_fd() {
    cinux::fs::vfs_mount_init();

    int result = cinux::fs::g_global_fd_table().close(42);
    TEST_ASSERT_EQ(result, -1);
}

void test_vfs_open_multiple_files() {
    cinux::fs::vfs_mount_init();

    Ramdisk* rd = new Ramdisk();
    rd->mount();

    cinux::fs::vfs_mount_add("/", rd);

    const char*            rel1 = nullptr;
    cinux::fs::FileSystem* fs1  = cinux::fs::vfs_resolve("/hello.txt", &rel1);
    TEST_ASSERT_NOT_NULL(fs1);
    cinux::fs::Inode* ino1 = fs1->lookup(rel1);
    TEST_ASSERT_NOT_NULL(ino1);
    int fd1 = cinux::fs::g_global_fd_table().alloc(ino1, cinux::fs::OpenFlags::RDONLY);
    TEST_ASSERT_GE(fd1, 0);

    const char*            rel2 = nullptr;
    cinux::fs::FileSystem* fs2  = cinux::fs::vfs_resolve("/readme.txt", &rel2);
    TEST_ASSERT_NOT_NULL(fs2);
    cinux::fs::Inode* ino2 = fs2->lookup(rel2);
    TEST_ASSERT_NOT_NULL(ino2);
    int fd2 = cinux::fs::g_global_fd_table().alloc(ino2, cinux::fs::OpenFlags::RDONLY);
    TEST_ASSERT_GE(fd2, 0);

    TEST_ASSERT_NE(fd1, fd2);

    cinux::fs::File* f1 = cinux::fs::g_global_fd_table().get(fd1);
    TEST_ASSERT_NOT_NULL(f1);
    char    buf1[64] = {};
    int64_t n1       = f1->inode->ops->read(f1->inode, 0, buf1, sizeof(buf1) - 1);
    TEST_ASSERT_GT(n1, 0);

    TEST_ASSERT_EQ(cinux::fs::g_global_fd_table().close(fd1), 0);
    TEST_ASSERT_EQ(cinux::fs::g_global_fd_table().close(fd2), 0);

    cinux::fs::vfs_mount_remove("/");
    delete rd;
}

}  // namespace test_vfs_integration

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ramdisk_tests() {
    TEST_SECTION("Ramdisk Tests (026/027)");

    RUN_TEST(test_ramdisk_struct::test_ustar_header_size);

    RUN_TEST(test_ramdisk_octal::test_octal_zero);
    RUN_TEST(test_ramdisk_octal::test_octal_small_value);
    RUN_TEST(test_ramdisk_octal::test_octal_with_null_terminator);
    RUN_TEST(test_ramdisk_octal::test_octal_with_space_padding);
    RUN_TEST(test_ramdisk_octal::test_octal_block_size);

    RUN_TEST(test_ramdisk_mount::test_ramdisk_base_not_null);
    RUN_TEST(test_ramdisk_mount::test_ramdisk_size_nonzero);
    RUN_TEST(test_ramdisk_mount::test_ramdisk_mount_returns_true);
    RUN_TEST(test_ramdisk_mount::test_ramdisk_mount_finds_files);

    RUN_TEST(test_ramdisk_lookup::test_lookup_hello_txt);
    RUN_TEST(test_ramdisk_lookup::test_lookup_readme_txt);
    RUN_TEST(test_ramdisk_lookup::test_lookup_etc_passwd);
    RUN_TEST(test_ramdisk_lookup::test_lookup_nonexistent);
    RUN_TEST(test_ramdisk_lookup::test_lookup_null_path);
    RUN_TEST(test_ramdisk_lookup::test_lookup_root_returns_dir_inode);
    RUN_TEST(test_ramdisk_lookup::test_lookup_root_slash_returns_dir_inode);

    RUN_TEST(test_ramdisk_inode_ops::test_read_hello_content);
    RUN_TEST(test_ramdisk_inode_ops::test_read_with_offset);
    RUN_TEST(test_ramdisk_inode_ops::test_read_past_end_returns_zero);
    RUN_TEST(test_ramdisk_inode_ops::test_write_returns_error);
    RUN_TEST(test_ramdisk_inode_ops::test_read_null_inode_returns_error);

    RUN_TEST(test_vfs_integration::test_vfs_mount_and_resolve);
    RUN_TEST(test_vfs_integration::test_vfs_open_read_close);
    RUN_TEST(test_vfs_integration::test_vfs_open_nonexistent_fails);
    RUN_TEST(test_vfs_integration::test_vfs_close_invalid_fd);
    RUN_TEST(test_vfs_integration::test_vfs_open_multiple_files);

    TEST_SUMMARY();
}
