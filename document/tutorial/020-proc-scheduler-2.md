---
title: 020-proc-scheduler-2 · 进程调度
---

# 时间片轮转：从 tick 到 context_switch

> 标签：tick, schedule, idle 任务, block/unblock, RoundRobin, 抢占式调度
> 前置：[020-1 从协作到抢占：内核多任务的进化](020-proc-scheduler-1.md)

## 前言

上一篇我们搭好了抢占式调度的三块基础设施——Spinlock 保护共享队列、PerCPU 为每个 CPU 保存运行时状态、TSS.RSP0 确保任务切换后内核栈指针正确。这一篇我们深入调度器本身的核心逻辑：定时器中断如何驱动 `tick()`，`tick()` 如何在时间片耗尽时调用 `schedule()`，`schedule()` 如何选下一个任务并完成上下文切换，以及 idle 任务和 block/unblock 机制的设计。

这是 tag 020 最核心的部分——理解了这条从 PIT 到 context_switch 的完整链条，你就理解了抢占式调度的全部精髓。

## 完整链条：从 PIT 到 context_switch

在进入代码之前，先从全局视角看一眼抢占调度发生时的完整调用链。PIT 芯片以 100Hz 的频率产生 IRQ0 中断。中断到达后，CPU 通过中断门进入 ISR stub——此时 IF 被自动清零（Intel SDM Vol. 3A Section 6.12.1.3）。ISR stub 调用 `PIT::irq0_handler()`，handler 先递增 tick 计数器、发送 EOI 通知 PIC 可以接收下一个中断，然后调用 `Scheduler::tick()`。`tick()` 检查时间片是否耗尽，如果耗尽了就调用 `schedule()`。`schedule()` 从 RoundRobin 队列中选出下一个任务，更新全局状态，调用 `context_switch` 完成物理切换。

这条链路里有几个细节值得注意。第一，EOI 必须在 `schedule()` 之前发送——如果先 `schedule()` 再 EOI，`context_switch` 切到新任务后 PIC 仍然认为 IRQ0 正在被处理中，下一个定时器中断就无法到达。第二，`schedule()` 执行时中断是关闭的（中断门语义），所以 `context_switch` 的执行不会被另一个定时器中断打断——调度本身是原子的。第三，`context_switch` 内部在切换栈之后有一条 `sti` 指令，这和我们第三篇要讲的 IF 标志修复有关，这里先按下不表。

## tick()：定时器中断的入口

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

`tick()` 是抢占式调度的触发点，由 PIT 的 IRQ0 handler 在每个定时器中断中调用。前两行是防御性检查——在调度器初始化完成之前、或者在还没有运行任务时，直接返回。然后递增全局 tick 计数器和当前任务的时间片计数器。当 `current_slice_` 达到 `DEFAULT_TIME_SLICE`（值为 2）时，重置计数器并调用 `schedule()` 执行上下文切换。

`DEFAULT_TIME_SLICE = 2` 意味着每个线程在耗尽 2 个 tick（即 20ms，PIT 配置为 100Hz）后被强制切换。这个值经过了一番调试才确定——最初设为 10（100ms），在 QEMU TCG 模式下线程在自己的时间片内就跑完了，根本没有触发抢占。OSDev Wiki 的 Scheduling Algorithms 页面建议"A frequently chosen compromise for the quantum is between 20ms and 50ms"，Cinux 选择了最短的 20ms，原因也和虚拟化环境有关——CPU 密集循环在 QEMU 中比物理机快得多。

值得注意的是 `tick()` 是在中断上下文中被调用的。Intel SDM Vol. 3A Section 6.12.1.3 指出，通过中断门进入 ISR 时 CPU 自动清除 IF 标志，所以 `schedule()` 在执行时中断是关闭的——`context_switch` 的执行不会被另一个定时器中断打断，调度本身是原子的。

## schedule()：核心调度函数

`schedule()` 是整个抢占式调度的核心，也是代码量最大的函数。我们分三段来看。

首先是保存当前状态并选出下一个任务：

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

保存当前任务指针，然后把它的状态从 `Running` 改回 `Ready`——注意只有 Running 状态才会改，如果任务是 Blocked 或 Dead 就保持原状。接着通过 `pick_next()` 从 RoundRobin 队列中取出下一个任务。

接下来是边界情况处理——这是整个函数最精巧的部分：

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

如果 `pick_next()` 返回空（队列为空）或返回的还是当前任务（队列中只有一个任务且正在运行），我们需要检查当前任务是否还能继续执行。如果当前任务的状态既不是 Blocked 也不是 Dead，说明它只是被抢占了一下，恢复为 Running 直接返回即可——没有切换的必要。但如果当前任务已经被阻塞或退出了，就必须切到 idle 任务，否则就没人可执行了。这种"正常抢占不需要切换、异常状态才需要兜底"的分支逻辑是抢占式调度中常见的模式。

最后是全局状态更新和物理切换：

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

