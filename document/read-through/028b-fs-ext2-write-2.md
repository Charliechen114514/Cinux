---
title: 028b-fs-ext2-write-2 · Ext2 写入
---

# ext2 文件创建、目录操作与删除

## 概览

上一篇文章我们把 ext2 分配器的底层机制搞清楚了。现在站到更高的层次来看文件系统的"业务逻辑"：创建文件、创建目录、往目录里添加或删除目录项、把文件/目录从文件系统中移除。

这篇文章覆盖 `create()`、`mkdir()`、`unlink()`、`add_dir_entry()`、`remove_dir_entry()` 和 `Ext2FileOps::write()`。读完之后你会明白，从用户态发出一个 `touch /hello.txt`，在 ext2 层面到底发生了多少事情。

## 架构图

```
create()                    mkdir()
  │                           │
  ├─ alloc_inode()            ├─ alloc_inode()
  ├─ init Ext2Inode (REG)     ├─ alloc_block()        ← 目录数据块
  ├─ write_disk_inode()       ├─ init Ext2Inode (DIR)
  ├─ add_dir_entry()          ├─ write_disk_inode()
  └─ write_disk_inode(parent) ├─ init "." and ".."
                              ├─ add_dir_entry()
                              ├─ parent.i_links_count++
                              ├─ bg_used_dirs_count++
                              └─ write_disk_inode(parent)

add_dir_entry()              remove_dir_entry()
  │                           │
  ├─ 计算 required_rec_len    ├─ 扫描目录数据块
  ├─ 在已有块中找空洞         ├─ 找到匹配 entry
  │  (rec_len 拆分)           ├─ pos==0: 清 inode=0
  ├─ 没空间→alloc_block()     └─ pos>0: 合并到前一个 rec_len
  └─ write_block()

unlink()
  ├─ remove_dir_entry()
  ├─ i_links_count--
  ├─ links==0 → free all blocks + free_inode
  └─ is_dir → bg_used_dirs_count--, parent.links--
```

## 代码精讲

### 目录项操作 add_dir_entry

`add_dir_entry` 是 `create` 和 `mkdir` 的公共依赖。ext2 目录项（`Ext2DirEntry`）是变长结构：8 字节头部 + 不定长文件名，总长度向上取整到 4 字节。entry 通过 `rec_len` 串联，最后一个 entry 的 `rec_len` 延伸到块尾——用"浪费尾部空间"简化分配。

```cpp
bool Ext2::add_dir_entry(uint32_t dir_ino, Ext2Inode& dir_disk, uint32_t entry_ino,
                         const char* name, uint32_t name_len, Ext2FileType file_type) {
    uint32_t required_rec_len = EXT2_DIR_ENTRY_HDR_SIZE + name_len;
    required_rec_len          = (required_rec_len + 3) & ~3u;

    uint32_t bs           = block_size_;
    uint32_t total_blocks = (dir_disk.i_size + bs - 1) / bs;
    if (total_blocks == 0) { total_blocks = 1; }

    // Try to find space in existing blocks by splitting the last entry
    for (uint32_t b = 0; b < total_blocks && b < EXT2_DIRECT_BLOCKS; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0) { continue; }
        if (!read_block(blk)) { return false; }

        auto*    block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
        uint32_t pos        = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) { break; }
            auto* entry = reinterpret_cast<Ext2DirEntry*>(block_data + pos);
            if (entry->rec_len == 0) { break; }

            uint32_t entry_min = EXT2_DIR_ENTRY_HDR_SIZE + entry->name_len;
            entry_min          = (entry_min + 3) & ~3u;
            uint32_t extra = entry->rec_len - entry_min;

            if (extra >= required_rec_len) {
                // Split: shrink current entry, insert new one after it
                entry->rec_len = static_cast<uint16_t>(entry_min);

                auto* new_entry      = reinterpret_cast<Ext2DirEntry*>(block_data + pos + entry_min);
                new_entry->inode     = entry_ino;
                new_entry->rec_len   = static_cast<uint16_t>(extra);
                new_entry->name_len  = static_cast<uint8_t>(name_len);
                new_entry->file_type = static_cast<uint8_t>(file_type);
                for (uint32_t i = 0; i < name_len; ++i) {
                    new_entry->name[i] = name[i];
                }
                if (!write_block(blk)) { return false; }
                return true;
            }
            pos += entry->rec_len;
        }
    }
```

