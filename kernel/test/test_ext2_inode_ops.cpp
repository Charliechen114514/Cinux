/**
 * @file kernel/test/test_ext2_inode_ops.cpp
 * @brief QEMU in-kernel integration tests for InodeOps virtual class (028b)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that:
 *   - lookup("file") returns inode with Ext2FileOps (read/write available)
 *   - lookup("dir") returns inode with Ext2DirOps (readdir/create/mkdir/unlink available)
 *   - ops->create creates file and file is visible via lookup
 *   - ops->mkdir creates directory and directory is visible via lookup
 *   - ops->unlink removes entry and lookup confirms removal
 *   - Ext2FileOps: read/write work, create/mkdir/unlink return defaults
 *   - Ext2DirOps: readdir/create/mkdir/unlink work, read/write return defaults
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

using cinux::drivers::pci::PCI;
using cinux::drivers::pci::PCIDevice;
using cinux::drivers::ahci::AHCI;
using cinux::fs::Ext2;
using cinux::fs::Ext2FileOps;
using cinux::fs::Ext2DirOps;
using cinux::fs::Inode;
using cinux::fs::InodeOps;
using cinux::fs::InodeType;

// ============================================================
// Helper: create an initialised AHCI + Ext2 for each test
// ============================================================

namespace {

struct AhciExt2Pair {
    AHCI* ahci;
    Ext2* ext2;
};

AhciExt2Pair setup_ext2() {
    AhciExt2Pair result{nullptr, nullptr};

    PCI pci;
    pci.init();

    PCIDevice ahci_dev{};
    if (!pci.find_ahci(ahci_dev)) {
        return result;
    }

    result.ahci = new AHCI();
    result.ahci->init(ahci_dev);
    if (result.ahci->hba_mem() == nullptr) {
        return result;
    }

    result.ext2 = new Ext2(*result.ahci, 1);
    result.ext2->mount();

    return result;
}

void teardown_ext2(AhciExt2Pair& pair) {
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
// Test 1: File inode has Ext2FileOps -- read/write work
// ============================================================

namespace test_file_ops {

/**
 * @brief Verify that a file inode's ops pointer supports read/write
 */
void test_file_inode_read_write() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Create a file via the top-level API
    char name[32];
    gen_name(name, 32, "iof");
    Inode* ino = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(ino);
    TEST_ASSERT_NOT_NULL(ino->ops);

    // Write data through ops
    const char write_data[] = "InodeOps virtual dispatch";
    uint32_t   len          = sizeof(write_data) - 1;

    int64_t written = ino->ops->write(ino, 0, write_data, len);
    TEST_ASSERT_EQ(written, static_cast<int64_t>(len));

    // Read data back through ops
    char    read_buf[64] = {};
    int64_t read_back    = ino->ops->read(ino, 0, read_buf, len);
    TEST_ASSERT_EQ(read_back, static_cast<int64_t>(len));

    for (uint32_t i = 0; i < len; ++i) {
        TEST_ASSERT_EQ(read_buf[i], write_data[i]);
    }

    cinux::lib::kprintf("[INODE_OPS] File inode read/write OK\n");

    // Cleanup
    pair.ext2->unlink(2, name, name_len(name));
    teardown_ext2(pair);
}

}  // namespace test_file_ops

// ============================================================
// Test 2: File inode's ops rejects dir operations
// ============================================================

namespace test_file_ops_defaults {

/**
 * @brief Verify file ops returns defaults for create/mkdir/unlink
 */
void test_file_ops_dir_defaults() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "fd");
    Inode* ino = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(ino);
    TEST_ASSERT_NOT_NULL(ino->ops);

    // create should return nullptr (default)
    Inode* created = ino->ops->create(ino, "x", 1);
    TEST_ASSERT_NULL(created);

    // mkdir should return nullptr (default)
    Inode* dir = ino->ops->mkdir(ino, "d", 1);
    TEST_ASSERT_NULL(dir);

    // unlink should return -1 (default)
    int64_t rc = ino->ops->unlink(ino, "x", 1);
    TEST_ASSERT_EQ(rc, static_cast<int64_t>(-1));

    cinux::lib::kprintf("[INODE_OPS] File ops dir defaults OK\n");

    pair.ext2->unlink(2, name, name_len(name));
    teardown_ext2(pair);
}

}  // namespace test_file_ops_defaults

