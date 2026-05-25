---
title: 012-driver-serial-1 · 串口驱动
---

# 从硬件到软件：x86_64 串口驱动的设计与实现

> 标签：x86-64, UART, 16550, 串口驱动, 端口映射 I/O, PMIO, polling, 8N1, 内核开发, QEMU
> 前置：[011 — 大内核 PIC 与 PIT：让内核第一次听到硬件的心跳](011-big-kernel-pic-irq-1)

---

## 前言

milestone 011 结束的时候，大内核已经能通过 PIT 每秒输出一行 `[TICK] uptime: Ns` 了——但如果你回头看看之前的章节，会发现一个一直被我们含糊带过的东西：kprintf 本身到底是怎么把字符送到屏幕上的？从 tag 009 开始，我们就在用 `kprintf` 打印调试信息，一路从 mini 内核用到大内核，可底层的数据通路始终是个黑盒：格式化引擎把字符吐给某个"输出设备"，然后字符就神奇地出现在了 QEMU 的终端窗口里。

这一章我们要拆开这个黑盒，从最底层的硬件开始，亲手写一个 UART 16550 串口驱动。

你可能会问：为什么是串口而不是 VGA 文本模式或者 framebuffer？原因在于串口是内核开发中最基础、最可靠的输出通道——它不依赖视频内存初始化，不依赖任何图形硬件状态，只要 CPU 能执行 `out` 指令就能工作。在内核启动的最早期阶段，GDT、IDT、内存管理全都还没就绪，串口几乎是唯一的调试手段。几乎所有操作系统项目——从 xv6 到 Linux 到 SerenityOS——都用串口作为 early console 的首选输出设备。

我们这一章要讨论的核心问题有三个：UART 16550 硬件的编程模型到底是什么样的，如何通过 x86 端口映射 I/O 与它交互，以及 Cinux 的 `Serial` 驱动类是怎么设计出来的。在这个过程中我们会横向对比 xv6、Linux 和 SerenityOS 的串口驱动实现，看看不同的操作系统为什么会在同一块硬件上做出截然不同的设计选择。

## 环境说明

和之前几个 milestone 保持一致：x86_64 平台，GNU AS（AT&T 语法）+ GCC/G++ + CMake 构建，QEMU 模拟运行。Cinux 大内核仍然是 freestanding C++23，编译标志 `-mcmodel=large -mno-red-zone -fno-exceptions -fno-rtti`，无标准库。

一个需要特别说明的硬件约束是：我们完全依赖 QEMU 的虚拟 UART。QEMU 默认模拟的 16550 UART 工作在 115200 波特率，这意味着我们不需要在初始化时手动设置 DLAB 分频器——QEMU 帮我们搞定了。如果你以后想把 Cinux 跑在真机上，这一步就必须补上，但在当前的 QEMU-only 开发流程中，跳过 DLAB 编程是合理的简化。

## UART 16550：一块活了三十多年的芯片

在动手写驱动之前，我们先来搞清楚 UART 16550 到底是个什么东西，以及它是怎么被编程的。

UART 全称 Universal Asynchronous Receiver/Transmitter，意思是"通用异步收发器"。它的工作是把 CPU 并行输出的字节数据转换成串行信号（一个 bit 一个 bit 地发出去），反过来也负责把串行信号重新组装成字节交给 CPU。16550 是这款芯片的型号，由 National Semiconductor 在 1980 年代设计，是 IBM PC 兼容机的标准串口控制器。虽然今天你买到的主板早就不用独立的 16550 芯片了（功能被集成到了 Super I/O 芯片或主板芯片组中），但编程接口——寄存器布局、I/O 端口地址、初始化序列——和四十年前一模一样。QEMU 模拟的也是这个接口。

