# F3: 进程与线程

> 为 Cinux 引入信号系统、线程支持（clone/futex）、进程组/会话。
> 是 POSIX 兼容性和多线程应用的基础。

## 实现决策

| 决策 | 选择 |
|------|------|
| 信号范围 | 核心 POSIX 信号 + 抽象接口（方便扩展） |
| 线程模型 | clone + futex（Linux 风格，为 pthread 打基础） |
| 进程组/会话 | F3 包含（为 job control 打基础） |
| 调度器 | 现有 SchedulingClass 已支持插拔，验证 + 小幅增强 |

## Milestone 依赖

```
M1 信号系统 ──→ M2 clone + futex + TLS
       ↓              ↓
M3 进程组/会话    M4 调度器接口验证
```

M1 是 M2 的前置（clone 需要信号 mask 继承）。M3 和 M4 相对独立。

## 文件清单

| 文件 | Milestone | 说明 |
|------|-----------|------|
| [00-signals.md](00-signals.md) | M1 | 核心 POSIX 信号系统 |
| [01-threading.md](01-threading.md) | M2 | clone + futex + TLS |
| [02-process-group.md](02-process-group.md) | M3 | 进程组/会话/Job Control 前置 |
| [03-scheduler.md](03-scheduler.md) | M4 | 调度器接口验证与增强 |

## 关键代码位置

| 模块 | 文件 |
|------|------|
| Task 结构体 | `kernel/proc/process.hpp` — TCB，400+ 行 |
| 进程管理 | `kernel/proc/process.cpp` — fork/execve/waitpid |
| 调度器 | `kernel/proc/scheduler.hpp` — SchedulingClass + RoundRobin |
| 上下文切换 | `kernel/arch/x86_64/context_switch.S` — callee-saved |
| 同步原语 | `kernel/proc/sync.hpp` — Spinlock/Mutex/Semaphore |
| ELF 加载 | `kernel/proc/elf_types.hpp` — 仅静态 |
| 用户态切换 | `kernel/arch/x86_64/usermode.S` — SYSRET |
| PID 分配 | `kernel/proc/pid.hpp` — 1..256 |
| Syscall 表 | `kernel/syscall/syscall_nums.hpp` — 21 个 |

## 验收标准

每个 Milestone：
1. `cmake --build build/` 编译通过
2. QEMU 启动 + 用户程序运行
3. 新增 syscall 有测试
4. 每文件 ≤ 500 行
