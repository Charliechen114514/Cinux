---
title: 028b-fs-ext2-write-2 · Ext2 写入
---

# 028b-2 Tutorial: ext2 目录操作——创建与删除

> 标签：ext2、目录操作、create、mkdir、unlink、目录项、文件写入
> 前置：[028b-fs-ext2-write-1](028b-fs-ext2-write-1.md)

## 前言

上一篇我们把 ext2 写入的地基打好了——块分配器能从 bitmap 里挖出空闲块，inode 分配器能找到空闲的文件控制块，元数据回写保证每次操作后磁盘状态一致。但这些函数本身并不能直接被上层调用，它们只是工具函数。用户需要的是 `touch /hello.txt` 创建文件、`mkdir /subdir` 建目录、`rm /hello.txt` 删文件——这些操作背后需要一套完整的数据结构操作链。

我们现在要做的是在这套分配器基础设施之上搭建三个核心操作：`create()`（创建普通文件）、`mkdir()`（创建目录）、`unlink()`（删除文件或目录）。这三个操作涉及 ext2 最精巧的数据结构操作——目录项链表的插入和删除。目录项链表看起来简单，实际上暗藏玄机，尤其是 `rec_len` 的切分和合并操作，稍有不慎就会把整个目录结构搞崩。我在这块 debug 的时间比写代码还长，不过搞通之后确实有一种豁然开朗的感觉。

## 第一步——目录项回顾：一个伪装成链表的变长结构体数组

在动手之前，我们先快速回顾一下 ext2 目录项的内存布局。每个目录项（directory entry，简称 dir entry）由 8 字节头部加上变长文件名组成。头部四个字段分别是：inode 号（4 字节，uint32_t）、rec_len（2 字节，uint16_t，"record length"——从本 entry 头部到下一个 entry 头部的字节数）、name_len（1 字节，文件名长度）、file_type（1 字节，文件类型——Regular 或 Directory）。文件名紧跟在头部后面，整个 entry 的大小（8 + name_len）必须向上对齐到 4 字节的倍数。但 rec_len 不一定等于这个对齐后的值——它通常大于或等于这个值。

这里最关键的字段是 `rec_len`。一个数据块里的所有目录项通过 `rec_len` 串联——从块头开始，每读一个 entry 就前进 `rec_len` 字节，直到遍历完整个块。最后一个 entry 的 `rec_len` 会延伸到块末尾。你可以把它理解为一个"带长度的链表"——和传统的链表用指针连接不同，ext2 的目录项用"距离"连接。这种设计有一个巨大优势：不需要额外的指针字段（节省空间），也不需要遍历时解引用指针（对磁盘 I/O 友好）。

但这也意味着 `rec_len` 的正确性是整个目录结构的命脉。如果 `rec_len` 计算错了哪怕 1 字节，整个目录的遍历就会跑偏——后续的 entry 解析会读到垃圾数据，导致文件"消失"或者 inode 号错乱。ext2 的 fsck 工具（`e2fsck`）在修复损坏的文件系统时，相当大一部分工作就是在修复目录项的 `rec_len` 链。

## 第二步——add_dir_entry：在现有空间中切出一条缝

`add_dir_entry()` 是目录项插入的核心，也是 ext2 写入操作中最精巧的部分。它的策略是遍历目录的所有数据块，在每个块中扫描每一个 entry，计算该 entry 的实际使用空间和 `rec_len` 的差值。如果差值足够放下新 entry，就"切分"这个 entry：把当前 entry 的 `rec_len` 缩减到刚好够用，然后在释放出来的空间里构造新 entry。

先别急，我们用具体数字来说明。假设一个块大小 4096 字节，里面有一个 entry "hello"（name_len=5，对齐后 8+5=13→16 字节），rec_len = 4096（因为它把从自己到块末尾的所有空间都"霸占"了）。现在要插入一个新 entry "world"（name_len=5，对齐后 16 字节）。扫描到 "hello" 时，计算它的实际使用空间 `entry_min = (8 + 5 + 3) & ~3 = 16`，空闲空间 `extra = 4096 - 16 = 4080`。4080 >= 16，空间足够！于是切分："hello" 的 rec_len 变成 16，新 entry "world" 的 rec_len 变成 4084（占据剩余全部空间）。

