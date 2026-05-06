# M4: tmpfs / ramfs

> 内存文件系统。tmpfs 用于共享内存和临时文件，
> ramfs 是更简单的纯内存 FS。

## 目标

实现内存驻留文件系统，为 POSIX shm 和临时文件提供基础。

## 任务清单

### T1: ramfs 基础

**文件**: `kernel/fs/ramfs.hpp`, `kernel/fs/ramfs.cpp`

```cpp
class RamFS : public FileSystem {
public:
    bool mount() override;
    Inode* lookup(const char* path) override;

private:
    struct RamNode {
        char name[256];
        InodeType type;
        uint8_t* data;      // 文件内容（堆分配）
        size_t   data_size;
        size_t   capacity;
        RamNode* children;  // 目录的子节点链表
        RamNode* next;      // 同级链表
        Inode    inode;
    };

    RamNode* root_;
};
```

- [ ] RamFS 继承 FileSystem
- [ ] root 目录节点
- [ ] 文件：动态增长的 data 缓冲区
- [ ] 目录：子节点链表
- [ ] InodeOps：read/write/create/mkdir/unlink/readdir

### T2: tmpfs（基于 ramfs）

```cpp
class TmpFS : public RamFS {
    // tmpfs = ramfs + 容量限制 + swap 支持（后续）
    size_t max_size_;
    size_t used_size_;
};
```

- [ ] 容量限制（默认 50% 物理内存）
- [ ] 写入时检查剩余空间
- [ ] 统计：df 命令可以看到 tmpfs 使用量

### T3: mount 集成

- [ ] `mount -t tmpfs tmpfs /tmp` — 临时文件
- [ ] `mount -t tmpfs shm /dev/shm` — POSIX 共享内存
- [ ] 启动时自动挂载 /tmp 和 /dev/shm

### T4: 单元测试

- [ ] 创建文件 + 写入 + 读取验证
- [ ] 创建子目录
- [ ] 容量限制生效
- [ ] unmount 后内存释放

## 产出物

- [ ] `kernel/fs/ramfs.hpp` / `.cpp` — ramfs + tmpfs
- [ ] /tmp 和 /dev/shm 自动挂载
