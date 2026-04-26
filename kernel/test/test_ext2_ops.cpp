/**
 * @file kernel/test/test_ext2_ops.cpp
 * @brief QEMU in-kernel integration tests for ext2 write/create/unlink (028b)
 *
 * Runs inside QEMU as part of the big kernel test suite.  Verifies that:
 *   - create() allocates an inode and adds a directory entry
 *   - file write/read round-trip preserves data
 *   - unlink() removes entries and frees resources (link_count==0)
 *   - Creating a file, unlinking it, then recreating with the same name works
 *   - Full flow: create -> write -> read -> unlink
 *
 * NOTE: mkdir is tested exclusively in the host-side unit tests
 * (test/unit/test_ext2_ops.cpp) because directory unlink only decrements
 * link_count from 2 to 1 (not 0), leaking inode+block resources on the
 * shared test disk.  This would cause subsequent test runs to fail.
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
using cinux::fs::Inode;

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
// Test 1: create — creates file with directory entry
// ============================================================

namespace test_ext2_create {

/**
 * @brief Verify create() allocates an inode and adds a directory entry,
 *        then unlink cleanly removes it (link_count 1->0)
 */
void test_create_and_unlink_file() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "tf");

    Inode* ino = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(ino);
    TEST_ASSERT_EQ(ino->type, cinux::fs::InodeType::Regular);
    TEST_ASSERT_EQ(ino->size, 0u);

    // Verify we can look up the file
    Inode* found = pair.ext2->lookup(name);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQ(found->ino, ino->ino);

    cinux::lib::kprintf("[EXT2_OPS] create file OK (ino=%lu)\n", ino->ino);

    // Unlink (file link_count 1->0, fully freed)
    int rc = pair.ext2->unlink(2, name, name_len(name));
    TEST_ASSERT_EQ(rc, 0);

    // Verify the file is gone
    Inode* gone = pair.ext2->lookup(name);
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[EXT2_OPS] create+unlink file OK\n");

    teardown_ext2(pair);
}

}  // namespace test_ext2_create

// ============================================================
// Test 2: file write + read round-trip
// ============================================================

namespace test_ext2_write {

/**
 * @brief Create file, write data, read back, verify integrity
 */
void test_write_then_read() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "wt");
    Inode* ino = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(ino);
    TEST_ASSERT_NOT_NULL(ino->ops);

    // Write data
    const char write_data[] = "Hello from ext2 write!";
    uint32_t   len          = sizeof(write_data) - 1;

    int64_t written = ino->ops->write(ino, 0, write_data, len);
    TEST_ASSERT_EQ(written, static_cast<int64_t>(len));

    // Read data back
    char    read_buf[64] = {};
    int64_t read_back    = ino->ops->read(ino, 0, read_buf, len);
    TEST_ASSERT_EQ(read_back, static_cast<int64_t>(len));

    // Verify data integrity
    for (uint32_t i = 0; i < len; ++i) {
        TEST_ASSERT_EQ(read_buf[i], write_data[i]);
    }

    cinux::lib::kprintf("[EXT2_OPS] Write+read round-trip OK\n");

    // Verify inode size updated
    TEST_ASSERT_EQ(ino->size, static_cast<uint64_t>(len));

    // Clean up
    pair.ext2->unlink(2, name, name_len(name));
    teardown_ext2(pair);
}

/**
 * @brief Write data that spans two blocks
 */
void test_write_cross_block() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "bf");
    Inode* ino = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(ino);

    uint32_t bs = pair.ext2->block_size();

    // Write data at offset (block_size - 10), crossing block boundary
    uint8_t write_data[20];
    for (int i = 0; i < 20; ++i) {
        write_data[i] = static_cast<uint8_t>('A' + i);
    }

    int64_t written = ino->ops->write(ino, bs - 10, write_data, 20);
    TEST_ASSERT_EQ(written, 20);

    // Read back
    uint8_t read_buf[20] = {};
    int64_t read_back    = ino->ops->read(ino, bs - 10, read_buf, 20);
    TEST_ASSERT_EQ(read_back, 20);

    for (int i = 0; i < 20; ++i) {
        TEST_ASSERT_EQ(read_buf[i], write_data[i]);
    }

    cinux::lib::kprintf("[EXT2_OPS] Cross-block write+read OK (bs=%u)\n", bs);

    pair.ext2->unlink(2, name, name_len(name));
    teardown_ext2(pair);
}

}  // namespace test_ext2_write

