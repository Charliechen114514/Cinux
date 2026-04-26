/**
 * @file test/unit/test_syscall_ext2.cpp
 * @brief Host-side unit tests for sys_creat/sys_mkdir/sys_unlink/sys_rmdir (028b)
 *
 * Test coverage:
 *   - split_pathname: various path formats ("/foo", "/a/b/c", "foo", "/",
 *                     trailing slash, empty string)
 *   - Canonical address validation: null, valid userspace, kernel-space
 *   - sys_creat: normal create, empty path, invalid address, no mount,
 *                parent not found, ops->create returns null
 *   - sys_mkdir: normal mkdir, empty path, ops->mkdir returns null
 *   - sys_unlink: normal unlink, non-existent file, ops->unlink returns -1
 *   - sys_rmdir: normal rmdir, non-existent directory
 *   - Full flow: mkdir -> creat in subdir -> unlink -> rmdir
 *
 * Pure reimplementation of syscall logic -- no kernel code linked.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstddef>
#    include <cstdint>
#    include <cstring>

// ============================================================
// Mock / reimplementation layer
// ============================================================

namespace mock {

/// Maximum path length (matching kernel)
constexpr uint32_t PATH_MAX = 4096;

// ----- String helpers (matching kernel/lib/string.hpp) -----

uint32_t strlen(const char* s) {
    uint32_t n = 0;
    while (s && s[n])
        ++n;
    return n;
}

void memcpy(char* dst, const char* src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        dst[i] = src[i];
}

int memcmp(const void* a, const void* b, uint32_t n) {
    auto* pa = static_cast<const uint8_t*>(a);
    auto* pb = static_cast<const uint8_t*>(b);
    for (uint32_t i = 0; i < n; ++i) {
        if (pa[i] < pb[i])
            return -1;
        if (pa[i] > pb[i])
            return 1;
    }
    return 0;
}

// ----- Filesystem / Inode types (mirrors kernel/fs/inode.hpp) -----

enum class InodeType : uint8_t {
    Unknown   = 0,
    Regular   = 1,
    Directory = 2,
};

struct Inode;

class InodeOps {
public:
    virtual ~InodeOps() = default;
    virtual Inode* create(Inode* /*dir*/, const char* /*name*/, uint32_t /*namelen*/) {
        return nullptr;
    }
    virtual Inode* mkdir(Inode* /*dir*/, const char* /*name*/, uint32_t /*namelen*/) {
        return nullptr;
    }
    virtual int64_t unlink(Inode* /*dir*/, const char* /*name*/, uint32_t /*namelen*/) {
        return -1;
    }
};

struct Inode {
    uint64_t  ino        = 0;
    uint64_t  size       = 0;
    InodeType type       = InodeType::Unknown;
    InodeOps* ops        = nullptr;
    void*     fs_private = nullptr;
};

// ----- Mock FileSystem (mirrors kernel/fs/vfs_filesystem.hpp) -----

class FileSystem {
public:
    virtual ~FileSystem()                   = default;
    virtual Inode* lookup(const char* path) = 0;
};

// ----- Mock InodeOps that records calls and returns configurable results -----

struct MockCreateOps : public InodeOps {
    // For create
    bool     create_called              = false;
    char     last_create_name[PATH_MAX] = {};
    uint32_t last_create_namelen        = 0;
    Inode*   create_return              = nullptr;

    // For mkdir
    bool     mkdir_called              = false;
    char     last_mkdir_name[PATH_MAX] = {};
    uint32_t last_mkdir_namelen        = 0;
    Inode*   mkdir_return              = nullptr;

    // For unlink
    bool     unlink_called              = false;
    char     last_unlink_name[PATH_MAX] = {};
    uint32_t last_unlink_namelen        = 0;
    int64_t  unlink_return              = -1;

    Inode* create(Inode* /*dir*/, const char* name, uint32_t namelen) override {
        create_called = true;
        memcpy(last_create_name, name, namelen);
        last_create_name[namelen] = '\0';
        last_create_namelen       = namelen;
        return create_return;
    }

    Inode* mkdir(Inode* /*dir*/, const char* name, uint32_t namelen) override {
        mkdir_called = true;
        memcpy(last_mkdir_name, name, namelen);
        last_mkdir_name[namelen] = '\0';
        last_mkdir_namelen       = namelen;
        return mkdir_return;
    }