```cpp
// 计算 entry 的最小尺寸：8 字节头 + name_len，向上 4 字节对齐
uint32_t entry_min = EXT2_DIR_ENTRY_HDR_SIZE + entry->name_len;
entry_min = (entry_min + 3) & ~3u;  // 向上对齐到 4 的倍数

// 检查这个 entry 后面有多少空闲空间
uint32_t extra = entry->rec_len - entry_min;

if (extra >= required_rec_len) {
    // 切分：缩减当前 entry，插入新 entry
    entry->rec_len = static_cast<uint16_t>(entry_min);
    auto* new_entry = reinterpret_cast<Ext2DirEntry*>(block_data + pos + entry_min);
    new_entry->inode = entry_ino;
    new_entry->rec_len = static_cast<uint16_t>(extra);
    // ... 填入 name_len, file_type, name
}
```

这里有一个值得讨论的设计选择：新 entry 的 `rec_len` 被设为 `extra`（剩余的全部空间），而不是 `required_rec_len`（刚好够用的空间）。这是 ext2 规范推荐的做法——每个 entry 的 rec_len 应该覆盖从自己到下一个 entry（或块末尾）的全部空间。这样即使有"空洞"（inode=0 的已删除 entry），遍历时也不会跳跃到未初始化的区域。如果我们在中间插入的 entry 只占用 `required_rec_len`，那后面会有一个无人管理的区域，这可能导致 fsck 报错。

如果所有现有块都没有足够空间，函数会分配一个新块，在里面放一个 `rec_len = block_size` 的大 entry。这意味着目录会按整块增长——每次加一个块就是 4K 的目录大小增量。Linux 的 ext2 在这方面有优化：它会尝试把目录项紧凑地排列在多个块中，并且在使用 htree 索引（ext3/4 特性）时可以高效地在大型目录中查找。但 Cinux 不需要这种优化——我们的目录通常只有几个到十几个 entry。

## 第三步——remove_dir_entry：合并相邻空间

删除操作的关键在于处理被删除 entry 的 `rec_len`。有两种情况需要分别处理。

如果被删除的 entry 是块内的第一个 entry（pos == 0），我们不能简单地把它"吞掉"——因为块内的遍历总是从 pos=0 开始的。所以只能把它的 inode 字段清零，保留 rec_len 不变。遍历目录时遇到 inode=0 的 entry 就跳过，相当于一个"空洞"。这种方式虽然浪费了一些空间（entry 的 rec_len 仍然占据着位置），但保持了目录项链表的结构完整性。

如果被删除的 entry 不是第一个，那就简单了——把它的 `rec_len` 加到前一个 entry 的 `rec_len` 上，实现空间合并：

```cpp
if (pos == 0) {
    entry->inode = 0;  // 清除 inode，保留 rec_len——变成"空洞"
} else {
    // 合并：前一个 entry 吸收被删除 entry 的空间
    auto* prev = reinterpret_cast<Ext2DirEntry*>(block_data + prev_pos);
    prev->rec_len += entry->rec_len;
}
```

这种"合并到前一个"的策略有一个隐含前提：前一个 entry 一定是有效的（inode 非零）。在 ext2 的目录项链表设计中，除了块首可能存在"空洞"外，后面的 entry 都是紧挨着排列的。所以前一个 entry 一定存在且有效。这个前提听起来理所当然，但如果你的代码里有什么 bug 导致了"空洞"出现在中间，那合并操作可能会把多个空洞串联起来，形成更大的空洞——这倒也不影响正确性，只是会浪费空间。

你可能会问：为什么不在删除块首 entry 时也做合并？答案是不能——没有"更前面的 entry"来吸收这个空间。块首 entry 的 rec_len 是遍历的起点，不能随便改。唯一的办法是清零 inode，等待将来在这个块里插入新 entry 时复用这个空间。实际上 Linux 的 `ext2_delete_entry()` 有一个额外的优化——如果被删除的 entry 是块首且后面的 entry 也是空洞，它会尝试把多个连续空洞合并成一个大的空洞 entry。但 Cinux 的简单实现不做这种优化。

