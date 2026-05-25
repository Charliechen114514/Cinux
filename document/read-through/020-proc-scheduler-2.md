---
title: 020-proc-scheduler-2 · 进程调度
---

# 020-2 通读：scheduler.cpp 抢占式调度核心——tick/schedule/block/unblock 完整实现

## 概览

本文是 tag `020_proc_scheduler` 三篇通读教程的第二篇，聚焦 `scheduler.cpp` 中所有新增函数的完整实现。Tag 019 的调度器只支持协作式调度——线程必须主动调用 `yield()` 才会切换。Tag 020 把它改造成了抢占式调度器：定时器中断驱动 `tick()`，时间片耗尽时自动调用 `schedule()`，任务可以被 `block()` 挂起或被 `unblock()` 唤醒。同时新增了 idle 任务作为 CPU 空闲时的兜底，以及 `remove_task()` 用于从运行队列中移除已完成的任务。我们从 scheduler.hpp 的新增声明开始，然后逐一分析每个函数的实现。

## 架构图

```
    抢占式调度核心流程：
    ┌────────────────────────────────────────────────────────────────┐
    │  IRQ0 (每 10ms)                                                │
    │    └── PIT::irq0_handler()                                     │
    │          ├── tick_count_++                                      │
    │          ├── PIC::send_eoi(0)                                  │
    │          └── Scheduler::tick()                                 │
    │                ├── current_slice_++                             │
    │                └── if (slice >= 2) → schedule()                │
    │                      ├── prev→Ready, pick_next()→next          │
    │                      ├── g_per_cpu.current = next              │
    │                      ├── GDT::tss_set_rsp0(next→stack)        │
    │                      └── context_switch(&prev→ctx, &next→ctx) │
    │                                                                 │
    │  block/unblock 流程（为 Mutex/Semaphore 预留）：              │
    │    block(task, "waiting for mutex")                            │
    │      ├── task→state = Blocked                                  │
    │      ├── dequeue(task)                                         │
    │      └── if (task == current_) → schedule()                    │
    │    unblock(task)                                                │
    │      ├── task→state = Ready                                    │
    │      └── enqueue(task)                                         │
    └────────────────────────────────────────────────────────────────┘
```

## 代码精讲

### scheduler.hpp 新增声明

在 scheduler.hpp 中，`Scheduler` 类新增了以下成员：

```cpp
class Scheduler {
public:
    static constexpr int DEFAULT_TIME_SLICE = 2;

    static void remove_task(Task* task);
    static bool is_initialized();

    static void tick();
    static void schedule();
    static void block(Task* task, const char* reason);
    static void unblock(Task* task);

private:
    static void idle_entry();
    static Task* idle_task_;
    static bool initialized_;
    static int tick_count_;
    static int current_slice_;
};
```

`DEFAULT_TIME_SLICE = 2` 意味着每个线程在耗尽 2 个 tick（即 20ms，PIT 配置为 100Hz）后被强制切换。这个值经过了一番调试才确定——最初设为 10（100ms），在 QEMU TCG 模式下线程在自己的时间片内就跑完了。`idle_task_` 指向一个永不退出、只执行 `hlt` 的特殊任务，`initialized_` 是标志位防止启动阶段误触发调度。

### idle_entry 与 idle 任务创建

```cpp
void Scheduler::idle_entry() {
    while (true) {
        __asm__ volatile("hlt");
    }
}
```

idle 任务是系统中最特殊的一个线程——它唯一的职责就是在没有其他任务可运行时让 CPU 进入低功耗状态。`hlt` 指令让 CPU 停止执行直到下一个中断到来，在 QEMU 中能显著降低宿主机的 CPU 占用。idle 任务被创建后设为 `Ready` 状态，但不会被加入 RoundRobin 的运行队列——它只在所有普通任务都阻塞或退出时才被调度器手动选中。

```cpp
void Scheduler::init() {
    class_count_ = 0;
    current_     = nullptr;
    idle_task_   = nullptr;
    tick_count_  = 0;
    current_slice_ = 0;
    register_class(&default_rr_);

    idle_task_ = TaskBuilder()
        .set_entry(idle_entry)
        .set_name("idle")
        .set_priority(255)
        .build();

    if (idle_task_ != nullptr) {
        idle_task_->state = TaskState::Ready;
        cinux::lib::kprintf("[SCHED] Idle task created tid=%u\n", idle_task_->tid);
    }

    initialized_ = true;
    cinux::lib::kprintf("[SCHED] Scheduler initialised with %s class\n",
                        default_rr_.name());
}
```

