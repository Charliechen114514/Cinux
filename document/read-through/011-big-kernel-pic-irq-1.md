---
title: 011-big-kernel-pic-irq-1 · PIC 与 IRQ
---

# 011 Big Kernel PIC 重映射 + PIT 定时器 + IRQ 中断（一）— 通读版

**本章 git tag**：`011_big_kernel_pic_irq`，上一章 tag：`010_big_kernel_gdt_idt`

> 次篇：[011-2 PIT 驱动 + IRQ 注册](011-big-kernel-pic-irq-2.md)

---

## 本章概览

到了 milestone 010，我们的大内核已经有了 GDT 和 IDT——CPU 异常可以被捕获和报告了。但说实话，这只是一半的故事。一个只有 CPU 异常处理能力而没有硬件中断的内核，就像一部只能拨打 110 报警但永远收不到短信的手机——它能处理"出事了"的场景，却无法响应外界的任何主动联系。PIT 定时器每秒滴答一百次、键盘敲击、磁盘读写完成——这些都是硬件通过中断线主动通知 CPU 的事件，而管理这些中断线的"交通警察"就是 PIC（Programmable Interrupt Controller）。这一章我们要做的事情就是：把 8259A PIC 芯片初始化好，把 IRQ 向量重映射到不会和 CPU 异常冲突的区间，配置 PIT 定时器让它每秒滴答一百次，然后让串口每秒打印一行 `[TICK] uptime: Ns`。当那行 tick 消息第一次出现在终端上的时候，你的内核就真正"活"过来了——它有了时间感。

本章分为三篇。本篇（第一篇）覆盖 PIC 驱动的完整代码讲解。第二篇覆盖 PIT 驱动和 IRQ handler 注册。第三篇覆盖 kernel_main 集成、设计决策分析和扩展方向。

本章的核心产出是四个模块。PIC 驱动（`pic.hpp` / `pic.cpp`）封装了 8259A 双芯片的初始化序列（ICW1–ICW4）、中断屏蔽/解除屏蔽、以及 EOI（End-Of-Interrupt）信号发送，把 IRQ0-7 重映射到 INT 0x20-0x27、IRQ8-15 重映射到 INT 0x28-0x2F。PIT 驱动（`pit.hpp` / `pit.cpp`）配置 Intel 8254 定时器的 Channel 0 为方波发生器模式，以 100 Hz 的频率产生 IRQ0 中断，并维护一个全局 tick 计数器和 uptime 追踪。IRQ handler 注册模块（`irq_handlers.cpp`）用数据驱动的路由表把 16 个硬件 IRQ 的 ISR stub 注册到 IDT 的向量 0x20-0x2F，同时为未配置的 IRQ 线提供默认 EOI-only handler 以防止未响应中断导致的系统挂死。最后，`interrupts.S` 新增了 16 个 IRQ stub 的宏实例化，和上一章的异常 stub 共用同一套 `ISR_NOERRCODE` 宏。

---

## 架构图