## 第四步——create：创建普通文件的完整流程

有了 `add_dir_entry` 之后，`create()` 就是一个组装步骤。让我们从头到尾走一遍创建 `/hello.txt` 的完整流程。

首先是重名检查。调用 `lookup_in_dir(parent_ino, name, name_len)` 在父目录中搜索同名 entry。如果找到了，直接返回 nullptr——Cinux 不支持覆盖式创建。然后读取父目录的 inode 到内存（`read_disk_inode`），这一步是为了后续添加目录项时能够访问父目录的数据块指针。

接下来分配 inode——调用 `alloc_inode()` 获取一个空闲 inode 号。假设返回 42。然后初始化这个 inode：清零整个 `Ext2Inode` 结构体（128 字节），设置 `i_mode = S_IFREG | 0644`（普通文件，权限 rw-r--r--）、`i_links_count = 1`（刚创建，只有一个目录项指向它）、`i_size = 0`（空文件）、`i_blocks = 0`。然后调用 `write_disk_inode(42, new_disk)` 把这个初始化后的 inode 写到磁盘上的 inode 表中。

```cpp
// 初始化新 inode——清零后设置关键字段
Ext2Inode new_disk;
for (uint32_t i = 0; i < sizeof(Ext2Inode); ++i) {
    reinterpret_cast<uint8_t*>(&new_disk)[i] = 0;
}
new_disk.i_mode        = EXT2_S_IFREG | 0644;
new_disk.i_links_count = 1;
// i_size = 0, i_blocks = 0, 所有 i_block[] = 0（已清零）
```

然后调用 `add_dir_entry(parent_ino, dir_disk, 42, "hello.txt", 10, Ext2FileType::Regular)` 在父目录中插入新 entry。这一步可能修改父目录的 `i_size` 和 `i_block`（如果需要分配新的目录数据块的话），所以最后还要 `write_disk_inode(parent_ino, dir_disk)` 写回父目录的 inode。

这里有一个设计选择值得讨论：为什么先写回新 inode，再添加目录项？如果添加目录项失败了怎么办？这时候新 inode 已经写到磁盘上了，但没有目录项指向它——它就变成了一个"孤儿 inode"。严格来说，我们应该在添加目录项失败时回滚——把新分配的 inode 释放掉。Cinux 的代码确实做了这个清理：

```cpp
if (!add_dir_entry(parent_ino, dir_disk, new_ino, name, name_len, Ext2FileType::Regular)) {
    free_inode(new_ino);  // 回滚：释放刚分配的 inode
    return nullptr;
}
```

这种回滚并不完美——`write_disk_inode` 已经把初始化后的 inode 写到了磁盘上，而 `free_inode` 只是清除 bitmap 中的 bit 并递增计数器，并没有清零 inode 表中的数据。下次分配到同一个 inode 时，它里面还残留着上次的数据。不过由于新分配的 inode 总是会被初始化（清零 + 设置字段），所以这不会导致实际问题。真正的文件系统会使用事务（journal）来保证操作的原子性——要么 inode 和目录项都成功写入，要么什么都不发生。但 journal 的实现复杂度远超我们的教学目标。

## 第五步——mkdir：比 create 多出来的那些事

`mkdir()` 比 `create()` 复杂不少。创建目录不仅仅是分配一个 inode，还需要：分配一个数据块、写入 "." 和 ".." 两个特殊目录项、更新父目录的 `i_links_count`、更新 BGDT 的 `bg_used_dirs_count`。让我们逐步拆解。

新建目录的 inode 初始 `i_links_count = 2`——为什么是 2 而不是 1？因为有两个东西指向这个新目录：一个是父目录中即将添加的 entry（比如 `/subdir` 中的 `subdir`），另一个是新目录自己的 "." entry。".." 指向父目录，不指向自己。这是 ext2 规范中硬链接计数的一个微妙之处。

而父目录的 `i_links_count` 要递增 1，因为新目录的 ".." entry 指向了父目录——".." 本质上就是一个硬链接。这就是为什么在 Unix 系统中，一个空目录的硬链接数通常是 2（自身 + "."），而有子目录的目录硬链接数是 2 + 子目录数。你可以在 Linux 系统上试试 `ls -la` 看目录的硬链接数——每个子目录都会让父目录的 link count 加 1。这个规则在 ext4 中已经不再严格遵守（ext4 使用了 nlink 溢出保护），但在 ext2 中是硬性要求。