// ============================================================
// Test 3: Directory inode has Ext2DirOps -- readdir works
// ============================================================

namespace test_dir_ops_readdir {

/**
 * @brief Verify that root directory inode's ops supports readdir
 */
void test_dir_inode_readdir() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Root is inode 2
    Inode* root = pair.ext2->lookup("");
    // lookup("") may return nullptr; try "."
    if (root == nullptr) {
        root = pair.ext2->lookup(".");
    }
    // If neither works, create a dir and use it
    if (root == nullptr) {
        // Use the ext2 object's root_inode indirectly by creating a dir
        char dirname[32];
        gen_name(dirname, 32, "td");
        Inode* dir_ino = pair.ext2->mkdir(2, dirname, name_len(dirname));
        TEST_ASSERT_NOT_NULL(dir_ino);
        TEST_ASSERT_NOT_NULL(dir_ino->ops);

        // readdir on the new directory should at least find "." and ".."
        char    rname[256] = {};
        int64_t rc         = dir_ino->ops->readdir(dir_ino, 0, rname, 256);
        TEST_ASSERT_GT(rc, 0);  // readdir returns 1 on success, -1 on failure
        TEST_ASSERT_EQ(rname[0], '.');

        cinux::lib::kprintf("[INODE_OPS] Dir inode readdir OK\n");

        pair.ext2->unlink(2, dirname, name_len(dirname));
        teardown_ext2(pair);
        return;
    }

    TEST_ASSERT_NOT_NULL(root->ops);

    char    rname[256] = {};
    int64_t rc         = root->ops->readdir(root, 0, rname, 256);
    // At least one entry should be readable (readdir returns 1 on success)
    TEST_ASSERT_GT(rc, 0);

    cinux::lib::kprintf("[INODE_OPS] Root dir readdir OK\n");

    teardown_ext2(pair);
}

}  // namespace test_dir_ops_readdir

// ============================================================
// Test 4: ops->create on directory creates file
// ============================================================

namespace test_dir_ops_create {

/**
 * @brief Use ops->create to create a file in a directory, verify via lookup
 */
void test_create_via_ops() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Create a subdirectory first
    char dirname[32];
    gen_name(dirname, 32, "oc");
    Inode* dir = pair.ext2->mkdir(2, dirname, name_len(dirname));
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_NOT_NULL(dir->ops);

    // Use ops->create to create a file inside
    const char filename[] = "inner";
    Inode*     file       = dir->ops->create(dir, filename, 5);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQ(file->type, InodeType::Regular);

    // Verify the file can be found via lookup
    // Build composite lookup path: dirname + "/" + filename
    char     lookup_path[64];
    uint32_t li = 0;
    for (uint32_t j = 0; dirname[j] && li < sizeof(lookup_path) - 7; ++j)
        lookup_path[li++] = dirname[j];
    lookup_path[li++] = '/';
    for (uint32_t j = 0; filename[j] && li < sizeof(lookup_path) - 1; ++j)
        lookup_path[li++] = filename[j];
    lookup_path[li] = '\0';

    Inode* found = pair.ext2->lookup(lookup_path);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(found->ino, file->ino);

    cinux::lib::kprintf("[INODE_OPS] ops->create file OK (ino=%lu)\n", file->ino);

    // Cleanup
    pair.ext2->unlink(static_cast<uint32_t>(dir->ino), filename, 5);
    pair.ext2->unlink(2, dirname, name_len(dirname));
    teardown_ext2(pair);
}

}  // namespace test_dir_ops_create

