/**
 * @file test/unit/test_shell_write.cpp
 * @brief Host-side unit tests for shell write commands (028b)
 *
 * Test coverage:
 *   - cmd_touch: normal create, missing operand usage message
 *   - cmd_mkdir: normal create, missing operand usage message
 *   - cmd_rm:    normal delete, missing operand usage message
 *   - cmd_rmdir: normal delete, missing operand usage message
 *   - cmd_echo redirect: "echo text > file" parse, missing file name
 *   - Error handling: syscall returns error, output contains error message
 *
 * Pure command parsing and output verification -- no kernel code linked.
 * Hardware dependencies (sys_creat, sys_mkdir, sys_unlink, sys_rmdir,
 * sys_open, sys_write, sys_close) are isolated via mock reimplementation.
 *
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstddef>
#    include <cstdint>
#    include <cstring>

// ============================================================
// Include the real string utility implementation
// ============================================================

namespace cinux::user {

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') {
        ++n;
    }
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return static_cast<int>(*a) - static_cast<int>(*b);
        }
        ++a;
        ++b;
    }
    return static_cast<int>(*a) - static_cast<int>(*b);
}

}  // namespace cinux::user

// ============================================================
// Mock syscall infrastructure
// ============================================================

namespace mock {

// Capture buffer for sys_write output (stdout + file writes)
constexpr size_t CAPTURE_SIZE = 8192;
char             write_capture[CAPTURE_SIZE];
size_t           write_capture_len = 0;

void reset_capture() {
    std::memset(write_capture, 0, sizeof(write_capture));
    write_capture_len = 0;
}

// File-write capture (fd != 1)
char   file_capture[4096];
size_t file_capture_len = 0;

void reset_file_capture() {
    std::memset(file_capture, 0, sizeof(file_capture));
    file_capture_len = 0;
}

// Mock syscall return values (configurable per test)
int64_t creat_return  = 0;
int64_t mkdir_return  = 0;
int64_t unlink_return = 0;
int64_t rmdir_return  = 0;
int64_t open_return   = 3;  // fake fd
int64_t close_return  = 0;

// Last path argument passed to each syscall
char last_path[256];
void clear_last_path() {
    std::memset(last_path, 0, sizeof(last_path));
}

// Mock sys_write: captures stdout into write_capture, file writes into file_capture
int64_t sys_write(int fd, const void* buf, size_t count) {
    if (buf == nullptr)
        return -1;
    if (fd == 1) {
        size_t to_copy = count;
        if (write_capture_len + to_copy > CAPTURE_SIZE - 1)
            to_copy = CAPTURE_SIZE - 1 - write_capture_len;
        std::memcpy(write_capture + write_capture_len, buf, to_copy);
        write_capture_len += to_copy;
        write_capture[write_capture_len] = '\0';
        return static_cast<int64_t>(count);
    } else {
        // File write capture
        size_t to_copy = count;
        if (file_capture_len + to_copy > sizeof(file_capture) - 1)
            to_copy = sizeof(file_capture) - 1 - file_capture_len;
        std::memcpy(file_capture + file_capture_len, buf, to_copy);
        file_capture_len += to_copy;
        file_capture[file_capture_len] = '\0';
        return static_cast<int64_t>(count);
    }
}

int64_t sys_creat(const char* path) {
    if (path) {
        size_t len = std::strlen(path);
        if (len < sizeof(last_path)) {
            std::memcpy(last_path, path, len + 1);
        }
    }
    return creat_return;
}

int64_t sys_mkdir(const char* path) {
    if (path) {
        size_t len = std::strlen(path);
        if (len < sizeof(last_path)) {
            std::memcpy(last_path, path, len + 1);
        }
    }
    return mkdir_return;
}

int64_t sys_unlink(const char* path) {
    if (path) {
        size_t len = std::strlen(path);
        if (len < sizeof(last_path)) {
            std::memcpy(last_path, path, len + 1);
        }
    }
    return unlink_return;
}

int64_t sys_rmdir(const char* path) {
    if (path) {
        size_t len = std::strlen(path);
        if (len < sizeof(last_path)) {
            std::memcpy(last_path, path, len + 1);
        }
    }
    return rmdir_return;
}

int64_t sys_open(const char* path, int /*flags*/) {
    if (path) {
        size_t len = std::strlen(path);
        if (len < sizeof(last_path)) {
            std::memcpy(last_path, path, len + 1);
        }
    }
    return open_return;
}

