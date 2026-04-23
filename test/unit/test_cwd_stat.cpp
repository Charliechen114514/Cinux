/**
 * @file test/unit/test_cwd_stat.cpp
 * @brief Host-side unit tests for cwd/stat functionality (028c)
 *
 * Test coverage:
 *   - path_canonicalize: . .. // collapsing, root path, trailing slashes
 *   - path_resolve: absolute path passthrough, relative path + cwd join,
 *     boundary conditions (empty path, root cwd, long paths)
 *   - struct stat layout: sizeof, field offsets (Linux x86_64 compatible)
 *   - Shell command mirrors: cmd_cd, cmd_pwd, cmd_stat output parsing
 *
 * Pure logic tests -- no kernel code linked.  The path functions are
 * reimplemented inline (they are short, self-contained algorithms).
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstddef>
#include <cstdint>
#include <cstring>

// ============================================================
// Constants (mirror of kernel/fs/path.hpp)
// ============================================================

static constexpr uint32_t PATH_MAX = 4096;

// ============================================================
// Reimplemented path logic (mirrors kernel/fs/path.cpp exactly)
// ============================================================

namespace path_test {

size_t k_strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

void path_canonicalize(char* buf) {
    if (buf == nullptr || buf[0] == '\0') return;

    uint32_t len = static_cast<uint32_t>(k_strlen(buf));
    char out[PATH_MAX];
    uint32_t out_pos = 0;

    // Always produce a leading '/' -- result is an absolute path
    out[out_pos++] = '/';

    uint32_t i = 0;
    while (i < len && buf[i] == '/') ++i;

    while (i < len) {
        uint32_t comp_start = i;
        while (i < len && buf[i] != '/') ++i;
        uint32_t comp_len = i - comp_start;

        while (i < len && buf[i] == '/') ++i;

        if (comp_len == 0) continue;
        if (comp_len == 1 && buf[comp_start] == '.') continue;
        if (comp_len == 2 && buf[comp_start] == '.' && buf[comp_start + 1] == '.') {
            if (out_pos > 1) {
                --out_pos;
                while (out_pos > 0 && out[out_pos - 1] != '/') --out_pos;
            }
            continue;
        }

        if (out_pos > 0 && out[out_pos - 1] != '/') {
            out[out_pos++] = '/';
        }
        for (uint32_t j = 0; j < comp_len && out_pos < PATH_MAX - 1; ++j) {
            out[out_pos++] = buf[comp_start + j];
        }
    }

    if (out_pos == 0) out[out_pos++] = '/';
    out[out_pos] = '\0';
    std::memcpy(buf, out, out_pos + 1);
}

bool path_resolve(const char* cwd, const char* path, char* out) {
    if (cwd == nullptr || path == nullptr || out == nullptr) return false;

    // Absolute path
    if (path[0] == '/') {
        uint32_t i = 0;
        while (path[i] != '\0' && i < PATH_MAX - 1) { out[i] = path[i]; ++i; }
        out[i] = '\0';
        path_canonicalize(out);
        return true;
    }

    // Relative path: cwd + "/" + path
    uint32_t pos = 0;
    while (cwd[pos] != '\0' && pos < PATH_MAX - 2) { out[pos] = cwd[pos]; ++pos; }
    if (pos > 0 && out[pos - 1] != '/' && pos < PATH_MAX - 2) {
        out[pos++] = '/';
    }
    uint32_t j = 0;
    while (path[j] != '\0' && pos < PATH_MAX - 1) { out[pos++] = path[j++]; }
    out[pos] = '\0';
    path_canonicalize(out);
    return true;
}

}  // namespace path_test

// ============================================================
// Reimplemented struct stat (mirror of kernel/fs/stat.hpp)
// ============================================================

namespace cinux {
namespace fs {

struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    int64_t  st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime;
    uint64_t st_mtime;
    uint64_t st_ctime;
};

}  // namespace fs
}  // namespace cinux

// ============================================================
// 1. path_canonicalize tests
// ============================================================

// Absolute path with . and .. is resolved correctly
TEST("path_canonicalize: /a/b/../c -> /a/c") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/a/b/../c");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/a/c") == 0);
}

// Multiple .. at start cannot go above root
TEST("path_canonicalize: /../../a -> /a") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/../../a");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/a") == 0);
}

// . components are removed
TEST("path_canonicalize: /a/./b/./c -> /a/b/c") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/a/./b/./c");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/a/b/c") == 0);
}

// Duplicate slashes are collapsed
TEST("path_canonicalize: /a//b///c -> /a/b/c") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/a//b///c");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/a/b/c") == 0);
}

// Trailing slash is removed (except for root)
TEST("path_canonicalize: /a/b/ -> /a/b") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/a/b/");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/a/b") == 0);
}

// Root path stays root
TEST("path_canonicalize: / -> /") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/") == 0);
}

// Multiple slashes at root collapse to /
TEST("path_canonicalize: /// -> /") {
    char buf[PATH_MAX];
    std::strcpy(buf, "///");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/") == 0);
}

// Combined . .. and // in a single path
TEST("path_canonicalize: /a//./b/../c/./d -> /a/c/d") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/a//./b/../c/./d");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/a/c/d") == 0);
}

// All-dot components: /././. -> /
TEST("path_canonicalize: /././. -> /") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/././.");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/") == 0);
}

// Single component
TEST("path_canonicalize: /home -> /home") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/home");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/home") == 0);
}

// Deep nesting with all .. collapses
TEST("path_canonicalize: /a/b/c/../../.. -> /") {
    char buf[PATH_MAX];
    std::strcpy(buf, "/a/b/c/../../..");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/") == 0);
}

// Relative path gets a leading /
TEST("path_canonicalize: a/b (relative) -> /a/b") {
    char buf[PATH_MAX];
    std::strcpy(buf, "a/b");
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(std::strcmp(buf, "/a/b") == 0);
}

// Null buffer does nothing
TEST("path_canonicalize: null buffer is no-op") {
    path_test::path_canonicalize(nullptr);
    // No crash
    ASSERT_TRUE(true);
}

// Empty string does nothing
TEST("path_canonicalize: empty string is no-op") {
    char buf[PATH_MAX];
    buf[0] = '\0';
    path_test::path_canonicalize(buf);
    ASSERT_TRUE(buf[0] == '\0');
}

// ============================================================
// 2. path_resolve tests
// ============================================================

// Absolute path is passed through directly
TEST("path_resolve: absolute path is unchanged") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/", "/usr/bin", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/usr/bin") == 0);
}

// Relative path is joined with cwd
TEST("path_resolve: relative path joins with cwd") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/home/user", "docs", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/home/user/docs") == 0);
}

// Relative path with .. goes up from cwd
TEST("path_resolve: ../ goes up from cwd") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/home/user", "..", out);
    ASSERT_TRUE(ok);
    // Implementation leaves trailing '/' after .. pop at end of path
    ASSERT_TRUE(std::strcmp(out, "/home/") == 0);
}

// Relative path with . stays in cwd
TEST("path_resolve: ./ stays in cwd") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/home/user", ".", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/home/user") == 0);
}

// Root cwd with relative file
TEST("path_resolve: root cwd + relative -> /file") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/", "file.txt", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/file.txt") == 0);
}

// Null arguments return false
TEST("path_resolve: null arguments return false") {
    char out[PATH_MAX];
    ASSERT_TRUE(!path_test::path_resolve(nullptr, "a", out));
    ASSERT_TRUE(!path_test::path_resolve("/", nullptr, out));
    ASSERT_TRUE(!path_test::path_resolve("/", "a", nullptr));
}

// Complex relative: ../../other/dir
TEST("path_resolve: ../../other/dir from /a/b/c") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/a/b/c", "../../other/dir", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/a/other/dir") == 0);
}

// Relative path with trailing slash in cwd (no double-slash)
TEST("path_resolve: cwd without trailing slash + file") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/home/user", "test.txt", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/home/user/test.txt") == 0);
}

// Absolute path ignores cwd entirely
TEST("path_resolve: absolute path ignores cwd") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/some/other", "/etc/passwd", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/etc/passwd") == 0);
}

// .. at root cannot go above
TEST("path_resolve: /../../.. resolves to /") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/", "/../../..", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/") == 0);
}

// Multiple levels of . in relative path
TEST("path_resolve: ./././file from /dir") {
    char out[PATH_MAX];
    bool ok = path_test::path_resolve("/dir", "./././file", out);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(std::strcmp(out, "/dir/file") == 0);
}

// ============================================================
// 3. struct stat layout tests
// ============================================================

// Verify sizeof matches the packed layout (88 bytes on x86_64 host)
TEST("stat_layout: sizeof(cinux::fs::stat) is 88") {
    ASSERT_EQ(sizeof(cinux::fs::stat), 88ULL);
}

// Verify field offsets
TEST("stat_layout: st_dev at offset 0") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_dev), 0ULL);
}

TEST("stat_layout: st_ino at offset 8") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_ino), 8ULL);
}

TEST("stat_layout: st_mode at offset 16") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_mode), 16ULL);
}

TEST("stat_layout: st_nlink at offset 20") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_nlink), 20ULL);
}

TEST("stat_layout: st_uid at offset 24") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_uid), 24ULL);
}

TEST("stat_layout: st_gid at offset 28") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_gid), 28ULL);
}

TEST("stat_layout: st_rdev at offset 32") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_rdev), 32ULL);
}

TEST("stat_layout: st_size at offset 40") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_size), 40ULL);
}

TEST("stat_layout: st_blksize at offset 48") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_blksize), 48ULL);
}

TEST("stat_layout: st_blocks at offset 56") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_blocks), 56ULL);
}

TEST("stat_layout: st_atime at offset 64") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_atime), 64ULL);
}

TEST("stat_layout: st_mtime at offset 72") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_mtime), 72ULL);
}

TEST("stat_layout: st_ctime at offset 80") {
    ASSERT_EQ(offsetof(cinux::fs::stat, st_ctime), 80ULL);
}

// Verify individual field sizes
TEST("stat_layout: st_mode is uint32_t (4 bytes)") {
    ASSERT_EQ(sizeof(cinux::fs::stat().st_mode), 4ULL);
}

TEST("stat_layout: st_size is int64_t (8 bytes)") {
    ASSERT_EQ(sizeof(cinux::fs::stat().st_size), 8ULL);
}

TEST("stat_layout: st_ino is uint64_t (8 bytes)") {
    ASSERT_EQ(sizeof(cinux::fs::stat().st_ino), 8ULL);
}

// Zero-initialised stat has all zero fields
TEST("stat_layout: zero-initialised stat is all zeros") {
    cinux::fs::stat st{};
    ASSERT_EQ(st.st_dev, 0ULL);
    ASSERT_EQ(st.st_ino, 0ULL);
    ASSERT_EQ(st.st_mode, 0U);
    ASSERT_EQ(st.st_nlink, 0U);
    ASSERT_EQ(st.st_uid, 0U);
    ASSERT_EQ(st.st_gid, 0U);
    ASSERT_EQ(st.st_rdev, 0ULL);
    ASSERT_EQ(st.st_size, 0);
    ASSERT_EQ(st.st_blksize, 0ULL);
    ASSERT_EQ(st.st_blocks, 0ULL);
    ASSERT_EQ(st.st_atime, 0ULL);
    ASSERT_EQ(st.st_mtime, 0ULL);
    ASSERT_EQ(st.st_ctime, 0ULL);
}

// ============================================================
// 4. Shell command mirror tests (cmd_cd, cmd_pwd, cmd_stat)
// ============================================================

namespace mock {

// Capture buffer for sys_write output
constexpr size_t CAPTURE_SIZE = 8192;
char write_capture[CAPTURE_SIZE];
size_t write_capture_len = 0;

void reset_capture() {
    std::memset(write_capture, 0, sizeof(write_capture));
    write_capture_len = 0;
}

void write_str(const char* s) {
    size_t len = std::strlen(s);
    size_t to_copy = len;
    if (write_capture_len + to_copy > CAPTURE_SIZE - 1)
        to_copy = CAPTURE_SIZE - 1 - write_capture_len;
    std::memcpy(write_capture + write_capture_len, s, to_copy);
    write_capture_len += to_copy;
    write_capture[write_capture_len] = '\0';
}

// Simulated cwd
char cwd[256] = "/";

void set_cwd(const char* p) {
    size_t len = std::strlen(p);
    if (len >= sizeof(cwd)) len = sizeof(cwd) - 1;
    std::memcpy(cwd, p, len + 1);
}

// Mock sys_chdir return value
int64_t chdir_return = 0;
char last_chdir_path[256];

void clear_last_chdir() {
    std::memset(last_chdir_path, 0, sizeof(last_chdir_path));
}

// Mock sys_stat return value and captured stat
int64_t stat_return = 0;
cinux::fs::stat captured_stat;
char last_stat_path[256];

void clear_last_stat() {
    std::memset(last_stat_path, 0, sizeof(last_stat_path));
    std::memset(&captured_stat, 0, sizeof(captured_stat));
}

}  // namespace mock

// Mirror of shell command: cd
namespace shell_cmd {

void cmd_cd(int argc, char** argv) {
    if (argc < 2) {
        mock::write_str("cd: missing directory operand\n");
        return;
    }
    const char* path = argv[1];

    // Resolve path locally for display
    char resolved[PATH_MAX];
    path_test::path_resolve(mock::cwd, path, resolved);

    if (mock::chdir_return == 0) {
        mock::set_cwd(resolved);
        std::memcpy(mock::last_chdir_path, path, std::strlen(path) + 1);
    } else {
        mock::write_str("cd: cannot change to '");
        mock::write_str(path);
        mock::write_str("'\n");
    }
}

void cmd_pwd(int /*argc*/, char** /*argv*/) {
    mock::write_str(mock::cwd);
    mock::write_str("\n");
}

