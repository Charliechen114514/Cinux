# 011 Big Kernel PIC + PIT + IRQ（三）：kernel_main 集成与设计决策 — 通读版

> 前篇：[011-2 PIT 驱动 + IRQ 注册](011-big-kernel-pic-irq-2.md) | 篇首：[011-1 PIC 驱动](011-big-kernel-pic-irq-1.md)

**本章 git tag**：`011_big_kernel_pic_irq`

---

## 关键代码精讲

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

值得注意的是，`hlt` 指令需要 CPL=0 才能执行（否则触发 #GP），我们的内核运行在 ring 0 所以没有问题。当将来引入用户态（ring 3）之后，用户程序不能直接用 `hlt`，需要通过系统调用让内核来执行 halt 操作——这就是 `pause()` 系统调用的底层实现原理。

另外一个容易忽略的细节是：`PIC::init()` 之后 IRQ0 仍然是屏蔽状态。PIC 初始化过程中我们恢复了之前保存的 IMR 值，而不是直接 `disable_all()`，但这意味着 BIOS 设置的默认屏蔽状态被保留了——而 BIOS 默认是屏蔽所有 IRQ 的。所以在 PIT 配置完成后，必须显式调用 `PIC::unmask(0)` 来解除 IRQ0 的屏蔽。如果跳过了这一步直接 `sti`，你会发现中断已经开启但 tick 永远不来——因为 PIC 在源头就把 IRQ0 信号拦截了。

---

## 设计决策深度分析

### 决策一：手动 EOI vs Auto-EOI

**问题**：8259A 的 ICW4 中有一个 Auto-EOI 选项。如果启用了 Auto-EOI，PIC 会在 CPU ACK 中断（通过 INTA 序列）后自动清除 In-Service 位，不需要 handler 显式调用 `send_eoi()`。Auto-EOI 看起来更简洁——少了每 handler 一个 `send_eoi()` 调用，少了忘记 EOI 导致中断卡死的风险。但我们的设计选择了手动 EOI。

**本项目的做法**：ICW4 只设置了 `ICW4_8086`（0x01），没有设置 `ICW4_AUTO_EOI`（0x02）。每个 IRQ handler 在处理完毕后必须显式调用 `PIC::send_eoi(irq)`。

**备选方案**：在 ICW4 中设置 Auto-EOI 位（`ICW4_8086 | ICW4_AUTO_EOI = 0x03`）。很多小型 OS 教学项目（包括 xv6 的 Slave PIC）就是这么做的，大大减少了代码量和出错概率。

**为什么不选备选方案**：Auto-EOI 有一个根本性的问题——它在 handler 开始执行之前就已经清除了 In-Service 位，这意味着同优先级的中断可以在 handler 执行期间嵌套进来。设想 PIT 的 IRQ0 handler 正在执行 `kprintf`，此时下一个 IRQ0 又到了，Auto-EOI 模式下这个新中断会立刻嵌套执行，导致 `tick_count_++` 和 `kprintf` 在嵌套上下文中运行，产生竞态条件。手动 EOI 模式下 In-Service 位在整个 handler 执行期间保持设置，给了我们对中断时序的完全控制权。此外，手动 EOI 是将来实现中断下半部（bottom-half）处理的基础——我们可以先做关键的上半部处理，发送 EOI 允许新中断进来，然后再执行不那么紧急的下半部工作。

**如果要扩展/改进**：当引入更多 IRQ handler 时，可以考虑封装一个 RAII wrapper，在构造时保存 IRQ 号，析构时自动发送 EOI——这样即使 handler 因为异常提前返回，EOI 也不会漏发。

### 决策二：PIT 100 Hz 频率的选择

**问题**：PIT 的中断频率是一个需要权衡的参数——频率越高，定时精度越好，但中断开销也越大；频率越低，中断开销小了，但定时精度下降。常见的选择有 18.2 Hz（BIOS 默认）、100 Hz（Linux 传统默认）、250 Hz（部分 Linux 配置）、1000 Hz（某些实时系统）。

**本项目的做法**：默认 100 Hz（10 ms per tick），通过 `PIT::init(100)` 配置。divisor = 1193182 / 100 = 11931，这是一个合法的 16 位值。

**备选方案**：用 1000 Hz（1 ms per tick）获得更高的定时精度，代价是每秒多 10 倍的中断开销——每次中断都要保存/恢复 15 个通用寄存器，跳转到 ISR stub，执行 C handler，发送 EOI。用 18.2 Hz（BIOS 默认，divisor = 65535）最小化中断开销，但定时精度粗糙到约 55 ms 一步。

**为什么不选备选方案**：100 Hz 是一个经过实战检验的平衡点——Linux 内核从诞生之初就使用 100 Hz 的时钟中断频率（直到 2.6 版本引入了可配置的 HZ 值）。对于我们的教学内核来说，100 Hz 既足够精细到能做简单的调度时间片管理（10 ms 粒度），又不会让中断开销吃掉太多的 CPU 时间。1000 Hz 在 QEMU 模拟环境中尤其不推荐——QEMU 的中断注入本身就有一点延迟，过高的频率可能导致中断堆积。18.2 Hz 虽然是 BIOS 默认值，但那是 1981 年 IBM PC 的选择——当时的 CPU 是 4.77 MHz 的 8088，100 Hz 对它来说太奢侈了。我们有的是 CPU 时间，不需要这么抠门。