核心概念是"rec_len 拆分"。假设目录里 ".." 的 `rec_len` 是 4084（延伸到块尾），但实际只需要 12 字节（`8 + 2 = 10`，对齐到 12）。`extra = 4084 - 12 = 4072` 字节完全可以插入新 entry。拆分就是把当前 entry 的 `rec_len` 缩到最小值，紧跟着放新 entry，新 entry 的 `rec_len` 等于原来 extra 的值。

如果已有块都找不到空间，就分配新块：

```cpp
    uint32_t new_block_idx = total_blocks;
    if (new_block_idx >= EXT2_DIRECT_BLOCKS) { return false; }

    uint32_t new_blk = alloc_block();
    if (new_blk == 0) { return false; }

    auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    for (uint32_t i = 0; i < bs; ++i) { dma[i] = 0; }

    auto* new_entry      = reinterpret_cast<Ext2DirEntry*>(dma);
    new_entry->inode     = entry_ino;
    new_entry->rec_len   = static_cast<uint16_t>(bs);
    new_entry->name_len  = static_cast<uint8_t>(name_len);
    new_entry->file_type = static_cast<uint8_t>(file_type);
    for (uint32_t i = 0; i < name_len; ++i) { new_entry->name[i] = name[i]; }

    if (!write_block(new_blk)) { free_block(new_blk); return false; }

    dir_disk.i_block[new_block_idx] = new_blk;
    dir_disk.i_size += bs;
    dir_disk.i_blocks = ((dir_disk.i_size + bs - 1) / bs) * (bs / 512);
    if (!write_disk_inode(dir_ino, dir_disk)) { return false; }
    return true;
}
```

新块里放一个 entry，`rec_len` 设为整个块大小——剩余空间"吃"进去，后续插入新 entry 时可以拆分。然后更新目录 inode 的 `i_block`、`i_size`、`i_blocks` 并回写。

### 目录项删除 remove_dir_entry

删除不是真正的"删除"，而是修改 `rec_len` 跳过被删 entry。

```cpp
bool Ext2::remove_dir_entry(uint32_t /*dir_ino*/, const Ext2Inode& dir_disk, const char* name,
                            uint32_t name_len, uint32_t& out_entry_ino) {
    uint32_t bs = block_size_;
    uint32_t total_blocks = (dir_disk.i_size + bs - 1) / bs;
    if (total_blocks > EXT2_DIRECT_BLOCKS) { total_blocks = EXT2_DIRECT_BLOCKS; }

    for (uint32_t b = 0; b < total_blocks; ++b) {
        uint32_t blk = dir_disk.i_block[b];
        if (blk == 0) { continue; }
        if (!read_block(blk)) { return false; }

        auto*    block_data = reinterpret_cast<uint8_t*>(dma_buf_virt_);
        uint32_t pos = 0, prev_pos = 0;

        while (pos < bs) {
            if (pos + EXT2_DIR_ENTRY_HDR_SIZE > bs) { break; }
            auto* entry = reinterpret_cast<Ext2DirEntry*>(block_data + pos);
            if (entry->rec_len == 0) { break; }

            if (entry->inode != 0 && entry->name_len == name_len) {
                bool match = true;
                for (uint32_t i = 0; i < name_len; ++i) {
                    if (entry->name[i] != name[i]) { match = false; break; }
                }
                if (match) {
                    out_entry_ino = entry->inode;
                    if (pos == 0) {
                        entry->inode = 0;  // First entry: can't merge back
                    } else {
                        auto* prev = reinterpret_cast<Ext2DirEntry*>(block_data + prev_pos);
                        prev->rec_len += entry->rec_len;  // Merge into previous
                    }
                    if (!write_block(blk)) { return false; }
                    return true;
                }
            }
            prev_pos = pos;
            pos += entry->rec_len;
        }
    }
    return false;
}
```