// ============================================================
// Test 5: ops->mkdir on directory creates subdirectory
// ============================================================

namespace test_dir_ops_mkdir {

/**
 * @brief Use ops->mkdir to create a subdirectory, verify via lookup
 */
void test_mkdir_via_ops() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Create a parent directory
    char parent[32];
    gen_name(parent, 32, "om");
    Inode* dir = pair.ext2->mkdir(2, parent, name_len(parent));
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_NOT_NULL(dir->ops);

    // Use ops->mkdir to create a subdirectory
    const char child[] = "sub";
    Inode*     subdir  = dir->ops->mkdir(dir, child, 3);
    TEST_ASSERT_NOT_NULL(subdir);
    TEST_ASSERT_EQ(subdir->type, InodeType::Directory);

    // Verify the subdirectory can be found via lookup
    // Build composite lookup path: parent + "/" + child
    char     lookup_path[64];
    uint32_t li = 0;
    for (uint32_t j = 0; parent[j] && li < sizeof(lookup_path) - 5; ++j)
        lookup_path[li++] = parent[j];
    lookup_path[li++] = '/';
    for (uint32_t j = 0; child[j] && li < sizeof(lookup_path) - 1; ++j)
        lookup_path[li++] = child[j];
    lookup_path[li] = '\0';

    Inode* found = pair.ext2->lookup(lookup_path);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(found->ino, subdir->ino);

    cinux::lib::kprintf("[INODE_OPS] ops->mkdir dir OK (ino=%lu)\n", subdir->ino);

    // Cleanup: unlink sub then parent
    // Note: dir unlink only decrements link_count 2->1, doesn't free
    pair.ext2->unlink(static_cast<uint32_t>(dir->ino), child, 3);
    pair.ext2->unlink(2, parent, name_len(parent));
    teardown_ext2(pair);
}

}  // namespace test_dir_ops_mkdir

// ============================================================
// Test 6: ops->unlink removes entry
// ============================================================

namespace test_dir_ops_unlink {

/**
 * @brief Use ops->unlink to remove a file, verify via lookup
 */
void test_unlink_via_ops() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Create a parent directory
    char dirname[32];
    gen_name(dirname, 32, "or");
    Inode* dir = pair.ext2->mkdir(2, dirname, name_len(dirname));
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_NOT_NULL(dir->ops);

    // Create a file inside
    const char filename[] = "victim";
    Inode*     file       = dir->ops->create(dir, filename, 6);
    TEST_ASSERT_NOT_NULL(file);

    // Verify file exists via composite lookup path
    char     lookup_path[64];
    uint32_t li = 0;
    for (uint32_t j = 0; dirname[j] && li < sizeof(lookup_path) - 7; ++j)
        lookup_path[li++] = dirname[j];
    lookup_path[li++] = '/';
    for (uint32_t j = 0; filename[j] && li < sizeof(lookup_path) - 1; ++j)
        lookup_path[li++] = filename[j];
    lookup_path[li] = '\0';

    Inode* found = pair.ext2->lookup(lookup_path);
    TEST_ASSERT_NOT_NULL(found);

    // Use ops->unlink to remove it
    int64_t rc = dir->ops->unlink(dir, filename, 6);
    TEST_ASSERT_EQ(rc, static_cast<int64_t>(0));

    // Verify file is gone
    Inode* gone = pair.ext2->lookup(lookup_path);
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[INODE_OPS] ops->unlink file OK\n");

    pair.ext2->unlink(2, dirname, name_len(dirname));
    teardown_ext2(pair);
}

}  // namespace test_dir_ops_unlink

// ============================================================
// Test 7: Dir ops read/write return defaults (-1)
// ============================================================

namespace test_dir_ops_file_defaults {

/**
 * @brief Verify dir ops returns -1 for read and write
 */
void test_dir_ops_file_defaults() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char dirname[32];
    gen_name(dirname, 32, "dd");
    Inode* dir = pair.ext2->mkdir(2, dirname, name_len(dirname));
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_NOT_NULL(dir->ops);

