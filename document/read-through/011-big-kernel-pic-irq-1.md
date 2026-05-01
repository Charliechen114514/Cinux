# 011 Big Kernel PIC 重映射 + PIT 定时器 + IRQ 中断 — 通读版

**本章 git tag**：`011_big_kernel_pic_irq`，上一章 tag：`010_big_kernel_gdt_idt`

---

## 本章概览

到了 milestone 010，我们的大内核已经有了 GDT 和 IDT——CPU 异常可以被捕获和报告了。但说实话，这只是一半的故事。一个只有 CPU 异常处理能力而没有硬件中断的内核，就像一部只能拨打 110 报警但永远收不到短信的手机——它能处理"出事了"的场景，却无法响应外界的任何主动联系。PIT 定时器每秒滴答一百次、键盘敲击、磁盘读写完成——这些都是硬件通过中断线主动通知 CPU 的事件，而管理这些中断线的"交通警察"就是 PIC（Programmable Interrupt Controller）。这一章我们要做的事情就是：把 8259A PIC 芯片初始化好，把 IRQ 向量重映射到不会和 CPU 异常冲突的区间，配置 PIT 定时器让它每秒滴答一百次，然后让串口每秒打印一行 `[TICK] uptime: Ns`。当那行 tick 消息第一次出现在终端上的时候，你的内核就真正"活"过来了——它有了时间感。

本章的核心产出是四个模块。PIC 驱动（`pic.hpp` / `pic.cpp`）封装了 8259A 双芯片的初始化序列（ICW1–ICW4）、中断屏蔽/解除屏蔽、以及 EOI（End-Of-Interrupt）信号发送，把 IRQ0-7 重映射到 INT 0x20-0x27、IRQ8-15 重映射到 INT 0x28-0x2F。PIT 驱动（`pit.hpp` / `pit.cpp`）配置 Intel 8254 定时器的 Channel 0 为方波发生器模式，以 100 Hz 的频率产生 IRQ0 中断，并维护一个全局 tick 计数器和 uptime 追踪。IRQ handler 注册模块（`irq_handlers.cpp`）用数据驱动的路由表把 16 个硬件 IRQ 的 ISR stub 注册到 IDT 的向量 0x20-0x2F，同时为未配置的 IRQ 线提供默认 EOI-only handler 以防止未响应中断导致的系统挂死。最后，`interrupts.S` 新增了 16 个 IRQ stub 的宏实例化，和上一章的异常 stub 共用同一套 `ISR_NOERRCODE` 宏。

关键设计决策方面：PIC 初始化采用手动 EOI 模式而非 Auto-EOI 模式，这样做的原因是手动 EOI 给了我们在 handler 内部精确控制中断完成时机的能力，也为将来实现中断优先级和中断嵌套留出了空间；PIT 的 IRQ0 handler 内部自行调用 `PIC::send_eoi(0)` 发送 EOI，而不是在公共框架层统一发送——这看起来像是把 EOI 责任分散到了各个 handler，但实际上让每个 handler 拥有完整的控制权，在将来需要做延迟 EOI（比如中断下半部处理）的时候会更加灵活；IRQ 注册采用了和上一章异常注册相同的数据驱动路由表模式，16 条 IRQ 线通过一张 constexpr 数组统一注册，替代了 16 个重复的 `set_handler()` 调用。和 xv6 对比的话，xv6 的 PIC 初始化更加简洁——它把 ICW 序列直接写在 `picinit()` 函数里，没有 class 封装也没有 namespace 组织，而且 xv6 的 Slave PIC 用了 Auto-EOI 模式（ICW4=0x03），这比我们的手动 EOI 少了一步但牺牲了中断嵌套控制。Linux 早期版本（现在的 `arch/x86/kernel/i8259.c`）的 8259A 驱动则比我们复杂得多——它需要处理各种主板的怪异硬件配置、SMP 下的 IRQ 路由、以及与 IO-APIC 的共存，使用 spinlock 保护所有 PIC 操作、cached IMR 避免频繁读硬件、Specific EOI 替代 Non-specific EOI。我们的设计在工程性和可读性之间取了个务实的平衡——足够清晰到能教会读者 PIC 的每一个细节，又不会过度设计到看不出核心逻辑。

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

