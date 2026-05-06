# 028b-1 Tutorial: ext2 写入——从读取到创建文件

> 标签：ext2、块分配器、inode 分配器、bitmap、元数据回写、磁盘文件系统
> 前置：[028_fs_ext2](028-fs-ext2-1.md)

## 前言

上一 tag 我们把 ext2 的读取链路跑通了——superblock 校验、BGDT 加载、inode 解析、目录遍历、文件内容读取，一整套只读流程已经可以在 QEMU 里正确地 `cat /hello.txt`。说实话，能做到这一步已经相当有成就感了，毕竟从一个裸硬盘镜像上读出真实的文件内容，这意味着我们的 VFS + ext2 栈确实能干活。

但一个只能读的文件系统充其量算半个文件系统。你不能创建新文件、不能建目录、不能写数据——用户在 Shell 里输入 `touch /test.txt` 的时候内核只能一脸无辜地返回 -1。这显然不是一个完整的 OS 体验。更关键的是，写入操作才是真正考验文件系统设计的地方：读取只需要从磁盘搬数据到内存，写入则涉及资源分配（块、inode）、元数据一致性（superblock/BGDT/bitmap 三处同步更新）、以及复杂的数据结构操作（目录项的插入和删除）。我们之前在 tag 026 做的 ramdisk 虽然也有"写入"（往内存里的 ustar 归档追加数据），但那不涉及磁盘分配——ramdisk 的数据都在内存里，不存在"管理空闲空间"这个概念。

这一 tag 我们要给 ext2 加上完整的写入能力。三篇文章分别覆盖"分配器与元数据回写"、"目录操作与文件写入"、"系统调用与 Shell 命令集成"。我们现在要做的是先搞定基础设施——块分配器、inode 分配器、元数据回写机制——这些是后续所有写入操作的地基。

在阅读本篇之前，建议你先回顾一下 ext2 的磁盘布局（OSDev Wiki 的 ext2 页面是最好的参考）：superblock 在字节偏移 1024 处，BGDT 紧随其后，每个 group 包含 block bitmap、inode bitmap、inode 表和数据块。理解了这些布局之后，你才能理解分配器为什么要读 bitmap 块、为什么要更新 BGDT 计数器、为什么要回写 superblock。如果你对这些概念还不熟悉，建议先读 [028-fs-ext2-1](028-fs-ext2-1.md) 再回来。

## 环境说明

开发基于 028_fs_ext2 tag 的代码，工具链不变：GCC 14 + CMake + QEMU。硬盘镜像用 `dd` + `mkfs.ext2` 预先格式化，通过 AHCI DMA 进行块读写。一个需要注意的变化是，写入操作需要 AHCI 驱动支持 `write()` 方法——我们在 tag 025 里实现的 AHCI 驱动已经有这个能力了（`ahci_.write()` 通过构造 FIS 命令块写入硬盘），只是此前一直没有被调用过。第一次在 QEMU 里看到写入命令成功落盘的那一刻，说实话还是有点小激动的。

测试用的 ext2 镜像建议用 8MB 大小——`dd if=/dev/zero of=disk.img bs=1M count=8 && mkfs.ext2 disk.img`。`mkfs.ext2` 默认会创建 1K 块大小，这意味着每个 block group 的 block bitmap 管理 8192 个 block（1K bitmap 块 × 8 bit），对应 8MB 空间——刚好一个 group 就能覆盖整个镜像。小镜像的好处是分配操作总是发生在 group 0，调试的时候不用考虑跨 group 的边界情况。

这里值得花一点篇幅讨论一下 ext2 写入和 AHCI 的关系。ext2 驱动本身是纯粹的软件逻辑——bitmap 操作、计数器更新、目录项链表维护——完全不涉及硬件细节。但写入操作最终必须通过 AHCI 把数据刷到硬盘上，而 AHCI 的 `write()` 方法有一个关键前提：DMA 缓冲区必须是物理连续的内存页，并且已经通过 VMM 映射到了内核虚拟地址空间。我们的 DMA 缓冲区在 `ensure_dma_buffer()` 中分配——调用 PMM 获取一个物理页，然后通过 VMM 映射到 `KMEM_EXT2_DMA_BASE`。这个缓冲区只有一个页（4K），也就是说我们同一时刻只能持有一个块的数据。对于 1K 块大小的 ext2 来说，一个 4K 页可以容纳 4 个块——但我们的实现目前一次只操作一个块，没有利用这个优化空间。