`init()` 在原有基础上做了三件事：重置新增的静态变量、创建 idle 任务、设置 `initialized_ = true`。idle 任务的优先级设为 255，虽然当前的 RoundRobin 不使用优先级，但这为未来预留了语义。注意 idle 任务创建后没有调用 `add_task()`，所以它不会进入 RoundRobin 队列——只在所有普通任务都阻塞或退出时才被手动选中。

### tick()——定时器中断的入口

```cpp
void Scheduler::tick() {
    if (!initialized_ || current_ == nullptr) {
        return;
    }

    tick_count_++;
    current_slice_++;

    if (current_slice_ >= DEFAULT_TIME_SLICE) {
        current_slice_ = 0;
        schedule();
    }
}
```

`tick()` 是抢占式调度的触发点，由 PIT 的 IRQ0 handler 在每个定时器中断中调用。前两行是防御性检查——在调度器初始化完成之前、或者在还没有运行任务时，直接返回。然后递增全局 tick 计数器和当前任务的时间片计数器。当 `current_slice_` 达到 `DEFAULT_TIME_SLICE`（2）时，重置计数器并调用 `schedule()` 执行上下文切换。

值得注意的是，`tick()` 是在中断上下文中被调用的。Intel SDM Vol. 3A Section 6.12.1.3 指出，通过中断门进入 ISR 时 CPU 自动清除 IF 标志，所以 `schedule()` 在执行时中断是关闭的——`context_switch` 的执行不会被另一个定时器中断打断，调度本身是原子的。

### schedule()——核心调度函数

```cpp
void Scheduler::schedule() {
    if (current_ == nullptr) {
        return;
    }

    Task* prev = current_;

    if (prev->state == TaskState::Running) {
        prev->state = TaskState::Ready;
    }

    Task* next = default_rr_.pick_next();
```

`schedule()` 是整个抢占式调度的核心。首先保存当前任务指针，然后把它的状态从 `Running` 改回 `Ready`（注意只有 Running 状态才会改，如果任务是 Blocked 或 Dead 就保持原状）。接着通过 `pick_next()` 从 RoundRobin 队列中取出下一个任务。

```cpp
    if (next == nullptr || next == prev) {
        if (prev->state != TaskState::Blocked && prev->state != TaskState::Dead) {
            prev->state = TaskState::Running;
            return;
        }

        if (idle_task_ != nullptr && idle_task_ != prev) {
            next = idle_task_;
        } else {
            return;
        }
    }
```

这段分支逻辑处理了几种边界情况。如果 `pick_next()` 返回空（队列为空）或返回的还是当前任务（队列中只有一个任务且正在运行），我们需要检查当前任务是否还能继续执行。如果当前任务的状态既不是 Blocked 也不是 Dead，说明它只是被抢占了一下，恢复为 Running 直接返回即可。但如果当前任务已经被阻塞或退出了，就必须切到 idle 任务，否则就没人可执行了。

```cpp
    current_          = next;
    g_per_cpu.current = next;
    current_slice_    = 0;

    if (next != idle_task_) {
        cinux::arch::GDT::tss_set_rsp0(next->kernel_stack_top);
    }

    context_switch(&prev->ctx, &next->ctx);
}
```

确定好 `next` 之后，更新所有全局状态：`current_`、`g_per_cpu.current`、重置时间片计数器。如果切到的不是 idle 任务，还需要更新 TSS.RSP0 和 PerCPU 的 syscall 栈。idle 任务不需要更新 TSS.RSP0，因为它永远不会在用户态运行。最后调用 `context_switch` 完成寄存器切换。

### block() 与 unblock()——任务阻塞与唤醒

```cpp
void Scheduler::block(Task* task, const char* reason) {
    if (task == nullptr) {
        return;
    }

    task->state = TaskState::Blocked;
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }

    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' blocked: %s\n",
                        task->tid, task->name, reason ? reason : "unknown");

    if (task == current_) {
        schedule();
    }
}
```

