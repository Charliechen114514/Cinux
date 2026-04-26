/**
 * @file kernel/fs/ext2.cpp
 * @brief ext2 filesystem driver implementation
 *
 * Reads the ext2 superblock and block group descriptor table from disk
 * during mount(), then provides path-based lookup() and InodeOps for
 * reading files and listing directories.  All disk I/O goes through
 * the AHCI driver using DMA buffers allocated from PMM/VMM.
 */

#include "ext2.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/drivers/ahci/ahci.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/lib/string.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::fs {

// ============================================================
// Virtual address for ext2 DMA buffers
// ============================================================

/// Base virtual address for ext2 DMA page mappings
static constexpr uint64_t EXT2_DMA_VIRT_BASE = cinux::arch::KMEM_EXT2_DMA_BASE;

// ============================================================
// Ext2FileOps method implementations
// ============================================================

Ext2FileOps::Ext2FileOps(Ext2& ext2) : ext2_(ext2) {}

int64_t Ext2FileOps::read(const Inode* inode, uint64_t offset, void* buf, uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return -1;
    }

    auto*            cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk   = cached->disk_inode;

    if (offset >= disk.i_size) {
        return 0;
    }

    uint64_t available = disk.i_size - offset;
    uint64_t to_read   = (count < available) ? count : available;

    if (to_read == 0) {
        return 0;
    }

    uint32_t bs                   = ext2_.block_size();
    uint64_t block_ptrs_per_block = bs / sizeof(uint32_t);

    auto*    dst        = static_cast<uint8_t*>(buf);
    uint64_t total_read = 0;

    while (total_read < to_read) {
        uint64_t file_block   = (offset + total_read) / bs;
        uint64_t block_offset = (offset + total_read) % bs;
        uint64_t chunk        = bs - block_offset;
        if (chunk > to_read - total_read) {
            chunk = to_read - total_read;
        }

        uint32_t disk_block = 0;

        if (file_block < EXT2_DIRECT_BLOCKS) {
            disk_block = disk.i_block[file_block];
        } else if (file_block < EXT2_DIRECT_BLOCKS + block_ptrs_per_block) {
            uint32_t indirect_block = disk.i_block[EXT2_INDIRECT_BLOCK];
            if (indirect_block == 0) {
                break;
            }

            if (!ext2_.read_block(indirect_block)) {
                break;
            }

            uint32_t idx      = static_cast<uint32_t>(file_block - EXT2_DIRECT_BLOCKS);
            auto*    indirect = reinterpret_cast<uint32_t*>(ext2_.dma_buf_virt());
            disk_block        = indirect[idx];
        } else {
            break;
        }

        if (disk_block == 0) {
            for (uint64_t i = 0; i < chunk; ++i) {
                dst[total_read + i] = 0;
            }
            total_read += chunk;
            continue;
        }

        if (!ext2_.read_block(disk_block)) {
            break;
        }

        auto* src = reinterpret_cast<const uint8_t*>(ext2_.dma_buf_virt()) + block_offset;
        memcpy(dst + total_read, src, chunk);
        total_read += chunk;
    }

    return static_cast<int64_t>(total_read);
}

int64_t Ext2FileOps::write(Inode* inode, uint64_t offset, const void* buf, uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) {
        return -1;
    }

    if (count == 0) {
        return 0;
    }

    auto*      cached = static_cast<Ext2CachedInode*>(inode->fs_private);
    Ext2Inode& disk   = cached->disk_inode;

    uint32_t bs = ext2_.block_size();

    auto*    src           = static_cast<const uint8_t*>(buf);
    uint64_t total_written = 0;

    while (total_written < count) {
        uint64_t file_block   = (offset + total_written) / bs;
        uint64_t block_offset = (offset + total_written) % bs;
        uint64_t chunk        = bs - block_offset;
        if (chunk > count - total_written) {
            chunk = count - total_written;
        }

        if (file_block > EXT2_DIRECT_BLOCKS) {
            break;
        }

        uint32_t disk_block = ext2_.get_or_alloc_block(disk, static_cast<uint32_t>(file_block));
        if (disk_block == 0) {
            cinux::lib::kprintf("[EXT2] file_write: failed to alloc block for file_block %u\n",
                                static_cast<uint32_t>(file_block));
            break;
        }

        if (block_offset != 0 || chunk != bs) {
            if (!ext2_.read_block(disk_block)) {
                break;
            }
        } else {
            auto* dma = reinterpret_cast<uint8_t*>(ext2_.dma_buf_virt());
            for (uint32_t i = 0; i < bs; ++i) {
                dma[i] = 0;
            }
        }

        auto* dst = reinterpret_cast<uint8_t*>(ext2_.dma_buf_virt()) + block_offset;
        for (uint64_t i = 0; i < chunk; ++i) {
            dst[i] = src[total_written + i];
        }

        if (!ext2_.write_block(disk_block)) {
            break;
        }

        total_written += chunk;
    }

    if (total_written > 0) {
        uint64_t new_end = offset + total_written;
        if (new_end > disk.i_size) {
            disk.i_size = static_cast<uint32_t>(new_end);

            uint32_t sectors_used = ((disk.i_size + bs - 1) / bs) * (bs / 512);
            disk.i_blocks         = sectors_used;
        }

        ext2_.write_disk_inode(static_cast<uint32_t>(inode->ino), disk);

        inode->size = disk.i_size;
    }

    return static_cast<int64_t>(total_written);
}

