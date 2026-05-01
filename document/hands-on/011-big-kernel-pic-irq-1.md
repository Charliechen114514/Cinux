# 011 PIC 重映射 + PIT 定时器 + IRQ 中断 —— 让内核拥有时间感

## 章节导语

上一章（010）我们给大内核装上了 GDT、IDT 和 CPU 异常处理，用 `int $3` 验证了整条链路能正常工作。但说实话，目前这个内核还只是一个"只会接 CPU 异常电话"的系统，外部世界的任何事件它都听不到——定时器在嘀嗒、键盘在按键、磁盘在读写，CPU 对这些一无所知，因为我们根本没配置硬件中断的入口。

这一章我们要做的是把硬件中断（IRQ）真正接进来。具体来说，我们需要配置 8259A PIC（可编程中断控制器），把 IRQ0-15 重映射到 IDT 的空闲向量区域（0x20-0x2F），然后配置 PIT（可编程间隔定时器）让它每秒产生 100 次中断（100 Hz），最后写一个 IRQ0 处理函数来统计 tick 并每秒在串口打印一行 `[TICK] uptime: Ns`。完成本章后，你会看到 QEMU 串口每秒稳定地输出一行 uptick 信息——这意味着我们的内核第一次有了"时间感"，第一次能对外部硬件事件做出响应。

本章的前置知识是上一章（010_big_kernel_gdt_idt）的 GDT/IDT/ISR 基础设施。你需要理解 IDT 的 set_handler 机制、ISR stub 的工作流程、以及 `InterruptFrame` 的布局。如果你还没读完 010，建议先回去补完。

---

## 概念精讲

### 8259A PIC：x86 外部中断的"前台接待"

在 x86 架构里，CPU 并不是直接和每个外部设备打交道的。外部设备（键盘、定时器、磁盘等）产生的中断信号先到达一个叫做 PIC（Programmable Interrupt Controller，可编程中断控制器）的芯片，PIC 收到信号后做两件事：一是根据优先级决定哪个中断应该先处理（IRQ0 优先级最高，IRQ7 最低），二是把中断请求转换成一个向量号发送给 CPU。CPU 收到这个向量号之后，就去 IDT 里查对应的处理程序——这个过程和我们上一章处理 CPU 异常的流程完全一样，唯一的区别是触发源从"CPU 内部异常"变成了"外部硬件中断"。

PC 兼容机上有两片 8259A PIC 芯片，以级联（cascade）方式连接——Master PIC 负责处理 IRQ0-7，Slave PIC 负责处理 IRQ8-15，Slave 通过 Master 的 IRQ2 线路级联上去。这套设计是 IBM PC/AT 时代留下来的遗产，虽然现代系统早就用 APIC 替代了 8259A，但在 QEMU 的默认配置里 8259A 仍然存在且可用，对于教学目的来说完全够用。

```
硬件中断的传递路径：

外部设备 → IRQ 线路 → PIC → INTR 信号 → CPU → 查 IDT[向量号] → 跳转到 ISR stub → C handler

Master PIC (IRQ0-7) 的级联拓扑：
                ┌──────────────────────────────────────────┐
  PIT Timer ──→ │ IRQ0                                     │
  Keyboard ───→ │ IRQ1                                     │
                │ IRQ2 ←── Slave PIC (IRQ8-15)            │ → CPU INTR
  COM2 ──────→ │ IRQ3                                     │
  COM1 ──────→ │ IRQ4                                     │
  ...          │ IRQ5, IRQ6, IRQ7                         │
                └──────────────────────────────────────────┘

Slave PIC (IRQ8-15):
  RTC ──────→  │ IRQ8-15                                 │ → Master IRQ2
  ...
```

这里有一个非常重要的问题需要解决——BIOS 默认把 Master PIC 的 IRQ0-7 映射到 INT 0x08-0x0F，Slave PIC 的 IRQ8-15 映射到 INT 0x70-0x77。这个映射和 CPU 的异常向量号冲突了——INT 0x08 是 #DF（Double Fault），INT 0x0E 是 #PF（Page Fault），如果 IRQ 不重映射的话，定时器中断一来，CPU 以为发生了 Double Fault，直接跳到 Double Fault 处理函数去了。所以 PIC 初始化的核心任务就是把 IRQ 重映射到 IDT 里空闲的区域——习惯上用 0x20-0x2F（也就是 IDT 的 32-47 号条目），因为 Intel 保留了 0-31 给 CPU 异常，32 开始就是自由使用的。