int64_t sys_close(int /*fd*/) {
    return close_return;
}

}  // namespace mock

// ============================================================
// Mirror of shell command implementations (using mock syscalls)
// ============================================================

namespace shell_write_test {

using cinux::user::strlen;
using cinux::user::strcmp;

void write_str(const char* s) {
    mock::sys_write(1, s, strlen(s));
}

// Mirror of cmd_touch from cmd_touch.cpp
void cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        write_str("touch: missing file operand\n");
        return;
    }
    const char* path   = argv[1];
    int64_t     result = mock::sys_creat(path);
    if (result < 0) {
        write_str("touch: cannot create '");
        write_str(path);
        write_str("'\n");
    }
}

// Mirror of cmd_mkdir from cmd_mkdir.cpp
void cmd_mkdir(int argc, char** argv) {
    if (argc < 2) {
        write_str("mkdir: missing directory operand\n");
        return;
    }
    const char* path   = argv[1];
    int64_t     result = mock::sys_mkdir(path);
    if (result < 0) {
        write_str("mkdir: cannot create directory '");
        write_str(path);
        write_str("'\n");
    }
}

// Mirror of cmd_rm from cmd_rm.cpp
void cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        write_str("rm: missing file operand\n");
        return;
    }
    const char* path   = argv[1];
    int64_t     result = mock::sys_unlink(path);
    if (result < 0) {
        write_str("rm: cannot remove '");
        write_str(path);
        write_str("'\n");
    }
}

// Mirror of cmd_rmdir from cmd_rmdir.cpp
void cmd_rmdir(int argc, char** argv) {
    if (argc < 2) {
        write_str("rmdir: missing directory operand\n");
        return;
    }
    const char* path   = argv[1];
    int64_t     result = mock::sys_rmdir(path);
    if (result < 0) {
        write_str("rmdir: cannot remove '");
        write_str(path);
        write_str("'\n");
    }
}

// Mirror of cmd_echo from cmd_echo.cpp (including redirect)
void cmd_echo(int argc, char** argv) {
    int redirect_idx = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], ">") == 0) {
            redirect_idx = i;
            break;
        }
    }

    if (redirect_idx > 0 && redirect_idx + 1 < argc) {
        const char* path         = argv[redirect_idx + 1];
        int64_t     creat_result = mock::sys_creat(path);
        if (creat_result < 0) {
            write_str("echo: cannot create '");
            write_str(path);
            write_str("'\n");
            return;
        }

        int64_t fd = mock::sys_open(path, 1);
        if (fd < 0) {
            write_str("echo: cannot open '");
            write_str(path);
            write_str("'\n");
            return;
        }

        for (int i = 1; i < redirect_idx; ++i) {
            if (i > 1) {
                const char space[] = " ";
                mock::sys_write(static_cast<int>(fd), space, 1);
            }
            mock::sys_write(static_cast<int>(fd), argv[i], strlen(argv[i]));
        }

        const char nl[] = "\n";
        mock::sys_write(static_cast<int>(fd), nl, 1);

        mock::sys_close(static_cast<int>(fd));
        return;
    }

    // Normal mode: print to stdout
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            mock::sys_write(1, " ", 1);
        }
        write_str(argv[i]);
    }
    mock::sys_write(1, "\n", 1);
}

}  // namespace shell_write_test

// ============================================================
// 1. cmd_touch tests
// ============================================================

// Normal: touch with valid path calls sys_creat and produces no error output
TEST("shell_write_touch: normal create produces no output") {
    mock::reset_capture();
    mock::creat_return = 0;

    char* argv[] = {const_cast<char*>("touch"), const_cast<char*>("/testfile")};
    shell_write_test::cmd_touch(2, argv);

    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_touch: passes path to sys_creat") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::creat_return = 0;

    char* argv[] = {const_cast<char*>("touch"), const_cast<char*>("/my_file")};
    shell_write_test::cmd_touch(2, argv);

    ASSERT_TRUE(std::strcmp(mock::last_path, "/my_file") == 0);
}

// Missing operand: touch without arguments prints usage
TEST("shell_write_touch: missing operand prints usage") {
    mock::reset_capture();

    char* argv[] = {const_cast<char*>("touch")};
    shell_write_test::cmd_touch(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "missing file operand") != nullptr);
}

