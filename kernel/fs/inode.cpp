/**
 * @file kernel/fs/inode.cpp
 * @brief Default InodeOps method implementations
 *
 * Unsupported operations return -1 or nullptr by default.
 */

#include "inode.hpp"

namespace cinux::fs {

int64_t InodeOps::read(const Inode*, uint64_t, void*, uint64_t) {
    return -1;
}

int64_t InodeOps::write(Inode*, uint64_t, const void*, uint64_t) {
    return -1;
}

int64_t InodeOps::readdir(const Inode*, uint64_t, char*, uint64_t) {
    return -1;
}

Inode* InodeOps::create(Inode*, const char*, uint32_t) {
    return nullptr;
}

Inode* InodeOps::mkdir(Inode*, const char*, uint32_t) {
    return nullptr;
}

int64_t InodeOps::unlink(Inode*, const char*, uint32_t) {
    return -1;
}

int64_t InodeOps::stat(const Inode*, struct stat*) {
    return -1;
}

}  // namespace cinux::fs
