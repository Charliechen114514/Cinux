# ext2 分配器与元数据回写

## 概览

前一个 tag 我们让 ext2 驱动跑通了只读路径。但一个真正的文件系统光能读显然不够，我们需要往磁盘上写东西。这一切的根基是 ext2 的资源分配器：分配空闲的数据块、分配空闲的 inode、在不再需要时回收它们，以及每次修改元数据后刷回磁盘。

这篇文章拆解 Cinux ext2 驱动中所有和"写"相关的基础设施：`alloc_block` / `free_block`、`alloc_inode` / `free_inode`、`write_superblock` / `write_bgdt` / `write_disk_inode` / `write_block`，以及把文件逻辑块号映射到物理盘块的 `get_or_alloc_block`。搞清楚了这些，后面讲文件创建和目录操作就顺水推舟了。

## 架构图

```
用户层 syscall (creat / mkdir / unlink / write)
        │
        ▼
   Ext2DirOps / Ext2FileOps   ← VFS InodeOps 虚函数
        │
        ▼
   Ext2::create / mkdir / unlink / write
        │
   ┌────┴─────────────────────────────┐
   │                                  │
   ▼                                  ▼
alloc_inode()                    alloc_block()
   │                                  │
   ├─ 扫描 bitmap                    ├─ 扫描 bitmap
   ├─ 标记占用                        ├─ 标记占用
   └─ 更新 sb_ / bgdt_              └─ 更新 sb_ / bgdt_
        │                                  │
        ▼                                  ▼
   write_superblock() / write_bgdt() / write_disk_inode()
        │
        ▼
   write_block()  →  AHCI::write()  →  磁盘
```

## 代码精讲

### 块分配器 alloc_block

块分配是 ext2 写操作最核心的原语。我们需要给文件或目录分配新数据块时，就靠它从空闲池子里捞出一个来。

```cpp
uint32_t Ext2::alloc_block() {
    if (!mounted_) { return 0; }

    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_blocks_count == 0) { continue; }

        uint32_t bitmap_block = bgdt_[group].bg_block_bitmap;
        if (bitmap_block == 0) { continue; }
        if (!read_block(bitmap_block)) { return 0; }

        auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);

        uint32_t blocks_in_group = blocks_per_group_;
        uint32_t first_block     = group * blocks_per_group_ + first_data_block_;
        uint32_t total_blocks = sb_.s_blocks_count;
        if (first_block + blocks_in_group > total_blocks) {
            blocks_in_group = total_blocks - first_block;
        }
        uint32_t bytes_needed = (blocks_in_group + 7) / 8;

        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (bitmap[byte_idx] == 0xFF) { continue; }
            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local_block = byte_idx * 8 + bit;
                if (local_block >= blocks_in_group) { break; }
                if ((bitmap[byte_idx] & (1U << bit)) == 0) {
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);
                    if (!write_block(bitmap_block)) { return 0; }
                    uint32_t global_block = first_block + local_block;
                    if (sb_.s_free_blocks_count > 0) { --sb_.s_free_blocks_count; }
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
    return 0;
}
```

我们从 group 0 开始线性扫描。先跳过 `bg_free_blocks_count` 为 0 的组，找到可能有空间的组就读取它的 block bitmap 到 DMA 缓冲区。然后两层循环扫描 bitmap——外层按字节，内层按 bit。一个字节是 `0xFF` 就直接跳过，比逐 bit 快得多。

找到第一个 0 bit 后，标记为 1、写回 bitmap 块、计算全局块号（`first_block + local_block`），然后更新 superblock 和 BGDT 的空闲块计数并刷盘。一次 `alloc_block` 总共产生三次磁盘写：bitmap、superblock、BGDT。我们没有任何延迟写或 write-back cache——所有修改立即同步刷盘，保证崩溃一致性：任何时刻断电，磁盘元数据要么已更新要么什么都没发生。

最后一个 group 的块数可能比 `blocks_per_group_` 少，所以做了边界裁剪。所有 group 扫完还没找到空闲块就返回 0，调用方需要检查。

### 块释放器 free_block

```cpp
bool Ext2::free_block(uint32_t block_num) {
    if (block_num == 0 || !mounted_) { return false; }

    uint32_t group = (block_num - first_data_block_) / blocks_per_group_;
    if (group >= group_count_) { return false; }

    uint32_t bitmap_block = bgdt_[group].bg_block_bitmap;
    if (bitmap_block == 0) { return false; }
    uint32_t local_block = block_num - (group * blocks_per_group_ + first_data_block_);

    if (!read_block(bitmap_block)) { return false; }
    uint32_t byte_idx = local_block / 8;
    uint32_t bit      = local_block % 8;
    auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    if (!write_block(bitmap_block)) { return false; }
    ++sb_.s_free_blocks_count;
    ++bgdt_[group].bg_free_blocks_count;
    write_superblock();
    write_bgdt(group);
    return true;
}
```