如果 IRQ 来自 Slave PIC（IRQ 编号 >= 8），你必须同时向 Slave 和 Master 两个 PIC 都发送 EOI。原因在于级联拓扑——当一个 Slave IRQ 触发时，信号传导路径是 Slave → Master IRQ2 → CPU INTR。所以"正在服务"标记同时存在于 Slave 和 Master 的 ISR（In-Service Register）中。如果只给 Slave 发 EOI 而忘了 Master，Master 会一直认为 IRQ2 还在服务中，后续来自 Slave 的所有中断都会被阻塞。这个 bug 笔者见过不止一个人踩过——症状是"中断收到了几个就再也不来了"，非常诡异。

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

### PIT 驱动：让内核拥有时间感

PIT（Programmable Interval Timer，Intel 8254）是 PC 平台上最经典的可编程定时器，它的 Channel 0 直接连接到 Master PIC 的 IRQ0 线——这意味着只要我们正确配置了 PIT 和 PIC，定时器中断就会周期性地送达 CPU，我们就有了一个"时钟"。

`pit.hpp` 首先定义了 `PitHW` namespace 来存放硬件常量。Channel 0 的数据端口是 `0x40`，命令寄存器是 `0x43`。PIT 的基准时钟频率是 `1193182 Hz`——这个看起来很随意的数字实际上是有历史原因的，它等于 1.193182 MHz，来自 NTSC 电视信号的 subcarrier 频率（3.579545 MHz）除以 3。PC 的设计师当年为了节省芯片成本，直接用电视信号的时钟分频来驱动定时器，于是这个神奇的数字就一直沿用至今。

```cpp
// kernel/drivers/pit.hpp
#pragma once
#include <stdint.h>

namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers {

namespace PitHW {
constexpr uint16_t CHANNEL_0    = 0x40;  ///< Channel 0 data port
constexpr uint16_t CHANNEL_1    = 0x41;  ///< Channel 1 data port (unused)
constexpr uint16_t CHANNEL_2    = 0x42;  ///< Channel 2 data port (speaker)
constexpr uint16_t COMMAND      = 0x43;  ///< Mode/Command register

constexpr uint32_t BASE_FREQ    = 1193182;  ///< Input clock frequency (Hz)

constexpr uint8_t CMD_BINARY     = 0x00;  ///< Binary counter mode
constexpr uint8_t CMD_BCD        = 0x01;  ///< BCD counter mode
constexpr uint8_t CMD_MODE_0     = 0x00;  ///< Interrupt on terminal count
constexpr uint8_t CMD_MODE_2     = 0x04;  ///< Rate generator
constexpr uint8_t CMD_MODE_3     = 0x06;  ///< Square wave generator
constexpr uint8_t CMD_LATCH      = 0x00;  ///< Latch count value
constexpr uint8_t CMD_LSB_ONLY   = 0x10;  ///< LSB only
constexpr uint8_t CMD_MSB_ONLY   = 0x20;  ///< MSB only
constexpr uint8_t CMD_LSB_MSB    = 0x30;  ///< LSB then MSB
constexpr uint8_t CMD_CHANNEL_0  = 0x00;  ///< Select channel 0
}  // namespace PitHW

class PIT {
public:
    static void init(uint32_t freq_hz = 100);
    static void irq0_handler(cinux::arch::InterruptFrame* frame);
    static uint64_t get_ticks();
    static uint64_t get_uptime_ms();
    static uint32_t freq_hz();

private:
    static uint64_t tick_count_;
    static uint32_t freq_hz_;
};

}  // namespace cinux::drivers
```

PIT 的命令字结构是这样的：高 2 位选择通道（00 = Channel 0），中间 2 位选择访问模式（11 = LSB then MSB），再 3 位选择工作模式（011 = Square Wave Generator），最低位选择计数进制（0 = Binary）。组合起来就是 `0x00 | 0x30 | 0x06 | 0x00 = 0x36`，这正是我们在 `init()` 里写到命令寄存器的值。方波模式（Mode 3）的含义是计数器从 divisor 值开始递减，到 0 时输出一个方波的上升沿（触发 IRQ0），然后自动重新加载 divisor 继续计数——如此周而复始，产生稳定的周期性中断。

