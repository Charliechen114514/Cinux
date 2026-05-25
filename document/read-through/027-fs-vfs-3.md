---
title: 027-fs-vfs-3 · VFS 虚拟文件系统
---

# 027-3 Read-through: Ramdisk VFS 适配与系统调用 — open/close/read/write/getdents

## 概览

本文讲解 Ramdisk 如何适配 VFS 框架（继承 FileSystem、提供 InodeOps 函数指针表）以及五个系统调用的完整实现。这是 027 tag 代码量最大、最核心的部分——把之前搭建好的数据结构和挂载点表真正串起来，让 open/read/write/close/getdents 全链路跑通。

关键设计决策：Ramdisk 使用预分配 Inode（嵌入 RamdiskEntry）而非动态创建；sys_read/sys_write 对 fd=0/1 保留旧路径，其他 fd 走 VFS；地址校验从简单的上限检查改为 canonical address 规则。

## 架构图

```
sys_open("/hello.txt", O_RDONLY)
  ├── canonical address 校验
  ├── vfs_resolve("/hello.txt") → Ramdisk, rel_path="hello.txt"
  ├── Ramdisk::lookup("hello.txt") → &entries_[0].inode
  ├── OpenFlags 映射: flags=0 → RDONLY
  └── FDTable::alloc(inode, RDONLY) → fd=3

sys_read(3, buf, 256)
  ├── fd != 0, 走 VFS 路径
  ├── FDTable::get(3) → File{inode, offset=0, RDONLY}
  ├── inode->ops->read(inode, 0, buf, 256)
  │   └── ramdisk_read: memcpy from archive data
  └── file->offset += bytes_read

sys_close(3)
  └── FDTable::close(3): delete File, slot=nullptr
```

## 代码精讲

### Ramdisk 继承 FileSystem

Ramdisk 的头文件变化不小——类声明从 `class Ramdisk` 变成 `class Ramdisk : public FileSystem`。mount() 的返回类型从 `uint32_t` 变成 `bool`（与 FileSystem 基类接口一致），新增 `Inode* lookup(const char* path) override`。

RamdiskEntry 结构体也从轻量变得厚重：

```cpp
struct RamdiskEntry {
    char        name[RAMDISK_NAME_MAX];  // 拷贝出来的文件名（不再是指针）
    uint64_t    size;
    const void* data;
    Inode       inode;                   // 预分配的 Inode
};
```

name 字段从 `const char*` 变成 `char[100]` 是因为 ustar header 的生命周期不能保证——header 指针只在 mount() 遍历期间有效，之后可能被覆盖。把文件名拷贝出来是更安全的做法。内嵌 Inode 意味着不需要为每个文件动态分配 inode——Ramdisk 管理的文件数量有限（最多 64 个），预分配的内存开销完全可以接受。

新增的 RamdiskDirContext 结构是为了支持 readdir：

```cpp
struct RamdiskDirContext {
    const RamdiskEntry* entries;
    uint32_t            count;
};
```

之所以需要这个结构，是因为 readdir 的函数指针指向 ramdisk.cpp 匿名命名空间里的实现函数，不能直接访问 Ramdisk 类的私有成员。通过 fs_private 指向 RamdiskDirContext，readdir 就能拿到条目表和数量了。

### Ramdisk 的 InodeOps 实现

在实际代码中，Ramdisk 提供两个静态 InodeOps 实例——ramdisk_file_ops 和 ramdisk_dir_ops——作为函数指针表。每个实例的函数指针指向匿名命名空间里的实现函数，不支持的操作（如普通文件的 write）保持 nullptr。所有文件的 inode 共享同一个 ops 指针。