匹配到目标后两种处理方式：不是块首 entry 时，把它的 `rec_len` 加到前一个 entry 上——遍历时自然跳过被删 entry；如果是块首 entry（pos==0），没有前驱可合并，只能把 `inode` 清零标记为空洞。

### 文件创建 create

有了 `alloc_inode` 和 `add_dir_entry`，`create` 就是顺理成章的了。

```cpp
Inode* Ext2::create(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) { return nullptr; }
    if (lookup_in_dir(parent_ino, name, name_len) != 0) { return nullptr; }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) { return nullptr; }

    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) { return nullptr; }

    Ext2Inode new_disk;
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
    }
    new_disk.i_mode        = EXT2_S_IFREG | 0644;
    new_disk.i_links_count = 1;
    // all other fields zeroed above

    if (!write_disk_inode(new_ino, new_disk)) { free_inode(new_ino); return nullptr; }
    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Regular)) {
        free_inode(new_ino);
        return nullptr;
    }
    if (!write_disk_inode(parent_ino, dir_disk)) { return nullptr; }
    return get_cached_inode(new_ino);
}
```

参数检查和重名检查后，读取父目录 inode、分配新 inode、清零并初始化为普通文件（mode `S_IFREG | 0644`，links=1，size=0），写回 inode、添加目录项、回写父目录 inode。每步失败都回滚之前分配的资源。`add_dir_entry` 可能修改了 `dir_disk`（比如分配了新块），所以之后要回写父目录 inode。

### 目录创建 mkdir

`mkdir` 比 `create` 复杂——新目录需要一个数据块来存放 "." 和 ".."。

```cpp
Inode* Ext2::mkdir(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0 || name_len > EXT2_NAME_MAX) { return nullptr; }
    if (lookup_in_dir(parent_ino, name, name_len) != 0) { return nullptr; }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) { return nullptr; }

    uint32_t new_ino = alloc_inode();
    if (new_ino == 0) { return nullptr; }
    uint32_t data_blk = alloc_block();
    if (data_blk == 0) { free_inode(new_ino); return nullptr; }
```

先分配 inode 和数据块。如果 block 分配失败，释放之前分配的 inode。

```cpp
    Ext2Inode new_disk;
    for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
        reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
    }
    new_disk.i_mode        = EXT2_S_IFDIR | 0755;
    new_disk.i_size        = block_size_;
    new_disk.i_links_count = 2;  // "." + parent's entry
    new_disk.i_blocks      = block_size_ / 512;
    new_disk.i_block[0]    = data_blk;
```

新目录的关键参数：mode 是 `S_IFDIR | 0755`；`i_size` 为一个块大小；`i_links_count` 初始为 2（"." 指向自己 + 父目录中的 entry 指向自己）；`i_block[0]` 指向刚分配的数据块。

```cpp
    auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
    for (uint32_t i = 0; i < block_size_; ++i) { dma[i] = 0; }

    // "." entry
    auto* dot            = reinterpret_cast<Ext2DirEntry*>(dma);
    dot->inode           = new_ino;
    dot->name_len        = 1;
    dot->file_type       = static_cast<uint8_t>(Ext2FileType::Directory);
    dot->name[0]         = '.';
    uint32_t dot_rec_len = (EXT2_DIR_ENTRY_HDR_SIZE + 1 + 3) & ~3u;  // 12
    dot->rec_len         = static_cast<uint16_t>(dot_rec_len);

    // ".." entry
    auto* dotdot      = reinterpret_cast<Ext2DirEntry*>(dma + dot_rec_len);
    dotdot->inode     = parent_ino;
    dotdot->name_len  = 2;
    dotdot->file_type = static_cast<uint8_t>(Ext2FileType::Directory);
    dotdot->name[0]   = '.'; dotdot->name[1] = '.';
    dotdot->rec_len   = static_cast<uint16_t>(block_size_ - dot_rec_len);

    if (!write_block(data_blk)) { free_block(data_blk); free_inode(new_ino); return nullptr; }
```

