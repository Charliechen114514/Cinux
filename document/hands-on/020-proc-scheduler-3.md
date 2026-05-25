---
title: 020-proc-scheduler-3 · 进程调度
---

# 020-3 定时器集成与 IF 标志修复

## 导语

前两篇我们把抢占式调度的全部基础设施和核心逻辑都实现好了——Spinlock、PerCPU、tick()、schedule()、block/unblock、idle 任务。但是如果你编译运行现在的代码，内核的表现和 tag 019 完全一样——线程还是得手动 yield()。原因很简单：tick() 虽然写好了，但 PIT 的 IRQ0 handler 还没有调用它，定时器中断来了也到不了调度器。

这一篇我们要做三件事：第一，把 PIT 的 IRQ0 handler 和 Scheduler::tick() 连起来，让定时器真正驱动调度；第二，修复一个让我血压拉满的 bug——context_switch 中 IF 标志丢失导致新任务永远收不到中断；第三，更新 kernel_main 中的演示代码，用 6 个 CPU 密集型线程来验证抢占式调度是否正常工作。

说实话，这是整个 tag 020 中最有意思的一篇。第一个 bug（时间片过长）让人意识到虚拟化环境下 CPU 速度的欺骗性，第二个 bug（IF 标志丢失）则是一个教科书级的从"协作式"到"抢占式"迁移陷阱——Intel SDM 里写得清清楚楚的机制，在代码里就是一行的差异，但排查起来能让你怀疑人生。

这一篇涉及的文件变更比较分散：pit.cpp（加两行代码）、context_switch.S（加一行 sti 指令）、main.cpp（重写演示线程和启动顺序）。每个文件的改动都不大，但它们组合在一起才能让抢占式调度真正运转起来。

## 概念精讲

### PIT IRQ0 与调度器的连接

PIT（Programmable Interval Timer，Intel 8254）在 tag 011 中就已经配置好了——100Hz 频率，每次计数归零时通过 IRQ0 发出一个硬件中断。在 tag 020 之前，IRQ0 的处理函数 `PIT::irq0_handler()` 只是递增一个 tick 计数器然后发送 EOI（End of Interrupt）。现在我们需要在 EOI 之后加一行调用——`Scheduler::tick()`。

为什么是在 EOI 之后而不是之前？因为 EOI 告诉 PIC "我已经处理完了这个中断，你可以发下一个了"。如果在 EOI 之前就调用 tick() → schedule() → context_switch，那么 PIC 会认为 IRQ0 还在处理中，后续的中断会被阻塞。虽然在我们的单核教学内核中，context_switch 切走再切回来的时间间隔内不会有太多 IRQ0 堆积，但"先 EOI 再调度"是一个正确且安全的习惯——Linux 和 xv6 也是这么做的。

对比 xv6-riscv 的做法：`clockintr()` 先调用 `w_stimecmp()` 设置下一次时钟中断，然后调用 `yield()`。xv6 的时钟中断通过 S-mode 的 `stimecmp` 寄存器控制，不需要手动 EOI，所以顺序无关紧要。而 x86 的 PIC/APIC 模型要求显式 EOI，必须先完成这个操作再进行上下文切换。这个差异体现了 x86 和 RISC-V 在中断控制器设计上的根本不同——x86 的 8259 PIC 是一个有状态的设备，需要软件主动通知它 "中断处理完毕"；RISC-V 的 CLINT 是一个无状态的定时器，软件只需编程下一次中断的时间。

