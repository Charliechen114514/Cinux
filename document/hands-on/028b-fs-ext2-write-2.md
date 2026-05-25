---
title: 028b-fs-ext2-write-2 · Ext2 写入
---

# 028b-2 Hands-on: ext2 文件写入与目录操作

## 导语

上一章我们把 ext2 的资源分配器搭好了——alloc_block 能从位图里找到空闲块，alloc_inode 能找到空闲 inode，free_block 和 free_inode 能把资源还回去，四种元数据回写函数也各就各位。这些是地基，但光有地基盖不了房子。我们还需要在上面构建真正的文件操作——创建文件、写入数据、创建目录、删除文件和目录。这些操作的复杂度比分配器高一个量级，因为它们不再是单一资源的获取和释放，而是涉及多个资源的协调分配、目录项链表的操作、父子关系的维护。

举个例子，创建一个新文件需要：分配一个 inode、初始化它的元数据字段、在父目录的数据块中找到一个能塞下新目录项的空洞、插入新的目录项、写回父目录的 inode。如果中间任何一步失败，前面已经分配的资源必须全部回滚——否则就会出现"位图说已占用但 inode 没有被使用"的泄漏。创建目录就更复杂了——除了创建文件的全部步骤之外，还要分配一个数据块、写入 "." 和 ".." 两个特殊目录项、更新父目录的 i_links_count（因为新目录的 ".." 指向父目录）。

这一章我们把 create、mkdir、unlink 三个核心操作以及它们依赖的辅助函数——get_or_alloc_block、add_dir_entry、remove_dir_entry——全部实现出来。完成之后，Cinux 就具备了完整的文件系统写入能力。

知识前置：028b-1（分配器与元数据回写）、028-2/028-3（ext2 只读驱动中的块指针解析和目录项遍历）。

## 概念精讲

### get_or_alloc_block —— 按需分配块指针

文件写入时，我们需要把逻辑块号（文件内第几个块）转换为物理块号（磁盘上第几个块）。对于已经分配过的块，直接从 inode 的 i_block 数组里读就行了。但如果文件在增长，这个逻辑块号可能还没有对应的物理块——我们需要现场分配一个。

get_or_alloc_block 封装了这个"查询 or 分配"的逻辑。给定一个 inode 和逻辑块号，它首先检查 i_block 数组中对应位置是否为 0。如果是 0，调用 alloc_block 分配一个新块，把新块号填入 i_block，然后返回这个块号。如果非零，直接返回已有的块号。

逻辑块号 0-11 对应 12 个直接块指针，逻辑块号 12 开始走单间接块。对于间接块的情况，处理稍微复杂一些——首先确保 i_block[12]（间接块指针）本身已经分配，如果没分配就先分配一个块作为"指针数组块"，然后在这个指针数组中找到或分配目标数据块。间接块本身也是一个数据块，只不过里面存的全是 32 位块指针而不是文件数据。对于 1024 字节块大小，一个间接块能存 256 个指针，意味着文件通过间接块可以额外寻址 256KB。

有一个非常容易踩坑的地方：分配新块后必须把新块清零再写入磁盘。因为新分配的块可能包含上次使用后的残留数据——如果我们不清零，文件读取到未写入的部分时会看到垃圾数据。更严重的是，间接块如果不清零，那些非零的"残留指针"会被当作有效的块指针，后续操作可能会写入到错误的位置。

### 文件写入的 read-modify-write 策略

文件写入不是简单地往磁盘上写数据那么简单。ext2 的写入单位是整个块，但用户可能只想写入几个字节——比如在文件末尾追加一行文字。这时候我们面临一个问题：目标块可能已经有部分数据了，我们只想修改其中几个字节。

解决方案是 read-modify-write：先 read_block 把目标块的当前内容读到 DMA buffer，然后在 buffer 中修改需要写入的部分，最后 write_block 把整个 buffer 写回磁盘。如果写入恰好覆盖整个块（offset 是块大小的整数倍且长度正好是一个块），可以跳过 read 步骤，直接在 buffer 中构造新数据然后写入。

我们还需要更新 inode 的 i_size——如果写入导致文件变大了，i_size 要更新为 offset + written 的新值，同时 i_blocks 也要更新为文件实际占用的 512 字节扇区数。

### 目录项的链表操作

ext2 的目录项是一个变长链表，通过 rec_len 串联。每个目录项占用的空间是 8 字节头部加上变长文件名，总长度向上取整到 4 的倍数。但 rec_len 可能大于实际需要的空间——这是为了在删除条目后不用移动后续数据。