// ============================================================
// Test 3: create, unlink, recreate with same name
// ============================================================

namespace test_ext2_recreate {

/**
 * @brief Create a file, unlink it, create another file with the same name
 */
void test_recreate_same_name() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    char name[32];
    gen_name(name, 32, "rc");

    // Create first file
    Inode* file1 = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(file1);
    uint64_t ino1 = file1->ino;

    // Write some data
    const char data[] = "version1";
    file1->ops->write(file1, 0, data, 8);

    // Unlink
    int result = pair.ext2->unlink(2, name, name_len(name));
    TEST_ASSERT_EQ(result, 0);

    // Lookup should fail
    Inode* gone = pair.ext2->lookup(name);
    TEST_ASSERT_NULL(gone);

    // Recreate with same name
    Inode* file2 = pair.ext2->create(2, name, name_len(name));
    TEST_ASSERT_NOT_NULL(file2);
    // New inode should be fresh (size=0)
    TEST_ASSERT_EQ(file2->size, 0u);

    cinux::lib::kprintf("[EXT2_OPS] Recreate same name: ino1=%lu -> ino2=%lu OK\n", ino1,
                        file2->ino);

    // Clean up
    pair.ext2->unlink(2, name, name_len(name));
    teardown_ext2(pair);
}

}  // namespace test_ext2_recreate

// ============================================================
// Test 4: Full flow — create, write, read, unlink
// ============================================================

namespace test_ext2_full_flow {

/**
 * @brief End-to-end test: create file, write data, read it back, unlink
 */
void test_full_flow() {
    auto pair = setup_ext2();
    TEST_ASSERT_NOT_NULL(pair.ext2);
    TEST_ASSERT_TRUE(pair.ext2->is_mounted());

    // 1. Create a file in root
    char filename[32];
    gen_name(filename, 32, "ft");
    Inode* file = pair.ext2->create(2, filename, name_len(filename));
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQ(file->type, cinux::fs::InodeType::Regular);

    cinux::lib::kprintf("[EXT2_OPS] Full flow: created file '%s'\n", filename);

    // 2. Write data
    const char write_data[] = "Cinux ext2 full flow test data!";
    uint32_t   len          = sizeof(write_data) - 1;
    int64_t    written      = file->ops->write(file, 0, write_data, len);
    TEST_ASSERT_EQ(written, static_cast<int64_t>(len));

    // 3. Read data back
    char    read_buf[64] = {};
    int64_t read_back    = file->ops->read(file, 0, read_buf, len);
    TEST_ASSERT_EQ(read_back, static_cast<int64_t>(len));

    for (uint32_t i = 0; i < len; ++i) {
        TEST_ASSERT_EQ(read_buf[i], write_data[i]);
    }

    cinux::lib::kprintf("[EXT2_OPS] Full flow: write+read verified\n");

    // 4. Unlink the file
    int result = pair.ext2->unlink(2, filename, name_len(filename));
    TEST_ASSERT_EQ(result, 0);

    // 5. Verify the file is gone
    Inode* gone = pair.ext2->lookup(filename);
    TEST_ASSERT_NULL(gone);

    cinux::lib::kprintf("[EXT2_OPS] Full flow: complete OK\n");

    teardown_ext2(pair);
}

}  // namespace test_ext2_full_flow

// ============================================================
// Entry point
// ============================================================

extern "C" void run_ext2_ops_tests() {
    TEST_SECTION("Ext2 Ops Tests (028b)");

    // create + unlink
    RUN_TEST(test_ext2_create::test_create_and_unlink_file);

    // write tests
    RUN_TEST(test_ext2_write::test_write_then_read);
    RUN_TEST(test_ext2_write::test_write_cross_block);

    // recreate test
    RUN_TEST(test_ext2_recreate::test_recreate_same_name);

    // full flow
    RUN_TEST(test_ext2_full_flow::test_full_flow);

    TEST_SUMMARY();
}