> 参考：OSDev Wiki — Programmable Interval Timer (https://wiki.osdev.org/Programmable_Interval_Timer)

### IF 标志丢失——从协作式到抢占式的经典陷阱

这是整个 tag 020 中最关键的 bug，也是最值得深入理解的一个。让我从头讲起。

x86 的 RFLAGS 寄存器中有一个 IF（Interrupt Enable Flag）位，位于 bit 9。IF=1 时 CPU 响应外部硬件中断（INTR 引脚或 Local APIC），IF=0 时 CPU 忽略这些中断。IF 可以通过 sti（set IF）和 cli（clear IF）指令来手动控制。

关键点来了：当 CPU 通过中断门（Interrupt Gate）进入中断处理程序时，处理器会自动清除 IF 标志。这是 Intel SDM Vol.3A Section 6.12.1.3 明确规定的——中断门的语义就是"进入处理程序时禁止中断"，防止中断嵌套导致栈溢出。从中断处理程序返回时，IRETQ 指令会从栈上的中断帧中恢复原始的 RFLAGS，包括 IF 位。

现在来看 context_switch 在抢占式调度中的调用链：

```
IRQ0 到来 → ISR stub（CPU 自动清除 IF）→ pit_irq0_handler → Scheduler::tick() → schedule() → context_switch
```

进入 ISR 时 IF 被清零了。context_switch 只保存和恢复 callee-saved 寄存器（r15-r12, rbp, rbx, rsp, rip）——它不保存 RFLAGS。当 context_switch 切换到新任务的栈并跳转到新任务的入口时，新任务继承的是当前的 IF=0 状态。

对于首次运行的新任务（rip 指向线程入口函数），它通过 `jmp *56(%rsi)` 直接跳转，IF 一直是 0——它永远收不到定时器中断，永远不会被抢占。对于被抢占过的老任务（rip 指向 context_switch 的 `.restore` 标签），它会通过 ret 链回到 ISR stub，最终通过 IRETQ 从中断帧恢复原始的 RFLAGS（IF=1）——中断正常恢复。

所以症状是这样的：只有第一个被抢占的任务（比如线程 A）能恢复中断，后续所有首次被调度的新线程全部带着 IF=0 运行，永远不会被抢占。你看到的现象就是 A 被抢占一次后 B/C/D/E/F 全部顺序跑完，再也不会被打断。

修复方法极其简单——在 `context_switch.S` 中找到切换栈指针的那行汇编（`movq` 指令把 `to` 结构体的 RSP 字段加载到 `%rsp`），在它和跳转到新任务的那行（`jmp` 指令跳转到 `to` 结构体的 RIP 字段）之间插入一条 `sti` 指令。位置很关键：必须在切换栈之后、跳转之前。如果放在切换栈之前，sti 执行时还在旧任务的栈上，万一此时中断到来就会把中断帧压入错误的栈。

为什么这条 sti 是安全的？对于新任务，sti 正确地开启了中断，后续的定时器中断能正常到达。对于被抢占过的老任务，它恢复执行后最终会通过 IRETQ 恢复原始 RFLAGS，sti 只是多此一举但不会造成错误。至于嵌套中断的风险——从 `.restore` 到 IRETQ 的退栈路径只有几条指令（微秒级），100Hz 的定时器间隔是 10ms，在这个窗口中命中下一个 IRQ0 的概率可以忽略不计。

参考：

- Intel SDM Vol.3A Section 2.3 — RFLAGS.IF flag (bit 9)
- Intel SDM Vol.3A Section 6.12.1.3 — Interrupt Gate IF clearing
- Intel SDM Vol.3A Section 6.8.1 — Masking Maskable Hardware Interrupts

### 时间片过长的教训

在 QEMU TCG（软件模拟）模式下，CPU 密集型循环的执行速度比直觉中快得多。一个 100 万次的空循环（`for (volatile int j = 0; j < 1000000; j++) {}`）在 TCG 模式下可能只需要 3-5ms。如果你的时间片设成 100ms，三个线程各跑 5 次迭代总共才 15-25ms——全都在各自的时间片内跑完了，抢占永远不会发生。

修复方案有两个：把 DEFAULT_TIME_SLICE 从 10 改成 2（20ms），同时把忙循环迭代次数从 100 万提升到 2000 万。两者配合才能确保每个线程跨越多个时间片，让抢占有足够的机会介入。

为什么用 `volatile int j` 作为忙循环变量？`volatile` 告诉编译器不要优化掉这个循环——没有 volatile，编译器可能认为循环没有副作用而直接删除它，或者把迭代次数从 2000 万优化成 1 次。`int j = 0; j < 20000000; j++` 在 x86-64 上会被编译成 `inc` + `cmp` + `jl` 三条指令的循环，在 QEMU TCG 模式下大约每秒能执行 1-2 亿次迭代。2000 万次迭代大约需要 100-200ms，足以跨越多个 20ms 的时间片。

## 动手实现

### Step 1: 将 tick() 接入 PIT IRQ0 handler

**目标**: 在 `kernel/drivers/pit/pit.cpp` 的 irq0_handler 中调用 Scheduler::tick()。

**设计思路**: 在 irq0_handler 的末尾，PIC::send_eoi(0) 之后，加一行 `cinux::proc::Scheduler::tick()`。需要 include `kernel/proc/scheduler.hpp` 头文件。

注意 tick() 内部会检查 is_initialized()——所以即使在调度器初始化完成之前有 IRQ0 到来（不太可能，因为我们先初始化调度器再 sti，但防御性编程不嫌多），tick() 也不会崩溃，只是静默返回。

**实现约束**: 只需要在 pit.cpp 中加一个 include 和一行函数调用。不要在 send_eoi 之前调用 tick()——前面已经解释过原因。

**验证**: 编译通过。

### Step 2: 在 context_switch.S 中添加 sti 指令

**目标**: 修复 IF 标志丢失的 bug，在新任务启动前开启中断。

**设计思路**: 在 context_switch.S 中找到切换栈指针的那行（`movq 48(%rsi), %rsp`），在它和跳转到新任务的那行（`jmp *56(%rsi)`）之间插入一条 `sti` 指令。加一条注释说明为什么需要这条指令——"我们可能从中断上下文进入（IF=0），新任务必须以中断开启状态启动"。

**实现约束**: sti 只有一条指令，不需要操作数。注释风格和文件中其他注释保持一致（`/* */` 块注释）。

**踩坑预警**: 如果你把 sti 放在 `movq 48(%rsi), %rsp` 之前（切换栈之前），那么 sti 执行时还在旧任务的栈上，如果此时一个中断到来，中断帧会压入旧任务的栈——而旧任务的寄存器已经被保存了但还没切换栈指针，栈帧布局会乱掉。虽然概率极低（sti 和下一句之间只有几个时钟周期），但正确的位置是切换栈之后、跳转之前。

**验证**: 编译通过。

### Step 3: 更新 kernel_main——调度器初始化顺序和演示线程

**目标**: 重写 kernel_main 中的调度器相关代码，适配抢占式调度。

**设计思路**: 这里有几个重要的改动。第一是初始化顺序：Scheduler::init() 和所有任务的创建必须在 sti（开中断）之前完成。如果在开中断之后再创建任务，可能遇到 tick() 在任务创建过程中就被调用的竞态。第二是演示线程从 2 个变成 6 个，使用统一的 worker 函数——每个线程跑 10 次迭代，每次迭代包含一个 2000 万次的忙循环来确保跨越多个时间片。

演示线程的入口函数 worker 接受一个字符标签和迭代次数，循环中打印当前线程的 tid 和迭代序号，然后执行忙循环。6 个线程分别命名为 thread_a 到 thread_f，标签为 A 到 F。创建后全部通过 add_task 注册到调度器。worker 函数不调用 yield——线程函数完全是 CPU 密集型的工作循环，调度完全由定时器中断驱动。

第三处改动是 include `kernel/proc/per_cpu.hpp`——虽然 main.cpp 中不直接使用 g_per_cpu，但保持 include 的一致性是个好习惯。

**实现约束**: 调度器初始化和任务创建必须在 `__asm__ volatile("sti")` 之前。boot_task 的构造和 run_first() 调用保持不变——它们是启动调度器主循环的入口。

**目标**: 重写 kernel_main 中的调度器相关代码，适配抢占式调度。

**设计思路**: 这里有几个重要的改动。第一是初始化顺序：Scheduler::init() 和所有任务的创建必须在 sti（开中断）之前完成。如果在开中断之后再创建任务，可能遇到 tick() 在任务创建过程中就被调用的竞态。第二是演示线程从 2 个变成 6 个，使用统一的 worker 函数——每个线程跑 10 次迭代，每次迭代包含一个 2000 万次的忙循环来确保跨越多个时间片。

演示线程的入口函数 worker 接受一个字符标签和迭代次数，循环中打印当前线程的 tid 和迭代序号，然后执行忙循环。6 个线程分别命名为 thread_a 到 thread_f，标签为 A 到 F。创建后全部通过 add_task 注册到调度器。

第三处改动是 include `kernel/proc/per_cpu.hpp`——虽然 main.cpp 中不直接使用 g_per_cpu，但保持 include 的一致性是个好习惯。

**实现约束**: 调度器初始化和任务创建必须在 `__asm__ volatile("sti")` 之前。boot_task 的构造和 run_first() 调用保持不变——它们是启动调度器主循环的入口。

**踩坑预警**: 如果你把 `Scheduler::init()` 放在 `sti` 之后，第一个 IRQ0 可能在 init() 完成之前就到了——tick() 检查 initialized_ 发现是 false 就直接返回，看起来没问题。但如果你在 init() 中间（idle 任务还没创建、default_rr_ 还没注册）收到了中断，tick() 不会崩溃但它检查不到的中间状态可能导致后续逻辑出错。所以最安全的做法是：先完成所有初始化，最后再 sti。

**验证**: 编译并运行，检查抢占式交替输出。

**踩坑预警**: worker 函数中的 `volatile int j` 不要漏掉 volatile。如果漏了，编译器可能在 -O2 优化级别下把循环完全删除，导致线程在第一次迭代就立刻完成，看不到任何抢占效果。检查编译生成的汇编代码中是否存在这个循环：`objdump -d build/big_kernel | grep -A5 "worker"` 应该能看到 loop 结构。

### Step 4: 编译并运行完整测试

**目标**: 编译内核并在 QEMU 中验证抢占式调度。

```bash
cmake --build build --target big_kernel && \
  qemu-system-x86_64 -kernel build/big_kernel.bin -serial stdio -display none \
    -no-reboot 2>&1 | grep -E '\[A-F\]|\[SCHED\]|\[BIG\]' | head -80
```

预期输出中，你应该看到 6 个线程的输出是交错的——不再是 A 全部跑完再跑 B，而是类似 A 跑 1-2 次后切到 B，B 跑 1-2 次后切到 C，如此循环。每次切换大约间隔 20ms（2 个 tick）。

**验证成功的标志**: 输出中同一个线程的连续迭代之间插入了其他线程的输出。比如你看到 `[A] iter 1/10` 后面紧跟的不是 `[A] iter 2/10` 而是 `[B] iter 1/10` 或 `[C] iter 1/10`，说明抢占成功了。

如果输出是完全顺序的（A 全部跑完才跑 B），说明时间片还是太长或者 tick() 根本没被调用——检查 pit.cpp 中是否正确调用了 tick()，以及 DEFAULT_TIME_SLICE 是否为 2。如果只有第一个被抢占的线程能恢复而后续线程顺序跑完，说明 sti 修复没生效——检查 context_switch.S 中 sti 的位置。

## 构建与运行

如果一切正确，你看到的输出大致是这样的（每次运行具体交错模式可能不同）：

```
[BIG] Starting preemptive demo (6 threads x 10 iters, timer-driven)...
[BIG] IRQ0+IRQ1 unmasked, enabling interrupts...
[BIG] Interrupts enabled.
[A] tid=2 iter 1/10
[A] tid=2 iter 2/10
[B] tid=3 iter 1/10
[B] tid=3 iter 2/10
[C] tid=4 iter 1/10
...
[A] tid=2 iter 3/10
...
[F] done
[A] done
```

每个线程的两次迭代之间大约间隔 20ms（2 个 tick），6 个线程轮流使用 CPU。线程完成后它的 exit_current() 被调用，从运行队列中移除，剩下的线程继续轮转。当所有线程都退出后，调度器切换到 idle 任务，CPU 进入 hlt 状态。

## 调试技巧

### 时间片过长的调试过程

当你发现线程完全顺序执行没有任何交错时，调试步骤应该是这样的。第一步，在 tick() 入口加一行 kprintf，确认 tick() 是否被调用。如果没被调用，检查 pit.cpp 中是否正确连接了 Scheduler::tick()。如果被调用了，第二步在 tick() 中打印 current_slice_ 的值。如果 current_slice_ 在增长但从未达到 DEFAULT_TIME_SLICE，说明时间片太长或工作负载太轻。第三步，缩小 DEFAULT_TIME_SLICE 并增大忙循环迭代次数。

这个调试过程的核心思路是"二分法"——先确认信号链路的前半段（中断是否到达、tick 是否被调用），再确认后半段（时间片计数、schedule 是否触发）。不要一上来就去查 schedule() 或 context_switch 的实现——大多数情况下问题出在更简单的参数配置上。

### IF 标志丢失的调试过程

当你发现第一次抢占成功但后续线程不再被抢占时，调试思路完全不同。第一步，确认 tick() 是否还在被调用——在被抢占后 B 线程的执行过程中，tick() 是否仍然被定期调用？如果 tick() 没被调用，说明中断被关了。第二步，如果 tick() 确实没被调用，检查 IF 标志。你可以在 B 线程的入口处加一行内联汇编读取 RFLAGS 的第 9 位并打印。如果 IF=0，说明新任务启动时中断就被关了。第三步，回溯调用链：IRQ0 -> ISR stub (IF=0) -> tick() -> schedule() -> context_switch -> jmp -> B 线程入口。问题出在 jmp 之前没有恢复 IF。

这个 bug 的关键诊断步骤是在纸上画出两条恢复路径：新任务走 `jmp -> entry`（不经过 IRETQ），老任务走 `.restore -> ret -> ISR stub -> IRETQ`（恢复 IF=1）。一旦画清楚这两条路径，sti 的修复方案就显而易见了。

**运行后线程完全顺序执行，没有任何交错**: 这可能是时间片过长的 bug（详见 note 001）。确认 DEFAULT_TIME_SLICE 为 2（不是 10），忙循环迭代次数为 2000 万（不是 100 万）。在 QEMU TCG 模式下，100 万次循环的执行时间不到 5ms，远小于 20ms 的时间片。

**第一次抢占成功后后续线程不再被抢占**: 这就是 IF 标志丢失的 bug（详见 note 002）。确认 context_switch.S 中在 `movq 48(%rsi), %rsp` 和 `jmp *56(%rsi)` 之间有一条 `sti` 指令。如果没有，新任务启动时 IF=0，收不到定时器中断，永远不会被抢占。

**系统启动后立刻 triple fault**: 可能是 context_switch.S 的修改破坏了汇编格式——检查 sti 指令的缩进是否正确（AT&T 汇编中缩进不影响功能，但如果你不小心多敲了一个字符导致指令不合法，as 汇编器会报错）。用 `cmake --build build 2>&1 | grep error` 检查编译阶段是否有汇编错误。

**IRQ0 频率不对**: 如果感觉切换频率太高或太低，检查 PIT 的配置。PIT channel 0 的除数值决定了 IRQ0 的频率——除数值 = 1193182 / 目标频率。100Hz 对应除数值 11931。确认 pit.cpp 中的除数值计算正确。

**编译报错 "Scheduler::tick is not declared in this scope"**: pit.cpp 需要 include `kernel/proc/scheduler.hpp`。如果你用的是相对路径 include，确保路径正确。在这个项目中，所有 include 都使用从项目根目录开始的绝对路径（`#include "kernel/proc/scheduler.hpp"`），而不是相对路径（`#include "../proc/scheduler.hpp"`）。

**sti 修复后系统仍然只有第一个线程能被抢占**: 检查 sti 指令的位置是否正确。它必须在 `movq 48(%rsi), %rsp`（切换栈）之后、`jmp *56(%rsi)`（跳转到新任务）之前。如果 sti 在 movq 之前，中断可能在旧任务的栈上触发，导致栈帧损坏。如果 sti 在 jmp 之后，那它永远不会被执行——jmp 不返回。

**运行后系统立刻 triple fault**: 可能是 context_switch.S 的修改破坏了汇编格式。用 `cmake --build build 2>&1 | grep error` 检查编译阶段是否有汇编错误。特别注意 sti 指令的缩进是否正确（AT&T 汇编中缩进不影响功能，但如果你不小心多敲了一个字符导致指令不合法，as 汇编器会报错）。也可以用 `objdump -d build/big_kernel | grep -B2 -A2 "sti"` 检查 sti 是否出现在 context_switch 函数的正确位置。

## 设计决策

**sti 为什么放在 context_switch 而不是 ISR stub 里？** 一种替代方案是在 ISR stub 中、调用 context_switch 之后再 sti——如果发生了切换的话。但这需要 ISR stub 知道是否发生了切换，增加了 ISR 和调度器之间的耦合。而且 ISR stub 要怎么知道"是否发生了切换"？要么让 context_switch 返回一个 bool（改变了函数签名），要么引入一个全局标志（引入了额外的共享状态）。这两种方案都不如把 sti 放在 context_switch 内部来得干净。把 sti 放在 context_switch 内部，紧挨着栈切换之后，是语义最清晰的位置："我已经在新任务的栈上了，该让中断恢复工作了。"

**为什么不把 RFLAGS 加入 CpuContext？** 一种更"完整"的修复方案是在 CpuContext 中加一个 RFLAGS 字段，用 pushfq/popfq 保存和恢复。这样不仅能解决 IF 问题，还能正确处理其他标志位（DF、AF 等）。但对当前场景来说这是过度设计——内核代码不会假设某个标志位的初始值，而且多保存/恢复一个 RFLAGS 会增加 CpuContext 的大小（从 64 字节变成 72 字节），context_switch.S 中所有偏移量都要调整。一条 sti 是最简洁的修复。如果未来引入用户态任务，那时再考虑完整的状态保存也不迟。

**6 线程 2000 万次迭代的负载设计？** 这是调试时间片过长问题后得出的参数。初始版本用 3 个线程 x 5 次迭代 x 100 万次忙循环，在 100ms 时间片下线程在自己的量子内就跑完了。增加到 6 线程 x 10 次迭代 x 2000 万次忙循环后，每个线程至少需要 3-5 个时间片才能完成，能充分验证抢占式轮转的正确性。在虚拟化环境中，CPU 密集循环比物理机快得多，测试参数必须留出足够的余量。

## 本章小结

这一篇是整个 tag 020 的收尾和最高潮。我们把 PIT 的 IRQ0 handler 和调度器的 tick() 连接了起来，让定时器真正驱动任务调度；修复了 context_switch 中丢失 IF 标志的经典 bug——一条 sti 指令解决了新任务永远无法被抢占的问题；更新了 kernel_main 的演示代码，用 6 个 CPU 密集型线程验证了抢占式调度的正确性。

回顾整个 tag 020 的三个阶段：第一篇搭建了 Spinlock、PerCPU 和 TSS RSP0 更新的基础设施；第二篇实现了 tick()、schedule()、block/unblock 和 idle 任务的完整调度逻辑；第三篇完成了定时器集成、修复了两个关键的调试坑（时间片过长和 IF 标志丢失），并做了完整的集成验证。到这一步，Cinux 已经拥有了一个功能完整的抢占式调度器——线程不再需要主动 yield，定时器中断会在固定的时间间隔后强制切换任务。

这是操作系统开发中一个相当重要的里程碑。当然，和真实的操作系统相比我们的调度器还非常原始——没有优先级调度、没有 CFS、没有 load balancing、没有 NUMA 感知——但对于一个教学内核来说，一个能工作的抢占式 RoundRobin 调度器已经足以让你理解多任务操作系统最核心的机制了。

回顾整个 tag 020 的三个阶段：第一篇搭建了 Spinlock、PerCPU 和 TSS RSP0 更新的基础设施；第二篇实现了 tick()、schedule()、block/unblock 和 idle 任务的完整调度逻辑；第三篇完成了定时器集成、修复了两个关键的调试坑（时间片过长和 IF 标志丢失），并做了完整的集成验证。

从代码量来看，整个 tag 020 新增了约 521 行代码，涉及 19 个文件的变更。核心变更集中在 kernel/proc 子系统（sync.hpp、per_cpu.hpp、scheduler.hpp/cpp）和 arch/x86_64（context_switch.S、gdt.hpp/cpp），以及 PIT 驱动的集成（pit.cpp）和演示代码的更新（main.cpp）。

下一个 tag 021 会在这些基础设施上构建 Mutex 和 Semaphore，实现真正的 producer-consumer 同步。到那时，block/unblock 机制就会真正发挥作用——线程在等待锁时不再忙等，而是主动让出 CPU 进入 Blocked 状态，等锁可用时被唤醒。这将是 Cinux 并发能力的又一次质变。