`init()` 的实现如下：

```cpp
// kernel/drivers/pit.cpp (init)
void PIT::init(uint32_t freq_hz) {
    freq_hz_ = freq_hz;

    uint32_t divisor = PitHW::BASE_FREQ / freq_hz;
    if (divisor > 65535) divisor = 65535;
    if (divisor == 0) divisor = 1;

    io_outb(PitHW::COMMAND,
            PitHW::CMD_CHANNEL_0 |
            PitHW::CMD_LSB_MSB |
            PitHW::CMD_MODE_3 |
            PitHW::CMD_BINARY);

    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>(divisor & 0xFF));
    io_outb(PitHW::CHANNEL_0, static_cast<uint8_t>((divisor >> 8) & 0xFF));

    tick_count_ = 0;
    kprintf("[PIT] Initialised at %u Hz (divisor=%u)\n", freq_hz_, divisor);
}
```

除数的计算是 `1193182 / freq_hz`，结果限制在 1 到 65535 之间（16 位除数的范围）。对于 100 Hz 的请求频率，除数是 11931，完全在范围内。`divisor == 0` 的检查是防御性的——如果有人传了一个比 1193182 还大的频率值，整数除法会得到 0，PIT 除数为 0 的行为是未定义的。写 divisor 的顺序很重要——必须先写低字节到 Channel 0 的数据端口（`0x40`），再写高字节到同一个端口，8254 内部有一个 8 位寄存器来追踪"下一个写入是低字节还是高字节"。

接下来是中断处理函数——这是本章最关键的部分之一：

```cpp
// kernel/drivers/pit.cpp (irq0_handler)
void PIT::irq0_handler(cinux::arch::InterruptFrame* /*frame*/) {
    tick_count_++;

    if ((tick_count_ % freq_hz_) == 0) {
        uint64_t seconds = tick_count_ / freq_hz_;
        kprintf("[TICK] uptime: %us\n", static_cast<unsigned>(seconds));
    }

    PIC::send_eoi(0);
}

uint64_t PIT::get_ticks() { return tick_count_; }

uint64_t PIT::get_uptime_ms() {
    return (tick_count_ * 1000) / freq_hz_;
}

uint32_t PIT::freq_hz() { return freq_hz_; }

}  // namespace cinux::drivers

extern "C" void pit_irq0_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::PIT::irq0_handler(frame);
}
```

irq0_handler 的逻辑非常清晰：每次被调用，tick_count_ 加 1；当 tick_count_ 是 freq_hz_（100）的整数倍时，说明又过了一秒，打印 uptime。最后一行 `PIC::send_eoi(0)` 极其关键——这是告诉 Master PIC "IRQ0 已经处理完了，你可以接收下一个中断了"。如果漏掉这一行，PIT 中断只会来一次就再也不来了，因为 PIC 认为上一个 IRQ0 还没处理完。

注意 `%u` 格式化输出的 cast——`static_cast<unsigned>(seconds)`。我们的 `kprintf` 实现不支持 `%lu` 或 `%llu`，所以需要把 `uint64_t` 截断为 `unsigned`（32 位）。这在 uptime 超过 2^32 秒（约 136 年）的时候会溢出，但说实话一个教学内核跑 136 年有点不现实。

文件最底部的 `extern "C"` 桥接函数是必要的，因为 `PIT::irq0_handler` 是一个 C++ 成员函数，经过 name mangling 之后链接器找不到它。ISR stub 在汇编里 `call pit_irq0_handler`，用的是 C 语言的命名规则，所以我们需要一个 `extern "C"` 的包装函数来做桥接。这个模式和上一章 exception handlers 里用的完全一样。

### IRQ handler 注册：数据驱动的路由表

`irq_handlers.cpp` 把 16 条 IRQ 线的 ISR stub 注册到 IDT 中，和上一章 `idt.cpp` 里的异常注册逻辑如出一辙。

