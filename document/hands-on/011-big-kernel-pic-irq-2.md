---
title: 011-big-kernel-pic-irq-2 · PIC 与 IRQ
---

# 011 PIC 重映射 + PIT 定时器 + IRQ 中断（二）：PIT 驱动 —— 给内核装上心跳

> 前篇：[011-1 PIC 驱动](011-big-kernel-pic-irq-1.md) | 次篇：[011-3 IRQ 注册与集成](011-big-kernel-pic-irq-3.md)

---

## 从上一篇到本篇的衔接

上一篇我们完成了 PIC 驱动——`PIC::init()` 把 IRQ0-7 重映射到 INT 0x20-0x27、IRQ8-15 重映射到 INT 0x28-0x2F，`PIC::send_eoi()` 处理中断结束签收，`PIC::mask()`/`unmask()` 控制单条 IRQ 线的开关。PIC 驱动是"被动"的——它管理中断信号的转译和分发，但自身不产生任何中断。

本篇我们要配置一个"主动"产生中断的硬件——PIT（Intel 8254 定时器）。PIT 以固定频率产生方波信号，信号到达 PIC 的 IRQ0 引脚后，PIC 按照 init() 中设置的重映射规则把它转译成 INT 0x20，CPU 查 IDT 找到对应 handler 跳转执行。整条链路是：PIT 方波 -> IRQ0 -> PIC 重映射 -> INT 0x20 -> IDT 查表 -> ISR stub -> C handler。

---

## 概念精讲

### PIT：x86 的"心脏起搏器"

PIT（Programmable Interval Timer，Intel 8254）是 x86 平台上的定时器芯片，它有一个固定的输入时钟频率 1193182 Hz（约 1.193 MHz，这个奇怪的数字是 PC 原始设计里 NTSC 彩色副载波频率 3.579545 MHz 除以 3 得来的），通过设置一个 16 位的除数（divisor），可以得到任意频率的中断输出：`输出频率 = 1193182 / divisor`。

PIT 有三个通道（Channel 0/1/2），其中 Channel 0（I/O 端口 0x40）直接连接到 IRQ0——也就是说，我们只要配置好 Channel 0，每到一个 tick，PIC 就会给 CPU 发一个 IRQ0 中断。Channel 1 是 RAM 刷新用的（别碰），Channel 2 连着 PC 喇叭（可以用来发蜂鸣声，但不影响中断）。

配置 Channel 0 的步骤是：先往 Command Register（端口 0x43）写命令字节 `0x36`（选择 Channel 0、LSB-then-MSB 读写模式、方波生成模式 Mode 3、二进制计数），然后把除数拆成低字节和高字节，依次写入 Channel 0 数据端口（0x40）。

```
PIT 命令字节 0x36 的位域分解：

Bit 7-6: 00 = 选择 Channel 0
Bit 5-4: 11 = LSB 然后 MSB（先写低字节再写高字节）
Bit 3-1: 110 = Mode 3（方波生成器，square wave）
Bit 0:   0 = 二进制计数（非 BCD）

最终值: 00_11_110_0 = 0x36
```

我们选择 100 Hz 的频率（除数 = 1193182 / 100 = 11931），意味着每 10 毫秒产生一次中断。这是一个比较常见的选择——Linux 内核早期也用 100 Hz（HZ=100），后来提高到 250 Hz、300 Hz 甚至 1000 Hz。频率越高计时精度越好，但中断开销也越大。对于我们这个教学内核，100 Hz 完全够用。

PIT 有 6 种工作模式（Mode 0 到 Mode 5），其中 Mode 3（Square Wave Generator，方波生成器）是定时器应用中最常用的。Mode 3 的工作原理是：计数器从 divisor 值开始递减，到 0 时输出一个方波的上升沿（触发 IRQ0），然后自动重新加载 divisor 继续计数——如此周而复始。偶数 divisor 会产生完美的 50% 占空比方波，奇数 divisor 会有一个时钟周期的偏差，但在 100 Hz 这种低频下完全可以忽略。Mode 2（Rate Generator）也很常见，它在计数器到 1 时（而非 0）触发输出，适合需要精确周期但不需要方波对称性的场景。

