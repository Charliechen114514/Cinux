---
title: 011-big-kernel-pic-irq-3 · PIC 与 IRQ
---

# 011 PIC 重映射 + PIT 定时器 + IRQ 中断（三）：IRQ 注册与集成 —— 点火！

> 前篇：[011-2 PIT 驱动](011-big-kernel-pic-irq-2.md) | 篇首：[011-1 PIC 驱动](011-big-kernel-pic-irq-1.md)

---

## 动手实现

### 第五步——IRQ 汇编 Stub：复用 ISR_NOERRCODE 宏生成 16 个跳板

**目标**：在 interrupts.S 里添加 16 个 IRQ stub（irq0_stub 到 irq15_stub），复用上一章写好的 `ISR_NOERRCODE` 宏。

**设计思路**：所有硬件中断都没有硬件 error code，所以全部用 NOERRCODE 版本。IRQ0 的 handler 绑定 pit_irq0_handler，IRQ1-15 全部绑定 irq_default_handler（一个什么都不做只发 EOI 的兜底处理函数）。16 个 stub 的命名规律是 irq0_stub 到 irq15_stub，每个上方加注释标明对应的硬件设备和 INT 向量号。

文件路径：`kernel/arch/x86_64/interrupts.S`（在现有内容之后追加）

**踩坑预警**：irq_default_handler 目前只给 Master 发 EOI（PIC::send_eoi(0)），对 Slave PIC 的中断不完全正确。但当前阶段所有 Slave IRQ 都被屏蔽着，不会触发，这个问题暂时不会暴露。

**验证**：编译通过即可。

### 第六步——IRQ 路由表：数据驱动的 IDT 注册

**目标**：在 irq_handlers.cpp 里实现 irq_init() 函数，用一个 constexpr 路由表把 16 个 IRQ stub 注册到 IDT 的 0x20-0x2F 向量位置。

**设计思路**：

声明 16 个 extern "C" 的 ISR stub 函数（irq0_stub 到 irq15_stub）。定义一个 IRQRoute 结构体（uint8_t vector + IDT::Stub stub）和 constexpr 数组 k_irq_routes，把 IRQ0-15 映射到向量 0x20-0x2F。定义 kIRQAttr = make_idt_attr(IDTPrivilege::Kernel, IDTGateType::Interrupt)。irq_default_handler 只做 PIC::send_eoi(0)。irq_init() 遍历路由表调用 g_idt.set_handler()。

所有 IRQ 用 Interrupt Gate（进入 handler 自动关中断，防止嵌套），DPL=0（内核态特权级），IST=0。

文件路径：`kernel/arch/x86_64/irq_handlers.cpp`

**踩坑预警**：路由表里的向量号必须和 PIC init 设置的 offset 一致。如果 PIC 把 IRQ0 映射到 0x20 但路由表写的是 0x30，中断来了查 IDT 找不到 handler，直接 Triple Fault。

**验证**：编译通过即可。

### 第七步——kernel_main 串起来：PIC init → IRQ init → PIT init → unmask → sti → halt loop

**目标**：在 kernel_main 里按正确顺序调用 PIC::init()、irq_init()、PIT::init()，然后 unmask IRQ0、执行 sti 开中断，最后进入 hlt 空闲循环。

**设计思路**：

初始化顺序是一条严格依赖链，每一步都有前置条件：

1. 串口最先（kprintf_init）——后续所有 init 都需要日志输出。
2. GDT 第二——IDT 条目里的 selector 引用 GDT 代码段。
3. IDT 第三——IRQ stub 注册需要 IDT 已存在。
4. PIC 第四——重映射向量号。
5. irq_init 第五——往 IDT 注册 IRQ handler。
6. PIT 第六——配置定时器。PIT 配好后中断信号随时可能来，但 IRQ0 还被 mask 着，不会到达 CPU。
7. int $3 测试——回归测试，验证 CPU 异常处理在 IRQ 基础设施就位后仍正常工作。
8. PIC::unmask(0) + sti——unmask 打开 PIC 端门禁，sti 打开 CPU 端 IF 标志，两者都打开后 PIT 中断才能真正到达 CPU。
9. while(1) { hlt; }——CPU 低功耗等待中断，每次 IRQ0 到来被唤醒处理完后继续 hlt。