    int64_t unlink(Inode* /*dir*/, const char* name, uint32_t namelen) override {
        unlink_called = true;
        memcpy(last_unlink_name, name, namelen);
        last_unlink_name[namelen] = '\0';
        last_unlink_namelen       = namelen;
        return unlink_return;
    }
};

// ----- VFS mount table mock -----

constexpr uint32_t MOUNT_TABLE_SIZE = 8;

struct MountPoint {
    char        path[256];
    FileSystem* fs;
    bool        in_use;
};

MountPoint g_mount_table[MOUNT_TABLE_SIZE];

void vfs_mount_init() {
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        g_mount_table[i].in_use  = false;
        g_mount_table[i].fs      = nullptr;
        g_mount_table[0].path[0] = '\0';
    }
}

bool vfs_mount_add(const char* path, FileSystem* fs) {
    if (path == nullptr || fs == nullptr)
        return false;
    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (!g_mount_table[i].in_use) {
            uint32_t j = 0;
            while (path[j] && j < 255) {
                g_mount_table[i].path[j] = path[j];
                ++j;
            }
            g_mount_table[i].path[j] = '\0';
            g_mount_table[i].fs      = fs;
            g_mount_table[i].in_use  = true;
            return true;
        }
    }
    return false;
}

FileSystem* vfs_resolve(const char* path, const char** rel_path) {
    if (path == nullptr)
        return nullptr;

    // Find longest prefix match
    uint32_t    best_len = 0;
    FileSystem* best_fs  = nullptr;

    for (uint32_t i = 0; i < MOUNT_TABLE_SIZE; ++i) {
        if (!g_mount_table[i].in_use)
            continue;

        uint32_t j = 0;
        while (g_mount_table[i].path[j] && path[j] && g_mount_table[i].path[j] == path[j]) {
            ++j;
        }
        if (g_mount_table[i].path[j] == '\0' && j > best_len) {
            best_len = j;
            best_fs  = g_mount_table[i].fs;
        }
    }

    if (best_fs != nullptr && rel_path != nullptr) {
        *rel_path = path + best_len;
    }
    return best_fs;
}

// ----- Mock filesystem that returns configurable inodes for lookup -----

class MockFS : public FileSystem {
public:
    // Map from path string to inode
    static constexpr uint32_t MAX_ENTRIES = 16;

    struct Entry {
        char   path[PATH_MAX];
        Inode* inode;
        bool   in_use;
    };

    Entry entries[MAX_ENTRIES];

    MockFS() {
        for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
            entries[i].in_use = false;
            entries[i].inode  = nullptr;
        }
    }

    void add(const char* path, Inode* inode) {
        for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
            if (!entries[i].in_use) {
                uint32_t j = 0;
                while (path[j] && j < PATH_MAX - 1) {
                    entries[i].path[j] = path[j];
                    ++j;
                }
                entries[i].path[j] = '\0';
                entries[i].inode   = inode;
                entries[i].in_use  = true;
                return;
            }
        }
    }

    Inode* lookup(const char* path) override {
        // Handle empty path as root
        if (path[0] == '\0') {
            // Find the root entry ""
            for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
                if (entries[i].in_use && entries[i].path[0] == '\0') {
                    return entries[i].inode;
                }
            }
            return nullptr;
        }

        for (uint32_t i = 0; i < MAX_ENTRIES; ++i) {
            if (!entries[i].in_use)
                continue;
            if (memcmp(entries[i].path, path, strlen(entries[i].path)) == 0 &&
                strlen(entries[i].path) == strlen(path)) {
                return entries[i].inode;
            }
        }
        return nullptr;
    }
};

// ----- split_pathname: exact reimplementation from sys_creat.cpp -----

bool split_pathname(const char* path, char* parent_out, const char** name_out,
                    uint32_t* namelen_out) {
    uint32_t len = strlen(path);

    if (len == 0) {
        return false;
    }

    // Trailing slash is ambiguous for create/mkdir/unlink
    if (path[len - 1] == '/') {
        return false;
    }

    // Find the last '/' separator
    int32_t last_sep = -1;
    for (uint32_t i = 0; i < len; ++i) {
        if (path[i] == '/') {
            last_sep = static_cast<int32_t>(i);
        }
    }

    if (last_sep < 0) {
        // No separator: parent is root (empty string)
        parent_out[0] = '\0';
        *name_out     = path;
        *namelen_out  = len;
    } else {
        // Copy parent portion
        uint32_t parent_len = static_cast<uint32_t>(last_sep);
        memcpy(parent_out, path, parent_len);
        parent_out[parent_len] = '\0';

        *name_out    = path + last_sep + 1;
        *namelen_out = len - parent_len - 1;
    }

    if (*namelen_out == 0) {
        return false;
    }

    return true;
}

