/**
 * @file kernel/syscall/sys_pipe.cpp
 * @brief sys_pipe handler implementation
 *
 * Creates a Pipe on the kernel heap, wraps each end in an Inode with
 * the appropriate PipeReadOps / PipeWriteOps, allocates two fd slots
 * in the global FDTable, and writes the two descriptor numbers into
 * a user-space int[2] array.
 */

#include "kernel/syscall/sys_pipe.hpp"

#include <stdint.h>

#include "kernel/fs/file.hpp"
#include "kernel/fs/vfs_mount.hpp"
#include "kernel/ipc/pipe.hpp"
#include "kernel/ipc/pipe_ops.hpp"

namespace cinux::syscall {

namespace {

/**
 * @brief Validate a user-space virtual address
 *
 * Checks the canonical-address rules for x86_64 user space:
 *   - bit 47 must equal bits 48..63
 *   - bit 47 == 0 and upper == 0  =>  user-space low half
 *   - bit 47 == 1 and upper == 0xFFFF  =>  kernel-space (rejected)
 *
 * @param addr  The virtual address to validate
 * @return true if the address looks like a valid user-space pointer
 */
bool is_user_addr(uint64_t addr) {
    if (addr == 0) {
        return false;
    }
    uint64_t bit47 = (addr >> 47) & 1;
    uint64_t upper = addr >> 48;
    if (bit47 == 0 && upper != 0) {
        return false;
    }
    // Reject kernel-space addresses (bit 47 set)
    if (bit47 == 1) {
        return false;
    }
    return true;
}

}  // anonymous namespace

int64_t sys_pipe(uint64_t pipefd_virt, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
    // Step 1: Validate the user-space pointer to the int[2] array
    if (!is_user_addr(pipefd_virt)) {
        return -1;
    }

    // Step 2: Create the Pipe on the kernel heap
    auto* pipe = new cinux::ipc::Pipe();

    // Step 3: Create PipeReadOps and PipeWriteOps (also on the heap)
    auto* read_ops  = new cinux::ipc::PipeReadOps(pipe);
    auto* write_ops = new cinux::ipc::PipeWriteOps(pipe);

    // Step 4: Create two Inodes wrapping each end
    cinux::fs::Inode* read_inode = new cinux::fs::Inode();
    read_inode->ops              = read_ops;
    read_inode->type             = cinux::fs::InodeType::Regular;

    cinux::fs::Inode* write_inode = new cinux::fs::Inode();
    write_inode->ops              = write_ops;
    write_inode->type             = cinux::fs::InodeType::Regular;

    // Step 5: Allocate two fd slots in the current task's FDTable
    auto& table = cinux::fs::current_fd_table();

    int read_fd = table.alloc(read_inode, cinux::fs::OpenFlags::RDONLY);
    if (read_fd < 0) {
        // Cleanup on failure
        delete write_inode;
        delete read_inode;
        delete write_ops;
        delete read_ops;
        delete pipe;
        return -1;
    }

    int write_fd = table.alloc(write_inode, cinux::fs::OpenFlags::WRONLY);
    if (write_fd < 0) {
        // Close the already-allocated read_fd and cleanup
        table.close(read_fd);
        delete write_inode;
        delete read_inode;
        delete write_ops;
        delete read_ops;
        delete pipe;
        return -1;
    }

    // Step 6: Write the two fd numbers into user-space int[2]
    auto* pipefd = reinterpret_cast<int32_t*>(pipefd_virt);
    pipefd[0]    = read_fd;
    pipefd[1]    = write_fd;

    return 0;
}

}  // namespace cinux::syscall