x86 PC 上，UART 通过端口映射 I/O（Port-Mapped I/O，PMIO）访问。CPU 使用专门的 `IN` 和 `OUT` 指令与 I/O 端口交互，而不是像内存映射 I/O（MMIO）那样用普通的 `MOV` 指令读写某个物理地址。Intel SDM Vol. 2A Chapter 3 对 `IN`/`OUT` 指令有详细说明：端口地址是 16 位的，可以用立即数指定（适用于 0-255 范围内的端口），也可以放在 DX 寄存器中（适用于全部 65536 个端口）。数据宽度支持 byte（AL）、word（AX）和 dword（EAX），在 64 位模式下操作数大小固定为 8 或 32 位。

PC 标准定义了四个 COM 端口，每个对应一个 UART 芯片实例，基地址是硬件固定的：

| COM 端口 | I/O 基地址 |
|----------|-----------|
| COM1 | 0x3F8 |
| COM2 | 0x2F8 |
| COM3 | 0x3E8 |
| COM4 | 0x2E8 |

这些地址可以追溯到 IBM PC/AT 的硬件设计——在那个年代，硬件资源分配就是靠硬连线或者跳线帽完成的，没有什么"自动协商"。到今天，QEMU 和所有 x86 硬件模拟器依然沿用这个映射。Cinux 只使用 COM1（0x3F8），因为内核调试输出只需要一个通道就够了。

每个 UART 有 8 个寄存器，通过 `base_port + offset` 访问。offset 从 0 到 7，但有些寄存器共享同一个 offset——读操作和写操作映射到不同的寄存器，或者通过 LCR 的 DLAB 位切换不同的含义。以下是 Cinux 驱动关心的核心寄存器（DLAB=0 时）：

| Offset | 读操作 | 写操作 | 用途 |
|--------|--------|--------|------|
| 0 | RBR | THR | 接收缓冲 / 发送保持 |
| 1 | - | IER | 中断使能 |
| 2 | IIR | FCR | 中断标识 / FIFO 控制 |
| 3 | LCR | LCR | 线路控制（数据位、校验、停止位、DLAB） |
| 4 | MCR | MCR | Modem 控制 |
| 5 | LSR | LSR | 线路状态（收发就绪标志） |

其中最重要的两个是 THR（Transmit Holding Register，发送保持寄存器）和 LSR（Line Status Register，线路状态寄存器）。THR 是你要发送的字节的目的地——往 `base_port + 0` 写一个字节，UART 就会把它串行发送出去。LSR 的 bit 5（THRE，Transmit Holding Register Empty）告诉你 THR 是不是空的，是不是可以安全地写入下一个字节。这就是轮询发送的全部基础：读 LSR，等 bit 5 变成 1，然后写 THR。

## 第一步——I/O 原语与端口访问

在直接操作 UART 寄存器之前，我们需要一层最底层的 I/O 原语。Cinux 把它们封装在 `kernel/arch/x86_64/io.hpp` 中：

```cpp
namespace cinux::io {

inline uint8_t io_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0"
                     : "=a"(value)
                     : "Nd"(port)
                     : "memory");
    return value;
}

inline void io_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1"
                     :
                     : "a"(value), "Nd"(port)
                     : "memory");
}

}  // namespace cinux::io
```

`io_inb` 对应 `IN AL, imm8` 指令（当端口号 <= 255 时用立即数，否则通过 DX），`io_outb` 对应 `OUT imm8, AL`。内联汇编的约束 `"Nd"(port)` 表示端口号可以用立即数或 DX 寄存器传入，`"=a"(value)` 表示结果放在 AL 中。`"memory"` clobber 告诉编译器这条指令会影响内存（I/O 指令本身就是同步操作），不要把内存访问重排到 I/O 指令的另一侧。这是内核里 inline assembly 的标准写法——没有花哨的东西，就是薄薄的一层封装。

## 第二步——Serial 类的结构设计

有了 I/O 原语，我们就可以开始构建串口驱动了。Cinux 的驱动封装在 `kernel/drivers/serial.hpp` 和 `serial.cpp` 中，核心是一个 `Serial` 类：