add_dir_entry 的核心思路是"分割"：遍历目录的最后一个数据块中的目录项链表，对每个条目计算它的实际最小空间 `min_rec_len = align_up(8 + name_len, 4)`，如果该条目的 rec_len 大于 min_rec_len，说明后面有"空闲空间"。如果空闲空间足够放下新条目（extra >= required_rec_len），就把它一分为二——把原条目的 rec_len 缩减为 min_rec_len，然后在腾出来的空间构造新条目，新条目的 rec_len 设为剩余的全部空间。如果现有块都放不下，就分配一个全新的数据块，在新块中只放一个条目，rec_len 设为整个块的大小。

remove_dir_entry 的核心思路是"合并"：找到目标条目和它前面的条目，把目标条目的 rec_len 加到前一条目的 rec_len 上——这样目标条目的空间就被前一条目"吸收"了。如果目标恰好是块中的第一个条目（没有前一条目），就只能把它的 inode 字段设为 0 来标记为空——不能合并，因为没有前面的条目可以吸收。

这种增量修改策略和 Linux 的做法类似。SerenityOS 用的是更暴力的方式——每次 add/remove 都重写整个目录的全部内容（read-all + filter + write-all）。暴力方式的优点是代码更简单，缺点是对于大目录性能很差。Cinux 选择增量修改，虽然复杂度高一些但更接近生产级实现。

### create / mkdir / unlink 的资源管理

create 的流程我们在导语里已经说过了——分配 inode、初始化、加目录项、写回父目录。有一个重要的前置检查：创建前先 lookup_in_dir 看看名字是否已存在。如果已存在，直接返回失败，不做任何修改。这是"先检查后操作"的模式——必须保证检查和操作之间没有竞态（对于我们的单线程内核来说这不是问题，但多线程内核需要锁来保证原子性）。

mkdir 比 create 多了几步。首先 inode 的 i_mode 要设为 S_IFDIR（目录类型），i_size 初始化为一个块大小（因为目录至少有一个数据块），i_links_count 初始化为 2——1 来自父目录中的目录项，1 来自自身的 "." 条目。然后需要分配一个数据块，写入 "." （指向自身 inode）和 ".." （指向父目录 inode）两个目录项。最后还要递增父目录的 i_links_count（因为新目录的 ".." 指向了父目录），以及 BGDT 的 bg_used_dirs_count。

unlink 需要处理 link count 的概念。一个 inode 可以有多个硬链接（多个目录项指向同一个 inode）。unlink 只是从父目录中移除一个目录项，然后递减 inode 的 i_links_count。只有当 i_links_count 降为 0 时，才真正释放 inode 的所有数据块和 inode 本身。如果是目录被删除，还需要递减 bg_used_dirs_count 和父目录的 i_links_count。

## 动手实现

### Step 1: 实现 get_or_alloc_block

**目标**: 给定 inode 和逻辑块号，返回物理块号；如果该逻辑块尚未分配，自动分配一个新块。

**设计思路**: 直接块（file_block < 12）的情况最简单——检查 i_block[file_block] 是否为 0，如果是就 alloc_block 并清零新块，然后填入 i_block[file_block] 并返回。间接块（file_block >= 12）需要两步：首先确保 i_block[12]（间接块指针）已分配，如果未分配就先分配一个块并清零作为间接块；然后读间接块，检查其中第 indirect_idx 个指针是否为 0，如果是就分配一个新数据块并清零，然后把新块号写入间接块的对应位置。

**踩坑预警**: 间接块的分配中有一个特别容易出错的顺序问题——分配新数据块之后，DMA buffer 里存的是新数据块的内容（被我们清零了），但此时我们需要把新块号写入间接块。也就是说我们需要先写新数据块，然后重新读间接块，写入新块号，再写回间接块。中间穿插了 read_block 和 write_block 的切换，必须非常小心地管理 DMA buffer 的内容——每次 read_block 都会覆盖之前的 buffer 数据。

**验证**: 编译通过。

### Step 2: 实现 Ext2FileOps::write

**目标**: 文件级写入——给定 VFS inode、偏移量、数据和长度，把数据写入文件的正确位置。

**设计思路**: 写入是一个循环，每次迭代处理一个块范围内的数据。对于每次迭代，先用 get_or_alloc_block 获取或分配目标物理块；如果是部分块写入（不是从头到尾覆盖整个块），先 read_block 读出当前块内容（read-modify-write）；然后把用户数据拷贝到 DMA buffer 的正确位置；最后 write_block 写回磁盘。循环结束后更新 inode 的 i_size 和 i_blocks，并调用 write_disk_inode 写回。

**验证**: 编译通过。

### Step 3: 实现 add_dir_entry

