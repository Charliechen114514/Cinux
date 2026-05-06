# 从零在 x64 内核里驱动 PIC 和 PIT（三）：集成、点火与验证

> 标签：x86-64, 内核开发, QEMU, 中断, 定时器, C++
> 前篇：[011-2 PIT 与 IRQ 注册](011-big-kernel-pic-irq-2.md) | 篇首：[011-1 PIC 驱动](011-big-kernel-pic-irq-1.md)

---

## 第五步——kernel_main 里串起来，点火！

所有组件就绪后，在 `kernel/main.cpp` 里按正确顺序把它们串起来：

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[BIG] GDT loaded.\n");

    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded.\n");

    PIC::init();
    cinux::lib::kprintf("[BIG] PIC initialised.\n");

    irq_init();

    PIT::init(100);

    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    PIC::unmask(0);
    cinux::lib::kprintf("[BIG] IRQ0 unmasked, enabling interrupts...\n");
    __asm__ volatile("sti");
    cinux::lib::kprintf("[BIG] Interrupts enabled. Entering idle loop.\n");

    while (1) {
        __asm__ volatile("hlt");
    }
}
```

初始化顺序是一条严格依赖链，每一步都有前置条件。kprintf_init 最先，因为后续步骤需要日志输出；GDT 其次，因为 IDT 条目引用 GDT 中的代码段选择子；IDT 再次，因为 IRQ 注册需要 IDT 已经存在；PIC init 在 IDT 之后，因为 PIC 重映射的向量号必须有对应的 IDT 条目才能被正确处理；irq_init 在 PIC 之后，因为它往 IDT 里注册 IRQ handler；PIT init 在 irq_init 之后，因为 PIT 配好后中断信号随时可能到达，handler 必须已经就位。最后 unmask IRQ0 + sti 开中断——到这一步，定时器中断才真正开始流动。

你可能注意到我们在 sti 之前还触发了一次 `int $3` 断点。这是从 milestone 010 继承下来的回归测试——确认 CPU 异常处理在 PIC/IRQ 注册之后仍然正常工作。如果 PIC 初始化过程中不小心覆盖了 IDT 的某些条目（实际上不会，因为 PIC 只操作自己的 I/O 端口），这个断点测试会立刻暴露问题。

idle 循环用 `hlt` 而不是裸的 `while(1)` 是有讲究的。`hlt` 指令让 CPU 进入低功耗状态直到下一个中断到来，和 `while(1)` 的忙等待相比能显著降低 CPU 占用率（在 QEMU 里体现为宿主机的 CPU 使用率）。每次 IRQ0 到来时 CPU 被唤醒，执行完 handler 后又回到 `hlt`，周而复始。

## 上板验证

构建运行后，串口输出应该是这样的：

```
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] PIC initialised.
[IRQ] Registering IRQ handlers (0x20-0x2F)...
[IRQ] All IRQ handlers registered.
[PIT] Initialised at 100 Hz (divisor=11931)
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0xffffffff80XXXXXX   CS  = 0x0008
  ...
