/**
 * @file test/unit/test_ext2_inode_ops.cpp
 * @brief Host-side unit tests for InodeOps virtual class hierarchy
 *
 * Test coverage:
 *   - InodeOps default implementations: read/write/readdir return -1,
 *     create/mkdir return nullptr, unlink returns -1
 *   - Ext2FileOps overrides: read, write are virtual dispatch targets
 *   - Ext2DirOps overrides: readdir, create, mkdir, unlink are virtual dispatch targets
 *   - Default fallback: Ext2FileOps calling create/mkdir/unlink returns nullptr/-1
 *   - Default fallback: Ext2DirOps calling read/write returns -1
 *   - Inode ops pointer assignment: file->Ext2FileOps, dir->Ext2DirOps
 *   - Virtual dispatch correctness via base-class pointer
 *
 * Pure C++ virtual dispatch testing -- no kernel code linked.
 * Uses mock InodeOps subclasses to verify override behaviour without
 * needing real AHCI/Ext2 infrastructure.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>

#include "fs/inode.hpp"

using namespace cinux::fs;

// ============================================================
// Mock InodeOps subclasses for host testing
//
// Ext2FileOps and Ext2DirOps require an Ext2& reference, which
// in turn requires AHCI hardware.  Instead, we create lightweight
// mock subclasses that mirror the override pattern:
//   MockFileOps: overrides read, write (like Ext2FileOps)
//   MockDirOps:  overrides readdir, create, mkdir, unlink (like Ext2DirOps)
// ============================================================

/**
 * @brief Mock file ops -- mirrors Ext2FileOps override pattern
 *
 * Overrides read() and write(); all other operations use the
 * InodeOps defaults (return -1 / nullptr).
 */
class MockFileOps : public InodeOps {
public:
    int64_t read(const Inode* /*inode*/, uint64_t /*offset*/,
                 void* buf, uint64_t count) override {
        // Simulate a successful read: fill buffer with 'R' bytes
        auto* data = static_cast<uint8_t*>(buf);
        for (uint64_t i = 0; i < count; ++i) {
            data[i] = 'R';
        }
        return static_cast<int64_t>(count);
    }

    int64_t write(Inode* /*inode*/, uint64_t /*offset*/,
                  const void* /*buf*/, uint64_t count) override {
        // Simulate a successful write
        return static_cast<int64_t>(count);
    }
};

/**
 * @brief Mock dir ops -- mirrors Ext2DirOps override pattern
 *
 * Overrides readdir(), create(), mkdir(), unlink();
 * read()/write() use the InodeOps defaults.
 */
class MockDirOps : public InodeOps {
public:
    int64_t readdir(const Inode* /*inode*/, uint64_t index,
                    char* name, uint64_t name_max) override {
        // Simulate returning a single directory entry "entry0"
        if (index == 0 && name != nullptr && name_max >= 7) {
            const char* entry = "entry0";
            for (int i = 0; i < 7; ++i) {
                name[i] = entry[i];
            }
            name[6] = '\0';
            return 0;  // success
        }
        return -1;  // no more entries
    }

    Inode* create(Inode* /*dir*/, const char* /*name*/,
                  uint32_t /*namelen*/) override {
        // Return a pointer to a static Inode to simulate creation
        static Inode fake{42, 0, InodeType::Regular, nullptr, nullptr};
        return &fake;
    }

    Inode* mkdir(Inode* /*dir*/, const char* /*name*/,
                 uint32_t /*namelen*/) override {
        static Inode fake{99, 1024, InodeType::Directory, nullptr, nullptr};
        return &fake;
    }

    int64_t unlink(Inode* /*dir*/, const char* /*name*/,
                   uint32_t /*namelen*/) override {
        return 0;  // success
    }
};

// ============================================================
// Test 1: InodeOps base class defaults return correct values
// ============================================================

