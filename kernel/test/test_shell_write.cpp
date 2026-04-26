/**
 * @file kernel/test/test_shell_write.cpp
 * @brief QEMU in-kernel integration tests for shell write commands (028b)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * full syscall flow that shell write commands depend on:
 *   - sys_creat + lookup verify + sys_unlink cleanup (touch flow)
 *   - sys_mkdir + lookup verify + sys_rmdir cleanup (mkdir flow)
 *   - sys_creat + sys_open + sys_write + sys_read verify + sys_unlink (echo redirect)
 *   - Full flow: mkdir -> creat in dir -> write -> read -> unlink -> rmdir
 *
 * Uses random naming (PIT ticks + counter) to ensure test idempotency.
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM, VMM, Heap, AddressSpace initialised
 *   - usermode_init() and syscall_init() called
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
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_creat.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_rmdir.hpp"
#include "kernel/syscall/sys_unlink.hpp"
#include "kernel/syscall/sys_write.hpp"

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

AhciExt2Pair setup_shell_write() {
    AhciExt2Pair result{nullptr, nullptr};

    cinux::fs::vfs_mount_init();

    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return result;
    }

    result.ahci = new AHCI();
    result.ahci->init(ahci_dev);

    result.ext2 = new Ext2(*result.ahci, 1);
    result.ext2->mount();

    cinux::fs::vfs_mount_add("/", result.ext2);

    return result;
}

void teardown_shell_write(AhciExt2Pair& pair) {
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


/// Build "/name" path in buf from a bare name
void make_root_path(char* buf, uint32_t buf_len, const char* name) {
    uint32_t i = 0;
    buf[i++]   = '/';
    for (uint32_t j = 0; name[j] && i < buf_len - 1; ++j)
        buf[i++] = name[j];
    buf[i] = '\0';
}

/// Build "/dir/file" path in buf from dir name and file name
void make_sub_path(char* buf, uint32_t buf_len, const char* dirname, const char* fname) {
    uint32_t i = 0;
    buf[i++]   = '/';
    for (uint32_t j = 0; dirname[j] && i < buf_len - 2; ++j)
        buf[i++] = dirname[j];
    buf[i++] = '/';
    for (uint32_t j = 0; fname[j] && i < buf_len - 1; ++j)
        buf[i++] = fname[j];
    buf[i] = '\0';
}

}  // anonymous namespace

// ============================================================
// Test 1: touch flow — sys_creat -> lookup -> sys_unlink
// ============================================================

namespace test_touch_flow {

void test_touch_creates_and_cleanup() {
    auto pair = setup_shell_write();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "tw");
    char path[64];
    make_root_path(path, 64, name);
    auto path_addr = reinterpret_cast<uint64_t>(path);

    // sys_creat (touch)
    int64_t creat_result = cinux::syscall::sys_creat(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(creat_result, 0);

    // lookup verifies file exists
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(static_cast<uint32_t>(found->type), static_cast<uint32_t>(InodeType::Regular));

    cinux::lib::kprintf("[SHELL_WRITE] touch /%s OK (ino=%lu)\n", name, found->ino);

    // sys_unlink cleanup
    int64_t unlink_result = cinux::syscall::sys_unlink(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(unlink_result, 0);

    Inode* gone = pair.ext2->lookup(name);
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[SHELL_WRITE] touch cleanup /%s OK\n", name);

    teardown_shell_write(pair);
}

}  // namespace test_touch_flow

// ============================================================
// Test 2: mkdir flow — sys_mkdir -> lookup -> sys_rmdir
// ============================================================

namespace test_mkdir_flow {

void test_mkdir_creates_and_cleanup() {
    auto pair = setup_shell_write();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "mw");
    char path[64];
    make_root_path(path, 64, name);
    auto path_addr = reinterpret_cast<uint64_t>(path);

    // sys_mkdir
    int64_t mkdir_result = cinux::syscall::sys_mkdir(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(mkdir_result, 0);

    // lookup verifies directory exists
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(static_cast<uint32_t>(found->type), static_cast<uint32_t>(InodeType::Directory));

    cinux::lib::kprintf("[SHELL_WRITE] mkdir /%s OK (ino=%lu)\n", name, found->ino);

    // sys_rmdir cleanup
    int64_t rmdir_result = cinux::syscall::sys_rmdir(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(rmdir_result, 0);

    Inode* gone = pair.ext2->lookup(name);
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[SHELL_WRITE] mkdir cleanup /%s OK\n", name);

    teardown_shell_write(pair);
}

}  // namespace test_mkdir_flow

// ============================================================
// Test 3: echo redirect flow — creat -> open -> write -> read -> unlink
// ============================================================

namespace test_echo_redirect_flow {

void test_echo_redirect_write_read() {
    auto pair = setup_shell_write();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "ew");
    char path[64];
    make_root_path(path, 64, name);
    auto path_addr = reinterpret_cast<uint64_t>(path);

    // sys_creat (echo redirect creates the file)
    int64_t creat_result = cinux::syscall::sys_creat(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(creat_result, 0);

    cinux::lib::kprintf("[SHELL_WRITE] echo redirect: creat /%s OK\n", name);

    // sys_open for writing (O_WRONLY = 1)
    int64_t fd = cinux::syscall::sys_open(path_addr, 1, 0, 0, 0, 0);
    TEST_ASSERT_GE(fd, 0);

    cinux::lib::kprintf("[SHELL_WRITE] echo redirect: open fd=%ld\n", fd);

    // sys_write text to file
    const char text[]    = "hello world\n";
    auto       text_addr = reinterpret_cast<uint64_t>(text);
    int64_t    write_result =
        cinux::syscall::sys_write(static_cast<uint64_t>(fd), text_addr, sizeof(text) - 1, 0, 0, 0);
    TEST_ASSERT_EQ(write_result, static_cast<int64_t>(sizeof(text) - 1));

    cinux::lib::kprintf("[SHELL_WRITE] echo redirect: wrote %ld bytes\n", write_result);

    // sys_close
    int64_t close_result = cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(close_result, 0);

    // sys_open for reading (O_RDONLY = 0)
    int64_t rfd = cinux::syscall::sys_open(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_GE(rfd, 0);

    // sys_read back the data
    char read_buf[64];
    for (uint32_t i = 0; i < sizeof(read_buf); ++i)
        read_buf[i] = 0;
    auto    buf_addr = reinterpret_cast<uint64_t>(read_buf);
    int64_t nread    = cinux::syscall::sys_read(static_cast<uint64_t>(rfd), buf_addr,
                                                sizeof(read_buf) - 1, 0, 0, 0);
    TEST_ASSERT_EQ(nread, static_cast<int64_t>(sizeof(text) - 1));

    // Verify content
    TEST_ASSERT_EQ(memcmp(read_buf, text, sizeof(text) - 1), 0);

    cinux::lib::kprintf("[SHELL_WRITE] echo redirect: read back verified (%ld bytes)\n", nread);

    // sys_close read fd
    cinux::syscall::sys_close(static_cast<uint64_t>(rfd), 0, 0, 0, 0, 0);

    // sys_unlink cleanup
    int64_t unlink_result = cinux::syscall::sys_unlink(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(unlink_result, 0);

    cinux::lib::kprintf("[SHELL_WRITE] echo redirect: cleanup /%s OK\n", name);

    teardown_shell_write(pair);
}

}  // namespace test_echo_redirect_flow

// ============================================================
// Test 4: Full flow — mkdir -> creat in dir -> write -> read -> unlink -> rmdir
// ============================================================

namespace test_full_flow {

void test_full_shell_write_flow() {
    auto pair = setup_shell_write();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Step 1: mkdir
    char dirname[32];
    gen_name(dirname, 32, "fw");
    char dir_path[64];
    make_root_path(dir_path, 64, dirname);
    auto dir_addr = reinterpret_cast<uint64_t>(dir_path);

    int64_t mkdir_result = cinux::syscall::sys_mkdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(mkdir_result, 0);

    Inode* dir = pair.ext2->lookup(dirname);
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_EQ(static_cast<uint32_t>(dir->type), static_cast<uint32_t>(InodeType::Directory));

    cinux::lib::kprintf("[SHELL_WRITE] full flow: mkdir /%s OK\n", dirname);

    // Step 2: creat file in dir
    char fname[32];
    gen_name(fname, 32, "fwf");
    char file_path[96];
    make_sub_path(file_path, 96, dirname, fname);
    auto file_addr = reinterpret_cast<uint64_t>(file_path);

    int64_t creat_result = cinux::syscall::sys_creat(file_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(creat_result, 0);

    cinux::lib::kprintf("[SHELL_WRITE] full flow: creat %s OK\n", file_path);

    // Step 3: open for writing, write text
    int64_t fd = cinux::syscall::sys_open(file_addr, 1, 0, 0, 0, 0);
    TEST_ASSERT_GE(fd, 0);

    const char text[]    = "shell write test\n";
    auto       text_addr = reinterpret_cast<uint64_t>(text);
    int64_t    write_result =
        cinux::syscall::sys_write(static_cast<uint64_t>(fd), text_addr, sizeof(text) - 1, 0, 0, 0);
    TEST_ASSERT_EQ(write_result, static_cast<int64_t>(sizeof(text) - 1));

    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    cinux::lib::kprintf("[SHELL_WRITE] full flow: write %ld bytes OK\n", write_result);

    // Step 4: open for reading, read back and verify
    int64_t rfd = cinux::syscall::sys_open(file_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_GE(rfd, 0);

    char read_buf[64];
    for (uint32_t i = 0; i < sizeof(read_buf); ++i)
        read_buf[i] = 0;
    auto    buf_addr = reinterpret_cast<uint64_t>(read_buf);
    int64_t nread    = cinux::syscall::sys_read(static_cast<uint64_t>(rfd), buf_addr,
                                                sizeof(read_buf) - 1, 0, 0, 0);
    TEST_ASSERT_EQ(nread, static_cast<int64_t>(sizeof(text) - 1));
    TEST_ASSERT_EQ(memcmp(read_buf, text, sizeof(text) - 1), 0);

    cinux::syscall::sys_close(static_cast<uint64_t>(rfd), 0, 0, 0, 0, 0);

    cinux::lib::kprintf("[SHELL_WRITE] full flow: read verified OK\n");

    // Step 5: unlink the file
    int64_t unlink_result = cinux::syscall::sys_unlink(file_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(unlink_result, 0);

    cinux::lib::kprintf("[SHELL_WRITE] full flow: unlink %s OK\n", file_path);

    // Step 6: rmdir the directory
    int64_t rmdir_result = cinux::syscall::sys_rmdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(rmdir_result, 0);

    cinux::lib::kprintf("[SHELL_WRITE] full flow: rmdir /%s OK\n", dirname);

    teardown_shell_write(pair);
}

}  // namespace test_full_flow

// ============================================================
// Entry point
// ============================================================

extern "C" void run_shell_write_tests() {
    TEST_SECTION("Shell Write Tests (028b)");

    // touch flow
    RUN_TEST(test_touch_flow::test_touch_creates_and_cleanup);

    // mkdir flow
    RUN_TEST(test_mkdir_flow::test_mkdir_creates_and_cleanup);

    // echo redirect flow
    RUN_TEST(test_echo_redirect_flow::test_echo_redirect_write_read);

    // full flow
    RUN_TEST(test_full_flow::test_full_shell_write_flow);

    TEST_SUMMARY();
}
