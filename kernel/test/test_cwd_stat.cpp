/**
 * @file kernel/test/test_cwd_stat.cpp
 * @brief QEMU in-kernel integration tests for cwd/stat syscalls (028c)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Exercises the
 * complete syscall flow for per-process working directory and stat:
 *
 *   - CWD initial value (new process cwd = "/")
 *   - path_canonicalize: . .. // collapsing
 *   - sys_chdir: change to existing dir, reject non-dir, reject ENOENT
 *   - sys_getcwd: retrieve current cwd after chdir
 *   - sys_stat: stat root dir, stat file by path, stat nonexistent
 *   - sys_fstat: fstat via fd, verify matches stat result
 *   - Shell commands: cd, pwd, stat (via mock dispatch)
 *
 * Preconditions (set up by main_test.cpp before this runs):
 *   - Serial port initialised (kprintf works)
 *   - GDT and IDT loaded
 *   - PMM initialised (needed for DMA buffer allocation)
 *   - VMM initialised (needed for DMA buffer mapping)
 *   - Heap initialised (needed for new/delete)
 *   - usermode_init() and syscall_init() called
 */

#include <stdint.h>
#include <stddef.h>

#include "big_kernel_test.h"

#include "kernel/drivers/pit/pit.hpp"
#include "kernel/drivers/pci/pci.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/fs/ext2.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/fs/path.hpp"
#include "kernel/fs/stat.hpp"
#include "kernel/syscall/sys_chdir.hpp"
#include "kernel/syscall/sys_getcwd.hpp"
#include "kernel/syscall/sys_stat.hpp"
#include "kernel/syscall/sys_creat.hpp"
#include "kernel/syscall/sys_mkdir.hpp"
#include "kernel/syscall/sys_open.hpp"
#include "kernel/syscall/sys_write.hpp"
#include "kernel/syscall/sys_read.hpp"
#include "kernel/syscall/sys_close.hpp"
#include "kernel/syscall/sys_unlink.hpp"
#include "kernel/syscall/sys_rmdir.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/lib/string.hpp"

using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::ahci::AHCI;
using cinux::fs::Ext2;
using cinux::fs::Inode;
using cinux::fs::InodeType;

// ============================================================
// Freestanding helpers
// ============================================================

namespace {

/// Copy a string (like strcpy, but using the kernel's memcpy)
void k_strcpy(char* dst, const char* src) {
    size_t len = strlen(src);
    memcpy(dst, src, len + 1);
}

}  // anonymous namespace

// ============================================================
// Helper: set up VFS + AHCI + Ext2 for each test
// ============================================================