另外一个在测试时需要注意的问题：QEMU 的 AHCI 模拟在写入后不会主动刷新缓存。如果你在写入操作后立即关掉 QEMU（而不是正常退出），最后几次写入可能会丢失。这在开发早期不是大问题——你可以通过串口日志确认写入操作确实被执行了——但在做压力测试时需要留意。Linux 的 AHCI 驱动会发送 FLUSH CACHE 命令来确保数据落盘，但 Cinux 的 AHCI 驱动目前没有这个功能。

在实际硬件上测试 ext2 写入时还有一个注意事项：确保硬盘镜像中预留了足够的空闲 inode 和空闲 block。`mkfs.ext2` 默认会预留 5% 的空间给 root 用户，但对于我们的教学内核来说不需要考虑这个——我们用 root 身份运行，可以自由使用所有空间。你可以通过 `dumpe2fs disk.img` 埥看镜像的详细信息，包括每个 group 的空闲 block 数和空闲 inode 数。这个工具在调试分配器问题时非常有用——你可以对比 Cinux 的日志输出和 `dumpe2fs` 的报告来确认分配器是否正确工作。

## 第一步——块分配器：从 bitmap 里挖出空闲块

ext2 的磁盘空间管理完全依赖 bitmap。每个 block group 有一个 block bitmap，这个 bitmap 本身占据一个完整的块（4K 块大小下就是 4K 字节，每 bit 代表一个 block，所以一个 bitmap 块能管理 32768 个 block，即 128MB）。bit 为 0 表示空闲，1 表示已使用。块分配器的任务就是找到第一个为 0 的 bit，把它置 1，然后更新所有相关的计数器。

在进入代码之前，我们先搞清楚一个容易混淆的问题：为什么 ext2 用 bitmap 而不是空闲链表（free list）？空闲链表在很多简单文件系统中被采用（比如 FAT 用 FAT 表本身来跟踪空闲簇，minix fs 用 zone bitmap），但 bitmap 有几个独特优势。第一，bitmap 是固定大小的——一个 group 的 block bitmap 永远是一个块，不管文件系统用了多少空间。第二，bitmap 支持"就近分配"——扫描 bitmap 时可以轻松找到目标位置附近的空闲块，这对磁盘性能至关重要。第三，bitmap 的碎片化信息可以直接从 bit 分布看出来——如果 bitmap 中出现大片的 0，说明有大片连续空闲空间。空闲链表做不到这一点。

我们先来看 `alloc_block()` 的实现思路。函数从 block group 0 开始线性遍历所有 group，对每个 group 先检查 BGDT 中的 `bg_free_blocks_count`——如果已经是 0 就直接跳过，避免无谓的磁盘读取。这是一个非常实用的小优化：BGDT 在 mount 的时候已经全部缓存到内存了，所以这个检查几乎零开销。然后读取该 group 的 block bitmap 块，在 DMA 缓冲区里逐字节扫描。如果一个字节是 0xFF，说明里面 8 个 bit 全被占了，跳过；否则逐 bit 检查，找到第一个 0 bit。

```cpp
for (uint32_t byte_idx = 0; byte_idx < bytes_needed; ++byte_idx) {
    if (bitmap[byte_idx] == 0xFF) {
        continue;  // 这个字节里 8 个 bit 全部被占
    }
    for (uint32_t bit = 0; bit < 8; ++bit) {
        uint32_t local_block = byte_idx * 8 + bit;
        if (local_block >= blocks_in_group) {
            break;  // 超出了这个 group 的范围
        }
        if ((bitmap[byte_idx] & (1U << bit)) == 0) {
            // 找到了一个空闲 bit——标记为已使用
            bitmap[byte_idx] |= static_cast<uint8_t>(1U << bit);
            // ... 后续处理：写回 bitmap、更新计数器
        }
    }
}
```

你会发现这段代码有一个不太明显但很重要的边界处理：最后一个 group 可能不完整。比如一个 8MB 的 ext2 镜像用 4K 块大小，理论上每个 group 管理 32768 个 block，但实际总共只有 2048 个 block。所以循环中需要检查 `local_block >= blocks_in_group`，防止扫描到 bitmap 中不属于本 group 的 bit。这些高位 bit 在 `mkfs.ext2` 格式化时应该被置为 1（标记为已使用），但我们不能依赖这个假设——防御性编程在文件系统代码中是必须的。