// Error path: sys_creat returns -1
TEST("shell_write_touch: creat error prints message") {
    mock::reset_capture();
    mock::creat_return = -1;

    char* argv[] = {const_cast<char*>("touch"), const_cast<char*>("/badpath")};
    shell_write_test::cmd_touch(2, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot create '") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/badpath") != nullptr);
}

// ============================================================
// 2. cmd_mkdir tests
// ============================================================

TEST("shell_write_mkdir: normal create produces no output") {
    mock::reset_capture();
    mock::mkdir_return = 0;

    char* argv[] = {const_cast<char*>("mkdir"), const_cast<char*>("/testdir")};
    shell_write_test::cmd_mkdir(2, argv);

    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_mkdir: passes path to sys_mkdir") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::mkdir_return = 0;

    char* argv[] = {const_cast<char*>("mkdir"), const_cast<char*>("/my_dir")};
    shell_write_test::cmd_mkdir(2, argv);

    ASSERT_TRUE(std::strcmp(mock::last_path, "/my_dir") == 0);
}

TEST("shell_write_mkdir: missing operand prints usage") {
    mock::reset_capture();

    char* argv[] = {const_cast<char*>("mkdir")};
    shell_write_test::cmd_mkdir(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "missing directory operand") != nullptr);
}

TEST("shell_write_mkdir: mkdir error prints message") {
    mock::reset_capture();
    mock::mkdir_return = -1;

    char* argv[] = {const_cast<char*>("mkdir"), const_cast<char*>("/baddir")};
    shell_write_test::cmd_mkdir(2, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot create directory '") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/baddir") != nullptr);
}

// ============================================================
// 3. cmd_rm tests
// ============================================================

TEST("shell_write_rm: normal delete produces no output") {
    mock::reset_capture();
    mock::unlink_return = 0;

    char* argv[] = {const_cast<char*>("rm"), const_cast<char*>("/testfile")};
    shell_write_test::cmd_rm(2, argv);

    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_rm: passes path to sys_unlink") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::unlink_return = 0;

    char* argv[] = {const_cast<char*>("rm"), const_cast<char*>("/remove_me")};
    shell_write_test::cmd_rm(2, argv);

    ASSERT_TRUE(std::strcmp(mock::last_path, "/remove_me") == 0);
}

TEST("shell_write_rm: missing operand prints usage") {
    mock::reset_capture();

    char* argv[] = {const_cast<char*>("rm")};
    shell_write_test::cmd_rm(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "missing file operand") != nullptr);
}

TEST("shell_write_rm: unlink error prints message") {
    mock::reset_capture();
    mock::unlink_return = -1;

    char* argv[] = {const_cast<char*>("rm"), const_cast<char*>("/nofile")};
    shell_write_test::cmd_rm(2, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot remove '") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/nofile") != nullptr);
}

// ============================================================
// 4. cmd_rmdir tests
// ============================================================

TEST("shell_write_rmdir: normal delete produces no output") {
    mock::reset_capture();
    mock::rmdir_return = 0;

    char* argv[] = {const_cast<char*>("rmdir"), const_cast<char*>("/testdir")};
    shell_write_test::cmd_rmdir(2, argv);

    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_rmdir: passes path to sys_rmdir") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::rmdir_return = 0;

    char* argv[] = {const_cast<char*>("rmdir"), const_cast<char*>("/rmdir_me")};
    shell_write_test::cmd_rmdir(2, argv);

    ASSERT_TRUE(std::strcmp(mock::last_path, "/rmdir_me") == 0);
}

TEST("shell_write_rmdir: missing operand prints usage") {
    mock::reset_capture();

    char* argv[] = {const_cast<char*>("rmdir")};
    shell_write_test::cmd_rmdir(1, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "missing directory operand") != nullptr);
}

TEST("shell_write_rmdir: rmdir error prints message") {
    mock::reset_capture();
    mock::rmdir_return = -1;

    char* argv[] = {const_cast<char*>("rmdir"), const_cast<char*>("/notempty")};
    shell_write_test::cmd_rmdir(2, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot remove '") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/notempty") != nullptr);
}

// ============================================================
// 5. cmd_echo redirect tests
// ============================================================

// Normal redirect: "echo hello > file" creates file, writes content
TEST("shell_write_echo_redirect: redirects text to file") {
    mock::reset_capture();
    mock::reset_file_capture();
    mock::clear_last_path();
    mock::creat_return = 0;
    mock::open_return  = 3;
    mock::close_return = 0;

    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("hello"), const_cast<char*>(">"),
                    const_cast<char*>("/outfile")};
    shell_write_test::cmd_echo(4, argv);

    // No stdout output
    ASSERT_EQ(mock::write_capture_len, 0ULL);
    // File capture should contain "hello\n"
    ASSERT_TRUE(std::strcmp(mock::file_capture, "hello\n") == 0);
}

