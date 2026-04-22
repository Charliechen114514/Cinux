/**
 * @file test/unit/test_fd_table.cpp
 * @brief Host-side unit tests for FDTable (kernel/fs/file.hpp)
 *
 * Test coverage:
 *   - Construction: all slots are nullptr after construction
 *   - Alloc: returns sequential indices 0, 1, 2, ...
 *   - Alloc: returns FD_NONE when table is full
 *   - Alloc: reuses lowest freed slot
 *   - Close: releases a descriptor, returns 0 on success
 *   - Close: returns -1 for out-of-range fd
 *   - Close: returns -1 for already-closed fd
 *   - Close: returns -1 for negative fd
 *   - Get: returns valid File* for open descriptor
 *   - Get: returns nullptr for unopened descriptor
 *   - Get: returns nullptr for out-of-range fd
 *   - Get: returns nullptr for closed descriptor
 *   - File fields: inode, offset, flags are stored correctly
 *
 * Links directly with kernel/fs/file.cpp -- no hardware dependencies.
 * Compile condition: -DCINUX_HOST_TEST
 */

#define TEST_FRAMEWORK_IMPL
#include "test_framework.h"

#ifdef CINUX_HOST_TEST

#include <cstdint>
#include <cstring>

#include "fs/file.hpp"
#include "fs/inode.hpp"

using namespace cinux::fs;

// ============================================================
// 1. Construction
// ============================================================

// After construction, every slot in the table must be nullptr.
TEST("fd_table: all slots are nullptr after construction") {
    FDTable table;
    for (uint32_t i = 0; i < FD_TABLE_SIZE; ++i) {
        ASSERT_NULL(table.get(static_cast<int>(i)));
    }
}

// ============================================================
// 2. Alloc -- sequential allocation
// ============================================================

// First alloc returns fd 3 (0-2 reserved for stdin/stdout/stderr).
TEST("fd_table: first alloc returns fd 3") {
    FDTable table;
    Inode dummy{};
    int fd = table.alloc(&dummy, OpenFlags::RDONLY);
    ASSERT_EQ(fd, 3);
}

// Sequential allocs return monotonically increasing indices starting from 3.
TEST("fd_table: sequential allocs return 3 4 5") {
    FDTable table;
    Inode dummy{};

    ASSERT_EQ(table.alloc(&dummy, OpenFlags::RDONLY), 3);
    ASSERT_EQ(table.alloc(&dummy, OpenFlags::WRONLY), 4);
    ASSERT_EQ(table.alloc(&dummy, OpenFlags::RDWR),   5);
}

// ============================================================
// 3. Alloc -- table full
// ============================================================

// Filling all assignable slots (3-255) returns FD_NONE on next alloc.
TEST("fd_table: alloc returns FD_NONE when table is full") {
    FDTable table;
    Inode dummy{};

    for (uint32_t i = 3; i < FD_TABLE_SIZE; ++i) {
        int fd = table.alloc(&dummy, OpenFlags::RDWR);
        ASSERT_EQ(fd, static_cast<int>(i));
    }

    // Table is now full; next alloc must fail.
    int fd = table.alloc(&dummy, OpenFlags::RDWR);
    ASSERT_EQ(fd, FD_NONE);
}

// ============================================================
// 4. Alloc -- reuses lowest freed slot
// ============================================================

// After closing fd 4, the next alloc should return 4 (lowest free).
TEST("fd_table: alloc reuses lowest freed slot") {
    FDTable table;
    Inode dummy{};

    // Allocate 3, 4, 5
    table.alloc(&dummy, OpenFlags::RDONLY);
    table.alloc(&dummy, OpenFlags::RDONLY);
    table.alloc(&dummy, OpenFlags::RDONLY);

    // Close fd 4
    ASSERT_EQ(table.close(4), 0);

    // Next alloc should reuse slot 4
    int fd = table.alloc(&dummy, OpenFlags::WRONLY);
    ASSERT_EQ(fd, 4);
}

