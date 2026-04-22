/**
 * @file test/unit/test_vfs_mount.cpp
 * @brief Host-side unit tests for VFS mount table (kernel/fs/vfs_mount.hpp)
 *
 * Test coverage:
 *   - vfs_mount_init: table is empty after init
 *   - vfs_mount_add: add a single mount point
 *   - vfs_mount_add: add multiple mount points
 *   - vfs_mount_add: full table returns false
 *   - vfs_mount_add: nullptr path returns false
 *   - vfs_mount_add: nullptr fs returns false
 *   - vfs_mount_add: empty path returns false
 *   - vfs_mount_remove: normal removal
 *   - vfs_mount_remove: non-existent path returns false
 *   - vfs_resolve: exact match
 *   - vfs_resolve: longest prefix match (/mnt wins over /)
 *   - vfs_resolve: no match returns nullptr
 *   - g_global_fd_table: returns same reference on repeated calls
 *
 * Links directly with kernel/fs/vfs_mount.cpp and kernel/fs/file.cpp.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>

#include "fs/vfs_mount.hpp"
#include "fs/vfs_filesystem.hpp"
#include "fs/inode.hpp"
#include "fs/file.hpp"

using namespace cinux::fs;

// ============================================================
// Mock FileSystem
// ============================================================

class MockFileSystem : public FileSystem {
public:
    bool   mount() override { return true; }
    Inode* lookup(const char* /*path*/) override { return nullptr; }
};

// ============================================================
// 1. Init
// ============================================================

// After vfs_mount_init(), the table should have no in-use entries.
TEST("vfs_mount: table is empty after init") {
    vfs_mount_init();
    // Attempting to resolve any path should return nullptr
    const char* rel = nullptr;
    ASSERT_NULL(vfs_resolve("/", &rel));
}

// ============================================================
// 2. Add -- single mount point
// ============================================================

TEST("vfs_mount: add single mount point") {
    vfs_mount_init();
    MockFileSystem fs;
    ASSERT_TRUE(vfs_mount_add("/", &fs));

    const char* rel = nullptr;
    FileSystem* result = vfs_resolve("/", &rel);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result == &fs);
    ASSERT_EQ(std::strcmp(rel, ""), 0);
}

// ============================================================
// 3. Add -- multiple mount points
// ============================================================

TEST("vfs_mount: add multiple mount points") {
    vfs_mount_init();
    MockFileSystem fs_root;
    MockFileSystem fs_mnt;

    ASSERT_TRUE(vfs_mount_add("/", &fs_root));
    ASSERT_TRUE(vfs_mount_add("/mnt", &fs_mnt));

    // Both should be resolvable
    const char* rel = nullptr;
    ASSERT_TRUE(vfs_resolve("/", &rel) == &fs_root);
    ASSERT_TRUE(vfs_resolve("/mnt", &rel) == &fs_mnt);
}

// ============================================================
// 4. Add -- full table returns false
// ============================================================

TEST("vfs_mount: add full table returns false") {
    vfs_mount_init();
    MockFileSystem fs[MOUNT_TABLE_SIZE];

    // Fill all 8 slots
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        char path[MOUNT_PATH_MAX];
        snprintf(path, sizeof(path), "/mp%u", i);
        ASSERT_TRUE(vfs_mount_add(path, &fs[i]));
    }

    // Next add should fail
    MockFileSystem extra;
    ASSERT_FALSE(vfs_mount_add("/overflow", &extra));
}

// ============================================================
// 5. Add -- invalid parameters
// ============================================================

TEST("vfs_mount: add nullptr path returns false") {
    vfs_mount_init();
    MockFileSystem fs;
    ASSERT_FALSE(vfs_mount_add(nullptr, &fs));
}

TEST("vfs_mount: add nullptr fs returns false") {
    vfs_mount_init();
    ASSERT_FALSE(vfs_mount_add("/", nullptr));
}

TEST("vfs_mount: add empty path returns false") {
    vfs_mount_init();
    MockFileSystem fs;
    ASSERT_FALSE(vfs_mount_add("", &fs));
}

// ============================================================
// 6. Remove -- normal removal
// ============================================================

TEST("vfs_mount: remove existing mount point") {
    vfs_mount_init();
    MockFileSystem fs;
    ASSERT_TRUE(vfs_mount_add("/test", &fs));
    ASSERT_TRUE(vfs_mount_remove("/test"));

    // After removal, resolve should fail
    const char* rel = nullptr;
    ASSERT_NULL(vfs_resolve("/test", &rel));
}