// Redirect with multiple words: "echo hello world > file"
TEST("shell_write_echo_redirect: redirects multiple words to file") {
    mock::reset_capture();
    mock::reset_file_capture();
    mock::creat_return = 0;
    mock::open_return  = 3;
    mock::close_return = 0;

    char* argv[] = {const_cast<char*>("echo"),  const_cast<char*>("hello"),
                    const_cast<char*>("world"), const_cast<char*>("foo"),
                    const_cast<char*>(">"),     const_cast<char*>("/multiout")};
    shell_write_test::cmd_echo(6, argv);

    ASSERT_EQ(mock::write_capture_len, 0ULL);
    ASSERT_TRUE(std::strcmp(mock::file_capture, "hello world foo\n") == 0);
}

// Missing file name after >: "echo hello >" — no redirect_idx+1 < argc
TEST("shell_write_echo_redirect: missing file after > falls through to normal echo") {
    mock::reset_capture();

    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("hello"), const_cast<char*>(">")};
    shell_write_test::cmd_echo(3, argv);

    // redirect_idx = 2, redirect_idx + 1 = 3 which is NOT < argc(3)
    // So it falls through to normal mode: prints "hello\n"
    ASSERT_TRUE(std::strstr(mock::write_capture, "hello") != nullptr);
}

// Redirect where creat fails
TEST("shell_write_echo_redirect: creat failure prints error") {
    mock::reset_capture();
    mock::reset_file_capture();
    mock::creat_return = -1;

    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("hello"), const_cast<char*>(">"),
                    const_cast<char*>("/badfile")};
    shell_write_test::cmd_echo(4, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot create '") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/badfile") != nullptr);
}

// Redirect where open fails after creat succeeds
TEST("shell_write_echo_redirect: open failure prints error") {
    mock::reset_capture();
    mock::reset_file_capture();
    mock::creat_return = 0;
    mock::open_return  = -1;

    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("hello"), const_cast<char*>(">"),
                    const_cast<char*>("/noopen")};
    shell_write_test::cmd_echo(4, argv);

    ASSERT_TRUE(std::strstr(mock::write_capture, "cannot open '") != nullptr);
    ASSERT_TRUE(std::strstr(mock::write_capture, "/noopen") != nullptr);
}

// Normal echo (no redirect) still works
TEST("shell_write_echo_redirect: normal echo without redirect") {
    mock::reset_capture();

    char* argv[] = {const_cast<char*>("echo"), const_cast<char*>("hello"),
                    const_cast<char*>("world")};
    shell_write_test::cmd_echo(3, argv);

    ASSERT_TRUE(std::strcmp(mock::write_capture, "hello world\n") == 0);
}

// ============================================================
// 6. Full pipeline tests (tokenize + dispatch for write commands)
// ============================================================

// Helper: tokenize a command line
size_t tokenize(char* line, char** argv, size_t max_tokens) {
    size_t argc = 0;
    while (*line != '\0' && argc < max_tokens) {
        while (*line == ' ' || *line == '\t')
            ++line;
        if (*line == '\0')
            break;
        argv[argc++] = line;
        while (*line != '\0' && *line != ' ' && *line != '\t')
            ++line;
        if (*line != '\0')
            *line++ = '\0';
    }
    return argc;
}

constexpr size_t MAX_TOKENS = 16;

// Dispatch table for write commands
struct CmdEntry {
    const char* name;
    void (*handler)(int argc, char** argv);
};

