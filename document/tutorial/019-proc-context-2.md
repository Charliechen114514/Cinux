# 一帧汇编搞定上下文切换

> 标签：context_switch, 汇编, callee-saved, RoundRobin, yield, exit_current, 协作调度
> 前置：[019-1 从单任务到多任务：内核线程的诞生](019-proc-context-1.md)

## 前言

上一章我们把多任务的基础积木搭好了——`CpuContext` 保存寄存器快照，`Task` 打包线程的全部状态，`TaskBuilder` 封装构造逻辑。但这些数据结构本身是静态的，它们躺在内存里什么也不会做。真正让两个线程跑起来的是一个 75 行的汇编文件 `context_switch.S` 和一个约 200 行的 C++ 调度器。本章我们逐行拆解这两部分，搞清楚"Cinux 怎么从线程 A 切到线程 B"这件看似神秘的事。

说实话，上下文切换是操作系统里少数几个"必须用汇编"的组件之一——因为 C 编译器不理解"我要把 RSP 存到一个结构体里然后加载另一个值到 RSP"这种操作，它会觉得 RSP 被改了之后所有局部变量都失效了。所以你必须自己写汇编，而且必须非常清楚每个指令在干什么，因为调试器在这里帮不了太多忙——当你设断点的时候，线程已经切走了。

## 环境说明

核心文件有三个：`kernel/arch/x86_64/context_switch.S`（75 行汇编）、`kernel/proc/scheduler.hpp`（57 行声明）和 `kernel/proc/scheduler.cpp`（160 行实现）。运行环境与前几个 tag 一致，GCC cross-compiler (`x86_64-elf`)、QEMU、higher-half 内核。前置条件是 `process.hpp` 中的 `Task` 和 `CpuContext` 已经定义完毕，PMM 和 VMM 已经初始化（`TaskBuilder::build()` 需要它们来分配和映射内核栈）。

## context_switch.S：逐行拆解

整个文件做的事情可以用一句话概括：把当前线程的 callee-saved 寄存器存到 `from`，从 `to` 恢复下一个线程的寄存器，然后跳过去。我们分段来看。

### 保存阶段

```asm
context_switch:
    /* ---- Save callee-saved registers into `from` ---- */
    movq %r15, 0(%rdi)               # r15 → from+0
    movq %r14, 8(%rdi)               # r14 → from+8
    movq %r13, 16(%rdi)              # r13 → from+16
    movq %r12, 24(%rdi)              # r12 → from+24
    movq %rbp, 32(%rdi)              # rbp → from+32
    movq %rbx, 40(%rdi)              # rbx → from+40
```

函数入口处，`%rdi` 指向 `from`（当前线程的 `CpuContext`），`%rsi` 指向 `to`（目标线程的 `CpuContext`），这是 System V AMD64 ABI 的前两个参数。六条 `movq` 把 callee-saved 寄存器逐个写入 `from` 的对应偏移位置。这些偏移量——0、8、16、24、32、40——和 `process.hpp` 中的 `static_assert` 完全一致。如果你不小心在结构体里插了个字段忘了更新汇编，编译时 `static_assert` 就会报错。

```asm
    /* Save current RSP */
    movq %rsp, 48(%rdi)              # rsp → from+48

    /* Compute the resume address and save as RIP */
    leaq .restore(%rip), %rax
    movq %rax, 56(%rdi)              # .restore → from+56
```

保存 RSP 是切换的核心——当前线程的栈指针被记录下来了，以后切回来时从 `from+48` 读回就行。然后是整个文件中我最喜欢的一行：`leaq .restore(%rip), %rax`。这条指令用 RIP-relative 寻址计算 `.restore` 标签的绝对地址（因为内核在 higher-half，地址运行时才确定），存入 `%rax`，再写入 `from+56`。它的含义是：当这个线程以后被切回来时，应该从 `.restore` 这个位置继续执行。

你可能想问：为什么不用 `call` + `pop` 来获取返回地址？因为那样会在栈上压入一个额外的返回地址，改变 RSP 的值，而我们需要精确控制 RSP 指向的位置。用 `lea` 计算地址直接存入结构体更加干净，调试时查看 `ctx.rip` 就知道恢复点在哪里。

### 恢复阶段

```asm
    /* ---- Restore callee-saved registers from `to` ---- */
    movq 0(%rsi), %r15               # to+0 → r15
    movq 8(%rsi), %r14               # to+8 → r14
    movq 16(%rsi), %r13              # to+16 → r13
    movq 24(%rsi), %r12              # to+24 → r12
    movq 32(%rsi), %rbp              # to+32 → rbp
    movq 40(%rsi), %rbx              # to+40 → rbx

    /* Switch to the new task's stack */
    movq 48(%rsi), %rsp              # to+48 → RSP: 切换栈！

    /* Jump to the new task's saved instruction pointer */
    jmp *56(%rsi)                    # to+56 → RIP: 跳过去！

.restore:
    /* Execution resumes here when this task is switched back in */
    ret                               # 返回到 context_switch 的调用者
```

