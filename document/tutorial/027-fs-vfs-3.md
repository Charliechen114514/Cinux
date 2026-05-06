# 027-3 Tutorial: 系统调用全链路 — 从 open 到 read 到 close 的完整旅程

> 标签：syscall、VFS、open、read、write、close、getdents、canonical address
> 前置：[027-2 挂载点表](027-fs-vfs-2.md)

## 前言

前面两章我们把 VFS 的数据结构和挂载点表都搭好了——Inode 能代表一个文件、FileSystem 能被继承、挂载点表能把路径路由到正确的后端。但这一切到目前为止都还只是"静态的骨架"——没有任何代码真正地把用户态的 open 调用和 VFS 串联起来。数据结构在那里静静地等着被使用，InodeOps 的函数指针表还没有任何实例填充，FDTable 的 256 个槽位全是空的。

这一章我们要做的事用大白话说就是——让这些骨架活起来。我们要把 Ramdisk 从一个只能打印文件名的"解析器"升级为一个真正的文件系统后端（继承 FileSystem、实现 mount/lookup、提供 InodeOps 函数指针表），然后实现三个新的系统调用（sys_open、sys_close、sys_getdents），改造两个已有的系统调用（sys_read、sys_write），让它们在 fd != 0/1 时走 VFS 路径。完成之后，从用户态的 libc wrapper 到 syscall dispatch，再到 VFS resolve、FileSystem lookup、InodeOps 回调，最终到 Ramdisk 的数据拷贝——这条完整的调用链就全部打通了。

这是 027 tag 中代码量最大的一步（光 ramdisk.cpp 就改了 257 行，新增 sys_open/sys_close/sys_getdents 三个文件共 166 行），但逻辑上并不复杂——每一步都是在前面搭好的框架上"填空"。

## 环境说明

新增文件：`kernel/syscall/sys_open.cpp`、`sys_open.hpp`、`sys_close.cpp`、`sys_close.hpp`、`kernel/syscall/sys_getdents.cpp`、`sys_getdents.hpp`。大量修改：`kernel/fs/ramdisk.hpp` 和 `ramdisk.cpp`（继承 FileSystem + 提供 InodeOps 函数指针表 + 新增 lookup）、`kernel/syscall/sys_read.cpp`（添加 VFS 路径）、`kernel/syscall/sys_write.cpp`（添加 VFS 路径）、`kernel/syscall/syscall_nums.hpp`（新增三个系统调用号）、`kernel/arch/x86_64/syscall.cpp`（注册新 handler）。地址校验从简单的 USER_ADDR_MAX 改为 x86_64 canonical address 规则。

## 第一步——Ramdisk 适配 VFS：穿上文件系统的外衣

026 的 Ramdisk::mount() 只做了两件事：遍历 ustar 归档打印文件名，返回条目数量。现在它要做的事情多得多——为每个文件建立一个完整的 RamdiskEntry，设置预分配的 Inode，构建根目录的 readdir context。这是 ramdisk.cpp 中修改量最大的部分，但每一步都是对原有逻辑的自然扩展。

RamdiskEntry 的变化最显著。之前 name 字段是 const char*（直接指向 ustar header 的 name 数组），现在变成了 char[100]（把文件名拷贝出来）。原因很实际：ustar header 的生命周期在概念上只在 mount() 遍历期间有效——虽然我们的归档数据是 static 嵌入的、不会移动，但依赖指针的稳定性不是好的编程习惯。更重要的是，每个 entry 现在内嵌了一个 Inode——这意味着 mount 期间每个文件的 inode 就已经准备好了，lookup 不需要动态分配任何东西。这种"预分配"策略在嵌入式系统和高性能场景中很常见——用空间换时间，避免运行时的堆分配和碎片化。

三个 InodeOps 的实现各有特色。ramdisk_file_ops 的 read 函数指针指向的 ramdisk_read 是最核心的一个——从 inode->fs_private 拿到 RamdiskEntry 指针，计算可读字节数 min(count, size-offset)，从归档数据区 memcpy 到用户缓冲区。这里有一个值得注意的细节：offset >= size 时返回 0（EOF），这是 Unix read() 的标准语义——读到文件末尾不是错误，只是"没有更多数据了"。Linux 的 generic_file_buffered_read 和 xv6 的 fileread 都遵循同样的语义。

ramdisk_file_ops 的 write 指针为 nullptr——这是只读文件系统，调用方在调用前需要检查函数指针是否为空。Linux 对只读文件系统的写操作也是返回 -EROFS（Error: Read-only file system），我们通过 nullptr 表示不支持。ramdisk_dir_ops 的 readdir 函数指针指向的 ramdisk_readdir 用 index 分三种情况：0 返回 "."，1 返回 ".."，>= 2 减去 2 后从条目表取真实文件名。这种 index-based 的设计比 Linux 的 linux_dirent 结构简单得多。

## 第二步——sys_open：四步串起 resolve→lookup→alloc

sys_open 的实现是 VFS 设计的教科书范例——每一步都在前一步成功的基础上推进，任何一步失败都返回 -1。