还有一个你可能注意到的优化机会：外层循环先检查 `bitmap[byte_idx] == 0xFF` 来跳过满字节。这比逐 bit 检查快 8 倍。更进一步，我们可以检查 4 字节或 8 字节是否为 0xFFFFFFFF/0xFFFFFFFFFFFFFFFF 来一次跳过 32/64 个 bit。Linux 的 `ext2_find_next_zero_bit()` 就使用了这种字长优化。但对于教学内核来说，逐字节扫描已经足够清晰和高效了——毕竟我们的磁盘镜像只有几 MB。

找到了空闲 bit 之后要做三件事：把 bitmap 写回磁盘、更新 superblock 的 `s_free_blocks_count`、更新 BGDT 的 `bg_free_blocks_count`。然后计算全局块号：`global_block = group * blocks_per_group + first_data_block + local_block`。这里 `first_data_block` 对于 1K 块大小是 1（superblock 占用了 block 0 和 block 1 的前两个扇区），对于更大的块大小是 0（superblock 嵌在 block 0 的偏移 1024 处）。这个值直接来自 superblock 的 `s_first_data_block` 字段，Cinux 在 `mount()` 时就缓存了它。

`free_block()` 是 `alloc_block()` 的逆操作——计算 block 属于哪个 group，清除 bitmap 中的对应 bit，递增两个计数器，写回 bitmap/superblock/BGDT。有一个很容易忽略的细节：inode 号从 1 开始（inode 0 是保留的），但 block 号从 0 开始。所以在 `free_block` 中计算 group 时不需要减 1，而 `free_inode` 中需要 `(ino - 1)` 来做除法。这种 base 不一致的情况在 ext2 规范中比比皆是，写代码时一定要仔细对照 spec。

`free_block()` 的 group 计算公式是 `group = (block_num - first_data_block_) / blocks_per_group_`。这里减的是 `first_data_block_` 而不是 1——因为 block 0 到 `first_data_block_ - 1` 这些块是 superblock 和 BGDT 占用的（对于 1K 块大小，`first_data_block_ = 1`，所以 block 0 是 superblock 所在块）。这个细节如果搞错了，在磁盘比较满的时候（多个 group）就会导致释放操作更新错误的 group 的 bitmap 和计数器，后果非常严重——轻则 `s_free_blocks_count` 不准确，重则两个 group 的 bitmap 同时损坏。

另一个在 `free_block` 和 `free_inode` 中都会遇到的问题是"double free"——释放一个已经空闲的 block/inode。Cinux 目前不做 double free 检测——如果调用方不小心连续两次 `free_block(42)`，第二次会清除 bitmap 中已经是 0 的 bit（无效果），但会递增 `s_free_blocks_count`，导致计数器比实际空闲块数多。Linux 的 `ext2_free_blocks()` 会在释放前检查 bitmap 位是否确实是 1，如果不是就报 warning。这种防御性检查在调试阶段非常有价值。

你会发现在 Cinux 的实现中，每次分配或释放一个 block 都要写三次磁盘（bitmap + superblock + BGDT）。这当然不是什么高效的做法，但对于教学内核来说有一个巨大的好处：数据一致性。每次操作完成后磁盘上的状态一定是完整的，随时断电再挂载不会出现 bitmap 说 block 空闲但 superblock 计数器说已经用完这种矛盾。Linux 的 ext2 通过 delayed write（脏数据延迟回写）大幅减少了磁盘 I/O，但代价是崩溃后可能出现元数据不一致——这正是 ext3/4 引入 journal 的根本原因。

### 与 Linux 和 SerenityOS 的对比

Cinux 的线性扫描策略是最朴素的 first-fit——从 group 0 开始，找到第一个空闲块就返回。这种策略的问题很明显：所有文件的 block 都会集中在磁盘前半部分，造成空间局部性不佳。Linux 的 ext2 驱动（`fs/ext2/balloc.c`，约 580 行）采用的是完全不同的策略：它使用 "goal-based allocation" + "reservation window" 机制。所谓 goal 就是"期望分配位置"——通常是文件的当前最后一个 block 的下一个 block。如果 goal 附近 64 个 block 范围内找不到空闲块，才退回到逐 bit 搜索。reservation window 是一个基于红黑树的数据结构，每个正在写入的 inode 都预留了一片连续的 block 区域，防止并发分配时互相争抢同一片空间。这些优化在 Linux 的 ext2 驱动中由 per-group spinlock 保护，确保 SMP 环境下的原子性。你可以看到，Linux 的分配策略在"尽量连续"这个目标上做了大量工作——连续的 block 意味着顺序读写，而 HDD 上的顺序读写比随机读写快几个数量级。

