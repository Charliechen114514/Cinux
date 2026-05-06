# F8: IPC 扩展

> 四项 IPC 增强：Pipe 调度器阻塞、命名管道、Unix Socket、共享内存。

## 实现决策

全部四项：
1. Pipe 增强（轮询→等待队列阻塞）
2. 命名管道 (FIFO)
3. Unix Domain Socket
4. 共享内存 (shm)

## Milestone 依赖

```
M1 Pipe 增强 ──→ M2 命名管道 (FIFO)
       ↓
M3 Unix Domain Socket
M4 共享内存 (shm) ← F2 mmap
M5 epoll 事件通知（依赖 M3 Unix Socket 的就绪通知）
```

M1 是 M2 前置。M3/M4 相对独立。

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-pipe.md](00-pipe.md) | M1: Pipe 等待队列增强 |
| [01-fifo.md](01-fifo.md) | M2: 命名管道 |
| [02-unix-socket.md](02-unix-socket.md) | M3: Unix Domain Socket |
| [03-shm.md](03-shm.md) | M4: 共享内存 |
| [04-epoll.md](04-epoll.md) | M5: epoll 事件通知 |