数据块初始化为 "." 指向自己（12 字节，4 字节对齐）和 ".." 指向父目录（`rec_len` 延伸到块尾）。这种"吃掉剩余空间"的布局正是 `add_dir_entry` 能做 rec_len 拆分的前提。

```cpp
    if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Directory)) {
        free_block(data_blk); free_inode(new_ino); return nullptr;
    }

    dir_disk.i_links_count++;  // new subdir's ".." points to parent

    uint32_t new_group = (new_ino - 1) / inodes_per_group_;
    if (new_group < group_count_) {
        bgdt_[new_group].bg_used_dirs_count++;
        write_bgdt(new_group);
    }

    if (!write_disk_inode(parent_ino, dir_disk)) { return nullptr; }
    return get_cached_inode(new_ino);
}
```

添加父目录 entry 后，父目录的 `i_links_count` 要加 1（新目录的 ".." 相当于给父目录增加一个硬链接），新目录所在 group 的 `bg_used_dirs_count` 也要加 1。

### 文件删除 unlink

`unlink` 是最长的函数，因为要处理最多情况：从父目录移除 entry、递减链接计数、链接归零就释放所有资源。

```cpp
int Ext2::unlink(uint32_t parent_ino, const char* name, uint32_t name_len) {
    if (name == nullptr || name_len == 0) { return -1; }

    Ext2Inode dir_disk;
    if (!read_disk_inode(parent_ino, dir_disk)) { return -1; }

    uint32_t entry_ino = 0;
    if (!remove_dir_entry(parent_ino, dir_disk, name, name_len, entry_ino)) { return -1; }

    Ext2Inode target_disk;
    if (!read_disk_inode(entry_ino, target_disk)) { return -1; }

    if (target_disk.i_links_count > 0) { target_disk.i_links_count--; }
```

读取父目录、移除目录项（同时得到被删目标的 inode 号）、读取目标 inode、递减链接计数。

```cpp
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
                    if (indirect[i] != 0) { free_block(indirect[i]); }
                }
            }
            free_block(indirect_blk);
            target_disk.i_block[EXT2_INDIRECT_BLOCK] = 0;
        }

        target_disk.i_size = 0;
        target_disk.i_blocks = 0;
        write_disk_inode(entry_ino, target_disk);
        free_inode(entry_ino);

        if ((target_disk.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
            uint32_t group = (entry_ino - 1) / inodes_per_group_;
            if (group < group_count_ && bgdt_[group].bg_used_dirs_count > 0) {
                bgdt_[group].bg_used_dirs_count--;
                write_bgdt(group);
            }
            if (dir_disk.i_links_count > 0) { dir_disk.i_links_count--; }
        }
    } else {
        write_disk_inode(entry_ino, target_disk);
    }
```

链接归零后释放所有数据块：先释放 12 个直接块，再释放间接块指向的数据块和间接块本身。然后清空 inode 的 size/blocks 并写回，释放 inode。如果目标是目录，还要递减 `bg_used_dirs_count` 和父目录 `i_links_count`。

```cpp
    // Invalidate cache entry
    for (uint32_t i = 0; i < EXT2_INODE_CACHE_SIZE; ++i) {
        if (inode_cache_[i].in_use && inode_cache_[i].ino == entry_ino) {
            inode_cache_[i].in_use = false;
            break;
        }
    }
    write_disk_inode(parent_ino, dir_disk);
    return 0;
}
```

