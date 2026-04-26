/**
 * @file test/unit/test_sys_pipe.cpp
 * @brief Host-side unit tests for sys_pipe integration (031 Phase 2)
 *
 * Test coverage:
 *   - FDTable::set() installs a File at a specific slot
 *   - FDTable::set() returns false for out-of-range fd
 *   - FDTable::set() replaces an existing entry (caller manages old File)
 *   - Pipe + FDTable::set() + InodeOps round-trip: write via PipeWriteOps,
 *     read via PipeReadOps through the FDTable indirection
 *   - Close read end (via FDTable) then write returns -1
 *   - Close write end (via FDTable) then read returns 0 (EOF)
 *
 * Links directly with kernel/fs/file.cpp, kernel/ipc/pipe.cpp,
 * kernel/ipc/pipe_ops.cpp, kernel/fs/inode.cpp, and unit/host_spinlock.cpp.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#    include <cstdint>
#    include <cstring>

#    include "kernel/fs/file.hpp"
#    include "kernel/fs/inode.hpp"
#    include "kernel/ipc/pipe.hpp"
#    include "kernel/ipc/pipe_ops.hpp"

using namespace cinux::fs;
using namespace cinux::ipc;

// ============================================================
// Helper: create a Pipe on the heap with two Inodes
// ============================================================

struct PipeEndpoints {
    Pipe*         pipe;
    Inode*        read_inode;
    Inode*        write_inode;
    PipeReadOps*  read_ops;
    PipeWriteOps* write_ops;
};

static PipeEndpoints make_pipe_endpoints() {
    PipeEndpoints ep;
    ep.pipe      = new Pipe();
    ep.read_ops  = new PipeReadOps(ep.pipe);
    ep.write_ops = new PipeWriteOps(ep.pipe);

    ep.read_inode       = new Inode();
    ep.read_inode->ops  = ep.read_ops;
    ep.read_inode->type = InodeType::Regular;

    ep.write_inode       = new Inode();
    ep.write_inode->ops  = ep.write_ops;
    ep.write_inode->type = InodeType::Regular;

    return ep;
}

static void cleanup_pipe_endpoints(PipeEndpoints& ep) {
    delete ep.write_inode;
    delete ep.read_inode;
    delete ep.write_ops;
    delete ep.read_ops;
    delete ep.pipe;
}

// ============================================================
// 1. FDTable::set() -- basic slot assignment
// ============================================================

// set() installs a File at a specific slot and get() retrieves it.
TEST("sys_pipe: FDTable set installs File at specific slot") {
    FDTable table;
    Inode   inode{};

    File* f  = new File(&inode, 0, OpenFlags::RDONLY);
    bool  ok = table.set(0, f);
    ASSERT_TRUE(ok);

    File* retrieved = table.get(0);
    ASSERT_NOT_NULL(retrieved);
    ASSERT_TRUE(retrieved == f);
    ASSERT_TRUE(retrieved->inode == &inode);
    ASSERT_TRUE(retrieved->flags == OpenFlags::RDONLY);

    // Clean up to avoid leak (set bypasses alloc, so we delete manually)
    delete retrieved;
}

// set() at slot 1 installs correctly.
TEST("sys_pipe: FDTable set installs File at slot 1") {
    FDTable table;
    Inode   inode{};

    File* f = new File(&inode, 0, OpenFlags::WRONLY);
    ASSERT_TRUE(table.set(1, f));

    File* retrieved = table.get(1);
    ASSERT_NOT_NULL(retrieved);
    ASSERT_TRUE(retrieved == f);
    ASSERT_TRUE(retrieved->flags == OpenFlags::WRONLY);

    delete retrieved;
}

// ============================================================
// 2. FDTable::set() -- out-of-range rejection
// ============================================================

// set() with negative fd returns false.
TEST("sys_pipe: FDTable set rejects negative fd") {
    FDTable table;
    Inode   inode{};
    File    f(&inode, 0, OpenFlags::RDONLY);

    ASSERT_FALSE(table.set(-1, &f));
}

// set() with fd == FD_TABLE_SIZE returns false.
TEST("sys_pipe: FDTable set rejects fd at table size") {
    FDTable table;
    Inode   inode{};
    File    f(&inode, 0, OpenFlags::RDONLY);

    ASSERT_FALSE(table.set(static_cast<int>(FD_TABLE_SIZE), &f));
}

// set() with fd > FD_TABLE_SIZE returns false.
TEST("sys_pipe: FDTable set rejects fd beyond table size") {
    FDTable table;
    Inode   inode{};
    File    f(&inode, 0, OpenFlags::RDONLY);

    ASSERT_FALSE(table.set(9999, &f));
}

// ============================================================
// 3. FDTable::set() -- replaces existing entry
// ============================================================

// set() replaces whatever was at the slot (caller manages old entry).
TEST("sys_pipe: FDTable set replaces existing entry") {
    FDTable table;
    Inode   inode1{};
    Inode   inode2{};

    File* f1 = new File(&inode1, 0, OpenFlags::RDONLY);
    File* f2 = new File(&inode2, 0, OpenFlags::WRONLY);

    ASSERT_TRUE(table.set(0, f1));
    ASSERT_TRUE(table.set(0, f2));  // replace

    File* retrieved = table.get(0);
    ASSERT_NOT_NULL(retrieved);
    ASSERT_TRUE(retrieved == f2);
    ASSERT_TRUE(retrieved->flags == OpenFlags::WRONLY);

    // f1 was replaced without being freed -- caller responsibility
    delete f1;
    delete retrieved;
}

// ============================================================
// 4. Pipe + FDTable::set() round-trip: write then read
// ============================================================

// Create a pipe, install both ends via FDTable::set(), write via
// the write inode's ops, read via the read inode's ops.
TEST("sys_pipe: pipe write/read round-trip through FDTable") {
    FDTable       table;
    PipeEndpoints ep = make_pipe_endpoints();

    // Install pipe ends at slots 0 (read) and 1 (write)
    File* read_file  = new File(ep.read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(ep.write_inode, 0, OpenFlags::WRONLY);

    ASSERT_TRUE(table.set(0, read_file));
    ASSERT_TRUE(table.set(1, write_file));

    // Write data through the write inode's ops
    const char msg[] = "PipeData";
    int64_t    w     = ep.write_inode->ops->write(ep.write_inode, 0, msg, 8);
    ASSERT_EQ(w, 8);

    // Read data through the read inode's ops
    char    buf[16] = {};
    int64_t r       = ep.read_inode->ops->read(ep.read_inode, 0, buf, 8);
    ASSERT_EQ(r, 8);
    ASSERT_TRUE(memcmp(buf, "PipeData", 8) == 0);

    // Cleanup: delete File objects (table.set gave us ownership)
    delete write_file;
    delete read_file;
    cleanup_pipe_endpoints(ep);
}

// ============================================================
// 5. Close read end, then write returns -1
// ============================================================

// After closing the read end (close_reader on Pipe), writing through
// the write inode's ops returns -1.
TEST("sys_pipe: write returns -1 after close_reader") {
    FDTable       table;
    PipeEndpoints ep = make_pipe_endpoints();

    File* read_file  = new File(ep.read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(ep.write_inode, 0, OpenFlags::WRONLY);

    ASSERT_TRUE(table.set(0, read_file));
    ASSERT_TRUE(table.set(1, write_file));

    // Close the reader
    ep.pipe->close_reader();

    // Write should fail
    int64_t w = ep.write_inode->ops->write(ep.write_inode, 0, "data", 4);
    ASSERT_EQ(w, -1);

    delete write_file;
    delete read_file;
    cleanup_pipe_endpoints(ep);
}

// ============================================================
// 6. Close write end, then read returns 0 (EOF)
// ============================================================

// After closing the write end (close_writer on Pipe), reading through
// the read inode's ops returns 0.
TEST("sys_pipe: read returns 0 after close_writer") {
    FDTable       table;
    PipeEndpoints ep = make_pipe_endpoints();

    File* read_file  = new File(ep.read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(ep.write_inode, 0, OpenFlags::WRONLY);

    ASSERT_TRUE(table.set(0, read_file));
    ASSERT_TRUE(table.set(1, write_file));

    // Close the writer
    ep.pipe->close_writer();

    // Read should return 0 (EOF)
    char    buf[16] = {};
    int64_t r       = ep.read_inode->ops->read(ep.read_inode, 0, buf, 8);
    ASSERT_EQ(r, 0);

    delete write_file;
    delete read_file;
    cleanup_pipe_endpoints(ep);
}

// ============================================================
// 7. Drain then EOF
// ============================================================

// Write data, close writer, drain all data, then read returns 0.
TEST("sys_pipe: drain then EOF through FDTable") {
    FDTable       table;
    PipeEndpoints ep = make_pipe_endpoints();

    File* read_file  = new File(ep.read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(ep.write_inode, 0, OpenFlags::WRONLY);

    ASSERT_TRUE(table.set(0, read_file));
    ASSERT_TRUE(table.set(1, write_file));

    // Write some data
    ASSERT_EQ(ep.write_inode->ops->write(ep.write_inode, 0, "AB", 2), 2);

    // Close writer
    ep.pipe->close_writer();

    // Drain remaining data
    char    buf[16] = {};
    int64_t r       = ep.read_inode->ops->read(ep.read_inode, 0, buf, 8);
    ASSERT_EQ(r, 2);
    ASSERT_TRUE(memcmp(buf, "AB", 2) == 0);

    // Now EOF
    r = ep.read_inode->ops->read(ep.read_inode, 0, buf, 8);
    ASSERT_EQ(r, 0);

    delete write_file;
    delete read_file;
    cleanup_pipe_endpoints(ep);
}

// ============================================================
// 8. FDTable::set() preserves inode fields
// ============================================================

// After set(), the File at the slot has the correct inode pointer,
// offset, and flags.
TEST("sys_pipe: FDTable set preserves File fields") {
    FDTable table;
    Inode   inode{};
    inode.ino  = 77;
    inode.size = 2048;

    File* f = new File(&inode, 42, OpenFlags::RDWR);
    ASSERT_TRUE(table.set(5, f));

    File* retrieved = table.get(5);
    ASSERT_NOT_NULL(retrieved);
    ASSERT_TRUE(retrieved->inode == &inode);
    ASSERT_EQ(retrieved->offset, 42ULL);
    ASSERT_TRUE(retrieved->flags == OpenFlags::RDWR);

    delete retrieved;
}

// ============================================================
// 9. FDTable::set() then close() releases correctly
// ============================================================

// set() a File, then close() the fd -- get() should return nullptr.
TEST("sys_pipe: set then close releases File") {
    FDTable table;
    Inode   inode{};
    File*   f = new File(&inode, 0, OpenFlags::RDONLY);

    ASSERT_TRUE(table.set(0, f));
    ASSERT_NOT_NULL(table.get(0));

    ASSERT_EQ(table.close(0), 0);
    ASSERT_NULL(table.get(0));
}

// ============================================================
// 10. Multiple pipe write/read cycles through FDTable
// ============================================================

// Write-then-read multiple times in sequence through the FDTable
// indirection.
TEST("sys_pipe: multiple write/read cycles") {
    FDTable       table;
    PipeEndpoints ep = make_pipe_endpoints();

    File* read_file  = new File(ep.read_inode, 0, OpenFlags::RDONLY);
    File* write_file = new File(ep.write_inode, 0, OpenFlags::WRONLY);

    ASSERT_TRUE(table.set(0, read_file));
    ASSERT_TRUE(table.set(1, write_file));

    // First cycle
    ASSERT_EQ(ep.write_inode->ops->write(ep.write_inode, 0, "AB", 2), 2);
    char buf[8] = {};
    ASSERT_EQ(ep.read_inode->ops->read(ep.read_inode, 0, buf, 2), 2);
    ASSERT_TRUE(memcmp(buf, "AB", 2) == 0);

    // Second cycle
    ASSERT_EQ(ep.write_inode->ops->write(ep.write_inode, 0, "CDEF", 4), 4);
    ASSERT_EQ(ep.read_inode->ops->read(ep.read_inode, 0, buf, 4), 4);
    ASSERT_TRUE(memcmp(buf, "CDEF", 4) == 0);

    delete write_file;
    delete read_file;
    cleanup_pipe_endpoints(ep);
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
