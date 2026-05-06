# 027-1 Tutorial: 从 Ramdisk 到 VFS — 为什么内核需要一层文件系统抽象

> 标签：VFS、inode、文件系统、系统调用、内核架构
> 前置：[026_fs_ramdisk](026-fs-ramdisk-1.md)

## 前言

说实话，写完 026 tag 的 Ramdisk 之后我有一种"差不多行了"的错觉——ustar 解析能跑、文件名能打印、归档数据在内存里摆着，看起来伸手就能够到 open/read 了。但真正开始写 sys_open 的时候，问题啪地一下就拍脸上了：内核里根本没有"文件系统"这个概念。sys_read 的代码写死了 fd=0 读键盘，sys_write 写死了 fd=1 走 kprintf。如果我现在直接在 sys_read 里加一段"如果 fd > 2 就去 Ramdisk 里找文件"的逻辑，等以后加 ext2 支持的时候怎么办？再写一套 if-else？然后等 NFS 支持再加一层？那 sys_read 就变成了一个巨大的 switch-case 深渊。

这种"先硬编码、以后再抽象"的思路在 OS 开发中是一个非常常见的陷阱。xv6 实际上就掉进了这个陷阱——它的文件系统代码（fs.c 和 file.c）是直接硬编码的，没有独立的 VFS 层。结果当有人试图在 xv6 上加第二文件系统时（MIT 有篇论文叫 "A Virtual Filesystem Layer in XV6"），发现需要重构几乎整个文件系统子系统。

所以这一步是绕不过去的——我们必须在 syscall 和具体文件系统之间加一层抽象。这层抽象在 Unix 世界里叫 VFS（Virtual File System），最早由 Sun Microsystems 在 1986 年的 "Vnodes" 论文中提出（Kleiman, "Vnodes: An Architecture for Multiple File System Types in Sun UNIX"）。Linux 从 2.0 时代就有了，xv6 虽然没有显式的 VFS 层但其 inode 结构本身就承担了这个角色。我们今天要做的事情用一句话概括：定义一套通用的文件系统接口，让 sys_open/sys_read/sys_write 不需要知道底层是 Ramdisk 还是 ext2 还是别的什么。

## 环境说明

这次开发的起点是 026_fs_ramdisk tag 的代码。工具链还是老样子——GCC 14 + CMake + QEMU。一个值得注意的变化是，我们在 `kernel/arch/x86_64/crt_stub.cpp` 中新增了 `__cxa_guard_acquire` 和 `__cxa_guard_release` 两个函数，这是因为 VFS 层用到了函数内的 static 局部变量（g_global_fd_table），编译器会自动生成对这两个函数的调用。在 freestanding 环境下（-ffreestanding），它们不会被标准库提供，必须手动实现。这个坑坑了我半天——链接的时候突然冒出来两个 undefined reference，搜了半天才明白是 C++ ABI 的一部分。

## 第一步——定义 Inode：文件系统对象的身份卡

我们从一个问题开始：一个文件在内核里到底长什么样？Linux 的回答是 inode——一个包含文件所有元数据（大小、权限、时间戳、磁盘块地址）的结构体，定义在 fs/include/linux/fs.h 里，光字段就有三十多个。xv6 的回答也是 inode，只是简单得多——类型（T_FILE/T_DIR/T_DEV）、大小、NINDIRECT 个块号。Cinux 的回答介于两者之间，但更偏向 xv6 的简洁风格。

我们的 Inode 有五个核心字段：ino（编号，uint64_t）、size（字节大小，uint64_t）、type（Regular 或 Directory，InodeType 枚举）、ops（操作函数指针表指针，InodeOps*）、fs_private（后端私有数据指针，void*）。前三个是所有文件系统都需要的公共信息，后两个是"插件点"——ops 指向一个 InodeOps 函数指针结构体实例，不同的文件系统通过提供自己的 InodeOps 静态实例（函数指针指向自己的实现函数）来提供不同实现；fs_private 是一个 void 指针，Ramdisk 可以用它指向自己的 RamdiskEntry（包含文件名和数据指针），ext2 可以用它指向磁盘块地址列表。除了这五个核心字段外，Inode 还有 mode/uid/gid/nlink/atime/ctime/mtime/blocks 等字段，为 stat 系统调用和权限检查预留。

