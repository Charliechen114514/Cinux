/**
 * @file kernel/fs/ext2_inode.cpp
 * @brief Ext2 inode read/write, allocation, caching, and block pointer resolution
 *
 * Handles reading and writing on-disk inodes, inode bitmap allocation
 * and deallocation, the fixed-size inode cache, VFS inode population,
 * and the get_or_alloc_block() block-pointer resolver.
 */

#include <stddef.h>
#include <stdint.h>

#include "ext2.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"

namespace cinux::fs {

// ============================================================
// Disk inode read/write
// ============================================================

bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out_inode) {
    if (ino == 0) {
        return false;
    }

    uint32_t group = (ino - 1) / inodes_per_group_;

    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] Inode %u: group %u out of range\n", ino, group);
        return false;
    }

    uint32_t inode_table_block = bgdt_[group].bg_inode_table;
    uint32_t index_in_group    = (ino - 1) % inodes_per_group_;
    uint64_t byte_offset       = static_cast<uint64_t>(index_in_group) * inode_size_;

    uint32_t block_offset        = static_cast<uint32_t>(byte_offset / block_size_);
    uint32_t within_block_offset = static_cast<uint32_t>(byte_offset % block_size_);

    uint32_t target_block = inode_table_block + block_offset;

    if (!read_block(target_block)) {
        cinux::lib::kprintf("[EXT2] Failed to read inode block %u\n", target_block);
        return false;
    }

    auto* block_data = reinterpret_cast<const uint8_t*>(dma_buf_virt_);

    if (within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] Inode %u crosses block boundary\n", ino);
        return false;
    }

    memcpy(&out_inode, block_data + within_block_offset, sizeof(Ext2Inode));
    return true;
}

bool Ext2::write_disk_inode(uint32_t ino, const Ext2Inode& inode) {
    if (ino == 0) {
        return false;
    }

    uint32_t group = (ino - 1) / inodes_per_group_;

    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: ino %u group %u out of range\n", ino, group);
        return false;
    }

    uint32_t inode_table_block   = bgdt_[group].bg_inode_table;
    uint32_t index_in_group      = (ino - 1) % inodes_per_group_;
    uint64_t byte_offset         = static_cast<uint64_t>(index_in_group) * inode_size_;
    uint32_t block_offset        = static_cast<uint32_t>(byte_offset / block_size_);
    uint32_t within_block_offset = static_cast<uint32_t>(byte_offset % block_size_);
    uint32_t target_block        = inode_table_block + block_offset;

    if (!read_block(target_block)) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to read block %u\n", target_block);
        return false;
    }

    if (within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: ino %u crosses block boundary\n", ino);
        return false;
    }

    auto* block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    memcpy(block_data + within_block_offset, &inode, sizeof(Ext2Inode));

    if (!write_block(target_block)) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to write block %u\n", target_block);
        return false;
    }

    return true;
}

// ============================================================
// Inode cache management
// ============================================================

Inode* Ext2::get_cached_inode(uint32_t ino) {
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (inode_cache_[i].in_use && inode_cache_[i].ino == ino) {
            return &inode_cache_[i].vfs_inode;
        }
    }

    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (!inode_cache_[i].in_use) {
            if (!read_disk_inode(ino, inode_cache_[i].disk_inode)) {
                return nullptr;
            }

            inode_cache_[i].ino    = ino;
            inode_cache_[i].in_use = true;
            populate_vfs_inode(inode_cache_[i]);
            return &inode_cache_[i].vfs_inode;
        }
    }

    // Cache full -- evict slot 1+ (slot 0 is always root)
    uint32_t evict = 1 + (ino % (EXT2_INODE_CACHE_SIZE - 1));

    inode_cache_[evict].in_use = false;
    if (!read_disk_inode(ino, inode_cache_[evict].disk_inode)) {
        return nullptr;
    }

    inode_cache_[evict].ino    = ino;
    inode_cache_[evict].in_use = true;
    populate_vfs_inode(inode_cache_[evict]);
    return &inode_cache_[evict].vfs_inode;
}

void Ext2::populate_vfs_inode(Ext2CachedInode& cached) {
    const Ext2Inode& disk = cached.disk_inode;

    cached.vfs_inode.ino = cached.ino;

    cached.vfs_inode.size = disk.i_size;

    uint16_t mode_type = disk.i_mode & EXT2_S_IFMT;
    if (mode_type == EXT2_S_IFDIR) {
        cached.vfs_inode.type = InodeType::Directory;
        cached.vfs_inode.ops  = &dir_ops_;
    } else if (mode_type == EXT2_S_IFREG) {
        cached.vfs_inode.type = InodeType::Regular;
        cached.vfs_inode.ops  = &file_ops_;
    } else {
        cached.vfs_inode.type = InodeType::Unknown;
        cached.vfs_inode.ops  = nullptr;
    }

    cached.vfs_inode.fs_private = &cached;

    cached.vfs_inode.mode   = disk.i_mode;
    cached.vfs_inode.uid    = disk.i_uid;
    cached.vfs_inode.gid    = disk.i_gid;
    cached.vfs_inode.nlink  = disk.i_links_count;
    cached.vfs_inode.atime  = disk.i_atime;
    cached.vfs_inode.ctime  = disk.i_ctime;
    cached.vfs_inode.mtime  = disk.i_mtime;
    cached.vfs_inode.blocks = disk.i_blocks;
}