从全局块号反推 group：`(block_num - first_data_block_) / blocks_per_group_`。算出 group 内局部索引后在 bitmap 里清零对应位，然后递增两个计数器并刷盘。和 `alloc_block` 一样是三次磁盘写。

### inode 分配器 alloc_inode

inode 分配和 block 分配的逻辑几乎是镜像的，区别在于操作 inode bitmap，而且 inode 号是 1-based 的。

```cpp
uint32_t Ext2::alloc_inode() {
    if (!mounted_) { return 0; }

    for (uint32_t group = 0; group < group_count_; ++group) {
        if (bgdt_[group].bg_free_inodes_count == 0) { continue; }
        uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
        if (bitmap_block == 0) { continue; }
        if (!read_block(bitmap_block)) { return 0; }

        auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);
        uint32_t inodes_in_group = inodes_per_group_;
        uint32_t bytes_needed    = (inodes_in_group + 7) / 8;

        for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
            if (bitmap[byte_idx] == 0xFF) { continue; }
            for (uint32_t bit = 0; bit < 8; ++bit) {
                uint32_t local_index = byte_idx * 8 + bit;
                if (local_index >= inodes_in_group) { break; }
                if ((bitmap[byte_idx] & (1U << bit)) == 0) {
                    bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);
                    if (!write_block(bitmap_block)) { return 0; }
                    uint32_t global_ino = group * inodes_per_group_ + local_index + 1;
                    if (sb_.s_free_inodes_count > 0) { --sb_.s_free_inodes_count; }
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
    return 0;
}
```

inode 号是 1-based（inode 0 不存在，inode 2 是根目录），所以全局号计算公式是 `group * inodes_per_group_ + local_index + 1`，末尾的 `+ 1` 千万不能丢。block 号是 0-based，不需要这个偏移——这是两个分配器之间最容易搞混的细节。

### inode 释放器 free_inode

```cpp
bool Ext2::free_inode(uint32_t ino) {
    if (ino == 0 || !mounted_) { return false; }

    uint32_t group = (ino - 1) / inodes_per_group_;
    if (group >= group_count_) { return false; }
    uint32_t bitmap_block = bgdt_[group].bg_inode_bitmap;
    if (bitmap_block == 0) { return false; }
    uint32_t local_index = (ino - 1) % inodes_per_group_;

    if (!read_block(bitmap_block)) { return false; }
    uint32_t byte_idx = local_index / 8;
    uint32_t bit      = local_index % 8;
    auto* bitmap = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    bitmap[byte_idx] &= static_cast<uint8_t>(~(1U << bit));

    if (!write_block(bitmap_block)) { return false; }
    ++sb_.s_free_inodes_count;
    ++bgdt_[group].bg_free_inodes_count;
    write_superblock();
    write_bgdt(group);
    return true;
}
```

因为 inode 是 1-based，group 计算用 `(ino - 1) / inodes_per_group_`，局部索引用 `(ino - 1) % inodes_per_group_`。那个 `- 1` 非常关键——忘了的话 bitmap 里清零的就是错误的位，inode 表会被悄悄搞乱。

### 元数据回写：write_block, write_superblock, write_bgdt, write_disk_inode

这几个函数都比较短，但被上层频繁调用。`write_block` 是所有磁盘写的底层入口：

```cpp
bool Ext2::write_block(uint32_t block_num) {
    if (!ensure_dma_buffer()) { return false; }
    uint64_t lba = static_cast<uint64_t>(block_num) * sectors_per_block_;
    bool ok =
        ahci_.write(port_index_, lba, static_cast<uint16_t>(sectors_per_block_), dma_buf_phys_);
    if (!ok) { cinux::lib::kprintf("[EXT2] write_block(%u) I/O failed\n", block_num); }
    return ok;
}
```

块号转 LBA（乘以每块的扇区数），然后调用 AHCI 的 `write`。调用方需事先填好 DMA 缓冲区。

`write_superblock` 用扇区地址直接写，因为 superblock 位置固定在字节偏移 1024：

```cpp
bool Ext2::write_superblock() {
    constexpr uint64_t SB_LBA     = EXT2_SUPERBLOCK_OFFSET / EXT2_SECTOR_SIZE;
    constexpr uint16_t SB_SECTORS = EXT2_SUPERBLOCK_SIZE / EXT2_SECTOR_SIZE;
    auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    memcpy(dma, &sb_, sizeof(Ext2Superblock));
    if (!ahci_.write(port_index_, SB_LBA, SB_SECTORS, dma_buf_phys_)) { return false; }
    return true;
}
```

`write_bgdt` 和 `write_disk_inode` 都用了经典的 read-modify-write 模式——一个磁盘块里可能装了多个 entry，不能直接覆盖整个块，得先读出来修改指定位置再写回。它们的定位逻辑和对应的 read 函数完全一致，就不重复贴代码了。

