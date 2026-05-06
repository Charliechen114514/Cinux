# 从零在 x64 内核里驱动 PIC 和 PIT（二）：PIT 定时器与 IRQ 注册

> 标签：x86-64, PIT, 8254, IRQ, 定时器, 内核开发, C++, QEMU
> 前篇：[011-1 PIC 驱动](011-big-kernel-pic-irq-1.md) | 次篇：[011-3 集成与验证](011-big-kernel-pic-irq-3.md)

---

## 第三步——PIT 驱动：给内核装上心跳

PIC 配好后，我们需要一个能持续产生中断的硬件来验证整条链路。PIT（Intel 8254）是最理想的选择——它独立于任何外部设备，上电就能用，只需要写几个 I/O 端口就能让它以固定频率发出中断。

PIT 的硬件结构很清晰：它有三个通道（channel 0、1、2），共用一个输入时钟 1193182 Hz（约 1.193 MHz），每个通道有一个 16 位计数器，通过设置除数（divisor）来控制输出频率。最终频率 = 1193182 / divisor。我们只使用 channel 0，因为它直接连接到 IRQ0——channel 1 用于 RAM 刷新（不要碰），channel 2 连接 PC 扬声器（以后做声音的时候再用）。

PIT 驱动封装在 `kernel/drivers/pit.hpp` 和 `pit.cpp` 中，同样采用 class + 静态方法的设计：

```cpp
namespace PitHW {
constexpr uint16_t CHANNEL_0    = 0x40;
constexpr uint16_t COMMAND      = 0x43;
constexpr uint32_t BASE_FREQ    = 1193182;
constexpr uint8_t CMD_CHANNEL_0  = 0x00;
constexpr uint8_t CMD_LSB_MSB    = 0x30;
constexpr uint8_t CMD_MODE_3     = 0x06;
constexpr uint8_t CMD_BINARY     = 0x00;
}

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
```

