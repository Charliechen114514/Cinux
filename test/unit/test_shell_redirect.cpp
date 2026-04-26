/**
 * @file test/unit/test_shell_redirect.cpp
 * @brief Host-side unit tests for shell stdin/stdout pipe redirection (Phase 5)
 *
 * Test coverage:
 *   - FDTable::set() installs pipe-backed File at fd 0 and fd 1
 *   - PipeReadOps read path: pipe data -> File -> InodeOps::read
 *   - PipeWriteOps write path: data -> File -> InodeOps::write -> pipe
 *   - Round-trip: write to stdout pipe, read from stdin pipe
 *   - try_read/try_write non-blocking path for GUI polling
 *   - EOF semantics through FDTable indirection
 *
 * Links with kernel/fs/file.cpp, kernel/ipc/pipe.cpp, kernel/ipc/pipe_ops.cpp,
 * kernel/fs/inode.cpp, kernel/fs/vfs_mount.cpp, and unit/host_spinlock.cpp.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>

#    include "kernel/fs/file.hpp"
#    include "kernel/fs/inode.hpp"
#    include "kernel/fs/vfs_mount.hpp"
#    include "kernel/ipc/pipe.hpp"
#    include "kernel/ipc/pipe_ops.hpp"

using namespace cinux::ipc;
using namespace cinux::fs;

// ============================================================
// Helper: create a complete pipe-backed fd setup
// ============================================================

/**
 * @brief RAII wrapper for a pipe-backed fd setup (stdin + stdout)
 *
 * Creates two pipes, wraps them in Inodes with PipeReadOps/PipeWriteOps,
 * and installs them at fd 0 (stdin) and fd 1 (stdout) in the global FDTable.
 * Automatically cleans up on destruction.
 */
struct PipeRedirect {
    Pipe*         stdin_pipe;
    Pipe*         stdout_pipe;
    Inode*        stdin_inode;
    Inode*        stdout_inode;
    PipeReadOps*  stdin_ops;
    PipeWriteOps* stdout_ops;

    PipeRedirect() {
        cinux::fs::vfs_mount_init();

        stdin_pipe  = new Pipe();
        stdout_pipe = new Pipe();

        stdin_ops  = new PipeReadOps(stdin_pipe);
        stdout_ops = new PipeWriteOps(stdout_pipe);

        stdin_inode       = new Inode();
        stdin_inode->ops  = stdin_ops;
        stdin_inode->type = InodeType::Regular;

        stdout_inode       = new Inode();
        stdout_inode->ops  = stdout_ops;
        stdout_inode->type = InodeType::Regular;

        auto* stdin_file  = new File(stdin_inode, 0, OpenFlags::RDONLY);
        auto* stdout_file = new File(stdout_inode, 0, OpenFlags::WRONLY);

        g_global_fd_table().set(0, stdin_file);
        g_global_fd_table().set(1, stdout_file);
    }

    ~PipeRedirect() {
        // Cleanup fd table entries
        g_global_fd_table().close(0);
        g_global_fd_table().close(1);

        delete stdout_file;
        delete stdin_file;
        delete stdout_inode;
        delete stdin_inode;
        delete stdout_ops;
        delete stdin_ops;
        delete stdout_pipe;
        delete stdin_pipe;
    }

private:
    File* stdin_file  = nullptr;
    File* stdout_file = nullptr;
};

// ============================================================
// 1. FDTable set/get round-trip
// ============================================================

// Verify fd 0 and fd 1 are correctly installed in the FDTable.
TEST("shell_redirect: fd 0 and 1 installed in FDTable") {
    PipeRedirect setup;

    File* f0 = g_global_fd_table().get(0);
    File* f1 = g_global_fd_table().get(1);

    ASSERT_NOT_NULL(f0);
    ASSERT_NOT_NULL(f1);
    ASSERT_NOT_NULL(f0->inode);
    ASSERT_NOT_NULL(f1->inode);
    ASSERT_EQ(f0->flags, OpenFlags::RDONLY);
    ASSERT_EQ(f1->flags, OpenFlags::WRONLY);
}

// ============================================================
// 2. Write to stdout pipe via InodeOps
// ============================================================

// Simulate shell sys_write(1) by writing through fd 1's InodeOps.
TEST("shell_redirect: write through stdout fd goes into pipe") {
    PipeRedirect setup;

    const char msg[] = "Hello from shell";
    File*      f1    = g_global_fd_table().get(1);
    int64_t    w     = f1->inode->ops->write(f1->inode, f1->offset, msg, 16);
    ASSERT_EQ(w, 16);

    // Verify data is in the stdout pipe
    ASSERT_EQ(setup.stdout_pipe->available(), 16u);

    char    buf[32] = {};
    int64_t r       = setup.stdout_pipe->try_read(buf, 32);
    ASSERT_EQ(r, 16);
    ASSERT_TRUE(memcmp(buf, "Hello from shell", 16) == 0);
}

// ============================================================
// 3. Read from stdin pipe via InodeOps
// ============================================================

