---
title: 020-proc-scheduler-3 · 进程调度
---

# 020-3 通读：context_switch sti 修复、PIT tick 集成与 6 线程抢占演示

## 概览

本文是 tag `020_proc_scheduler` 三篇通读教程的第三篇，也是最后一篇。前两篇我们看了 Spinlock/PerCPU/GDT 基础设施和 scheduler.cpp 的调度核心逻辑。这篇聚焦三个集成层面：`context_switch.S` 中新增的 `sti` 指令及其背后的 IF 标志丢失 bug、`pit.cpp` 如何在 IRQ0 handler 中接入 `Scheduler::tick()`、以及 `main.cpp` 中的 6 线程抢占式 demo。最后看新增的 `test_scheduler.cpp` 测试用例和 `big_kernel_loader.cpp` 的 higher-half 入口点修复。

## 架构图

```
    完整的抢占调度链路：
    ┌────────────────────────────────────────────────────────────────┐
    │  PIT 100Hz → IRQ0 → ISR stub (IF=0) → pit_irq0_handler       │
    │    → PIT::irq0_handler()                                       │
    │        ├── tick_count_++                                        │
    │        ├── PIC::send_eoi(0)    ← 先 EOI，再调度               │
    │        └── Scheduler::tick()                                   │
    │              └── schedule() → context_switch                    │
    │                    ├── 切换 RSP (新栈)                          │
    │                    ├── sti ← 开中断（修复 IF=0 问题）         │
    │                    └── jmp *rip (跳转到新任务)                  │
    │                                                                 │
    │  main.cpp 6 线程 demo:                                         │
    │    Scheduler::init() → add_task(A-F) → sti → run_first(boot)  │
    │    A/B/C/D/E/F 在定时器驱动下轮流执行，无需手动 yield          │
    └────────────────────────────────────────────────────────────────┘
```

## 代码精讲

### context_switch.S——那一行 sti 指令

这是 tag 020 中最关键的 bug fix，也是调试时间最长的改动。新增的代码只有一行：

```asm
    /* Switch to the new task's stack */
    movq 48(%rsi), %rsp              # to+48→%rsp: switch stack pointer

    /* Re-enable interrupts -- we may have entered from ISR context (IF=0).
       New tasks must start with interrupts on; resumed tasks will restore
       their original RFLAGS via IRETQ unwinding regardless. */
    sti

    /* Jump to the new task's saved instruction pointer */
    jmp *56(%rsi)                    # to+56→RIP: resume execution
```

在切换栈之后、跳转到新任务之前，插入了一条 `sti` 指令。为什么需要它？事情要从抢占式调度的调用链说起。当 IRQ0 触发时，CPU 通过中断门进入 ISR stub。Intel SDM Vol. 3A Section 6.12.1.3 明确指出："When accessing an exception- or interrupt-handling procedure through an interrupt gate, the processor clears the IF flag to prevent other interrupts from interfering with the current interrupt handler。"也就是说，进入 ISR 的那一刻 IF 被自动清零了。

在 tag 019 的协作式调度中，`context_switch` 总是从普通函数（`yield()`）中被调用的，IF 保持为 1，不存在问题。但 tag 020 的 `context_switch` 现在可以从 ISR 上下文中被调用——调用链是 IRQ0 -> `pit_irq0_handler` -> `Scheduler::tick()` -> `schedule()` -> `context_switch`。此时 IF=0。

问题出在 `context_switch` 只保存 callee-saved 寄存器，不保存 RFLAGS。当它跳转到新任务时，新任务继承了 IF=0 的状态。对于首次运行的新任务，`ctx.rip` 指向入口函数，`jmp *56(%rsi)` 直接跳转过去，IF 仍然是 0——这意味着该任务永远收不到定时器中断，永远不会被抢占。而对于被抢占后恢复运行的老任务，它们的恢复路径经过 `.restore` -> `ret` -> ISR stub -> `IRETQ`，`IRETQ` 会从栈上的中断帧还原原始 RFLAGS（其中 IF=1），所以老任务不受影响。

这个 bug 的表现非常诡异：只有第一个被抢占的任务能正常参与轮转，后续所有新启动的线程都带着 IF=0 运行到结束。添加 `sti` 之后，新任务以 IF=1 启动，定时器中断正常到达。对于老任务，`sti` 是无害的冗余操作——因为 `IRETQ` 最终会还原正确的 RFLAGS。

### pit.cpp——在 IRQ0 handler 中接入 tick

PIT 驱动的修改非常小但极为关键。新增了两行：

```cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    // Increment the global tick counter
    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);

    // Signal End-Of-Interrupt to the PIC so the next IRQ can arrive
    PIC::send_eoi(0);

    cinux::proc::Scheduler::tick();
}
```

