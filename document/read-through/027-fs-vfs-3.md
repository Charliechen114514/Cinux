# 027-3 Read-through: Ramdisk VFS 适配与系统调用 — open/close/read/write/getdents

## 概览

本文讲解 Ramdisk 如何适配 VFS 框架（继承 FileSystem、实现 InodeOps）以及五个系统调用的完整实现。这是 027 tag 代码量最大、最核心的部分——把之前搭建好的数据结构和挂载点表真正串起来，让 open/read/write/close/getdents 全链路跑通。

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

之所以需要这个结构，是因为 readdir 的 InodeOps 函数在 ramdisk.cpp 的匿名命名空间里，不能直接访问 Ramdisk 类的私有成员。通过 fs_private 指向 RamdiskDirContext，readdir 就能拿到条目表和数量了。

### Ramdisk 的三个 InodeOps

```cpp
int64_t ramdisk_read(const Inode* inode, uint64_t offset,
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
```

ramdisk_read 的逻辑非常直接——从 fs_private 拿到 RamdiskEntry，计算可读字节数（min(count, size-offset)），从归档数据区 memcpy 到用户缓冲区。offset >= size 时返回 0（EOF），这是 Unix read() 的标准语义。这里没有做任何页对齐或块对齐的处理——因为 Ramdisk 的数据就在内存里，不需要像磁盘文件系统那样做扇区级别的读写。

ramdisk_write 直接返回 -1——这是只读文件系统，任何写操作都是错误。

ramdisk_readdir 用 index 分三种情况：0 返回 "."，1 返回 ".."，>=2 减去 2 后从条目表取真实文件名。越界时返回 0（目录遍历完毕）。这种 index-based 的设计比 Linux 的 linux_dirent 结构简单得多——我们不需要计算 d_off 或 d_reclen，只需要返回一个名称字符串。

### sys_open — 四步流程

```cpp
int64_t sys_open(uint64_t path_virt, uint64_t flags, ...) {
    // Step 1: canonical address 校验
    if (path_virt == 0) return -1;
    uint64_t bit47 = (path_virt >> 47) & 1;
    uint64_t upper = path_virt >> 48;
    if (bit47 == 0 && upper != 0) return -1;
    if (bit47 == 1 && upper != 0xFFFF) return -1;
    if (path[0] == '\0') return -1;

    // Step 2: vfs_resolve
    const char* rel_path = nullptr;
    FileSystem* fs = vfs_resolve(path, &rel_path);
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
    int fd = g_global_fd_table().alloc(inode, open_flags);
    return (fd == FD_NONE) ? -1 : fd;
}
```

canonical address 校验是 x86_64 的重要安全措施。x86_64 使用 48 位线性地址空间，bit 47 是符号位——如果 bit 47 是 0，那 bits 48-63 必须全为 0（用户空间）；如果 bit 47 是 1，那 bits 48-63 必须全为 1（内核空间）。非规范地址访问会触发 #GP。我们在 syscall handler 里提前拒绝非规范地址，避免后续访问时产生不可控的异常。

四步流程的每一步都可能失败：地址不合法 → resolve 没找到挂载点 → lookup 没找到文件 → FD 表满了。每一步失败都返回 -1，这是 Unix 系统调用的标准错误返回值。

### sys_read — 双路径分发

sys_read 的结构变成了一个 if-else 分支：fd=0 走键盘读取的旧路径（保留原有逻辑完全不变），fd!=0 走 VFS 新路径。VFS 路径的逻辑是：从 FDTable 拿 File → 拿 Inode → 拿 ops → 调用 ops->read → 更新 offset。

```cpp
// VFS read path (fd != 0)
File* file = g_global_fd_table().get(static_cast<int>(fd));
if (file == nullptr || file->inode == nullptr || file->inode->ops == nullptr)
    return -1;
if (file->inode->ops->read == nullptr) return -1;

auto* buf = reinterpret_cast<void*>(buf_virt);
int64_t result = file->inode->ops->read(file->inode, file->offset, buf, count);
if (result > 0) {
    file->offset += static_cast<uint64_t>(result);
}
return result;
```

这里做了一个四层 nullptr 检查——file、inode、ops、ops->read，任何一层为空都返回 -1。这看起来啰嗦但在内核里非常重要：一个空指针解引用在用户态只是 segfault，在内核态会直接 panic。

sys_write 的结构类似：fd=1 走 kprintf 输出的旧路径，fd!=1 走 VFS 写路径。

### sys_getdents — 复用 offset 做索引

```cpp
int64_t sys_getdents(uint64_t fd, uint64_t buf_virt, uint64_t count, ...) {
    // canonical address 校验...
    File* file = g_global_fd_table().get(static_cast<int>(fd));
    // nullptr checks...
    if (file->inode->ops->readdir == nullptr) return -1;

    auto* name_buf = reinterpret_cast<char*>(buf_virt);
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
```

file->offset 在这里被用作目录项的索引——因为对目录 fd 没有"文件偏移量"的概念，所以这个字段可以安全地挪用。每次 readdir 成功后 offset++，下次调用就读下一个目录项。返回值是目录项名称的长度（不含 null 终止符），0 表示目录遍历完毕，-1 表示错误。

## 设计决策

### 决策：预分配 Inode vs 动态分配

**问题**: Ramdisk 的 Inode 怎么管理？

**本项目的做法**: 每个 RamdiskEntry 内嵌一个 Inode，mount 时预分配。

**备选方案**: lookup 时动态 new Inode 并填入信息。

**为什么不选备选方案**: Ramdisk 管理的文件数量已知且有限（mount 时就确定），预分配避免了运行时的堆开销和生命周期管理的复杂性。而且 Ramdisk 的 Inode 数据直接来自 ustar header 的解析结果，不需要延迟创建。

### 决策：canonical address vs 简单上限

**问题**: 用户态地址校验用什么规则？

**本项目的做法**: x86_64 canonical address 规则（bit 47 符号扩展到 bits 48-63）。

**备选方案**: 简单的 `addr >= 0x800000000000` 上限检查。

**为什么不选备选方案**: 简单上限无法正确处理内核空间的高半部分地址（如 0xFFFF800000000000），这些地址在 bit 47 以上全为 1，不是"超过上限"而是另一段合法地址空间。canonical address 规则是 x86_64 硬件实际使用的地址合法性检查，更准确。

## 参考资料

- Intel SDM Vol.1 §3.3.7.1: Canonical Address — 48 位线性地址空间的规范地址定义
- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — open/read/write/close 系统调用的 VFS 流程
- xv6: [file.c](https://github.com/mit-pdos/xv6-public/blob/master/file.c) — 全局 ftable 和 filealloc/fileclose