// Closing fd 3 when 3,4 are open; next alloc returns 3.
TEST("fd_table: alloc reuses slot 3 after closing it") {
    FDTable table;
    Inode dummy{};

    table.alloc(&dummy, OpenFlags::RDONLY);
    table.alloc(&dummy, OpenFlags::RDONLY);

    ASSERT_EQ(table.close(3), 0);
    ASSERT_EQ(table.alloc(&dummy, OpenFlags::RDWR), 3);
}

// ============================================================
// 5. Close -- normal operation
// ============================================================

// Close a valid open fd returns 0 and the slot becomes nullptr.
TEST("fd_table: close valid fd returns 0") {
    FDTable table;
    Inode dummy{};

    int fd = table.alloc(&dummy, OpenFlags::RDONLY);
    ASSERT_EQ(table.close(fd), 0);
    ASSERT_NULL(table.get(fd));
}

// ============================================================
// 6. Close -- invalid fd
// ============================================================

// Negative fd returns -1.
TEST("fd_table: close negative fd returns -1") {
    FDTable table;
    ASSERT_EQ(table.close(-1), -1);
}

// fd equal to FD_TABLE_SIZE returns -1.
TEST("fd_table: close fd equal to table size returns -1") {
    FDTable table;
    ASSERT_EQ(table.close(static_cast<int>(FD_TABLE_SIZE)), -1);
}

// fd greater than FD_TABLE_SIZE returns -1.
TEST("fd_table: close large fd returns -1") {
    FDTable table;
    ASSERT_EQ(table.close(9999), -1);
}

// Already-closed fd returns -1.
TEST("fd_table: close already closed fd returns -1") {
    FDTable table;
    Inode dummy{};

    int fd = table.alloc(&dummy, OpenFlags::RDONLY);
    ASSERT_EQ(table.close(fd), 0);
    ASSERT_EQ(table.close(fd), -1);
}

// Close on a slot that was never allocated returns -1.
TEST("fd_table: close never allocated fd returns -1") {
    FDTable table;
    ASSERT_EQ(table.close(0), -1);
}

// ============================================================
// 7. Get -- normal retrieval
// ============================================================

// get() on a valid open fd returns a non-null File with correct fields.
TEST("fd_table: get returns non-null for open fd") {
    FDTable table;
    Inode inode{};
    inode.ino = 42;

    int fd = table.alloc(&inode, OpenFlags::RDWR);
    File* f = table.get(fd);
    ASSERT_NOT_NULL(f);
    ASSERT_TRUE(f->inode == &inode);
    ASSERT_EQ(f->offset, 0ULL);
    ASSERT_TRUE(f->flags == OpenFlags::RDWR);
}

// Multiple open fds each return distinct File objects.
TEST("fd_table: distinct fds have distinct File objects") {
    FDTable table;
    Inode dummy{};

    int fd0 = table.alloc(&dummy, OpenFlags::RDONLY);
    int fd1 = table.alloc(&dummy, OpenFlags::WRONLY);

    File* f0 = table.get(fd0);
    File* f1 = table.get(fd1);

    ASSERT_NOT_NULL(f0);
    ASSERT_NOT_NULL(f1);
    ASSERT_TRUE(f0 != f1);
}

// ============================================================
// 8. Get -- out of range / invalid
// ============================================================

// Negative fd returns nullptr.
TEST("fd_table: get negative fd returns nullptr") {
    FDTable table;
    ASSERT_NULL(table.get(-1));
}

// fd equal to FD_TABLE_SIZE returns nullptr.
TEST("fd_table: get fd at table size returns nullptr") {
    FDTable table;
    ASSERT_NULL(table.get(static_cast<int>(FD_TABLE_SIZE)));
}

// Large positive fd returns nullptr.
TEST("fd_table: get large fd returns nullptr") {
    FDTable table;
    ASSERT_NULL(table.get(9999));
}

// Unallocated slot returns nullptr.
TEST("fd_table: get unallocated slot returns nullptr") {
    FDTable table;
    ASSERT_NULL(table.get(0));
}

// ============================================================
// 9. Get -- after close
// ============================================================

// After closing, get() returns nullptr.
TEST("fd_table: get returns nullptr after close") {
    FDTable table;
    Inode dummy{};

    int fd = table.alloc(&dummy, OpenFlags::RDONLY);
    ASSERT_NOT_NULL(table.get(fd));

    table.close(fd);
    ASSERT_NULL(table.get(fd));
}

