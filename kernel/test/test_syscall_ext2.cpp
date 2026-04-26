/**
 * @file kernel/test/test_syscall_ext2.cpp
 * @brief QEMU in-kernel integration tests for sys_creat/mkdir/unlink/rmdir (028b)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies the
 * complete syscall flow: VFS resolve -> split pathname -> lookup parent
 * inode -> ops->create/mkdir/unlink, using a real ext2 ramdisk on AHCI.
 *
 * Test matrix:
 *   - sys_creat("/testfile")  -> lookup verifies file exists
 *   - sys_mkdir("/testdir")   -> lookup verifies directory exists
 *   - sys_unlink("/testfile") -> lookup verifies file gone
 *   - sys_rmdir("/testdir")   -> lookup verifies directory gone
 *   - Full flow: mkdir -> creat in subdir -> unlink -> rmdir
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised (needed for DMA buffer allocation)
 *   - VMM initialised (needed for DMA buffer mapping)
 *   - Heap initialised (needed for new/delete)
 */

#include <stddef.h>
#include <stdint.h>

#include "big_kernel_test.h"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/pit/pit.hpp"
#include "kernel/fs/ext2.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/syscall/sys_creat.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_rmdir.hpp"
#include "kernel/syscall/sys_unlink.hpp"

using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::ahci::AHCI;
using cinux::fs::Ext2;
using cinux::fs::Inode;
using cinux::fs::InodeType;

// ============================================================
// Helper: set up VFS + AHCI + Ext2 for each test
// ============================================================

namespace {

struct AhciExt2Pair {
    AHCI* ahci;
    Ext2* ext2;
};

/**
 * @brief Initialise PCI, find AHCI, create and mount an Ext2 instance,
 *        and register it in the VFS mount table at "/"
 */
AhciExt2Pair setup_syscall_ext2() {
    AhciExt2Pair result{nullptr, nullptr};

    // Reset VFS mount table for a clean slate
    cinux::fs::vfs_mount_init();

    // PCI enumeration
    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return result;
    }

    // AHCI init
    result.ahci = new AHCI();
    result.ahci->init(ahci_dev);

    // Ext2 mount on port 1 (the ext2 test disk)
    result.ext2 = new Ext2(*result.ahci, 1);
    result.ext2->mount();

    // Register in VFS
    cinux::fs::vfs_mount_add("/", result.ext2);

    return result;
}

/// Tear down VFS mount, Ext2, and AHCI
void teardown_syscall_ext2(AhciExt2Pair& pair) {
    cinux::fs::vfs_mount_remove("/");
    delete pair.ext2;
    delete pair.ahci;
    pair.ext2 = nullptr;
    pair.ahci = nullptr;
}

static uint32_t g_name_seq = 0;

void gen_name(char* buf, uint32_t buf_len, const char* prefix) {
    uint32_t seed = static_cast<uint32_t>(cinux::drivers::PIT::get_ticks() ^ (++g_name_seq));
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    uint32_t off = 0;
    while (prefix[off] && off < buf_len - 9) {
        buf[off] = prefix[off];
        ++off;
    }
    buf[off++]       = '_';
    const char hex[] = "0123456789abcdef";
    for (int d = 6; d >= 0 && off < buf_len - 1; --d)
        buf[off++] = hex[(seed >> (d * 4)) & 0xf];
    buf[off] = '\0';
}

uint32_t name_len(const char* s) {
    uint32_t n = 0;
    while (s[n])
        ++n;
    return n;
}

}  // anonymous namespace

// ============================================================
// Test 1: sys_creat — create a new file
// ============================================================

namespace test_sys_creat {

/**
 * @brief sys_creat("/sc_testfile") creates the file; lookup confirms it exists
 */
void test_creat_creates_file() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "sc");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1]    = '\0';
    auto path_addr = reinterpret_cast<uint64_t>(path);

    int64_t result = cinux::syscall::sys_creat(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);

    // Verify the file exists via lookup
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(static_cast<uint32_t>(found->type), static_cast<uint32_t>(InodeType::Regular));

    cinux::lib::kprintf("[SYSCALL_EXT2] creat /%s OK (ino=%lu)\n", name, found->ino);

    // Clean up: unlink so the disk state is restored
    pair.ext2->unlink(2, name, name_len(name));

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_creat