`block()` 把任务状态设为 `Blocked` 并从运行队列中移除。`reason` 参数是给调试日志用的。最关键的是最后那个判断：如果阻塞的是当前正在运行的任务，必须立即调用 `schedule()` 让出 CPU。否则当前任务的状态是 Blocked 但还在执行——这显然是矛盾的。

```cpp
void Scheduler::unblock(Task* task) {
    if (task == nullptr) {
        return;
    }

    task->state = TaskState::Ready;
    if (task->sched_class == nullptr) {
        task->sched_class = &default_rr_;
    }
    task->sched_class->enqueue(task);

    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' unblocked\n",
                        task->tid, task->name);
}
```

`unblock()` 是 `block()` 的逆操作：状态改回 `Ready`，重新加入运行队列。注意防御性地处理了 `sched_class` 为空的情况——如果任务从未被分配过调度策略，就给它分配默认的 RoundRobin。

### remove_task()——从调度中移除任务

```cpp
void Scheduler::remove_task(Task* task) {
    if (task == nullptr) return;
    if (task->sched_class != nullptr) {
        task->sched_class->dequeue(task);
    }
    task->state = TaskState::Dead;
    cinux::lib::kprintf("[SCHED] Task tid=%u '%s' removed\n", task->tid, task->name);
}
```

`remove_task()` 从运行队列中移除任务并标记为 `Dead`。和 `exit_current()` 不同，它可以移除任意任务而非仅当前任务。如果 `sched_class` 为空（任务未被加入过队列），直接标记 Dead 即可。

### yield()、exit_current() 与 is_initialized() 适配

`yield()` 从 tag 019 的直接 `pick_next + context_switch` 简化为了调用 `schedule()`——一行核心逻辑。`exit_current()` 则新增了对 idle 任务的回退逻辑以及 PerCPU/GDT 状态更新：当所有普通任务都退出后，`pick_next()` 返回空，调度器切换到 idle 任务而不是直接 halt。idle 任务会一直执行 `hlt` 等待中断唤醒。如果连 idle 任务也没有（理论上不应该发生），才回退到永久的 `cli; hlt`。

`is_initialized()` 是一个简单的 getter，供 PIT 的 IRQ0 handler 在调用 `tick()` 之前做检查。在 `init()` 完成之前，`tick()` 可能被中断触发，此时静态变量都是零值，`current_` 为空，必须跳过调度逻辑。

## 设计决策

**tick() 为什么在中断上下文中直接调用 schedule() 而不延迟？** Linux 采用了延迟抢占模型——`scheduler_tick()` 只设置 `TIF_NEED_RESCHED` 标志，实际的上下文切换推迟到下一个 preemption point。这样更灵活，因为可以在任意安全的点执行切换。但 Cinux 目前只有内核线程，没有用户态 preempt_count 等复杂机制，直接在 tick 中调用 schedule 是最简洁的实现。代价是 tick 中的调度逻辑必须保证在中断上下文中是安全的——当前通过中断门自动关 IF 来保证。

**block/unblock 为什么不在内部加锁？** 当前是单核系统，block 的调用点通常已经在中断关闭的状态下（要么从中断 handler 调用，要么在持有 Spinlock 时调用）。加锁反而会增加复杂度。Linux 的做法是在 `proc` 结构体上持有 `p->lock`，但那是因为 SMP 环境下多个 CPU 可能同时操作同一个任务。

**idle 任务为什么不加入 RoundRobin 队列？** RoundRobin 的 `pick_next()` 是从队列头部取出一个任务。如果 idle 在队列里，当没有其他任务时它会不断被 pick 出来再放回去，形成空转。把 idle 排除在队列之外、只在 `schedule()` 的回退路径中手动选中它，语义更清晰——idle 不是"等待运行的普通任务"，而是"无事可做时的最后手段"。

## 参考资料

- Intel SDM Vol. 3A, Section 6.12.1.3 "Flag Usage By Exception- or Interrupt-Handler Procedure" (PDF page 213): 中断门自动清除 IF 标志
- OSDev Wiki, "Scheduling Algorithms": Round Robin 时间片通常选择 20ms-50ms，Cinux 使用 20ms
- Linux `kernel/sched/core.c`, `scheduler_tick()`: Linux 的延迟抢占模型对比
- xv6-riscv `proc.c`, `yield()` / `sched()`: xv6 总是通过 scheduler 中间上下文切换