更新 `current_`、`g_per_cpu.current`、重置时间片计数器。如果切到的不是 idle 任务，还需要更新 TSS.RSP0。idle 任务不需要更新 TSS.RSP0，因为它永远不会在用户态运行。最后调用 `context_switch` 完成寄存器切换。

这段代码有一个微妙的正确性保证：`context_switch` 调用时中断仍然是关闭的（因为我们在 ISR 上下文中），所以从 `current_ = next` 到 `context_switch` 之间不会有中断打断——状态更新是原子的。如果在这里被另一个定时器中断打断，`current_` 指向新任务但实际还没切换过去，第二次 `tick()` 就会在错误的状态上做调度决策。

对比 Linux 的 `schedule()` 函数（同样在 `kernel/sched/core.c` 中），Linux 在切换前会做大量准备工作：更新运行时统计、检查抢占计数、处理 RT throttling、调用 `sched_fork` 的回调等等。Cinux 省掉了这些，因为当前只有纯内核线程和单一的 RoundRobin 策略。当未来引入 CFS 或优先级调度时，`schedule()` 会在 `pick_next()` 前面加入更多的决策逻辑。

## idle 任务：无事可做时的最后手段

```cpp
void Scheduler::idle_entry() {
    while (true) {
        __asm__ volatile("hlt");
    }
}
```

idle 任务是系统中最特殊的一个线程——它唯一的职责就是在没有其他任务可运行时让 CPU 进入低功耗状态。`hlt` 指令让 CPU 停止执行直到下一个中断到来，在 QEMU 中能显著降低宿主机的 CPU 占用。

idle 任务在 `Scheduler::init()` 中被创建，优先级设为 255，设为 `Ready` 状态，但不会被加入 RoundRobin 的运行队列。它只在所有普通任务都阻塞或退出时才被 `schedule()` 的回退路径手动选中。为什么不加入队列？因为 RoundRobin 的 `pick_next()` 是从队列头部取出一个任务。如果 idle 在队列里，当没有其他任务时它会不断被 pick 出来再放回去，形成空转。把 idle 排除在队列之外、只在回退路径中手动选中它，语义更清晰——idle 不是"等待运行的普通任务"，而是"无事可做时的最后手段"。

对比 xv6 的 scheduler 循环：xv6 不需要专门的 idle 任务——当没有 RUNNABLE 进程时，scheduler 循环就在 `intr_on(); hlt` 中等待中断。这个设计更简洁，但需要 scheduler 上下文始终存在。Cinux 的 idle 任务把"等待中断"封装成了一个独立的线程，代价是多了一个 TCB 和一个内核栈，好处是调度逻辑统一——所有执行流都是 Task，不存在特殊的 scheduler 上下文。

## block() 与 unblock()：为同步原语铺路

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

`block()` 把任务状态设为 `Blocked` 并从运行队列中移除。`reason` 参数是给调试日志用的——当系统出现死锁时，日志里的 "waiting for mutex" 比单纯的 "blocked" 有用得多。最关键的是最后那个判断：如果阻塞的是当前正在运行的任务，必须立即调用 `schedule()` 让出 CPU。否则当前任务的状态是 Blocked 但还在执行——这显然是矛盾的。

`unblock()` 是 `block()` 的逆操作，把状态改回 `Ready`，重新加入运行队列。注意防御性地处理了 `sched_class` 为空的情况——如果任务从未被分配过调度策略，就给它分配默认的 RoundRobin。

这两个函数在 tag 020 中只是定义了接口，真正的使用者是 tag 021 的 Mutex 和 Semaphore。Mutex 的 `lock()` 发现锁已被持有时，会调用 `block(current, "waiting for mutex")` 把自己挂起；当锁的持有者调用 `unlock()` 时，会调用 `unblock(waiter)` 唤醒等待队列头的线程。这种 "block on contention, unblock on release" 的模式是内核同步原语的标准范式。

为什么不在这里加锁？当前是单核系统，block 的调用点通常已经在中断关闭的状态下（要么从中断 handler 调用，要么在持有 Spinlock 时调用）。加锁反而会增加复杂度。Linux 的做法是在 `proc` 结构体上持有 `p->lock`，但那是因为 SMP 环境下多个 CPU 可能同时操作同一个任务。

## yield() 的简化

tag 019 中 `yield()` 的实现是直接调 `pick_next + context_switch`，tag 020 把它简化成了调用 `schedule()`——一行核心逻辑。这是因为 `schedule()` 已经包含了完整的调度决策：保存当前状态、选下一个任务、处理边界情况、更新全局状态、执行切换。`yield()` 只是一个语义化的包装——"我主动让出 CPU，让调度器决定下一个跑谁"。

这个简化的好处是消除了代码重复。tag 019 的 `yield()` 和 `schedule()` 之间有大量重叠的逻辑，维护两份几乎相同的代码容易出错——就像 `exit_current` 的指针覆盖 bug 那样（tag 019 第三篇讲过的故事）。

## main.cpp：6 线程抢占式 demo

理论讲完了，来看实际跑起来的效果。main.cpp 的改动体现了从协作式到抢占式的范式转换：