前六条 `movq` 从 `to` 加载目标线程的寄存器值。然后关键的一步：`movq 48(%rsi), %rsp`——这一瞬间，栈切换了。从此以后所有栈操作都在目标线程的栈上进行。如果目标线程是第一次被调度，它的 RSP 指向 `TaskBuilder` 初始化时设好的栈顶；如果它之前被 yield 过，RSP 指向它上次被切走时的栈位置。不管是哪种情况，切换到这一步之后，我们就站在目标线程的地盘上了。

`jmp *56(%rsi)` 间接跳转到目标线程保存的 RIP。对于新线程，这个 RIP 是入口函数的地址；对于被 yield 的线程，这个 RIP 是 `.restore` 标签的地址。

这里有一个非常容易忽略的细节——`jmp` 不是 `call`，不会在栈上压入返回地址。那被切走的线程是怎么"返回"的呢？答案是：当这个线程以后被切回来的时候，别的线程的 `context_switch` 会执行 `jmp *56(%rsi)` 跳到这个线程的 `.restore` 标签，然后 `ret` 弹出的是当初调用 `context_switch` 时压入的返回地址——也就是 `yield()` 函数中 `call context_switch` 的下一条指令。整个调用链是 `yield()` -> `call context_switch`（压入 yield 的返回地址）-> 切走 -> ... -> 切回来 -> `jmp .restore` -> `ret`（弹回 `yield()`）。所以虽然中间隔了一段时间（可能几毫秒也可能几秒），但从 `yield()` 的视角看，`context_switch()` 就是一个普通的函数调用——它调用了，然后过了一会儿就返回了。

整个文件只有 75 行——纯粹的寄存器保存/恢复和栈切换，没有 MSR 读写、没有中断控制、没有 FPU 状态操作。这种极简设计的好处是显而易见的：代码量少，出错面小，容易验证。后续如果需要 GS MSR 保存（per-CPU 数据区）、FPU 状态保存（用户态浮点）、中断控制（抢占式调度），可以在此基础上逐步添加。

和 xv6 对比一下：xv6 的 `swtch.S` 把返回地址隐式地保存在栈上（`call` 压入的），而 Cinux 用 `lea .restore(%rip)` 显式计算并存入结构体字段。xv6 的方式更简洁，但 Cinux 的方式更利于调试——你可以直接打印 `ctx.rip` 看到一个有意义的恢复地址，而不是一个栈地址。

## RoundRobin：最朴素的调度策略

有了上下文切换原语，我们还需要一个决策者来回答"下一个该跑谁"。Cinux 用 `RoundRobin` 实现最简单的轮转调度。

```cpp
class SchedulingClass {
public:
    virtual ~SchedulingClass() = default;
    virtual void enqueue(Task* task) = 0;
    virtual void dequeue(Task* task) = 0;
    virtual auto pick_next() -> Task* = 0;
    virtual const char* name() const = 0;
};
```

`SchedulingClass` 是一个虚接口，定义了调度策略的四个基本操作。之所以用虚基类而不是直接写 `RoundRobin`，是因为 Linux 的 `SchedulingClass` 层次结构（CFS / RT / Deadline / Idle）是一个非常值得借鉴的设计——现在只需要一种策略，但接口先抽象出来，以后加 CFS 或者实时调度器不需要改 `Scheduler` 的代码。

`RoundRobin` 用一个固定大小的环形缓冲区（`MAX_TASKS = 64`）管理运行队列。`enqueue` 在队尾插入，`dequeue` 是 O(n) 线性查找加紧缩（对 64 线程的教学内核完全可以接受），`pick_next` 从头部取出任务设为 `Running` 状态后立即回插队尾——这才是"轮转"的含义，每个任务都有机会跑，跑完一圈从头来。

```cpp
Task* RoundRobin::pick_next() {
    if (count_ == 0) {
        return nullptr;
    }
    Task* task = run_queue_[head_];
    head_ = (head_ + 1) % MAX_TASKS;
    count_--;

    task->state = TaskState::Running;

    run_queue_[tail_] = task;    // 回插队尾
    tail_ = (tail_ + 1) % MAX_TASKS;
    count_++;

    return task;
}
```

三个核心操作没有加锁——因为当前是单核协作式调度，`yield()` 和 `exit_current()` 调用时不会发生中断嵌套。当后续加入抢占式调度时，需要在 `enqueue`/`dequeue`/`pick_next` 中加上自旋锁保护。

## Scheduler：静态门面

`Scheduler` 是一组静态方法的集合，作为调度子系统的对外接口。它的核心职责是维护 `current_` 指针（当前正在运行的线程），并在合适的时机调用 `context_switch`。

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

`yield()` 是调度器的心脏。它先获取当前任务的调度策略，从中取出下一个线程。如果下一个就是当前线程（只有一个线程在跑的情况），直接返回避免空切。否则更新 `current_` 指针并调用 `context_switch`。注意这里 `prev` 是在修改 `current_` 之前保存的——这看起来是理所当然的，但如果不这么做就会踩到一个非常坑的 bug，第三篇会详细讲。

