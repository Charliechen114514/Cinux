/**
 * @file kernel/ipc/pipe_ops.cpp
 * @brief PipeReadOps and PipeWriteOps implementations
 *
 * Thin VFS adapters that delegate to the underlying Pipe object.
 * The offset parameter from InodeOps is intentionally ignored because
 * pipes are non-seekable byte streams.
 */

#include "kernel/ipc/pipe_ops.hpp"

#include "kernel/ipc/pipe.hpp"

namespace cinux::ipc {

// ============================================================
// PipeReadOps
// ============================================================

PipeReadOps::PipeReadOps(Pipe* pipe) : pipe_(pipe) {}

int64_t PipeReadOps::read(const cinux::fs::Inode*, uint64_t, void* buf, uint64_t count) {
    if (pipe_ == nullptr || buf == nullptr) {
        return -1;
    }
    return pipe_->read(static_cast<char*>(buf), count);
}

// ============================================================
// PipeWriteOps
// ============================================================

PipeWriteOps::PipeWriteOps(Pipe* pipe) : pipe_(pipe) {}

int64_t PipeWriteOps::write(cinux::fs::Inode*, uint64_t, const void* buf, uint64_t count) {
    if (pipe_ == nullptr || buf == nullptr) {
        return -1;
    }
    return pipe_->write(static_cast<const char*>(buf), count);
}

}  // namespace cinux::ipc