```cpp
// kernel/arch/x86_64/irq_handlers.cpp
#include <stdint.h>
#include "gdt.hpp"
#include "idt.hpp"
#include "kernel/drivers/pit.hpp"
#include "kernel/lib/kprintf.hpp"
#include "pic.hpp"

using cinux::arch::ExceptionVector;
using cinux::arch::GDT_KERNEL_CODE;
using cinux::arch::IDT;
using cinux::arch::IDTGateType;
using cinux::arch::IDTPrivilege;
using cinux::arch::InterruptFrame;
using cinux::arch::PIC;
using cinux::arch::g_idt;
using cinux::arch::make_idt_attr;
using cinux::lib::kprintf;

extern "C" {
void irq0_stub();
void irq1_stub();
void irq2_stub();
void irq3_stub();
void irq4_stub();
void irq5_stub();
void irq6_stub();
void irq7_stub();
void irq8_stub();
void irq9_stub();
void irq10_stub();
void irq11_stub();
void irq12_stub();
void irq13_stub();
void irq14_stub();
void irq15_stub();
}  // extern "C"

struct IRQRoute {
    uint8_t   vector;
    IDT::Stub stub;
};

static constexpr IRQRoute k_irq_routes[] = {
    {0x20, irq0_stub},  {0x21, irq1_stub},  {0x22, irq2_stub},  {0x23, irq3_stub},
    {0x24, irq4_stub},  {0x25, irq5_stub},  {0x26, irq6_stub},  {0x27, irq7_stub},
    {0x28, irq8_stub},  {0x29, irq9_stub},  {0x2A, irq10_stub}, {0x2B, irq11_stub},
    {0x2C, irq12_stub}, {0x2D, irq13_stub}, {0x2E, irq14_stub}, {0x2F, irq15_stub},
};

static constexpr uint8_t kIRQAttr = make_idt_attr(
    IDTPrivilege::Kernel, IDTGateType::Interrupt);

extern "C" void irq_default_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(0);
}

extern "C" void irq_init() {
    kprintf("[IRQ] Registering IRQ handlers (0x20-0x2F)...\n");

    for (const auto& route : k_irq_routes) {
        g_idt.set_handler(
            static_cast<ExceptionVector>(route.vector),
            route.stub, GDT_KERNEL_CODE, kIRQAttr, 0);
    }

    kprintf("[IRQ] All IRQ handlers registered.\n");
}
```

路由表把"向量号 → stub"的映射关系集中在一行，一眼就能看清整个 IRQ 布局。所有 IRQ 条目的属性都是一样的——`IDTPrivilege::Kernel`（DPL=0）加上 `IDTGateType::Interrupt`（Interrupt Gate，进入处理函数时自动关中断）。之所以用 Interrupt Gate 而不是 Trap Gate，是因为硬件中断处理期间我们不想被新的中断打断——如果 PIT 中断正在处理到一半，又来了一个新的 PIT 中断，tick_count_ 的递增可能会出问题。

`irq_default_handler` 做的事情简单到令人发指：直接调用 `PIC::send_eoi(0)` 发送 Master-only EOI。这里有一点不完美——对于来自 Slave PIC 的 IRQ（IRQ8-15），如果我们没有注册专门的 handler，说明我们不期望这些中断到来——但如果由于某种原因它们还是来了，我们至少要给 Master 发 EOI 防止中断系统卡死。严格来说，对于来自 Slave 的 IRQ 应该同时给两个 PIC 都发 EOI，但由于我们屏蔽了所有不需要的 IRQ，`irq_default_handler` 理论上不会被触发——它只是一个安全网。

### interrupts.S：IRQ stub 的宏实例化

`interrupts.S` 在上一章的基础上新增了 16 个 IRQ stub 的实例化。这些 stub 和异常 stub 完全共用 `ISR_NOERRCODE` 宏——因为硬件中断没有 error code，所以所有 IRQ stub 都走"push dummy 0"的路径。