InodeType 用 enum class 定义，底层类型是 uint8_t——这样枚举值不会泄漏到外层命名空间，而且只占 1 字节。Linux 的 inode 也有类似的 i_mode 字段区分文件类型（S_IFREG/S_IFDIR 等），但它用了 16 位的 mode_t，还包含了权限位。

```cpp
enum class InodeType : uint8_t {
    Unknown   = 0,
    Regular   = 1,
    Directory = 2,
};
```

InodeOps 在实际代码中是一个函数指针结构体——包含 read、write、readdir 等函数指针字段，以及 create/mkdir/unlink/stat 等扩展操作。不支持的操作设为 nullptr，调用方在调用前需要检查函数指针是否为空。比如普通文件的 ops 不需要设置 readdir（保持 nullptr），目录的 ops 不需要设置 read（保持 nullptr）。

```cpp
struct InodeOps {
    int64_t (*read)(const Inode* inode, uint64_t offset, void* buf, uint64_t count);
    int64_t (*write)(Inode* inode, uint64_t offset, const void* buf, uint64_t count);
    int64_t (*readdir)(const Inode* inode, uint64_t index, char* name, uint64_t name_max);
    Inode*  (*create)(Inode* dir, const char* name, uint32_t namelen);
    Inode*  (*mkdir)(Inode* dir, const char* name, uint32_t namelen);
    int64_t (*unlink)(Inode* dir, const char* name, uint32_t namelen);
    int64_t (*stat)(const Inode* inode, struct stat* st);
};
```

你可能会问：为什么用函数指针结构体而不是虚基类？说实话两种方式都能达到目的，但函数指针表有几个实实在在的好处。第一，不支持的操作就是 nullptr——Ramdisk 里所有普通文件的 write 直接不设置，调用方看到 nullptr 就知道不支持。第二，新增操作只需要在结构体里加一个函数指针字段，已有的 ops 实例中未初始化的新字段自然是零（nullptr），不需要逐个修改。第三，每个文件系统后端的 ops 实例（如 ramdisk_file_ops）仍然是按类别共享的——所有 Ramdisk 普通文件共享同一个 ramdisk_file_ops 实例。这种设计和 Linux 内核的 file_operations / inode_operations 如出一辙。

## 第二步——FileSystem 抽象基类：所有后端的统一接口

有了 Inode 和 InodeOps 之后，我们需要一个"文件系统后端"的抽象——所有具体的文件系统（Ramdisk、ext2、将来的网络文件系统）都实现这套接口。注意 FileSystem 是真正的 C++ 抽象基类（有纯虚函数），而 InodeOps 是纯 C 风格的函数指针结构体——两者配合使用，FileSystem 负责"挂载和查找"的多态，InodeOps 负责"文件操作"的多态。

```cpp
class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual bool mount() = 0;
    virtual Inode* lookup(const char* path) = 0;
};
```

mount() 负责初始化后端——Ramdisk 的 mount 解析 ustar 归档建立条目表，ext2 的 mount 会读取超级块和块组描述符。lookup() 根据路径找到对应的 Inode——注意参数是相对路径，挂载点前缀由上层 VFS 在 resolve 时剥离。

这个设计和 Linux 的 super_block + super_operations 有异曲同工之妙，但我们去掉了 superblock 结构（信息太少不值得单独抽象），直接把 mount/lookup 放在 FileSystem 类里。xv6 没有这层抽象——它的 inode 操作直接嵌入在 fs.c 里，所有函数都是全局的。这对单文件系统来说够用，但一旦要加 ext2，就得大量 if-else 分支。

## 第三步——File 和 FDTable：进程看到的世界

Inode 是文件系统视角的对象（"这个文件存在，大小是 19 字节"），而 File 是进程视角的对象（"我打开了这个文件，当前读到了第 6 个字节"）。为什么要分开？因为同一个文件可以被打开多次——每次 open 都应该有独立的读写位置。如果你在 Inode 上存 offset，两个进程 open 同一个文件就会互相干扰。

```cpp
struct File {
    File(Inode* in, uint64_t off, OpenFlags fl)
        : inode(in), offset(off), flags(fl) {}

    Inode*    inode;         // 指向底层 Inode
    uint64_t  offset;        // 当前读写偏移量
    OpenFlags flags;         // 访问模式 (RDONLY/WRONLY/RDWR)

    mutable cinux::proc::Spinlock offset_lock_;  // 保护 offset 并发访问
};
```