========================================
[EXCEPTION] Breakpoint at RIP=0xffffffff80XXXXXX
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
[BIG] IRQ0 unmasked, enabling interrupts...
[BIG] Interrupts enabled. Entering idle loop.
[TICK] uptime: 1s
[TICK] uptime: 2s
[TICK] uptime: 3s
[TICK] uptime: 4s
...
```

看到 `[TICK] uptime: Ns` 每秒稳定输出一行，就说明整条链路完全通畅：PIT 硬件以 100 Hz 产生方波 -> IRQ0 信号到达 master PIC -> PIC 把它转译成 INT 0x20 -> CPU 查 IDT 向量 0x20 -> 跳到 `irq0_stub` -> 保存寄存器 -> 调用 `pit_irq0_handler` -> 递增 tick_count -> 每秒打印 uptime -> `PIC::send_eoi(0)` -> ISR stub 恢复寄存器 -> IRETQ -> 回到 hlt 等待下一次中断。

断点测试也正常返回，说明 PIC/IRQ 的加入没有破坏之前 milestone 010 搭好的 CPU 异常处理链路——两者和平共处，各管各的向量号范围。

如果你在验证过程中遇到问题，这里有几个关键的诊断步骤：

1. 如果串口输出在 `[BIG] PIC initialised.` 之后就停止了，很可能是 `irq_init()` 里注册 IDT 条目时出了问题。检查路由表中的向量号是否是 0x20-0x2F，以及 `kIRQAttr` 是否正确生成了 Interrupt Gate 的属性字节。

2. 如果输出到 `Interrupts enabled. Entering idle loop.` 之后就没有 tick，说明中断信号没有到达 CPU。首先检查 `PIC::unmask(0)` 是否被调用了——PIC 初始化后所有 IRQ 都是屏蔽状态，不 unmask 就不会有中断信号通过。其次检查 `sti` 是否执行了——没有 `sti` CPU 的 IF 标志就是 0，即使 PIC 转发了中断信号 CPU 也会忽略。

3. 如果 tick 来了但只来了一次就再也不来了，99% 的原因是 `PIC::send_eoi(0)` 被遗漏了。在 `irq0_handler` 的末尾确认有这个调用。

## 设计决策回顾：从 PIC 到 APIC 的过渡准备

你可能会问——既然 8259A 已经是老古董了，为什么还要花时间写它的驱动？

答案在于分层和渐进。SerenityOS 直接使用 APIC 而非 8259A，其中断系统用 `GenericInterruptHandler` 基类实现动态注册，每个 CPU 有独立的 GDTR/IDTR，支持 SMP。APIC 的编程模型比 8259A 复杂得多——它使用 MMIO 而不是 I/O 端口，有多组寄存器需要配置，支持多核中断路由，还有 local APIC 和 I/O APIC 两层架构。如果我们一上来就搞 APIC，需要同时解决 MMIO 映射、ACPI 表解析、多核初始化三个维度的问题，调试难度会指数级增长。

先用 8259A 把中断驱动的框架搭好——PIC 初始化、IRQ 注册、handler 编写、EOI 信号、mask/unmask——这些概念在 APIC 里完全一样，只是编程接口不同。等框架跑通了，后面迁移到 APIC 只需要把底层 I/O 端口操作换成 MMIO 读写，上层的 handler 注册和 EOI 逻辑基本不用改。同理，PIT 到 HPET/LAPIC Timer 的迁移也是类似——上层接口（`get_ticks()`、`get_uptime_ms()`）保持不变，只换底层的硬件编程。这种"先简单后复杂"的分层策略在 OS 开发中非常实用，能把每次调试的范围控制在一个可管理的维度内。

### 关于 Interrupt Gate vs Trap Gate

你可能注意到上一章的 CPU 异常使用了 Trap Gate，而本章的硬件中断使用了 Interrupt Gate。两者的核心区别在于 CPU 进入 handler 时是否自动关中断：Trap Gate 不修改 IF，Interrupt Gate 清除 IF。

异常使用 Trap Gate 的原因是有时异常处理需要允许嵌套——比如 page fault handler 可能访问一个触发另一个 page fault 的内存地址。但硬件中断使用 Interrupt Gate 是为了防止中断嵌套导致的栈溢出和共享状态破坏。如果 PIT 100 Hz 的中断嵌套起来，每次中断在内核栈上压入约 120 字节的 InterruptFrame，10 次嵌套就是 1200 字节。在中断频率高、处理时间长的情况下，栈空间很快就会耗尽。

我们的 ISR stub 不做 FXSAVE/XRSTOR（不保存 FPU/SSE 状态），这在中断上下文中是安全的，因为 Interrupt Gate 保证了不会嵌套。如果将来要支持中断嵌套（比如让高优先级中断打断低优先级中断），就需要在中断入口处增加 FPU 状态保存，并且为每个中断上下文分配独立的内核栈。

### idle 循环的演进

我们的 idle 循环经历了从 milestone 010 的 `cli; hlt` 到现在的 `hlt` 的变化。在 milestone 010 中，`cli; hlt` 意味着关中断然后暂停 CPU——如果没有外部事件可以唤醒 CPU，它就永远不会醒来。这在当时是合理的，因为我们没有任何硬件中断源。但现在有了 PIT，`hlt` 就足够了——CPU 暂停后，PIT 的下一个 tick 会通过 IRQ0 唤醒它。

当将来引入更多设备驱动（键盘、磁盘、网络）后，idle 循环可以进一步改进——比如在 idle 时统计每个 IRQ 的触发次数作为性能监控，或者在 idle 时做一些低优先级的内核维护工作（比如清理缓存、回收内存）。Linux 的 idle 线程就是一个很好的参考——它不仅在 idle 时 halt CPU，还会根据系统的电源管理策略选择不同深度的睡眠状态（C-states），以降低功耗。

---

## QEMU 调试技巧

在验证过程中如果遇到问题，QEMU 提供了几个有用的调试选项：

1. **`-d int`**：追踪所有中断事件。这会在 QEMU 的日志输出中显示每次中断的向量号、类型（硬件/软件/异常）和目标地址。如果你看不到 `[TICK]` 输出，可以用这个选项确认 IRQ0 是否真的在以 100 Hz 的频率到达。

2. **`-d cpu_reset`**：追踪 CPU reset 事件（Triple Fault 会导致 reset）。如果你的内核在 sti 后立即重启，这个选项可以帮助你确认是 Triple Fault 还是其他原因。

3. **GDB 远程调试**：在 QEMU 启动参数中加入 `-s -S`，然后在另一个终端运行 `gdb` 并连接到 `target remote localhost:1234`。你可以在 `PIC::init`、`irq_init`、`PIT::init` 和 `irq0_handler` 处设断点，逐步跟踪初始化过程。

## 收尾

到这里，大内核终于"活"过来了。回顾一下 milestone 011 做了什么：用 C++ class 封装了 8259A PIC 驱动（ICW1-ICW4 重映射、EOI 信号、mask/unmask），封装了 8254 PIT 驱动（channel 0 square wave 模式、tick 计数、uptime 追踪），用数据驱动的路由表注册了 16 个 IRQ handler 到 IDT，最后在 kernel_main 里按正确顺序初始化所有组件、unmask IRQ0、sti 开中断。验证结果是串口每秒稳定输出 `[TICK] uptime: Ns`，CPU 第一次真正响应了外部硬件事件。

下一步的方向很明确——键盘驱动（IRQ1）让内核能接收用户输入，然后是更精细的时间管理（睡眠、定时器队列），再之后是进程调度。内核有了心跳，离"能和外部世界互动"就不远了。

## 参考资料

**Intel 手册**：
- Intel 64 and IA-32 Architectures SDM, Volume 3A
  - Section 6.14 — 64-Bit Mode Interrupt Handling
- https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

**OSDev Wiki**：
- [8259 PIC](https://wiki.osdev.org/8259_PIC) — PIC 重映射、ICW 序列、EOI 机制
- [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer) — PIT 8254 编程指南
- [Interrupts](https://wiki.osdev.org/Interrupts) — x86 中断机制总览

**其他 OS 实现**：
- SerenityOS `Kernel/Arch/x86_64/Interrupts/APIC.cpp` — APIC 中断系统（https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp）
