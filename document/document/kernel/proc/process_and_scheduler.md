# 进程与调度器

> 里程碑: `019_proc_context` `020_proc_scheduler`

## 功能概述

内核线程管理 + Round-Robin 时间片调度器。支持上下文切换、优先级、阻塞/唤醒、idle 任务，由 PIT 时钟中断驱动。

## 任务控制块 (`kernel/proc/process.hpp`)

### 状态
```cpp
enum class TaskState { Running, Ready, Blocked, Zombie, Dead };
```

### CPU 上下文
```cpp
struct alignas(16) CpuContext {
    uint64_t r15, r14, r13, r12, rbp, rbx, rsp, rip;
};
```

### 任务结构
```cpp
struct Task {
    CpuContext ctx;
    TaskState state;
    uint32_t tid;
    int priority;
    uint64_t kernel_stack, kernel_stack_top;
    AddressSpace* addr_space;
    // 扩展字段 (034):
    uint32_t pid_, ppid_;
    int exit_status_;
    Task* children_;
    Task* parent_;
    // ...
};
```

## 任务创建
- `TaskBuilder::set_entry/set_name/set_priority/set_addr_space/build`
- `kmalloc` 分配 TCB，`PMM::alloc_pages(4)` 分配 16KB 内核栈
- 栈底写 magic `0xDEADC0DE` 检测栈溢出

## 上下文切换 (`kernel/arch/x86_64/context_switch.S`)
- `context_switch(CpuContext* from, CpuContext* to)`
- 保存 callee-saved (r15-r12, rbp, rbx, rsp) + rip
- 恢复 to 的对应字段

## 调度器 (`kernel/proc/scheduler.hpp/cpp`)

### API
- `Scheduler::init()` — 初始化调度器
- `add_task(Task*)` — 加入就绪队列
- `remove_task(Task*)` — 移除
- `tick()` — IRQ0 调用，每 N tick 触发 `schedule()`
- `yield()` — 主动让出
- `block(Task*, reason)` — 阻塞任务
- `unblock(Task*)` — 唤醒任务
- `current()` — 获取当前任务

### 调度策略
- 就绪队列: 环形数组 Round-Robin
- `idle_task`: 只执行 `hlt`
- `schedule()`: 先发 EOI 再 `context_switch`
- 切换时更新 `TSS.RSP0` 为新 task 内核栈顶

## Per-CPU 数据 (`kernel/proc/per_cpu.hpp`)
```cpp
struct PerCPU {
    Task* current;
    uint64_t kernel_stack;
};
```
单核使用静态全局。

## 线程安全
- 调度器运行队列: Spinlock 保护 (TIER 0)
- `tick_count_` / `current_slice_`: 原子操作 (TIER 1)

## 源码位置
- `kernel/proc/process.hpp` — Task/TaskBuilder 定义
- `kernel/proc/scheduler.hpp/cpp` — 调度器
- `kernel/proc/per_cpu.hpp` — Per-CPU 数据
- `kernel/arch/x86_64/context_switch.S` — 上下文切换汇编