    char    buf[16] = {};
    int64_t read_rc = dir->ops->read(dir, 0, buf, 16);
    TEST_ASSERT_EQ(read_rc, static_cast<int64_t>(-1));

    const char data[]   = "x";
    int64_t    write_rc = dir->ops->write(dir, 0, data, 1);
    TEST_ASSERT_EQ(write_rc, static_cast<int64_t>(-1));

    cinux::lib::kprintf("[INODE_OPS] Dir ops read/write defaults OK\n");

    pair.ext2->unlink(2, dirname, name_len(dirname));
    teardown_ext2(pair);
}

}  // namespace test_dir_ops_file_defaults

// ============================================================
// Test 8: Verify ops pointer type -- file vs dir dispatch
// ============================================================

namespace test_ops_dispatch_type {

/**
 * @brief Verify file inode gets file ops and dir inode gets dir ops
 *        by checking that overridden methods produce different results
 */
void test_ops_type_dispatch() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // Create a directory
    char dirname[32];
    gen_name(dirname, 32, "td");
    Inode* dir = pair.ext2->mkdir(2, dirname, name_len(dirname));
    TEST_ASSERT_NOT_NULL(dir);
    TEST_ASSERT_NOT_NULL(dir->ops);

    // Create a file inside
    const char filename[] = "tfile";
    Inode*     file       = dir->ops->create(dir, filename, 5);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_NOT_NULL(file->ops);

    // File read should succeed (not return -1)
    const char wdata[] = "hello";
    int64_t    written = file->ops->write(file, 0, wdata, 5);
    TEST_ASSERT_EQ(written, static_cast<int64_t>(5));

    char    rbuf[16]  = {};
    int64_t read_back = file->ops->read(file, 0, rbuf, 5);
    TEST_ASSERT_EQ(read_back, static_cast<int64_t>(5));

    // Dir read should fail (return -1 default)
    char    dbuf[16] = {};
    int64_t dir_read = dir->ops->read(dir, 0, dbuf, 16);
    TEST_ASSERT_EQ(dir_read, static_cast<int64_t>(-1));

    // Dir create should succeed (not return nullptr)
    const char fname2[] = "f2";
    Inode*     file2    = dir->ops->create(dir, fname2, 2);
    TEST_ASSERT_NOT_NULL(file2);

    // File create should fail (return nullptr default)
    Inode* bad = file->ops->create(file, "x", 1);
    TEST_ASSERT_NULL(bad);

    cinux::lib::kprintf("[INODE_OPS] Ops type dispatch verified OK\n");

    // Cleanup
    dir->ops->unlink(dir, filename, 5);
    dir->ops->unlink(dir, fname2, 2);
    pair.ext2->unlink(2, dirname, name_len(dirname));
    teardown_ext2(pair);
}

}  // namespace test_ops_dispatch_type

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ext2_inode_ops_tests() {
    TEST_SECTION("Ext2 InodeOps Virtual Class Tests (028b)");

    // File ops: read/write
    RUN_TEST(test_file_ops::test_file_inode_read_write);

    // File ops: defaults for dir operations
    RUN_TEST(test_file_ops_defaults::test_file_ops_dir_defaults);

    // Dir ops: readdir
    RUN_TEST(test_dir_ops_readdir::test_dir_inode_readdir);

    // Dir ops: create via ops
    RUN_TEST(test_dir_ops_create::test_create_via_ops);

    // Dir ops: mkdir via ops
    RUN_TEST(test_dir_ops_mkdir::test_mkdir_via_ops);

    // Dir ops: unlink via ops
    RUN_TEST(test_dir_ops_unlink::test_unlink_via_ops);

    // Dir ops: read/write defaults
    RUN_TEST(test_dir_ops_file_defaults::test_dir_ops_file_defaults);

    // Ops type dispatch verification
    RUN_TEST(test_ops_dispatch_type::test_ops_type_dispatch);

    TEST_SUMMARY();
}