int64_t Ext2FileOps::stat(const Inode* inode, struct stat* st) {
    if (inode == nullptr || inode->fs_private == nullptr || st == nullptr) {
        return -1;
    }

    auto*            cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk   = cached->disk_inode;

    st->st_dev     = 0;
    st->st_ino     = inode->ino;
    st->st_mode    = disk.i_mode;
    st->st_nlink   = disk.i_links_count;
    st->st_uid     = disk.i_uid;
    st->st_gid     = disk.i_gid;
    st->st_rdev    = 0;
    st->st_size    = disk.i_size;
    st->st_blksize = ext2_.block_size();
    st->st_blocks  = disk.i_blocks;
    st->st_atime   = disk.i_atime;
    st->st_mtime   = disk.i_mtime;
    st->st_ctime   = disk.i_ctime;

    return 0;
}

Ext2DirOps::Ext2DirOps(Ext2& ext2) : ext2_(ext2) {}

int64_t Ext2DirOps::readdir(const Inode* inode, uint64_t index, char* name, uint64_t name_max) {
    if (inode == nullptr || inode->fs_private == nullptr || name == nullptr || name_max == 0) {
        return -1;
    }

    auto*            cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk   = cached->disk_inode;

    uint32_t bs = ext2_.block_size();

    if (index == 0) {
        if (name_max < 2) {
            return -1;
        }
        name[0] = '.';
        name[1] = '\0';
        return 1;
    }
    if (index == 1) {
        if (name_max < 3) {
            return -1;
        }
        name[0] = '.';
        name[1] = '.';
        name[2] = '\0';
        return 1;
    }

    uint64_t target   = index - 2;
    uint64_t found    = 0;
    uint64_t dir_size = disk.i_size;

    uint32_t total_blocks = static_cast<uint32_t>((dir_size + bs - 1) / bs);
    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;
    }

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!ext2_.read_block(blk)) {
            return -1;
        }

        auto*    block_data = reinterpret_cast<const uint8_t*>(ext2_.dma_buf_virt());
        uint32_t pos        = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<const Ext2DirEntry*>(block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0) {
                // Skip "." and ".." — they are handled by the
                // hardcoded indices 0 and 1 above
                if (entry->name_len == 1 && entry->name[0] == '.') {
                    pos += entry->rec_len;
                    continue;
                }
                if (entry->name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.') {
                    pos += entry->rec_len;
                    continue;
                }

                if (found == target) {
                    uint32_t copy_len = static_cast<uint32_t>(name_max) - 1;
                    if (entry->name_len < copy_len) {
                        copy_len = entry->name_len;
                    }

                    for (uint32_t i = 0; i < copy_len; ++i) {
                        name[i] = entry->name[i];
                    }
                    name[copy_len] = '\0';
                    return 1;
                }
                ++found;
            }

            pos += entry->rec_len;
        }
    }

    return 0;
}

Inode* Ext2DirOps::create(Inode* dir, const char* name, uint32_t namelen) {
    if (dir == nullptr || name == nullptr || namelen == 0) {
        return nullptr;
    }

    return ext2_.create(static_cast<uint32_t>(dir->ino), name, namelen);
}

Inode* Ext2DirOps::mkdir(Inode* dir, const char* name, uint32_t namelen) {
    if (dir == nullptr || name == nullptr || namelen == 0) {
        return nullptr;
    }

    return ext2_.mkdir(static_cast<uint32_t>(dir->ino), name, namelen);
}

int64_t Ext2DirOps::unlink(Inode* dir, const char* name, uint32_t namelen) {
    if (dir == nullptr || name == nullptr || namelen == 0) {
        return -1;
    }

    return ext2_.unlink(static_cast<uint32_t>(dir->ino), name, namelen);
}

int64_t Ext2DirOps::stat(const Inode* inode, struct stat* st) {
    if (inode == nullptr || inode->fs_private == nullptr || st == nullptr) {
        return -1;
    }

    auto*            cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk   = cached->disk_inode;

    st->st_dev     = 0;
    st->st_ino     = inode->ino;
    st->st_mode    = disk.i_mode;
    st->st_nlink   = disk.i_links_count;
    st->st_uid     = disk.i_uid;
    st->st_gid     = disk.i_gid;
    st->st_rdev    = 0;
    st->st_size    = disk.i_size;
    st->st_blksize = ext2_.block_size();
    st->st_blocks  = disk.i_blocks;
    st->st_atime   = disk.i_atime;
    st->st_mtime   = disk.i_mtime;
    st->st_ctime   = disk.i_ctime;

    return 0;
}

// ============================================================
// Ext2 constructor
// ============================================================

Ext2::Ext2(cinux::drivers::ahci::AHCI& ahci, uint8_t port_index)
    : file_ops_(*this), dir_ops_(*this), ahci_(ahci), port_index_(port_index) {}

// ============================================================
// DMA buffer management
// ============================================================

bool Ext2::ensure_dma_buffer() {
    if (dma_ready_) {
        return true;
    }

    // Allocate a physical page for DMA
    dma_buf_phys_ = cinux::mm::g_pmm.alloc_page();
    if (dma_buf_phys_ == 0) {
        cinux::lib::kprintf("[EXT2] Failed to allocate DMA page\n");
        return false;
    }

    // Map it into kernel virtual address space
    constexpr uint64_t flags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE;
    dma_buf_virt_            = EXT2_DMA_VIRT_BASE;

    if (!cinux::mm::g_vmm.map(dma_buf_virt_, dma_buf_phys_, flags)) {
        cinux::lib::kprintf("[EXT2] Failed to map DMA page\n");
        cinux::mm::g_pmm.free_page(dma_buf_phys_);
        dma_buf_phys_ = 0;
        return false;
    }

    // Zero the buffer
    auto* buf = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    for (uint32_t i = 0; i < cinux::arch::PAGE_SIZE; ++i) {
        buf[i] = 0;
    }

    dma_ready_ = true;
    return true;
}