```
PIC / PIT / IRQ 中断处理全链路：

  PIT 硬件（8254 Channel 0）
       │
       │  每 10ms 产生一次方波上升沿
       ▼
  8259A Master PIC
       │  IRQ0 线被拉高
       │  PIC 检查 IMR（Interrupt Mask Register）
       │  若 IRQ0 未被屏蔽 → 向 CPU 发送 INT vector 0x20
       │
       ├── Master PIC: I/O port 0x20/0x21
       │     IRQ0 (PIT) ────────→ INT 0x20
       │     IRQ1 (Keyboard) ───→ INT 0x21
       │     IRQ2 (Cascade) ────→ 连接到 Slave PIC
       │     IRQ3-7 ────────────→ INT 0x23-0x27
       │
       └── Slave PIC: I/O port 0xA0/0xA1
             IRQ8 (RTC) ────────→ INT 0x28
             IRQ9-15 ───────────→ INT 0x29-0x2F
                  │
                  │  Slave 通过 Master 的 IRQ2 级联
                  ▼
       CPU 收到 INT 0x20
            │
            │  CPU 自动行为：
            │    1. 查 IDT[0x20] → 找到 irq0_stub
            │    2. 保存 RFLAGS/RIP/CS/RSP/SS 到内核栈
            │    3. Interrupt Gate → 清 IF（禁止嵌套）
            │    4. 跳转到 irq0_stub
            ▼
  ┌─ interrupts.S: ISR_NOERRCODE irq0_stub, pit_irq0_handler ──┐
  │                                                              │
  │  push $0（dummy error code）                                  │
  │  保存 15 个通用寄存器 → 栈上形成 InterruptFrame               │
  │  movq %rsp, %rdi                                             │
  │  call pit_irq0_handler → C bridge                            │
  │    → PIT::irq0_handler(frame)                                │
  │      tick_count_++                                            │
  │      if (tick_count_ % freq_hz_ == 0)                        │
  │        kprintf("[TICK] uptime: %us\n", ...)                  │
  │      PIC::send_eoi(0)                                        │
  │  恢复 15 个通用寄存器                                         │
  │  addq $8, %rsp → 弹出 dummy error code                      │
  │  iretq → 回到被中断的代码                                     │
  └──────────────────────────────────────────────────────────────┘


  初始化调用链（kernel_main）：

  kernel_main()
      │
      ├── kprintf_init()          ← 串口初始化（kprintf 基础设施）
      │
      ├── g_gdt.init()            ← GDT 必须最先（段描述符）
      │
      ├── g_idt.init()            ← IDT 依赖 GDT 的段选择子
      │
      ├── PIC::init()             ← PIC 重映射（ICW1-ICW4）
      │     │
      │     ├── ICW1: 开始初始化，级联模式，需要 ICW4
      │     ├── ICW2: Master→0x20, Slave→0x28（向量偏移）
      │     ├── ICW3: Master bit 2=1（Slave 在 IRQ2）
      │     │        Slave ID=2（级联身份编号）
      │     ├── ICW4: 8086 模式，手动 EOI
      │     └── 恢复保存的 IMR mask
      │
      ├── irq_init()              ← 注册 IRQ stub 到 IDT 0x20-0x2F
      │
      ├── PIT::init(100)          ← 配置 PIT Channel 0 @ 100 Hz
      │     │
      │     ├── 计算 divisor = 1193182 / 100 = 11931
      │     ├── 写命令字 0x36 到 port 0x43
      │     └── 写 divisor 低/高字节到 port 0x40
      │
      ├── PIC::unmask(0)          ← 解除 IRQ0 的屏蔽
      │
      ├── sti                     ← 开中断！
      │
      └── while(1) { hlt; }      ← 空闲循环，等待中断
```

---

## 关键代码精讲

### PIC 驱动：和 8259A 打交道的艺术

x86 PC 上有两个 8259A PIC 芯片——一个 Master、一个 Slave，通过 IRQ2 级联在一起，总共提供 16 条 IRQ 线。这个设计从 IBM PC/AT 时代一直沿用到现在，虽然现代机器早就用上了 APIC，但 QEMU 模拟的仍然是最经典的 8259A 配置，所以我们得先把它搞定。

8259A 有一个非常让人头疼的默认行为——上电之后，它把 IRQ0-7 映射到 INT 0x08-0x0F、IRQ8-15 映射到 INT 0x70-0x77。你看到问题了吧？INT 0x08-0x0F 正好和 CPU 的异常向量重叠——`#DF`（Double Fault）是向量 8，`#GP`（General Protection）是向量 13。如果不清掉这个默认映射，IRQ0 的定时器中断会伪装成一个 Double Fault，你的异常 handler 会看到一堆莫名其妙的寄存器快照然后直接 fatal halt。这就是为什么 PIC 初始化的第一步永远是重映射——把 IRQ 的向量基址挪到 0x20 以后，避开 Intel 保留的 0x00-0x1F 异常区间。

先看 `pic.hpp` 的设计。整个 PIC 驱动封在 `cinux::arch` namespace 下，用一组 constexpr 常量把 8259A 的 I/O 端口和命令字参数做了语义化命名，然后用一个全是静态方法的 `PIC` class 把初始化、屏蔽、EOI 三个核心操作封装起来。之所以用全静态方法而不是实例，原因很直接——系统里只有一对 PIC 芯片，而且它们的 I/O 端口是硬编码的（Master 0x20/0x21、Slave 0xA0/0xA1），不存在需要多实例或者动态配置端口的场景。