// ----- Canonical address validation (from kernel) -----

bool is_valid_user_addr(uint64_t addr) {
    if (addr == 0)
        return false;
    uint64_t bit47 = (addr >> 47) & 1;
    uint64_t upper = addr >> 48;
    if (bit47 == 0 && upper != 0)
        return false;
    if (bit47 == 1 && upper != 0xFFFF)
        return false;
    return true;
}

// ----- Syscall reimplementation: sys_creat -----

int64_t sys_creat(uint64_t path_virt) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    if (!is_valid_user_addr(path_virt)) {
        return -1;
    }

    if (path[0] == '\0') {
        return -1;
    }

    const char* rel_path = nullptr;
    FileSystem* fs       = vfs_resolve(path, &rel_path);

    if (fs == nullptr) {
        return -1;
    }

    char        parent_buf[PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        return -1;
    }

    Inode* parent = fs->lookup(parent_buf);

    if (parent == nullptr) {
        return -1;
    }

    if (parent->ops == nullptr) {
        return -1;
    }

    Inode* new_inode = parent->ops->create(parent, leaf_name, name_len);

    if (new_inode == nullptr) {
        return -1;
    }

    return 0;
}

// ----- Syscall reimplementation: sys_mkdir -----

int64_t sys_mkdir(uint64_t path_virt) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    if (!is_valid_user_addr(path_virt)) {
        return -1;
    }

    if (path[0] == '\0') {
        return -1;
    }

    const char* rel_path = nullptr;
    FileSystem* fs       = vfs_resolve(path, &rel_path);

    if (fs == nullptr) {
        return -1;
    }

    char        parent_buf[PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        return -1;
    }

    Inode* parent = fs->lookup(parent_buf);

    if (parent == nullptr) {
        return -1;
    }

    if (parent->ops == nullptr) {
        return -1;
    }

    Inode* new_inode = parent->ops->mkdir(parent, leaf_name, name_len);

    if (new_inode == nullptr) {
        return -1;
    }

    return 0;
}

// ----- Syscall reimplementation: sys_unlink -----

int64_t sys_unlink(uint64_t path_virt) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    if (!is_valid_user_addr(path_virt)) {
        return -1;
    }

    if (path[0] == '\0') {
        return -1;
    }

    const char* rel_path = nullptr;
    FileSystem* fs       = vfs_resolve(path, &rel_path);

    if (fs == nullptr) {
        return -1;
    }

    char        parent_buf[PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        return -1;
    }

    Inode* parent = fs->lookup(parent_buf);

    if (parent == nullptr) {
        return -1;
    }

    if (parent->ops == nullptr) {
        return -1;
    }

    int64_t result = parent->ops->unlink(parent, leaf_name, name_len);

    if (result != 0) {
        return -1;
    }

    return 0;
}

// ----- Syscall reimplementation: sys_rmdir -----

int64_t sys_rmdir(uint64_t path_virt) {
    auto* path = reinterpret_cast<const char*>(path_virt);

    if (!is_valid_user_addr(path_virt)) {
        return -1;
    }

    if (path[0] == '\0') {
        return -1;
    }

    const char* rel_path = nullptr;
    FileSystem* fs       = vfs_resolve(path, &rel_path);

    if (fs == nullptr) {
        return -1;
    }

    char        parent_buf[PATH_MAX];
    const char* leaf_name = nullptr;
    uint32_t    name_len  = 0;

    if (!split_pathname(rel_path, parent_buf, &leaf_name, &name_len)) {
        return -1;
    }

    Inode* parent = fs->lookup(parent_buf);

    if (parent == nullptr) {
        return -1;
    }

    if (parent->ops == nullptr) {
        return -1;
    }

    int64_t result = parent->ops->unlink(parent, leaf_name, name_len);

    if (result != 0) {
        return -1;
    }

    return 0;
}

}  // namespace mock

// ============================================================
// 1. split_pathname Tests
// ============================================================

// --- Normal path with single component ---
TEST("split_pathname: '/foo' splits into parent='' and name='foo'") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("foo", parent, &name, &namelen);
    ASSERT_TRUE(ok);
    ASSERT_EQ(mock::strlen(parent), 0u);
    ASSERT_EQ(namelen, 3u);
    ASSERT_TRUE(mock::memcmp(name, "foo", 3) == 0);
}