最后让 inode 缓存中的对应条目失效，回写父目录 inode。`unlink` 同时处理文件和目录删除——空目录检查在上层的 `sys_rmdir` 中完成。

### Ext2FileOps::write

文件写入把用户缓冲区的数据写到文件指定偏移处：

```cpp
int64_t Ext2FileOps::write(Inode* inode, uint64_t offset, const void* buf, uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr) { return -1; }
    if (count == 0) { return 0; }

    auto*      cached = static_cast<Ext2CachedInode*>(inode->fs_private);
    Ext2Inode& disk   = cached->disk_inode;
    uint32_t bs = ext2_.block_size();
    auto*    src           = static_cast<const uint8_t*>(buf);
    uint64_t total_written = 0;

    while (total_written < count) {
        uint64_t file_block   = (offset + total_written) / bs;
        uint64_t block_offset = (offset + total_written) % bs;
        uint64_t chunk = bs - block_offset;
        if (chunk > count - total_written) { chunk = count - total_written; }
        if (file_block > EXT2_DIRECT_BLOCKS) { break; }

        uint32_t disk_block = ext2_.get_or_alloc_block(disk, static_cast<uint32_t>(file_block));
        if (disk_block == 0) { break; }

        if (block_offset != 0 || chunk != bs) {
            if (!ext2_.read_block(disk_block)) { break; }
        } else {
            auto* dma = reinterpret_cast<uint8_t*>(ext2_.dma_buf_virt());
            for (uint32_t i = 0; i < bs; ++i) { dma[i] = 0; }
        }

        auto* dst = reinterpret_cast<uint8_t*>(ext2_.dma_buf_virt()) + block_offset;
        for (uint64_t i = 0; i < chunk; ++i) { dst[i] = src[total_written + i]; }
        if (!ext2_.write_block(disk_block)) { break; }
        total_written += chunk;
    }

    if (total_written > 0) {
        uint64_t new_end = offset + total_written;
        if (new_end > disk.i_size) {
            disk.i_size = static_cast<uint32_t>(new_end);
            disk.i_blocks = ((disk.i_size + bs - 1) / bs) * (bs / 512);
        }
        ext2_.write_disk_inode(static_cast<uint32_t>(inode->ino), disk);
        inode->size = disk.i_size;
    }
    return static_cast<int64_t>(total_written);
}
```

写入循环和 `read` 类似：计算逻辑块号和块内偏移，`get_or_alloc_block` 自动分配未分配的块。如果写入不是覆盖整个块，先读出原有内容再修改；如果是整块覆盖就清零 DMA 缓冲区。然后把用户数据拷贝到 DMA 缓冲区正确位置，写回磁盘。写入完成后更新 on-disk inode 的 `i_size` 和 `i_blocks`，同时更新 VFS Inode 的 `size`。

## 设计决策

ext2 目录项的变长链表结构（通过 `rec_len` 串联）看起来粗糙——删除 entry 不能回收空间，只能标记为空洞或合并到前一个 entry。好处是简单：顺着 `rec_len` 走就能遍历完目录，不需要维护空闲链表。Linux ext2 也是这么做的，ext4 后来引入了 htree 加速大目录。

`unlink` 同时处理文件和目录删除。空目录检查放在上层 `sys_rmdir` 中（用 `readdir(target, 2, ...)` 检查第三个 entry 是否存在），ext2 层不区分。

## 扩展方向

可以实现目录项空洞复用（`add_dir_entry` 中检查 inode=0 的 entry）；支持 doubly/triple indirect blocks 打破文件大小上限；加入 htree 索引加速大目录查找；或者添加 journaling 变成 ext3。

## 参考资料

- The Second Extended Filesystem (ext2) — https://wiki.osdev.org/Ext2
- Linux kernel: `fs/ext2/namei.c` (create, mkdir, unlink)
- POSIX.1-2017: `creat()`, `mkdir()`, `unlink()`, `rmdir()` semantics