"." 和 ".." 的构造需要精确计算 `rec_len`。"." 的 name_len 是 1，对齐后 `rec_len = (8 + 1 + 3) & ~3 = 12`。".." 的 `rec_len` 等于 `block_size - 12`（占据块剩余的所有空间）。这两个 entry 加起来正好占满一个块。

```cpp
// "." entry — 指向自身
auto* dot = reinterpret_cast<Ext2DirEntry*>(dma);
dot->inode     = new_ino;          // 指向自己
dot->name_len  = 1;
dot->file_type = static_cast<uint8_t>(Ext2FileType::Directory);
dot->name[0]   = '.';
uint32_t dot_rec_len = (EXT2_DIR_ENTRY_HDR_SIZE + 1 + 3) & ~3u;  // 12
dot->rec_len   = static_cast<uint16_t>(dot_rec_len);

// ".." entry — 指向父目录
auto* dotdot = reinterpret_cast<Ext2DirEntry*>(dma + dot_rec_len);
dotdot->inode     = parent_ino;    // 指向父目录
dotdot->name_len  = 2;
dotdot->file_type = static_cast<uint8_t>(Ext2FileType::Directory);
dotdot->name[0]   = '.';
dotdot->name[1]   = '.';
dotdot->rec_len   = static_cast<uint16_t>(block_size_ - dot_rec_len);  // 占满剩余空间
```

除了 inode 和目录项，`mkdir()` 还需要更新 BGDT 的 `bg_used_dirs_count`。这个计数器跟踪每个 group 中有多少个目录——它不直接影响文件系统的功能，但分配器可以用它来做优化决策（比如 Orlov 分配器根据各 group 的目录密度来分散新目录）。Cinux 目前不用这个信息，但维护它保证了 BGDT 和实际状态的一致性，也为将来的优化留了接口。

## 第六步——unlink：删除不只是移除目录项

`unlink()` 的名字有点误导——它并不是"取消链接"这么简单。它的完整流程是：先从父目录中移除 entry（调用 `remove_dir_entry`），然后递减目标 inode 的 `i_links_count`。如果 `i_links_count` 降到 0，才真正释放 inode 的所有资源——遍历所有数据块指针调用 `free_block()`，清零 inode 结构体（设置 `i_dtime` 标记删除时间），调用 `free_inode()` 释放 inode 号。

释放数据块时需要处理直接块和单间接块。直接块（`i_block[0..11]`）的释放很简单——遍历 12 个指针，对每个非零的调用 `free_block()`。单间接块的处理更复杂一些：如果 `i_block[12]` 非零，说明有一个间接块，需要先读取它，遍历里面所有非零的数据块指针逐个释放，最后释放间接块本身。间接块本身也占一个 data block，千万别忘了释放它。

```cpp
// 释放单间接块及其引用的所有数据块
if (target_disk.i_block[EXT2_INDIRECT_BLOCK] != 0) {
    uint32_t indirect_blk = target_disk.i_block[EXT2_INDIRECT_BLOCK];
    if (read_block(indirect_blk)) {
        auto* indirect = reinterpret_cast<uint32_t*>(dma_buf_virt_);
        uint32_t ptrs_per_block = bs / sizeof(uint32_t);
        for (uint32_t i = 0; i < ptrs_per_block; ++i) {
            if (indirect[i] != 0) {
                free_block(indirect[i]);
            }
        }
    }
    free_block(indirect_blk);  // 别忘了间接块本身
    target_disk.i_block[EXT2_INDIRECT_BLOCK] = 0;
}
```

如果被删除的是目录而不是文件，还需要额外递减 BGDT 的 `bg_used_dirs_count` 和父目录的 `i_links_count`（因为子目录的 ".." 引用消失了）。这就是 `mkdir()` 中递增的那些计数的逆操作。这种"每个操作都有对应的逆操作"的对称性在文件系统代码中非常普遍——维护好这种对称性是避免资源泄漏的关键。如果你发现 `mkdir` 中有某个 `+1` 操作但在 `unlink` 中没有对应的 `-1`，那几乎可以确定是一个 bug。