// ============================================================
// Block I/O
// ============================================================

bool Ext2::read_block(uint32_t block_num) {
    if (!ensure_dma_buffer()) {
        return false;
    }

    // Compute the LBA of the first sector of this block
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;

    // Read all sectors for this block
    bool ok =
        ahci_.read(port_index_, lba, static_cast<uint16_t>(sectors_per_block_), dma_buf_phys_);
    if (!ok) {
        cinux::lib::kprintf("[EXT2] read_block(%u) I/O failed\n", block_num);
    }
    return ok;
}

bool Ext2::write_block(uint32_t block_num) {
    if (!ensure_dma_buffer()) {
        return false;
    }

    // Compute the LBA of the first sector of this block
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;

    // Write all sectors for this block from the DMA buffer to disk
    bool ok =
        ahci_.write(port_index_, lba, static_cast<uint16_t>(sectors_per_block_), dma_buf_phys_);
    if (!ok) {
        cinux::lib::kprintf("[EXT2] write_block(%u) I/O failed\n", block_num);
    }
    return ok;
}

// ============================================================
// mount()
// ============================================================

bool Ext2::mount() {
    cinux::lib::kprintf("[EXT2] Mounting ext2 filesystem on AHCI port %u\n", port_index_);

    // Step 1: Ensure DMA buffer is ready
    if (!ensure_dma_buffer()) {
        return false;
    }

    // Step 2: Read the superblock (at byte offset 1024 = LBA 2, 2 sectors)
    // The superblock is 1024 bytes starting at offset 1024.
    // For 512-byte sectors: LBA 2, count 2.
    constexpr uint64_t SB_LBA     = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE;
    constexpr uint16_t SB_SECTORS = EXT2_SUPERBLOCK_SIZE / EXT2_SECTOR_SIZE;

    if (!ahci_.read(port_index_, SB_LBA, SB_SECTORS, dma_buf_phys_)) {
        cinux::lib::kprintf("[EXT2] Failed to read superblock\n");
        return false;
    }

    // Copy the superblock from the DMA buffer
    auto* dma = reinterpret_cast<const uint8_t*>(dma_buf_virt_);
    memcpy(&sb_, dma, sizeof(Ext2Superblock));

    // Step 3: Validate magic number
    if (sb_.s_magic != EXT2_SUPER_MAGIC) {
        cinux::lib::kprintf("[EXT2] Invalid magic: 0x%x (expected 0x%x)\n", sb_.s_magic,
                            EXT2_SUPER_MAGIC);
        return false;
    }

    // Step 4: Compute filesystem parameters
    block_size_        = 1024U << sb_.s_log_block_size;
    sectors_per_block_ = block_size_ / EXT2_SECTOR_SIZE;
    first_data_block_  = sb_.s_first_data_block;
    inode_size_        = (sb_.s_rev_level == 0) ? EXT2_INODE_SIZE_DEFAULT : sb_.s_inode_size;
    inodes_per_group_  = sb_.s_inodes_per_group;
    blocks_per_group_  = sb_.s_blocks_per_group;

    // Compute group count
    group_count_ = (sb_.s_blocks_count + blocks_per_group_ - 1) / blocks_per_group_;
    if (group_count_ > EXT2_MAX_GROUPS) {
        group_count_ = EXT2_MAX_GROUPS;
    }

    cinux::lib::kprintf("[EXT2] Superblock valid: magic=0x%x\n", sb_.s_magic);
    cinux::lib::kprintf("[EXT2]   block_size=%u  inode_size=%u\n", block_size_, inode_size_);
    cinux::lib::kprintf("[EXT2]   blocks=%u  inodes=%u  groups=%u\n", sb_.s_blocks_count,
                        sb_.s_inodes_count, group_count_);
    cinux::lib::kprintf("[EXT2]   blocks_per_group=%u  inodes_per_group=%u\n", blocks_per_group_,
                        inodes_per_group_);

    // Step 5: Read the block group descriptor table
    // The BGDT starts at the block after the superblock block.
    // For 1K blocks: superblock is in block 1, BGDT starts at block 2.
    // For larger blocks: superblock is in block 0 (at offset 1024), BGDT at block 1.
    uint32_t bgdt_block = (block_size_ == 1024) ? 2 : 1;

    uint32_t bgdt_entries       = group_count_;
    uint32_t bgdt_bytes         = bgdt_entries * sizeof(Ext2BlockGroupDescriptor);
    uint32_t bgdt_blocks_needed = (bgdt_bytes + block_size_ - 1) / block_size_;

    for (uint32_t i = 0; i < bgdt_blocks_needed; ++i) {
        if (!read_block(bgdt_block + i)) {
            cinux::lib::kprintf("[EXT2] Failed to read BGDT block %u\n", bgdt_block + i);
            return false;
        }

        auto*    src                   = reinterpret_cast<const uint8_t*>(dma_buf_virt_);
        uint32_t entries_in_this_block = block_size_ / sizeof(Ext2BlockGroupDescriptor);
        uint32_t start_entry           = i * entries_in_this_block;
        uint32_t copy_count            = entries_in_this_block;

        if (start_entry + copy_count > bgdt_entries) {
            copy_count = bgdt_entries - start_entry;
        }

        memcpy(&bgdt_[start_entry], src, copy_count * sizeof(Ext2BlockGroupDescriptor));
    }

    cinux::lib::kprintf("[EXT2] BGDT loaded: %u groups\n", group_count_);

    // Step 6: Set up the root directory inode (inode 2 in ext2)
    Ext2Inode root_disk;
    if (!read_disk_inode(2, root_disk)) {
        cinux::lib::kprintf("[EXT2] Failed to read root inode (ino=2)\n");
        return false;
    }

    // Place root inode in cache slot 0
    inode_cache_[0].ino        = 2;
    inode_cache_[0].disk_inode = root_disk;
    inode_cache_[0].in_use     = true;
    populate_vfs_inode(inode_cache_[0]);
    root_inode_ = inode_cache_[0].vfs_inode;

    cinux::lib::kprintf("[EXT2] Root inode: size=%u mode=0x%x\n", root_disk.i_size,
                        root_disk.i_mode);

    mounted_ = true;
    return true;
}