SerenityOS 的策略（`Kernel/FileSystem/Ext2FS/Inode.cpp`）和 Cinux 比较接近——简单的 bitmap 扫描，但有一个额外步骤：`allocate_and_zero_block()` 在分配新块后会立即用零填充整个块。这避免了新分配的块中残留旧数据的安全问题。Cinux 实际上也在 `get_or_alloc_block()` 中做了清零操作，只不过是在调用点而不是分配器内部完成的。权衡下来，三种策略反映了同一个设计光谱上的三个点：Cinux 追求最简实现，SerenityOS 加了安全清零，Linux 做了全套生产级优化。

## 第二步——inode 分配器：和块分配器几乎对称

inode 分配器和块分配器的逻辑几乎完全对称——唯一的本质区别是 inode 号从 1 开始而不是 0。这意味着计算全局 inode 号时有一个 `+1`：`global_ino = group * inodes_per_group + local_index + 1`。在 `free_inode` 中计算 group 时则需要先减 1：`group = (ino - 1) / inodes_per_group`。

这个 1-based 编号是 ext2 规范规定的——inode 0 不存在（有些实现用它做 internal marker），inode 1 是 defect block list（已弃用），inode 2 是根目录，inode 3-10 是保留 inode（ACL 块等）。所以第一个可分配的 inode 号至少是 11（如果 group 0 的前 10 个 inode 都被保留了的话）。实际上 `mkfs.ext2` 默认会预留这些 inode，但具体分配多少取决于参数。Cinux 的 `alloc_inode()` 不做任何过滤——它只是扫描 bitmap 找第一个 0 bit，而 `mkfs.ext2` 已经把保留 inode 对应的 bit 置为 1 了，所以不会分配到这些特殊 inode。

inode 分配器的完整流程和块分配器如出一辙：遍历 group → 检查 `bg_free_inodes_count` → 读取 inode bitmap → 扫描空闲 bit → 置位 → 写回 bitmap → 更新 superblock 和 BGDT 计数器 → 返回 inode 号。整个实现从代码结构上几乎可以和 `alloc_block()` 做一个 diff——改几个变量名就是另一个分配器。Linux 的做法不同——`ext2_new_inode()` 使用 Orlov 分配器来决定新目录应该放在哪个 group（目标是分散目录，避免热点），对普通文件则使用 quadratic hash 搜索。还有一个 "debt" 机制来平衡各 group 之间的目录密度。Orlov 分配器的核心思想是：目录和它的子目录应该分散到不同的 group（减少磁头移动），但一个目录和它包含的文件应该在同一个 group（保持局部性）。这些优化在大规模文件系统上有显著效果，但实现复杂度也高了一个数量级。

值得一提的是 `bg_used_dirs_count` 的维护——这个计数器在 `mkdir()` 时递增，在 `unlink()` 删除目录时递减。它的作用是告诉分配器"这个 group 里有多少个目录"，Orlov 分配器用这个信息来决定新目录该放哪里。Cinux 虽然不用 Orlov，但维护这个计数器仍然是必要的——它保证了 BGDT 和磁盘状态的一致性，也为将来可能的优化留了接口。注意 `bg_used_dirs_count` 的递减不在 `free_inode()` 中完成——它在 `unlink()` 的链接归零分支中处理，因为只有 `unlink` 知道被删除的 inode 是不是一个目录。

## 第三步——元数据回写：一次分配操作背后的三次磁盘写入

我们前面提到每次分配操作都需要同步写回三种元数据。现在来拆解一下这三个函数分别做了什么，以及为什么要这样做。

`write_superblock()` 最直接——把内存中的 `sb_` 结构体拷贝到 DMA 缓冲区，然后通过 AHCI 写入磁盘 LBA 2（字节偏移 1024）开始的 2 个扇区。superblock 的写入不经过 `read_block/write_block` 的块读写接口，而是直接操作扇区。这里有一个容易混淆的点：superblock 在磁盘上的位置是字节偏移 1024，不是"块 0"。对于 1K 块大小，superblock 恰好是 block 1 的全部内容（1K 偏移 1024，刚好一个块）；但对于 4K 块大小，superblock 是 block 0 的前 1024 字节——block 0 从字节偏移 0 开始，前 1024 字节是引导扇区保留区，偏移 1024 处是 superblock。如果我们用 `write_block(1)` 来写 superblock，在 4K 块大小下会把 block 0 的全部 4K 数据覆写，覆盖掉引导保留区。直接用 LBA 操作可以精确控制写入范围，避免这个问题。

