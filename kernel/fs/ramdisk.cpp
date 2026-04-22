/**
 * @file kernel/fs/ramdisk.cpp
 * @brief Ramdisk driver implementation (ustar archive parser with VFS support)
 *
 * Parses the embedded ustar initrd archive and builds an internal entry
 * table.  Each file entry gets a pre-allocated Inode with Ramdisk-specific
 * InodeOps (read from archive data, write returns error, readdir lists
 * entries).
 */

#include "ramdisk.hpp"
#include "ramdisk_config.hpp"

#include <stdint.h>
#include <stddef.h>
#include "kernel/lib/string.hpp"

#include "kernel/lib/kprintf.hpp"

namespace cinux::fs {

// ============================================================
// Linker symbols for embedded initrd
// ============================================================

extern "C" {
/// Start of the embedded initrd archive (set by linker)
extern const uint8_t _binary_initrd_start[];
/// End of the embedded initrd archive (set by linker)
extern const uint8_t _binary_initrd_end[];
}

// ============================================================
// Ramdisk InodeOps
// ============================================================

namespace {

/**
 * @brief Read bytes from a ramdisk inode at a given offset
 *
 * Copies data from the ramdisk archive into the caller's buffer.
 * The file data pointer and size are stored in the RamdiskEntry
 * referenced by the inode's fs_private field.
 *
 * @param inode   The inode to read from
 * @param offset  Byte offset within the file
 * @param buf     Destination buffer (kernel address)
 * @param count   Number of bytes to read
 * @return Number of bytes actually read, or -1 on error
 */
int64_t ramdisk_read(const Inode* inode, uint64_t offset,
                     void* buf, uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return -1;
    }

    auto* entry = static_cast<const RamdiskEntry*>(inode->fs_private);

    if (offset >= entry->size) {
        return 0;
    }

    uint64_t available = entry->size - offset;
    uint64_t to_read = (count < available) ? count : available;

    const auto* src = static_cast<const uint8_t*>(entry->data) + offset;
    memcpy(buf, src, to_read);