```cpp
// 匿名命名空间中的实现函数
static int64_t ramdisk_read(const Inode* inode, uint64_t offset,
                             void* buf, uint64_t count) {
    if (inode == nullptr || inode->fs_private == nullptr || buf == nullptr)
        return -1;
    auto* entry = static_cast<const RamdiskEntry*>(inode->fs_private);
    if (offset >= entry->size) return 0;
    uint64_t available = entry->size - offset;
    uint64_t to_read = (count < available) ? count : available;
    const auto* src = static_cast<const uint8_t*>(entry->data) + offset;
    memcpy(buf, src, to_read);
    return static_cast<int64_t>(to_read);
}

// 静态 InodeOps 实例——函数指针表
static InodeOps ramdisk_file_ops = {
    .read    = ramdisk_read,
    .write   = nullptr,    // 只读文件系统，write 不支持
    .readdir = nullptr,    // 普通文件不支持 readdir
    .create  = nullptr,
    .mkdir   = nullptr,
    .unlink  = nullptr,
    .stat    = nullptr,
};
```

ramdisk_read 的逻辑非常直接——从 fs_private 拿到 RamdiskEntry，计算可读字节数（min(count, size-offset)），从归档数据区 memcpy 到用户缓冲区。offset >= size 时返回 0（EOF），这是 Unix read() 的标准语义。这里没有做任何页对齐或块对齐的处理——因为 Ramdisk 的数据就在内存里，不需要像磁盘文件系统那样做扇区级别的读写。

ramdisk_file_ops 的 write 指针为 nullptr——这是只读文件系统，调用方在调用前需要检查函数指针是否为空。

ramdisk_readdir 用 index 分三种情况：0 返回 "."，1 返回 ".."，>=2 减去 2 后从条目表取真实文件名。越界时返回 0（目录遍历完毕）。这种 index-based 的设计比 Linux 的 linux_dirent 结构简单得多——我们不需要计算 d_off 或 d_reclen，只需要返回一个名称字符串。

### sys_open — 路径解析与 VFS 流程

```cpp
int64_t sys_open(uint64_t path_virt, uint64_t flags, ...) {
    // Step 1: Resolve the path (cwd-aware, includes canonical address check)
    char resolved[PATH_MAX];
    if (!resolve_user_path(path_virt, resolved)) return -1;

    // Step 2: vfs_resolve
    const char* rel_path = nullptr;
    FileSystem* fs = vfs_resolve(resolved, &rel_path);
    if (fs == nullptr) return -1;

    // Step 3: lookup
    Inode* inode = fs->lookup(rel_path);
    if (inode == nullptr) return -1;

    // Step 4: alloc fd
    OpenFlags open_flags;
    switch (flags) {
        case 0: open_flags = OpenFlags::RDONLY; break;
        case 1: open_flags = OpenFlags::WRONLY; break;
        case 2: open_flags = OpenFlags::RDWR;   break;
        default: open_flags = OpenFlags::RDONLY; break;
    }
    int fd = current_fd_table().alloc(inode, open_flags);
    return (fd == FD_NONE) ? -1 : fd;
}
```

sys_open 的第一步调用 resolve_user_path()，这个函数在 path_util.hpp 中定义，内部做了 canonical address 校验和 cwd（当前工作目录）感知的路径解析。canonical address 校验是 x86_64 的重要安全措施：x86_64 使用 48 位线性地址空间，bit 47 是符号位——如果 bit 47 是 0，那 bits 48-63 必须全为 0（用户空间）；如果 bit 47 是 1，那 bits 48-63 必须全为 1（内核空间）。非规范地址访问会触发 #GP。resolve_user_path 把这些检查封装了起来，各个 syscall handler 不需要重复。

四步流程的每一步都可能失败：路径解析失败 → resolve 没找到挂载点 → lookup 没找到文件 → FD 表满了。每一步失败都返回 -1，这是 Unix 系统调用的标准错误返回值。注意这里使用的是 current_fd_table() 而非 g_global_fd_table()——前者支持 per-process FDTable。

### sys_read — VFS 优先的分发策略

sys_read 的实际实现采用"VFS 优先"的策略：先查 FDTable，如果 fd 对应的 File 存在就走 VFS 路径；如果 FDTable 里没有这个 fd，再检查是不是 fd=0 的键盘路径。这种设计比"fd==0 → 键盘, fd!=0 → VFS"的简单分支更灵活——它允许 fd=0 被重新分配为 pipe 或其他 VFS 后端（比如 shell 重定向场景）。

