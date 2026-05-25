---
title: 028c-fs-cwd-stat-2 · CWD 与 Stat
---

# 028c-2 Tutorial: chdir / getcwd / stat —— 从用户路径到文件元数据

## 前言

上一章我们搭好了路径解析的基础设施。现在来用它构建四个真正的 syscall——sys_chdir 让进程能切换工作目录，sys_getcwd 让进程能查询当前工作目录，sys_stat 和 sys_fstat 让进程能获取文件的元数据信息。这四个 syscall 看起来各自独立，但它们共享了同一个底层模式：路径相关的操作都是"解析路径 -> VFS 查找 -> 操作 inode"这条流水线的变体。

## chdir：我要搬到这个目录

chdir 是 Unix 最古老的 syscall 之一——它的编号是 12，在 Linux x86_64 syscall 表中排第 12 位。在 V7 Unix 中就已经存在了。Cinux 的 sys_chdir 实现非常直白：

```cpp
int64_t sys_chdir(uint64_t path_virt, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }

    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(resolved, &rel_path);
    if (fs == nullptr) { return -1; }

    cinux::fs::Inode* inode = fs->lookup(rel_path);
    if (inode == nullptr) { return -1; }

    if (inode->type != cinux::fs::InodeType::Directory) { return -1; }

    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) { return -1; }

    uint32_t i = 0;
    while (resolved[i] != '\0' && i < sizeof(current->cwd) - 1) {
        current->cwd[i] = resolved[i]; ++i;
    }
    current->cwd[i] = '\0';
    return 0;
}
```

五步走：解析路径、找文件系统、查 inode、验目录、更新 CWD。每一步失败都返回 -1 且不修改 CWD。`sizeof(current->cwd) - 1` 确保路径不会溢出 256 字节的 cwd 缓冲区。

Linux 的 `sys_chdir()` 实现在 `fs/open.c` 中，流程完全一样——`kern_path()` 做路径解析，`d_is_dir()` 检查是否为目录，`set_fs_pwd()` 更新 `task_struct->fs->pwd`。区别在于 Linux 用 dentry 引用而不是字符串拷贝，而且有 RCU 读锁和 seqlock 保护并发访问。

SerenityOS 在 `Kernel/Syscalls/chdir.cpp` 中用 `VFS::the().resolve_path()` 做路径解析，然后用 `process.set_current_directory(custody)` 更新 CWD。同样是用引用而不是字符串。

Cinux 的字符串方案虽然简单，但它有一个值得讨论的语义问题：如果目录 A 被 `chdir` 进去之后又被 `rmdir` 删掉了，CWD 字符串仍然指向那个已不存在的路径。下次 `getcwd` 返回的路径实际上已经失效了。Linux 不存在这个问题——因为 dentry 有引用计数，被 rmdir 的目录只要还有进程的 CWD 引用它就不会真正被回收。这是字符串方案的固有限制。

## getcwd：我现在在哪

sys_getcwd 的实现几乎是四个 syscall 中最简单的——直接从 Task 中读 CWD 字符串：

```cpp
int64_t sys_getcwd(uint64_t buf_virt, uint64_t size, uint64_t,
                   uint64_t, uint64_t, uint64_t) {
    // 验证用户指针和大小 ...
    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) { return -1; }

    uint32_t cwd_len = 0;
    while (current->cwd[cwd_len] != '\0') { ++cwd_len; }
    ++cwd_len;  // include NUL

    if (cwd_len > size) { return -1; }

    auto* dst = reinterpret_cast<char*>(buf_virt);
    memcpy(dst, current->cwd, cwd_len);
    return static_cast<int64_t>(cwd_len);
}
```

Linux 的 `getcwd()` 实现在 `fs/d_path.c` 中，逻辑完全不同——因为 CWD 存储的是 dentry 引用，Linux 需要 `d_path()` 从 dentry 树反向构建路径字符串。这是一个从叶子节点沿 `d_parent` 指针遍历到根目录的过程，每一步都要读取 dentry 的名字。SerenityOS 的做法也类似，通过 `Custody` 链构建路径。

Cinux 的字符串存储方案让 getcwd 变成了一个简单的 memcpy——O(n) 时间复杂度，n 是路径长度。Linux 的 d_path 方案在最坏情况下是 O(d*n)，d 是目录深度，n 是平均目录名长度。有趣的是，Cinux 在这种场景下反而更快。

## stat：告诉我这个文件的详细信息

stat 系统调用是 Unix 查询文件元数据的标准接口。它的设计体现了 VFS 层的分层思想——syscall 层只管路径解析和结果拷贝，具体的元数据读取委托给文件系统驱动。