namespace {

struct AhciExt2Pair {
    AHCI* ahci;
    Ext2* ext2;
};

AhciExt2Pair setup_cwd_stat() {
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

void teardown_cwd_stat(AhciExt2Pair& pair) {
    cinux::fs::vfs_mount_remove("/");
    delete pair.ext2;
    delete pair.ahci;
    pair.ext2 = nullptr;
    pair.ahci = nullptr;
}

static uint32_t g_name_seq = 0;

void gen_name(char* buf, uint32_t buf_len, const char* prefix) {
    uint32_t seed = static_cast<uint32_t>(
        cinux::drivers::PIT::get_ticks() ^ (++g_name_seq));
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    uint32_t off = 0;
    while (prefix[off] && off < buf_len - 9) { buf[off] = prefix[off]; ++off; }
    buf[off++] = '_';
    const char hex[] = "0123456789abcdef";
    for (int d = 6; d >= 0 && off < buf_len - 1; --d)
        buf[off++] = hex[(seed >> (d * 4)) & 0xf];
    buf[off] = '\0';
}

void make_root_path(char* buf, uint32_t buf_len, const char* name) {
    uint32_t i = 0;
    buf[i++] = '/';
    for (uint32_t j = 0; name[j] && i < buf_len - 1; ++j)
        buf[i++] = name[j];
    buf[i] = '\0';
}

void make_sub_path(char* buf, uint32_t buf_len,
                   const char* dirname, const char* fname) {
    uint32_t i = 0;
    buf[i++] = '/';
    for (uint32_t j = 0; dirname[j] && i < buf_len - 2; ++j)
        buf[i++] = dirname[j];
    buf[i++] = '/';
    for (uint32_t j = 0; fname[j] && i < buf_len - 1; ++j)
        buf[i++] = fname[j];
    buf[i] = '\0';
}

/// Helper: reset the current task's cwd to "/"
void reset_cwd() {
    cinux::proc::Task* cur = cinux::proc::Scheduler::current();
    if (cur != nullptr) {
        cur->cwd[0] = '/';
        cur->cwd[1] = '\0';
    }
}

}  // anonymous namespace

// ============================================================
// Test 1: CWD initial value
// ============================================================

namespace test_cwd_init {

void test_cwd_initial_is_root() {
    // The scheduler's current task should have cwd = "/"
    cinux::proc::Task* cur = cinux::proc::Scheduler::current();
    TEST_ASSERT_NOT_NULL(cur);
    TEST_ASSERT_TRUE(strcmp(cur->cwd, "/") == 0);
}

}  // namespace test_cwd_init

// ============================================================
// Test 2: path_canonicalize (kernel implementation)
// ============================================================

namespace test_path_canonicalize {

void test_dot_dot_collapses() {
    char buf[cinux::fs::PATH_MAX];
    k_strcpy(buf, "/a/b/../c");
    cinux::fs::path_canonicalize(buf);
    TEST_ASSERT_TRUE(strcmp(buf, "/a/c") == 0);
}

void test_dot_is_removed() {
    char buf[cinux::fs::PATH_MAX];
    k_strcpy(buf, "/a/./b/./c");
    cinux::fs::path_canonicalize(buf);
    TEST_ASSERT_TRUE(strcmp(buf, "/a/b/c") == 0);
}

void test_duplicate_slashes_collapsed() {
    char buf[cinux::fs::PATH_MAX];
    k_strcpy(buf, "/a//b///c");
    cinux::fs::path_canonicalize(buf);
    TEST_ASSERT_TRUE(strcmp(buf, "/a/b/c") == 0);
}

void test_trailing_slash_removed() {
    char buf[cinux::fs::PATH_MAX];
    k_strcpy(buf, "/a/b/");
    cinux::fs::path_canonicalize(buf);
    TEST_ASSERT_TRUE(strcmp(buf, "/a/b") == 0);
}

void test_root_stays_root() {
    char buf[cinux::fs::PATH_MAX];
    k_strcpy(buf, "/");
    cinux::fs::path_canonicalize(buf);
    TEST_ASSERT_TRUE(strcmp(buf, "/") == 0);
}

void test_all_dot_components() {
    char buf[cinux::fs::PATH_MAX];
    k_strcpy(buf, "/././.");
    cinux::fs::path_canonicalize(buf);
    TEST_ASSERT_TRUE(strcmp(buf, "/") == 0);
}

void test_dotdot_at_root() {
    char buf[cinux::fs::PATH_MAX];
    k_strcpy(buf, "/../../a");
    cinux::fs::path_canonicalize(buf);
    TEST_ASSERT_TRUE(strcmp(buf, "/a") == 0);
}

}  // namespace test_path_canonicalize

// ============================================================
// Test 3: path_resolve (kernel implementation)
// ============================================================

namespace test_path_resolve {

void test_absolute_passthrough() {
    char out[cinux::fs::PATH_MAX];
    bool ok = cinux::fs::path_resolve("/", "/usr/bin", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(strcmp(out, "/usr/bin") == 0);
}

void test_relative_join() {
    char out[cinux::fs::PATH_MAX];
    bool ok = cinux::fs::path_resolve("/home/user", "docs", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(strcmp(out, "/home/user/docs") == 0);
}

void test_dotdot_goes_up() {
    char out[cinux::fs::PATH_MAX];
    bool ok = cinux::fs::path_resolve("/home/user", "..", out);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE(strcmp(out, "/home") == 0);
}

void test_null_args_fail() {
    char out[cinux::fs::PATH_MAX];
    TEST_ASSERT_TRUE(!cinux::fs::path_resolve(nullptr, "a", out));
    TEST_ASSERT_TRUE(!cinux::fs::path_resolve("/", nullptr, out));
    TEST_ASSERT_TRUE(!cinux::fs::path_resolve("/", "a", nullptr));
}

}  // namespace test_path_resolve

// ============================================================
// Test 4: sys_chdir
// ============================================================

namespace test_sys_chdir {

void test_chdir_to_existing_dir() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create a directory to chdir into
    char dirname[32]; gen_name(dirname, 32, "cd");
    char dir_path[64]; make_root_path(dir_path, 64, dirname);
    auto dir_addr = reinterpret_cast<uint64_t>(dir_path);

    int64_t mkdir_result = cinux::syscall::sys_mkdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(mkdir_result, 0);

    // chdir to the new directory
    int64_t chdir_result = cinux::syscall::sys_chdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(chdir_result, 0);

    // Verify cwd updated
    cinux::proc::Task* cur = cinux::proc::Scheduler::current();
    TEST_ASSERT_NOT_NULL(cur);
    TEST_ASSERT_TRUE(strcmp(cur->cwd, dir_path) == 0);

    cinux::lib::kprintf("[CWD_STAT] chdir to %s OK\n", dir_path);

    // Cleanup
    reset_cwd();
    cinux::syscall::sys_rmdir(dir_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_chdir_then_getcwd() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char dirname[32]; gen_name(dirname, 32, "gc");
    char dir_path[64]; make_root_path(dir_path, 64, dirname);
    auto dir_addr = reinterpret_cast<uint64_t>(dir_path);

    cinux::syscall::sys_mkdir(dir_addr, 0, 0, 0, 0, 0);

    int64_t chdir_result = cinux::syscall::sys_chdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(chdir_result, 0);

    // getcwd should return the new path
    char cwd_buf[256];
    for (uint32_t i = 0; i < sizeof(cwd_buf); ++i) cwd_buf[i] = 0;
    auto buf_addr = reinterpret_cast<uint64_t>(cwd_buf);

    int64_t n = cinux::syscall::sys_getcwd(buf_addr, sizeof(cwd_buf), 0, 0, 0, 0);
    TEST_ASSERT_GT(n, 0);
    TEST_ASSERT_TRUE(strcmp(cwd_buf, dir_path) == 0);

    cinux::lib::kprintf("[CWD_STAT] getcwd after chdir = %s\n", cwd_buf);

    reset_cwd();
    cinux::syscall::sys_rmdir(dir_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_chdir_nonexistent_fails() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char path[] = "/no_such_dir_abcdef";
    auto path_addr = reinterpret_cast<uint64_t>(path);

    int64_t result = cinux::syscall::sys_chdir(path_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, -1);

    // cwd should still be "/"
    cinux::proc::Task* cur = cinux::proc::Scheduler::current();
    TEST_ASSERT_NOT_NULL(cur);
    TEST_ASSERT_TRUE(strcmp(cur->cwd, "/") == 0);

    teardown_cwd_stat(pair);
}

void test_chdir_file_fails() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create a regular file
    char fname[32]; gen_name(fname, 32, "cf");
    char fpath[64]; make_root_path(fpath, 64, fname);
    auto fpath_addr = reinterpret_cast<uint64_t>(fpath);

    int64_t creat_result = cinux::syscall::sys_creat(fpath_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(creat_result, 0);

    // chdir to a regular file should fail
    int64_t chdir_result = cinux::syscall::sys_chdir(fpath_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(chdir_result, -1);

    // cwd should still be "/"
    cinux::proc::Task* cur = cinux::proc::Scheduler::current();
    TEST_ASSERT_NOT_NULL(cur);
    TEST_ASSERT_TRUE(strcmp(cur->cwd, "/") == 0);

    // Cleanup
    cinux::syscall::sys_unlink(fpath_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_consecutive_chdir() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create /cd1/cd2
    char d1[32]; gen_name(d1, 32, "c1");
    char d2[32]; gen_name(d2, 32, "c2");
    char p1[64]; make_root_path(p1, 64, d1);
    char p2[96]; make_sub_path(p2, 96, d1, d2);

    auto p1_addr = reinterpret_cast<uint64_t>(p1);
    auto p2_addr = reinterpret_cast<uint64_t>(p2);

    cinux::syscall::sys_mkdir(p1_addr, 0, 0, 0, 0, 0);
    cinux::syscall::sys_mkdir(p2_addr, 0, 0, 0, 0, 0);

    // chdir to d1
    int64_t r1 = cinux::syscall::sys_chdir(p1_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(r1, 0);

    // chdir to d2 using relative path
    auto d2_rel = reinterpret_cast<uint64_t>(d2);
    int64_t r2 = cinux::syscall::sys_chdir(d2_rel, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(r2, 0);

    // Verify cwd is /d1/d2
    cinux::proc::Task* cur = cinux::proc::Scheduler::current();
    TEST_ASSERT_NOT_NULL(cur);
    TEST_ASSERT_TRUE(strcmp(cur->cwd, p2) == 0);

    cinux::lib::kprintf("[CWD_STAT] consecutive chdir -> %s OK\n", cur->cwd);

    // Cleanup
    reset_cwd();
    cinux::syscall::sys_rmdir(p2_addr, 0, 0, 0, 0, 0);
    cinux::syscall::sys_rmdir(p1_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

}  // namespace test_sys_chdir

// ============================================================
// Test 5: sys_stat
// ============================================================

namespace test_sys_stat {

void test_stat_root_directory() {
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char path[] = "/";
    auto path_addr = reinterpret_cast<uint64_t>(path);

    cinux::fs::stat st;
    for (uint32_t i = 0; i < sizeof(st); ++i)
        reinterpret_cast<uint8_t*>(&st)[i] = 0xAA;

    auto st_addr = reinterpret_cast<uint64_t>(&st);
    int64_t result = cinux::syscall::sys_stat(path_addr, st_addr, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);

    // Root inode number should be 2 for ext2
    TEST_ASSERT_EQ(st.st_ino, 2ULL);

    cinux::lib::kprintf("[CWD_STAT] stat / -> ino=%lu size=%ld\n",
                        st.st_ino, st.st_size);

    teardown_cwd_stat(pair);
}

void test_stat_file_by_path() {
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create a file and write data
    char fname[32]; gen_name(fname, 32, "sf");
    char fpath[64]; make_root_path(fpath, 64, fname);
    auto fpath_addr = reinterpret_cast<uint64_t>(fpath);

    int64_t creat_result = cinux::syscall::sys_creat(fpath_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(creat_result, 0);

    // Write some data
    int64_t fd = cinux::syscall::sys_open(fpath_addr, 1, 0, 0, 0, 0);
    TEST_ASSERT_GE(fd, 0);

    const char data[] = "stat test data\n";
    auto data_addr = reinterpret_cast<uint64_t>(data);
    cinux::syscall::sys_write(static_cast<uint64_t>(fd), data_addr,
                              sizeof(data) - 1, 0, 0, 0);
    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    // stat the file
    cinux::fs::stat st;
    for (uint32_t i = 0; i < sizeof(st); ++i)
        reinterpret_cast<uint8_t*>(&st)[i] = 0;
    auto st_addr = reinterpret_cast<uint64_t>(&st);

    int64_t result = cinux::syscall::sys_stat(fpath_addr, st_addr, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);

    // Verify stat fields
    TEST_ASSERT_GT(st.st_ino, 0ULL);
    TEST_ASSERT_GT(st.st_size, 0);

    cinux::lib::kprintf("[CWD_STAT] stat %s -> ino=%lu size=%ld\n",
                        fpath, st.st_ino, st.st_size);

    // Cleanup
    cinux::syscall::sys_unlink(fpath_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_stat_nonexistent_fails() {
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    char path[] = "/no_such_file_for_stat";
    auto path_addr = reinterpret_cast<uint64_t>(path);

    cinux::fs::stat st;
    auto st_addr = reinterpret_cast<uint64_t>(&st);

    int64_t result = cinux::syscall::sys_stat(path_addr, st_addr, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, -1);

    teardown_cwd_stat(pair);
}

void test_stat_relative_path_after_chdir() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create /st_dir/st_file
    char dname[32]; gen_name(dname, 32, "sd");
    char fname[32]; gen_name(fname, 32, "se");
    char dpath[64]; make_root_path(dpath, 64, dname);
    char fpath[96]; make_sub_path(fpath, 96, dname, fname);

    auto dpath_addr = reinterpret_cast<uint64_t>(dpath);
    auto fpath_addr = reinterpret_cast<uint64_t>(fpath);

    cinux::syscall::sys_mkdir(dpath_addr, 0, 0, 0, 0, 0);
    cinux::syscall::sys_creat(fpath_addr, 0, 0, 0, 0, 0);

    // chdir to the directory
    cinux::syscall::sys_chdir(dpath_addr, 0, 0, 0, 0, 0);

    // stat the file using relative path
    cinux::fs::stat st;
    for (uint32_t i = 0; i < sizeof(st); ++i)
        reinterpret_cast<uint8_t*>(&st)[i] = 0;
    auto st_addr = reinterpret_cast<uint64_t>(&st);
    auto fname_addr = reinterpret_cast<uint64_t>(fname);

    int64_t result = cinux::syscall::sys_stat(fname_addr, st_addr, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);
    TEST_ASSERT_GT(st.st_ino, 0ULL);

    cinux::lib::kprintf("[CWD_STAT] stat relative '%s' -> ino=%lu OK\n",
                        fname, st.st_ino);

    // Cleanup
    reset_cwd();
    cinux::syscall::sys_unlink(fpath_addr, 0, 0, 0, 0, 0);
    cinux::syscall::sys_rmdir(dpath_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

}  // namespace test_sys_stat

// ============================================================
// Test 6: sys_fstat
// ============================================================

namespace test_sys_fstat {

void test_fstat_open_file() {
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create and open a file
    char fname[32]; gen_name(fname, 32, "ff");
    char fpath[64]; make_root_path(fpath, 64, fname);
    auto fpath_addr = reinterpret_cast<uint64_t>(fpath);

    cinux::syscall::sys_creat(fpath_addr, 0, 0, 0, 0, 0);

    // Write data
    int64_t fd = cinux::syscall::sys_open(fpath_addr, 1, 0, 0, 0, 0);
    TEST_ASSERT_GE(fd, 0);

    const char data[] = "fstat test data\n";
    auto data_addr = reinterpret_cast<uint64_t>(data);
    cinux::syscall::sys_write(static_cast<uint64_t>(fd), data_addr,
                              sizeof(data) - 1, 0, 0, 0);

    // Close and reopen for clean offset
    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    // fstat via fd
    int64_t fd2 = cinux::syscall::sys_open(fpath_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_GE(fd2, 0);

    cinux::fs::stat st_fstat;
    for (uint32_t i = 0; i < sizeof(st_fstat); ++i)
        reinterpret_cast<uint8_t*>(&st_fstat)[i] = 0;
    auto st_addr = reinterpret_cast<uint64_t>(&st_fstat);

    int64_t result = cinux::syscall::sys_fstat(
        static_cast<uint64_t>(fd2), st_addr, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);
    TEST_ASSERT_GT(st_fstat.st_ino, 0ULL);

    cinux::lib::kprintf("[CWD_STAT] fstat fd=%ld -> ino=%lu size=%ld\n",
                        fd2, st_fstat.st_ino, st_fstat.st_size);

    // Cleanup
    cinux::syscall::sys_close(static_cast<uint64_t>(fd2), 0, 0, 0, 0, 0);
    cinux::syscall::sys_unlink(fpath_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_fstat_matches_stat() {
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create a file and write data
    char fname[32]; gen_name(fname, 32, "fm");
    char fpath[64]; make_root_path(fpath, 64, fname);
    auto fpath_addr = reinterpret_cast<uint64_t>(fpath);

    cinux::syscall::sys_creat(fpath_addr, 0, 0, 0, 0, 0);

    int64_t fd = cinux::syscall::sys_open(fpath_addr, 1, 0, 0, 0, 0);
    TEST_ASSERT_GE(fd, 0);

    const char data[] = "fstat stat consistency\n";
    auto data_addr = reinterpret_cast<uint64_t>(data);
    cinux::syscall::sys_write(static_cast<uint64_t>(fd), data_addr,
                              sizeof(data) - 1, 0, 0, 0);
    cinux::syscall::sys_close(static_cast<uint64_t>(fd), 0, 0, 0, 0, 0);

    // stat by path
    cinux::fs::stat st_path;
    for (uint32_t i = 0; i < sizeof(st_path); ++i)
        reinterpret_cast<uint8_t*>(&st_path)[i] = 0;
    cinux::syscall::sys_stat(fpath_addr,
                             reinterpret_cast<uint64_t>(&st_path),
                             0, 0, 0, 0);

    // fstat by fd
    int64_t fd2 = cinux::syscall::sys_open(fpath_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_GE(fd2, 0);

    cinux::fs::stat st_fd;
    for (uint32_t i = 0; i < sizeof(st_fd); ++i)
        reinterpret_cast<uint8_t*>(&st_fd)[i] = 0;
    cinux::syscall::sys_fstat(static_cast<uint64_t>(fd2),
                              reinterpret_cast<uint64_t>(&st_fd),
                              0, 0, 0, 0);

    // Compare: inode number and size should match
    TEST_ASSERT_EQ(st_path.st_ino, st_fd.st_ino);
    TEST_ASSERT_EQ(st_path.st_size, st_fd.st_size);

    cinux::lib::kprintf("[CWD_STAT] stat vs fstat: ino=%lu/%lu size=%ld/%ld OK\n",
                        st_path.st_ino, st_fd.st_ino,
                        st_path.st_size, st_fd.st_size);

    // Cleanup
    cinux::syscall::sys_close(static_cast<uint64_t>(fd2), 0, 0, 0, 0, 0);
    cinux::syscall::sys_unlink(fpath_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_fstat_bad_fd_fails() {
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    cinux::fs::stat st;
    auto st_addr = reinterpret_cast<uint64_t>(&st);

    // fd 99 should not be open
    int64_t result = cinux::syscall::sys_fstat(99, st_addr, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, -1);

    teardown_cwd_stat(pair);
}

}  // namespace test_sys_fstat

// ============================================================
// Test 7: Shell command tests (cd / pwd / stat dispatch)
// ============================================================

namespace test_shell_cwd_stat {

// Minimal string utilities
size_t k_strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

int k_strcmp(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return static_cast<int>(*a) - static_cast<int>(*b);
        ++a; ++b;
    }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

// Tokenizer (same as test_shell.cpp)
constexpr size_t MAX_TOKENS = 16;

size_t tokenize(char* line, char** argv, size_t max_tokens) {
    size_t argc = 0;
    while (*line != '\0' && argc < max_tokens) {
        while (*line == ' ' || *line == '\t') ++line;
        if (*line == '\0') break;
        argv[argc++] = line;
        while (*line != '\0' && *line != ' ' && *line != '\t') ++line;
        if (*line != '\0') *line++ = '\0';
    }
    return argc;
}

void test_cd_command_changes_cwd() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create a directory
    char dirname[32]; gen_name(dirname, 32, "sc");
    char dir_path[64]; make_root_path(dir_path, 64, dirname);
    auto dir_addr = reinterpret_cast<uint64_t>(dir_path);

    cinux::syscall::sys_mkdir(dir_addr, 0, 0, 0, 0, 0);

    // Simulate "cd /dirname" via syscall
    int64_t chdir_result = cinux::syscall::sys_chdir(dir_addr, 0, 0, 0, 0, 0);
    TEST_ASSERT_EQ(chdir_result, 0);

    cinux::proc::Task* cur = cinux::proc::Scheduler::current();
    TEST_ASSERT_NOT_NULL(cur);
    TEST_ASSERT_TRUE(k_strcmp(cur->cwd, dir_path) == 0);

    cinux::lib::kprintf("[CWD_STAT] shell cd %s OK\n", dir_path);

    reset_cwd();
    cinux::syscall::sys_rmdir(dir_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_pwd_command_outputs_cwd() {
    reset_cwd();
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create a directory and chdir
    char dirname[32]; gen_name(dirname, 32, "sp");
    char dir_path[64]; make_root_path(dir_path, 64, dirname);
    auto dir_addr = reinterpret_cast<uint64_t>(dir_path);

    cinux::syscall::sys_mkdir(dir_addr, 0, 0, 0, 0, 0);
    cinux::syscall::sys_chdir(dir_addr, 0, 0, 0, 0, 0);

    // getcwd (equivalent to pwd command)
    char cwd_buf[256];
    for (uint32_t i = 0; i < sizeof(cwd_buf); ++i) cwd_buf[i] = 0;
    auto buf_addr = reinterpret_cast<uint64_t>(cwd_buf);

    int64_t n = cinux::syscall::sys_getcwd(buf_addr, sizeof(cwd_buf), 0, 0, 0, 0);
    TEST_ASSERT_GT(n, 0);
    TEST_ASSERT_TRUE(k_strcmp(cwd_buf, dir_path) == 0);

    cinux::lib::kprintf("[CWD_STAT] shell pwd -> %s OK\n", cwd_buf);

    reset_cwd();
    cinux::syscall::sys_rmdir(dir_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

void test_stat_command_outputs_info() {
    auto pair = setup_cwd_stat();
    TEST_ASSERT_NOT_NULL(pair.ext2);

    // Create a file
    char fname[32]; gen_name(fname, 32, "ss");
    char fpath[64]; make_root_path(fpath, 64, fname);
    auto fpath_addr = reinterpret_cast<uint64_t>(fpath);

    cinux::syscall::sys_creat(fpath_addr, 0, 0, 0, 0, 0);

    // stat the file (equivalent to stat command)
    cinux::fs::stat st;
    for (uint32_t i = 0; i < sizeof(st); ++i)
        reinterpret_cast<uint8_t*>(&st)[i] = 0;
    auto st_addr = reinterpret_cast<uint64_t>(&st);

    int64_t result = cinux::syscall::sys_stat(fpath_addr, st_addr, 0, 0, 0, 0);
    TEST_ASSERT_EQ(result, 0);
    TEST_ASSERT_GT(st.st_ino, 0ULL);

    cinux::lib::kprintf("[CWD_STAT] shell stat %s -> ino=%lu size=%ld OK\n",
                        fpath, st.st_ino, st.st_size);

    // Cleanup
    cinux::syscall::sys_unlink(fpath_addr, 0, 0, 0, 0, 0);
    teardown_cwd_stat(pair);
}

}  // namespace test_shell_cwd_stat

// ============================================================
// Entry point
// ============================================================

extern "C" void run_cwd_stat_tests() {
    TEST_SECTION("CWD/Stat Tests (028c)");

    // Set up a mock current task so chdir/getcwd can read/write cwd
    cinux::proc::Task test_task;
    for (uint32_t i = 0; i < sizeof(test_task); ++i)
        reinterpret_cast<uint8_t*>(&test_task)[i] = 0;
    test_task.cwd[0] = '/';
    test_task.cwd[1] = '\0';
    cinux::proc::Scheduler::set_current(&test_task);

    // CWD initial value
    RUN_TEST(test_cwd_init::test_cwd_initial_is_root);

    // path_canonicalize
    RUN_TEST(test_path_canonicalize::test_dot_dot_collapses);
    RUN_TEST(test_path_canonicalize::test_dot_is_removed);
    RUN_TEST(test_path_canonicalize::test_duplicate_slashes_collapsed);
    RUN_TEST(test_path_canonicalize::test_trailing_slash_removed);
    RUN_TEST(test_path_canonicalize::test_root_stays_root);
    RUN_TEST(test_path_canonicalize::test_all_dot_components);
    RUN_TEST(test_path_canonicalize::test_dotdot_at_root);

    // path_resolve
    RUN_TEST(test_path_resolve::test_absolute_passthrough);
    RUN_TEST(test_path_resolve::test_relative_join);
    RUN_TEST(test_path_resolve::test_dotdot_goes_up);
    RUN_TEST(test_path_resolve::test_null_args_fail);

    // sys_chdir
    RUN_TEST(test_sys_chdir::test_chdir_to_existing_dir);
    RUN_TEST(test_sys_chdir::test_chdir_then_getcwd);
    RUN_TEST(test_sys_chdir::test_chdir_nonexistent_fails);
    RUN_TEST(test_sys_chdir::test_chdir_file_fails);
    RUN_TEST(test_sys_chdir::test_consecutive_chdir);

    // sys_stat
    RUN_TEST(test_sys_stat::test_stat_root_directory);
    RUN_TEST(test_sys_stat::test_stat_file_by_path);
    RUN_TEST(test_sys_stat::test_stat_nonexistent_fails);
    RUN_TEST(test_sys_stat::test_stat_relative_path_after_chdir);

    // sys_fstat
    RUN_TEST(test_sys_fstat::test_fstat_open_file);
    RUN_TEST(test_sys_fstat::test_fstat_matches_stat);
    RUN_TEST(test_sys_fstat::test_fstat_bad_fd_fails);

    // Shell commands
    RUN_TEST(test_shell_cwd_stat::test_cd_command_changes_cwd);
    RUN_TEST(test_shell_cwd_stat::test_pwd_command_outputs_cwd);
    RUN_TEST(test_shell_cwd_stat::test_stat_command_outputs_info);

    TEST_SUMMARY();

    // Tear down mock current task
    cinux::proc::Scheduler::set_current(nullptr);
}
