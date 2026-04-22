/**
 * @file kernel/fs/file.cpp
 * @brief Implementation of FDTable -- per-process file descriptor table
 *
 * Provides alloc() / close() / get() operations on a fixed-size (256-entry)
 * array of File pointers.
 *
 * Namespace: cinux::fs
 */

#include "kernel/fs/file.hpp"

namespace cinux::fs {

// ============================================================
// Construction
// ============================================================

FDTable::FDTable() {
    for (uint32_t i = 0; i < FD_TABLE_SIZE; ++i) {
        fds_[i] = nullptr;
    }
}

// ============================================================
// Alloc
// ============================================================

/// First assignable fd (0=stdin, 1=stdout, 2=stderr are reserved)
static constexpr uint32_t FD_FIRST = 3;

int FDTable::alloc(Inode* inode, OpenFlags flags) {
    for (uint32_t i = FD_FIRST; i < FD_TABLE_SIZE; ++i) {
        if (fds_[i] == nullptr) {
            fds_[i] = new File{inode, 0, flags};
            return static_cast<int>(i);
        }
    }
    return FD_NONE;
}

// ============================================================
// Close
// ============================================================

int FDTable::close(int fd) {
    if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
        return -1;
    }
    if (fds_[fd] == nullptr) {
        return -1;
    }
    delete fds_[fd];
    fds_[fd] = nullptr;
    return 0;
}

// ============================================================
// Get
// ============================================================

File* FDTable::get(int fd) const {
    if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
        return nullptr;
    }
    return fds_[fd];
}

}  // namespace cinux::fs