`write_bgdt()` 稍微复杂一些。BGDT 由多个块组成（每个 block group 的描述符占 32 字节，一个 4K 块能放 128 个描述符）。写入单个 group 的描述符需要先读取包含该描述符的整个 BGDT 块（read-modify-write 模式），然后用 `memcpy` 把修改后的描述符覆盖到正确位置，最后写回磁盘。这里 read-modify-write 是必须的——我们不能只写一个 32 字节的描述符，因为 AHCI 的最小写入单位是一个扇区（512 字节）。

`write_disk_inode()` 同样是 read-modify-write 模式。inode 表是一个连续的块数组，每个 inode 占 `inode_size_` 字节（通常是 128 或 256）。给定 inode 号，先算出它属于哪个 group（`(ino - 1) / inodes_per_group`）、在 group 内的索引（`(ino - 1) % inodes_per_group`）、落在哪个表块中（`byte_offset / block_size`）、块内偏移多少（`byte_offset % block_size`）。然后读那个块，在对应偏移处覆写 128 字节，写回磁盘。这里有一个安全检查——如果 `within_block_offset + sizeof(Ext2Inode) > block_size`，说明这个 inode 跨越了块边界。理论上这不应该发生（`mkfs.ext2` 不会让 inode 跨块），但防御性地检查一下总没坏处。

这三个函数组合起来保证了 ext2 的元数据一致性。这里还有一个小细节值得一提：`write_block()` 是 `read_block()` 的镜像——它把 DMA 缓冲区中的数据通过 AHCI 写入磁盘。调用方需要先把要写入的数据填入 DMA 缓冲区（通过 `dma_buf_virt()` 获取虚拟地址），然后调用 `write_block(block_num)`。这意味着写入路径上的数据流是：调用方构造数据 → 拷贝到 DMA 缓冲区 → AHCI DMA 传输到磁盘。没有任何中间缓存——数据直接从 CPU 通过 DMA 控制器流向磁盘。这种"零缓存"的设计在性能上当然不如有缓存的方案，但在正确性上有一个巨大优势：调用方在任何时刻都精确知道磁盘上的内容是什么——如果 `write_block()` 返回了 true，数据就一定已经落盘了。。分配一个 block 的完整磁盘 I/O 序列是：读 bitmap → 写 bitmap → 写 superblock → 读 BGDT 块 → 写 BGDT 块。5 次磁盘操作分配一个 block——这就是简单设计的代价。Linux 的 ext2 使用 buffer cache 把这些写入缓冲起来，攒一批再统一刷盘（bdflush 内核线程），但那需要先实现一个完整的 buffer cache 子系统，对于教学内核来说太重了。而且同步写入有一个不可替代的好处：每次操作完成后文件系统处于确定性的状态，调试的时候你可以随时停下来检查磁盘内容，不用担心"数据还在内存里没刷盘"这种令人困惑的情况。

## 第四步——get_or_alloc_block：惰性块分配的艺术

有了分配器之后，我们还需要一个关键的胶水函数 `get_or_alloc_block()`。它的职责是：给定一个 inode 和一个逻辑块号（文件内的第 N 个块），返回对应的物理块号。如果这个逻辑块还没有分配物理块，就自动分配一个。这就是所谓的"惰性分配"（lazy allocation）——不是在创建文件时就分配所有可能用到的块，而是在真正写入数据的时候才分配。

惰性分配有几个重要的好处。首先，创建文件的开销极低——只需要分配 inode 和插入目录项，不需要分配任何数据块。`touch /hello.txt` 创建一个空文件几乎零磁盘 I/O（除了 inode 本身）。其次，磁盘空间利用率更高——如果一个程序创建了文件但从未写入数据，那个文件就不占用任何数据块。Linux 甚至把惰性分配推到了极致——它的 `fallocate()` 系统调用可以"承诺"分配空间但不实际分配块，以及 `O_TMPFILE` 可以创建一个完全没有目录项的匿名 inode。这些都是建立在"按需分配"这一核心理念之上的。