```cpp
// kernel/arch/x86_64/pic.hpp
#pragma once
#include <stdint.h>

namespace cinux::arch {

namespace PicPort {
constexpr uint16_t MASTER_CMD  = 0x20;  ///< Master PIC command / status
constexpr uint16_t MASTER_DATA = 0x21;  ///< Master PIC data (mask / ICW2-4)
constexpr uint16_t SLAVE_CMD   = 0xA0;  ///< Slave PIC command / status
constexpr uint16_t SLAVE_DATA  = 0xA1;  ///< Slave PIC data (mask / ICW2-4)
}  // namespace PicPort

namespace PicICW {
constexpr uint8_t ICW1_ICW4     = 0x01;  ///< ICW4 needed
constexpr uint8_t ICW1_SINGLE   = 0x02;  ///< Single (cascade) mode
constexpr uint8_t ICW1_INTERVAL4 = 0x04; ///< Call address interval 4 (8086)
constexpr uint8_t ICW1_LEVEL     = 0x08; ///< Level triggered (edge) mode
constexpr uint8_t ICW1_INIT      = 0x10; ///< Initialization

constexpr uint8_t ICW4_8086      = 0x01; ///< 8086/88 (MCS-80/85) mode
constexpr uint8_t ICW4_AUTO_EOI  = 0x02; ///< Auto End-Of-Interrupt
constexpr uint8_t ICW4_BUF_MASTER = 0x04;///< Buffered mode master
constexpr uint8_t ICW4_BUF_SLAVE  = 0x00;///< Buffered mode slave
constexpr uint8_t ICW4_SFNM      = 0x10; ///< Special Fully Nested Mode
}  // namespace PicICW

class PIC {
public:
    static void init(uint8_t master_offset = 0x20,
                     uint8_t slave_offset = 0x28);
    static void send_eoi(uint8_t irq);
    static void mask(uint8_t irq);
    static void unmask(uint8_t irq);
    static void disable_all();
    static uint8_t master_offset();
    static uint8_t slave_offset();

private:
    static uint8_t master_offset_;
    static uint8_t slave_offset_;
};

}  // namespace cinux::arch
```

`PicPort` namespace 里定义了四个端口地址。8259A 的命令/数据端口复用机制有点微妙——当你往命令端口（CMD）写数据时，8259A 根据 ICW/OCW 命令字类型来解读你的数据；当你往数据端口（DATA）写数据时，它通常理解为中断屏蔽寄存器（IMR）的操作或者 ICW2-4 的后续初始化字。`PicICW` namespace 则把 ICW（Initialization Command Word）的各个位域做了命名，这样我们在写初始化序列的时候，读代码的人能直接看懂每一位的含义，而不是对着一个裸的 `0x11` 猜这是什么意思。注意我们还定义了实际上没用到的位域常量（比如 `ICW1_SINGLE`、`ICW4_AUTO_EOI`），把它们列出来是为了完整性——读者看头文件的时候就能知道 8259A 还支持哪些功能，不用另外去查 datasheet。

现在来看 `pic.cpp` 的 `PIC::init()` 实现。这个函数做的事情可以用一句话概括：往 Master 和 Slave 的 PIC 发送四组初始化命令字（ICW1-ICW4），期间每次 I/O 写入之后调用 `io_wait()` 延时约 1 微秒以满足 ISA 总线的时序要求。

```cpp
// kernel/arch/x86_64/pic.cpp (init 函数)
void PIC::init(uint8_t master_offset, uint8_t slave_offset) {
    master_offset_ = master_offset;
    slave_offset_  = slave_offset;

    uint8_t master_mask = io_inb(PicPort::MASTER_DATA);
    uint8_t slave_mask  = io_inb(PicPort::SLAVE_DATA);

    // ICW1: start init, cascade mode, ICW4 needed
    io_outb(PicPort::MASTER_CMD, PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();
    io_outb(PicPort::SLAVE_CMD,  PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();

    // ICW2: vector offsets
    io_outb(PicPort::MASTER_DATA, master_offset);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  slave_offset);
    io_wait();

    // ICW3: cascade wiring
    io_outb(PicPort::MASTER_DATA, 0x04);  // Master: slave on IRQ2
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  0x02);  // Slave: cascade identity = 2
    io_wait();

    // ICW4: 8086 mode, no auto-EOI
    io_outb(PicPort::MASTER_DATA, PicICW::ICW4_8086);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  PicICW::ICW4_8086);
    io_wait();

    // Restore saved masks
    io_outb(PicPort::MASTER_DATA, master_mask);
    io_outb(PicPort::SLAVE_DATA,  slave_mask);
}
```

函数开头先把两个 PIC 当前的 IMR mask 读出来保存——这个操作看似多余，但考虑到 BIOS 或者之前的代码可能已经设置了特定的中断屏蔽位，直接覆盖掉不太礼貌。然后进入正式的 ICW 序列。

ICW1 发送到命令端口（CMD），内容是 `ICW1_INIT | ICW1_ICW4`，即 `0x10 | 0x01 = 0x11`。这个字节告诉 8259A 两件事：第一，"请进入初始化模式"（bit 4 = 1）；第二，"我会发送 ICW4"（bit 0 = 1）。注意 bit 1（`ICW1_SINGLE`）没有设——这表示我们工作在级联模式（cascade mode），即有两片 PIC，而不是单片。ICW1 必须同时发给 Master 和 Slave——两片芯片各自独立进入初始化状态。

