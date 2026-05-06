# M4: 共享内存 (shm)

> POSIX 共享内存。基于 F2 mmap MAP_SHARED 实现。
> 进程间共享物理页。

## 任务清单

### T1: shm_open / shm_unlink

**文件**: `kernel/ipc/shm.hpp`, `kernel/ipc/shm.cpp`

```cpp
// shm 在 tmpfs (/dev/shm/) 上创建特殊文件
// mmap MAP_SHARED 映射该文件 → 多进程共享物理页

class SharedMemory {
public:
    // 创建共享内存对象
    static Inode* create(const char* name, size_t size);

    // 打开已存在的共享内存
    static Inode* open(const char* name);

    // 删除
    static int unlink(const char* name);
};
```

- [ ] shm_open() — 在 /dev/shm/ 创建 tmpfs 文件
- [ ] shm_unlink() — 删除
- [ ] mmap MAP_SHARED 映射 → 多进程看到同一物理页
- [ ] 引用计数：最后一个 munmap 时释放物理页

### T2: MAP_SHARED 页面处理

- [ ] 写入不触发 CoW（与 MAP_PRIVATE 区别）
- [ ] 物理页通过 Page Cache 共享
- [ ] 多进程映射同一 inode + offset → 同一物理页

### T3: Syscall

- [ ] shm_open() — 通过 VFS open 实现
- [ ] shm_unlink() — 通过 VFS unlink 实现
- [ ] 不需要新 syscall（复用 open/mmap/unlink）

## 产出物

- [ ] `kernel/ipc/shm.hpp` / `.cpp`
- [ ] /dev/shm/ tmpfs 挂载
- [ ] MAP_SHARED 共享物理页
