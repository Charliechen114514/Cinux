# 019-2 通读：context_switch 汇编与调度器实现——让两个线程跑起来的核心机制

## 概览

本文是 tag `019_proc_context` 三篇通读教程的第二篇，聚焦上下文切换汇编原语 `context_switch.S` 和调度器 C++ 实现 `scheduler.cpp`。`context_switch` 是调度器的心脏——保存当前线程的 callee-saved 寄存器，恢复下一个线程的寄存器，然后跳转过去。`Scheduler` 静态类执行调度策略：`yield()` 主动让出 CPU，`exit_current()` 处理线程退出，`run_first()` 从 boot 跳到第一个线程。最后看 `main.cpp` 中的双线程协作演示。

## 架构图

```
    context_switch(CpuContext* from, CpuContext* to) 调用流程：
    ┌────────────────────────────────────────────────────────────────┐
    │  Scheduler::yield()                                            │
    │    ├── prev = current_                                         │
    │    ├── next = pick_next()  (RoundRobin 从队列头部取出)         │
    │    ├── current_ = next                                         │
    │    ├── context_switch(&prev->ctx, &next->ctx)                  │
    │    │     ├── 保存 r15-r12, rbp, rbx → from                    │
    │    │     ├── 保存 RSP → from+48                                │
    │    │     ├── leaq .restore(%rip) → from+56 (恢复点)           │
    │    │     ├── 恢复 r15-r12, rbp, rbx ← to                      │
    │    │     ├── 加载 to+48 → RSP (切换栈)                        │
    │    │     ├── jmp *to+56 (跳转到目标线程的 RIP)                 │
    │    │     └── .restore: ret (被切回来时在这里返回)              │
    └────────────────────────────────────────────────────────────────┘

    RoundRobin 队列旋转（3 个线程 A B C）：
    初始: [A, B, C]
    pick_next(): 弹出 A，设 Running，A 重新入队尾
    → [B, C, A]  current=A
    pick_next(): 弹出 B，设 Running，B 入队尾
    → [C, A, B]  current=B
```

## 代码精讲

### context_switch.S——保存与恢复

```asm
.global context_switch
.type context_switch, @function
context_switch:

    /* ---- Save callee-saved registers into `from` ---- */
    movq %r15, 0(%rdi)               # %r15→from+0: save R15
    movq %r14, 8(%rdi)               # %r14→from+8: save R14
    movq %r13, 16(%rdi)              # %r13→from+16: save R13
    movq %r12, 24(%rdi)              # %r12→from+24: save R12
    movq %rbp, 32(%rdi)              # %rbp→from+32: save RBP
    movq %rbx, 40(%rdi)              # %rbx→from+40: save RBX

    /* Save current RSP (stack pointer) */
    movq %rsp, 48(%rdi)              # %rsp→from+48: save RSP

    /* Compute the resume address and save as RIP */
    leaq .restore(%rip), %rax        # .restore address→%rax: compute return point
    movq %rax, 56(%rdi)              # %rax→from+56: save resume RIP
```

函数入口处，`%rdi` 指向 `from`（当前线程 CpuContext），`%rsi` 指向 `to`（目标线程 CpuContext），遵循 System V AMD64 ABI。前六条 `movq` 将 callee-saved 寄存器写入 `from` 对应偏移位置，偏移量与 `process.hpp` 的 `static_assert` 完全一致。然后保存 RSP 到 `from+48`。`leaq .restore(%rip), %rax` 计算 `.restore` 标签的绝对地址存入 `from+56`——记录"这个线程被切回来时应该从哪里恢复"。用的是 RIP-relative 寻址，因为内核在 higher-half，地址运行时才确定。

```asm
    /* ---- Restore callee-saved registers from `to` ---- */

    movq 0(%rsi), %r15               # to+0→%r15: restore R15
    movq 8(%rsi), %r14               # to+8→%r14: restore R14
    movq 16(%rsi), %r13              # to+16→%r13: restore R13
    movq 24(%rsi), %r12              # to+24→%r12: restore R12
    movq 32(%rsi), %rbp              # to+32→%rbp: restore RBP
    movq 40(%rsi), %rbx              # to+40→%rbx: restore RBX

    /* Switch to the new task's stack */
    movq 48(%rsi), %rsp              # to+48→%rsp: switch stack pointer

    /* Jump to the new task's saved instruction pointer */
    jmp *56(%rsi)                    # to+56→RIP: resume execution

.restore:
    /* Execution resumes here when this task is switched back in */
    ret                               # return to caller of context_switch
```

恢复阶段从 `to` 加载目标线程的寄存器值。前六条 `movq` 恢复 callee-saved GPR，然后将目标线程的 RSP 加载到 `%rsp`——这一瞬间栈切换了，之后所有栈操作都在目标线程的栈上进行。最后 `jmp *56(%rsi)` 跳转到目标线程的 RIP。新线程的 RIP 指向入口函数；被 yield 的线程的 RIP 指向 `.restore` 标签，到达后 `ret` 弹出调用 `context_switch()` 时压入的返回地址，回到 `yield()` 的调用者。