```cpp
// Check FDTable first -- if the fd has a valid VFS entry (e.g. pipe),
// use the VFS read path regardless of fd number.
FDTable& tbl = current_fd_table();
File* file = tbl.get(static_cast<int>(fd));
if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
    auto* buf = reinterpret_cast<void*>(buf_virt);
    auto g = file->offset_lock_.guard();
    (void)g;
    int64_t result = file->inode->ops->read(file->inode, file->offset, buf, count);
    if (result > 0) {
        file->offset += static_cast<uint64_t>(result);
    }
    return result;
}

// fd=0 (stdin): legacy keyboard read path when no VFS entry is present
if (fd == 0) { /* keyboard reading logic */ }
```

这里做了一个三层 nullptr 检查——file、inode、ops，任何一层为空都跳过 VFS 路径。这看起来啰嗦但在内核里非常重要：一个空指针解引用在用户态只是 segfault，在内核态会直接 panic。注意 offset 的更新受 offset_lock_ 自旋锁保护。

sys_write 的结构类似：也是 VFS 优先，先查 FDTable 走 VFS 写路径，如果 FDTable 里没有再检查 fd=1 的 kprintf 路径。这种 VFS-first 的设计让 pipe（fd=1 被重定向到 pipe）能正确工作。

### sys_getdents — 复用 offset 做索引

```cpp
int64_t sys_getdents(uint64_t fd, uint64_t buf_virt, uint64_t count, ...) {
    // canonical address 校验...
    File* file = current_fd_table().get(static_cast<int>(fd));
    // nullptr checks...
    if (file->inode->ops->readdir == nullptr) return -1;

    auto* name_buf = reinterpret_cast<char*>(buf_virt);
    {
        auto g = file->offset_lock_.guard();
        (void)g;
        int64_t result = file->inode->ops->readdir(
            file->inode, file->offset, name_buf, count);

        if (result == 1) {
            file->offset++;
            uint64_t len = 0;
            while (len < count && name_buf[len] != '\0') { ++len; }
            return static_cast<int64_t>(len);
        }
        return result;
    }
}
```

file->offset 在这里被用作目录项的索引——因为对目录 fd 没有"文件偏移量"的概念，所以这个字段可以安全地挪用。每次 readdir 成功后 offset++，下次调用就读下一个目录项。返回值是目录项名称的长度（不含 null 终止符），0 表示目录遍历完毕，-1 表示错误。注意 offset 的读写受 offset_lock_ 自旋锁保护。

## 设计决策

### 决策：预分配 Inode vs 动态分配

**问题**: Ramdisk 的 Inode 怎么管理？

**本项目的做法**: 每个 RamdiskEntry 内嵌一个 Inode，mount 时预分配。

**备选方案**: lookup 时动态 new Inode 并填入信息。

**为什么不选备选方案**: Ramdisk 管理的文件数量已知且有限（mount 时就确定），预分配避免了运行时的堆开销和生命周期管理的复杂性。而且 Ramdisk 的 Inode 数据直接来自 ustar header 的解析结果，不需要延迟创建。

### 决策：canonical address vs 简单上限

**问题**: 用户态地址校验用什么规则？

**本项目的做法**: x86_64 canonical address 规则（bit 47 符号扩展到 bits 48-63），封装在 validate_user_ptr() 和 resolve_user_path() 中复用。

**备选方案**: 简单的 `addr >= 0x800000000000` 上限检查。

**为什么不选备选方案**: 简单上限无法正确处理内核空间的高半部分地址（如 0xFFFF800000000000），这些地址在 bit 47 以上全为 1，不是"超过上限"而是另一段合法地址空间。canonical address 规则是 x86_64 硬件实际使用的地址合法性检查，更准确。

## 参考资料

- Intel SDM Vol.1 §3.3.7.1: Canonical Address — 48 位线性地址空间的规范地址定义
- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — open/read/write/close 系统调用的 VFS 流程
- xv6: [file.c](https://github.com/mit-pdos/xv6-public/blob/master/file.c) — 全局 ftable 和 filealloc/fileclose
