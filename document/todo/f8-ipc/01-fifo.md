# M2: 命名管道 (FIFO)

> 文件系统中的命名管道。mkfifo() 创建，进程通过路径名打开。
> 复用 M1 增强后的 Pipe 实现。

## 任务清单

### T1: FIFO InodeOps

**文件**: `kernel/ipc/fifo.hpp`, `kernel/ipc/fifo.cpp`

```cpp
class FifoOps : public InodeOps {
    // open() 时创建 Pipe 实例
    // read() 委托到 Pipe::read()
    // write() 委托到 Pipe::write()
};
```

- [ ] FIFO InodeOps 实现
- [ ] open() for read → 等待写入端打开
- [ ] open() for write → 等待读取端打开
- [ ] 多个读取端/写入端共享同一个 Pipe

### T2: mkfifo Syscall

- [ ] mkfifo(path, mode) — 在 VFS 创建 FIFO inode
- [ ] 支持 DevFS/tmpfs/ext2 上的 FIFO
- [ ] stat() 显示 FIFO 类型 (S_IFIFO)

## 产出物

- [ ] `kernel/ipc/fifo.hpp` / `.cpp`
- [ ] mkfifo syscall
