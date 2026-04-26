# Pipe 管道

> 里程碑: `031_gui_native_app` (Phase 1-2)

## 功能概述

4KB 环形缓冲区单向管道，通过 VFS InodeOps 适配，实现进程间字节流通信。`sys_pipe` 系统调用创建一对 fd (读端/写端)。

## Pipe 类 (`kernel/ipc/pipe.hpp/cpp`)
- 4KB 环形缓冲区
- `write(data, len)` — 写入 (满时 spin-wait 阻塞)
- `read(buf, len)` — 读取
- `close()` — 关闭管道
- Spinlock 保护内部状态

## VFS 适配 (`kernel/ipc/pipe_ops.hpp/cpp`)
- `PipeReadOps` — InodeOps 读方向适配
- `PipeWriteOps` — InodeOps 写方向适配
- 通过 `sys_read/sys_write` 自然支持 pipe fd

## 系统调用
- `sys_pipe(fd_pair)` — 创建 pipe，返回 [read_fd, write_fd]
- FDTable 扩展: `set()` 方法支持手动设置 fd 0/1

## 集成
- Terminal 窗口通过 pipe 连接 shell: stdin → pipe → shell stdin, shell stdout → pipe → Terminal

## 源码位置
- `kernel/ipc/pipe.hpp/cpp` — Pipe 实现
- `kernel/ipc/pipe_ops.hpp/cpp` — VFS InodeOps 适配