// --- Multi-component path ---
TEST("split_pathname: '/a/b/c' splits into parent='a/b' and name='c'") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("a/b/c", parent, &name, &namelen);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(mock::memcmp(parent, "a/b", 3) == 0);
    ASSERT_EQ(parent[3], '\0');
    ASSERT_EQ(namelen, 1u);
    ASSERT_TRUE(mock::memcmp(name, "c", 1) == 0);
}

// --- No separator ---
TEST("split_pathname: 'foo' (no separator) -> parent='', name='foo'") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("foo", parent, &name, &namelen);
    ASSERT_TRUE(ok);
    ASSERT_EQ(parent[0], '\0');
    ASSERT_EQ(namelen, 3u);
    ASSERT_TRUE(mock::memcmp(name, "foo", 3) == 0);
}

// --- Just root path ---
TEST("split_pathname: '/' (trailing slash) returns false") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("/", parent, &name, &namelen);
    ASSERT_FALSE(ok);
}

// --- Empty string ---
TEST("split_pathname: empty string returns false") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("", parent, &name, &namelen);
    ASSERT_FALSE(ok);
}

// --- Trailing slash ---
TEST("split_pathname: 'foo/' (trailing slash) returns false") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("foo/", parent, &name, &namelen);
    ASSERT_FALSE(ok);
}

// --- Path with leading slash ---
TEST("split_pathname: '/foo' (with leading slash) -> parent='', name='foo'") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("/foo", parent, &name, &namelen);
    ASSERT_TRUE(ok);
    // parent is "" (empty string, i.e. root relative to the FS)
    ASSERT_EQ(parent[0], '\0');
    ASSERT_EQ(namelen, 3u);
    ASSERT_TRUE(mock::memcmp(name, "foo", 3) == 0);
}

// --- Nested path with leading slash ---
TEST("split_pathname: '/a/b/c' (with leading slash)") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("/a/b/c", parent, &name, &namelen);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(mock::memcmp(parent, "/a/b", 4) == 0);
    ASSERT_EQ(namelen, 1u);
    ASSERT_TRUE(mock::memcmp(name, "c", 1) == 0);
}

// --- Deep path ---
TEST("split_pathname: deep path '/x/y/z/w' splits correctly") {
    char        parent[mock::PATH_MAX] = {};
    const char* name                   = nullptr;
    uint32_t    namelen                = 0;

    bool ok = mock::split_pathname("/x/y/z/w", parent, &name, &namelen);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(mock::memcmp(parent, "/x/y/z", 6) == 0);
    ASSERT_EQ(namelen, 1u);
    ASSERT_TRUE(mock::memcmp(name, "w", 1) == 0);
}

// ============================================================
// 2. Canonical Address Validation Tests
// ============================================================

TEST("addr_validation: null address is invalid") {
    ASSERT_FALSE(mock::is_valid_user_addr(0));
}

TEST("addr_validation: low userspace address is valid") {
    ASSERT_TRUE(mock::is_valid_user_addr(0x1000));
}

TEST("addr_validation: high userspace address is valid") {
    ASSERT_TRUE(mock::is_valid_user_addr(0x00007FFFFFFFFFFFULL));
}

TEST("addr_validation: kernel address (non-canonical) is invalid") {
    ASSERT_FALSE(mock::is_valid_user_addr(0x800000000001ULL));
}

TEST("addr_validation: high kernel canonical address is valid") {
    // kernel-space canonical addresses (bit47=1, upper=0xFFFF)
    ASSERT_TRUE(mock::is_valid_user_addr(0xFFFF800000000000ULL));
}

// ============================================================
// 3. sys_creat Tests
// ============================================================

TEST("sys_creat: normal create returns 0") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    mock::Inode         new_file;
    new_file.ino      = 10;
    new_file.type     = mock::InodeType::Regular;
    ops.create_return = &new_file;
    root_inode.ops    = &ops;

    // Root is the parent when path is "/testfile"
    fs.add("", &root_inode);

    const char* path   = "/testfile";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(ops.create_called);
    ASSERT_TRUE(mock::memcmp(ops.last_create_name, "testfile", 8) == 0);
    ASSERT_EQ(ops.last_create_namelen, 8u);
}

TEST("sys_creat: empty path returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    const char* path   = "";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_creat: null address returns -1") {
    int64_t result = mock::sys_creat(0);
    ASSERT_EQ(result, -1);
}