关于 PIT 的 BASE_FREQ 为什么是 1193182 而不是一个整数——这个数字的由来涉及 PC 的硬件设计史。原始 IBM PC 使用 14.31818 MHz 的基振荡器（这是 NTSC 彩色电视信号的副载波频率，大量生产所以便宜）。这个时钟经过一个三分频器得到 4.77273 MHz 给 8088 CPU，同时经过一个十二分频器（先三分频再四分频）得到 1.193182 MHz 给 8254 PIT。IBM 的设计师为了节省一个晶振的成本，创造了一个永远无法取整的时钟频率。

### AT&T 汇编语法速查

这一章涉及的新汇编指令不多，主要就是 `sti`（Set Interrupt Flag，开中断）和 `hlt`（Halt，CPU 暂停直到下一个中断到来）。如果你跟着上一章走过来的，ISR stub 的宏我们完全复用，不需要写新的汇编代码。

| 操作 | AT&T 语法 | 含义 |
|------|-----------|------|
| 开中断 | `sti` | 设置 RFLAGS.IF=1，允许 CPU 响应可屏蔽中断 |
| 关中断 | `cli` | 清除 RFLAGS.IF=0，禁止 CPU 响应可屏蔽中断 |
| CPU 暂停 | `hlt` | CPU 停止执行直到下一个中断到来 |
| 空闲循环 | `sti; hlt`（组合） | 开中断后暂停，等待下一个中断唤醒 CPU |

---

## 动手实现

### 第三步——PIT 驱动头文件：硬件常量、class 封装

**目标**：创建 PIT 驱动的 C++ 头文件，用命名空间常量定义 PIT 的 I/O 端口和命令字节位域，用 class 封装 PIT 的操作（init、irq0_handler、get_ticks、get_uptime_ms）。

**设计思路**：

`PitHW` 命名空间放硬件常量：Channel 0 数据端口 0x40、命令寄存器 0x43、基准时钟频率 1193182。命令字位域：CMD_MODE_3 = 0x06（方波生成器 Mode 3）、CMD_LSB_MSB = 0x30（先低字节后高字节）、CMD_CHANNEL_0 = 0x00（选择通道 0）、CMD_BINARY = 0x00（二进制计数）。

`PIT` class 提供 static 方法：init（接受 freq_hz 参数，默认 100）、irq0_handler（接受 InterruptFrame* 参数）、get_ticks、get_uptime_ms、freq_hz。private 有两个 static 成员变量：tick_count_（uint64_t）和 freq_hz_（uint32_t）。

注意 InterruptFrame 用前向声明而不是 include，因为头文件里只需要知道类型存在即可。

文件路径：`kernel/drivers/pit.hpp`

**验证**：编译通过即可。

---

## 深入理解：PIT 的三种工作模式对比

PIT 有 6 种工作模式，其中与定时器相关的三种最常用。了解它们的区别有助于理解为什么我们选择 Mode 3：

| 模式 | 名称 | 触发条件 | 输出波形 | 自动重载 | 适用场景 |
|------|------|----------|----------|----------|----------|
| Mode 0 | Interrupt on Terminal Count | 计数器到 0 | 单次高电平 | 否 | 一次性超时 |
| Mode 2 | Rate Generator | 计数器到 1 | 窄脉冲 | 是 | 精确周期计时 |
| Mode 3 | Square Wave Generator | 计数器到 0 | 对称方波 | 是 | 周期性中断 |

Mode 0 是一次性的——计数器到 0 后就停止了，不会自动重新开始。如果你想用它做周期性中断，需要在每次 handler 中重新编程 PIT，这既低效又容易引入 jitter。Mode 2 和 Mode 3 都是自动重载的，区别在于输出波形：Mode 2 在计数器到 1 时产生一个很窄的脉冲（一个时钟周期宽度），Mode 3 产生占空比约 50% 的方波。方波的好处是中断间隔非常均匀，不会因为 IRQ 信号的上升沿/下降沿位置不同而产生 timing jitter。对于我们的 100 Hz 定时器来说，Mode 3 是最佳选择。