// ============================================================
// read_disk_inode()
// ============================================================

bool Ext2::read_disk_inode(uint32_t ino, Ext2Inode& out_inode) {
    if (ino == 0) {
        return false;
    }

    // Compute block group
    uint32_t group = (ino - 1) / inodes_per_group_;

    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] Inode %u: group %u out of range\n", ino, group);
        return false;
    }

    // Get the inode table start block from the group descriptor
    uint32_t inode_table_block = bgdt_[group].bg_inode_table;

    // Compute the index within the group
    uint32_t index_in_group = (ino - 1) % inodes_per_group_;

    // Compute byte offset within the inode table
    uint64_t byte_offset = static_cast<uint64_t>(index_in_group) * inode_size_;

    // Compute which block of the inode table contains this inode
    uint32_t block_offset        = static_cast<uint32_t>(byte_offset / block_size_);
    uint32_t within_block_offset = static_cast<uint32_t>(byte_offset % block_size_);

    uint32_t target_block = inode_table_block + block_offset;

    // Read the block
    if (!read_block(target_block)) {
        cinux::lib::kprintf("[EXT2] Failed to read inode block %u\n", target_block);
        return false;
    }

    // Extract the inode from the DMA buffer
    auto* block_data = reinterpret_cast<const uint8_t*>(dma_buf_virt_);

    // Safety: ensure we don't read past the block boundary
    if (within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] Inode %u crosses block boundary\n", ino);
        return false;
    }

    memcpy(&out_inode, block_data + within_block_offset, sizeof(Ext2Inode));
    return true;
}

// ============================================================
// write_disk_inode()
// ============================================================

bool Ext2::write_disk_inode(uint32_t ino, const Ext2Inode& inode) {
    if (ino == 0) {
        return false;
    }

    // Compute block group
    uint32_t group = (ino - 1) / inodes_per_group_;

    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: ino %u group %u out of range\n", ino, group);
        return false;
    }

    // Locate the inode within its group's inode table
    uint32_t inode_table_block   = bgdt_[group].bg_inode_table;
    uint32_t index_in_group      = (ino - 1) % inodes_per_group_;
    uint64_t byte_offset         = static_cast<uint64_t>(index_in_group) * inode_size_;
    uint32_t block_offset        = static_cast<uint32_t>(byte_offset / block_size_);
    uint32_t within_block_offset = static_cast<uint32_t>(byte_offset % block_size_);
    uint32_t target_block        = inode_table_block + block_offset;

    // Read-modify-write: read the block, patch the inode, write back
    if (!read_block(target_block)) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to read block %u\n", target_block);
        return false;
    }

    // Safety check
    if (within_block_offset + sizeof(Ext2Inode) > block_size_) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: ino %u crosses block boundary\n", ino);
        return false;
    }

    // Patch the inode data into the DMA buffer
    auto* block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    memcpy(block_data + within_block_offset, &inode, sizeof(Ext2Inode));

    // Write the block back to disk
    if (!write_block(target_block)) {
        cinux::lib::kprintf("[EXT2] write_disk_inode: failed to write block %u\n", target_block);
        return false;
    }

    return true;
}

// ============================================================
// write_superblock()
// ============================================================

bool Ext2::write_superblock() {
    // The superblock is at byte offset 1024, which is block 1 for 1K blocks
    // or within block 0 for larger block sizes.  We always write via the
    // sector-based approach (LBA 2, 2 sectors) to keep it simple.

    constexpr uint64_t SB_LBA     = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE;
    constexpr uint16_t SB_SECTORS = EXT2_SUPERBLOCK_SIZE / EXT2_SECTOR_SIZE;

    // Place the superblock data into the DMA buffer
    auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    memcpy(dma, &sb_, sizeof(Ext2Superblock));

    // Write via AHCI (write needs physical address)
    if (!ahci_.write(port_index_, SB_LBA, SB_SECTORS, dma_buf_phys_)) {
        cinux::lib::kprintf("[EXT2] write_superblock: I/O failed\n");
        return false;
    }

    return true;
}

// ============================================================
// write_bgdt()
// ============================================================