```cpp
static void worker(const char label, int iters) {
    for (int i = 0; i < iters; i++) {
        cinux::lib::kprintf("[%c] tid=%u iter %d/%d\n", label,
                            cinux::proc::Scheduler::current()->tid, i + 1, iters);
        for (volatile int j = 0; j < 20000000; j++) {}
    }
    cinux::lib::kprintf("[%c] done\n", label);
}
```

和 tag 019 相比有几个关键变化。第一，不再调用 `yield()`——线程函数完全是 CPU 密集型的工作循环，调度完全由定时器中断驱动。第二，2000 万次的忙循环保证每个线程需要多个时间片才能完成——`DEFAULT_TIME_SLICE=2` 对应 20ms，20M 次迭代在 QEMU 中大约需要 50-100ms，确保定时器中断有充分的机会介入。第三，6 个线程从 A 到 F 覆盖了更多场景，更容易暴露调度器中的边界问题。

启动顺序也做了关键调整——`Scheduler::init()` 移到了 `sti` 之前。这是因为在抢占式调度中，定时器中断随时可能触发 `tick()`，而 `tick()` 依赖 `initialized_` 标志。如果先 `sti` 再 `init()`，第一次 IRQ0 到达时调度器还没准备好，虽然 `tick()` 内部有 `!initialized_` 检查不会崩溃，但会丢失 tick。调整顺序后，调度器在第一个中断到来之前就已经完全初始化了。

## PIT tick 集成

最后看 PIT 驱动的修改。新增了两行：

```cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);
    PIC::send_eoi(0);
    cinux::proc::Scheduler::tick();
}
```

先递增 tick 计数器，然后发送 EOI 通知 PIC 可以接收下一个中断，最后调用 `Scheduler::tick()`。这里的顺序很重要——必须先 EOI 再调度。对比 xv6-riscv 的做法：`clockintr()` 先调用 `w_stimecmp()` 设置下一次时钟中断，然后调用 `yield()`。xv6 的时钟中断通过 S-mode 的 `stimecmp` 寄存器控制，不需要手动 EOI，所以顺序无关紧要。而 x86 的 PIC/APIC 模型要求显式 EOI，必须先完成这个操作再进行上下文切换。

## 设计决策

tick() 为什么在中断上下文中直接调用 schedule() 而不延迟？Linux 采用了延迟抢占模型——`scheduler_tick()` 只设置 `TIF_NEED_RESCHED` 标志，实际的上下文切换推迟到下一个 preemption point。这样更灵活，因为可以在任意安全的点执行切换。但 Cinux 目前只有内核线程，没有用户态 preempt_count 等复杂机制，直接在 tick 中调用 schedule 是最简洁的实现。代价是 tick 中的调度逻辑必须保证在中断上下文中是安全的——当前通过中断门自动关 IF 来保证。当未来引入用户态和 SMP 后，延迟抢占是必须的，但那是后话了。

block/unblock 为什么不在内部加锁？如前所述，当前是单核系统，block 的调用点通常已经在中断关闭的状态下。加锁反而增加复杂度。Linux 的 `p->lock` 是为 SMP 准备的。

## 收尾

到这里我们已经把抢占式调度的完整链条从头到尾走了一遍：PIT 以 100Hz 产生 IRQ0 -> `tick()` 递增时间片计数 -> 时间片耗尽时调用 `schedule()` -> `schedule()` 从 RoundRobin 队列选下一个任务 -> 更新全局状态和 TSS.RSP0 -> `context_switch` 完成物理切换。idle 任务在没有其他任务可运行时兜底，block/unblock 为未来的同步原语铺好了路。

但故事到这里还没完。这条看似简单的链路在实际运行时踩了两个非常经典的暗坑——一个让抢占从未触发，一个让后续线程的中断全关。下一篇我们就来拆这两个坑。

## 参考资料

- Intel SDM Vol. 3A, Section 6.12.1.3 "Flag Usage By Exception- or Interrupt-Handler Procedure" (PDF page 213)：中断门自动清除 IF 标志
- Intel SDM Vol. 3A, Section 6.8.1 "Masking Maskable Hardware Interrupts" (PDF page 203)：IF 标志控制可屏蔽中断
- OSDev Wiki, "Scheduling Algorithms"：[https://wiki.osdev.org/Scheduling_Algorithms](https://wiki.osdev.org/Scheduling_Algorithms) — Round Robin 时间片建议 20ms-50ms
- OSDev Wiki, "Programmable Interval Timer"：[https://wiki.osdev.org/Programmable_Interval_Timer](https://wiki.osdev.org/Programmable_Interval_Timer) — Intel 8253/8254 PIT 作为 IRQ0 定时源
- Linux `kernel/sched/core.c`, `scheduler_tick()`：[https://docs.kernel.org/scheduler/sched-design-CFS.html](https://docs.kernel.org/scheduler/sched-design-CFS.html) — Linux 的延迟抢占模型
- xv6-riscv `trap.c` / `proc.c`：[https://github.com/mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv) — timer interrupt -> yield -> sched -> swtch