第一步做路径解析。sys_open 调用 resolve_user_path(path_virt, resolved)——这个函数封装了 canonical address 校验和 cwd（当前工作目录）感知的路径解析。canonical address 校验是 x86_64 的重要安全措施：x86_64 使用 48 位线性地址空间，bit 47 是符号位——如果 bit 47 是 0，那 bits 48-63 必须全为 0（用户空间地址）；如果 bit 47 是 1，那 bits 48-63 必须全为 1（内核空间地址）。非规范地址在硬件级别会触发 #GP，resolve_user_path 内部提前拒绝这类地址。这个检查比之前用的 `addr >= 0x800000000000` 上限检查更准确——后者无法正确处理内核空间的高半部分地址。

第二步调用 vfs_resolve，传入完整路径，返回对应的 FileSystem 后端和剥离前缀后的相对路径。比如 resolve("/hello.txt") 在 Ramdisk 挂载在 "/" 时，返回 Ramdisk 实例和 "hello.txt"。

第三步调用 fs->lookup(rel_path)，让具体文件系统后端去查找 Inode。Ramdisk 的 lookup 做线性搜索——简单但有效。

第四步把 flags 值（0/1/2）映射到 OpenFlags 枚举（RDONLY/WRONLY/RDWR），然后调用 FDTable::alloc 分配文件描述符。返回给用户的就是这个 fd 编号。

## 第三步——sys_read/sys_write：双路径分发

改造 sys_read 的关键策略是"VFS 优先"——先查 FDTable 看这个 fd 是否有对应的 File，如果有就走 VFS 路径；如果没有，再检查是不是 fd=0 的键盘路径。这比"fd==0 → 键盘, fd!=0 → VFS"的简单分支更灵活——它允许 fd=0 被重新分配为 pipe 或其他 VFS 后端（比如 shell 重定向场景）。

VFS 读取路径做了三层 nullptr 检查——file != nullptr、file->inode != nullptr、file->inode->ops != nullptr。在内核里，空指针解引用不是 segfault 而是 kernel panic——一个 panic 意味着整个系统停转，比用户态的 segfault 严重得多。所以宁可在入口处多检查几遍，也不要让任何一种 nullptr 漏过去。Linux 的 vfs_read 也有类似的检查链（file->f_op->read_iter != NULL 等）。offset 的更新受 file->offset_lock_ 自旋锁保护。

读取成功后 file->offset += result 更新读写位置。这是 Unix read 的核心语义：每次 read 从当前 offset 读取，读完后 offset 自动前进。xv6 的 fileread（`f->off += r`）和 Linux 的 vfs_read（`file->f_pos += ret`）都是同样的做法。

sys_write 的改造也采用 VFS 优先策略——先查 FDTable 走 VFS 写路径，如果没有再检查 fd=1 的 kprintf 路径。对于 Ramdisk 来说写操作返回 -1，但将来 ext2 支持写入后，这条路径就能正常工作了。这种"先搭好路、后通车"的设计思路在 OS 开发中非常重要——接口先行，实现后补。

## 第四步——sys_getdents：用 offset 当目录项索引

sys_getdents 是 027 tag 里最"非标"的系统调用。Linux 的 getdents(2) 返回一个 linux_dirent 结构体数组（包含 d_ino、d_off、d_reclen、d_name），调用方需要根据 d_reclen 步进遍历返回的缓冲区。这种设计一次系统调用可以返回多个目录项，性能好但实现复杂——你需要计算对齐填充、维护 d_off 的值、处理缓冲区不够放一个完整 dirent 的情况。

我们的简化版本每次只返回一个目录项名称（字符串），用 file->offset 做索引。因为对目录 fd 来说"文件偏移量"没有意义——你不能 seek 一个目录——所以 offset 字段可以安全地挪用为目录项的 0-based 索引。每次 readdir 成功后 offset++，下次调用就读下一个目录项。这种设计牺牲了性能（每个目录项一次 syscall），但大幅降低了实现复杂度。对于 Ramdisk 里 3-5 个文件的教学场景来说，多几次 syscall 完全不是问题。

## 收尾

到这里 open/read/write/close/getdents 的全链路已经打通了。但用户态的程序还不能直接调用这些 syscall——它们需要 libc wrapper 和 shell 命令。下一章我们补上这最后一块拼图，加上 cat 和 ls 命令以及一套完整的测试矩阵，让整个 milestone 画上句号。

## 参考资料

- Intel SDM Vol.1 §3.3.7.1: Linear Address — x86_64 48 位线性地址空间和 canonical address 的硬件定义（bit 47 符号扩展）
- Linux: [getdents(2)](https://man7.org/linux/man-pages/man2/getdents.2.html) — POSIX getdents 的完整语义和 linux_dirent 结构
- xv6: [sysfile.c](https://github.com/mit-pdos/xv6-public/blob/master/sysfile.c) — xv6 的 sys_open/sys_read/sys_write 实现，可以对比 Cinux 的简化版本
- Linux Kernel: [open.c](https://elixir.bootlin.com/linux/latest/source/fs/open.c) — Linux do_filp_open 的完整流程（path_lookup + get_unused_fd + do_open）
- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — open/read/write/close 系统调用的 VFS 流程描述