```cpp
namespace cinux::drivers {

constexpr uint16_t SERIAL_COM1 = 0x03F8;
constexpr uint16_t SERIAL_COM2 = 0x02F8;
constexpr uint16_t SERIAL_COM3 = 0x03E8;
constexpr uint16_t SERIAL_COM4 = 0x2E8;

namespace SerialReg {
constexpr uint8_t RBR = 0;  // Receive Buffer Register  (read)
constexpr uint8_t THR = 0;  // Transmit Holding Register (write)
constexpr uint8_t IER = 1;  // Interrupt Enable Register
constexpr uint8_t FCR = 2;  // FIFO Control Register
constexpr uint8_t LCR = 3;  // Line Control Register
constexpr uint8_t MCR = 4;  // Modem Control Register
constexpr uint8_t LSR = 5;  // Line Status Register
constexpr uint8_t MSR = 6;  // Modem Status Register
constexpr uint8_t SCR = 7;  // Scratch Register
}  // namespace SerialReg

namespace SerialLSR {
constexpr uint8_t RX_READY  = 0x01;  // Data available in RBR
constexpr uint8_t TX_READY  = 0x20;  // THR empty, safe to write
}  // namespace SerialLSR

class Serial {
public:
    explicit Serial(uint16_t port = SERIAL_COM1);
    void init(uint16_t port = SERIAL_COM1, uint32_t baud = 115200);
    void putc(char c);
    void puts(const char* s);
    bool is_ready() const;

private:
    uint16_t base_port_;
    bool is_tx_ready() const;
};

}  // namespace cinux::drivers
```

COM 端口地址和 UART 寄存器偏移用 `constexpr` 常量定义——编译期确定，零运行时开销。`SerialReg` 和 `SerialLSR` 是嵌套的命名空间，把相关的常量分组在一起，避免污染外层命名空间。`Serial` 类本身非常精简：构造函数只存储端口号，不做硬件配置；真正的初始化延迟到 `init()` 调用时才发生。

这里有一个值得展开说说的设计选择：为什么"构造不初始化"？在内核启动阶段，全局对象的构造函数执行顺序是不完全确定的（虽然 C++ 标准规定了同一编译单元内的顺序，但跨编译单元的顺序是未定义的）。如果 `Serial` 在构造时就操作硬件，可能会在其他关键基础设施还没就绪的情况下触发 I/O——虽然对于串口这种简单设备来说一般不会出问题，但"延迟初始化"是内核编程中一个更安全、更通用的模式。我们在 `kernel_main()` 里显式调用 `kprintf_init()` 来触发串口初始化，此时 CPU 状态、栈、BSS 段都已经设置好了，一切尽在掌控。

## 第三步——UART 初始化序列

`Serial::init()` 是驱动的起点，也是和硬件"握手"的过程：

```cpp
void Serial::init(uint16_t /*port*/, uint32_t /*baud*/) {
    // Disable interrupts, set 8N1, enable FIFO, set MCR, verify LSR
    io_outb(base_port_ + SerialReg::IER, 0x00);
    io_outb(base_port_ + SerialReg::LCR, 0x03);
    io_outb(base_port_ + SerialReg::FCR, 0xC7);
    io_outb(base_port_ + SerialReg::MCR, 0x03);
    io_inb(base_port_ + SerialReg::LSR);
}
```

只有五条 I/O 指令，但每一条都有明确的意图。

第一条 `IER = 0x00` 禁用所有 UART 中断。我们采用的是纯轮询模式，不需要 UART 产生任何中断信号——发送时我们主动轮询 LSR，接收时（如果有的话）也一样。在更复杂的驱动里，IER 会被用来使能接收数据中断（bit 0）和发送空中断（bit 1），以实现中断驱动的异步收发。但对于现在的 Cinux 来说，轮询就是最简单、最可靠的选择。

第二条 `LCR = 0x03` 设置 8 个数据位、无校验、1 个停止位——经典的 8N1 配置。`0x03` 的位含义是 bit 0-1 = `11`（8 位数据），bit 2 = `0`（无校验），bit 3 = `0`（1 个停止位），bit 7 = `0`（DLAB 关闭）。OSDev Wiki 的 Serial Ports 页面对这个寄存器有完整的位域说明。

