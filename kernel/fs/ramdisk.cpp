/**
 * @file kernel/fs/ramdisk.cpp
 * @brief Ramdisk driver implementation (ustar archive parser)
 */

#include "ramdisk.hpp"
#include "ramdisk_config.hpp"

#include <stdint.h>
#include <stddef.h>

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
// Internal helpers
// ============================================================

namespace {

/**
 * @brief Check whether a ustar header has a valid magic field
 *
 * Compares the magic field against the expected "ustar" string.
 * A header with all-zero name[0] indicates end-of-archive.
 *
 * @param hdr  Pointer to the header to validate
 * @return     true if the magic field matches "ustar"
 */
bool is_valid_ustar(const UstarHeader* hdr) {
    // Compare magic field byte-by-byte (5 chars + null terminator)
    for (uint32_t i = 0; i < 5; ++i) {
        if (hdr->magic[i] != USTAR_MAGIC[i]) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Calculate the number of 512-byte data blocks for a given file size
 *
 * The ustar format pads file data to 512-byte block boundaries.
 * A file of size 0 occupies no data blocks.
 *
 * @param size  File size in bytes
 * @return      Number of 512-byte blocks occupied by the file data
 */
uint32_t data_blocks(uint64_t size) {
    if (size == 0) {
        return 0;
    }
    return static_cast<uint32_t>((size + USTAR_BLOCK_SIZE - 1) / USTAR_BLOCK_SIZE);
}

/**
 * @brief Print a bounded-length string to kprintf (avoids buffer overflows)
 *
 * Outputs up to max_len characters from str, stopping at the first
 * null byte.  Does not append a newline.
 *
 * @param str      Pointer to the character string
 * @param max_len  Maximum number of characters to examine
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

    // Parse each character as an octal digit until null or space terminator
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];

        // Null or space terminates the octal string
        if (c == '\0' || c == ' ') {
            break;
        }

        // Accumulate: result = result * 8 + digit
        result = (result << 3) + static_cast<uint64_t>(c - '0');
    }

    return result;
}

uint32_t Ramdisk::mount() {
    // Step 1: Resolve archive boundaries from linker symbols
    base_ = _binary_initrd_start;
    size_ = static_cast<uint64_t>(_binary_initrd_end - _binary_initrd_start);

    if (base_ == nullptr || size_ == 0) {
        cinux::lib::kprintf("[RAMDISK] No initrd archive found.\n");
        return 0;
    }

    cinux::lib::kprintf("[RAMDISK] Archive at 0x%p, size %u bytes\n",
                        base_, size_);

    // Step 2: Iterate through ustar entries
    uint32_t entry_count = 0;
    uint64_t offset = 0;

    while (offset + sizeof(UstarHeader) <= size_) {
        auto* hdr = reinterpret_cast<const UstarHeader*>(base_ + offset);

        // Step 3: Check for end-of-archive (two consecutive all-zero headers)
        // A zero name[0] indicates no more entries
        if (hdr->name[0] == '\0') {
            break;
        }

        // Step 4: Validate ustar magic
        if (!is_valid_ustar(hdr)) {
            cinux::lib::kprintf("[RAMDISK] Invalid ustar magic at offset %u, stopping.\n",
                                offset);
            break;
        }

        // Step 5: Parse file size from octal field
        uint64_t file_size = octal_to_uint(hdr->size, sizeof(hdr->size));

        // Step 6: Print entry info based on type flag
        char type = hdr->typeflag;

        if (type == UstarType::REGULAR || type == UstarType::CONTIGUOUS) {
            cinux::lib::kprintf("[RAMDISK]   FILE: ");
            print_bounded(hdr->name, RAMDISK_NAME_MAX);
            cinux::lib::kprintf("  (%u bytes)\n", file_size);
            ++entry_count;
        } else if (type == UstarType::DIRECTORY) {
            cinux::lib::kprintf("[RAMDISK]   DIR:  ");
            print_bounded(hdr->name, RAMDISK_NAME_MAX);
            cinux::lib::kprintf("\n");
        }

        // Step 7: Advance past header + data blocks
        uint32_t blocks = data_blocks(file_size);
        offset += sizeof(UstarHeader) + static_cast<uint64_t>(blocks) * USTAR_BLOCK_SIZE;
    }

    cinux::lib::kprintf("[RAMDISK] %u file(s) found in initrd.\n", entry_count);

    return entry_count;
}

const void* Ramdisk::base() const {
    return base_;
}

uint64_t Ramdisk::total_size() const {
    return size_;
}

}  // namespace cinux::fs