还有一个不常用的细节：PIT 的计数器可以工作在 BCD（Binary Coded Decimal）模式或二进制模式。我们选择二进制模式（CMD_BINARY = 0x00）。BCD 模式下除数的范围是 0-9999（每位 0-9），而二进制模式下除数范围是 0-65535。对于我们的需求来说，二进制模式提供了更大的除数范围和更简单的计算。

### 第四步——PIT 驱动实现：配置 Channel 0、tick 计数、uptime 打印

**目标**：实现 PIT::init() 配置 Channel 0 为 100 Hz 方波生成器，实现 PIT::irq0_handler() 递增 tick 计数并每秒打印一次 uptime，以及 C-linkage 桥接函数 pit_irq0_handler()。

**设计思路**：

init() 函数做三件事：计算除数、写命令字节、写除数值。除数 = 1193182 / freq_hz，限制在 1 到 65535 之间。命令字节是 CMD_CHANNEL_0 | CMD_LSB_MSB | CMD_MODE_3 | CMD_BINARY = 0x36，写到命令寄存器 0x43。然后先写除数低字节到 0x40，再写高字节到 0x40。重置 tick_count_ 为 0。

irq0_handler() 每次被调用递增 tick_count_，当 tick_count_ 是 freq_hz_（100）的整数倍时说明过了一秒，打印 `[TICK] uptime: Ns`。最后一行调用 PIC::send_eoi(0) 告诉 Master PIC "IRQ0 处理完了"。

文件末尾需要一个 extern "C" 桥接函数 pit_irq0_handler()，把调用转发给 PIT::irq0_handler()。因为汇编 stub 用 C 链接规范 call pit_irq0_handler，而 PIT::irq0_handler 是 C++ 成员函数有 name mangling。

文件路径：`kernel/drivers/pit.cpp`

**踩坑预警**：除数必须先写低字节再写高字节——写反了 PIT 会把第一个字节当成高字节，频率完全不对。比如你想设 11931（0x2E9B），低字节 0x9B 先写、高字节 0x2E 后写；写反了 PIT 看到的除数变成 0x9B2E = 39726，对应频率约 30 Hz。另外 send_eoi(0) 绝对不能忘——忘了的话 PIT 中断只来一次就再也不来了。

关于 `tick_count_` 的类型选择——我们用 `uint64_t` 而不是 `uint32_t`，原因是 `uint32_t` 在 100 Hz 下约 49 天就会溢出（2^32 / 100 / 86400 ≈ 49.7 天），而 `uint64_t` 在同样频率下需要约 5.8 * 10^9 年才溢出，完全不用担心。`get_uptime_ms()` 的计算是 `(tick_count_ * 1000) / freq_hz_`，先乘后除避免了浮点运算，但要注意乘法可能溢出——`tick_count_ * 1000` 在 `tick_count_` 达到约 1.8 * 10^16 时会溢出 uint64_t，这对应约 580 万年的运行时间，同样不需要担心。

**验证**：编译通过即可。

---

## 中断上下文编程注意事项

在实现 PIT 的 `irq0_handler()` 之前，需要了解中断上下文（Interrupt Context）的几个关键约束。这些约束不是 C++ 语言强制的，而是由 x86 硬件中断机制和内核设计的需要共同决定的。违反这些约束通常不会导致编译错误，但会在运行时造成极其难以调试的问题。

**约束一：handler 必须快速执行**。从 IRQ0 到来到 `iretq` 返回之间的所有代码都运行在中断上下文中。由于我们使用 Interrupt Gate（IF 自动清除），在 `send_eoi()` 调用之前，同优先级和更低优先级的硬件中断全部被阻塞。如果 handler 执行时间过长（比如在 `kprintf` 里等待串口输出），后续的中断就会被延迟甚至丢失。我们的 `irq0_handler` 每秒只打印一次，大部分时间只做 `tick_count_++` 和 `send_eoi()`，执行时间在微秒级别，完全满足要求。

