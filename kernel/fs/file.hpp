/**
 * @file kernel/fs/file.hpp
 * @brief Open-file description and per-process file descriptor table
 *
 * Defines:
 *   - OpenFlags:  flags passed to sys_open (read/write/rdwr)
 *   - File:       an open file with a reference to its Inode, an offset,
 *                 and access flags
 *   - FDTable:    a fixed-size (256-entry) file descriptor table with
 *                 alloc() and close() operations
 *
 * FDTable is intended to be embedded in the Process / Task struct so that
 * each task gets its own descriptor table.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "kernel/fs/inode.hpp"

namespace cinux::fs {

// ============================================================
// Open Flags
// ============================================================

/// Access mode flags for sys_open
enum class OpenFlags : uint32_t {
    RDONLY = 0,
    WRONLY = 1,
    RDWR   = 2,
};

// ============================================================
// File Descriptor Limits
// ============================================================

/// Maximum number of open file descriptors per process
static constexpr uint32_t FD_TABLE_SIZE = 256;

/// Sentinel value indicating an unallocated fd slot
static constexpr int FD_NONE = -1;

// ============================================================
// File Structure (open file description)
// ============================================================

/**
 * @brief Represents an open file within a process
 *
 * A File ties an Inode pointer to an access mode and a seek offset.
 * Multiple File descriptors may share the same Inode but maintain
 * independent offsets.
 */
struct File {
    Inode*    inode;    ///< Pointer to the underlying inode (non-null when in use)
    uint64_t  offset;   ///< Current read/write offset in bytes
    OpenFlags flags;    ///< Access mode (RDONLY, WRONLY, RDWR)
};

// ============================================================
// File Descriptor Table
// ============================================================

/**
 * @brief Per-process file descriptor table
 *
 * Manages a fixed-size array of File pointers.  Descriptor 0 is
 * reserved for stdin, 1 for stdout, and 2 for stderr (allocated
 * externally by the shell / init setup).
 *
 * Lifetime: the FDTable owns the File objects; close() releases them.
 */
class FDTable {
public:
    /**
     * @brief Construct an empty descriptor table
     *
     * All slots initialised to nullptr.
     */
    FDTable();

    /**
     * @brief Allocate a file descriptor and assign a File to it
     *
     * Searches for the first free slot, creates a File entry for the
     * given inode and flags, and returns the descriptor index.
     *
     * @param inode  The inode to associate with the new descriptor
     * @param flags  Access mode for the open file
     * @return Non-negative descriptor index on success, or FD_NONE (-1)
     *         if the table is full
     */
    int alloc(Inode* inode, OpenFlags flags);

    /**
     * @brief Close a file descriptor and release its File entry
     *
     * @param fd  Descriptor index to close
     * @return 0 on success, or -1 if fd is out of range or already closed
     */
    int close(int fd);

    /**
     * @brief Retrieve the File pointer for a given descriptor
     *
     * @param fd  Descriptor index
     * @return Pointer to the File, or nullptr if fd is invalid / unused
     */
    File* get(int fd) const;

private:
    /// Fixed-size array of File pointers (nullptr = unused slot)
    File* fds_[FD_TABLE_SIZE];
};

}  // namespace cinux::fs