```asm
# kernel/arch/x86_64/interrupts.S (追加内容)

# ============================================================
# Hardware IRQ stubs (PIC remapped vectors 0x20-0x2F)
# ============================================================

/* Master PIC IRQs */
ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
ISR_NOERRCODE irq2_stub,  irq_default_handler   /* IRQ2(0x22): Cascade */
ISR_NOERRCODE irq3_stub,  irq_default_handler   /* IRQ3(0x23): COM2 */
ISR_NOERRCODE irq4_stub,  irq_default_handler   /* IRQ4(0x24): COM1 */
ISR_NOERRCODE irq5_stub,  irq_default_handler   /* IRQ5(0x25): LPT2 */
ISR_NOERRCODE irq6_stub,  irq_default_handler   /* IRQ6(0x26): Floppy */
ISR_NOERRCODE irq7_stub,  irq_default_handler   /* IRQ7(0x27): LPT1 */

/* Slave PIC IRQs */
ISR_NOERRCODE irq8_stub,  irq_default_handler   /* IRQ8(0x28):  RTC */
ISR_NOERRCODE irq9_stub,  irq_default_handler   /* IRQ9(0x29):  Free */
ISR_NOERRCODE irq10_stub, irq_default_handler   /* IRQ10(0x2A): Free */
ISR_NOERRCODE irq11_stub, irq_default_handler   /* IRQ11(0x2B): Free */
ISR_NOERRCODE irq12_stub, irq_default_handler   /* IRQ12(0x2C): PS/2 Mouse */
ISR_NOERRCODE irq13_stub, irq_default_handler   /* IRQ13(0x2D): FPU */
ISR_NOERRCODE irq14_stub, irq_default_handler   /* IRQ14(0x2E): Primary ATA */
ISR_NOERRCODE irq15_stub, irq_default_handler   /* IRQ15(0x2F): Secondary ATA */
```

16 个 stub 的命名很规律——`irq0_stub` 到 `irq15_stub`，各自绑定到一个 C handler。其中 `irq0_stub` 绑定到 `pit_irq0_handler`，其余 15 个全部绑定到 `irq_default_handler`。每个 stub 上方都有注释标明对应的硬件设备和 INT 向量号。一个值得注意的点是 IRQ2（Cascade）——在标准 PC 配置中，Master PIC 的 IRQ2 被用来级联 Slave PIC，它不是一条真正的外部中断线。所以 IRQ2 的 handler 永远不会被外部设备触发，但我们仍然给它注册了一个 default handler 作为占位——空 IDT entry 在中断到来时会触发 Triple Fault。

### kernel_main：点火！

`kernel_main()` 中的初始化顺序经过精心安排，每一步都依赖于前一步的完成。

