# 027-1 Read-through: VFS 核心数据结构 — Inode、File 与 FileSystem

## 概览

本文讲解 Cinux VFS 层最基础的三个头文件——`inode.hpp`、`vfs_filesystem.hpp` 和 `file.hpp`，以及 `file.cpp` 的实现。这四个文件定义了 VFS 的全部核心抽象：Inode 是文件系统对象的统一表示，FileSystem 是所有后端的接口契约，File 是进程视角的打开文件，FDTable 管理文件描述符。它们构成了 VFS 的骨架，后续的挂载点表、系统调用、Ramdisk 适配都建立在这些数据结构之上。

关键设计决策一览：使用函数指针表（InodeOps）替代 C++ 虚函数实现多态；FDTable 使用固定大小数组而非链表或哈希表；File 和 Inode 分离以支持同一文件多次打开。

## 架构图

```
用户态 open("/hello.txt")
        │
        ▼
  sys_open ──► vfs_resolve ──► FileSystem::lookup()
        │                           │
        ▼                           ▼
  FDTable::alloc()          返回 Inode* ──► InodeOps::read()
        │                                        │
        ▼                                        ▼
  返回 fd 编号                           Ramdisk 数据拷贝
```

调用链路：fd → FDTable::get(fd) → File → Inode → InodeOps → 具体文件系统的 read/write/readdir 实现。

## 代码精讲

### InodeType 枚举和 InodeOps 函数指针表

```cpp
enum class InodeType : uint8_t {
    Unknown   = 0,
    Regular   = 1,
    Directory = 2,
};
```

InodeType 用 scoped enum（enum class）定义，底层类型是 uint8_t——这样做的好处是枚举值不会泄漏到外层命名空间，而且大小只有 1 字节。目前只区分普通文件和目录，Unknown 作为兜底值处理异常情况。

```cpp
struct Inode;

struct InodeOps {
    int64_t (*read)(const Inode* inode, uint64_t offset,
                    void* buf, uint64_t count);
    int64_t (*write)(Inode* inode, uint64_t offset,
                     const void* buf, uint64_t count);
    int64_t (*readdir)(const Inode* inode, uint64_t index,
                       char* name, uint64_t name_max);
};
```

注意 read 的第一个参数是 const Inode*（读操作不修改 inode），而 write 是 Inode*（写可能修改 size 等元数据）。InodeOps 需要在 Inode 之前做前置声明，因为函数签名引用了 Inode 类型。每个函数指针都可以为 nullptr——比如普通文件的 ops 不设 readdir，目录的 ops 不设 read。

### Inode 结构体

```cpp
struct Inode {
    uint64_t    ino;          // inode number
    uint64_t    size;         // file size in bytes
    InodeType   type;         // Regular / Directory / Unknown
    InodeOps*   ops;          // operation function table
    void*       fs_private;   // filesystem-specific data
};
```

五个字段的设计是参考了 Linux 的 inode 做了大量简化。ino 是文件系统特定的编号（Ramdisk 用条目索引，ext2 会用磁盘上的 inode 号）。fs_private 是留给后端的"后门"——Ramdisk 用它指向 RamdiskEntry（包含文件名和数据指针），ext2 会用它指向磁盘块地址列表。Inode 的生命周期归后端管理——Ramdisk 的 Inode 嵌入在 RamdiskEntry 里，不需要动态分配。

### FileSystem 抽象基类

```cpp
class FileSystem {
public:
    virtual ~FileSystem() = default;
    virtual bool mount() = 0;
    virtual Inode* lookup(const char* path) = 0;
};
```

整个 VFS 层用到的唯一一个 C++ 虚函数就在这里——FileSystem 的多态接口。mount() 在文件系统被注册到挂载点表时调用，负责初始化后端的数据结构。lookup() 接收的 path 是相对路径（挂载点前缀已被剥离），返回对应的 Inode 指针。我们用了 `= default` 的虚析构，确保通过基类指针 delete 时能正确调用派生类的析构函数。

### OpenFlags 和 File 结构体

```cpp
enum class OpenFlags : uint32_t {
    RDONLY = 0,
    WRONLY = 1,
    RDWR   = 2,
};

static constexpr uint32_t FD_TABLE_SIZE = 256;
static constexpr int FD_NONE = -1;

struct File {
    Inode*    inode;
    uint64_t  offset;
    OpenFlags flags;
};
```