// Simulate shell sys_read(0) by reading through fd 0's InodeOps.
TEST("shell_redirect: read through stdin fd gets pipe data") {
    PipeRedirect setup;

    // Write data into the stdin pipe (simulating Terminal on_key)
    const char input[] = "echo test\n";
    setup.stdin_pipe->try_write(input, 11);
    setup.stdin_pipe->close_writer();  // EOF so blocking read returns after draining

    // Read through fd 0
    File*   f0      = g_global_fd_table().get(0);
    char    buf[32] = {};
    int64_t r       = f0->inode->ops->read(f0->inode, f0->offset, buf, 32);
    ASSERT_EQ(r, 11);
    ASSERT_TRUE(memcmp(buf, "echo test\n", 11) == 0);
}

// ============================================================
// 4. Full round-trip: Terminal -> stdin pipe -> shell -> stdout pipe
// ============================================================

// Simulate: Terminal types "help\n", shell reads from stdin pipe,
// shell writes "Cinux shell...\n" to stdout pipe.
TEST("shell_redirect: full round-trip terminal to shell and back") {
    PipeRedirect setup;

    // Terminal on_key writes to stdin pipe
    const char input[] = "help\n";
    int64_t    w       = setup.stdin_pipe->try_write(input, 5);
    ASSERT_EQ(w, 5);
    setup.stdin_pipe->close_writer();  // EOF so blocking read returns after draining

    // Shell reads from stdin pipe (via fd 0 InodeOps)
    File*   f0           = g_global_fd_table().get(0);
    char    read_buf[32] = {};
    int64_t r            = f0->inode->ops->read(f0->inode, f0->offset, read_buf, 32);
    ASSERT_EQ(r, 5);
    ASSERT_TRUE(memcmp(read_buf, "help\n", 5) == 0);

    // Shell writes response to stdout pipe (via fd 1 InodeOps)
    const char response[] = "Cinux shell - type 'help' for commands\n";
    File*      f1         = g_global_fd_table().get(1);
    w                     = f1->inode->ops->write(f1->inode, f1->offset, response, 40);
    ASSERT_EQ(w, 40);

    // Terminal polls stdout pipe and reads response
    char out_buf[64] = {};
    r                = setup.stdout_pipe->try_read(out_buf, 64);
    ASSERT_EQ(r, 40);
    ASSERT_TRUE(memcmp(out_buf, "Cinux shell - type 'help' for commands\n", 40) == 0);
}

// ============================================================
// 5. Multiple writes then poll read (simulating shell output)
// ============================================================

// Shell writes prompt + command output in separate writes.
TEST("shell_redirect: multiple writes then poll read") {
    PipeRedirect setup;

    File* f1 = g_global_fd_table().get(1);

    f1->inode->ops->write(f1->inode, 0, "cinux> ", 7);
    f1->inode->ops->write(f1->inode, 0, "echo hello\n", 11);
    f1->inode->ops->write(f1->inode, 0, "hello\n", 6);

    // Terminal polls all output
    char    buf[64] = {};
    int64_t total   = 0;
    while (true) {
        int64_t r = setup.stdout_pipe->try_read(buf + total, sizeof(buf) - total);
        if (r <= 0)
            break;
        total += r;
    }
    ASSERT_EQ(total, 24);
    ASSERT_TRUE(memcmp(buf, "cinux> echo hello\nhello\n", 24) == 0);
}

// ============================================================
// 6. Empty pipe poll returns 0
// ============================================================

// poll_output on empty stdout pipe should return 0 immediately.
TEST("shell_redirect: poll empty stdout pipe returns 0") {
    PipeRedirect setup;

    char    buf[32] = {};
    int64_t r       = setup.stdout_pipe->try_read(buf, 32);
    ASSERT_EQ(r, 0);
}

// ============================================================
// 7. stdin pipe write then read via ops
// ============================================================

// Write to stdin pipe using try_write, read via PipeReadOps.
TEST("shell_redirect: try_write stdin then read via ops") {
    PipeRedirect setup;

    // Simulate Terminal sending characters one at a time
    setup.stdin_pipe->try_write("l", 1);
    setup.stdin_pipe->try_write("s", 1);
    setup.stdin_pipe->try_write("\n", 1);
    setup.stdin_pipe->close_writer();  // EOF so blocking read returns after draining

    // Shell reads all 3 bytes at once
    File*   f0      = g_global_fd_table().get(0);
    char    buf[16] = {};
    int64_t r       = f0->inode->ops->read(f0->inode, f0->offset, buf, 16);
    ASSERT_EQ(r, 3);
    ASSERT_TRUE(memcmp(buf, "ls\n", 3) == 0);
}

// ============================================================
// 8. Direction enforcement: cannot read from stdout, cannot write to stdin
// ============================================================

// PipeWriteOps (on stdout fd) does not support read.
TEST("shell_redirect: cannot read from stdout fd") {
    PipeRedirect setup;

    // Write some data to stdout pipe first
    setup.stdout_pipe->try_write("data", 4);

    File*   f1      = g_global_fd_table().get(1);
    // PipeWriteOps inherits read() from InodeOps which returns -1
    char    buf[16] = {};
    int64_t r       = f1->inode->ops->read(f1->inode, f1->offset, buf, 16);
    ASSERT_EQ(r, -1);
}

// PipeReadOps (on stdin fd) does not support write.
TEST("shell_redirect: cannot write to stdin fd") {
    PipeRedirect setup;

    File*   f0 = g_global_fd_table().get(0);
    int64_t w  = f0->inode->ops->write(f0->inode, f0->offset, "data", 4);
    ASSERT_EQ(w, -1);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