FDTable 是文件描述符表——一个固定 256 槽位的 File 指针数组。fd 0/1/2 保留给 stdin/stdout/stderr，alloc 从 fd=3 开始线性扫描找第一个空位。这个设计和 xv6 的 ofile[] 数组如出一辙，只是 xv6 的 NOFILE 是 16（太小了），我们选了 256（和 Linux 默认的 RLIMIT_NOFILE 一致）。Linux 用 fdtable/fd_array 管理，支持动态扩展。FDTable 还有一个 set() 方法，用于 sys_pipe 和 dup2 等需要把 File 安装到指定 fd 编号的场景。

FDTable::alloc 做的事情很简单——线性扫描找空位，new 一个 File 对象放进去。Linux 的 alloc_fd 用位图加速了查找，但我们的 256 槽位线性扫描在缓存友好性上其实不差，何况教学 OS 不需要极端性能。close 做 delete + 置 nullptr，防御性地检查越界和双重关闭——内核里的 double-free 会直接 panic，绝不能手滑。

## 设计对比：Cinux vs xv6 vs Linux

这是理解 VFS 设计最有效的方式——对比三个系统在同一个问题上的不同选择。

**xv6** 没有独立的 VFS 层。它的 inode 结构直接嵌入磁盘布局（dinode），全局 ftable 管理所有打开文件，每个进程的 ofile[] 数组映射 fd 到 ftable 条目。readi/writei 是全局函数，根据 inode 的 type 字段决定行为。这种"大一统"设计简单直接，但只能支持一种文件系统。如果你试图给 xv6 加 ext2 支持，需要修改 readi/writei/filealloc 几乎所有函数，加入 if (inode->type == T_EXT2) 的分支——很快就不可维护了。

**Linux** 的 VFS 是工业级的。四大对象（superblock 管理挂载实例、inode 管理文件元数据、dentry 管理路径缓存、file 管理打开文件），每个对象有对应的 operations 结构（super_operations、inode_operations、dentry_operations、file_operations）。dentry cache 加速路径查找，vfsmount 树管理挂载点层次。复杂度极高但扩展性极强——新文件系统只需要实现对应的 operations 就能接入，Linux 支持几十种文件系统就是这个架构的成果。

**Cinux** 选了中间路线。我们有 FileSystem 抽象基类（类似 superblock 的角色）、Inode + InodeOps（类似 inode + inode_operations）、File + FDTable（类似 file + fdtable）。但没有 dentry cache（每次 lookup 都线性扫描）、没有 superblock 的日志功能、没有 ACL 权限检查。这种简化是刻意的——足够理解 VFS 的核心思想（抽象、多态、分层），又不至于被 Linux 级别的复杂度淹没。

| 维度 | xv6 | Cinux | Linux |
|------|-----|-------|-------|
| VFS 抽象 | 无，全局函数 | FileSystem 基类 | super_operations |
| Inode 多态 | 无，按 type 分支 | InodeOps 函数指针表 | inode_operations |
| 路径缓存 | 无 | 无 | dentry cache |
| 挂载管理 | 无 | 固定数组 | vfsmount 树 |
| FD 管理 | ofile[16] | FDTable[256] | fdtable/fd_array |
| 扩展方式 | 改源码 | 继承 FileSystem | register_filesystem() |

## 收尾

到这里我们已经搭好了 VFS 的数据结构骨架——Inode、InodeOps（函数指针表）、FileSystem（抽象基类）、File、FDTable。这些数据结构本身就是 VFS 的核心抽象，定义了"文件系统对象长什么样"、"文件系统后端怎么接入"、"进程怎么看文件"这三个基本问题的答案。下一章我们加上挂载点表和路径解析，让 sys_open 真正能通过路径找到文件。

## 参考资料

- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — Mount Point List 模型、vnode/open-file 分离、三种 VFS 架构
- Linux Kernel: [Overview of the Virtual File System](https://docs.kernel.org/filesystems/vfs.html) — 四大 VFS 对象（superblock/inode/dentry/file）详解
- xv6 Book: [File System Chapter](https://github.com/mit-pdos/xv6-book/blob/master/fs.t) — xv6 七层文件系统架构和 inode 设计
- Kleiman, 1986: "Vnodes: An Architecture for Multiple File System Types in Sun UNIX" — VFS/vnode 的原始设计论文
- Intel SDM Vol.1 §3.3.7.1: Linear Address — x86_64 canonical address 定义（后面 sys_open 会用到）