```
PIC 重映射前后的向量号对照：

重映射前（BIOS 默认，会冲突！）：
  IRQ0 → INT 0x08  ← 和 #DF 冲突！
  IRQ1 → INT 0x09  ← 和 #DB 冲突！
  ...
  IRQ7 → INT 0x0F
  IRQ8 → INT 0x70
  ...

重映射后（本章的配置，安全）：
  IRQ0 → INT 0x20  (vector 32)
  IRQ1 → INT 0x21  (vector 33)
  ...
  IRQ7 → INT 0x27  (vector 39)
  IRQ8 → INT 0x28  (vector 40)
  ...
  IRQ15 → INT 0x2F (vector 47)
```

### ICW1-ICW4：PIC 初始化的四次"握手"

8259A 的初始化过程是一个固定的四步序列，叫做 ICW（Initialization Command Word）。每一步往 PIC 的 I/O 端口写一个字节，PIC 内部状态机按顺序接收。这四步分别是：

- **ICW1**：告诉 PIC "我要开始初始化了"，同时指明是否级联模式、是否需要 ICW4。我们写 `0x11`（INIT=1, ICW4 needed=1, cascade mode）。
- **ICW2**：设置向量号基址。Master 写 `0x20`，Slave 写 `0x28`。这一步决定了 IRQ0 会映射到 INT 0x20、IRQ1 到 INT 0x21，以此类推。
- **ICW3**：告诉两片 PIC 级联拓扑。Master 写 `0x04`（表示 IRQ2 上接了 Slave），Slave 写 `0x02`（表示自己连在 Master 的 IRQ2 线路上）。
- **ICW4**：设置工作模式。我们写 `0x01`（8086 模式，不用 auto-EOI，不用 buffered mode）。

一个很关键的细节是——8259A 的数据手册要求两次连续的 I/O 写入之间有一定的延迟（因为这是 ISA 总线时代的芯片，时序很慢）。我们在每次 `io_outb` 之后调用一次 `io_wait()`（往端口 0x80 写一个字节，大约 1 微秒的延迟），来满足这个时序要求。如果你的 PIC 初始化之后行为异常（比如该来的中断不来、EOI 发了没反应），第一个要检查的就是有没有漏掉 `io_wait()`。

### EOI：中断结束的"签收单"

8259A 有一个非常重要的机制——当它把一个中断发给 CPU 之后，它会"记住"这个中断正在被处理，在收到 EOI（End Of Interrupt）信号之前，同优先级和更低优先级的中断都不会被转发给 CPU。这意味着中断处理函数在结束之前必须显式地给 PIC 发一个 EOI，告诉它"这个中断我已经处理完了，你可以接收下一个了"。

EOI 的发送方式是往 PIC 的 Command 端口写 `0x20`。对于来自 Slave PIC（IRQ8-15）的中断，需要先给 Slave 发 EOI，再给 Master 发 EOI——因为 Master 是通过 IRQ2 收到 Slave 的级联信号的，如果只给 Slave 发了 EOI 而不给 Master 发，Master 就一直认为 IRQ2 上的中断还没处理完，后续所有中断都会被阻塞。

这个 EOI 机制是一个极其常见的坑——如果你忘了发 EOI，或者 EOI 发错了顺序，症状就是"中断只来一次就再也不来了"。这种情况很容易被误认为是 PIC 配置错误或者 IDT 注册错误，但实际原因只是忘了签收。

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

### 第一步——PIC 驱动头文件：端口常量、ICW 常量、class 封装

**目标**：创建 PIC 驱动的 C++ 头文件，用命名空间常量定义 PIC 的 I/O 端口和 ICW 位域，用 class 封装 PIC 的全部操作（init、send_eoi、mask、unmask、disable_all）。