**约束二：handler 不能阻塞**。中断上下文中没有"当前进程"的概念——你不能 sleep、不能等待信号量、不能做任何可能导致"让出 CPU"的操作。在我们的教学内核里这一点目前不明显（因为还没有进程概念），但一旦引入了调度器，这个约束就变得至关重要。如果在中断上下文中调用了 `schedule()`，内核会直接 panic 或出现不可预测的行为。

**约束三：handler 不能使用浮点**。内核态的 FPU/SSE 寄存器默认是不保存/恢复的（为了减少上下文切换开销），ISR stub 里也没有做 FXSAVE/XRSTOR。如果在 handler 里使用了 `float` 或 `double`，编译器可能会生成 SSE 指令，导致用户态的 FPU 状态被破坏。

**约束四：共享数据需要保护**。`tick_count_` 是一个 `uint64_t` 类型的全局变量，在 64 位系统上 `++` 操作通常是原子的（单条 `inc` 指令），但如果将来其他代码也要读写 `tick_count_`，就需要考虑竞态条件。对于当前的教学内核，中断处理期间 IF 被清除（Interrupt Gate），不存在嵌套中断的可能性，所以不需要额外的锁保护。

---

## 踩坑案例

### 案例：除数写反导致频率异常

假设你在实现 `PIT::init()` 时不小心把除数的高字节和低字节写反了——先写高字节再写低字节。对于 11931（0x2E9B），PIT 会把第一个写入的 0x2E 当成高字节、第二个写入的 0x9B 当成低字节，拼出的除数是 0x9B2E = 39726。最终频率 = 1193182 / 39726 ≈ 30 Hz，每 33 毫秒一次中断。表现是 tick 打印间隔变成约 3 秒而不是 1 秒。

排查方法：在 `kprintf` 里打印除数值，确认是 11931。如果显示 39726，说明字节顺序写反了。

### 案例：忘了 send_eoi 导致中断卡死

这是本章最可能遇到的 bug。如果 `irq0_handler()` 最后漏掉了 `PIC::send_eoi(0)`，8259A 会认为 IRQ0 仍在处理中（In-Service 位没有清除），后续所有同优先级和更低优先级的中断都不会被转发给 CPU。现象是串口输出 `[PIT] Initialised at 100 Hz` 之后再也没有任何 tick 信息出现——内核看起来像是"冻住"了，但 `hlt` 循环实际上还在运行，只是 CPU 收不到中断。

排查方法：在 `irq0_handler()` 的 `send_eoi()` 调用前后各加一个 `kprintf("EOI_PRE\n")` 和 `kprintf("EOI_POST\n")`。如果只看到 EOI_PRE 没有 EOI_POST，说明 `send_eoi` 调用前就 crash 了。如果两个都没看到，说明 handler 根本没被调用——可能是 IDT 注册有问题或者 PIC 没有正确 unmask IRQ0。

---

## 本篇小结

本篇介绍了 Intel 8254 PIT 的硬件架构、Channel 0 与 IRQ0 的连接关系、命令字节 0x36 的位域含义、以及方波模式 Mode 3 的工作原理。动手部分完成了 PIT 驱动的头文件和实现——`PIT::init()` 配置 Channel 0 为 100 Hz 方波，`PIT::irq0_handler()` 递增 tick 计数并每秒打印 uptime，`pit_irq0_handler()` 提供 C-linkage 桥接。本篇还详细讨论了中断上下文编程的四个关键约束（快速执行、不能阻塞、不能使用浮点、共享数据保护）以及两个最常见的踩坑案例。

下一篇我们将完成 IRQ 汇编 stub 的注册、数据驱动路由表、以及 kernel_main 中的完整集成，最终验证内核的定时器中断是否正常工作。

## 参考资料

**Intel 手册**：
- Intel 64 and IA-32 Architectures SDM, Volume 3A
  - Section 6.14 — 64-Bit Mode Interrupt Handling

**OSDev Wiki**：
- [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer) -- PIT 8254 编程指南
- [Interrupts](https://wiki.osdev.org/Interrupts) -- x86 中断机制总览