第三条 `FCR = 0xC7` 启用 FIFO 并设置触发阈值。`0xC7` = `11000111b`：bit 0 启用 FIFO，bit 1 清空接收 FIFO，bit 2 清空发送 FIFO，bit 6-7 = `11` 设置接收 FIFO 触发阈值为 14 字节。14 字节阈值在轮询发送模式下不太重要（反正我们只关心发送），但设置好 FIFO 能让 UART 在接收方向上有一定的缓冲能力。

第四条 `MCR = 0x03` 设置 RTS（Request To Send）和 DTR（Data Terminal Ready）信号。这是 RS-232 的 Modem 控制信号，在 QEMU 的虚拟环境中意义不大，但某些终端程序会检查这些信号来决定是否建立连接，所以设置一下比较稳妥。

最后一条 `io_inb(LSR)` 是一个验证读——读一次 LSR 确保它能正常响应，同时清除了可能存在的挂起状态。这是一个防御性的操作，不影响功能但增加了初始化的可靠性。

你会发现 `init()` 的两个参数 `port` 和 `baud` 被注释掉了，完全没有使用。这是因为端口地址已经在构造函数中确定了，波特率依赖 QEMU 的默认值 115200。参数保留在接口上是为了将来的兼容性——如果我们以后要支持动态切换波特率或端口，调用方不需要改签名。

## 第四步——轮询发送：putc 和 puts

驱动最核心的方法是 `putc()`，往串口发送一个字符：

```cpp
void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }
    io_outb(base_port_ + SerialReg::THR, static_cast<uint8_t>(c));
}
```

逻辑很直白：进入一个 `while` 循环，反复读 LSR 寄存器，等 bit 5（TX_READY）变成 1——意味着 THR 已空，可以安全地写入下一个字节了。循环体里的 `__asm__ volatile("pause")` 是 x86 的 hint 指令，告诉 CPU"我在等一个资源，你可以降低功耗或者让出超线程资源给另一个逻辑核"。在 spin-wait 循环里使用 `pause` 是好习惯，虽然对 QEMU 单核环境来说区别不大，但在真实的多核平台上能显著减少功耗和总线争用。

`is_tx_ready()` 是这个轮询机制的基础：

```cpp
bool Serial::is_tx_ready() const {
    return (io_inb(base_port_ + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
}
```

一次 `io_inb` 读 LSR，用位与操作检查 bit 5。如果 THR 为空就返回 true，否则返回 false。就这么简单。

`puts()` 是 `putc()` 的字符串版本，加上了一个 `\n` 到 `\r\n` 的转换：

```cpp
void Serial::puts(const char* s) {
    if (s == nullptr) {
        return;
    }
    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}
```

串口终端和 Unix 文件系统在换行约定上不一样——终端需要 `\r\n`（回车 + 换行）才能正确换行，而内核代码里我们习惯用 `\n`。`puts()` 自动帮你做这个转换，这样 `kprintf("hello\n")` 在终端上就能正常显示，不需要每次手动写 `\r\n`。`nullptr` 检查是防御性的，避免空指针导致崩溃。

## 放眼望去——其他操作系统怎么做串口驱动

写完自己的驱动之后，我们回头看看其他操作系统是怎么处理同一块 16550 硬件的，这对理解设计权衡非常有帮助。

### xv6：MMIO 与中断驱动的 RISC-V 串口

xv6（MIT 的教学操作系统）的 UART 驱动和 Cinux 有一个根本性的架构差异：xv6 运行在 RISC-V 上，而 RISC-V 没有 x86 那样的独立 I/O 端口空间，所有设备寄存器都是内存映射的（MMIO）。所以 xv6 的 UART 操作用的是 `*(volatile uint8_t *)UART0` 这种指针读写，而不是 `inb`/`outb` 指令。这是一个很好的例子——同样的 16550 编程模型，换个 CPU 架构就得换一套 I/O 访问方式。