这里有一个容易忽略的细节：`jmp` 不是 `call`，不会在栈上压入返回地址。被切走的线程的 RIP 被设为 `.restore`，被切回来时 `jmp` 跳到 `.restore`，`ret` 弹出的是 `yield()` 调用 `context_switch()` 时压入的返回地址。整个函数只有 75 行，纯粹做寄存器保存/恢复和栈切换——没有 MSR 读写、没有中断控制、没有 FPU 状态操作。这些扩展可以在后续的 tag 中按需加入。

### RoundRobin——环形缓冲区调度

```cpp
// enqueue — 队尾插入
void RoundRobin::enqueue(Task* task) {
    if (count_ >= MAX_TASKS) {
        cinux::lib::kprintf("[SCHED] RoundRobin: run queue full\n");
        return;
    }
    run_queue_[tail_] = task;
    tail_             = (tail_ + 1) % MAX_TASKS;
    count_++;
    task->state = TaskState::Ready;
}

// dequeue — O(n) 线性查找 + 紧缩
void RoundRobin::dequeue(Task* task) {
    for (int i = 0; i < count_; i++) {
        int idx = (head_ + i) % MAX_TASKS;
        if (run_queue_[idx] == task) {
            for (int j = i; j < count_ - 1; j++) {
                int cur = (head_ + j) % MAX_TASKS;
                int nxt = (head_ + j + 1) % MAX_TASKS;
                run_queue_[cur] = run_queue_[nxt];
            }
            run_queue_[(head_ + count_ - 1) % MAX_TASKS] = nullptr;
            tail_ = (tail_ - 1 + MAX_TASKS) % MAX_TASKS;
            count_--;
            return;
        }
    }
}

// pick_next — 头部取出，立即回插队尾（真正的轮转）
Task* RoundRobin::pick_next() {
    if (count_ == 0) {
        return nullptr;
    }
    Task* task = run_queue_[head_];
    head_ = (head_ + 1) % MAX_TASKS;
    count_--;

    task->state = TaskState::Running;

    run_queue_[tail_] = task;
    tail_ = (tail_ + 1) % MAX_TASKS;
    count_++;

    return task;
}
```

三个核心操作直接操作环形缓冲区，没有加锁——因为当前是单核协作式调度，`yield()` 和 `exit_current()` 调用时中断状态由调用者控制。`enqueue` 在队尾插入，任务状态设为 `Ready`。`dequeue` 是 O(n) 线性查找 + 紧缩——对 64 线程的教学内核可接受。`pick_next` 从头部取出任务设为 `Running`，然后立即回插队尾，实现真正的轮转。

### Scheduler 静态方法

```cpp
void Scheduler::init() {
    class_count_ = 0;
    current_     = nullptr;
    register_class(&default_rr_);
    cinux::lib::kprintf("[SCHED] Scheduler initialised with %s class\n",
                        default_rr_.name());
}
```

`init()` 重置状态，注册默认 RoundRobin。简单直接——没有创建 idle 任务，没有初始化时间片计数器。

```cpp
void Scheduler::yield() {
    if (current_ == nullptr) {
        return;
    }

    SchedulingClass* cls = current_->sched_class;
    if (cls == nullptr) {
        return;
    }

    Task* next = cls->pick_next();
    if (next == nullptr || next == current_) {
        return;
    }

    Task* prev = current_;
    current_ = next;
    context_switch(&prev->ctx, &next->ctx);
}
```

`yield()` 是主动让出 CPU 的核心方法。先获取当前任务的调度策略，从中 `pick_next()` 取出下一个任务。如果下一个就是自己（只有一个线程在跑）或者队列为空，直接返回不做空切。否则保存 `prev = current_`，更新 `current_ = next`，调用 `context_switch` 完成切换。注意这里 `prev` 是在修改 `current_` 之前保存的——这看起来理所当然，但 `exit_current()` 里曾经忘了这么做，导致了严重的 bug。

```cpp
void Scheduler::exit_current() {
    Task* prev = current_;
    if (prev != nullptr) {
        prev->state = TaskState::Dead;
        prev->sched_class->dequeue(prev);
        cinux::lib::kprintf("[SCHED] Task tid=%u '%s' exited\n",
                            prev->tid, prev->name);
    }

    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        cinux::lib::kprintf("[SCHED] No more tasks, halting.\n");
        while (1) __asm__ volatile("cli; hlt");
    }

    current_ = next;
    context_switch(&prev->ctx, &next->ctx);
}
```