先别急，在深入代码之前我们先来理解 ext2 的块寻址方案。ext2 的 inode 有 15 个块指针字段 `i_block[0..14]`。前 12 个（`i_block[0..11]`）是直接块指针，直接指向数据块——这意味着文件的前 12 个块（对于 4K 块就是前 48KB）可以通过一次间接寻址就找到。第 13 个（`i_block[12]`）是单间接块指针，指向一个包含 1024 个（4K 块 / 4 字节）块指针的间接块，覆盖块 12 到块 1035（约 4MB）。第 14 个和第 15 个分别是双间接和三间接块指针，覆盖范围指数级增长。Cinux 目前只实现了直接块和单间接块。

```cpp
uint32_t Ext2::get_or_alloc_block(Ext2Inode& disk, uint32_t file_block) {
    if (file_block < EXT2_DIRECT_BLOCKS) {
        // 直接块：检查是否已分配
        if (disk.i_block[file_block] == 0) {
            uint32_t blk = alloc_block();
            if (blk == 0) return 0;
            // 清零新块——防止残留旧数据
            auto* dma = reinterpret_cast<uint8_t*>(dma_buf_virt_);
            for (uint32_t i = 0; i < block_size_; ++i)
                dma[i] = 0;
            if (!write_block(blk)) {
                free_block(blk);  // 分配成功但写入失败，回滚
                return 0;
            }
            disk.i_block[file_block] = blk;
        }
        return disk.i_block[file_block];
    }
    // 单间接块处理（下文详述）
}
```

直接块的处理很直观：如果 `i_block[file_block]` 是 0 说明还没分配，调 `alloc_block()` 分配一个新块，清零后写盘，把物理块号存入 `i_block`。这里有一个细节值得注意——分配新块后立即清零。这不是 ext2 规范要求的，但是一个好的安全实践。新分配的块可能在之前被另一个文件使用过，如果不清零，新文件可能通过读取未初始化的部分看到旧文件的数据。这就是 SerenityOS 在 `allocate_and_zero_block()` 中做的事情。

单间接块的处理需要两步：首先确保间接块本身已经分配（如果 `i_block[12] == 0` 就分配一个新块作为间接块），然后在间接块中查找对应位置的数据块指针。这里有一个微妙之处——分配间接块和分配数据块是两个独立的 `alloc_block()` 调用，中间会改变 DMA 缓冲区的内容。所以代码在分配完数据块后需要重新读取间接块、写入数据块指针、再写回间接块。这是因为 Cinux 只有一个 DMA 缓冲区，同一时刻只能持有一个块的数据。

让我把间接块分配的完整步骤展开来说，因为这个过程中 DMA 缓冲区的"换手"是最容易出 bug 的地方。假设我们要写入文件的逻辑块 15（超过 12 个直接块的范围）：

第一步，检查 `i_block[12]` 是否为 0。如果是 0，分配一个新块作为间接块——`alloc_block()` 返回块号 N。把 DMA 缓冲区清零（这个块将成为 1024 个 32 位指针的数组，初始全为 0），然后 `write_block(N)` 写回磁盘。把 N 存入 `disk.i_block[12]`。

第二步，`read_block(N)` 重新读取刚写入的间接块。现在 DMA 缓冲区里是间接块的内容。检查 `indirect[3]`（因为逻辑块 15 对应间接块的索引 15-12=3）是否为 0。如果是 0，分配一个数据块——`alloc_block()` 返回块号 M。但注意：`alloc_block()` 会使用 DMA 缓冲区读写 bitmap，所以间接块的数据已经不在 DMA 缓冲区里了！

第三步，再次 `read_block(N)` 重新读取间接块。把 `indirect[3]` 设为 M。`write_block(N)` 写回间接块。现在间接块中的索引 3 指向了数据块 M。

第四步，`read_block(M)` 读取数据块（如果是部分写入的话），填入数据，`write_block(M)` 写回。

你看，分配一个间接块引用的数据块需要四次 DMA 缓冲区切换——间接块写入、bitmap 读写、间接块再次读写、数据块读写。如果 Cinux 有一个缓冲池（比如 4 个 DMA 缓冲区），就可以把间接块和 bitmap 同时保持在内存中，省掉两次磁盘读取。但这是将来的优化了。

