/**
 * @file kernel/fs/ext2_directory.cpp
 * @brief Ext2 directory entry manipulation and file/dir creation/deletion
 *
 * Handles adding and removing directory entries within data blocks,
 * creating regular files (create), creating subdirectories (mkdir),
 * and unlinking entries with automatic resource cleanup (unlink).
 */

#include <stddef.h>
#include <stdint.h>

#include "ext2.hpp"
#include "kernel/lib/kprintf.hpp"

namespace cinux::fs {

// ============================================================
// Directory entry insertion / removal
// ============================================================

bool Ext2::add_dir_entry(uint32_t dir_ino, Ext2Inode& dir_disk, uint32_t entry_ino,
                         const char* name, uint32_t name_len, Ext2FileType file_type) {
    uint32_t required_rec_len = EXT2_DIR_ENTRY_HDR_SIZE + name_len;
    required_rec_len          = (required_rec_len + 3) & ~3u;

    uint32_t bs           = block_size_;
    uint32_t total_blocks = (dir_disk.i_size + bs - 1) / bs;
    if (total_blocks == 0) {
        total_blocks = 1;
    }

    // Try to find space in existing blocks by splitting the last entry
    for (uint32_t b = 0; b < total_blocks && b < EXT2_DIRECT_BLOCKS; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!read_block(blk)) {
            return false;
        }

        auto*    block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
        uint32_t pos        = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<Ext2DirEntry*>(block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            uint32_t entry_min = EXT2_DIR_ENTRY_HDR_SIZE + entry->name_len;
            entry_min          = (entry_min + 3) & ~3u;

            uint32_t extra = entry->rec_len - entry_min;

            if (extra >= required_rec_len) {
                entry->rec_len = static_cast<uint16_t>(entry_min);

                auto* new_entry     = reinterpret_cast<Ext2DirEntry*>(block_data + pos + entry_min);
                new_entry->inode    = entry_ino;
                new_entry->rec_len  = static_cast<uint16_t>(extra);
                new_entry->name_len = static_cast<uint8_t>(name_len);
                new_entry->file_type = static_cast<uint8_t>(file_type);

                for (uint32_t i = 0; i < name_len; ++i) {
                    new_entry->name[i] = name[i];
                }

                if (!write_block(blk)) {
                    return false;
                }

                return true;
            }

            pos += entry->rec_len;
        }
    }

    // No space in existing blocks — allocate a new block
    uint32_t new_block_idx = total_blocks;
    if (new_block_idx >= EXT2_DIRECT_BLOCKS) {
        cinux::lib::kprintf("[EXT2] add_dir_entry: directory full (max direct blocks)\n");
        return false;
    }

    uint32_t new_blk = alloc_block();
    if (new_blk == 0) {
        cinux::lib::kprintf("[EXT2] add_dir_entry: failed to allocate new dir block\n");
        return false;
    }

    auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    for (uint32_t i = 0; i < bs; ++i) {
        dma[i] = 0;
    }

    auto* new_entry      = reinterpret_cast<Ext2DirEntry*>(dma);
    new_entry->inode     = entry_ino;
    new_entry->rec_len   = static_cast<uint16_t>(bs);
    new_entry->name_len  = static_cast<uint8_t>(name_len);
    new_entry->file_type = static_cast<uint8_t>(file_type);

    for (uint32_t i = 0; i < name_len; ++i) {
        new_entry->name[i] = name[i];
    }

    if (!write_block(new_blk)) {
        free_block(new_blk);
        return false;
    }

    dir_disk.i_block[new_block_idx] = new_blk;
    dir_disk.i_size += bs;

    uint32_t sectors_used = ((dir_disk.i_size + bs - 1) / bs) * (bs / 512);
    dir_disk.i_blocks     = sectors_used;

    if (!write_disk_inode(dir_ino, dir_disk)) {
        return false;
    }

    return true;
}

bool Ext2::remove_dir_entry(uint32_t /*dir_ino*/, const Ext2Inode& dir_disk, const char* name,
                            uint32_t name_len, uint32_t& out_entry_ino) {
    uint32_t bs           = block_size_;
    uint32_t dir_size     = dir_disk.i_size;
    uint32_t total_blocks = (dir_size + bs - 1) / bs;
    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;
    }

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!read_block(blk)) {
            return false;
        }

        auto*    block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
        uint32_t pos        = 0;
        uint32_t prev_pos   = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<Ext2DirEntry*>(block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0 && entry->name_len == name_len) {
                bool match = true;
                for (uint32_t i = 0; i < name_len; ++i) {
                    if (entry->name[i] != name[i]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    out_entry_ino = entry->inode;

                    if (pos == 0) {
                        entry->inode = 0;
                    } else {
                        auto* prev = reinterpret_cast<Ext2DirEntry*>(block_data + prev_pos);
                        prev->rec_len += entry->rec_len;
                    }

                    if (!write_block(blk)) {
                        return false;
                    }

                    return true;
                }
            }

            prev_pos = pos;
            pos += entry->rec_len;
        }
    }

    return false;
}

// ============================================================
// File / directory creation
// ============================================================

Inode* Ext2::create(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) {
        return nullptr;
    }

    if (lookup_in_dir(parent_ino, name, name_len) != 0) {
        return nullptr;
    }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) {
        cinux::lib::kprintf("[EXT2] create: no free inodes\n");
        return nullptr;
    }

    Ext2Inode new_disk;
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
    }

    new_disk.i_mode        = EXT2_S_IFREG | 0644;
    new_disk.i_uid         = 0;
    new_disk.i_size        = 0;
    new_disk.i_atime       = 0;
    new_disk.i_ctime       = 0;
    new_disk.i_mtime       = 0;
    new_disk.i_dtime       = 0;
    new_disk.i_gid         = 0;
    new_disk.i_links_count = 1;
    new_disk.i_blocks      = 0;
    new_disk.i_flags       = 0;

    if (!write_disk_inode(new_ino, new_disk)) {
        free_inode(new_ino);
        return nullptr;
    }

    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Regular)) {
        free_inode(new_ino);
        return nullptr;
    }

    if (!write_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    return get_cached_inode(new_ino);
}