除了 I/O 方式的差异，xv6 的 UART 驱动还支持中断驱动的收发。它有一个 `uartputc_sync()` 用于早期轮询输出（和 Cinux 的 `putc` 几乎一模一样：`while((ReadReg(LSR) & LSR_TX_IDLE) == 0); WriteReg(THR, c)`），以及一个中断驱动的 `uartputc()`，后者用 `tx_lock` 保护一个发送缓冲区，配合 `sleep()`/`wakeup()` 机制实现异步输出。xv6 还使能了 UART 的接收和发送中断（IER_RX_ENABLE | IER_TX_ENABLE），而 Cinux 干脆把 IER 设成 0 全部关掉了。

为什么 xv6 需要中断驱动而 Cinux 不需要？原因在于 xv6 是一个多进程操作系统，有用户程序在运行。如果 `printf` 每次都死等串口发送完毕，会浪费大量 CPU 时间——特别是当串口波特率较低时，每发送一个字符 CPU 都要空转好几百个周期。中断驱动让 CPU 在等待串口的时候可以去干别的事情。Cinux 现在还是单核裸机内核，没有并发任务，`kprintf` 的场景全部是内核主动打印调试信息，死等串口完全没问题。

### Linux：乐观写后轮询的多架构抽象

Linux 内核的早期串口控制台（`drivers/tty/serial/8250/8250_early.c`）展示了一个完全不同层次的设计思路。和 Cinux 的"保守轮询"（先等 THR 空再写）不同，Linux 采用的是"乐观写后轮询"——先把字符写到 THR 里，然后才轮询 LSR 等发送完成：

```c
static void serial_putc(struct uart_port *port, unsigned char c) {
    serial8250_early_out(port, UART_TX, c);  // 先写
    for (;;) {
        status = serial8250_early_in(port, UART_LSR);
        if (uart_lsr_tx_empty(status)) break;
        cpu_relax();
    }
}
```

两种策略各有道理。Cinux 的"先等后写"更安全——如果 THR 里已经有上一个字节还没发完，直接覆盖就会丢数据。Linux 的"先写后等"在特定场景下更快——如果 UART 恰好空闲，写完就可以去忙别的，等的时候再回来检查。不过这里有个前提：Linux 的 early console 是单线程顺序执行的，不存在并发写入的问题。

更值得注意的是 Linux 的多架构抽象。Linux 用 `struct uart_port` 封装了一个串口端口的所有信息，包括 `iotype`（PMIO、MMIO、MMIO16、MMIO32、MMIO32BE）和 `regshift`（寄存器之间的地址间隔），以及 `uartclk`（UART 时钟频率）。这意味着同一个驱动可以适配 x86 的端口 I/O、ARM 的内存映射 I/O、甚至大端序的 PowerPC 总线。Linux 还显式地计算和编程波特率分频器（`divisor = uartclk / (16 * baud)`），通过 DLAB 设置 DLL/DLM，而不是像 Cinux 那样依赖 QEMU 默认值。这是因为 Linux 要跑在无数种真实硬件上，每种硬件的 UART 时钟可能都不一样，不设波特率就乱套了。

另外一个有趣的差异是 FIFO 策略。Cinux 在初始化时启用了 FIFO（FCR = 0xC7），而 Linux 的 early console 反而禁用了 FIFO（FCR = 0）。Linux 的理由是 early console 追求的是简单可靠——启用 FIFO 意味着多了一层缓冲逻辑，在调试最早期崩溃的时候不如直来直去的 polling 可靠。这个取舍反映了两种不同的设计优先级：Cinux 在 QEMU 虚拟环境下不用担心硬件怪癖，所以大胆启用 FIFO；Linux 要兼容所有硬件，宁可保守一点。

### SerenityOS：从调试控制台到真正的 TTY 设备

SerenityOS 的 `Serial16550` 驱动把串口提升到了一个完全不同的抽象层次——它是一个挂载在 VFS 上的 `CharacterDevice`，用户空间程序可以通过 `/dev/ttyS0` 等设备文件打开、读取、写入串口，就像操作一个 Unix TTY 设备一样。