bool Ext2::write_bgdt(uint32_t group) {
    if (group >= group_count_) {
        return false;
    }

    // Determine which BGDT block contains this group's descriptor
    uint32_t bgdt_start_block  = (block_size_ == 1024) ? 2 : 1;
    uint32_t entries_per_block = block_size_ / sizeof(Ext2BlockGroupDescriptor);
    uint32_t bgdt_block_index  = group / entries_per_block;
    uint32_t entry_in_block    = group % entries_per_block;
    uint32_t disk_block        = bgdt_start_block + bgdt_block_index;

    // Read-modify-write
    if (!read_block(disk_block)) {
        cinux::lib::kprintf("[EXT2] write_bgdt: failed to read block %u\n", disk_block);
        return false;
    }

    // Patch the specific descriptor entry
    auto* block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    memcpy(block_data + entry_in_block * sizeof(Ext2BlockGroupDescriptor), &bgdt_[group],
           sizeof(Ext2BlockGroupDescriptor));

    if (!write_block(disk_block)) {
        cinux::lib::kprintf("[EXT2] write_bgdt: failed to write block %u\n", disk_block);
        return false;
    }

    return true;
}

// ============================================================
// Inode cache management
// ============================================================

Inode* Ext2::get_cached_inode(uint32_t ino) {
    // Search the cache for an existing entry
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (inode_cache_[i].in_use && inode_cache_[i].ino == ino) {
            return &inode_cache_[i].vfs_inode;
        }
    }

    // Find a free slot
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (!inode_cache_[i].in_use) {
            // Read the inode from disk
            if (!read_disk_inode(ino, inode_cache_[i].disk_inode)) {
                return nullptr;
            }

            inode_cache_[i].ino    = ino;
            inode_cache_[i].in_use = true;
            populate_vfs_inode(inode_cache_[i]);
            return &inode_cache_[i].vfs_inode;
        }
    }

    // Cache full -- evict slot 1 (slot 0 is always root)
    // Simple FIFO eviction
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
// lookup_in_dir()
// ============================================================

uint32_t Ext2::lookup_in_dir(uint32_t dir_ino, const char* name, uint32_t name_len) {
    // Read the directory inode
    Ext2Inode dir_disk;
    if (!read_disk_inode(dir_ino, dir_disk)) {
        return 0;
    }

    uint32_t bs           = block_size_;
    uint32_t dir_size     = dir_disk.i_size;
    uint32_t total_blocks = (dir_size + bs - 1) / bs;

    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;
    }

    // Scan each data block of the directory
    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0) {
            continue;
        }

        if (!read_block(blk)) {
            return 0;
        }

        auto*    block_data = reinterpret_cast<const uint8_t*>(dma_buf_virt_);
        uint32_t pos        = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) {
                break;
            }

            auto* entry = reinterpret_cast<const Ext2DirEntry*>(block_data + pos);

            if (entry->rec_len == 0) {
                break;
            }

            if (entry->inode != 0 && entry->name_len == name_len) {
                // Compare names
                bool match = true;
                for (uint32_t i = 0; i < name_len; ++i) {
                    if (entry->name[i] != name[i]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    return entry->inode;
                }
            }

            pos += entry->rec_len;
        }
    }

    return 0;  // not found
}

// ============================================================
// lookup()
// ============================================================

Inode* Ext2::lookup(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }

    // Root directory
    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        return &root_inode_;
    }

    // Skip leading '/'
    if (path[0] == '/') {
        ++path;
    }

    // Walk the path component by component
    uint32_t current_ino = 2;  // start at root

    while (path[0] != '\0') {
        // Find the length of the current component
        uint32_t comp_len = 0;
        while (path[comp_len] != '\0' && path[comp_len] != '/') {
            ++comp_len;
        }

        if (comp_len == 0) {
            // Skip consecutive slashes
            ++path;
            continue;
        }

        // Look up the component in the current directory
        uint32_t found_ino = lookup_in_dir(current_ino, path, comp_len);
        if (found_ino == 0) {
            return nullptr;  // component not found
        }

        // Advance past this component
        path += comp_len;
        if (path[0] == '/') {
            ++path;
        }

        // If there are more components, the found inode must be a directory
        if (path[0] != '\0') {
            // Check that this inode is a directory
            Ext2Inode check;
            if (!read_disk_inode(found_ino, check)) {
                return nullptr;
            }
            if ((check.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
                return nullptr;  // intermediate component is not a directory
            }
        }

        current_ino = found_ino;
    }

    // Return the cached inode for the final component
    return get_cached_inode(current_ino);
}

// ============================================================
// Block allocator
// ============================================================