首先递增 tick 计数器，然后发送 EOI 通知 PIC 可以接收下一个中断，最后调用 `Scheduler::tick()`。这里的顺序很重要——必须先 EOI 再调度。如果在 `schedule()` 之后再 EOI，那么 `context_switch` 切换到新任务后，PIC 仍然认为 IRQ0 正在被处理中，下一个定时器中断就无法到达。虽然 `context_switch` 内部的 `sti` 会重新打开 CPU 中断，但 PIC 那边的 ISR-in-service 位还没清除。

对比 xv6-riscv 的做法：`clockintr()` 先调用 `w_stimecmp()` 设置下一次时钟中断，然后调用 `yield()`。xv6 的时钟中断通过 S-mode 的 `stimecmp` 寄存器控制，不需要手动 EOI，所以顺序无关紧要。而 x86 的 PIC/APIC 模型要求显式 EOI，必须先完成这个操作再进行上下文切换。

### main.cpp——6 线程抢占式 demo

main.cpp 的改动体现了从协作式到抢占式的范式转换。首先是线程函数的重构：

```cpp
static void worker(const char label, int iters) {
    for (int i = 0; i < iters; i++) {
        cinux::lib::kprintf("[%c] tid=%u iter %d/%d\n", label,
                            cinux::proc::Scheduler::current()->tid, i + 1, iters);
        for (volatile int j = 0; j < 20000000; j++) {}
    }
    cinux::lib::kprintf("[%c] done\n", label);
}

static void thread_a() { worker('A', 10); }
static void thread_b() { worker('B', 10); }
static void thread_c() { worker('C', 10); }
static void thread_d() { worker('D', 10); }
static void thread_e() { worker('E', 10); }
static void thread_f() { worker('F', 10); }
```

对比 tag 019，这里有几个关键变化。第一，不再调用 `yield()`——线程函数完全是 CPU 密集型的工作循环，调度完全由定时器中断驱动。第二，引入了 2000 万次的忙循环 `for (volatile int j = 0; j < 20000000; j++) {}`。这个循环的设计有深意：`volatile` 防止编译器优化掉循环，2000 万次迭代保证每个线程需要多个时间片才能完成（DEFAULT_TIME_SLICE=2 对应 20ms，20M 次迭代在 QEMU 中大约需要 50-100ms）。第三，6 个线程从 A 到 F 覆盖了更多场景，更容易暴露调度器中的边界问题。

启动顺序也做了调整：

```cpp
    // Step 15: Initialise scheduler and create preemptive tasks
    cinux::proc::Scheduler::init();

    auto* task_a = cinux::proc::TaskBuilder()
        .set_entry(thread_a).set_name("thread_a").build();
    // ... task_b through task_f ...

    cinux::proc::Task* tasks[] = {task_a, task_b, task_c, task_d, task_e, task_f};
    for (auto* t : tasks)
        cinux::proc::Scheduler::add_task(t);

    cinux::lib::kprintf("[BIG] Starting preemptive demo (6 threads x 10 iters, timer-driven)...\n");

    // Step 16: Unmask IRQ0 (PIT timer) and IRQ1 (Keyboard), enable interrupts
    PIC::unmask(0);
    PIC::unmask(1);
    __asm__ volatile("sti");
```

在 tag 019 中，调度器初始化在 `sti` 之后。而 tag 020 把 `Scheduler::init()` 移到了 `sti` 之前。这是因为在抢占式调度中，定时器中断随时可能触发 `tick()`，而 `tick()` 依赖 `initialized_` 标志。如果先 `sti` 再 `init()`，第一次 IRQ0 到达时调度器还没准备好，虽然 `tick()` 内部有 `!initialized_` 检查不会崩溃，但会丢失 tick。调整顺序后，调度器在第一个中断到来之前就已经完全初始化了。

### test_scheduler.cpp——新增的调度器测试

tag 020 在 `test_scheduler.cpp` 中新增了三个测试用例，都在 `test_scheduler_new` 命名空间中：

```cpp
namespace test_scheduler_new {

void test_is_initialized() {
    Scheduler::init();
    TEST_ASSERT_TRUE(Scheduler::is_initialized());
}

void test_remove_task() {
    Scheduler::init();

    Task* task =
        TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("remove_test").build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);
    TEST_ASSERT_NOT_NULL(task->sched_class);

    Scheduler::remove_task(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Dead));
}

void test_block_unblock() {
    Scheduler::init();

    Task* task =
        TaskBuilder().set_entry(test_task_builder::dummy_entry).set_name("block_test").build();
    TEST_ASSERT_NOT_NULL(task);

    Scheduler::add_task(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Ready));

    Scheduler::block(task, "test block");
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Blocked));

    Scheduler::unblock(task);
    TEST_ASSERT_EQ(static_cast<int>(task->state), static_cast<int>(TaskState::Ready));
}

}  // namespace test_scheduler_new
```