`exit_current()` 处理线程退出。先保存 `prev = current_`，把当前线程标记为 `Dead`，从运行队列中移除。然后 `pick_next()` 找下一个线程。如果队列为空，直接 `cli; hlt` 永久停机。否则更新 `current_` 并切换。`exit_current` 的关键细节就是第一行的 `Task* prev = current_;`——如果先写 `current_ = next` 再调用 `context_switch(&current_->ctx, &next->ctx)`，`from` 和 `to` 就指向同一个 CpuContext，切换变成空操作。这个 bug 在开发中真实发生过，排查过程见第三篇。

```cpp
void Scheduler::run_first(Task* boot_task) {
    current_ = boot_task;

    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        return;
    }

    current_ = next;
    context_switch(&boot_task->ctx, &next->ctx);
}
```

`run_first()` 从 boot 上下文切换到第一个真正的任务。`boot_task` 不在调度队列中——它只提供初始 CpuContext 作为 `from` 参数，这个上下文以后不会再被恢复。

### main.cpp——双线程协作演示

```cpp
static void thread_a() {
    for (int i = 0; i < 5; i++) {
        cinux::lib::kprintf("[A] thread_a iteration %d\n", i);
        cinux::proc::Scheduler::yield();
    }
    cinux::lib::kprintf("[A] thread_a done\n");
}

static void thread_b() {
    for (int i = 0; i < 5; i++) {
        cinux::lib::kprintf("[B] thread_b iteration %d\n", i);
        cinux::proc::Scheduler::yield();
    }
    cinux::lib::kprintf("[B] thread_b done\n");
}
```

两个线程函数结构完全对称：各打印 5 轮，每轮之后调用 `Scheduler::yield()` 主动让出 CPU。

```cpp
    cinux::proc::Scheduler::init();

    auto* task_a = cinux::proc::TaskBuilder()
        .set_entry(thread_a)
        .set_name("thread_a")
        .build();
    auto* task_b = cinux::proc::TaskBuilder()
        .set_entry(thread_b)
        .set_name("thread_b")
        .build();

    cinux::proc::Scheduler::add_task(task_a);
    cinux::proc::Scheduler::add_task(task_b);

    cinux::proc::Task boot_task;
    for (uint8_t* p = reinterpret_cast<uint8_t*>(&boot_task);
         p < reinterpret_cast<uint8_t*>(&boot_task + 1); p++) {
        *p = 0;
    }
    boot_task.state = cinux::proc::TaskState::Running;
    boot_task.tid = 0;
    boot_task.name = "boot";

    cinux::proc::Scheduler::run_first(&boot_task);
```

`kernel_main` 的调度启动分三步：初始化调度器，用 `TaskBuilder` 创建两个线程并加入队列，然后构造临时 `boot_task`（手动清零、设置初始状态）调用 `run_first`。`boot_task` 不在调度队列中——它只提供初始 CpuContext 作为 `from` 参数。`run_first` 从 boot 上下文切换到第一个线程，从此两个线程交替运行直到全部退出。

## 设计决策

**为什么用 `leaq .restore(%rip)` 而不是 `call` + `pop`？** Cinux 用 `leaq` 计算恢复地址存入 CpuContext，比 xv6 的"保存在栈上"更显式，调试时直接看 rip 字段就知道恢复点。

**为什么 `dequeue` 是 O(n)？** 线性扫描 + 紧缩在 64 线程以内可接受。真正的性能瓶颈在上下文切换本身，不在这里。

**为什么 context_switch 不保存 FPU/SIMD 状态？** 当前的内核线程不使用浮点运算，省去 512 字节的 `fxsave`/`fxrstor` 可以让每次切换更快。后续如果需要支持用户态进程（可能使用 SSE/AVX），可以在切换前后加上 FPU 状态保存。Linux 用延迟 FPU 保存来优化这一点——只有当另一个线程实际使用了 FPU 时才保存。

## 扩展方向

- **抢占式调度**：在 PIT/LAPIC 中断中调用调度方法，实现时间片驱动的自动切换。
- **多调度策略**：`SchedulingClass` 虚接口已就位，可以实现 CFS 风格的红黑树调度器或实时调度器。
- **FPU 状态保存**：在 Task 中加入 `fpu_state[512]` 字段，切换前后 `fxsave`/`fxrstor`。
- **GS MSR 保存**：如果使用 per-CPU 数据区（swapgs），需要在 context_switch 中保存/恢复 MSR_GS_BASE 和 MSR_KERNEL_GS_BASE。
- **中断安全加锁**：RoundRobin 操作加 Spinlock + irq_guard，为抢占式调度做准备。

## 参考资料

- Intel SDM Vol. 3A, Section 4.5.2: CR3 与地址空间切换
- Linux `__switch_to_asm` (arch/x86/entry/entry_64.S): 同样的 callee-saved 模式
- OSDev Wiki "Context Switching": https://wiki.osdev.org/Context_Switching
- OSDev Wiki "Scheduling Algorithms": https://wiki.osdev.org/Scheduling_Algorithms