**设计思路**：我们这一章的 PIC 设计全部用 static 方法——因为系统里只有一对 8259A PIC 芯片，不需要实例化。这和上一章 GDT 的 `g_gdt` 全局实例不太一样，PIC 没有需要存储在实例里的状态（除了两个 offset 值，它们用 static 成员变量就够了）。

**实现约束**：

头文件需要包含以下组成部分，全部放在 `cinux::arch` 命名空间下：

1. 一个名为 `PicPort` 的命名空间，定义四个 `constexpr uint16_t` 常量：Master PIC 的 Command 端口 0x20、Data 端口 0x21，Slave PIC 的 Command 端口 0xA0、Data 端口 0xA1。这些是 8259A 的标准 I/O 端口地址，从 IBM PC 时代就没变过。

2. 一个名为 `PicICW` 的命名空间，定义 ICW 相关的位域常量。其中 `ICW1_INIT = 0x10`（bit 4 = 进入初始化模式）和 `ICW1_ICW4 = 0x01`（bit 0 = 需要 ICW4）分开定义，使用时用 `|` 组合成 `0x11`。`ICW4_8086 = 0x01` 选择 8086 模式。这些分开定义是为了让代码的每一位含义一目了然，而不是面对魔法数字去猜。

3. 一个 `PIC` class，提供六个 static 方法：`init`（接受两个 uint8_t 参数设置 master/slave offset，默认 0x20/0x28）、`send_eoi`（接受 uint8_t irq 参数）、`mask`、`unmask`、`disable_all`、`master_offset`、`slave_offset`。private 部分有两个 static uint8_t 成员变量存储 offset。

文件路径：`kernel/arch/x86_64/pic.hpp`

**踩坑预警**：ICW1 的各个 bit 是独立的控制位，必须分开定义然后 OR 组合，不要直接写裸的 `0x11`。另外 `init` 的参数必须满足"是 8 的倍数"这个约束——0x20 和 0x28 都满足，但如果你传了 0x21 这样的值，8259A 会在低 3 位上做 IRQ 编号替换，向量号就不对了。

**验证**：此步完成后编译应该通过，但还没有可运行的输出。可以用 `cmake --build build -j$(nproc)` 验证编译无误。

### 第二步——PIC 驱动实现：ICW1-4 初始化序列、EOI、mask/unmask

**目标**：实现 PIC 的所有方法。init() 发送完整的 ICW1-ICW4 序列并附带 io_wait() 延迟；send_eoi() 处理 Master/Slave 双发逻辑；mask()/unmask() 通过 read-modify-write 操作 IMR 寄存器。

**设计思路**：

init() 函数的开头先把两个 PIC 当前的 IMR（Interrupt Mask Register）值读出来保存在局部变量里。为什么要保存？因为 ICW 序列写入的过程中会暂时覆盖 IMR 的值，如果我们不保存和恢复，init() 执行完毕后所有 IRQ 的屏蔽状态会被搞乱。然后依次发送 ICW1（command 端口，0x11）、ICW2（data 端口，master offset/slave offset）、ICW3（data 端口，Master 写 0x04 Slave 写 0x02）、ICW4（data 端口，0x01）。每次写入后跟一个 io_wait()。最后恢复保存的 IMR 值。

send_eoi() 的逻辑很直接：参数是 IRQ 号（0-15），不是 INT 向量号。如果 irq >= 8 说明来自 Slave PIC，先给 Slave 发 EOI 再给 Master 发；否则只给 Master 发。EOI 命令就是往 Command 端口写 0x20。

unmask() 和 mask() 的实现是对 IMR 的读-改-写操作。IMR 是 8 位寄存器，每位对应一条 IRQ 线，bit=1 屏蔽，bit=0 允许。对于 Master PIC 的 IRQ0-7 直接操作对应位，对于 Slave PIC 的 IRQ8-15 先把 IRQ 编号减 8 得到位偏移再操作。unmask 用 `& ~(1u << n)` 清零对应位，mask 用 `| (1u << n)` 置位对应位。

disable_all() 最简单——直接往两个 data 端口写 0xFF，屏蔽全部 16 条 IRQ 线。