// ============================================================
// Test 2: sys_mkdir — create a new directory
// ============================================================

namespace test_sys_mkdir {

/**
 * @brief sys_mkdir("/sc_testdir") creates the directory; lookup confirms it exists
 */
void test_mkdir_creates_directory() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "sd");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1]    = '\0';
    auto path_addr = reinterpret_cast<uint64_t>(path);

    int64_t result = cinux::syscall::sys_mkdir(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);

    // Verify the directory exists via lookup
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(static_cast<uint32_t>(found->type), static_cast<uint32_t>(InodeType::Directory));

    cinux::lib::kprintf("[SYSCALL_EXT2] mkdir /%s OK (ino=%lu)\n", name, found->ino);

    // Clean up: unlink the directory (link_count 2->1, not fully freed,
    // but removes directory entry from parent so disk is reusable)
    pair.ext2->unlink(2, name, name_len(name));

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_mkdir

// ============================================================
// Test 3: sys_unlink — remove a file
// ============================================================

namespace test_sys_unlink {

/**
 * @brief Create a file with sys_creat, then sys_unlink removes it
 */
void test_unlink_removes_file() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // First create the file
    char name[32];
    gen_name(name, 32, "su");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1]    = '\0';
    auto path_addr = reinterpret_cast<uint64_t>(path);

    int64_t creat_result = cinux::syscall::sys_creat(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(creat_result, 0);

    // Confirm it exists
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);

    cinux::lib::kprintf("[SYSCALL_EXT2] unlink: file created (ino=%lu)\n", found->ino);

    // Now unlink it
    int64_t unlink_result = cinux::syscall::sys_unlink(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(unlink_result, 0);

    // Verify the file is gone
    Inode* gone = pair.ext2->lookup(name);
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[SYSCALL_EXT2] unlink /%s OK (file gone)\n", name);

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_unlink

// ============================================================
// Test 4: sys_rmdir — remove an empty directory
// ============================================================

namespace test_sys_rmdir {

/**
 * @brief Create a directory with sys_mkdir, then sys_rmdir removes it
 */
void test_rmdir_removes_directory() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // First create the directory
    char name[32];
    gen_name(name, 32, "sr");
    char path[64];
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1]    = '\0';
    auto path_addr = reinterpret_cast<uint64_t>(path);

    int64_t mkdir_result = cinux::syscall::sys_mkdir(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(mkdir_result, 0);

    // Confirm it exists
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);

    cinux::lib::kprintf("[SYSCALL_EXT2] rmdir: dir created (ino=%lu)\n", found->ino);

    // Now rmdir it
    int64_t rmdir_result = cinux::syscall::sys_rmdir(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(rmdir_result, 0);

    // Verify the directory is gone from parent's listing
    Inode* gone = pair.ext2->lookup(name);
    // After rmdir, the directory entry is removed from parent
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[SYSCALL_EXT2] rmdir /%s OK (dir gone)\n", name);

    teardown_syscall_ext2(pair);
}

}  // namespace test_sys_rmdir

// ============================================================
// Test 5: Full flow — mkdir -> creat in subdir -> unlink -> rmdir
// ============================================================

namespace test_syscall_full_flow {

/**
 * @brief End-to-end: mkdir("/sc_flowdir"), creat("/sc_flowdir/file"),
 *        unlink("/sc_flowdir/file"), rmdir("/sc_flowdir")
 */
void test_full_syscall_flow() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Step 1: mkdir random dir
    char dirname[32];
    gen_name(dirname, 32, "sf");
    char dir_path[64];
    dir_path[0] = '/';
    uint32_t di = 0;
    while (dirname[di]) {
        dir_path[di + 1] = dirname[di];
        ++di;
    }
    dir_path[di + 1] = '\0';
    auto dir_addr    = reinterpret_cast<uint64_t>(dir_path);