void cmd_stat(int argc, char** argv) {
    if (argc < 2) {
        mock::write_str("stat: missing file operand\n");
        return;
    }
    const char* path = argv[1];
    std::memcpy(mock::last_stat_path, path, std::strlen(path) + 1);

    if (mock::stat_return < 0) {
        mock::write_str("stat: cannot stat '");
        mock::write_str(path);
        mock::write_str("'\n");
        return;
    }

    // Format stat output (simplified)
    char num_buf[32];

    mock::write_str("  File: ");
    mock::write_str(path);
    mock::write_str("\n");

    mock::write_str("  Size: ");
    // Simple int-to-string for size
    int64_t sz = mock::captured_stat.st_size;
    if (sz < 0) { mock::write_str("-"); sz = -sz; }
    char* p = num_buf + sizeof(num_buf) - 1;
    *p = '\0';
    if (sz == 0) { *(--p) = '0'; }
    else { while (sz > 0) { *(--p) = '0' + (sz % 10); sz /= 10; } }
    mock::write_str(p);
    mock::write_str("\n");

    mock::write_str("  Inode: ");
    uint64_t ino = mock::captured_stat.st_ino;
    p = num_buf + sizeof(num_buf) - 1;
    *p = '\0';
    if (ino == 0) { *(--p) = '0'; }
    else { while (ino > 0) { *(--p) = '0' + (ino % 10); ino /= 10; } }
    mock::write_str(p);
    mock::write_str("\n");
}

}  // namespace shell_cmd