### 与 Linux 和 SerenityOS 的目录操作对比

Linux 的 ext2 目录操作使用 `ext2_find_entry()` 和 `ext2_add_link()` 两个独立函数。`ext2_find_entry` 支持通过 htree 索引加速查找（这是 ext3/4 的扩展特性，ext2 本身不支持），`ext2_add_link` 的核心逻辑和 Cinux 类似——也是扫描最后一个 entry 的多余空间来做切分。但 Linux 有一个额外的优化：它会尝试把新 entry 放到和被查找 entry 同一个块组中，提高空间局部性。Linux 的 `ext2_delete_entry()` 同样使用合并 rec_len 的策略，但它还会处理"前一个 entry 也是空洞"的情况——把多个连续空洞的 rec_len 全部合并到第一个有效 entry 上。Linux 的目录操作代码（dir.c）约 350 行，虽然不长但每一行都经过了大量优化和测试。

SerenityOS 采取了完全不同的策略——`add_child()` 和 `remove_child()` 每次操作都重写整个目录内容。具体来说，`add_child()` 先把目录的所有 entry 读出来，过滤掉 inode=0 的空洞，把新 entry 追加到末尾，然后全部写回去。这种"全部重写"的策略实现简单——不需要处理 rec_len 切分的边界情况，不需要区分"块首删除"和"中间删除"——但对于大目录性能很差，因为每次操作都要读写整个目录的全部数据块。Cinux 的增量修改策略在这一点上更接近 Linux，也更接近 ext2 的设计初衷——利用 rec_len 的灵活性来做轻量级的增删操作，而不是每次都重写整个目录。

## 第七步——文件写入：逐块的 read-modify-write

最后来看看 `Ext2FileOps::write()`——这是用户数据真正落盘的路径。文件写入的核心逻辑是按块处理：把写入请求拆分成一个个块大小的 chunk，对每个 chunk 调用 `get_or_alloc_block()` 获取物理块号，然后把数据写入 DMA 缓冲区并写回磁盘。

写入的主循环结构如下：对每个 chunk，先计算它落在文件的哪个逻辑块（`file_block = (offset + total_written) / block_size`）、块内偏移（`block_offset`）、以及本次写入的字节数（`chunk`，取 `block_size - block_offset` 和剩余写入量的较小值）。然后调用 `get_or_alloc_block()` 获取物理块号。如果物理块号是 0 说明分配失败（磁盘满），跳出循环。

这里有一个经典的 read-modify-write 模式。如果写入不是覆盖整个块——要么 `block_offset != 0`（写入起始不在块头），要么 `chunk != block_size`（写入不到块尾）——就需要先读取原块内容，然后只修改其中被写入的部分，最后写回整块。如果恰好是整块写入（从块头写到块尾），就直接清零 DMA 缓冲区后填入新数据，省一次磁盘读取。这个优化看似微小，但对于顺序写入大文件的场景能减少一半的磁盘读取次数。

```cpp
if (block_offset != 0 || chunk != bs) {
    // 部分块写入：先读取原块内容，保留未修改的部分
    if (!ext2_.read_block(disk_block)) {
        break;
    }
} else {
    // 整块写入：直接清零缓冲区，不需要读取原块
    auto* dma = reinterpret_cast<uint8_t*>(ext2_.dma_buf_virt());
    for (uint32_t i = 0; i < bs; ++i) {
        dma[i] = 0;
    }
}
// 在 DMA 缓冲区中填入写入数据
auto* dst = reinterpret_cast<uint8_t*>(ext2_.dma_buf_virt()) + block_offset;
for (uint64_t i = 0; i < chunk; ++i) {
    dst[i] = src[total_written + i];
}
// 写回磁盘
if (!ext2_.write_block(disk_block)) {
    break;
}
```