idle 循环从上一章的 `cli; hlt` 改成了单纯的 `hlt`，因为现在有了 PIT 定时器，用 hlt 让 CPU 等中断就行——hlt 状态下 CPU 几乎不耗电，不会占用总线带宽。

文件路径：`kernel/main.cpp`

**踩坑预警**：顺序绝对不能乱。如果先 PIT init 再 irq_init，PIT 配好后中断信号来了但 IDT 里还没注册 handler，直接 Triple Fault。如果先 sti 再 unmask，CPU 开了中断但 PIC 还在屏蔽 IRQ0，中断信号被 PIC 拦截，看不到 tick。如果忘了 sti 直接进入 hlt loop，CPU 永远不会醒来。

**验证**：这是我们可以第一次看到完整输出的步骤。构建运行后，串口应该输出：

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
  RIP   = 0x...    CS  = 0x0008
  ...（寄存器 dump）...
========================================
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
[BIG] IRQ0 unmasked, enabling interrupts...
[BIG] Interrupts enabled. Entering idle loop.
[TICK] uptime: 1s
[TICK] uptime: 2s
[TICK] uptime: 3s
...
```

每秒稳定出现一行 `[TICK] uptime: Ns`，直到 Ctrl+C 关闭 QEMU。

如果验证过程中遇到问题，以下是三个最常见的排查方向：

1. **没有任何输出**——可能是串口初始化失败或者 CMake 构建有误。检查 `cmake --build build` 是否有编译错误。
2. **有初始化输出但卡在 `[BIG] IDT loaded.` 之后**——可能是 PIC init 导致了 crash。检查 `pic.cpp` 中的 io_wait() 调用是否都正确。
3. **有初始化输出和断点输出但没有 tick**——可能是 unmask 或 sti 的顺序有误，或者 `send_eoi(0)` 被漏掉了。

---

## 中断门 vs 陷阱门：为什么 IRQ 用 Interrupt Gate

在第六步的路由表中，我们为所有 IRQ 选择了 `IDTGateType::Interrupt`（中断门）而不是 `IDTGateType::Trap`（陷阱门）。这个选择值得展开说明。

两者的区别在于 CPU 的行为：进入 Interrupt Gate handler 时，CPU 自动清除 RFLAGS 的 IF 标志（关中断），handler 执行期间不会被新的可屏蔽中断打断；进入 Trap Gate handler 时，IF 标志不变，如果之前 IF=1 则 handler 执行期间可以被新的中断打断。

对于 CPU 异常（#BP、#GP 等），上一章使用了 Trap Gate，因为异常处理通常很快且有时需要允许嵌套（比如在 page fault handler 中访问可能触发另一个 page fault 的内存）。但对于硬件中断，我们选择 Interrupt Gate，原因是：

1. **防止中断嵌套导致的栈溢出**。如果 PIT 的 IRQ0 handler 正在执行时又来了一个 IRQ0，Interrupt Gate 会自动关中断，阻止嵌套。如果允许嵌套，每次中断都会在内核栈上压入一个 InterruptFrame（约 120 字节），高频中断下栈空间很快就会耗尽。

2. **保护共享状态**。`tick_count_++` 虽然在 64 位系统上是原子的，但 `kprintf` 不是可重入的——如果两个 IRQ0 handler 嵌套执行 `kprintf`，串口输出会乱成一团。

3. **EOI 时序的确定性**。手动 EOI 模式要求 handler 在"所有工作完成后"才发送 EOI。如果允许嵌套，内层 handler 的 EOI 可能在内层处理完成前就让 PIC 放行了新的中断，破坏了 EOI 的时序保证。

当然，Interrupt Gate 只阻止了可屏蔽中断（INTR）的嵌套，不阻止 NMI（Non-Maskable Interrupt）和 CPU 异常的嵌套。这是正确的行为——page fault 等异常在中断处理期间仍然需要被正确处理。

---

## 构建与运行

现在我们来构建并运行，看看内核的定时器中断是否真正工作。

```bash
# 从项目根目录
git checkout 011_big_kernel_pic_irq