OpenFlags 的值（0/1/2）和 Linux 的 O_RDONLY/O_WRONLY/O_RDWR 完全一致——这不是巧合，而是为了让用户态可以直接传递 flags 值而不需要转换。File 的三个字段中，inode 指向底层的文件系统对象，offset 记录当前读写位置，flags 记录访问模式。这里有一个巧妙之处：对于目录 fd，offset 被复用为目录项的索引——因为对目录做"seek"没有意义，所以 offset 字段在目录场景下可以安全地挪作他用。

### FDTable 类

```cpp
class FDTable {
public:
    FDTable();
    int  alloc(Inode* inode, OpenFlags flags);
    int  close(int fd);
    File* get(int fd) const;
private:
    File* fds_[FD_TABLE_SIZE];
};
```

FDTable 是一个简单但完整的文件描述符管理器。构造函数把 256 个槽位全部置 nullptr。alloc 从 fd=3 开始线性扫描（0-2 保留给 stdin/stdout/stderr），找到第一个空位后 new 一个 File 对象放入。close 做边界检查、双重关闭检查、delete File、置空。get 只做边界检查后返回指针。

```cpp
FDTable::FDTable() {
    for (uint32_t i = 0; i < FD_TABLE_SIZE; ++i) {
        fds_[i] = nullptr;
    }
}

int FDTable::alloc(Inode* inode, OpenFlags flags) {
    for (uint32_t i = FD_FIRST; i < FD_TABLE_SIZE; ++i) {
        if (fds_[i] == nullptr) {
            fds_[i] = new File{inode, 0, flags};
            return static_cast<int>(i);
        }
    }
    return FD_NONE;
}
```

alloc 的实现简单直接——线性扫描找第一个空位。Linux 内核的 alloc_fd 使用了位图加速查找，但我们的 256 槽位线性扫描已经足够快了。每次 alloc 都 new 一个 File 对象，close 时 delete——这意味着频繁 open/close 会有堆分配开销，但对教学 OS 来说完全可接受。

```cpp
int FDTable::close(int fd) {
    if (fd < 0 || fd >= static_cast<int>(FD_TABLE_SIZE)) {
        return -1;
    }
    if (fds_[fd] == nullptr) {
        return -1;
    }
    delete fds_[fd];
    fds_[fd] = nullptr;
    return 0;
}
```

close 的三重保护：越界返回 -1、已关闭返回 -1、正常关闭 delete 后置 nullptr。这种防御式编程在内核里非常重要——一个 double-free 或 use-after-free 在内核态会直接 panic。

## 设计决策

### 决策：函数指针表 vs 虚函数

**问题**: Inode 的多态操作（read/write/readdir）用函数指针表还是 C++ 虚函数？

**本项目的做法**: 使用独立的 InodeOps 结构体存储函数指针。

**备选方案**: 把 read/write/readdir 声明为 Inode 的虚函数，子类 override。

**为什么不选备选方案**: 第一，Inode 的 ops 通常按类别共享——所有 Ramdisk 普通文件共享同一个 ramdisk_file_ops，不需要每个 Inode 一个 vtable。第二，函数指针表可以在编译期静态定义，零运行时开销。第三，这种模式更接近 Linux 的 inode_operations 设计，便于读者理解真实内核的做法。

**如果要扩展**: 将来如果要支持更多操作（如 truncate、chmod、stat），只需在 InodeOps 里加函数指针字段，不需要修改 Inode 结构体本身。

## 扩展方向

- 为 FDTable 添加 dup2() 语义——复制一个 fd 到指定编号（难度：⭐）
- 实现 O_APPEND 标志——write 时自动 seek 到文件末尾（难度：⭐）
- 将 FDTable 改为 per-process（嵌入 Process 结构体），实现真正的进程隔离（难度：⭐⭐）
- 添加引用计数到 Inode，支持多个 File 共享同一个 Inode 并正确管理生命周期（难度：⭐⭐）
- 使用位图替代线性扫描加速 FDTable::alloc（难度：⭐⭐⭐）

## 参考资料

- OSDev Wiki: [VFS](https://wiki.osdev.org/VFS) — Mount Point List 模型和 vnode/open-file 分离
- Linux Kernel: [Overview of the Linux Virtual File System](https://docs.kernel.org/filesystems/vfs.html) — inode/file/superblock/dentry 四大对象
- xv6: [fs.c](https://github.com/mit-pdos/xv6-public/blob/master/fs.c) — 全局 ftable + per-process ofile[] 数组