// ============================================================
// 7. Remove -- non-existent path
// ============================================================

TEST("vfs_mount: remove non-existent path returns false") {
    vfs_mount_init();
    ASSERT_FALSE(vfs_mount_remove("/no_such_mount"));
}

TEST("vfs_mount: remove nullptr returns false") {
    vfs_mount_init();
    ASSERT_FALSE(vfs_mount_remove(nullptr));
}

// ============================================================
// 8. Resolve -- exact match
// ============================================================

TEST("vfs_mount: resolve exact match on root") {
    vfs_mount_init();
    MockFileSystem fs;
    vfs_mount_add("/", &fs);

    const char* rel = nullptr;
    FileSystem* result = vfs_resolve("/hello.txt", &rel);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result == &fs);
    ASSERT_EQ(std::strcmp(rel, "hello.txt"), 0);
}

// ============================================================
// 9. Resolve -- longest prefix match
// ============================================================

// When both "/" and "/mnt" are mounted, "/mnt/file" should resolve
// to "/mnt", not "/" (longest-prefix wins).
TEST("vfs_mount: resolve longest prefix /mnt wins over /") {
    vfs_mount_init();
    MockFileSystem fs_root;
    MockFileSystem fs_mnt;

    vfs_mount_add("/", &fs_root);
    vfs_mount_add("/mnt", &fs_mnt);

    const char* rel = nullptr;
    FileSystem* result = vfs_resolve("/mnt/file.txt", &rel);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result == &fs_mnt);
    ASSERT_EQ(std::strcmp(rel, "/file.txt"), 0);
}

// "/" should still match paths that don't start with "/mnt".
TEST("vfs_mount: resolve falls back to root for non-mnt paths") {
    vfs_mount_init();
    MockFileSystem fs_root;
    MockFileSystem fs_mnt;

    vfs_mount_add("/", &fs_root);
    vfs_mount_add("/mnt", &fs_mnt);

    const char* rel = nullptr;
    FileSystem* result = vfs_resolve("/home/user.txt", &rel);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result == &fs_root);
    ASSERT_EQ(std::strcmp(rel, "home/user.txt"), 0);
}

// Three-level hierarchy: "/", "/mnt", "/mnt/data -- deepest wins.
TEST("vfs_mount: resolve three-level longest prefix") {
    vfs_mount_init();
    MockFileSystem fs_root;
    MockFileSystem fs_mnt;
    MockFileSystem fs_data;

    vfs_mount_add("/", &fs_root);
    vfs_mount_add("/mnt", &fs_mnt);
    vfs_mount_add("/mnt/data", &fs_data);

    const char* rel = nullptr;
    FileSystem* result = vfs_resolve("/mnt/data/README", &rel);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result == &fs_data);
    ASSERT_EQ(std::strcmp(rel, "/README"), 0);
}

// ============================================================
// 10. Resolve -- no match
// ============================================================

TEST("vfs_mount: resolve returns nullptr when no mount matches") {
    vfs_mount_init();
    const char* rel = nullptr;
    ASSERT_NULL(vfs_resolve("/anything", &rel));
}

TEST("vfs_mount: resolve nullptr path returns nullptr") {
    vfs_mount_init();
    const char* rel = nullptr;
    ASSERT_NULL(vfs_resolve(nullptr, &rel));
}

TEST("vfs_mount: resolve nullptr rel_path returns nullptr") {
    vfs_mount_init();
    MockFileSystem fs;
    vfs_mount_add("/", &fs);
    ASSERT_NULL(vfs_resolve("/", nullptr));
}

// ============================================================
// 11. g_global_fd_table
// ============================================================

TEST("vfs_mount: g_global_fd_table returns same reference") {
    FDTable& a = g_global_fd_table();
    FDTable& b = g_global_fd_table();
    ASSERT_TRUE(&a == &b);
}

// ============================================================
// 12. Remove + re-add cycle
// ============================================================

TEST("vfs_mount: remove then re-add works") {
    vfs_mount_init();
    MockFileSystem fs1;
    MockFileSystem fs2;

    ASSERT_TRUE(vfs_mount_add("/cycle", &fs1));
    ASSERT_TRUE(vfs_mount_remove("/cycle"));
    ASSERT_TRUE(vfs_mount_add("/cycle", &fs2));

    const char* rel = nullptr;
    FileSystem* result = vfs_resolve("/cycle/file", &rel);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(result == &fs2);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
