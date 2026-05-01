# 从零在 x64 内核里驱动 PIC 和 PIT：让内核第一次"听到"硬件的心跳

> 标签：x86-64, PIC, 8259A, PIT, 8254, IRQ, 中断控制器, 定时器, 内核开发, C++, QEMU
> 前置：[010 GDT + IDT 教程](011-big-kernel-pic-irq-1.md)

---

## 前言

如果你跟我一样，是从 milestone 010 一路折腾过来的，那你应该记得上一章结尾我留了一个非常显眼的悬念——我们始终没有调用 `sti` 开中断。原因很简单：当时只配了 CPU 异常向量（0-14），IRQ（32-47）的 handler 全是空的，IDT 里未配置的向量 Present 位为 0，QEMU 的 PIT 定时器中断挂在 IRQ0 上，一开中断就会连环 triple fault。内核就像一台已经装好了发动机但没接油管的车——所有机械结构都在，但就是动不起来。

milestone 011 的目标就是把这条"油管"接上。具体来说，我们需要三样东西：PIC（8259A 可编程中断控制器）驱动，用来把硬件中断信号转译成 CPU 能理解的 INT 向量号；PIT（Intel 8254 可编程间隔定时器）驱动，用来以固定频率产生时钟中断；以及一套完整的 IRQ handler 注册机制，把 PIC 重映射后的向量号和对应的 ISR stub 绑定到 IDT 里。全部配好之后，内核串口每秒输出一行 `[TICK] uptime: Ns`——看似平淡无奇，但它的背后是 CPU 第一次真正意义上响应了外部硬件事件，内核从此不再是一个只会原地打转的死循环。

## 环境说明

实验环境和 milestone 010 保持一致。x86_64 平台，GNU AS + GCC/G++ + CMake 构建，QEMU 模拟运行。大内核仍然是 freestanding C++23，编译标志用 `-mcmodel=large`，其余约束照旧——无标准库、无异常、无 RTTI、无红区。大内核运行在物理地址 `0x1000000`（16MB），当前仍使用 identity mapping。

一个需要特别交代的硬件背景是：8259A PIC 和 8254 PIT 都是非常老的硬件——PIC 是 1980 年代随 IBM PC/AT 引入的，PIT 更早，可以追溯到 1981 年的 IBM PC。在现代 x86 系统中，它们已经被 APIC（Advanced PIC）和 HPET（High Precision Event Timer）取代，但 QEMU 默认仍然模拟这两个老硬件。更重要的是，几乎所有 x86 操作系统教程都从 8259A + 8254 开始讲中断和定时器，因为它们的编程模型非常简单——只有几个 I/O 端口，初始化流程完全确定，不需要处理 MMIO、MSR、ACPI 表这些复杂的东西。我们先搞定这俩，后面迁移到 APIC 和 HPET 的时候就有了对照基础。

## 第一步——理解 PIC 在中断链路中的角色

在动手写 PIC 驱动之前，我们先搞清楚它在整个中断链条里扮演什么角色。x86 PC 的传统中断架构是两级级联的 8259A——一片 master PIC 和一片 slave PIC，slave 通过 master 的 IRQ2 级联入口连接，两片合计提供 16 条 IRQ 线（IRQ0-15）。当外部设备产生中断信号时，PIC 负责把 IRQ 号转译成一个 INT 向量号，然后通过 CPU 的 INTR 引脚通知 CPU。CPU 收到信号后查 IDT 找到对应的 handler 跳过去执行，处理完毕后软件必须向 PIC 发送 EOI（End-Of-Interrupt）信号，PIC 才会把后续中断转发给 CPU。

这里有一个非常关键的细节：BIOS 在开机时把 master PIC 的向量基址设为 0x08，slave 设为 0x70。这意味着 IRQ0（定时器）默认会被映射到 INT 0x08，但 INT 0x08 是 CPU 的 Double Fault 异常（#DF）。如果不去重映射，定时器中断一触发就会走 #DF 的 handler 路径，然后大概率 triple fault。所以我们必须做的第一件事就是把 PIC 的向量基址挪到 0x20 和 0x28——Intel 保留向量 0-31 给 CPU 异常，从 32 开始才是用户可用的。这就是所谓的"PIC 重映射"，几乎是每一个 x86 OS 教程的开篇必修课。