# 配置 + 构建（Debug 模式）
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)

# 运行
cd build
make run
```

QEMU 启动后，你应该会看到初始化消息逐行打印，然后 `int $3` 触发一次断点异常（打印寄存器 dump 后恢复），接着出现 `Interrupts enabled. Entering idle loop.`，之后就是每秒一行的 `[TICK] uptime: Ns`。如果看到这些输出，说明 PIC 重映射、PIT 配置、IRQ 处理、EOI 发送这一整条链路全部正确工作了。

QEMU 的启动参数在上一章已经解释过了。这里需要特别关注的是 `-serial stdio`——因为我们的 tick 信息是通过 `kprintf` 往串口写的，所以终端上能直接看到。如果发现 tick 信息来得太慢或者太快，很可能是 PIT 除数计算有误——可以用 QEMU 的 `-d int` 参数来追踪中断事件，确认 IRQ0 的频率是否符合预期。

---

## 调试技巧

### 中断只来一次就再也不来了

这是这一章最最常见的 bug，99% 的原因就是忘了在 IRQ handler 里调用 `PIC::send_eoi()`。8259A 在收到 EOI 之前不会转发下一个同优先级或更低优先级的中断——所以如果 PIT 的第一个 tick 来了、处理了、但没发 EOI，PIC 就会认为 IRQ0 还在处理中，后续所有 tick 都被阻塞。更隐蔽的情况是 EOI 发了但发错了——比如应该发 `PIC::send_eoi(0)`（IRQ0 对应 Master PIC），结果写成了 `PIC::send_eoi(1)`（发给 IRQ1 了，IRQ0 还是被阻塞）。

排查方法：在 `PIT::irq0_handler` 的开头和末尾各加一个 `kprintf`，看是进入了处理函数就不出来了，还是处理函数正常返回了但下一个 tick 不来了。如果只在开头看到了一次打印、末尾没看到，说明 `kprintf` 本身有问题或者 `send_eoi` 之前就 crash 了。如果两头都看到了但还是没有第二个 tick，检查 `send_eoi` 的参数是不是正确的 IRQ 号。

### sti 之后立即 Triple Fault

如果 `sti` 执行后 QEMU 直接重启（或因 `-no-reboot` 停在 Shutdown），说明有一个中断来了，但 CPU 查 IDT 找不到有效的处理程序，于是触发 #GP，#GP 的处理函数 halt 了（或者 #GP 的 IDT 条目也不存在，那就 Double Fault → Triple Fault）。

最可能的原因是 `irq_init()` 没有被调用，或者 `irq_init()` 注册的向量号和 PIC 重映射的目标不一致。比如 PIC 把 IRQ0 重映射到了 0x20，但 `irq_init()` 把 `irq0_stub` 注册到了 0x30（向量号写错了），那 IRQ0 来的时候 CPU 去 IDT[0x20] 查，发现是空的（Present=0），直接 #GP。

排查方法：在 `irq_init()` 里把注册的向量号打印出来，确认是 0x20-0x2F。也可以在 `PIC::init()` 里打印 `master_offset_` 和 `slave_offset_`，确认是 0x20 和 0x28。这两个值必须对上。

### PIT 除数写反了（高字节先写低字节后写）

PIT 要求先写低字节再写高字节——如果你写反了，PIT 会把第一个字节当作高字节、第二个字节当作低字节来拼装除数。结果就是实际除数和你预期的不一样，中断频率完全错误。比如你想设置除数 11931（0x2E9B），低字节 0x9B 先写、高字节 0x2E 后写；如果写反了，PIT 看到的除数就变成了 0x9B2E = 39726，对应频率约 30 Hz，不是我们想要的 100 Hz。

排查方法：用 GDB 在 PIT::init 的两次 `io_outb` 处打断点，确认写入顺序。或者在 `kprintf("[PIT] Initialised at %u Hz (divisor=%u)\n", ...)` 里打印除数值，确认是 11931。

---

## 本章小结

| 类别 | 名称 | 说明 |
|------|------|------|
| 类 | `cinux::arch::PIC` | 8259A PIC 驱动：init/send_eoi/mask/unmask/disable_all |
| 类 | `cinux::drivers::PIT` | Intel 8254 PIT 驱动：init/irq0_handler/get_ticks/get_uptime_ms |
| 命名空间 | `PicPort` | PIC I/O 端口常量（0x20/0x21/0xA0/0xA1）|
| 命名空间 | `PicICW` | PIC ICW 位域常量（INIT/ICW4/8086）|
| 命名空间 | `PitHW` | PIT 硬件常量（端口 0x40/0x43、BASE_FREQ 1193182）|
| 函数 | `PIC::init()` | ICW1-4 完整初始化 + io_wait + mask 恢复 |
| 函数 | `PIC::send_eoi()` | EOI 信号，Slave IRQ 需要 Master+Slave 双发 |
| 函数 | `PIT::init()` | 配置 Channel 0 方波模式，写入 16 位除数 |
| 函数 | `PIT::irq0_handler()` | 递增 tick，每秒打印 uptime，发 EOI |
| 函数 | `irq_init()` | 数据驱动路由表，注册 IRQ0-15 到 IDT 0x20-0x2F |
| 函数 | `irq_default_handler()` | IRQ1-15 兜底处理，只发 EOI |
| 汇编 | 16 个 IRQ stub | 复用 ISR_NOERRCODE 宏生成 |
| I/O 端口 | 0x20/0x21 | Master PIC Command/Data |
| I/O 端口 | 0xA0/0xA1 | Slave PIC Command/Data |
| I/O 端口 | 0x40/0x43 | PIT Channel 0 Data / Command Register |
| 指令 | `sti` / `cli` / `hlt` | 开中断 / 关中断 / CPU 暂停等待中断 |

本章我们从零搭建了大内核的硬件中断处理基础设施——PIC 重映射让 IRQ 脱离 CPU 异常向量区域，PIT 配置让系统有了稳定的时间源，IRQ 路由表把汇编 stub 注册到 IDT，irq0_handler 统计 tick 并每秒报告 uptime。从这一章开始，我们的内核不再只是一个"能响应异常"的程序，而是一个"能感知时间流逝"的系统。

下一章我们会在这个定时器的基础上继续扩展——引入键盘输入（IRQ1）、串口输入（IRQ3/4），或者进一步改进定时器为完整的调度器 tick。所有这些工作的基础都在这一章打好了——PIC 已经重映射，IDT 里的 IRQ 向量已经注册好，新的设备驱动只需要写对应的 handler 函数、在 IDT 里替换掉默认的 `irq_default_handler` 就行。

## 参考资料

**Intel 手册**：
- Intel 64 and IA-32 Architectures SDM, Volume 3A, Section 6.12 -- IDT Gate Types
- Intel 64 and IA-32 Architectures SDM, Volume 3A, Section 6.14 -- 64-Bit Mode Interrupt Handling

**OSDev Wiki**：
- [Interrupts](https://wiki.osdev.org/Interrupts) -- x86 中断机制总览
- [8259 PIC](https://wiki.osdev.org/8259_PIC) -- PIC 重映射、ICW 序列、EOI 机制

**其他 OS 实现**：
- xv6 `picirq.c` -- 极简 PIC 初始化（https://github.com/mit-pdos/xv6-public/blob/master/picirq.c）
- Linux `arch/x86/kernel/i8259.c` -- 生产级 8259A 驱动（https://github.com/torvalds/linux/blob/master/arch/x86/kernel/i8259.c）