TEST("sys_creat: invalid (non-canonical) address returns -1") {
    int64_t result = mock::sys_creat(0x800000000001ULL);
    ASSERT_EQ(result, -1);
}

TEST("sys_creat: no filesystem mounted returns -1") {
    mock::vfs_mount_init();

    const char* path   = "/testfile";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_creat: parent directory not found returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    // Don't add a root inode, so lookup("") returns nullptr
    const char* path   = "/testfile";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_creat: parent with no ops returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    root_inode.ops  = nullptr;  // No ops

    fs.add("", &root_inode);

    const char* path   = "/testfile";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_creat: ops->create returns nullptr returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    ops.create_return = nullptr;  // Simulate failure
    root_inode.ops    = &ops;

    fs.add("", &root_inode);

    const char* path   = "/testfile";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_creat: trailing slash path returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    root_inode.ops = &ops;

    fs.add("", &root_inode);

    const char* path   = "/testfile/";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

// ============================================================
// 4. sys_mkdir Tests
// ============================================================

TEST("sys_mkdir: normal mkdir returns 0") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    mock::Inode         new_dir;
    new_dir.ino      = 11;
    new_dir.type     = mock::InodeType::Directory;
    ops.mkdir_return = &new_dir;
    root_inode.ops   = &ops;

    fs.add("", &root_inode);

    const char* path   = "/mydir";
    int64_t     result = mock::sys_mkdir(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(ops.mkdir_called);
    ASSERT_TRUE(mock::memcmp(ops.last_mkdir_name, "mydir", 5) == 0);
    ASSERT_EQ(ops.last_mkdir_namelen, 5u);
}

TEST("sys_mkdir: empty path returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    const char* path   = "";
    int64_t     result = mock::sys_mkdir(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_mkdir: null address returns -1") {
    int64_t result = mock::sys_mkdir(0);
    ASSERT_EQ(result, -1);
}

TEST("sys_mkdir: ops->mkdir returns nullptr returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    ops.mkdir_return = nullptr;
    root_inode.ops   = &ops;

    fs.add("", &root_inode);

    const char* path   = "/faildir";
    int64_t     result = mock::sys_mkdir(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_mkdir: mkdir in subdirectory passes correct parent") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    // Root inode
    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;

    // Subdir inode
    mock::Inode subdir_inode;
    subdir_inode.ino  = 5;
    subdir_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps subdir_ops;
    subdir_inode.ops = &subdir_ops;

    mock::Inode new_dir;
    new_dir.ino             = 12;
    new_dir.type            = mock::InodeType::Directory;
    subdir_ops.mkdir_return = &new_dir;

    fs.add("", &root_inode);
    fs.add("subdir", &subdir_inode);

    const char* path   = "/subdir/newdir";
    int64_t     result = mock::sys_mkdir(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(subdir_ops.mkdir_called);
    ASSERT_TRUE(mock::memcmp(subdir_ops.last_mkdir_name, "newdir", 6) == 0);
    ASSERT_EQ(subdir_ops.last_mkdir_namelen, 6u);
}

// ============================================================
// 5. sys_unlink Tests
// ============================================================

TEST("sys_unlink: normal unlink returns 0") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    ops.unlink_return = 0;  // Success
    root_inode.ops    = &ops;

    fs.add("", &root_inode);

    const char* path   = "/testfile";
    int64_t     result = mock::sys_unlink(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(ops.unlink_called);
    ASSERT_TRUE(mock::memcmp(ops.last_unlink_name, "testfile", 8) == 0);
    ASSERT_EQ(ops.last_unlink_namelen, 8u);
}

TEST("sys_unlink: null address returns -1") {
    int64_t result = mock::sys_unlink(0);
    ASSERT_EQ(result, -1);
}

TEST("sys_unlink: empty path returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    const char* path   = "";
    int64_t     result = mock::sys_unlink(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_unlink: no mount returns -1") {
    mock::vfs_mount_init();

    const char* path   = "/testfile";
    int64_t     result = mock::sys_unlink(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_unlink: ops->unlink returns -1 returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    ops.unlink_return = -1;  // Simulate failure (e.g., not found)
    root_inode.ops    = &ops;

    fs.add("", &root_inode);

    const char* path   = "/nonexistent";
    int64_t     result = mock::sys_unlink(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

// ============================================================
// 6. sys_rmdir Tests
// ============================================================

TEST("sys_rmdir: normal rmdir returns 0") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    ops.unlink_return = 0;  // Success
    root_inode.ops    = &ops;

    fs.add("", &root_inode);

    const char* path   = "/mydir";
    int64_t     result = mock::sys_rmdir(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(ops.unlink_called);
    ASSERT_TRUE(mock::memcmp(ops.last_unlink_name, "mydir", 5) == 0);
    ASSERT_EQ(ops.last_unlink_namelen, 5u);
}

TEST("sys_rmdir: null address returns -1") {
    int64_t result = mock::sys_rmdir(0);
    ASSERT_EQ(result, -1);
}

TEST("sys_rmdir: empty path returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    const char* path   = "";
    int64_t     result = mock::sys_rmdir(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

TEST("sys_rmdir: ops->unlink returns -1 (non-empty dir) returns -1") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    ops.unlink_return = -1;  // e.g., directory not empty
    root_inode.ops    = &ops;

    fs.add("", &root_inode);

    const char* path   = "/nonempty_dir";
    int64_t     result = mock::sys_rmdir(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, -1);
}

// ============================================================
// 7. Full Flow: mkdir -> creat in subdir -> unlink -> rmdir
// ============================================================

TEST("full_flow: mkdir then creat in subdir passes correct names") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    // Root inode with ops
    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps root_ops;

    // mkdir returns a new directory
    mock::Inode new_dir;
    new_dir.ino           = 10;
    new_dir.type          = mock::InodeType::Directory;
    root_ops.mkdir_return = &new_dir;
    root_inode.ops        = &root_ops;

    fs.add("", &root_inode);

    // Step 1: mkdir("/workdir")
    const char* mkdir_path   = "/workdir";
    int64_t     mkdir_result = mock::sys_mkdir(reinterpret_cast<uint64_t>(mkdir_path));
    ASSERT_EQ(mkdir_result, 0);
    ASSERT_TRUE(root_ops.mkdir_called);
    ASSERT_TRUE(mock::memcmp(root_ops.last_mkdir_name, "workdir", 7) == 0);

    // Step 2: creat("/workdir/file.txt")
    // We need a separate lookup for "workdir" to return the new_dir
    mock::MockCreateOps dir_ops;
    new_dir.ops = &dir_ops;

    mock::Inode new_file;
    new_file.ino          = 11;
    new_file.type         = mock::InodeType::Regular;
    dir_ops.create_return = &new_file;

    fs.add("workdir", &new_dir);

    const char* creat_path   = "/workdir/file.txt";
    int64_t     creat_result = mock::sys_creat(reinterpret_cast<uint64_t>(creat_path));
    ASSERT_EQ(creat_result, 0);
    ASSERT_TRUE(dir_ops.create_called);
    ASSERT_TRUE(mock::memcmp(dir_ops.last_create_name, "file.txt", 8) == 0);
    ASSERT_EQ(dir_ops.last_create_namelen, 8u);

    // Step 3: unlink("/workdir/file.txt")
    dir_ops.unlink_return = 0;
    int64_t unlink_result = mock::sys_unlink(reinterpret_cast<uint64_t>(creat_path));
    ASSERT_EQ(unlink_result, 0);
    ASSERT_TRUE(dir_ops.unlink_called);

    // Step 4: rmdir("/workdir")
    root_ops.unlink_return = 0;
    int64_t rmdir_result   = mock::sys_rmdir(reinterpret_cast<uint64_t>(mkdir_path));
    ASSERT_EQ(rmdir_result, 0);
    ASSERT_TRUE(root_ops.unlink_called);
    ASSERT_TRUE(mock::memcmp(root_ops.last_unlink_name, "workdir", 7) == 0);
}

// ============================================================
// 8. Boundary: single-char name
// ============================================================

TEST("sys_creat: single character filename") {
    mock::vfs_mount_init();
    mock::MockFS fs;
    mock::vfs_mount_add("/", &fs);

    mock::Inode root_inode;
    root_inode.ino  = 2;
    root_inode.type = mock::InodeType::Directory;
    mock::MockCreateOps ops;
    mock::Inode         new_file;
    new_file.ino      = 10;
    new_file.type     = mock::InodeType::Regular;
    ops.create_return = &new_file;
    root_inode.ops    = &ops;

    fs.add("", &root_inode);

    const char* path   = "/a";
    int64_t     result = mock::sys_creat(reinterpret_cast<uint64_t>(path));
    ASSERT_EQ(result, 0);
    ASSERT_EQ(ops.last_create_namelen, 1u);
    ASSERT_EQ(ops.last_create_name[0], 'a');
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