### struct stat 的布局

Cinux 的 struct stat 遵循 Linux x86_64 的惯例：

```cpp
struct stat {
    uint64_t st_dev;      // 设备 ID
    uint64_t st_ino;      // inode 号
    uint32_t st_mode;     // 文件类型和权限
    uint32_t st_nlink;    // 硬链接数
    uint32_t st_uid;      // 所有者用户 ID
    uint32_t st_gid;      // 所有者组 ID
    uint64_t st_rdev;     // 特殊文件的设备 ID
    int64_t  st_size;     // 文件大小（字节）
    uint64_t st_blksize;  // 推荐 I/O 块大小
    uint64_t st_blocks;   // 分配的 512 字节块数
    uint64_t st_atime;    // 最后访问时间
    uint64_t st_mtime;    // 最后修改时间
    uint64_t st_ctime;    // 最后状态改变时间
};
```

Linux 的 `struct stat64` 在 glibc 中有 120 字节，Cinux 的这个结构体因为省略了一些 padding 和纳秒级时间戳子字段，大约是 88 字节。但核心字段都在。st_mode 是一个 32 位整数，低 12 位是权限位（rwxrwxrwx + setuid/setgid/sticky），高 4 位是文件类型（regular、directory、symlink 等）。ext2 的 i_mode 字段直接对应 st_mode——这是 ext2 设计之初就和 Unix stat 语义对齐的结果。

### InodeOps 扩展

stat 的实现通过 InodeOps 虚函数分派到具体的文件系统驱动：

```cpp
class InodeOps {
public:
    // ... 已有虚函数 ...
    virtual int64_t stat(const Inode* inode, struct stat* st);
};
```

默认实现返回 -1。ext2 的两个实现（Ext2FileOps 和 Ext2DirOps）完全相同，都是从缓存的磁盘 inode 读取字段：

```cpp
int64_t Ext2FileOps::stat(const Inode* inode, struct stat* st) {
    if (inode == nullptr || inode->fs_private == nullptr || st == nullptr) {
        return -1;
    }

    auto* cached = static_cast<const Ext2CachedInode*>(inode->fs_private);
    const Ext2Inode& disk = cached->disk_inode;

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
```

这里的关键观察是：stat 不需要额外的磁盘 I/O。ext2 在 lookup 时已经把整个磁盘 inode 读入 `Ext2CachedInode`，stat 只是从内存缓存中读取字段。这在 Linux 中也是一样的——`ext4_getattr()` 直接从 `ext4_inode_info` 中读取数据，只有当文件被打开后 inode 还没有读入内存时才会触发 I/O。

### sys_stat 和 sys_fstat 的实现

sys_stat 通过路径查找 inode：

```cpp
int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, ...) {
    if (!validate_user_ptr(st_virt)) return -1;

    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) return -1;

    // VFS resolve + lookup + stat
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(resolved, &rel_path);
    if (fs == nullptr) return -1;

    cinux::fs::Inode* inode = fs->lookup(rel_path);
    if (inode == nullptr || inode->ops == nullptr) return -1;

    cinux::fs::stat kst;
    if (inode->ops->stat(inode, &kst) < 0) return -1;

    auto* user_st = reinterpret_cast<cinux::fs::stat*>(st_virt);
    memcpy(user_st, &kst, sizeof(cinux::fs::stat));
    return 0;
}
```

sys_fstat 通过文件描述符查找 inode——省去了路径解析和 VFS 查找两步，直接通过 `g_global_fd_table()` 拿到 inode 指针。后半段（调用 stat、拷贝结果）完全一样。

Linux 的 `sys_stat()` 实现在 `fs/stat.c` 中，流程相同但代码量多得多——需要处理符号链接跟随（stat vs lstat）、audit hook、security hook、compat ioctl 等。Cinux 的实现只有核心路径，大约 30 行。

## 参考

- Linux `sys_chdir()` 在 `fs/open.c`: https://elixir.bootlin.com/linux/latest/source/fs/open.c
- Linux `sys_getcwd()` 在 `fs/d_path.c`: https://elixir.bootlin.com/linux/latest/source/fs/d_path.c
- Linux `sys_stat()` 在 `fs/stat.c`: https://elixir.bootlin.com/linux/latest/source/fs/stat.c
- Linux `stat(2)` man page: https://man7.org/linux/man-pages/man2/stat.2.html
- Linux `chdir(2)` man page: https://man7.org/linux/man-pages/man2/chdir.2.html
- ext2 disk inode format: https://wiki.osdev.org/Ext2#Inode_Table