// ============================================================
// 10. File field storage
// ============================================================

// OpenFlags::RDONLY is stored correctly in the File.
TEST("fd_table: File stores RDONLY flag") {
    FDTable table;
    Inode dummy{};

    int fd = table.alloc(&dummy, OpenFlags::RDONLY);
    File* f = table.get(fd);
    ASSERT_TRUE(f->flags == OpenFlags::RDONLY);
}

// OpenFlags::WRONLY is stored correctly in the File.
TEST("fd_table: File stores WRONLY flag") {
    FDTable table;
    Inode dummy{};

    int fd = table.alloc(&dummy, OpenFlags::WRONLY);
    File* f = table.get(fd);
    ASSERT_TRUE(f->flags == OpenFlags::WRONLY);
}

// OpenFlags::RDWR is stored correctly in the File.
TEST("fd_table: File stores RDWR flag") {
    FDTable table;
    Inode dummy{};

    int fd = table.alloc(&dummy, OpenFlags::RDWR);
    File* f = table.get(fd);
    ASSERT_TRUE(f->flags == OpenFlags::RDWR);
}

// Inode pointer is stored correctly.
TEST("fd_table: File stores correct inode pointer") {
    FDTable table;
    Inode inode{};
    inode.ino = 123;
    inode.size = 4096;

    int fd = table.alloc(&inode, OpenFlags::RDONLY);
    File* f = table.get(fd);
    ASSERT_TRUE(f->inode == &inode);
}

// Offset is initialised to 0.
TEST("fd_table: File offset initialised to 0") {
    FDTable table;
    Inode dummy{};

    int fd = table.alloc(&dummy, OpenFlags::RDWR);
    File* f = table.get(fd);
    ASSERT_EQ(f->offset, 0ULL);
}

// ============================================================
// 11. Stress: fill, close all, refill
// ============================================================

// Fill the assignable table, close every other descriptor, then realloc.
TEST("fd_table: fill close_even then realloc") {
    FDTable table;
    Inode dummy{};

    // Fill all assignable slots (3-255)
    for (uint32_t i = 3; i < FD_TABLE_SIZE; ++i) {
        int fd = table.alloc(&dummy, OpenFlags::RDWR);
        ASSERT_EQ(fd, static_cast<int>(i));
    }

    // Close even-numbered fds (starting from 3)
    for (uint32_t i = 4; i < FD_TABLE_SIZE; i += 2) {
        ASSERT_EQ(table.close(static_cast<int>(i)), 0);
    }

    // Realloc should fill even-numbered slots in order
    for (uint32_t i = 4; i < FD_TABLE_SIZE; i += 2) {
        int fd = table.alloc(&dummy, OpenFlags::RDWR);
        ASSERT_EQ(fd, static_cast<int>(i));
    }

    // Table should be full again
    ASSERT_EQ(table.alloc(&dummy, OpenFlags::RDWR), FD_NONE);
}

// Fill, close all, fill again -- entire lifecycle.
TEST("fd_table: full lifecycle fill close refill") {
    FDTable table;
    Inode dummy{};

    // Fill assignable slots (3-255)
    for (uint32_t i = 3; i < FD_TABLE_SIZE; ++i) {
        ASSERT_EQ(table.alloc(&dummy, OpenFlags::RDWR), static_cast<int>(i));
    }

    // Close all
    for (uint32_t i = 3; i < FD_TABLE_SIZE; ++i) {
        ASSERT_EQ(table.close(static_cast<int>(i)), 0);
    }

    // Verify all nullptr
    for (uint32_t i = 3; i < FD_TABLE_SIZE; ++i) {
        ASSERT_NULL(table.get(static_cast<int>(i)));
    }

    // Refill -- should start from 3 again
    for (uint32_t i = 3; i < FD_TABLE_SIZE; ++i) {
        ASSERT_EQ(table.alloc(&dummy, OpenFlags::RDWR), static_cast<int>(i));
    }
}

// ============================================================
// Main
// ============================================================

int main() {
    RUN_ALL_TESTS();
    return _tests_failed > 0 ? 1 : 0;
}

#endif  // CINUX_HOST_TEST