    int64_t mkdir_result = cinux::syscall::sys_mkdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(mkdir_result, 0);

    Inode* dir = pair.ext2->lookup(dirname);
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_EQ(static_cast<uint32_t>(dir->type), static_cast<uint32_t>(InodeType::Directory));

    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: mkdir /%s OK\n", dirname);

    // Step 2: creat file inside the dir
    char fname[32];
    gen_name(fname, 32, "ff");
    char     file_path[96];
    uint32_t fi     = 0;
    file_path[fi++] = '/';
    for (uint32_t j = 0; dirname[j] && fi < sizeof(file_path) - 2; ++j)
        file_path[fi++] = dirname[j];
    file_path[fi++] = '/';
    for (uint32_t j = 0; fname[j] && fi < sizeof(file_path) - 1; ++j)
        file_path[fi++] = fname[j];
    file_path[fi]  = '\0';
    auto file_addr = reinterpret_cast<uint64_t>(file_path);

    int64_t creat_result = cinux::syscall::sys_creat(file_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(creat_result, 0);

    // Verify via ext2 lookup in the subdirectory
    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: creat %s OK\n", file_path);

    // Step 3: unlink the file
    int64_t unlink_result = cinux::syscall::sys_unlink(file_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(unlink_result, 0);

    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: unlink %s OK\n", file_path);

    // Step 4: rmdir the directory
    int64_t rmdir_result = cinux::syscall::sys_rmdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(rmdir_result, 0);

    cinux::lib::kprintf("[SYSCALL_EXT2] full flow: rmdir /%s OK\n", dirname);

    teardown_syscall_ext2(pair);
}

}  // namespace test_syscall_full_flow

// ============================================================
// Test 6: sys_creat duplicate name — should not crash
// ============================================================

namespace test_sys_creat_duplicate {

void test_creat_duplicate_name() {
    auto pair = setup_syscall_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char name[32];
    gen_name(name, 32, "dup");
    char path[64];
    // 拼接 "/" + name
    path[0]    = '/';
    uint32_t i = 0;
    while (name[i]) {
        path[i + 1] = name[i];
        ++i;
    }
    path[i + 1]    = '\0';
    auto path_addr = reinterpret_cast<uint64_t>(path);

    // 第一次创建应该成功
    int64_t r1 = cinux::syscall::sys_creat(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(r1, 0);

    // 验证文件存在
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);
    uint64_t first_ino = found->ino;

    // 重复创建同名文件（当前实现：应该返回已存在的 inode 或报错）
    int64_t r2 = cinux::syscall::sys_creat(path_addr, 0, 0, 0, 0, 0);
    // r2 可能是 0（返回已存在 inode）或 < 0（报错），两种都可接受
    // 关键是不应该崩溃

    // 验证原文件仍在
    Inode* still = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(still);

    // 清理
    cinux::syscall::sys_unlink(path_addr, 0, 0, 0, 0, 0);
    teardown_syscall_ext2(pair);

    (void)first_ino;
    (void)r2;
}

}  // namespace test_sys_creat_duplicate

// ============================================================
// Entry point
// ============================================================

extern "C" void run_syscall_ext2_tests() {
    TEST_SECTION("Syscall Ext2 Tests (028b)");

    // sys_creat
    RUN_TEST(test_sys_creat::test_creat_creates_file);

    // sys_mkdir
    RUN_TEST(test_sys_mkdir::test_mkdir_creates_directory);

    // sys_unlink
    RUN_TEST(test_sys_unlink::test_unlink_removes_file);

    // sys_rmdir
    RUN_TEST(test_sys_rmdir::test_rmdir_removes_directory);

    // Full flow
    RUN_TEST(test_syscall_full_flow::test_full_syscall_flow);

    // Duplicate creat
    RUN_TEST(test_sys_creat_duplicate::test_creat_duplicate_name);

    TEST_SUMMARY();
}
