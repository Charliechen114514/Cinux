# 从零在 x64 内核里驱动 PIC 和 PIT（一）：理解 PIC 与驱动封装

> 标签：x86-64, PIC, 8259A, IRQ, 中断控制器, 内核开发, C++, QEMU
> 前置：[010 GDT + IDT 教程](011-big-kernel-pic-irq-2.md)

---

## 前言

如果你跟我一样，是从 milestone 010 一路折腾过来的，那你应该记得上一章结尾我留了一个非常显眼的悬念——我们始终没有调用 `sti` 开中断。原因很简单：当时只配了 CPU 异常向量（0-14），IRQ（32-47）的 handler 全是空的，IDT 里未配置的向量 Present 位为 0，QEMU 的 PIT 定时器中断挂在 IRQ0 上，一开中断就会连环 triple fault。内核就像一台已经装好了发动机但没接油管的车——所有机械结构都在，但就是动不起来。

milestone 011 的目标就是把这条"油管"接上。具体来说，我们需要三样东西：PIC（8259A 可编程中断控制器）驱动，用来把硬件中断信号转译成 CPU 能理解的 INT 向量号；PIT（Intel 8254 可编程间隔定时器）驱动，用来以固定频率产生时钟中断；以及一套完整的 IRQ handler 注册机制，把 PIC 重映射后的向量号和对应的 ISR stub 绑定到 IDT 里。全部配好之后，内核串口每秒输出一行 `[TICK] uptime: Ns`——看似平淡无奇，但它的背后是 CPU 第一次真正意义上响应了外部硬件事件，内核从此不再是一个只会原地打转的死循环。

本教程分为三篇。本篇（第一篇）聚焦于 PIC 在中断链路中的角色和驱动封装。第二篇覆盖 PIT 定时器驱动和 IRQ 注册机制。第三篇完成 kernel_main 集成与上板验证。

从概念上说，milestone 010 到 011 的跨越是从"被动"到"主动"的转变。010 中的 CPU 异常是被动的——只有当程序执行了非法操作时才会触发。011 中的硬件中断是主动的——外部设备在需要 CPU 注意时主动发出信号，CPU 必须立即响应。这种"事件驱动"的模型是操作系统的核心运行模式——进程调度依赖定时器中断、用户输入依赖键盘中断、磁盘 I/O 依赖硬件完成中断。milestone 011 就是这个模型的第一步。

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
constexpr uint8_t ICW1_ICW4     = 0x01;
constexpr uint8_t ICW1_INIT      = 0x10;
constexpr uint8_t ICW4_8086      = 0x01;
// ... 还有 ICW1_SINGLE, ICW4_AUTO_EOI 等（头文件里列全了，这里不展开）
}
```

PIC 的 I/O 端口布局很简洁——master 占用 0x20（command）和 0x21（data），slave 占用 0xA0（command）和 0xA1（data）。command 端口用来发送 ICW 初始化序列和 EOI 信号，data 端口用来读写中断屏蔽寄存器（IMR）和 ICW2-ICW4。这些端口号是硬件固定的，任何 x86 系统都一样——从 1984 年的 IBM PC/AT 到今天的 QEMU 虚拟机，三十多年了一直没变。

8259A 的端口复用机制需要注意——同一个物理端口在不同时刻有不同的含义。当 PIC 处于正常工作模式（非初始化）时：command 端口用来发送 OCW（Operation Command Word）命令（比如 EOI = 0x20），也可以通过特定的 OCW3 命令读取 ISR（In-Service Register）和 IRR（Interrupt Request Register）的值来诊断中断状态；data 端口就是 IMR 的读写窗口——写入一个字节直接设置 8 个 IRQ 的屏蔽状态，读出一个字节获取当前屏蔽状态。当 PIC 进入初始化模式（收到 ICW1 后），data 端口的含义会依次切换为 ICW2、ICW3、ICW4 的接收寄存器。

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

---

## 参考资料

**Intel 手册**：
- Intel 64 and IA-32 Architectures SDM, Volume 3A
  - Section 6.2 — Exception and Interrupt Vectors
  - Section 6.12 — IDT Gate Types
- https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

**OSDev Wiki**：
- [8259 PIC](https://wiki.osdev.org/8259_PIC) — PIC 重映射、ICW 序列、EOI 机制
