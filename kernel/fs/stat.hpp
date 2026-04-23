/**
 * @file kernel/fs/stat.hpp
 * @brief File status structure (struct stat)
 *
 * Defines the stat structure returned by sys_stat / sys_fstat.
 * Layout follows the Linux x86_64 convention where practical.
 *
 * Namespace: cinux::fs
 */

#pragma once

#include <stdint.h>

namespace cinux::fs {

/**
 * @brief File status information returned by stat() / fstat()
 *
 * Fields are filled by the filesystem backend from on-disk inode data.
 */
struct stat {
    uint64_t st_dev;      ///< Device ID
    uint64_t st_ino;      ///< Inode number
    uint32_t st_mode;     ///< File type and permissions
    uint32_t st_nlink;    ///< Number of hard links
    uint32_t st_uid;      ///< Owner user ID
    uint32_t st_gid;      ///< Owner group ID
    uint64_t st_rdev;     ///< Device ID (if special file)
    int64_t  st_size;     ///< Total file size in bytes
    uint64_t st_blksize;  ///< Preferred block size for I/O
    uint64_t st_blocks;   ///< Number of 512-byte blocks allocated
    uint64_t st_atime;    ///< Time of last access
    uint64_t st_mtime;    ///< Time of last modification
    uint64_t st_ctime;    ///< Time of last status change
};

}  // namespace cinux::fs