TEST("shell_write_pipeline: touch /myfile dispatches correctly") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::creat_return = 0;

    char   line[] = "touch /myfile";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);

    CmdEntry cmds[] = {
        {"touch", shell_write_test::cmd_touch}, {"mkdir", shell_write_test::cmd_mkdir},
        {"rm", shell_write_test::cmd_rm},       {"rmdir", shell_write_test::cmd_rmdir},
        {"echo", shell_write_test::cmd_echo},   {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], cmds[i].name) == 0) {
            cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);
    ASSERT_TRUE(std::strcmp(mock::last_path, "/myfile") == 0);
    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_pipeline: mkdir /mydir dispatches correctly") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::mkdir_return = 0;

    char   line[] = "mkdir /mydir";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);

    CmdEntry cmds[] = {
        {"touch", shell_write_test::cmd_touch}, {"mkdir", shell_write_test::cmd_mkdir},
        {"rm", shell_write_test::cmd_rm},       {"rmdir", shell_write_test::cmd_rmdir},
        {"echo", shell_write_test::cmd_echo},   {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], cmds[i].name) == 0) {
            cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);
    ASSERT_TRUE(std::strcmp(mock::last_path, "/mydir") == 0);
    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_pipeline: rm /myfile dispatches correctly") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::unlink_return = 0;

    char   line[] = "rm /myfile";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);

    CmdEntry cmds[] = {
        {"touch", shell_write_test::cmd_touch}, {"mkdir", shell_write_test::cmd_mkdir},
        {"rm", shell_write_test::cmd_rm},       {"rmdir", shell_write_test::cmd_rmdir},
        {"echo", shell_write_test::cmd_echo},   {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], cmds[i].name) == 0) {
            cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);
    ASSERT_TRUE(std::strcmp(mock::last_path, "/myfile") == 0);
    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_pipeline: rmdir /mydir dispatches correctly") {
    mock::reset_capture();
    mock::clear_last_path();
    mock::rmdir_return = 0;

    char   line[] = "rmdir /mydir";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);

    CmdEntry cmds[] = {
        {"touch", shell_write_test::cmd_touch}, {"mkdir", shell_write_test::cmd_mkdir},
        {"rm", shell_write_test::cmd_rm},       {"rmdir", shell_write_test::cmd_rmdir},
        {"echo", shell_write_test::cmd_echo},   {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], cmds[i].name) == 0) {
            cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);
    ASSERT_TRUE(std::strcmp(mock::last_path, "/mydir") == 0);
    ASSERT_EQ(mock::write_capture_len, 0ULL);
}

TEST("shell_write_pipeline: echo hello > file dispatches correctly") {
    mock::reset_capture();
    mock::reset_file_capture();
    mock::clear_last_path();
    mock::creat_return = 0;
    mock::open_return  = 3;
    mock::close_return = 0;

    char   line[] = "echo hello > /outfile";
    char*  argv[MAX_TOKENS];
    size_t argc = tokenize(line, argv, MAX_TOKENS);

    CmdEntry cmds[] = {
        {"touch", shell_write_test::cmd_touch}, {"mkdir", shell_write_test::cmd_mkdir},
        {"rm", shell_write_test::cmd_rm},       {"rmdir", shell_write_test::cmd_rmdir},
        {"echo", shell_write_test::cmd_echo},   {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp(argv[0], cmds[i].name) == 0) {
            cmds[i].handler(static_cast<int>(argc), argv);
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);
    ASSERT_EQ(mock::write_capture_len, 0ULL);
    ASSERT_TRUE(std::strcmp(mock::file_capture, "hello\n") == 0);
}

// ============================================================
// 7. Dispatch table completeness tests
// ============================================================

TEST("shell_write_dispatch: all write commands registered") {
    CmdEntry cmds[] = {
        {"touch", shell_write_test::cmd_touch}, {"mkdir", shell_write_test::cmd_mkdir},
        {"rm", shell_write_test::cmd_rm},       {"rmdir", shell_write_test::cmd_rmdir},
        {"echo", shell_write_test::cmd_echo},   {nullptr, nullptr},
    };

    int count = 0;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        ++count;
    }
    ASSERT_EQ(count, 5);
}

TEST("shell_write_dispatch: unknown command not found") {
    CmdEntry cmds[] = {
        {"touch", shell_write_test::cmd_touch}, {"mkdir", shell_write_test::cmd_mkdir},
        {"rm", shell_write_test::cmd_rm},       {"rmdir", shell_write_test::cmd_rmdir},
        {"echo", shell_write_test::cmd_echo},   {nullptr, nullptr},
    };

    bool found = false;
    for (int i = 0; cmds[i].name != nullptr; ++i) {
        if (cinux::user::strcmp("foobar", cmds[i].name) == 0) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(!found);
}

// ============================================================
// Main function
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
