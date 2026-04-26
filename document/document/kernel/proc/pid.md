# PID 管理

> 里程碑: `034_process_fork_exec` (子迭代 1)

## 功能概述

进程 ID 分配器，从 1 开始递增分配，上限 `PID_MAX=256`，支持释放和复用。

## API (`kernel/proc/pid.hpp/cpp`)
- `PidAllocator::alloc()` — 分配下一个可用 PID
- `PidAllocator::free(pid)` — 释放 PID，允许后续复用
- `PID_MAX = 256`

## 与 TCB 的关系
- `Task` 结构体的 `pid_` 字段由 `PidAllocator::alloc()` 填充
- `ppid_` 继承自父进程

## 系统调用
- `SYS_getpid` — 返回当前进程 PID
- `SYS_getppid` — 返回父进程 PID

## 源码位置
- `kernel/proc/pid.hpp/cpp`