uint32_t Ext2::alloc_block() {
    if (!mounted_) {
        return 0;
    }

    // Each block group has its own block bitmap.  Iterate over groups
    // looking for one with free blocks.
    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_blocks_count == 0) {
            continue;
        }

        uint32_t bitmap_block = bgdt_[group].bg_block_bitmap;
        if (bitmap_block == 0) {
            continue;
        }

        // Read the block bitmap
        if (!read_block(bitmap_block)) {
            cinux::lib::kprintf("[EXT2] alloc_block: failed to read bitmap block %u\n",
                                bitmap_block);
            return 0;
        }

        auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);

        // Number of blocks described by this bitmap
        uint32_t blocks_in_group = blocks_per_group_;
        uint32_t first_block     = group * blocks_per_group_ + first_data_block_;

        // The last group may have fewer blocks
        uint32_t total_blocks = sb_.s_blocks_count;
        if (first_block + blocks_in_group > total_blocks) {
            blocks_in_group = total_blocks - first_block;
        }

        // Number of bytes needed to cover blocks_in_group bits
        uint32_t bytes_needed = (blocks_in_group + 7) / 8;

        // Scan the bitmap for a free bit (0 = free)
        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (bitmap[byte_idx] == 0xFF) {
                // All bits in this byte are used
                continue;
            }

            // Find the first 0 bit within this byte
            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local_block = byte_idx * 8 + bit;
                if (local_block >= blocks_in_group) {
                    break;
                }

                if ((bitmap[byte_idx] & (1U << bit)) == 0) {
                    // Found a free block -- mark it as used
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);

                    // Write the bitmap back to disk
                    if (!write_block(bitmap_block)) {
                        cinux::lib::kprintf("[EXT2] alloc_block: failed to write bitmap\n");
                        return 0;
                    }

                    // Compute the global block number
                    uint32_t global_block = first_block + local_block;

                    // Update superblock free-block count
                    if (sb_.s_free_blocks_count > 0) {
                        --sb_.s_free_blocks_count;
                    }

                    // Update group descriptor free-block count
                    if (bgdt_[group].bg_free_blocks_count > 0) {
                        --bgdt_[group].bg_free_blocks_count;
                    }

                    // Write back updated metadata
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

    // Determine which block group this block belongs to
    uint32_t group = (block_num - first_data_block_) / blocks_per_group_;
    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] free_block: block %u group out of range\n", block_num);
        return false;
    }

    uint32_t bitmap_block = bgdt_[group].bg_block_bitmap;
    if (bitmap_block == 0) {
        return false;
    }

    // Compute the local block index within the group
    uint32_t local_block = block_num - (group * blocks_per_group_ + first_data_block_);

    // Read the bitmap
    if (!read_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_block: failed to read bitmap block %u\n", bitmap_block);
        return false;
    }

    // Clear the bit
    uint32_t byte_idx = local_block / 8;
    uint32_t bit      = local_block % 8;

    auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    // Write the bitmap back
    if (!write_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_block: failed to write bitmap\n");
        return false;
    }

    // Update superblock free-block count
    ++sb_.s_free_blocks_count;

    // Update group descriptor free-block count
    ++bgdt_[group].bg_free_blocks_count;

    // Write back updated metadata
    write_superblock();
    write_bgdt(group);

    return true;
}

// ============================================================
// Inode allocator
// ============================================================

uint32_t Ext2::alloc_inode() {
    if (!mounted_) {
        return 0;
    }

    // Iterate over block groups looking for one with free inodes
    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_inodes_count == 0) {
            continue;
        }

        uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
        if (bitmap_block == 0) {
            continue;
        }

        // Read the inode bitmap
        if (!read_block(bitmap_block)) {
            cinux::lib::kprintf("[EXT2] alloc_inode: failed to read bitmap block %u\n",
                                bitmap_block);
            return 0;
        }

        auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);

        // Number of inodes described by this bitmap
        uint32_t inodes_in_group = inodes_per_group_;
        uint32_t bytes_needed    = (inodes_in_group + 7) / 8;

        // Scan for a free bit (0 = free)
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
                    // Found a free inode -- mark it as used
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);

                    // Write the bitmap back to disk
                    if (!write_block(bitmap_block)) {
                        cinux::lib::kprintf("[EXT2] alloc_inode: failed to write bitmap\n");
                        return 0;
                    }

                    // Compute the global inode number (1-based)
                    uint32_t global_ino = group * inodes_per_group_ + local_index + 1;

                    // Update superblock free-inode count
                    if (sb_.s_free_inodes_count > 0) {
                        --sb_.s_free_inodes_count;
                    }

                    // Update group descriptor free-inode count
                    if (bgdt_[group].bg_free_inodes_count > 0) {
                        --bgdt_[group].bg_free_inodes_count;
                    }

                    // Write back updated metadata
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

    // Determine which block group this inode belongs to
    uint32_t group = (ino - 1) / inodes_per_group_;
    if (group >= group_count_) {
        cinux::lib::kprintf("[EXT2] free_inode: ino %u group out of range\n", ino);
        return false;
    }

    uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
    if (bitmap_block == 0) {
        return false;
    }

    // Compute the local index within the group
    uint32_t local_index = (ino - 1) % inodes_per_group_;

    // Read the bitmap
    if (!read_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_inode: failed to read bitmap block %u\n", bitmap_block);
        return false;
    }

    // Clear the bit
    uint32_t byte_idx = local_index / 8;
    uint32_t bit      = local_index % 8;

    auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    // Write the bitmap back
    if (!write_block(bitmap_block)) {
        cinux::lib::kprintf("[EXT2] free_inode: failed to write bitmap\n");
        return false;
    }

    // Update superblock free-inode count
    ++sb_.s_free_inodes_count;

    // Update group descriptor free-inode count
    ++bgdt_[group].bg_free_inodes_count;

    // Write back updated metadata
    write_superblock();
    write_bgdt(group);

    return true;
}

// ============================================================
// Accessors
// ============================================================

uint32_t Ext2::block_size() const {
    return block_size_;
}

bool Ext2::is_mounted() const {
    return mounted_;
}

uint64_t Ext2::dma_buf_virt() const {
    return dma_buf_virt_;
}

// ============================================================
// get_or_alloc_block() — block pointer resolver with allocation
// ============================================================