常量命名和 PIC 驱动保持一致——`PitHW` 命名空间放硬件常量，class 里放操作方法。`BASE_FREQ = 1193182` 是 PIT 的输入时钟频率，这个数字在 PC 兼容系统上从 1981 年到现在一直没变过。根据 [OSDev Wiki PIT 文章](https://wiki.osdev.org/Programmable_Interval_Timer) 的说法，这个频率的来源是一个很有趣的故事——原始 PC 使用 14.31818 MHz 的基振荡器（因为这是电视电路中常用的频率），除以 3 得到 CPU 时钟 4.77 MHz，除以 4 得到 CGA 视频时钟 3.579545 MHz，再把这两个信号 AND 起来就得到了 14.31818 / 12 = 1.193182 MHz 给 PIT 用——当年为了省一个晶振的钱，创造了这么一个神奇的数字。

默认频率 100 Hz 意味着每 10 毫秒一次中断，这是一个很经典的选择——Linux 2.x 时代的默认 HZ 值就是 100。

PIT 的三个通道各自有独立的 16 位计数器，但共用同一个输入时钟。Channel 0 的输出直接连到 Master PIC 的 IRQ0 引脚——这是硬件固定连接，不可更改。Channel 1 的输出原本用于 RAM 刷新电路（在早期 PC 中 DRAM 需要定时刷新），现代系统不再使用但保留了这个端口。Channel 2 的输出连接到 PC 扬声器——可以通过编程产生不同频率的方波来发出蜂鸣声，这在调试时偶尔有用（比如内核 panic 时发出一声长鸣）。三个通道的命令字节格式相同，只是 bit 7-6 选择不同的通道号。

`init()` 的实现如下：

```cpp
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

command byte 是 `0x36`，拆开来看就是：选择 channel 0（bits 7-6 = 00）、LSB-then-MSB 访问模式（bits 5-4 = 11）、square wave generator 模式 3（bits 3-1 = 011）、binary 计数（bit 0 = 0）。Square wave mode 是最常用的定时器模式——计数器从 divisor 倒数到 0，然后自动重载并翻转输出电平，产生对称的方波。这种模式的好处是中断间隔非常均匀，不会出现累积漂移。OSDev Wiki 的 PIT 页面对 Mode 3 有非常详细的描述，包括偶数和奇数 divisor 的行为差异——简单来说，推荐使用偶数 divisor 以获得完美的 50% 占空比方波，不过 11931 是奇数，理论上占空比会有极微小的偏差，但在 100 Hz 这种低频下完全可以忽略。

divisor 的计算很简单——1193182 / 100 = 11931。写入 channel 0 数据端口时必须先写低字节再写高字节，这是 LSB-then-MSB 访问模式规定的写入顺序。如果你搞反了，PIT 会把第一个字节当成高字节，结果就是频率完全不对——比如本来应该 100 Hz 变成了不到 30 Hz，你会以为内核卡死了。

然后是 IRQ0 handler——这是整个 milestone 最关键的函数，因为它运行在中断上下文中，直接和硬件打交道：

```cpp
void PIT::irq0_handler(cinux::arch::InterruptFrame* /*frame*/) {
    tick_count_++;

    if ((tick_count_ % freq_hz_) == 0) {
        uint64_t seconds = tick_count_ / freq_hz_;
        kprintf("[TICK] uptime: %us\n", static_cast<unsigned>(seconds));
    }

    PIC::send_eoi(0);
}
```

逻辑非常清晰：每进入一次 handler 就递增 `tick_count_`，每隔 `freq_hz_` 次中断（即每秒一次）打印一行 uptime，最后向 PIC 发送 EOI。这里有一个非常容易踩的坑——`send_eoi()` 必须在所有操作完成之后调用，但也不能忘了调用。如果忘了发 EOI，PIC 就不会再把后续的 IRQ0 转发给 CPU，定时器会直接停摆，你永远看不到第二行 `[TICK]`。

和 xv6 的 timer 处理做个对比——xv6 的 IRQ0 handler 在 `trap.c` 的 `trap()` 函数中通过 case 分支处理，它做的事情比我们多一些：除了递增 tick 计数器，还会调用 `wakeup(&ticks)` 唤醒所有在 `sleep()` 系统调用上等待的进程。这是 xv6 实现进程调度的基石——每次 tick 到来时检查当前进程的时间片是否用完，如果用完就触发 context switch。我们的教学内核暂时没有进程概念，所以 handler 只做 tick 计数和 uptime 打印，但基础设施已经搭好了——将来加入调度器时，只需要在 `irq0_handler` 里加一个调度检查就行。

Linux 的时钟中断处理就更加复杂了。Linux 不直接用 PIT 做 tick——现代 Linux 使用 tickless 机制（`NO_HZ`），空闲时关闭周期性中断以省电，需要时才通过 hrtimer（高精度定时器）设置下一次中断。时钟源也从 PIT 迁移到了 TSC（Time Stamp Counter）或 HPET。但底层的中断处理流程是一样的——中断到来、handler 执行、EOI 发送、调度检查。

你可能注意到 `irq0_handler` 是 PIT 类的静态成员函数，但 ISR stub 需要一个 C linkage 的函数名——C++ 的 name mangling 会在链接时把函数名改得面目全非。所以我们提供了一个 C-linkage 桥接函数：

```cpp
extern "C" void pit_irq0_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::PIT::irq0_handler(frame);
}
```

这个桥接函数定义在 `pit.cpp` 的末尾，只有一行代码——把调用转发给 PIT 类的静态方法。汇编文件 `interrupts.S` 中的 ISR stub 引用的是 `pit_irq0_handler` 这个 C 符号名，不涉及任何 C++ name mangling。这个模式和上一章的异常处理完全一样。

## 第四步——IRQ 注册机制：数据驱动的路由表

有了 PIC 驱动和 PIT handler，接下来要把它们串起来。在 `kernel/arch/x86_64/irq_handlers.cpp` 中，我们定义了所有 16 个 IRQ 的 ISR stub 声明和注册逻辑。

ISR stub 仍然在 `interrupts.S` 中用 `ISR_NOERRCODE` 宏生成——和 CPU 异常的 stub 完全一样，只是对应的 C handler 不同。IRQ0 的 stub 调用 `pit_irq0_handler`，IRQ1-15 的 stub 调用 `irq_default_handler`（一个简单的 EOI + 返回的空操作）：

```asm
/* Master PIC IRQs */
ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
ISR_NOERRCODE irq2_stub,  irq_default_handler   /* IRQ2(0x22): Cascade */
/* ... IRQ3-15 同理 ... */
ISR_NOERRCODE irq15_stub, irq_default_handler   /* IRQ15(0x2F): Secondary ATA */
```

IRQ 注册沿用了 milestone 010 的数据驱动路由表模式：

```cpp
struct IRQRoute {
    uint8_t    vector;
    IDT::Stub  stub;
};