Inode* Ext2::mkdir(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) {
        return nullptr;
    }

    if (lookup_in_dir(parent_ino, name, name_len) != 0) {
        cinux::lib::kprintf("[EXT2] mkdir: '%s' already exists\n", name);
        return nullptr;
    }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) {
        cinux::lib::kprintf("[EXT2] mkdir: no free inodes\n");
        return nullptr;
    }

    uint32_t data_blk = alloc_block();
    if (data_blk == 0) {
        cinux::lib::kprintf("[EXT2] mkdir: no free blocks\n");
        free_inode(new_ino);
        return nullptr;
    }

    Ext2Inode new_disk;
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
    }

    new_disk.i_mode        = EXT2_S_IFDIR | 0755;
    new_disk.i_uid         = 0;
    new_disk.i_size        = block_size_;
    new_disk.i_atime       = 0;
    new_disk.i_ctime       = 0;
    new_disk.i_mtime       = 0;
    new_disk.i_dtime       = 0;
    new_disk.i_gid         = 0;
    new_disk.i_links_count = 2;
    new_disk.i_blocks      = block_size_ / 512;
    new_disk.i_flags       = 0;
    new_disk.i_block[0]    = data_blk;

    if (!write_disk_inode(new_ino, new_disk)) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    // Initialise the data block with "." and ".." entries
    auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    for (uint32_t i = 0; i < block_size_; ++i) {
        dma[i] = 0;
    }

    auto* dot            = reinterpret_cast<Ext2DirEntry*>(dma);
    dot->inode           = new_ino;
    dot->name_len        = 1;
    dot->file_type       = static_cast<uint8_t>(Ext2FileType::Directory);
    dot->name[0]         = '.';
    uint32_t dot_rec_len = EXT2_DIR_ENTRY_HDR_SIZE + 1;
    dot_rec_len          = (dot_rec_len + 3) & ~3u;
    dot->rec_len         = static_cast<uint16_t>(dot_rec_len);

    auto* dotdot      = reinterpret_cast<Ext2DirEntry*>(dma + dot_rec_len);
    dotdot->inode     = parent_ino;
    dotdot->name_len  = 2;
    dotdot->file_type = static_cast<uint8_t>(Ext2FileType::Directory);
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';
    dotdot->rec_len   = static_cast<uint16_t>(block_size_ - dot_rec_len);

    if (!write_block(data_blk)) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Directory)) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    dir_disk.i_links_count++;

    uint32_t new_group = (new_ino - 1) / inodes_per_group_;
    if (new_group < group_count_) {
        bgdt_[new_group].bg_used_dirs_count++;
        write_bgdt(new_group);
    }

    if (!write_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    return get_cached_inode(new_ino);
}

// ============================================================
// Unlink
// ============================================================

int Ext2::unlink(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0) {
        return -1;
    }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return -1;
    }

    uint32_t entry_ino = 0;
    if (!remove_dir_entry(parent_ino, dir_disk, name, name_len, entry_ino)) {
        cinux::lib::kprintf("[EXT2] unlink: '%s' not found\n", name);
        return -1;
    }

    Ext2Inode target_disk;
    if (!read_disk_inode(entry_ino, target_disk)) {
        return -1;
    }

    if (target_disk.i_links_count > 0) {
        target_disk.i_links_count--;
    }

    if (target_disk.i_links_count == 0) {
        uint32_t bs = block_size_;

        // Free direct blocks (0-11)
        for (uint32_t i = 0; i < EXT2_DIRECT_BLOCKS; ++i) {
            if (target_disk.i_block[i] != 0) {
                free_block(target_disk.i_block[i]);
                target_disk.i_block[i] = 0;
            }
        }

        // Free singly-indirect block and its referenced data blocks
        if (target_disk.i_block[EXT2_INDIRECT_BLOCK] != 0) {
            uint32_t indirect_blk = target_disk.i_block[EXT2_INDIRECT_BLOCK];

            if (read_block(indirect_blk)) {
                auto*    indirect       = reinterpret_cast<uint32_t*>(dma_buf_virt_);
                uint32_t ptrs_per_block = bs / sizeof(uint32_t);

                for (uint32_t i = 0; i < ptrs_per_block; ++i) {
                    if (indirect[i] != 0) {
                        free_block(indirect[i]);
                    }
                }
            }

            free_block(indirect_blk);
            target_disk.i_block[EXT2_INDIRECT_BLOCK] = 0;
        }

        target_disk.i_dtime  = 0;
        target_disk.i_size   = 0;
        target_disk.i_blocks = 0;

        write_disk_inode(entry_ino, target_disk);
        free_inode(entry_ino);

        if ((target_disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            uint32_t group = (entry_ino - 1) / inodes_per_group_;
            if (group < group_count_ && bgdt_[group].bg_used_dirs_count > 0) {
                bgdt_[group].bg_used_dirs_count--;
                write_bgdt(group);
            }

            if (dir_disk.i_links_count > 0) {
                dir_disk.i_links_count--;
            }
        }
    } else {
        write_disk_inode(entry_ino, target_disk);
    }

    // Invalidate the cache entry for the removed inode
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (inode_cache_[i].in_use && inode_cache_[i].ino == entry_ino) {
            inode_cache_[i].in_use = false;
            break;
        }
    }

    write_disk_inode(parent_ino, dir_disk);

    return 0;
}

}  // namespace cinux::fs