**如果要扩展/改进**：如果要实现更精确的时间测量，可以用 CPU 的 TSC（Time Stamp Counter）——通过 `rdtsc` 指令读取，精度达到 CPU 时钟周期级别。TSC 适合做高精度的时间戳和性能分析，PIT 则继续作为周期性中断源。如果要支持动态 tick（tickless kernel），可以在空闲时把 PIT 配置为一次触发模式（Mode 0），只在需要下一个超时时才重新编程，避免无意义的周期性唤醒——这是现代 Linux 的 `NO_HZ` 机制的核心思想。

### 决策三：默认 EOI-only handler vs 完全不注册未使用的 IRQ

**问题**：我们有 16 条 IRQ 线，但当前只用了 IRQ0。对于 IRQ1-15，有两种策略——要么在 IDT 中注册一个"什么都不做只发 EOI"的默认 handler，要么完全不注册（让 IDT entry 为空）。前者的好处是安全——即使意外收到中断也不会 Triple Fault；后者的好处是更"干净"——你确信不会收到的中断就没有必要注册。

**本项目的做法**：为所有 16 条 IRQ 线都注册了 handler。IRQ0 有专门的 `pit_irq0_handler`，IRQ1-15 统一使用 `irq_default_handler`（只发送 EOI）。同时 PIC 初始化后所有 IRQ 都被屏蔽，只有 IRQ0 被 unmask。

**备选方案**：只注册 IRQ0 的 handler，其余 15 个 IDT entry（0x21-0x2F）保持空。由于 PIC 已经屏蔽了 IRQ1-15，理论上它们不会触发，空 IDT entry 也不会被命中。这样做代码更少，但也更危险——如果某个时刻 PIC 的 mask 寄存器被意外篡改（比如一个有 bug 的 I/O 操作），一个未注册的 IRQ 就会导致 Triple Fault，而且你完全不知道发生了什么。

**为什么不选备选方案**：防御性编程在内核开发中不是可选项，而是生存法则。一个 EOI-only handler 的代码量几乎可以忽略——就一个 `PIC::send_eoi(0)` 调用——但它提供了一个至关重要的安全网。设想一个场景：你的某个驱动在调试时不小心往 PIC 的数据端口写了一个错误的值，意外 unmask 了 IRQ1（键盘）。如果你没有注册 IRQ1 的 handler，键盘上的每一次按键都会触发 Triple Fault。有了 EOI-only handler，最坏的情况是中断被忽略——系统继续运行，你可以在日志中看到异常行为然后排查。此外，注册全部 IRQ 的做法也是很多成熟内核的选择——Linux 的 `init_IRQ()` 就会为所有可能的 IRQ 线注册默认 handler。

**如果要扩展/改进**：可以为默认 handler 增加日志记录——当意外的 IRQ 到来时打印一条警告信息，包含 IRQ 编号和到达的次数。这有助于调试硬件配置问题。另外，当引入 APIC 后，IRQ 路由会变得动态——某些 IRQ 可能被重定向到特定的 CPU 核心，默认 handler 的设计也需要相应调整。

---

## IRQ default handler 的局限性分析

我们的 `irq_default_handler` 有一个已知的局限性：它只向 Master PIC 发送 EOI，无法区分是哪条 IRQ 线触发的。这意味着：

- 对于 Master PIC 的 IRQ（0-7），EOI 是正确的。
- 对于 Slave PIC 的 IRQ（8-15），只发了 Master EOI，Slave 那边的 In-Service 位没有被清除。如果 Slave 上的 IRQ 被 unmask 了（虽然当前不会），后续的 Slave IRQ 就会被卡住。

这个局限性在当前阶段是可以接受的，原因有三：

1. 所有 IRQ（除了 IRQ0）都处于屏蔽状态，default handler 不会被触发。
2. 将来为具体设备（键盘、磁盘）写驱动时，会注册专门的 handler 替换 default handler，每个专门的 handler 会发送正确的 EOI。
3. 如果确实需要处理 Slave 上的未注册 IRQ，可以将 default handler 改为接收 IRQ 号参数（需要为每个 IRQ stub 生成不同的 wrapper），或者在 handler 中读取 PIC 的 ISR（In-Service Register）来判断是哪条 IRQ 在服务。

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
  - Section 6.14 — 64-Bit Mode IDT and Interrupt Handling
- https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

**OSDev Wiki**：

- [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table) — IDT 结构详解

**其他 OS 实现**：

- xv6 `picirq.c` — 极简 PIC 初始化，约 30 行代码（https://github.com/mit-pdos/xv6-public/blob/master/picirq.c）
- Linux `arch/x86/kernel/i8259.c` — 生产级 8259A 驱动，完整 irq_chip 框架（https://github.com/torvalds/linux/blob/master/arch/x86/kernel/i8259.c）
- SerenityOS `Kernel/Arch/x86_64/Interrupts/APIC.cpp` — APIC 中断系统（https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp）