这个"单 DMA 缓冲区"的设计在教学内核中是完全可以接受的，但它也决定了我们的写入路径不能有并发——任何时刻只有一个操作在使用 DMA 缓冲区。Linux 的 ext2 驱动使用 buffer_head 缓存系统（每个 buffer_head 对应一个块），可以同时持有多块的脏数据在内存中。SerenityOS 使用 `BlockBasedFS` 的块缓存，效果类似。而 Cinux 的方案更接近 xv6——xv6 也有一个全局的磁盘缓冲区（buf 结构体数组），不过 xv6 的 buf 是一个 LRU 缓存池，比我们的单块 DMA 缓冲区要高级一些。xv6 的 buf 数组有 NBUF（通常 30）个槽位，通过一个链表管理 LRU 淘汰。如果要给 Cinux 做性能优化，一个简单的改进就是把单块 DMA 缓冲区扩展为一个小的缓冲池——比如 8 个块——就能大幅减少写入路径上的磁盘读取次数。这个改进的工程量不大（本质上就是把 `dma_buf_virt_` 从一个 uint64_t 变成一个数组），但收益显著。

## 一个有趣的数字游戏：一次 ext2 文件创建到底要写多少次磁盘？

在继续之前，让我们来做一个有趣的计算。这个计算不仅能帮你理解每个函数的作用，还能让你对文件系统性能有一个直觉性的认识。假设我们在一个已经有一些文件的 ext2 文件系统中创建一个新的空文件 `/hello.txt`，整个过程中到底发生了多少次磁盘写入？

首先是 `alloc_inode()` 的开销：读 inode bitmap（1 次读）→ 置位 → 写回 inode bitmap（1 次写）→ 写 superblock（1 次写）→ 读 BGDT 块（1 次读）→ 写回 BGDT 块（1 次写）。然后是 `write_disk_inode()`：读 inode 表块（1 次读）→ 覆写 → 写回（1 次写）。接着是 `add_dir_entry()`：读父目录的数据块（1 次读）→ 修改 → 写回（1 次写），如果需要分配新的目录数据块还要加上 `alloc_block()` 的 5 次 I/O。最后是 `write_disk_inode()` 写回父目录 inode（读+写各 1 次）。

如果父目录有足够空间（不需要分配新块），总磁盘 I/O 次数是：5 次读 + 6 次写 = 11 次（alloc_inode 的 2R+3W + write_disk_inode 新 inode 的 1R+1W + add_dir_entry 的 1R+1W + write_disk_inode 父目录的 1R+1W）。如果需要分配新目录块，则加上 alloc_block 的 2R+3W 和 add_dir_entry 中 write_disk_inode 的 1R+1W，总计 8 次读 + 10 次写 = 18 次。创建一个空文件就要 11 到 18 次磁盘操作——对于一块现代 SSD 来说这大概需要 1-2 毫秒，但对于 HDD 来说可能需要 50-100 毫秒（每次随机读写约 5-7ms）。

Linux 的 ext2 通过 buffer cache 把这些写入缓冲起来——bitmap、superblock、BGDT 的修改都先在内存中完成，定期（或者手动 sync 时）才真正刷盘。这样一次文件创建的"逻辑"操作次数不变，但"物理"磁盘 I/O 大幅减少。Linux 的 `sync` 命令就是用来手动触发刷盘的——你可以试试在 Linux 上创建文件后不 sync 直接拔电源，重启后文件很可能就丢了。

### 与 xv6 的块分配对比

说到教学内核的块分配，不能不提 xv6。xv6 的文件系统（`fs.c`）使用了一种比 bitmap 更简单的分配方式——它维护一个全局的空闲块号，从 superblock 的 `s_freeblock` 字段开始，每次分配后指向下一个空闲块。这本质上是一个单链表——每个空闲块的前几个字节存储下一个空闲块的号。分配时从链表头取出一个块，释放时把块插回链表头。

xv6 的方式极其简单——不需要 bitmap、不需要扫描、O(1) 分配和释放。但它有一个致命缺陷：无法做"就近分配"——空闲链表的分配顺序完全取决于释放顺序，文件的 block 很可能散布在磁盘各处。对于 HDD 来说这意味着大量的随机寻道，性能极差。而且空闲链表无法快速判断磁盘是否满了——你得遍历整个链表才能知道还有多少空闲块。ext2 的 bitmap 方案在这些方面都要优越得多——你可以瞬间看到哪些区域有连续空闲空间，也可以通过 `bg_free_blocks_count` 立刻知道某个 group 还有多少空闲块。

## 关于崩溃一致性的思考