uint32_t Ext2::get_or_alloc_block(Ext2Inode& disk, uint32_t file_block) {
    // Only support direct blocks 0-11 and singly-indirect block 12
    if (file_block < EXT2_DIRECT_BLOCKS) {
        // Direct block
        if (disk.i_block[file_block] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) {
                return 0;
            }

            // Zero the new block on disk
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
        // Singly-indirect block (file_block index 12)
        uint32_t indirect_idx = file_block - EXT2_DIRECT_BLOCKS;

        // Allocate the indirect block itself if needed
        if (disk.i_block[EXT2_INDIRECT_BLOCK] == 0) {
            uint32_t indirect_blk = alloc_block();
            if (indirect_blk == 0) {
                return 0;
            }

            // Zero the indirect block (all entries = 0)
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

        // Read the indirect block
        if (!read_block(indirect_blk)) {
            return 0;
        }

        auto* indirect = reinterpret_cast<uint32_t*>(dma_buf_virt_);

        if (indirect[indirect_idx] == 0) {
            // Allocate a data block for this indirect entry
            uint32_t data_blk = alloc_block();
            if (data_blk == 0) {
                return 0;
            }

            // Zero the new data block
            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i) {
                dma[i] = 0;
            }
            if (!write_block(data_blk)) {
                free_block(data_blk);
                return 0;
            }

            // Now re-read the indirect block and patch the entry
            if (!read_block(indirect_blk)) {
                return 0;
            }
            indirect               = reinterpret_cast<uint32_t*>(dma_buf_virt_);
            indirect[indirect_idx] = data_blk;
            if (!write_block(indirect_blk)) {
                return 0;
            }
        }

        // Re-read to get the final block number
        if (!read_block(indirect_blk)) {
            return 0;
        }
        indirect = reinterpret_cast<uint32_t*>(dma_buf_virt_);
        return indirect[indirect_idx];
    }

    // Beyond singly-indirect: not supported
    return 0;
}

// ============================================================
// add_dir_entry() — insert a directory entry into a parent dir
// ============================================================

bool Ext2::add_dir_entry(uint32_t dir_ino, Ext2Inode& dir_disk, uint32_t entry_ino,
                         const char* name, uint32_t name_len, Ext2FileType file_type) {
    // Compute the required entry size (8-byte header + name, rounded up to 4)
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

            // Compute the minimum rec_len for this existing entry
            uint32_t entry_min = EXT2_DIR_ENTRY_HDR_SIZE + entry->name_len;
            entry_min          = (entry_min + 3) & ~3u;

            // Check if there is unused space after this entry
            uint32_t extra = entry->rec_len - entry_min;

            if (extra >= required_rec_len) {
                // Split: shrink the current entry and insert new one after it
                entry->rec_len = static_cast<uint16_t>(entry_min);

                auto* new_entry     = reinterpret_cast<Ext2DirEntry*>(block_data + pos + entry_min);
                new_entry->inode    = entry_ino;
                new_entry->rec_len  = static_cast<uint16_t>(extra);
                new_entry->name_len = static_cast<uint8_t>(name_len);
                new_entry->file_type = static_cast<uint8_t>(file_type);

                for (uint32_t i = 0; i < name_len; ++i) {
                    new_entry->name[i] = name[i];
                }

                // Write the modified block back
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

    // Ensure the directory has enough i_block slots
    uint32_t new_blk = alloc_block();
    if (new_blk == 0) {
        cinux::lib::kprintf("[EXT2] add_dir_entry: failed to allocate new dir block\n");
        return false;
    }

    // Zero and populate the new block with a single entry spanning the whole block
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

    // Update the directory inode
    dir_disk.i_block[new_block_idx] = new_blk;
    dir_disk.i_size += bs;

    // Update i_blocks (512-byte sectors)
    uint32_t sectors_used = ((dir_disk.i_size + bs - 1) / bs) * (bs / 512);
    dir_disk.i_blocks     = sectors_used;

    // Write back the directory inode
    if (!write_disk_inode(dir_ino, dir_disk)) {
        return false;
    }

    return true;
}

// ============================================================
// remove_dir_entry() — remove a named entry from a directory
// ============================================================

bool Ext2::remove_dir_entry(uint32_t /*dir_ino*/, const Ext2Inode& dir_disk, const char* name,
                            uint32_t name_len, uint32_t& out_entry_ino) {
    uint32_t bs           = block_size_;
    uint32_t dir_size     = dir_disk.i_size;
    uint32_t total_blocks = (dir_size + bs - 1) / bs;
    if (total_blocks > EXT2_DIRECT_BLOCKS) {
        total_blocks = EXT2_DIRECT_BLOCKS;
    }

    // Scan each data block of the directory
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
                // Compare names
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
                        // First entry in the block: clear inode (cannot merge back)
                        entry->inode = 0;
                    } else {
                        // Merge this entry's rec_len into the previous entry
                        auto* prev = reinterpret_cast<Ext2DirEntry*>(block_data + prev_pos);
                        prev->rec_len += entry->rec_len;
                    }

                    // Write the modified block back
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

    return false;  // entry not found
}

// ============================================================
// create() — create a new regular file
// ============================================================

Inode* Ext2::create(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) {
        return nullptr;
    }

    // Check if the name already exists
    if (lookup_in_dir(parent_ino, name, name_len) != 0) {
        return nullptr;
    }

    // Read the parent directory inode
    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    // Allocate a new inode
    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) {
        cinux::lib::kprintf("[EXT2] create: no free inodes\n");
        return nullptr;
    }

    // Initialise the new inode as a regular file
    Ext2Inode new_disk;
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
    }

    new_disk.i_mode        = EXT2_S_IFREG | 0644;  // regular file, rw-r--r--
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

    // Write the new inode to disk
    if (!write_disk_inode(new_ino, new_disk)) {
        free_inode(new_ino);
        return nullptr;
    }

    // Add a directory entry in the parent
    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Regular)) {
        free_inode(new_ino);
        return nullptr;
    }

    // Update parent directory write-back (add_dir_entry may have modified dir_disk)
    if (!write_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    // Cache the new inode and return it
    return get_cached_inode(new_ino);
}

// ============================================================
// mkdir() — create a new directory
// ============================================================