如果你看过 OSDev Wiki 上关于 [8259 PIC](https://wiki.osdev.org/8259_PIC) 的文章，你会发现它把这个问题描述得一清二楚——IBM 在设计 PC/AT 时做了一个设计失误，把 IRQ 默认映射到了和 CPU 异常重叠的向量区间。这个"历史遗留问题"导致每一代 OS 开发者都要在启动初期显式重映射 PIC，成了 x86 OS 开发的一个"入门仪式"。

## 第二步——PIC 驱动的 C++ 封装

PIC 驱动封装在 `kernel/arch/x86_64/pic.hpp` 和 `pic.cpp` 中。按照项目的一致风格，我们用 class 来组织所有 PIC 操作——虽然系统里只有一对 PIC 芯片，用 namespace 加自由函数也能实现，但 class 封装能让我们把 master/slave 的端口常量、ICW 标志位、offset 存储这些状态绑在一起，后面迁移到 APIC 的时候只需要换一个实现，调用方完全不用改。

先看端口常量和 ICW 标志位的定义：

```cpp
namespace PicPort {
constexpr uint16_t MASTER_CMD  = 0x20;
constexpr uint16_t MASTER_DATA = 0x21;
constexpr uint16_t SLAVE_CMD   = 0xA0;
constexpr uint16_t SLAVE_DATA  = 0xA1;
}

namespace PicICW {
constexpr uint8_t ICW1_ICW4     = 0x01;   // ICW4 needed
constexpr uint8_t ICW1_INIT      = 0x10;   // Initialization
constexpr uint8_t ICW4_8086      = 0x01;   // 8086 mode
// ... 还有 ICW1_SINGLE, ICW4_AUTO_EOI 等（头文件里列全了，这里不展开）
}
```

PIC 的 I/O 端口布局很简洁——master 占用 0x20（command）和 0x21（data），slave 占用 0xA0（command）和 0xA1（data）。command 端口用来发送 ICW 初始化序列和 EOI 信号，data 端口用来读写中断屏蔽寄存器（IMR）和 ICW2-ICW4。这些端口号是硬件固定的，任何 x86 系统都一样——从 1984 年的 IBM PC/AT 到今天的 QEMU 虚拟机，三十多年了一直没变。

PIC 类提供五个静态方法：`init()` 负责发送 ICW1-ICW4 完成重映射，`send_eoi()` 在中断处理完毕后通知 PIC，`mask()` 和 `unmask()` 控制单条 IRQ 线的启用和禁用，`disable_all()` 一次性屏蔽全部 IRQ。所有方法都是静态的，因为系统里只有一对 PIC，不需要实例化多个对象——这点和 Linux 的 `i8259A_chip` 设计思路不同，Linux 用了 `struct irq_chip` 函数指针表来实现运行时多态，可以在 8259A 和 APIC 之间切换，但我们的教学内核目前不需要这么复杂。

接下来看 `init()` 的实现——这是 PIC 驱动的核心，也是最考验耐心的部分，因为 8259A 的初始化协议要求严格按 ICW1 到 ICW4 的顺序发送，每一步之间必须有足够的时间间隔：

```cpp
void PIC::init(uint8_t master_offset, uint8_t slave_offset) {
    master_offset_ = master_offset;
    slave_offset_  = slave_offset;

    // Save current masks so we can restore after init
    uint8_t master_mask = io_inb(PicPort::MASTER_DATA);
    uint8_t slave_mask  = io_inb(PicPort::SLAVE_DATA);

    // ICW1: start initialisation in cascade mode, ICW4 needed
    io_outb(PicPort::MASTER_CMD,
            PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();
    io_outb(PicPort::SLAVE_CMD,
            PicICW::ICW1_INIT | PicICW::ICW1_ICW4);
    io_wait();

    // ICW2: vector offsets
    io_outb(PicPort::MASTER_DATA, master_offset);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  slave_offset);
    io_wait();

    // ICW3: cascade wiring -- master has slave on IRQ2 (bit 2),
    //        slave reports its cascade identity as 2
    io_outb(PicPort::MASTER_DATA, 0x04);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  0x02);
    io_wait();

    // ICW4: 8086 mode, no auto-EOI, no buffered mode, no SFNM
    io_outb(PicPort::MASTER_DATA, PicICW::ICW4_8086);
    io_wait();
    io_outb(PicPort::SLAVE_DATA,  PicICW::ICW4_8086);
    io_wait();

    // Restore saved masks
    io_outb(PicPort::MASTER_DATA, master_mask);
    io_outb(PicPort::SLAVE_DATA,  slave_mask);
}
```

这段代码看起来重复很多——每个 `io_outb` 后面都跟着一个 `io_wait()`——但这正是 8259A datasheet 的要求。你可以把 ICW 序列理解为一次"握手协议"：ICW1 告诉 PIC "我要初始化你了，准备好了吗"，ICW2 告诉它"向量基址是这个"，ICW3 告诉它"级联接线是这样的"，ICW4 告诉它"工作模式用 8086"。每一步之间必须留出足够的 I/O 延迟，我们用 `io_wait()`（写一次 port 0x80，大约 1 微秒的延迟）来满足这个时序要求。OSDev Wiki 的 PIC 文章也特别提到了这个 io_wait 的重要性——在老式 ISA 总线硬件上不加延时就可能初始化失败。

具体来看四个 ICW 分别做了什么。ICW1 发送 `0x11`（INIT | ICW4_NEEDED），告诉 PIC "开始初始化，后面还有 ICW4 要发"。注意这个值必须写到 command 端口（0x20/0xA0），因为 ICW1 是通过 command 端口发送的——8259A 用一个简单的状态机来区分不同类型的命令字，ICW1 的特征是 bit 4 = 1。ICW2 写入向量基址——master 写 0x20，意味着 IRQ0-7 会被映射到 INT 0x20-0x27；slave 写 0x28，意味着 IRQ8-15 映射到 INT 0x28-0x2F。

ICW3 描述级联拓扑——master 的 ICW3 是 0x04（bit 2 置位，表示 IRQ2 上连接了 slave），slave 的 ICW3 是 0x02（表示自己的级联身份是 2）。这里有一个细节值得展开：这两个值看起来像是同一个数字，但语义完全不同——master 端的是一个 bitmask（哪根 IRQ 线上连着 slave），slave 端的是一个 ID 编号（slave 在级联链里的身份）。8259A 的设计就是这么有特色，这也是很多人写 PIC 驱动时最容易搞混的地方。ICW4 选择 8086 模式，不使用 auto-EOI——后面设计决策部分会详细讨论为什么我们选手动 EOI。

你会发现 `init()` 开头先保存了 master 和 slave 的当前 IMR 值，初始化完成后再恢复。这是因为 ICW2-ICW4 是通过 data 端口发送的，而 data 端口同时承担 IMR 读写功能——初始化过程中对 data 端口的写入会暂时覆盖 IMR 的值。如果我们不保存和恢复，`init()` 执行完毕后所有 IRQ 的屏蔽状态会被初始化过程中写入的 ICW 值搞乱。这个细节很多教程会忽略，因为它们通常在 `init()` 最后直接 `disable_all()` 把所有 IRQ 屏蔽掉，反正也不在乎之前的屏蔽状态。但我们选择保存恢复，这样调用方可以在 `init()` 之前预设好某些 IRQ 的屏蔽状态，`init()` 不会破坏它。

接下来看 EOI（End-Of-Interrupt）的发送：

```cpp
void PIC::send_eoi(uint8_t irq) {
    if (irq >= 8) {
        io_outb(PicPort::SLAVE_CMD, 0x20);   // Slave PIC EOI
    }
    io_outb(PicPort::MASTER_CMD, 0x20);      // Always master EOI
}
```

EOI 的逻辑非常直观——如果中断来自 slave PIC（IRQ8-15），必须先向 slave 发 EOI 再向 master 发；如果来自 master（IRQ0-7），只需要向 master 发。向 command 端口写 0x20 就是标准的 non-specific EOI 命令（OCW2 格式，bit 5 = 1），告诉 PIC "当前优先级最高的那个中断处理完了"。这里千万别搞反顺序——如果先向 master 发 EOI 再向 slave 发，master 会认为 slave 上的中断已经结束，可能开始转发新的中断，而 slave 那边还没收到 EOI，会造成中断丢失或者重入。

说到 EOI，不得不提 xv6 和 Linux 的做法差异。xv6 的 `picirq.c` 给 Slave PIC 用了 Auto-EOI 模式（ICW4 写 0x03 而不是 0x01），这意味着 Slave PIC 上的中断在 CPU ACK 之后会自动发送 EOI，handler 里完全不需要手动发送——这简化了代码但牺牲了对中断嵌套的控制。Linux 的 `i8259.c` 则走得更远：它不用 non-specific EOI（0x20），而是用 specific EOI（0x60 + irq），精确清除指定 IRQ 的 In-Service 位，这样即使在同一时刻有多个不同优先级的中断在服务，EOI 也不会误操作。Linux 还用 spinlock 保护所有 PIC 操作以保证 SMP 安全性，并且实现了完整的 spurious IRQ 检测——这些生产级特性我们的教学内核暂时不需要，但知道它们的存在有助于理解为什么 Linux 的 8259A 驱动有好几百行代码。

mask 和 unmask 的实现是标准的读-改-写模式：

```cpp
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

读出当前 IMR 值，清除对应位（unmask）或设置对应位（mask），写回去。IRQ0-7 对应 master 的 IMR，IRQ8-15 对应 slave 的 IMR，所以对 slave 的 IRQ 要减去 8 才是正确的 bit 位置。这里不要忘了读-改-写——如果你直接写一个值而不是先读再改，就会把其他 IRQ 的屏蔽状态全部覆盖。而且 mask/unmask 操作不需要 `io_wait()`——只有初始化序列才需要延迟，普通的 IMR 读写是即时的。

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

路由表把"向量号 → stub"的映射关系集中在一行，一眼就能看清整个 IRQ 布局。向量号从 0x20 开始——正好对应 PIC 重映射后的 master 偏移量。所有 IRQ handler 都使用内核中断门（DPL=0, IF cleared），IST 偏移为 0。

默认 handler 的工作就是发送 EOI 然后返回——这样即使某个我们没配驱动的 IRQ 线上产生了噪声信号，PIC 也不会卡住：

```cpp
extern "C" void irq_default_handler(InterruptFrame* /*frame*/) {
    PIC::send_eoi(0);
}
```

这里有一点不完美——`irq_default_handler` 无法区分是哪条 IRQ 线触发的，所以只能向 master 发 EOI。对于 IRQ0-7 来说没问题，但如果 slave 上的 IRQ（8-15）误触发了默认 handler，EOI 只发给了 master 而没发给 slave，slave 那边会一直处于"等待 EOI"的状态。不过当前阶段所有 16 条 IRQ 都被屏蔽着，只有显式 unmask 的 IRQ 才会产生中断，所以这个问题暂时不会暴露。后续如果要支持动态 IRQ 注册，这里需要改成能感知具体 IRQ 号的实现。

Linux 对这个问题的处理方式是：使用 `struct irq_chip` 的 `irq_mask_ack` 回调，在 mask_and_ack_8259A() 函数里用 specific EOI（0x60 + irq）精确地清除指定 IRQ 的 In-Service 位，并且加上 spurious IRQ 检测——如果 ISR（In-Service Register）显示该 IRQ 并不在服务中，就不发 EOI，因为那是一个伪中断。这种细致程度在教学内核里暂时不需要，但了解这些有助于将来做 APIC 迁移时知道应该往哪个方向扩展。

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

看到 `[TICK] uptime: Ns` 每秒稳定输出一行，就说明整条链路完全通畅：PIT 硬件以 100 Hz 产生方波 → IRQ0 信号到达 master PIC → PIC 把它转译成 INT 0x20 → CPU 查 IDT 向量 0x20 → 跳到 `irq0_stub` → 保存寄存器 → 调用 `pit_irq0_handler` → 递增 tick_count → 每秒打印 uptime → `PIC::send_eoi(0)` → ISR stub 恢复寄存器 → IRETQ → 回到 hlt 等待下一次中断。

断点测试也正常返回，说明 PIC/IRQ 的加入没有破坏之前 milestone 010 搭好的 CPU 异常处理链路——两者和平共处，各管各的向量号范围。

## 设计决策回顾：从 PIC 到 APIC 的过渡准备

你可能会问——既然 8259A 已经是老古董了，为什么还要花时间写它的驱动？

答案在于分层和渐进。SerenityOS 直接使用 APIC 而非 8259A，其中断系统用 `GenericInterruptHandler` 基类实现动态注册，每个 CPU 有独立的 GDTR/IDTR，支持 SMP。APIC 的编程模型比 8259A 复杂得多——它使用 MMIO 而不是 I/O 端口，有多组寄存器需要配置，支持多核中断路由，还有 local APIC 和 I/O APIC 两层架构。如果我们一上来就搞 APIC，需要同时解决 MMIO 映射、ACPI 表解析、多核初始化三个维度的问题，调试难度会指数级增长。

先用 8259A 把中断驱动的框架搭好——PIC 初始化、IRQ 注册、handler 编写、EOI 信号、mask/unmask——这些概念在 APIC 里完全一样，只是编程接口不同。等框架跑通了，后面迁移到 APIC 只需要把底层 I/O 端口操作换成 MMIO 读写，上层的 handler 注册和 EOI 逻辑基本不用改。同理，PIT 到 HPET/LAPIC Timer 的迁移也是类似——上层接口（`get_ticks()`、`get_uptime_ms()`）保持不变，只换底层的硬件编程。这种"先简单后复杂"的分层策略在 OS 开发中非常实用，能把每次调试的范围控制在一个可管理的维度内。

## 收尾

到这里，大内核终于"活"过来了。回顾一下 milestone 011 做了什么：用 C++ class 封装了 8259A PIC 驱动（ICW1-ICW4 重映射、EOI 信号、mask/unmask），封装了 8254 PIT 驱动（channel 0 square wave 模式、tick 计数、uptime 追踪），用数据驱动的路由表注册了 16 个 IRQ handler 到 IDT，最后在 kernel_main 里按正确顺序初始化所有组件、unmask IRQ0、sti 开中断。验证结果是串口每秒稳定输出 `[TICK] uptime: Ns`，CPU 第一次真正响应了外部硬件事件。

下一步的方向很明确——键盘驱动（IRQ1）让内核能接收用户输入，然后是更精细的时间管理（睡眠、定时器队列），再之后是进程调度。内核有了心跳，离"能和外部世界互动"就不远了。

## 参考资料

**Intel 手册**：
- Intel 64 and IA-32 Architectures SDM, Volume 3A
  - Section 6.2 — Exception and Interrupt Vectors
  - Section 6.12 — IDT Gate Types
  - Section 6.14 — 64-Bit Mode Interrupt Handling
- https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

**OSDev Wiki**：
- [8259 PIC](https://wiki.osdev.org/8259_PIC) — PIC 重映射、ICW 序列、EOI 机制
- [Programmable Interval Timer](https://wiki.osdev.org/Programmable_Interval_Timer) — PIT 8254 编程指南
- [Interrupts](https://wiki.osdev.org/Interrupts) — x86 中断机制总览

**其他 OS 实现**：
- xv6 `picirq.c` — 极简 PIC 初始化（https://github.com/mit-pdos/xv6-public/blob/master/picirq.c）
- Linux `arch/x86/kernel/i8259.c` — 生产级 8259A 驱动（https://github.com/torvalds/linux/blob/master/arch/x86/kernel/i8259.c）
- SerenityOS `Kernel/Arch/x86_64/Interrupts/APIC.cpp` — APIC 中断系统（https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp）