// ============================================================
// Inode allocator
// ============================================================

uint32_t Ext2::alloc_inode() {
    if (!mounted_) {
        return 0;
    }

    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_inodes_count == 0) {
            continue;
        }

        uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
        if (bitmap_block == 0) {
            continue;
        }

        if (!read_block(bitmap_block)) {
            cinux::lib::kprintf("[EXT2] alloc_inode: failed to read bitmap block %u\n",
                                bitmap_block);
            return 0;
        }

        auto*    bitmap          = reinterpret_cast<uint8_t*>(dma_buf_virt_);
        uint32_t inodes_in_group = inodes_per_group_;
        uint32_t bytes_needed    = (inodes_in_group + 7) / 8;

        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (bitmap[byte_idx] == 0xFF) {
                continue;
            }

            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local_index = byte_idx * 8 + bit;
                if (local_index >= inodes_in_group) {
                    break;
                }

                if ((bitmap[byte_idx] & (1U << bit)) == 0) {
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);

                    if (!write_block(bitmap_block)) {
                        cinux::lib::kprintf("[EXT2] alloc_inode: failed to write bitmap\n");
                        return 0;
                    }

                    uint32_t global_ino = group * inodes_per_group_ + local_index + 1;

                    if (sb_.s_free_inodes_count > 0) {
                        --sb_.s_free_inodes_count;
                    }

                    if (bgdt_[group].bg_free_inodes_count > 0) {
                        --bgdt_[group].bg_free_inodes_count;
                    }

                    write_superblock();
                    write_bgdt(group);

                    return global_ino;
                }
            }
        }
    }

    cinux::lib::kprintf("[EXT2] alloc_inode: no free inodes available\n");
    return 0;
}

bool Ext2::free_inode(uint32_t ino) {
    if (ino == 0 || !mounted_) {
        return false;
    }

    uint32_t group = (ino - 1) / inodes_per_group_;
    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] free_inode: ino %u group out of range\n", ino);
        return false;
    }

    uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
    if (bitmap_block == 0) {
        return false;
    }

    uint32_t local_index = (ino - 1) % inodes_per_group_;

    if (!read_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_inode: failed to read bitmap block %u\n", bitmap_block);
        return false;
    }

    uint32_t byte_idx = local_index / 8;
    uint32_t bit      = local_index % 8;

    auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    if (!write_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_inode: failed to write bitmap\n");
        return false;
    }

    ++sb_.s_free_inodes_count;
    ++bgdt_[group].bg_free_inodes_count;

    write_superblock();
    write_bgdt(group);

    return true;
}

// ============================================================
// Block pointer resolver with allocation
// ============================================================

uint32_t Ext2::get_or_alloc_block(Ext2Inode& disk, uint32_t file_block) {
    if (file_block < EXT2_DIRECT_BLOCKS) {
        if (disk.i_block[file_block] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(blk)) {
                free_block(blk);
                return 0;
            }

            disk.i_block[file_block] = blk;
        }

        return disk.i_block[file_block];
    }

    if (file_block < EXT2_DIRECT_BLOCKS + block_size_ / sizeof(uint32_t)) {
        uint32_t indirect_idx = file_block - EXT2_DIRECT_BLOCKS;

        if (disk.i_block[EXT2_INDIRECT_BLOCK] == 0) {
            uint32_t indirect_blk = alloc_block();
            if (indirect_blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(indirect_blk)) {
                free_block(indirect_blk);
                return 0;
            }

            disk.i_block[EXT2_INDIRECT_BLOCK] = indirect_blk;
        }

        uint32_t indirect_blk = disk.i_block[EXT2_INDIRECT_BLOCK];

        if (!read_block(indirect_blk)) {
            return 0;
        }

        auto* indirect = reinterpret_cast<uint32_t*>(dma_buf_virt_);

        if (indirect[indirect_idx] == 0) {
            uint32_t data_blk = alloc_block();
            if (data_blk == 0) {
                return 0;
            }

            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(data_blk)) {
                free_block(data_blk);
                return 0;
            }

            if (!read_block(indirect_blk)) {
                return 0;
            }
            indirect               = reinterpret_cast<uint32_t*>(dma_buf_virt_);
            indirect[indirect_idx] = data_blk;
            if (!write_block(indirect_blk)) {
                return 0;
            }
        }

        if (!read_block(indirect_blk)) {
            return 0;
        }
        indirect = reinterpret_cast<uint32_t*>(dma_buf_virt_);
        return indirect[indirect_idx];
    }

    return 0;
}

}  // namespace cinux::fs