Inode* Ext2::mkdir(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) {
        return nullptr;
    }

    // Check if the name already exists
    if (lookup_in_dir(parent_ino, name, name_len) != 0) {
        cinux::lib::kprintf("[EXT2] mkdir: '%s' already exists\n", name);
        return nullptr;
    }

    // Read the parent directory inode
    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    // Allocate a new inode
    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) {
        cinux::lib::kprintf("[EXT2] mkdir: no free inodes\n");
        return nullptr;
    }

    // Allocate a data block for the directory contents
    uint32_t data_blk = alloc_block();
    if (data_blk == 0) {
        cinux::lib::kprintf("[EXT2] mkdir: no free blocks\n");
        free_inode(new_ino);
        return nullptr;
    }

    // Initialise the new inode as a directory
    Ext2Inode new_disk;
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
    }

    new_disk.i_mode        = EXT2_S_IFDIR | 0755;  // directory, rwxr-xr-x
    new_disk.i_uid         = 0;
    new_disk.i_size        = block_size_;
    new_disk.i_atime       = 0;
    new_disk.i_ctime       = 0;
    new_disk.i_mtime       = 0;
    new_disk.i_dtime       = 0;
    new_disk.i_gid         = 0;
    new_disk.i_links_count = 2;  // "." entry + parent's entry
    new_disk.i_blocks      = block_size_ / 512;
    new_disk.i_flags       = 0;
    new_disk.i_block[0]    = data_blk;

    // Write the new inode to disk
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

    // "." entry at offset 0
    auto* dot            = reinterpret_cast<Ext2DirEntry*>(dma);
    dot->inode           = new_ino;
    dot->name_len        = 1;
    dot->file_type       = static_cast<uint8_t>(Ext2FileType::Directory);
    dot->name[0]         = '.';
    // rec_len for "." = 12 bytes (8 hdr + 1 name + 3 pad)
    uint32_t dot_rec_len = EXT2_DIR_ENTRY_HDR_SIZE + 1;
    dot_rec_len          = (dot_rec_len + 3) & ~3u;  // 12
    dot->rec_len         = static_cast<uint16_t>(dot_rec_len);

    // ".." entry follows "."
    auto* dotdot      = reinterpret_cast<Ext2DirEntry*>(dma + dot_rec_len);
    dotdot->inode     = parent_ino;
    dotdot->name_len  = 2;
    dotdot->file_type = static_cast<uint8_t>(Ext2FileType::Directory);
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';
    // ".." rec_len spans the rest of the block
    dotdot->rec_len   = static_cast<uint16_t>(block_size_ - dot_rec_len);

    if (!write_block(data_blk)) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    // Add a directory entry in the parent
    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Directory)) {
        free_block(data_blk);
        free_inode(new_ino);
        return nullptr;
    }

    // Update parent: increment links_count (for the new subdirectory's ".." entry)
    dir_disk.i_links_count++;

    // Update bg_used_dirs_count for the group containing the new inode
    uint32_t new_group = (new_ino - 1) / inodes_per_group_;
    if (new_group < group_count_) {
        bgdt_[new_group].bg_used_dirs_count++;
        write_bgdt(new_group);
    }

    // Write back the parent directory inode
    if (!write_disk_inode(parent_ino, dir_disk)) {
        return nullptr;
    }

    // Cache the new inode and return it
    return get_cached_inode(new_ino);
}

// ============================================================
// unlink() — remove a directory entry and free resources if needed
// ============================================================

int Ext2::unlink(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0) {
        return -1;
    }

    // Read the parent directory inode
    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) {
        return -1;
    }

    // Remove the directory entry (finds and removes in one pass)
    uint32_t entry_ino = 0;
    if (!remove_dir_entry(parent_ino, dir_disk, name, name_len, entry_ino)) {
        cinux::lib::kprintf("[EXT2] unlink: '%s' not found\n", name);
        return -1;
    }

    // Read the target inode
    Ext2Inode target_disk;
    if (!read_disk_inode(entry_ino, target_disk)) {
        return -1;
    }

    // Decrement link count
    if (target_disk.i_links_count > 0) {
        target_disk.i_links_count--;
    }

    if (target_disk.i_links_count == 0) {
        // No more links — free all data blocks
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

        // Mark the inode as deleted (set dtime, clear size)
        target_disk.i_dtime  = 0;  // TODO: use real timestamp when available
        target_disk.i_size   = 0;
        target_disk.i_blocks = 0;

        // Write back the target inode (zeroed) before freeing it
        write_disk_inode(entry_ino, target_disk);

        // Free the inode itself
        free_inode(entry_ino);

        // If it was a directory, update bg_used_dirs_count
        if ((target_disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            uint32_t group = (entry_ino - 1) / inodes_per_group_;
            if (group < group_count_ && bgdt_[group].bg_used_dirs_count > 0) {
                bgdt_[group].bg_used_dirs_count--;
                write_bgdt(group);
            }

            // Decrement parent's links_count (the ".." reference is gone)
            if (dir_disk.i_links_count > 0) {
                dir_disk.i_links_count--;
            }
        }
    } else {
        // Still has links — just write back the updated link count
        write_disk_inode(entry_ino, target_disk);
    }

    // Invalidate the cache entry for the removed inode
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (inode_cache_[i].in_use && inode_cache_[i].ino == entry_ino) {
            inode_cache_[i].in_use = false;
            break;
        }
    }

    // Write back the parent directory inode
    write_disk_inode(parent_ino, dir_disk);

    return 0;
}

}  // namespace cinux::fs