文件路径：`kernel/arch/x86_64/pic.cpp`

**踩坑预警**：ICW3 的两个值含义完全不同但都和数字 2 有关。Master 写 0x04 是一个位图（bit 2 置 1 表示 IRQ2 上有 Slave），Slave 写 0x02 是一个编号（级联身份 ID = 2）。写反了级联就断了。另外 io_wait() 不能省略，虽然 QEMU 不严格要求但保持一致性是好习惯。

**验证**：编译通过即可。可以用 `cmake --build build -j$(nproc)` 验证。

### 第三步——PIT 驱动头文件：硬件常量、class 封装

**目标**：创建 PIT 驱动的 C++ 头文件，用命名空间常量定义 PIT 的 I/O 端口和命令字节位域，用 class 封装 PIT 的操作（init、irq0_handler、get_ticks、get_uptime_ms）。

**设计思路**：

`PitHW` 命名空间放硬件常量：Channel 0 数据端口 0x40、命令寄存器 0x43、基准时钟频率 1193182。命令字位域：CMD_MODE_3 = 0x06（方波生成器 Mode 3）、CMD_LSB_MSB = 0x30（先低字节后高字节）、CMD_CHANNEL_0 = 0x00（选择通道 0）、CMD_BINARY = 0x00（二进制计数）。

`PIT` class 提供 static 方法：init（接受 freq_hz 参数，默认 100）、irq0_handler（接受 InterruptFrame* 参数）、get_ticks、get_uptime_ms、freq_hz。private 有两个 static 成员变量：tick_count_（uint64_t）和 freq_hz_（uint32_t）。

注意 InterruptFrame 用前向声明而不是 include，因为头文件里只需要知道类型存在即可。

文件路径：`kernel/drivers/pit.hpp`

**验证**：编译通过即可。

### 第四步——PIT 驱动实现：配置 Channel 0、tick 计数、uptime 打印

**目标**：实现 PIT::init() 配置 Channel 0 为 100 Hz 方波生成器，实现 PIT::irq0_handler() 递增 tick 计数并每秒打印一次 uptime，以及 C-linkage 桥接函数 pit_irq0_handler()。

**设计思路**：

init() 函数做三件事：计算除数、写命令字节、写除数值。除数 = 1193182 / freq_hz，限制在 1 到 65535 之间。命令字节是 CMD_CHANNEL_0 | CMD_LSB_MSB | CMD_MODE_3 | CMD_BINARY = 0x36，写到命令寄存器 0x43。然后先写除数低字节到 0x40，再写高字节到 0x40。重置 tick_count_ 为 0。

irq0_handler() 每次被调用递增 tick_count_，当 tick_count_ 是 freq_hz_（100）的整数倍时说明过了一秒，打印 `[TICK] uptime: Ns`。最后一行调用 PIC::send_eoi(0) 告诉 Master PIC "IRQ0 处理完了"。

文件末尾需要一个 extern "C" 桥接函数 pit_irq0_handler()，把调用转发给 PIT::irq0_handler()。因为汇编 stub 用 C 链接规范 call pit_irq0_handler，而 PIT::irq0_handler 是 C++ 成员函数有 name mangling。

文件路径：`kernel/drivers/pit.cpp`

**踩坑预警**：除数必须先写低字节再写高字节——写反了 PIT 会把第一个字节当成高字节，频率完全不对。比如你想设 11931（0x2E9B），低字节 0x9B 先写、高字节 0x2E 后写；写反了 PIT 看到的除数变成 0x9B2E = 39726，对应频率约 30 Hz。另外 send_eoi(0) 绝对不能忘——忘了的话 PIT 中断只来一次就再也不来了。

**验证**：编译通过即可。

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

排查方法：在 IRQ0 handler 的开头和末尾各加一个 `kprintf`，看是进入了处理函数就不出来了，还是处理函数正常返回了但下一个 tick 不来了。如果只在开头看到了一次打印、末尾没看到，说明 `send_eoi` 之前就 crash 了。如果两头都看到了但还是没有第二个 tick，检查 `send_eoi` 的参数是不是正确的 IRQ 号。

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