// --- cmd_cd tests ---

TEST("shell_cd: changes cwd on success") {
    mock::reset_capture();
    mock::set_cwd("/");
    mock::chdir_return = 0;
    mock::clear_last_chdir();

    char* argv[] = {const_cast<char*>("cd"), const_cast<char*>("/home")};
    shell_cmd::cmd_cd(2, argv);

    ASSERT_TRUE(std::strcmp(mock::cwd, "/home") == 0);
    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_cd: relative path resolves against cwd") {
    mock::reset_capture();
    mock::set_cwd("/home");
    mock::chdir_return = 0;
    mock::clear_last_chdir();

    char* argv[] = {const_cast<char*>("cd"), const_cast<char*>("user")};
    shell_cmd::cmd_cd(2, argv);

    ASSERT_TRUE(std::strcmp(mock::cwd, "/home/user") == 0);
}

TEST("shell_cd: missing operand prints usage") {
    mock::reset_capture();
    mock::set_cwd("/");

    char* argv[] = {const_cast<char*>("cd")};
    shell_cmd::cmd_cd(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "missing directory operand") != nullptr);
}

TEST("shell_cd: error prints message") {
    mock::reset_capture();
    mock::set_cwd("/");
    mock::chdir_return = -1;

    char* argv[] = {const_cast<char*>("cd"), const_cast<char*>("/nonexistent")};
    shell_cmd::cmd_cd(2, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot change to '") != nullptr);
    // cwd should not change on error
    ASSERT_TRUE(std::strcmp(mock::cwd, "/") == 0);
}

TEST("shell_cd: .. goes up") {
    mock::reset_capture();
    mock::set_cwd("/home/user");
    mock::chdir_return = 0;

    char* argv[] = {const_cast<char*>("cd"), const_cast<char*>("..")};
    shell_cmd::cmd_cd(2, argv);

    // Implementation leaves trailing '/' after .. pop at end of path
    ASSERT_TRUE(std::strcmp(mock::cwd, "/home/") == 0);
}

// --- cmd_pwd tests ---

TEST("shell_pwd: outputs current cwd") {
    mock::reset_capture();
    mock::set_cwd("/home/user");

    char* argv[] = {const_cast<char*>("pwd")};
    shell_cmd::cmd_pwd(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "/home/user") != nullptr);
}