    return static_cast<int64_t>(to_read);
}

/**
 * @brief Write bytes to a ramdisk inode (read-only, always returns error)
 */
int64_t ramdisk_write(Inode*, uint64_t, const void*, uint64_t) {
    return -1;
}

/**
 * @brief Read the next directory entry name from the ramdisk
 *
 * Treats the entire ramdisk as a flat directory.  The index maps to
 * the entry table: entry 0 is ".", entry 1 is "..", and entries
 * 2..N+1 are the actual file entries.
 *
 * The directory inode's fs_private points to a RamdiskDirContext that
 * holds the entry table pointer and entry count, set up during mount().
 *
 * @param inode      Directory inode
 * @param index      Entry index (0-based)
 * @param name       Output buffer for the entry name
 * @param name_max   Size of the output buffer
 * @return 1 if an entry was read, 0 if no more entries, -1 on error
 */
int64_t ramdisk_readdir(const Inode* inode, uint64_t index,
                        char* name, uint64_t name_max) {
    if (inode == nullptr || inode->fs_private == nullptr ||
        name == nullptr || name_max == 0) {
        return -1;
    }

    auto* ctx = static_cast<const RamdiskDirContext*>(inode->fs_private);

    if (index == 0) {
        // "." entry
        if (name_max < 2) {
            return -1;
        }
        name[0] = '.';
        name[1] = '\0';
        return 1;
    }

    if (index == 1) {
        // ".." entry
        if (name_max < 3) {
            return -1;
        }
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        return 1;
    }

    uint64_t file_index = index - 2;
    if (file_index >= ctx->count) {
        return 0;
    }

    // Copy the entry name from the entry table
    const char* src = ctx->entries[file_index].name;
    uint64_t copy_len = name_max - 1;
    uint64_t i = 0;
    while (src[i] != '\0' && i < copy_len) {
        name[i] = src[i];
        ++i;
    }
    name[i] = '\0';
    return 1;
}

/// Static InodeOps instance for regular file inodes
InodeOps ramdisk_file_ops = {
    ramdisk_read,
    ramdisk_write,
    nullptr,  // no readdir for regular files
};

/// Static InodeOps instance for the root directory inode
InodeOps ramdisk_dir_ops = {
    nullptr,          // no read on directories
    ramdisk_write,    // still returns -1
    ramdisk_readdir,
};

// ============================================================
// Internal helpers
// ============================================================

/**
 * @brief Check whether a ustar header has a valid magic field
 */
bool is_valid_ustar(const UstarHeader* hdr) {
    for (uint32_t i = 0; i < 5; ++i) {
        if (hdr->magic[i] != USTAR_MAGIC[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Calculate the number of 512-byte data blocks for a given file size
 */
uint32_t data_blocks(uint64_t size) {
    if (size == 0) {
        return 0;
    }
    return static_cast<uint32_t>((size + USTAR_BLOCK_SIZE - 1) / USTAR_BLOCK_SIZE);
}

/**
 * @brief Print a bounded-length string to kprintf (avoids buffer overflows)
 */
void print_bounded(const char* str, uint32_t max_len) {
    for (uint32_t i = 0; i < max_len; ++i) {
        if (str[i] == '\0') {
            break;
        }
        cinux::lib::kprintf("%c", str[i]);
    }
}

}  // anonymous namespace

// ============================================================
// Public interface
// ============================================================

uint64_t octal_to_uint(const char* s, size_t len) {
    uint64_t result = 0;

    for (size_t i = 0; i < len; ++i) {
        char c = s[i];

        if (c == '\0' || c == ' ') {
            break;
        }

        result = (result << 3) + static_cast<uint64_t>(c - '0');
    }

    return result;
}

bool Ramdisk::mount() {
    // Step 1: Resolve archive boundaries from linker symbols
    base_ = _binary_initrd_start;
    size_ = static_cast<uint64_t>(_binary_initrd_end - _binary_initrd_start);

    if (base_ == nullptr || size_ == 0) {
        cinux::lib::kprintf("[RAMDISK] No initrd archive found.\n");
        return false;
    }

    cinux::lib::kprintf("[RAMDISK] Archive at 0x%p, size %u bytes\n",
                        base_, size_);

    // Step 2: Iterate through ustar entries and build the entry table
    entry_count_ = 0;
    uint64_t offset = 0;

    while (offset + sizeof(UstarHeader) <= size_) {
        auto* hdr = reinterpret_cast<const UstarHeader*>(base_ + offset);

        // End-of-archive: zero name[0]
        if (hdr->name[0] == '\0') {
            break;
        }

        // Validate ustar magic
        if (!is_valid_ustar(hdr)) {
            cinux::lib::kprintf("[RAMDISK] Invalid ustar magic at offset %u, stopping.\n",
                                offset);
            break;
        }

        // Parse file size from octal field
        uint64_t file_size = octal_to_uint(hdr->size, sizeof(hdr->size));

        // Process by type flag
        char type = hdr->typeflag;

        if (type == UstarType::REGULAR || type == UstarType::CONTIGUOUS) {
            if (entry_count_ < RAMDISK_MAX_ENTRIES) {
                auto& entry = entries_[entry_count_];

                // Copy the file name (null-terminated)
                uint32_t name_len = 0;
                while (name_len < RAMDISK_NAME_MAX - 1 &&
                       hdr->name[name_len] != '\0') {
                    entry.name[name_len] = hdr->name[name_len];
                    ++name_len;
                }
                entry.name[name_len] = '\0';

                entry.size = file_size;
                entry.data = base_ + offset + sizeof(UstarHeader);

                // Set up the Inode for this entry
                entry.inode.ino = entry_count_;
                entry.inode.size = file_size;
                entry.inode.type = InodeType::Regular;
                entry.inode.ops = &ramdisk_file_ops;
                entry.inode.fs_private = &entry;

                cinux::lib::kprintf("[RAMDISK]   FILE: ");
                print_bounded(entry.name, RAMDISK_NAME_MAX);
                cinux::lib::kprintf("  (%u bytes)\n", file_size);

                ++entry_count_;
            } else {
                cinux::lib::kprintf("[RAMDISK] Entry table full, skipping remaining entries.\n");
                break;
            }
        } else if (type == UstarType::DIRECTORY) {
            cinux::lib::kprintf("[RAMDISK]   DIR:  ");
            print_bounded(hdr->name, RAMDISK_NAME_MAX);
            cinux::lib::kprintf("\n");
        }

        // Advance past header + data blocks
        uint32_t blocks = data_blocks(file_size);
        offset += sizeof(UstarHeader) + static_cast<uint64_t>(blocks) * USTAR_BLOCK_SIZE;
    }

    cinux::lib::kprintf("[RAMDISK] %u file(s) found in initrd.\n", entry_count_);

    // Set up the root directory inode for readdir support
    root_ctx_.entries = entries_;
    root_ctx_.count = entry_count_;
    root_inode_.ino = 0;
    root_inode_.size = 0;
    root_inode_.type = InodeType::Directory;
    root_inode_.ops = &ramdisk_dir_ops;
    root_inode_.fs_private = &root_ctx_;

    return entry_count_ > 0;
}

Inode* Ramdisk::lookup(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }

    // Special case: root directory lookup
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return &root_inode_;
    }

    // Skip leading '/' if present
    if (path[0] == '/') {
        ++path;
    }

    // Linear search through the entry table
    for (uint32_t i = 0; i < entry_count_; ++i) {
        // Compare the path with the entry name
        const char* entry_name = entries_[i].name;
        uint32_t j = 0;
        while (entry_name[j] != '\0' && path[j] != '\0') {
            if (entry_name[j] != path[j]) {
                break;
            }
            ++j;
        }
        if (entry_name[j] == '\0' && path[j] == '\0') {
            return &entries_[i].inode;
        }
    }

    return nullptr;
}

const void* Ramdisk::base() const {
    return base_;
}

uint64_t Ramdisk::total_size() const {
    return size_;
}

uint32_t Ramdisk::entry_count() const {
    return entry_count_;
}

}  // namespace cinux::fs