**目标**: 在父目录中插入一个新的目录项。

**设计思路**: 遍历目录的所有数据块，在每个块中扫描目录项链表。对每个已有条目，计算它的实际最小空间（align_up(8 + name_len, 4)），如果 rec_len 减去最小空间后剩余的空间足够放下新条目，就执行分割——缩减原条目的 rec_len，在其后构造新条目。如果所有现有块都放不下，分配一个新数据块，在新块中放一个占据整个块的目录项。

**实现约束**: 目录项的 name_len 字段是实际文件名长度，不做对齐。rec_len 是对齐后的总长度，必须是 4 的倍数。file_type 字段使用 Ext2FileType 枚举值（Regular = 1, Directory = 2）。

**验证**: 编译通过。

### Step 4: 实现 remove_dir_entry

**目标**: 从父目录中移除指定名字的目录项。

**设计思路**: 遍历目录的数据块，找到名字匹配的条目。如果目标条目是块中的第一个条目，把 inode 设为 0（标记为空，保留 rec_len）。否则，把目标条目的 rec_len 合并到前一条目的 rec_len 中——前一条目"吸收"目标条目的空间。最后 write_block 写回修改后的块。

**验证**: 编译通过。

### Step 5: 实现 create

**目标**: 创建一个新普通文件——分配 inode、初始化、加目录项。

**设计思路**: 先检查名字是否已存在（lookup_in_dir）。然后读父目录的 inode。调用 alloc_inode 分配新 inode 号。初始化新 inode：i_mode = S_IFREG | 0644，i_size = 0，i_links_count = 1，块指针全零。write_disk_inode 写回新 inode。add_dir_entry 在父目录中添加条目。write_disk_inode 写回父目录 inode（因为 add_dir_entry 可能修改了父目录的 i_size 或 i_block）。最后 get_cached_inode 把新 inode 放入缓存并返回。

**实现约束**: 如果中间任何一步失败，已经分配的资源必须释放——比如 add_dir_entry 失败后要调用 free_inode 把刚分配的 inode 还回去。

**验证**: 编译并在 QEMU 中测试：
```bash
cd build && cmake .. && make big_kernel_common
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin \
  -drive file=../ext2.img,format=raw,if=none,id=ahci0 \
  -device ahci,id=ahci_ctlr
# 在 shell 中测试（需要后续章节的 touch 命令支持）
# touch /testfile.txt
# ls /
# cat /testfile.txt
```

### Step 6: 实现 mkdir

**目标**: 创建一个新子目录——分配 inode + 数据块、写入 "."/".."、加父目录项。

**设计思路**: 先检查名字是否已存在。读父目录 inode。分配新 inode 和一个数据块。初始化新 inode 为目录类型，i_size = block_size，i_links_count = 2，i_block[0] = data_blk。在数据块中写入 "." 和 ".." 两个目录项——"." 的 inode 指向自身，rec_len = 12；".." 的 inode 指向父目录，rec_len 占据块剩余的全部空间。write_disk_inode 写回新 inode。add_dir_entry 在父目录中添加条目。递增父目录的 i_links_count 和 BGDT 的 bg_used_dirs_count。最后写回父目录 inode 和 BGDT。

**踩坑预警**: "." 条目的 rec_len 必须正确计算为 `align_up(8 + 1, 4) = 12`，".." 条目的 rec_len 为 `block_size - 12`。如果你把 "." 的 rec_len 算错了（比如设成了 8 + 1 = 9），那 ".." 条目的起始位置就不对了，readdir 会从错误的位置读目录项，轻则乱码重则 kernel panic。

**验证**: 编译并在 QEMU 中测试：
```bash
# 在 shell 中（需要后续章节的命令支持）
# mkdir /testdir
# ls /
# ls /testdir
```

### Step 7: 实现 unlink

**目标**: 从父目录移除条目，如果 i_links_count 降为 0 则释放所有资源。

**设计思路**: 读父目录 inode，调用 remove_dir_entry 移除目标条目。读目标 inode，递减 i_links_count。如果降为 0，遍历所有直接块指针（0-11）和间接块指针（12），对每个非零块调用 free_block 释放。然后把目标 inode 清零并设置 i_dtime，write_disk_inode 写回，free_inode 释放 inode 号。如果是目录，递减 bg_used_dirs_count 和父目录的 i_links_count。最后使缓存中的目标 inode 条目失效（设 in_use = false）。

**实现约束**: 释放间接块时，需要先读出间接块的内容，遍历其中的所有指针逐个释放数据块，最后释放间接块本身。