以 `write_disk_inode` 为例，定位步骤是：inode 号算出 group → group 的 inode table 起始块 → group 内索引算出字节偏移 → 算出落在哪个磁盘块的哪个位置。然后 read 块、patch inode 数据、write 回。128 字节 inode + 4K 块大小下一个块能放 32 个 inode，所以 inode 跨越块边界的情况不会出现。

### 块指针解析与分配 get_or_alloc_block

这是最复杂的基础设施函数。给定文件逻辑块号，返回对应磁盘块号；如果未分配就自动调 `alloc_block` 分配。

ext2 inode 有 15 个块指针：前 12 个直接块，第 13 个一次间接，第 14、15 个分别是二次和三次间接。我们只支持直接块和一次间接。

```cpp
uint32_t Ext2::get_or_alloc_block(Ext2Inode& disk, uint32_t file_block) {
    if (file_block < EXT2_DIRECT_BLOCKS) {
        // Direct block
        if (disk.i_block[file_block] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) { return 0; }
            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i) { dma[i] = 0; }
            if (!write_block(blk)) { free_block(blk); return 0; }
            disk.i_block[file_block] = blk;
        }
        return disk.i_block[file_block];
    }
```

直接块部分很简单：已有值就直接返回，是 0 就分配、清零、写盘、记到指针里。新块必须清零，因为后续写入可能只覆盖一部分，剩余区域不应残留旧数据。

接下来是一次间接块（file_block >= 12）：

```cpp
    if (file_block < EXT2_DIRECT_BLOCKS + block_size_ / sizeof(uint32_t)) {
        uint32_t indirect_idx = file_block - EXT2_DIRECT_BLOCKS;

        // Allocate the indirect block itself if needed
        if (disk.i_block[EXT2_INDIRECT_BLOCK] == 0) {
            uint32_t indirect_blk = alloc_block();
            if (indirect_blk == 0) { return 0; }
            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i) { dma[i] = 0; }
            if (!write_block(indirect_blk)) { free_block(indirect_blk); return 0; }
            disk.i_block[EXT2_INDIRECT_BLOCK] = indirect_blk;
        }

        uint32_t indirect_blk = disk.i_block[EXT2_INDIRECT_BLOCK];
        if (!read_block(indirect_blk)) { return 0; }
        auto* indirect = reinterpret_cast<uint32_t*>(dma_buf_virt_);

        if (indirect[indirect_idx] == 0) {
            uint32_t data_blk = alloc_block();
            if (data_blk == 0) { return 0; }
            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i) { dma[i] = 0; }
            if (!write_block(data_blk)) { free_block(data_blk); return 0; }
            // Re-read indirect block (DMA buf was clobbered by zeroing data block)
            if (!read_block(indirect_blk)) { return 0; }
            indirect               = reinterpret_cast<uint32_t*>(dma_buf_virt_);
            indirect[indirect_idx] = data_blk;
            if (!write_block(indirect_blk)) { return 0; }
        }

        if (!read_block(indirect_blk)) { return 0; }
        indirect = reinterpret_cast<uint32_t*>(dma_buf_virt_);
        return indirect[indirect_idx];
    }

    return 0;  // Beyond singly-indirect: not supported
}
```

间接块的原理：`i_block[12]` 指向一个"指针块"，里面全是 `uint32_t` 值，每个值是一个数据块号。4K 块下能存 1024 个指针。

处理间接块时有个麻烦：DMA 缓冲区只有一个块大小。分配数据块时要写零到缓冲区并写盘，这就覆盖了刚读出的间接块内容。所以分配完后必须重新读取间接块、填入新块号、再写回。这种"读-被覆盖-重读-修改-写回"的模式在单缓冲区架构下是必须的。

直接块加一次间接块能支持 `12 + 1024 = 1036` 个块，4K 块大小下约 4MB 文件上限，教学内核足够了。超出范围直接返回 0。

## 设计决策

所有分配操作都是同步的——找到空闲资源就立即刷盘，没有 write-back cache 或 journaling。每次 `alloc_block` / `alloc_inode` 产生三次磁盘写，性能不是最优但正确性有保证。

DMA 缓冲区只有一个块大小，处理间接块时必须反复读写同一个缓冲区。更高效的实现会用 buffer cache，但复杂度会大幅增加。

扫描策略是朴素的线性搜索。Linux 内核有 Orlov 分配器等优化策略来考虑局部性，我们没有做这些优化。

## 扩展方向

可以实现二次/三次间接块打破 4MB 文件上限；引入 buffer cache 减少磁盘 I/O；使用 Orlov 分配策略优化文件布局；或者加 journaling 升级成 ext3。

## 参考资料

- The Second Extended Filesystem (ext2) — https://www.nongnu.org/ext2-doc/
- Linux kernel source: `fs/ext2/balloc.c`, `fs/ext2/ialloc.c`