SerenityOS 的驱动架构有几个 Cinux 完全不具备的特征。首先是 `IOWindow` 抽象——一个统一的硬件寄存器访问层，自动处理 PMIO 和 MMIO 的差异。这样驱动代码不需要关心底层到底是 `inb`/`outb` 还是内存映射的 volatile 指针。其次是丰富的配置 API——波特率从 50 到 115200 都有对应的枚举值，校验位、停止位、数据位、FIFO 控制、Modem 控制全部参数化，调用方可以精确地配置通信参数。第三是 spinlock 保护——SerenityOS 是一个 SMP 系统，多个 CPU 核可能同时访问串口，必须用锁保护硬件访问的互斥性。Cinux 是单核内核，这个问题暂时不存在。第四是 CRLF 状态追踪——SerenityOS 记录了上一个输出的字符是不是 `\r`，避免在 `\r\n` 之后又追加一个 `\n` 导致多出一个空行。

说实话，Cinux 当前的 `Serial` 类和 SerenityOS 的 `Serial16550` 之间差了好几个量级的复杂度。但这个差距本身就有教学意义——它展示了一个"裸硬件调试输出驱动"和一个"生产级 TTY 设备驱动"之间的距离。SerenityOS 的做法告诉我们，串口驱动最终会演化成什么样：从最初的轮询 putc，到中断驱动，到缓冲区管理，到 VFS 集成，到用户空间访问，到 TTY 线路规程——每一步都有明确的动机和对应的硬件/软件需求。Cinux 现在处于这条演化路径的最起点，而理解起点和终点之间的差距，比直接跳到终点要更有教育价值。

## 收尾

到这里，Cinux 的 UART 16550 串口驱动就完成了。整个驱动的核心就是五条 I/O 指令的初始化序列加上一个轮询等待 + 写入的 `putc` 循环——不花哨，但完全满足当前的需求。通过对比 xv6、Linux 和 SerenityOS 的实现，我们看到了同一块硬件在不同设计约束下的多种实现方式：Cinux 追求的是教学内核的简洁性，xv6 展示了多进程环境下的中断驱动需求，Linux 演示了跨平台抽象的工程实践，SerenityOS 则呈现了一个真正生产级驱动应有的完整形态。

驱动写好了，但 `kprintf` 的格式化引擎还藏在 `kernel/lib/kprintf.cpp` 里和串口驱动耦合在一起。下一章我们要把它拆出来，做成一个硬件无关的模板引擎，让它既能输出到串口，也能在主机端的单元测试里直接验证——这才是真正的工程化。

## 参考资料

- Intel SDM: Vol. 2A Chapter 3 — IN/OUT 指令格式与操作数约束，8-bit 端口号用立即数，16-bit 端口号用 DX 寄存器。64-bit 模式下操作数大小固定为 8 或 32 位
- Intel SDM: Vol. 3A Section 2.5 "Control Registers" — I/O 特权级由 EFLAGS.IOPL 控制，TSS 的 I/O 权限位图提供细粒度端口访问控制
- OSDev Wiki: [Serial Ports](https://wiki.osdev.org/Serial_Ports) — UART 16550 完整寄存器映射、COM 端口地址、8N1 协议、波特率分频器计算
- xv6 (MIT) `uart.c` — RISC-V MMIO UART 驱动，轮询 `uartputc_sync()` 与中断驱动 `uartputc()` 并存，`tx_lock` + `sleep()/wakeup()` 异步输出
- Linux `drivers/tty/serial/8250/8250_early.c` — 早期串口控制台，乐观写后轮询模式，`struct uart_port` 多架构抽象（PMIO/MMIO/MMIO32）
- SerenityOS `Kernel/Devices/Serial/16550/Serial16550.cpp` — `CharacterDevice` VFS 集成，`IOWindow` PMIO/MMIO 抽象，spinlock SMP 保护，丰富配置 API