```cpp
// kernel/main.cpp
#include <stdint.h>
#include "kernel/arch/x86_64/gdt.hpp"
#include "kernel/arch/x86_64/idt.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/drivers/pit.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::drivers::PIT;

extern "C" void irq_init();

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

串口最先初始化——因为后面每一步都需要 kprintf 输出诊断信息。GDT 第二——IDT 中的段选择子要指向 GDT 中的合法描述符。IDT 第三——CPU 异常的 handler 需要 IDT。然后是 PIC 第四——重映射 IRQ 向量。irq_init 第五——在 IDT 中注册 16 个 IRQ handler。PIT 第六——配置定时器硬件开始产生方波。到这里所有硬件和软件基础设施都已就绪，但中断仍然是关闭的——CPU 的 IF 标志还是 0。

接下来是 `PIC::unmask(0)`——解除 IRQ0（PIT 定时器）的屏蔽。这一步很关键但也很容易忘：PIC 初始化后所有 IRQ 都是屏蔽状态，如果你忘了 unmask 就直接 `sti`，PIT 的中断信号会被 PIC 拦截，CPU 永远收不到 IRQ0。unmask 之后，`__asm__ volatile("sti")` 设置 RFLAGS 的 IF 标志，CPU 开始响应可屏蔽中断。从这一刻起，每 10 毫秒就会有一次 IRQ0 到来。

中间有一段有趣的测试代码——`__asm__ volatile("int $3")` 触发一个软件断点。这是上一章留下的回归测试，验证 PIC/IRQ 初始化后异常处理链路仍然正常工作。如果 PIC 重映射搞砸了，或者 IRQ stub 的 push/pop 不平衡，这个 `int $3` 就可能触发 Triple Fault 而不是正常的断点处理。

最后内核进入 `while(1) { hlt; }` 空闲循环。`hlt` 指令让 CPU 进入低功耗停机状态，直到下一个中断到来——这比纯 `while(1)` 死循环友好得多，因为它不会占满 CPU。每次中断到来时 CPU 醒来处理中断、执行 `iretq` 返回到 `hlt` 之后、然后又停机等待下一个中断——如此循环往复，内核就以一种事件驱动的方式运行着。

---

## 设计决策深度分析

### 决策一：手动 EOI vs Auto-EOI

**问题**：8259A 的 ICW4 中有一个 Auto-EOI 选项。如果启用了 Auto-EOI，PIC 会在 CPU ACK 中断后自动清除 In-Service 位，不需要 handler 显式调用 `send_eoi()`。Auto-EOI 看起来更简洁，但我们的设计选择了手动 EOI。

**本项目的做法**：ICW4 只设置了 `ICW4_8086`（0x01），没有设置 `ICW4_AUTO_EOI`（0x02）。每个 IRQ handler 在处理完毕后必须显式调用 `PIC::send_eoi(irq)`。

**备选方案**：在 ICW4 中设置 Auto-EOI 位（`ICW4_8086 | ICW4_AUTO_EOI = 0x03`）。很多小型 OS 教学项目（包括 xv6 的 Slave PIC）就是这么做的，大大减少了代码量和出错概率。

**为什么不选备选方案**：Auto-EOI 有一个根本性的问题——它在 handler 开始执行之前就已经清除了 In-Service 位，这意味着同优先级的中断可以在 handler 执行期间嵌套进来。设想 PIT 的 IRQ0 handler 正在执行 `kprintf`，此时下一个 IRQ0 又到了，Auto-EOI 模式下这个新中断会立刻嵌套执行，导致 `tick_count_++` 和 `kprintf` 在嵌套上下文中运行，产生竞态条件。手动 EOI 模式下 In-Service 位在整个 handler 执行期间保持设置，给了我们对中断时序的完全控制权。此外，手动 EOI 是将来实现中断下半部（bottom-half）处理的基础——我们可以先做关键的上半部处理，发送 EOI 允许新中断进来，然后再执行不那么紧急的下半部工作。

**如果要扩展/改进**：当引入更多 IRQ handler 时，可以考虑封装一个 RAII wrapper，在构造时保存 IRQ 号，析构时自动发送 EOI——这样即使 handler 因为异常提前返回，EOI 也不会漏发。

### 决策二：PIT 100 Hz 频率的选择

**问题**：PIT 的中断频率是一个需要权衡的参数。常见的选择有 18.2 Hz（BIOS 默认）、100 Hz（Linux 传统默认）、250 Hz（部分 Linux 配置）、1000 Hz（某些实时系统）。

**本项目的做法**：默认 100 Hz（10 ms per tick），通过 `PIT::init(100)` 配置。divisor = 1193182 / 100 = 11931。

**备选方案**：用 1000 Hz 获得更高的定时精度，代价是每秒多 10 倍的中断开销。用 18.2 Hz（BIOS 默认）最小化中断开销，但定时精度粗糙到约 55 ms 一步。

**为什么不选备选方案**：100 Hz 是一个经过实战检验的平衡点——Linux 内核从诞生之初就使用 100 Hz 的时钟中断频率（直到 2.6 版本引入了可配置的 HZ 值）。100 Hz 既足够精细到能做简单的调度时间片管理（10 ms 粒度），又不会让中断开销吃掉太多的 CPU 时间。1000 Hz 在 QEMU 模拟环境中不推荐——QEMU 的中断注入本身就有一点延迟，过高的频率可能导致中断堆积。

**如果要扩展/改进**：如果要实现更精确的时间测量，可以用 CPU 的 TSC（Time Stamp Counter）——通过 `rdtsc` 指令读取，精度达到 CPU 时钟周期级别。如果要支持动态 tick（tickless kernel），可以在空闲时把 PIT 配置为一次触发模式（Mode 0），只在需要下一个超时时才重新编程——这是现代 Linux 的 `NO_HZ` 机制的核心思想。

### 决策三：默认 EOI-only handler vs 完全不注册未使用的 IRQ

**问题**：我们有 16 条 IRQ 线，但当前只用了 IRQ0。对于 IRQ1-15，有两种策略。

**本项目的做法**：为所有 16 条 IRQ 线都注册了 handler。IRQ0 有专门的 `pit_irq0_handler`，IRQ1-15 统一使用 `irq_default_handler`（只发送 EOI）。同时 PIC 初始化后所有 IRQ 都被屏蔽，只有 IRQ0 被 unmask。

**备选方案**：只注册 IRQ0 的 handler，其余 15 个 IDT entry（0x21-0x2F）保持空。

**为什么不选备选方案**：防御性编程在内核开发中不是可选项，而是生存法则。一个 EOI-only handler 的代码量几乎可以忽略——就一个 `PIC::send_eoi(0)` 调用——但它提供了一个至关重要的安全网。设想某个驱动的 bug 意外 unmask 了 IRQ1（键盘），如果你没有注册 IRQ1 的 handler，键盘上的每一次按键都会触发 Triple Fault。有了 EOI-only handler，最坏的情况是中断被忽略——系统继续运行。Linux 的 `init_IRQ()` 也会为所有可能的 IRQ 线注册默认 handler。

---

## 常见变体与扩展方向

**1. 键盘驱动（IRQ1）** -- 难度: 低
PS/2 键盘控制器的数据端口是 `0x60`，每次按键产生 IRQ1 中断，handler 通过 `io_inb(0x60)` 读取 scancode，然后解码成 ASCII 字符。真正的挑战在于 scancode 到 key code 的映射表——PC 键盘有 Scan Code Set 1 和 Set 2 两种编码方案，扩展键的 scancode 是多字节的。

**2. 从 8259A PIC 迁移到 IO-APIC** -- 难度: 高
IO-APIC 支持 24 条以上的 IRQ 线、可编程的中断路由（把不同 IRQ 分配给不同 CPU 核心）、以及边沿/电平触发的配置。需要重写整个中断初始化逻辑，处理 MMIO 映射、Redirection Table 配置、EOI 寄存器操作。

**3. 实现可睡眠的定时器（sleep / msleep）** -- 难度: 中
可以维护一个定时器回调链表——每个定时器记录"到期 tick"和回调函数，`irq0_handler` 在每次 tick 时检查链表，到期就执行回调。在这个基础上可以实现 `msleep()`。

**4. 用 HPET 替代 PIT 作为系统时钟源** -- 难度: 中
HPET 提供纳秒级的计时能力，通过 MMIO 访问，需要先通过 ACPI 表定位物理地址。

**5. 中断计数和性能监控** -- 难度: 低
为每条 IRQ 线维护一个中断计数器，在 handler 入口处递增。这在性能分析和调试中非常有用。

---

## 参考资料

**Intel 手册（精确章节号）**：

- Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A
  - Section 6.2 — Exception and Interrupt Vectors（向量号分配表）
  - Section 6.12 — IDT Gate Types（Interrupt Gate vs Trap Gate）
  - Section 6.14 — 64-Bit Mode IDT and Interrupt Handling
- https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

**OSDev Wiki**：

- [8259 PIC](https://wiki.osdev.org/8259_PIC) — PIC 重映射、ICW 序列、EOI、mask/unmask、Spurious IRQ
- [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer) — PIT 8254 编程、三个通道、Mode 3、频率计算
- [Interrupts](https://wiki.osdev.org/Interrupts) — x86 中断机制总览
- [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table) — IDT 结构详解

**其他 OS 实现**：

- xv6 `picirq.c` — 极简 PIC 初始化，约 30 行代码（https://github.com/mit-pdos/xv6-public/blob/master/picirq.c）
- Linux `arch/x86/kernel/i8259.c` — 生产级 8259A 驱动，完整 irq_chip 框架（https://github.com/torvalds/linux/blob/master/arch/x86/kernel/i8259.c）
- SerenityOS `Kernel/Arch/x86_64/Interrupts/APIC.cpp` — APIC 中断系统（https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp）