static constexpr IRQRoute k_irq_routes[] = {
    {0x20, irq0_stub},  {0x21, irq1_stub},  {0x22, irq2_stub},  {0x23, irq3_stub},
    {0x24, irq4_stub},  {0x25, irq5_stub},  {0x26, irq6_stub},  {0x27, irq7_stub},
    {0x28, irq8_stub},  {0x29, irq9_stub},  {0x2A, irq10_stub}, {0x2B, irq11_stub},
    {0x2C, irq12_stub}, {0x2D, irq13_stub}, {0x2E, irq14_stub}, {0x2F, irq15_stub},
};

extern "C" void irq_init() {
    kprintf("[IRQ] Registering IRQ handlers (0x20-0x2F)...\n");
    for (const auto& route : k_irq_routes) {
        g_idt.set_handler(static_cast<ExceptionVector>(route.vector),
                          route.stub, GDT_KERNEL_CODE, kIRQAttr, 0);
    }
    kprintf("[IRQ] All IRQ handlers registered.\n");
}
```

路由表把"向量号 -> stub"的映射关系集中在一行，一眼就能看清整个 IRQ 布局。向量号从 0x20 开始——正好对应 PIC 重映射后的 master 偏移量。所有 IRQ handler 都使用内核中断门（DPL=0, IF cleared），IST 偏移为 0。

默认 handler 的工作就是发送 EOI 然后返回——这样即使某个我们没配驱动的 IRQ 线上产生了噪声信号，PIC 也不会卡住：

```cpp
extern "C" void irq_default_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(0);
}
```

这里有一点不完美——`irq_default_handler` 无法区分是哪条 IRQ 线触发的，所以只能向 master 发 EOI。对于 IRQ0-7 来说没问题，但如果 slave 上的 IRQ（8-15）误触发了默认 handler，EOI 只发给了 master 而没发给 slave，slave 那边会一直处于"等待 EOI"的状态。不过当前阶段所有 16 条 IRQ 都被屏蔽着，只有显式 unmask 的 IRQ 才会产生中断，所以这个问题暂时不会暴露。后续如果要支持动态 IRQ 注册，这里需要改成能感知具体 IRQ 号的实现。

Linux 对这个问题的处理方式是：使用 `struct irq_chip` 的 `irq_mask_ack` 回调，在 mask_and_ack_8259A() 函数里用 specific EOI（0x60 + irq）精确地清除指定 IRQ 的 In-Service 位，并且加上 spurious IRQ 检测——如果 ISR（In-Service Register）显示该 IRQ 并不在服务中，就不发 EOI，因为那是一个伪中断。这种细致程度在教学内核里暂时不需要，但了解这些有助于将来做 APIC 迁移时知道应该往哪个方向扩展。

### IRQ 路由表的设计哲学

数据驱动路由表是 milestone 010 引入的设计模式，本章继续沿用。这种模式的核心思想是把"做什么"和"怎么做"分离——路由表描述"哪些向量号对应哪些 stub"（做什么），`irq_init()` 里的 for 循环执行"遍历路由表并注册到 IDT"（怎么做）。

如果不用路由表，代码就会变成 16 个几乎一模一样的 `set_handler()` 调用，每个只有向量号和 stub 名不同。这种重复代码有三个问题：一是容易在复制粘贴时出错（比如忘了改向量号），二是难以一眼看清所有 IRQ 的布局，三是将来修改注册逻辑时需要同时修改 16 个地方。路由表把所有映射关系集中在一起，一眼就能看到全貌，修改注册逻辑只需要改循环里的一行。

这个模式在 Linux 中也有对应——Linux 的 `irq_desc[]` 数组就是一个全局的 IRQ 描述符表，每个条目记录了该 IRQ 的 handler、chip 操作、状态标志等信息。当然 Linux 的版本比我们的复杂得多——它支持动态注册和注销 handler、handler 链（多个 handler 共享一条 IRQ 线）、以及 per-CPU 的 IRQ 统计。但这些复杂特性的基础仍然是"一张表描述所有 IRQ"的数据驱动思想。

---

## 参考资料

**OSDev Wiki**：
- [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer) — PIT 8254 编程指南
- [Interrupts](https://wiki.osdev.org/Interrupts) — x86 中断机制总览

**其他 OS 实现**：
- xv6 `picirq.c` — 极简 PIC 初始化（https://github.com/mit-pdos/xv6-public/blob/master/picirq.c）
- Linux `arch/x86/kernel/i8259.c` — 生产级 8259A 驱动（https://github.com/torvalds/linux/blob/master/arch/x86/kernel/i8259.c）