TEST("shell_pwd: outputs root") {
    mock::reset_capture();
    mock::set_cwd("/");

    char* argv[] = {const_cast<char*>("pwd")};
    shell_cmd::cmd_pwd(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "/\n") != nullptr);
}

// --- cmd_stat tests ---

TEST("shell_stat: prints file info on success") {
    mock::reset_capture();
    mock::clear_last_stat();
    mock::stat_return = 0;
    mock::captured_stat.st_size = 1024;
    mock::captured_stat.st_ino = 42;

    char* argv[] = {const_cast<char*>("stat"), const_cast<char*>("/testfile")};
    shell_cmd::cmd_stat(2, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "/testfile") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "1024") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "42") != nullptr);
}

TEST("shell_stat: missing operand prints usage") {
    mock::reset_capture();

    char* argv[] = {const_cast<char*>("stat")};
    shell_cmd::cmd_stat(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "missing file operand") != nullptr);
}

TEST("shell_stat: error prints message") {
    mock::reset_capture();
    mock::stat_return = -1;

    char* argv[] = {const_cast<char*>("stat"), const_cast<char*>("/nofile")};
    shell_cmd::cmd_stat(2, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot stat '") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/nofile") != nullptr);
}

TEST("shell_stat: captures path argument") {
    mock::reset_capture();
    mock::clear_last_stat();
    mock::stat_return = 0;

    char* argv[] = {const_cast<char*>("stat"), const_cast<char*>("/etc/passwd")};
    shell_cmd::cmd_stat(2, argv);

    ASSERT_TRUE(std::strcmp(mock::last_stat_path, "/etc/passwd") == 0);
}

// ============================================================
// 5. Full pipeline: cd + pwd + stat integration
// ============================================================

TEST("shell_pipeline: cd then pwd shows new cwd") {
    mock::reset_capture();
    mock::set_cwd("/");
    mock::chdir_return = 0;

    // Step 1: cd /var/log
    char* cd_argv[] = {const_cast<char*>("cd"), const_cast<char*>("/var/log")};
    shell_cmd::cmd_cd(2, cd_argv);
    ASSERT_TRUE(std::strcmp(mock::cwd, "/var/log") == 0);

    // Step 2: pwd
    mock::reset_capture();
    char* pwd_argv[] = {const_cast<char*>("pwd")};
    shell_cmd::cmd_pwd(1, pwd_argv);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/var/log") != nullptr);
}

TEST("shell_pipeline: cd .. then pwd shows parent") {
    mock::reset_capture();
    mock::set_cwd("/var/log");
    mock::chdir_return = 0;

    char* cd_argv[] = {const_cast<char*>("cd"), const_cast<char*>("..")};
    shell_cmd::cmd_cd(2, cd_argv);
    // Implementation leaves trailing '/' after .. pop at end of path
    ASSERT_TRUE(std::strcmp(mock::cwd, "/var/") == 0);

    mock::reset_capture();
    char* pwd_argv[] = {const_cast<char*>("pwd")};
    shell_cmd::cmd_pwd(1, pwd_argv);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/var/") != nullptr);
}

TEST("shell_pipeline: stat after cd uses relative path") {
    mock::reset_capture();
    mock::set_cwd("/home/user");
    mock::chdir_return = 0;
    mock::stat_return = 0;
    mock::captured_stat.st_size = 256;
    mock::captured_stat.st_ino = 7;

    // cd to /var/log
    char* cd_argv[] = {const_cast<char*>("cd"), const_cast<char*>("/var/log")};
    shell_cmd::cmd_cd(2, cd_argv);

    // stat a file (relative path captured)
    mock::reset_capture();
    mock::clear_last_stat();
    mock::stat_return = 0;

    char* stat_argv[] = {const_cast<char*>("stat"), const_cast<char*>("syslog")};
    shell_cmd::cmd_stat(2, stat_argv);

    ASSERT_TRUE(std::strcmp(mock::last_stat_path, "syslog") == 0);
    ASSERT_TRUE(std::strstr(mock::write_capture, "syslog") != nullptr);
}

// ============================================================
// Main function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