ICW2 发送到数据端口（DATA），它设置了向量偏移——Master 用 `master_offset`（默认 0x20），Slave 用 `slave_offset`（默认 0x28）。这意味着 Master PIC 的 IRQ0-7 会分别触发 INT 0x20-0x27，Slave PIC 的 IRQ8-15 会触发 INT 0x28-0x2F。这里有一个容易踩的坑：ICW2 的低 3 位必须为零（8259A 会用向量号的高 5 位加上 IRQ 线号来拼出最终的 INT 向量），所以我们传入的偏移必须是 8 的倍数。0x20 和 0x28 都满足这个约束。

ICW3 是级联配置字，Master 和 Slave 的含义不同。Master 的 ICW3 指的是"我的哪根 IRQ 线连着 Slave"——在标准 PC 配置中，Slave 连在 Master 的 IRQ2 上，所以 Master 的 ICW3 写 `0x04`（bit 2 = 1）。Slave 的 ICW3 指的是"我的级联身份编号是多少"——Slave 应答 Master 的 cascade 线时用这个编号来标识自己，标准配置写 `0x02`。这两个值看起来像是同一个数字，但语义完全不同——一个是 bitmask，一个是 ID 编号，8259A 的设计就是这么...有特色。

ICW4 是最后一个初始化字，发到数据端口，内容是 `ICW4_8086 = 0x01`。注意我们没有设置 `ICW4_AUTO_EOI`（bit 1）——这意味着我们选择了手动 EOI 模式，每个中断 handler 在结束前必须显式调用 `PIC::send_eoi()`。手动 EOI 比 Auto-EOI 多了一步，但它给了我们一个非常重要的能力：在 handler 执行期间，同优先级的 IRQ 不会被提前响应，直到我们主动发送 EOI 表示"我处理完了"。

初始化序列的最后一步是恢复之前保存的 IMR mask，这种保守策略避免了在 IDT 还没准备好 handler 的时候就收到意外的硬件中断。

接下来看 `send_eoi()` 的实现。EOI（End-Of-Interrupt）是 x86 中断处理中最关键的概念之一——8259A 在收到一个中断请求后，会把它标记为"正在服务"（In-Service），在这个标记被清除之前，同优先级或更低优先级的中断不会被转发给 CPU。EOI 命令就是清除这个标记的方式。

```cpp
// kernel/arch/x86_64/pic.cpp (send_eoi)
void PIC::send_eoi(uint8_t irq) {
    if (irq >= 8) {
        io_outb(PicPort::SLAVE_CMD, 0x20);
    }
    io_outb(PicPort::MASTER_CMD, 0x20);
}
```

如果 IRQ 来自 Slave PIC（IRQ 编号 >= 8），你必须同时向 Slave 和 Master 两个 PIC 都发送 EOI。原因在于级联拓扑——当一个 Slave IRQ 触发时，信号传导路径是 Slave -> Master IRQ2 -> CPU INTR。所以"正在服务"标记同时存在于 Slave 和 Master 的 ISR（In-Service Register）中。如果只给 Slave 发 EOI 而忘了 Master，Master 会一直认为 IRQ2 还在服务中，后续来自 Slave 的所有中断都会被阻塞。这个 bug 笔者见过不止一个人踩过——症状是"中断收到了几个就再也不来了"，非常诡异。

mask 和 unmask 的实现都是对 IMR（Interrupt Mask Register）的读-改-写操作。IMR 是一个 8 位寄存器，每一位对应一条 IRQ 线——bit 0 对应 IRQ0，bit 7 对应 IRQ7。置 1 表示屏蔽，清 0 表示允许。

```cpp
// kernel/arch/x86_64/pic.cpp (unmask)
void PIC::unmask(uint8_t irq) {
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        port  = PicPort::MASTER_DATA;
        value = io_inb(port) & ~(1u << irq);
    } else {
        port  = PicPort::SLAVE_DATA;
        value = io_inb(port) & ~(1u << (irq - 8));
    }

    io_outb(port, value);
}
```

对于 Master PIC 的 IRQ0-7，直接在对应位操作；对于 Slave PIC 的 IRQ8-15，先把 IRQ 编号减 8 得到位偏移，然后在 Slave 的 IMR 上操作。`disable_all()` 则简单粗暴——直接往两个数据端口写 `0xFF`，一次屏蔽全部 16 条 IRQ 线。

---

## 参考资料

**Intel 手册（精确章节号）**：

- Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A
  - Section 6.2 — Exception and Interrupt Vectors（向量号分配表）
  - Section 6.12 — IDT Gate Types（Interrupt Gate vs Trap Gate）
- https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

**OSDev Wiki**：

- [8259 PIC](https://wiki.osdev.org/8259_PIC) — PIC 重映射、ICW 序列、EOI、mask/unmask、Spurious IRQ