三个测试分别验证了 `is_initialized()` 返回正确值、`remove_task()` 能将任务从队列中移除并标记为 Dead、以及 `block()`/`unblock()` 的状态转换。这些都是 tag 020 新增 API 的冒烟测试——它们在单次调用的粒度上验证行为正确性，不涉及真正的上下文切换。真正的抢占式调度验证由 `main.cpp` 中的 6 线程 demo 完成，因为上下文切换需要完整的 PMM/VMM/Heap 环境。

### big_kernel_loader.cpp——higher-half 入口点修复

Mini kernel loader 的改动不属于调度子系统，但它与 tag 020 一起提交。核心变更在 `big_kernel_loader.hpp`：

```cpp
/// Big kernel higher-half virtual base (must match kernel/linker.ld KERNEL_VMA)
constexpr uint64_t BIG_KERNEL_VMA = 0xFFFFFFFF80000000ULL;

/// Expected virtual entry point of the big kernel (_start)
constexpr uint64_t BIG_KERNEL_ENTRY_VADDR = BIG_KERNEL_VMA + BIG_KERNEL_LOAD_ADDR;
```

`load_big_kernel_phase2()` 现在返回虚拟入口地址而非物理地址。这是因为大内核运行在 higher-half，它的 `_start` 符号在链接时已经被分配了 `0xFFFFFFFF80000000` 以上的虚拟地址。mini kernel 在加载完成后直接跳转到这个虚拟地址，跳转目标已经在页表中映射好了。

同时，`main_test.cpp` 新增了对加载的内核是否为"真实内核"的检测：

```cpp
    auto* code           = reinterpret_cast<const uint8_t*>(big_kernel_entry);
    bool  is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) && (code[2] == 0xBC);
```

检查入口点的前三个字节是否是 `cli`（0xFA）+ `mov rsp, imm64`（REX.W MOVABS = 48 BC），用来区分真实内核和 stress test 的合成 ELF。这样在 CI 中运行 stress test 时不会误跳转到一段数据模式中。

## 设计决策

**sti 为什么加在 context_switch 而不是在 ISR stub 中？** 一种替代方案是在 ISR stub 中、调用 `context_switch` 之后（如果发生了切换）再 `sti`。但这需要 ISR stub 知道是否发生了切换——增加了 ISR 和调度器之间的耦合。把 `sti` 放在 `context_switch` 内部，紧挨着栈切换之后，是语义最清晰的位置："我已经在新任务的栈上了，该让中断恢复工作了。"这种自包含的设计让 `context_switch` 在任何上下文（ISR、普通函数）中调用都能正确工作。

**PIT 为什么先 EOI 再 schedule？** 如果先 `schedule()` 再 EOI，`context_switch` 切到新任务后 PIC 仍然处于 ISR-in-service 状态。虽然 `context_switch` 内的 `sti` 重新打开了 CPU 中断，但 PIC 不会发送下一个 IRQ0，因为上一个还没被确认。这会导致定时器中断"丢失"一个周期。先 EOI 确保 PIC 在 `context_switch` 发生之前就已经准备好接收下一个中断了。

**6 线程 2000 万次迭代的负载设计？** 这是调试时间片过长问题后得出的参数。初始版本用 3 个线程 × 5 次迭代 × 100 万次忙循环，在 100ms 时间片下线程在自己的量子内就跑完了。增加到 6 线程 × 10 次迭代 × 2000 万次忙循环后，每个线程至少需要 3-5 个时间片才能完成，能充分验证抢占式轮转的正确性。在虚拟化环境中，CPU 密集循环比物理机快得多，测试参数必须留出足够的余量。

## 参考资料

- Intel SDM Vol. 3A, Section 6.12.1.3 "Flag Usage By Exception- or Interrupt-Handler Procedure" (PDF page 213): 中断门自动清除 IF
- Intel SDM Vol. 3A, Section 6.8.1 "Masking Maskable Hardware Interrupts" (PDF page 203): IF 标志控制可屏蔽中断
- Intel SDM Vol. 3A, Section 6.12.1 "Procedure Calls and Returns for Exception- and Interrupt-Handling" (PDF page 209): IRETQ 恢复 RFLAGS
- OSDev Wiki, "Programmable Interval Timer": Intel 8253/8254 PIT 作为 IRQ0 定时源
- document/notes/020/002_if_flag_lost_in_context_switch.md: IF 标志丢失的完整调试记录
- document/notes/020/001_time_slice_too_long.md: 时间片过长问题的调试记录
