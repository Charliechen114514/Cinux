# 028c-2 Read-through: sys_chdir / sys_getcwd 与 stat 实现

## 导语

上一章我们看完了路径解析的基础设施。现在来看构建在它之上的四个新 syscall——sys_chdir、sys_getcwd、sys_stat、sys_fstat。这四个 syscall 都比较轻量，但它们展示了路径相关 syscall 的两种典型模式：chdir 和 stat 需要路径解析 + VFS 查找，getcwd 直接读 Task 字段，fstat 通过文件描述符找 inode。

## sys_chdir —— 切换工作目录

`kernel/syscall/sys_chdir.cpp` 共 75 行，完整实现了目录切换的流程：

```cpp
int64_t sys_chdir(uint64_t path_virt, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }
```

第一步，调用上一章实现的 `resolve_user_path`，把用户传入的（可能为相对的）路径解析为绝对路径。如果解析失败（指针无效、空字符串等），直接返回 -1。

```cpp
    // Step 2: Resolve through the VFS mount table
    const char* rel_path = nullptr;
    cinux::fs::FileSystem* fs = cinux::fs::vfs_resolve(resolved, &rel_path);

    if (fs == nullptr) {
        kprintf("[SYS_CHDIR] No filesystem mounted for '%s'\n", resolved);
        return -1;
    }
```

第二步，通过 VFS 挂载表找到负责这个路径的文件系统实例。`vfs_resolve` 会根据绝对路径的前缀匹配挂载点，返回文件系统指针和相对挂载点的剩余路径。

```cpp
    // Step 3: Look up the inode
    cinux::fs::Inode* inode = fs->lookup(rel_path);

    if (inode == nullptr) {
        kprintf("[SYS_CHDIR] Path not found: '%s'\n", resolved);
        return -1;
    }

    // Step 4: Verify it is a directory
    if (inode->type != cinux::fs::InodeType::Directory) {
        kprintf("[SYS_CHDIR] Not a directory: '%s'\n", resolved);
        return -1;
    }
```

第三步和第四步，通过文件系统的 lookup 方法查找 inode，然后验证它的类型必须是 Directory。`cd /etc/passwd` 不应该成功——passwd 是文件不是目录。

```cpp
    // Step 5: Update cwd
    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) {
        return -1;
    }

    uint32_t i = 0;
    while (resolved[i] != '\0' && i < sizeof(current->cwd) - 1) {
        current->cwd[i] = resolved[i];
        ++i;
    }
    current->cwd[i] = '\0';

    return 0;
}
```

最后一步，获取当前 Task 指针，把解析后的绝对路径字符串拷贝到 cwd 字段中。拷贝时限制长度不超过 `sizeof(cwd) - 1 = 255` 字节，保证 NUL 终止。注意这里没有任何锁保护——在单核教学内核中这不是问题，但多核环境下需要加锁。

## sys_getcwd —— 读取工作目录

`kernel/syscall/sys_getcwd.cpp` 是四个 syscall 中最简单的：

```cpp
int64_t sys_getcwd(uint64_t buf_virt, uint64_t size, uint64_t,
                   uint64_t, uint64_t, uint64_t) {
    // Validate user pointer
    if (buf_virt == 0) {
        return -1;
    }
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) {
        return -1;
    }
    if (bit47 == 1 && upper != 0xFFFF) {
        return -1;
    }

    if (size == 0) {
        return -1;
    }
```

函数开头做了用户指针的规范地址检查（和 validate_user_ptr 相同的逻辑）和缓冲区大小检查。这里没有使用 resolve_user_path，因为 getcwd 不涉及路径解析——它只是读一个字符串。

```cpp
    // Step 1: Get current task
    cinux::proc::Task* current = cinux::proc::Scheduler::current();
    if (current == nullptr) {
        return -1;
    }

    // Step 2: Compute cwd length (including NUL)
    uint32_t cwd_len = 0;
    while (current->cwd[cwd_len] != '\0') {
        ++cwd_len;
    }
    ++cwd_len;  // include NUL

    if (cwd_len > size) {
        return -1;
    }

    // Step 3: Copy cwd to user buffer
    auto* dst = reinterpret_cast<char*>(buf_virt);
    memcpy(dst, current->cwd, cwd_len);

    return static_cast<int64_t>(cwd_len);
}
```

核心逻辑是：计算 cwd 字符串长度（含 NUL），检查用户缓冲区是否足够大，然后 memcpy 拷贝过去。返回值是写入的字节数（含 NUL），不是 0——这和 POSIX getcwd 的语义一致。如果用户缓冲区不够大，返回 -1 表示错误。

注意这个函数可以直接访问 `buf_virt` 对应的用户空间内存，因为我们运行在内核态，页表映射覆盖了用户空间地址。当然，这里没有做页表权限检查——如果用户传了一个未映射的地址，memcpy 会触发 page fault。这是一个教学内核可以接受的简化。

## struct stat —— 文件元数据结构

`kernel/fs/stat.hpp` 定义了 stat 结构体：

```cpp
struct stat {
    uint64_t st_dev;      ///< Device ID
    uint64_t st_ino;      ///< Inode number
    uint32_t st_mode;     ///< File type and permissions
    uint32_t st_nlink;    ///< Number of hard links
    uint32_t st_uid;      ///< Owner user ID
    uint32_t st_gid;      ///< Owner group ID
    uint64_t st_rdev;     ///< Device ID (if special file)
    int64_t  st_size;     ///< Total file size in bytes
    uint64_t st_blksize;  ///< Preferred block size for I/O
    uint64_t st_blocks;   ///< Number of 512-byte blocks allocated
    uint64_t st_atime;    ///< Time of last access
    uint64_t st_mtime;    ///< Time of last modification
    uint64_t st_ctime;    ///< Time of last status change
};
```