写入完成后需要更新 inode 的 `i_size`（如果写入导致文件变大了）和 `i_blocks`（以 512 字节扇区为单位），然后调用 `write_disk_inode` 写回。`i_blocks` 的计算是 `((i_size + block_size - 1) / block_size) * (block_size / 512)`——文件占用的块数乘以每块的扇区数。在 ext2 的标准中，`i_blocks` 的单位是 512 字节扇区，不是文件系统块。即使你的块大小是 4K，`i_blocks` 仍然以 512 字节为单位计数。这是一个常见的混淆点——很多人看到 `i_blocks` 以为是"块数"，实际上是"扇区数"。POSIX 的 `stat` 结构体中 `st_blocks` 也是同样的约定——512 字节为单位。这个约定的历史原因在于早期 Unix 磁盘的物理扇区大小就是 512 字节，文件系统块大小和物理扇区大小解耦后，`i_blocks` 仍然保留了以扇区为单位的传统。

你会发现这个写入路径有一个明显的性能瓶颈：每写一个块就要做一次磁盘读取（如果是部分块写入）加上一次磁盘写入。连续写 10 个块就是 20 次 AHCI DMA 操作。Linux 通过 buffer cache 和 delayed write 大幅减少了实际的磁盘 I/O 次数——脏块在内存中停留一段时间后才批量刷盘。但实现 buffer cache 需要一个完整的块缓存子系统（buffer_head 或 folio），这个工程量对于教学内核来说不现实。折中方案是给 Cinux 加一个简单的 write-back 缓存——比如在内存中缓存最近写入的几个块，下次写入同一块时直接在缓存中修改，定期刷盘。但这引入了崩溃一致性的问题——如果内核 panic 了，缓存中的数据就丢了。

另外一个值得讨论的写入路径设计是 Cinux 目前不支持超过 `EXT2_DIRECT_BLOCKS + block_size/4` 个块的文件（即 12 个直接块 + 1024 个间接块，对于 4K 块约 4MB）。`write()` 函数中有一个硬限制：`if (file_block > EXT2_DIRECT_BLOCKS) break;`——等等，这个检查实际上是 `if (file_block > EXT2_DIRECT_BLOCKS)` 而不是 `>=`，说明它只允许直接块范围外加一个间接块的范围。对于教学内核来说这个限制完全可以接受——我们不会写超过 4MB 的文件。但如果将来要支持更大的文件，就需要实现双间接块和三间接块的寻址逻辑，那会使 `get_or_alloc_block` 复杂不少。

## 收尾

这一篇我们覆盖了 ext2 写入操作的核心逻辑：目录项的插入和删除、文件的创建和删除、目录的创建和删除、以及文件数据的逐块写入。目录项的 `rec_len` 切分和合并是 ext2 中最精巧的部分之一，理解了这套机制之后，你会发现 ext2 的目录结构其实是一种非常优雅的变长记录管理方案——没有内存分配、没有指针追链、没有碎片整理，全靠 `rec_len` 的加减法就搞定了动态增删。这和很多其他文件系统（比如 FAT 的固定 32 字节目录项、NTFS 的 B+ 树索引）形成了鲜明对比。

下一步我们要做的是把这些底层操作暴露给用户态——通过系统调用和 Shell 命令。那才是用户真正能"看到"的部分。不过在写系统调用之前，有一个 syscall 入口的栈对齐陷阱等着我们踩——那个 bug 杀了我一个下午才找到。我们下篇见。

## 参考资料

- [OSDev Wiki - ext2](https://wiki.osdev.org/Ext2) — ext2 Directory Entry 结构定义，rec_len 规则和 4 字节对齐要求
- [The ext2 filesystem driver](https://wiki.osdev.org/Ext2) — ext2 非官方规范文档，目录项结构的详细说明和操作规则
- [Linux kernel ext2 dir.c](https://github.com/torvalds/linux/blob/master/fs/ext2/dir.c) — Linux ext2 目录操作，ext2_find_entry / ext2_add_link / ext2_delete_entry，约 350 行
- [SerenityOS Ext2FS Inode.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/FileSystem/Ext2FS/Inode.cpp) — SerenityOS 的 add_child / remove_child 实现，整体重写策略，约 600 行
- [Linux kernel ext2 inode.c](https://github.com/torvalds/linux/blob/master/fs/ext2/inode.c) — Linux ext2 的文件读写实现，buffer_head 缓存机制
