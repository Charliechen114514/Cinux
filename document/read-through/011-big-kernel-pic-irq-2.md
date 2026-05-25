---
title: 011-big-kernel-pic-irq-2 · PIC 与 IRQ
---

# 011 Big Kernel PIC + PIT + IRQ（二）：PIT 驱动 + IRQ 注册 — 通读版

> 前篇：[011-1 PIC 驱动](011-big-kernel-pic-irq-1.md) | 次篇：[011-3 kernel_main 与设计决策](011-big-kernel-pic-irq-3.md)

**本章 git tag**：`011_big_kernel_pic_irq`

---

## 关键代码精讲

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
/* ... irq2_stub through irq14_stub ... */
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

路由表把"向量号 -> stub"的映射关系集中在一行，一眼就能看清整个 IRQ 布局。所有 IRQ 条目的属性都是一样的——`IDTPrivilege::Kernel`（DPL=0）加上 `IDTGateType::Interrupt`（Interrupt Gate，进入处理函数时自动关中断）。之所以用 Interrupt Gate 而不是 Trap Gate，是因为硬件中断处理期间我们不想被新的中断打断——如果 PIT 中断正在处理到一半，又来了一个新的 PIT 中断，tick_count_ 的递增可能会出问题。

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

---

## 参考资料

**OSDev Wiki**：

- [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer) — PIT 8254 编程、三个通道、Mode 3、频率计算
- [Interrupts](https://wiki.osdev.org/Interrupts) — x86 中断机制总览