这个布局遵循 Linux x86_64 的 `struct stat64` 约定。st_size 是 int64_t（支持负值作为错误标记，虽然实际上不会是负的），其余整数类型都是无符号的。st_blksize 对于 ext2 来说就是 block_size（通常是 1024），st_blocks 的单位是 512 字节块（这是 POSIX 的标准单位，不是文件系统的块大小）。

## InodeOps 扩展

`kernel/fs/inode.hpp` 中，`InodeOps` 基类增加了 stat 虚函数：

```cpp
class InodeOps {
public:
    // ... 已有虚函数 ...
    virtual int64_t stat(const Inode* inode, struct stat* st);
};
```

默认实现在 inode.cpp 中，返回 -1（不支持 stat 的文件系统不需要重写）。

`Inode` 结构体本身也扩展了元数据字段：

```cpp
struct Inode {
    // ... 已有字段 ...
    uint32_t    mode;         ///< File mode (type + permissions)
    uint32_t    uid;          ///< Owner user ID
    uint32_t    gid;          ///< Owner group ID
    uint32_t    nlink;        ///< Hard link count
    uint64_t    atime;        ///< Time of last access
    uint64_t    ctime;        ///< Time of last status change
    uint64_t    mtime;        ///< Time of last modification
    uint64_t    blocks;       ///< Number of 512-byte blocks allocated
};
```

这些字段在 ext2 的 `populate_vfs_inode()` 中从磁盘 inode 填充：

```cpp
    cached.vfs_inode.mode  = disk.i_mode;
    cached.vfs_inode.uid   = disk.i_uid;
    cached.vfs_inode.gid   = disk.i_gid;
    cached.vfs_inode.nlink = disk.i_links_count;
    cached.vfs_inode.atime = disk.i_atime;
    cached.vfs_inode.ctime = disk.i_ctime;
    cached.vfs_inode.mtime = disk.i_mtime;
    cached.vfs_inode.blocks = disk.i_blocks;
```

## Ext2 stat 实现

`kernel/fs/ext2.cpp` 中，`Ext2FileOps::stat()` 和 `Ext2DirOps::stat()` 实现完全相同：

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

这里的关键是 `inode->fs_private` 转型为 `Ext2CachedInode*`，然后从 `disk_inode` 中直接读取字段。不需要额外的磁盘 I/O——因为 lookup 的时候已经把整个磁盘 inode 读入内存缓存了。st_dev 和 st_rdev 设为 0，因为我们只有一个 AHCI 设备，没有设备号的概念。

## sys_stat 和 sys_fstat

`kernel/syscall/sys_stat.cpp` 包含两个 syscall handler。`sys_stat` 是基于路径的版本：

```cpp
int64_t sys_stat(uint64_t path_virt, uint64_t st_virt, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(st_virt)) {
        return -1;
    }

    // Step 1: Resolve the path (cwd-aware)
    char resolved[cinux::fs::PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) {
        return -1;
    }

    // Step 2-4: VFS resolve, lookup, stat
    const char*            rel_path = nullptr;
    cinux::fs::FileSystem* fs       = cinux::fs::vfs_resolve(resolved, &rel_path);
    if (fs == nullptr) { return -1; }

    cinux::fs::Inode* inode = fs->lookup(rel_path);
    if (inode == nullptr) { return -1; }
    if (inode->ops == nullptr) { return -1; }

    cinux::fs::stat kst;
    int64_t         ret = inode->ops->stat(inode, &kst);
    if (ret < 0) { return -1; }

    // Step 5: Copy to user buffer
    auto* user_st = reinterpret_cast<cinux::fs::stat*>(st_virt);
    memcpy(user_st, &kst, sizeof(cinux::fs::stat));

    return 0;
}
```

流程是 resolve_user_path -> vfs_resolve -> lookup -> stat -> memcpy。先在一个内核栈上的 `kst` 变量里填充数据，然后整体拷贝到用户缓冲区。

`sys_fstat` 是基于文件描述符的版本：

```cpp
int64_t sys_fstat(uint64_t fd, uint64_t st_virt, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    if (!validate_user_ptr(st_virt)) {
        return -1;
    }

    cinux::fs::File* file = cinux::fs::g_global_fd_table().get(static_cast<int>(fd));
    if (file == nullptr || file->inode == nullptr) { return -1; }

    cinux::fs::Inode* inode = file->inode;
    if (inode->ops == nullptr) { return -1; }

    cinux::fs::stat kst;
    int64_t         ret = inode->ops->stat(inode, &kst);
    if (ret < 0) { return -1; }

    auto* user_st = reinterpret_cast<cinux::fs::stat*>(st_virt);
    memcpy(user_st, &kst, sizeof(cinux::fs::stat));

    return 0;
}
```

区别在于获取 inode 的方式不同——fstat 通过全局 FD 表查找打开的文件，拿到 inode 指针后调用 stat 的逻辑完全一样。两者的后半段（"拿到 inode 调 stat，拷贝到用户空间"）是完全重复的代码，可以考虑将来提取成共享的内部函数。

## 小结

这一章我们看了四个新 syscall 的完整实现。chdir 的流程是 resolve -> VFS -> lookup -> 验证目录 -> 更新 CWD；getcwd 直接从 Task 读 CWD；stat 和 fstat 都走 "拿到 inode -> 调 InodeOps::stat -> 拷贝到用户空间" 的路径。ext2 驱动的 stat 实现从缓存的磁盘 inode 中读取字段，零额外 I/O。下一章我们来看路径解析如何被集成到已有的 syscall 中，以及 Shell 命令的实现。