**验证**: 编译并在 QEMU 中测试完整生命周期：
```bash
# 准备干净的镜像
dd if=/dev/zero of=ext2.img bs=1M count=8
mkfs.ext2 -b 1024 ext2.img

cd build && cmake .. && make big_kernel_common
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin \
  -drive file=../ext2.img,format=raw,if=none,id=ahci0 \
  -device ahci,id=ahci_ctlr

# 在 shell 中测试创建、列出、删除的完整流程
# touch /hello.txt
# ls /
# rm /hello.txt
# ls /
```

### Step 8: 用宿主机工具验证磁盘一致性

**目标**: 确认我们的写操作没有破坏 ext2 磁盘结构。

**验证**:
```bash
# 备份镜像
cp ext2.img ext2_before.img

# 在 QEMU 中执行一些创建和删除操作后退出

# 用 dumpe2fs 检查
dumpe2fs ext2.img 2>/dev/null | grep -E "Free blocks|Free inodes"

# 用 fsck 检查一致性
e2fsck -f -n ext2.img
```

`e2fsck -f -n` 会以只读模式检查文件系统一致性。-n 表示不修改任何东西。如果输出没有报错，说明我们的写入操作保持了磁盘结构的正确性。

## 踩坑预警——常见错误清单

**目录项 4 字节对齐**: rec_len 必须是 4 的倍数。如果 rec_len 不对齐，后续条目的起始位置也会偏移，整个目录项链表就会错位。对齐公式是 `(8 + name_len + 3) & ~3`——先加 3 再清掉低 2 位，等价于向上取整到 4 的倍数。

**mkdir 的 i_links_count**: 新目录的 i_links_count 初始化为 2（不是 1）——1 来自父目录中的目录项，1 来自自身的 "." 条目。如果初始化为 1，当 "." 不被算入时，删除目录后 i_links_count 降为 -1（uint 溢出），unlink 里的 `if (i_links_count > 0)` 判断会永远为真，inode 永远不会被释放。

**add_dir_entry 中父目录 i_size 的更新**: 如果 add_dir_entry 分配了新的数据块（现有块放不下），它会修改 dir_disk.i_size。但这个修改发生在 Ext2Inode 的内存拷贝上——调用方（create/mkdir）必须在 add_dir_entry 返回后把修改后的 dir_disk 写回磁盘（write_disk_inode），否则磁盘上父目录的 i_size 仍是旧值。

**get_or_alloc_block 的 DMA buffer 覆盖**: 在分配间接块指向的数据块时，步骤是：分配新块 -> 清零并写入新块 -> 读间接块 -> 写入指针 -> 写回间接块。如果你在"清零并写入新块"之后忘了"读间接块"这一步，DMA buffer 里还是新块的清零数据，你就会把间接块的内容覆盖成全零，丢失之前所有的间接指针。

**remove_dir_entry 不缩减目录**: 删除条目后我们不会把目录的 i_size 缩减，也不会释放空的数据块。这在 Linux 里也是一样的——目录只会增长不会自动缩小（除非你用特殊工具）。空闲空间通过 rec_len 合并来管理，等待未来的条目来复用。

## 构建与运行

```bash
# 完整构建
cd build && cmake .. && make big_kernel_common

# 准备干净的测试镜像
dd if=/dev/zero of=ext2.img bs=1M count=8
mkfs.ext2 -b 1024 ext2.img

# 运行
qemu-system-x86_64 -serial stdio -kernel big_kernel.bin \
  -drive file=../ext2.img,format=raw,if=none,id=ahci0 \
  -device ahci,id=ahci_ctlr

# 在 shell 中测试（需要第 3 章的命令支持）
# touch /created_by_cinux.txt
# echo hello > /echo_test.txt
# ls /
# cat /echo_test.txt
# rm /created_by_cinux.txt
# ls /
```

## 本章小结

| 概念 | 要点 |
|------|------|
| get_or_alloc_block | 直接块直接查 i_block，间接块需先分配间接块本身再分配数据块 |
| write | read-modify-write 部分块写入，循环逐块处理，最后更新 i_size |
| add_dir_entry | 分割现有条目的 rec_len 或分配新块，插入新目录项 |
| remove_dir_entry | 合并 rec_len 到前一条目或置 inode=0（块首条目） |
| create | alloc_inode + 初始化 + add_dir_entry，失败时 free_inode 回滚 |
| mkdir | alloc_inode + alloc_block + "."/".." + add_dir_entry + 更新 links_count |
| unlink | remove_dir_entry + 递减 links_count + 条件释放块和 inode |
| 目录项对齐 | rec_len 必须是 4 的倍数，计算公式 (8 + name_len + 3) & ~3 |