`exit_current()` 处理线程退出的情况：先把当前线程标记为 `Dead`，从运行队列中移除，然后切换到下一个线程。

```cpp
void Scheduler::exit_current() {
    Task* prev = current_;
    if (prev != nullptr) {
        prev->state = TaskState::Dead;
        prev->sched_class->dequeue(prev);
    }
    Task* next = default_rr_.pick_next();
    if (next == nullptr) {
        while (1) __asm__ volatile("cli; hlt");
    }
    current_ = next;
    context_switch(&prev->ctx, &next->ctx);
}
```

`exit_current` 的第一步是把 `current_` 存到局部变量 `prev` 里。这行代码虽然不起眼，但它是整个 tag 中最关键的 bug fix 之一。如果直接写 `current_ = next; context_switch(&current_->ctx, &next->ctx);`，那么 `from` 和 `to` 指向同一个 `Task` 的 `CpuContext`，上下文切换变成了保存自己然后从自己恢复——一个完美的空操作。线程永远不会真正退出。这个故事我们第三篇再细说。

## 双线程交替打印：看它跑起来

所有组件就绪后，`kernel_main` 里的演示代码非常简洁：

```cpp
static void thread_a() {
    for (int i = 0; i < 5; i++) {
        kprintf("[A] thread_a iteration %d\n", i);
        Scheduler::yield();
    }
    kprintf("[A] thread_a done\n");
}

static void thread_b() {
    for (int i = 0; i < 5; i++) {
        kprintf("[B] thread_b iteration %d\n", i);
        Scheduler::yield();
    }
    kprintf("[B] thread_b done\n");
}
```

两个线程函数结构完全对称：各打印 5 轮，每轮之后调用 `Scheduler::yield()` 主动让出 CPU。`kernel_main` 用 `TaskBuilder` 创建两个线程，构造一个临时的 `boot_task` 作为初始上下文，然后调用 `run_first()`：

```cpp
Scheduler::init();
auto* task_a = TaskBuilder().set_entry(thread_a).set_name("thread_a").build();
auto* task_b = TaskBuilder().set_entry(thread_b).set_name("thread_b").build();
Scheduler::add_task(task_a);
Scheduler::add_task(task_b);

Task boot_task;
// ... zero-init ...
boot_task.state = TaskState::Running;
boot_task.tid   = 0;
boot_task.name  = "boot";

Scheduler::run_first(&boot_task);
```

`boot_task` 不在调度队列里，它只是一个容器——提供初始的 `CpuContext` 作为 `context_switch` 的 `from` 参数。`run_first` 从 boot 上下文切换到 `thread_a`，从此两个线程开始交替运行。执行流是 boot -> A(打印第 0 轮) -> yield -> B(打印第 0 轮) -> yield -> A(打印第 1 轮) -> yield -> B(打印第 1 轮) -> ... -> A done -> exit -> B done -> exit -> 没有更多线程了，内核停机。

预期串口输出是这样的：

```
[A] thread_a iteration 0
[B] thread_b iteration 0
[A] thread_a iteration 1
[B] thread_b iteration 1
...
[A] thread_a done
[SCHED] Task tid=1 'thread_a' exited
[B] thread_b done
[SCHED] Task tid=2 'thread_b' exited
[SCHED] No more tasks, halting.
```

当然，这是修完所有 bug 之后的输出。实际上第一次跑的时候，看到的不是这个美好的画面，而是一个 RIP=`0xDEADC0DE` 的 emulation failure。这个故事我们下一章再讲。

## 收尾

这一章我们把上下文切换的完整链条串起来了：`context_switch.S` 是底层原语，`RoundRobin` 是调度策略，`Scheduler` 是粘合它们的静态门面。从汇编到 C++，从保存恢复寄存器到运行队列管理，整个切换过程大约 300 行代码就搞定了。当然，这只是合作式调度——线程主动让出 CPU 才会切换。要实现抢占式调度（时间片到了强制切换），只需要在时钟中断处理程序中调用调度方法，后续的 tag 会在当前架构上做这个扩展。

但正当你以为一切顺利的时候，bug 已经在暗处等着了。下一章我们来聊两个让内核崩掉的坑。

## 参考资料

- Intel SDM Vol. 3A, Section 4.5.2：CR3 与地址空间切换
- System V AMD64 ABI: [https://gitlab.com/x86-psABIs/x86-64-ABI](https://gitlab.com/x86-psABIs/x86-64-ABI)
- OSDev Wiki "Context Switching": [https://wiki.osdev.org/Context_Switching](https://wiki.osdev.org/Context_Switching) — 软件上下文切换的通用方法
- OSDev Wiki "Scheduling Algorithms": [https://wiki.osdev.org/Scheduling_Algorithms](https://wiki.osdev.org/Scheduling_Algorithms) — Round Robin 及其变体
- Linux `__switch_to_asm` (arch/x86/entry/entry_64.S)：同样的 callee-saved 模式
- xv6 `swtch.S`: [https://github.com/mit-pdos/xv6-public/blob/master/swtch.S](https://github.com/mit-pdos/xv6-public/blob/master/swtch.S) — 32 位上下文切换对比