前面我们提到 Cinux 的同步写入策略有一个好处是"数据一致性"。但这其实只是"操作完成后的数据一致性"，不是"崩溃一致性"。如果系统在分配 bitmap bit 之后、更新 superblock 计数器之前崩溃了，磁盘上就会出现不一致：bitmap 说那个 block 已分配，但 superblock 的空闲计数器没更新。下次挂载的时候，`s_free_blocks_count` 比实际空闲块数多 1。这种不一致不会导致数据损坏（最坏情况是 fsck 报一个警告然后修正），但它揭示了同步写入并不能完全解决崩溃一致性问题。

Linux 的 ext2 使用一种叫做 "write-ahead logging" 的策略来缓解这个问题——但实际上 ext2 根本没有日志，它的崩溃一致性完全依赖 fsck 在挂载前修复不一致。这也是 ext3/4 引入 journal 的核心动机。ext3 的 journal 模式会在真正修改元数据之前，先把计划的修改记录到一个环形日志区域中。如果修改过程中崩溃了，重启后 journal 回放可以原子地完成或回滚未完成的操作。Cinux 离 journal 还有很远的距离，但理解"同步写入 != 崩溃一致性"这个概念对于文件系统开发者来说非常重要。

## 收尾

到这里，ext2 写入操作的基础设施就全部就位了。块分配器负责从 bitmap 中找到空闲空间，inode 分配器负责找到空闲的文件控制块，元数据回写函数确保每次操作后磁盘状态一致，`get_or_alloc_block` 提供了惰性块分配的语义。这些函数本身并不复杂——总共也就几百行代码——但它们组合在一起构成了一个完整且自洽的磁盘空间管理系统。如果你在 QEMU 里跑一遍 `alloc_block` 的日志，会看到每次分配都伴随着精确的 bitmap 更新和计数器变化，这种确定性在调试的时候非常让人安心。

先别急，基础设施只是地基。下一篇文章我们会在这些分配器之上搭建真正的文件操作：创建文件、创建目录、删除文件，以及 ext2 目录项链表的精巧操作。那才是 ext2 写入的核心看点——目录项的 `rec_len` 切分和合并是我写 ext2 以来觉得最有趣的部分。

如果你想在动手写下一篇代码之前先消化一下本篇的内容，我建议做一个小实验：在 QEMU 里挂载一个 ext2 镜像，然后手动调用 `alloc_block()` 几次（可以在 Shell 命令处理函数里加一个测试命令），观察串口日志中 bitmap 更新和计数器变化的输出。这种"逐步观察分配器行为"的方式比单纯读代码更能加深理解——你会直观地看到"分配一个 block 要写三次磁盘"是什么意思。也可以试试连续调用 `alloc_block()` 十次然后 `free_block()` 十次，检查 `s_free_blocks_count` 是否回到了原值——如果没回，说明有 bug。这种"分配N次、释放N次、检查回原点"的测试模式在文件系统开发中非常实用。

## 参考资料

- [OSDev Wiki - ext2](https://wiki.osdev.org/Ext2) — ext2 数据结构参考，Block Bitmap、Inode Bitmap 字段定义，Superblock 和 BGDT 各字段的偏移和含义
- [Linux kernel ext2 balloc.c](https://github.com/torvalds/linux/blob/master/fs/ext2/balloc.c) — Linux ext2 块分配器实现，goal-based allocation + reservation window，约 580 行
- [Linux kernel ext2 ialloc.c](https://github.com/torvalds/linux/blob/master/fs/ext2/ialloc.c) — Linux ext2 inode 分配器，Orlov allocator 实现，约 450 行
- [SerenityOS Ext2FS Inode.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/FileSystem/Ext2FS/Inode.cpp) — SerenityOS 的 ext2 实现，简单 bitmap 扫描 + allocate_and_zero_block，约 600 行
- [The ext2 filesystem](https://wiki.osdev.org/Ext2) — ext2 非官方规范文档，块分配和 inode 分配的完整描述
- Intel SDM Vol. 2, Chapter 3 — `syscall`/`sysret` 指令行为（与后续 tag 028b-3 的 syscall 写入路径相关）
- [xv6 fs.c](https://github.com/mit-pdos/xv6-public/blob/master/fs.c) — xv6 的 balloc/bfree 实现，空闲链表式块分配
- [ext2 block allocation](https://wiki.osdev.org/Ext2#Block_Bitmap) — ext2 块 bitmap 的详细说明
