/**
 * @file kernel/fs/ext2_block.cpp
 * @brief Ext2 data block allocation and deallocation
 *
 * Manages the block bitmap: scanning for free blocks, marking them
 * used/unused, and updating the superblock and block group descriptor
 * free-block counts on each allocation or release.
 */

#include <stdint.h>

#include "ext2.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::fs {

// ============================================================
// Block allocator
// ============================================================

uint32_t Ext2::alloc_block() {
    if (!mounted_) {
        return 0;
    }

    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_blocks_count == 0) {
            continue;
        }

        uint32_t bitmap_block = bgdt_[group].bg_block_bitmap;
        if (bitmap_block == 0) {
            continue;
        }

        if (!read_block(bitmap_block)) {
            cinux::lib::kprintf("[EXT2] alloc_block: failed to read bitmap block %u\n",
                                bitmap_block);
            return 0;
        }

        auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);

        uint32_t blocks_in_group = blocks_per_group_;
        uint32_t first_block     = group * blocks_per_group_ + first_data_block_;

        uint32_t total_blocks = sb_.s_blocks_count;
        if (first_block + blocks_in_group > total_blocks) {
            blocks_in_group = total_blocks - first_block;
        }

        uint32_t bytes_needed = (blocks_in_group + 7) / 8;

        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (bitmap[byte_idx] == 0xFF) {
                continue;
            }

            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local_block = byte_idx * 8 + bit;
                if (local_block >= blocks_in_group) {
                    break;
                }

                if ((bitmap[byte_idx] & (1U << bit)) == 0) {
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);

                    if (!write_block(bitmap_block)) {
                        cinux::lib::kprintf("[EXT2] alloc_block: failed to write bitmap\n");
                        return 0;
                    }

                    uint32_t global_block = first_block + local_block;

                    if (sb_.s_free_blocks_count > 0) {
                        --sb_.s_free_blocks_count;
                    }

                    if (bgdt_[group].bg_free_blocks_count > 0) {
                        --bgdt_[group].bg_free_blocks_count;
                    }

                    write_superblock();
                    write_bgdt(group);

                    return global_block;
                }
            }
        }
    }

    cinux::lib::kprintf("[EXT2] alloc_block: no free blocks available\n");
    return 0;
}

bool Ext2::free_block(uint32_t block_num) {
    if (block_num == 0 || !mounted_) {
        return false;
    }

    uint32_t group = (block_num - first_data_block_) / blocks_per_group_;
    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] free_block: block %u group out of range\n", block_num);
        return false;
    }

    uint32_t bitmap_block = bgdt_[group].bg_block_bitmap;
    if (bitmap_block == 0) {
        return false;
    }

    uint32_t local_block = block_num - (group * blocks_per_group_ + first_data_block_);

    if (!read_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_block: failed to read bitmap block %u\n", bitmap_block);
        return false;
    }

    uint32_t byte_idx = local_block / 8;
    uint32_t bit      = local_block % 8;

    auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    if (!write_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_block: failed to write bitmap\n");
        return false;
    }

    ++sb_.s_free_blocks_count;
    ++bgdt_[group].bg_free_blocks_count;

    write_superblock();
    write_bgdt(group);

    return true;
}

}  // namespace cinux::fs