TEST("inode_ops_defaults: base read returns -1") {
    InodeOps base;
    Inode dummy{1, 0, InodeType::Regular, &base, nullptr};
    char buf[16];
    int64_t result = base.read(&dummy, 0, buf, 16);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

TEST("inode_ops_defaults: base write returns -1") {
    InodeOps base;
    Inode dummy{1, 0, InodeType::Regular, &base, nullptr};
    const char data[] = "test";
    int64_t result = base.write(&dummy, 0, data, 4);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

TEST("inode_ops_defaults: base readdir returns -1") {
    InodeOps base;
    Inode dummy{1, 0, InodeType::Directory, &base, nullptr};
    char name[64];
    int64_t result = base.readdir(&dummy, 0, name, 64);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

TEST("inode_ops_defaults: base create returns nullptr") {
    InodeOps base;
    Inode dummy{1, 0, InodeType::Directory, &base, nullptr};
    Inode* result = base.create(&dummy, "file", 4);
    ASSERT_NULL(result);
}

TEST("inode_ops_defaults: base mkdir returns nullptr") {
    InodeOps base;
    Inode dummy{1, 0, InodeType::Directory, &base, nullptr};
    Inode* result = base.mkdir(&dummy, "dir", 3);
    ASSERT_NULL(result);
}

TEST("inode_ops_defaults: base unlink returns -1") {
    InodeOps base;
    Inode dummy{1, 0, InodeType::Directory, &base, nullptr};
    int64_t result = base.unlink(&dummy, "file", 4);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

// ============================================================
// Test 2: MockFileOps overrides dispatch correctly
// ============================================================

TEST("mock_file_ops: read override returns correct byte count") {
    MockFileOps file_ops;
    Inode file{10, 100, InodeType::Regular, &file_ops, nullptr};
    char buf[32] = {};

    int64_t result = file_ops.read(&file, 0, buf, 32);
    ASSERT_EQ(result, 32);

    // Verify buffer was filled by the override
    for (int i = 0; i < 32; ++i) {
        ASSERT_EQ(buf[i], 'R');
    }
}

TEST("mock_file_ops: write override returns correct byte count") {
    MockFileOps file_ops;
    Inode file{10, 100, InodeType::Regular, &file_ops, nullptr};
    const char data[] = "Hello";

    int64_t result = file_ops.write(&file, 0, data, 5);
    ASSERT_EQ(result, 5);
}

// ============================================================
// Test 3: MockFileOps non-overridden methods fall back to defaults
// ============================================================

TEST("mock_file_ops: readdir falls back to default (-1)") {
    MockFileOps file_ops;
    Inode file{10, 100, InodeType::Regular, &file_ops, nullptr};
    char name[64];

    int64_t result = file_ops.readdir(&file, 0, name, 64);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

TEST("mock_file_ops: create falls back to default (nullptr)") {
    MockFileOps file_ops;
    Inode file{10, 100, InodeType::Regular, &file_ops, nullptr};

    Inode* result = file_ops.create(&file, "newfile", 7);
    ASSERT_NULL(result);
}

TEST("mock_file_ops: mkdir falls back to default (nullptr)") {
    MockFileOps file_ops;
    Inode file{10, 100, InodeType::Regular, &file_ops, nullptr};

    Inode* result = file_ops.mkdir(&file, "newdir", 6);
    ASSERT_NULL(result);
}

TEST("mock_file_ops: unlink falls back to default (-1)") {
    MockFileOps file_ops;
    Inode file{10, 100, InodeType::Regular, &file_ops, nullptr};

    int64_t result = file_ops.unlink(&file, "somefile", 8);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

// ============================================================
// Test 4: MockDirOps overrides dispatch correctly
// ============================================================

TEST("mock_dir_ops: readdir override returns entry at index 0") {
    MockDirOps dir_ops;
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};
    char name[64] = {};

    int64_t result = dir_ops.readdir(&dir, 0, name, 64);
    ASSERT_EQ(result, 0);

    // Verify name was populated
    ASSERT_EQ(name[0], 'e');
    ASSERT_EQ(name[1], 'n');
    ASSERT_EQ(name[2], 't');
    ASSERT_EQ(name[3], 'r');
    ASSERT_EQ(name[4], 'y');
    ASSERT_EQ(name[5], '0');
}

TEST("mock_dir_ops: readdir returns -1 for index out of range") {
    MockDirOps dir_ops;
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};
    char name[64];

    int64_t result = dir_ops.readdir(&dir, 1, name, 64);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

TEST("mock_dir_ops: create override returns non-null Inode") {
    MockDirOps dir_ops;
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};

    Inode* result = dir_ops.create(&dir, "newfile", 7);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(result->ino, 42u);
    ASSERT_EQ(result->type, InodeType::Regular);
}

TEST("mock_dir_ops: mkdir override returns non-null Inode") {
    MockDirOps dir_ops;
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};

    Inode* result = dir_ops.mkdir(&dir, "newdir", 6);
    ASSERT_NOT_NULL(result);
    ASSERT_EQ(result->ino, 99u);
    ASSERT_EQ(result->type, InodeType::Directory);
}

TEST("mock_dir_ops: unlink override returns 0 (success)") {
    MockDirOps dir_ops;
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};

    int64_t result = dir_ops.unlink(&dir, "somefile", 8);
    ASSERT_EQ(result, static_cast<int64_t>(0));
}

// ============================================================
// Test 5: MockDirOps non-overridden methods fall back to defaults
// ============================================================

TEST("mock_dir_ops: read falls back to default (-1)") {
    MockDirOps dir_ops;
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};
    char buf[16];

    int64_t result = dir_ops.read(&dir, 0, buf, 16);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

TEST("mock_dir_ops: write falls back to default (-1)") {
    MockDirOps dir_ops;
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};
    const char data[] = "test";

    int64_t result = dir_ops.write(&dir, 0, data, 4);
    ASSERT_EQ(result, static_cast<int64_t>(-1));
}

// ============================================================
// Test 6: Virtual dispatch through base-class InodeOps pointer
// ============================================================

TEST("virtual_dispatch: file ops via base pointer calls read override") {
    MockFileOps file_ops;
    InodeOps* base_ptr = &file_ops;  // upcast to base
    Inode file{10, 100, InodeType::Regular, base_ptr, nullptr};
    char buf[16] = {};

    int64_t result = base_ptr->read(&file, 0, buf, 16);
    ASSERT_EQ(result, 16);
    // Verify it was the override (filled with 'R'), not the default (-1)
    ASSERT_EQ(buf[0], 'R');
}

TEST("virtual_dispatch: file ops via base pointer calls write override") {
    MockFileOps file_ops;
    InodeOps* base_ptr = &file_ops;
    Inode file{10, 100, InodeType::Regular, base_ptr, nullptr};
    const char data[] = "test";

    int64_t result = base_ptr->write(&file, 0, data, 4);
    ASSERT_EQ(result, 4);
}

TEST("virtual_dispatch: dir ops via base pointer calls readdir override") {
    MockDirOps dir_ops;
    InodeOps* base_ptr = &dir_ops;
    Inode dir{2, 1024, InodeType::Directory, base_ptr, nullptr};
    char name[64] = {};

    int64_t result = base_ptr->readdir(&dir, 0, name, 64);
    ASSERT_EQ(result, 0);
}

TEST("virtual_dispatch: dir ops via base pointer calls create override") {
    MockDirOps dir_ops;
    InodeOps* base_ptr = &dir_ops;
    Inode dir{2, 1024, InodeType::Directory, base_ptr, nullptr};

    Inode* result = base_ptr->create(&dir, "file", 4);
    ASSERT_NOT_NULL(result);
}

TEST("virtual_dispatch: dir ops via base pointer calls mkdir override") {
    MockDirOps dir_ops;
    InodeOps* base_ptr = &dir_ops;
    Inode dir{2, 1024, InodeType::Directory, base_ptr, nullptr};

    Inode* result = base_ptr->mkdir(&dir, "dir", 3);
    ASSERT_NOT_NULL(result);
}

TEST("virtual_dispatch: dir ops via base pointer calls unlink override") {
    MockDirOps dir_ops;
    InodeOps* base_ptr = &dir_ops;
    Inode dir{2, 1024, InodeType::Directory, base_ptr, nullptr};

    int64_t result = base_ptr->unlink(&dir, "file", 4);
    ASSERT_EQ(result, 0);
}

// ============================================================
// Test 7: Inode ops pointer assignment for files and directories
// ============================================================

TEST("inode_ops_assignment: file inode gets file ops") {
    MockFileOps file_ops;
    MockDirOps dir_ops;

    Inode file_inode;
    file_inode.ino = 10;
    file_inode.type = InodeType::Regular;
    file_inode.ops = &file_ops;

    // File ops should handle read/write
    char buf[16] = {};
    ASSERT_EQ(file_inode.ops->read(&file_inode, 0, buf, 16), 16);
    const char data[] = "x";
    ASSERT_EQ(file_inode.ops->write(&file_inode, 0, data, 1), 1);

    // File ops should NOT handle dir operations
    char name[64];
    ASSERT_EQ(file_inode.ops->readdir(&file_inode, 0, name, 64),
              static_cast<int64_t>(-1));
    ASSERT_NULL(file_inode.ops->create(&file_inode, "f", 1));
    ASSERT_NULL(file_inode.ops->mkdir(&file_inode, "d", 1));
    ASSERT_EQ(file_inode.ops->unlink(&file_inode, "f", 1),
              static_cast<int64_t>(-1));
}

TEST("inode_ops_assignment: dir inode gets dir ops") {
    MockFileOps file_ops;
    MockDirOps dir_ops;

    Inode dir_inode;
    dir_inode.ino = 2;
    dir_inode.type = InodeType::Directory;
    dir_inode.ops = &dir_ops;

    // Dir ops should handle readdir/create/mkdir/unlink
    char name[64] = {};
    ASSERT_EQ(dir_inode.ops->readdir(&dir_inode, 0, name, 64), 0);
    ASSERT_NOT_NULL(dir_inode.ops->create(&dir_inode, "f", 1));
    ASSERT_NOT_NULL(dir_inode.ops->mkdir(&dir_inode, "d", 1));
    ASSERT_EQ(dir_inode.ops->unlink(&dir_inode, "f", 1),
              static_cast<int64_t>(0));

    // Dir ops should NOT handle file read/write
    char buf[16];
    ASSERT_EQ(dir_inode.ops->read(&dir_inode, 0, buf, 16),
              static_cast<int64_t>(-1));
    const char data[] = "x";
    ASSERT_EQ(dir_inode.ops->write(&dir_inode, 0, data, 1),
              static_cast<int64_t>(-1));
}

TEST("inode_ops_assignment: null ops pointer means no operations") {
    Inode inode;
    inode.ino = 1;
    inode.type = InodeType::Unknown;
    inode.ops = nullptr;

    ASSERT_NULL(inode.ops);
}

// ============================================================
// Test 8: Ext2FileOps/Ext2DirOps type identities (class hierarchy)
// ============================================================

TEST("class_hierarchy: MockFileOps is-a InodeOps") {
    MockFileOps file_ops;
    InodeOps* base = &file_ops;

    // Should be able to call through base pointer
    Inode dummy{1, 0, InodeType::Regular, base, nullptr};
    char buf[8] = {};
    ASSERT_EQ(base->read(&dummy, 0, buf, 8), 8);
}

TEST("class_hierarchy: MockDirOps is-a InodeOps") {
    MockDirOps dir_ops;
    InodeOps* base = &dir_ops;

    Inode dummy{1, 0, InodeType::Directory, base, nullptr};
    char name[64] = {};
    ASSERT_EQ(base->readdir(&dummy, 0, name, 64), 0);
}

// ============================================================
// Test 9: Multiple Inode objects can share the same ops instance
// ============================================================

TEST("shared_ops: two file inodes share same file ops") {
    MockFileOps shared_ops;

    Inode file1{10, 100, InodeType::Regular, &shared_ops, nullptr};
    Inode file2{11, 200, InodeType::Regular, &shared_ops, nullptr};

    char buf1[4] = {};
    char buf2[4] = {};

    ASSERT_EQ(file1.ops->read(&file1, 0, buf1, 4), 4);
    ASSERT_EQ(file2.ops->read(&file2, 0, buf2, 4), 4);

    // Both buffers should be filled by the same override
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(buf1[i], 'R');
        ASSERT_EQ(buf2[i], 'R');
    }
}

TEST("shared_ops: file and dir inodes use different ops instances") {
    MockFileOps file_ops;
    MockDirOps dir_ops;

    Inode file{10, 100, InodeType::Regular, &file_ops, nullptr};
    Inode dir{2, 1024, InodeType::Directory, &dir_ops, nullptr};

    // File read succeeds
    char buf[4] = {};
    ASSERT_EQ(file.ops->read(&file, 0, buf, 4), 4);

    // Dir read returns -1 (default)
    ASSERT_EQ(dir.ops->read(&dir, 0, buf, 4),
              static_cast<int64_t>(-1));

    // Dir create succeeds
    ASSERT_NOT_NULL(dir.ops->create(&dir, "f", 1));

    // File create returns nullptr (default)
    ASSERT_NULL(file.ops->create(&file, "f", 1));
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
