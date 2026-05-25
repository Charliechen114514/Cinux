---
title: 005-mini-kernel-entry-1 · 内核入口
---

# 让内核开口说话：串口驱动的原理与实现

> 标签：UART, 串口驱动, I/O 端口, PMIO, inline assembly, NS16550A
> 前置：[004C 从 BootInfo 到高半核跳转](004-boot-load-mini-kernel-c-1.md)

## 写在前面

到 tag 004C 为止，我们的 bootloader 已经完成了全部使命——从 MBR 启动、加载 Stage2、穿越保护模式、建立页表、进入 Long Mode、填充 BootInfo，最后用一个 `jmp *%rax` 把控制权交给了小内核。跳转之后内核确实在跑，我们可以在 `debug.log` 里看到 `OPLJ` 加上 `1234GCPPGC1V` 这些 debugcon 字符，说明内核的汇编入口 (`boot.S`) 和 C++ 运行时初始化都在正常工作。

但说实话，用 debugcon 一个字符一个字符猜内核的状态，这种调试体验实在是惨不忍睹。`debug.log` 里的 `1234` 是什么意思？你得翻代码才知道：`1` 是设栈完成，`2` 是清 BSS 完成，`3` 是全局构造函数调用完成，`4` 是进入 `main`。这种"暗号式调试"在最初几步还凑合，但当内核要做的事情越来越多、你需要输出的信息越来越复杂时，你就会无比怀念 `printf` ——一个能把数字、字符串、指针格式化输出的函数。

tag 005 要做的事情就是给内核装上真正的"嘴巴"。第一步是写一个串口驱动，让内核能通过 UART 硬件向外部发送字符；第二步是手搓一个 `kprintf`，实现格式化输出——那是下一篇的内容。这一篇我们聚焦在串口驱动上：x86 的 I/O 端口是怎么回事，UART 的寄存器长什么样，怎么在 C++ 里优雅地封装硬件操作，以及这个过程中踩过的坑。

## 环境 & 背景

工具链没有变化：GNU AS (AT&T 语法) + GCC/G++ + CMake。本篇新增的文件只有三个：`kernel/mini/driver/io.h`（I/O 端口封装）、`kernel/mini/driver/serial.h`（串口类声明）和 `kernel/mini/driver/serial.cpp`（串口实现）。它们都位于 `kernel/mini/driver/` 目录下——这是我们第一次在内核里引入"驱动"这个概念。

运行环境是 QEMU，它模拟了一块 NS16550A 兼容的 UART 芯片。QEMU 的 `-serial stdio` 参数把虚拟机的 COM1 端口重定向到宿主机的终端，所以我们只要往 COM1 写字符，宿主机终端上就能看到输出。

## 两种 I/O 世界：PMIO 与 MMIO

在动手写代码之前，我们先搞清楚一个根本问题：CPU 怎么和外设通信？

x86 架构提供了两条路。第一条叫 Port-Mapped I/O（PMIO），使用专门的 `IN` 和 `OUT` 指令访问一个独立的 I/O 地址空间（和内存地址空间完全隔离）。第二条叫 Memory-Mapped I/O（MMIO），把外设的寄存器映射到普通的物理内存地址上，用普通的 `mov` 指令就能读写。x86 上两种方式都存在，但串口用的是 PMIO。Intel SDM Vol. 2A 第 3 章对 `IN` 和 `OUT` 指令有详细的描述（约第 3-370 页和第 3-510 页）：`IN` 指令从 I/O 端口读数据到 AL/AX/EAX 寄存器，`OUT` 指令把 AL/AX/EAX 的值写到 I/O 端口。端口号可以是 8 位立即数（0-255）或 DX 寄存器的值（0-65535）。

如果你接触过 RISC-V 或 ARM，可能会觉得 PMIO 有点奇怪——这两种架构根本没有 `IN`/`OUT` 指令，所有外设通信都是 MMIO。x86 的 PMIO 是一个历史遗留物，但在 PC 兼容机平台上，串口、VGA、PS/2 键盘这些经典外设仍然使用 PMIO，所以我们绕不过去。

## inb/outb：用内联汇编包装 I/O 指令

C++ 编译器不会帮你生成 `IN`/`OUT` 指令，所以我们必须用 GCC 内联汇编来包装它们。这是 `io.h` 的全部内容：

```cpp
#pragma once
#include <stdint.h>

namespace cinux::mini::io {

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

}  // namespace cinux::mini::io
```

只有两个函数，但每个细节都有讲究。先看 `inb` 的内联汇编模板 `"inb %1, %0"`，这是 AT&T 语法，操作数顺序是源在前、目的在后，意思就是"从 `%1`（端口）读到 `%0`（value）"。约束 `"=a"(value)` 告诉编译器输出放在 `al` 寄存器里——`a` 约束对应 `eax`/`rax`，但因为返回类型是 `uint8_t`，实际只用了低 8 位 `al`。`"Nd"(port)` 表示输入可以是 8 位立即数（`N` 约束，范围 0-255）或者 `dx` 寄存器（`d` 约束）。之所以允许两种形式，是因为 x86 的 `IN` 指令确实只支持这两种：端口号小于 256 时可以用立即数编码，超过 256 必须先加载到 DX。

`volatile` 这个关键字在这里绝对不能省。编译器的优化器可能会认为"两次连续的端口读操作结果相同，去掉后面那次就行了"，但 I/O 端口的值每次读都可能变化——状态寄存器、数据寄存器都是如此。`volatile` 告诉编译器：别动这条指令，别重排，别删。

你可能注意到我们用 `inline` 函数而不是 `#define` 宏。OSDev Wiki 上很多示例代码用的是宏定义，很多教学项目也是这么做的。但我们选择 `inline` 函数有几个实实在在的好处：编译器会检查参数类型（`uint16_t` 和 `uint8_t`），参数求值顺序确定（宏的参数可能被求值多次），而且在调试器里能看到函数名而不是一坨展开后的汇编文本。在 `-O1` 以上优化级别下，`inline` 函数会被完全内联掉，运行时开销和宏一模一样。

## UART 8250：一块比 x86 还老的芯片

串口背后的硬件叫 UART（Universal Asynchronous Receiver/Transmitter），PC 上用的是 NS16550A 兼容芯片，也常被称为 8250。这块芯片的历史可以追溯到 1980 年代的 IBM PC——比 x86-64 架构老了二十多年。但正因为如此古老，它的编程模型极其稳定：你在 2026 年写的 UART 驱动代码，放到 1990 年代的机器上照样能跑。

PC 标准定义了四个 COM 端口：COM1 在 I/O 端口 0x3F8、COM2 在 0x2F8、COM3 在 0x3E8、COM4 在 0x2E8。QEMU 默认把 COM1 重定向到 stdio（通过 `-serial stdio` 参数），所以我们只用 COM1 就够了。UART 的寄存器通过基址加偏移来访问，偏移 0 到 7 各对应一个寄存器。OSDev Wiki 的 Serial Ports 页面（https://wiki.osdev.org/Serial_Ports）有完整的寄存器映射表。

有几个寄存器特别重要。偏移 0 有双重身份：读操作访问 RBR（Receive Buffer Register，接收缓冲），写操作访问 THR（Transmit Holding Register，发送保持）——硬件根据 `IN`/`OUT` 指令自动区分。偏移 5 是 LSR（Line Status Register，线路状态），其中 bit 5 叫 THRE（Transmit Holding Register Empty），当它为 1 时表示 THR 已空，可以写入下一个字节——这是发送侧最关键的状态位。偏移 3 是 LCR（Line Control Register），控制数据格式——8 位数据、无校验、1 位停止位（通常缩写为 8N1）对应 LCR = 0x03。

现在我们来看 Cinux 串口驱动的头文件设计。所有常量用 `constexpr` 而不是 `#define`，所有寄存器偏移和状态位用 scoped 命名空间组织：

```cpp
namespace cinux::mini::serial {

constexpr uint16_t SERIAL_COM1 = 0x03F8;
// ... COM2, COM3, COM4 省略

namespace SerialReg {
constexpr uint8_t THR = 0;  // Transmit Holding Register (write)
constexpr uint8_t IER = 1;  // Interrupt Enable Register
constexpr uint8_t FCR = 2;  // FIFO Control Register
constexpr uint8_t LCR = 3;  // Line Control Register
constexpr uint8_t MCR = 4;  // Modem Control Register
constexpr uint8_t LSR = 5;  // Line Status Register
}  // namespace SerialReg

namespace SerialLSR {
constexpr uint8_t TX_READY = 0x20;  // bit 5: THR Empty
}  // namespace SerialLSR
```

`constexpr` 相比 `#define` 的好处是它有类型（`uint8_t`）、有作用域（在命名空间里）、可以被调试器识别，而且在编译期求值，运行时零开销。这些细节在一个教学 OS 里可能看起来是"小题大做"，但养成好习惯比事后纠正坏习惯容易得多。

## Serial 类：封装 UART 硬件操作

驱动的核心是一个 `Serial` 类：

```cpp
class Serial {
private:
    uint16_t base_port;

    bool is_tx_ready() const {
        return (io::inb(base_port + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
    }

public:
    explicit Serial(uint16_t port = SERIAL_COM1);
    void putc(char c);
    void puts(const char* s);
    void init();
};

Serial& get_initial_serial();
```

`Serial` 只有一个成员变量 `base_port`，所有操作都通过它加偏移来定位寄存器。`is_tx_ready()` 是私有方法，检查 LSR 的 bit 5 是否为 1。`get_initial_serial()` 返回一个全局单例的引用——后面会看到它怎么初始化。

接下来是 `serial.cpp` 的实现。先看初始化：

```cpp
Serial::Serial(uint16_t port) : base_port(port) {
    __asm__ volatile("movb $0x5C, %%al; outb %%al, $0xE9" ::: "eax");  // '\'
    init();
    __asm__ volatile("movb $0x27, %%al; outb %%al, $0xE9" ::: "eax");  // '''
}

void Serial::init() {
    __asm__ volatile("movb $0x5B, %%al; outb %%al, $0xE9" ::: "eax");  // '['
    io::outb(base_port + SerialReg::IER, 0x00);  // 禁用中断
    io::outb(base_port + SerialReg::LCR, 0x03);  // 8N1
    io::outb(base_port + SerialReg::FCR, 0xC7);  // 启用 FIFO
    io::outb(base_port + SerialReg::MCR, 0x03);  // RTS + DTR
}
```

你可能会觉得散落在各处的 `outb %%al, $0xE9` 看起来很丑。这些是往 QEMU debugcon 端口写的调试标记。在 `debug.log` 中你会看到类似 `\[1234'` 的序列：`\` 表示构造函数开始，`[` 表示 init 开始，`1234` 分别表示 IER/LCR/FCR/MCR 四步完成，`'` 表示构造函数结束。这种"在每个关键步骤后往 debugcon 打一个字符"的做法在裸机开发中极为常见——当内核还在三步并作两步地初始化自己时，你不可能调 `printf` 来调试，因为 `printf` 本身就是你正在调试的东西。一个字符一个字符地往 debugcon 写是最原始也最可靠的诊断手段。

init 的四步对应 UART 的标准初始化序列，每一步都有明确的目的。第一步 `IER = 0x00` 禁用所有中断——因为我们用轮询模式发送数据，不依赖 UART 的中断机制。第二步 `LCR = 0x03` 设置 8N1 格式——8 位数据位、无奇偶校验、1 位停止位，这是串口通信中最通用的配置。第三步 `FCR = 0xC7` 启用 FIFO 并清空缓冲区：bit 0 = 1 启用 FIFO，bit 1-2 = 1 清空发送和接收 FIFO，bit 6-7 = 11 设置 14 字节的 FIFO 触发阈值。第四步 `MCR = 0x03` 拉高 RTS（Request To Send）和 DTR（Data Terminal Ready）信号——告诉对端"我准备好了，可以通信"。

你可能会问：为什么没设置波特率？物理 UART 需要通过 DLAB（Divisor Latch Access Bit）和除数寄存器来配置波特率，流程是先设 LCR bit 7 = 1 进入 DLAB 模式，然后往偏移 0 和 1 写入除数值（115200 / 目标波特率）。但 QEMU 的模拟 UART 默认就是 115200 bps，不需要手动设置。Cinux 目前只针对 QEMU 开发，所以这一步省了。OSDev Wiki 的初始化示例代码里是有 DLAB 设置步骤的，如果你将来要在真实硬件上跑，记得加上。

接下来是字符发送——这是串口驱动最核心的操作：

```cpp
void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }
    io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}
```

这是一个典型的忙等待（busy-wait）发送模式。循环查询 LSR 的 bit 5，直到 THR 空闲才写入。`__asm__ volatile("pause")` 是 x86 的 `PAUSE` 指令，Intel SDM Vol. 2B 中对它有说明：在自旋锁循环中插入它可以提示 CPU 这是一个自旋等待，降低功耗并减少总线上的访存冲突。在超线程处理器上，`PAUSE` 还能让出执行资源给另一个逻辑线程。虽然 QEMU 里这个指令基本上是空操作，但写上它是好习惯——万一将来在真机上跑呢。

然后是字符串发送，里面藏着一个很容易踩的坑：

```cpp
void Serial::puts(const char* s) {
    if (s == nullptr) return;

    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}
```

坑就在 `\n` 到 `\r\n` 的转换上。Unix 系统用 `\n`（LF，0x0A）表示换行，但串口终端（以及很多 Windows 程序）期望 `\r\n`（CR+LF，0x0D 0x0A）。如果你不做转换，QEMU 串口输出会变成"阶梯状"——每行的开头紧跟在上一行的末尾后面，而不是从新行的最左侧开始。说实话这个坑我踩了，调了半天才发现是少了 `\r`。所以每次遇到 `\n`，先发一个 `\r` 把光标挪到行首，再发 `\n` 把光标挪到下一行。

最后是全局单例：

```cpp
static Serial g_serial(SERIAL_COM1);

Serial& get_initial_serial() {
    return g_serial;
}
```

`g_serial` 是一个全局静态对象，它的构造函数会在 `main` 之前通过 `.init_array` 机制被调用——这意味着串口在 `mini_kernel_main` 执行之前就已经初始化完毕了。用全局构造函数来初始化串口是有意为之的设计决策：串口不依赖任何动态内存分配，不依赖其他全局对象，而且越早可用越好。如果改用"延迟初始化"（第一次调用时才 init），你就要处理并发问题和重入问题，复杂度不值得。

这里有一个非常微妙的依赖链：`g_serial` 的构造函数会调用 `Serial::init()`，后者调用 `io::outb()`，而 `outb` 是 inline 函数不需要链接。所以 `g_serial` 只依赖 I/O 端口操作这一个东西——在内核启动的最早阶段，I/O 端口是肯定可用的（我们运行在 Ring 0，IOPL 允许所有 I/O 操作），因此全局构造函数初始化串口是安全的。

## 从 QEMU debugcon 到真实 UART 的跨越

现在我们有了两种输出手段：debugcon（端口 0xE9）和串口（COM1，端口 0x3F8）。它们的区别值得说清楚。

debugcon 是 QEMU 私有的调试设备，往端口 0xE9 写一个字节，QEMU 就把内容输出到配置的文件或终端。它不需要任何初始化，不需要查状态位，直接写就行。速度极快，因为完全没有硬件模拟的开销。代价是真实硬件上 0xE9 什么都不是——你的 debugcon 输出代码在真机上跑就是一堆对空 I/O 端口的写入操作，什么都不会发生。

串口则是一块真实的硬件。COM1 的 UART 芯片有自己的寄存器、FIFO、状态位，发送前必须等 THR 空闲。在 QEMU 里这些都有模拟，而且 QEMU 会把 COM1 的数据通过 `-serial stdio` 重定向到宿主机终端。在真实硬件上，串口连接到物理的 RS-232 接口或者 USB 转串口芯片。所以串口驱动是"真"驱动——它操作的硬件在真实 PC 上确实存在。

我们做双通道输出的策略是：`kprintf` 走串口，`kdebugf` 走 debugcon。正式输出（比如 BootInfo 信息、错误报告）用 kprintf，高频调试信息（比如"进入函数 X"）用 kdebugf，避免污染串口输出。这给开发者更灵活的调试选择。

## 与 xv6 uart.c 和 Linux 8250 驱动的对比

说到串口驱动，我们不妨看看其他操作系统是怎么做的，这样能更清楚地看到 Cinux 设计决策的位置。

xv6 的 UART 驱动在 `kernel/uart.c` 中（https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c）。因为 xv6 跑在 RISC-V 上，它用的是 MMIO 而不是 PMIO——UART 寄存器被映射到一段物理内存地址上，用 `ReadReg`/`WriteReg` 宏（本质是 `*(volatile uint8_t *)addr`）来读写。但寄存器的布局和 Cinux 完全一样——THR、IER、FCR、LCR、LSR 偏移不变，初始化序列也大同小异。这说明 16550A 的编程模型真的是跨架构跨时代的。

一个有意思的差异是 xv6 支持中断驱动的 UART 收发。它用 `uartinit()` 启用了 UART 的接收和发送中断（`IER_RX_ENABLE | IER_TX_ENABLE`），发送时通过 `uartputc()` 把字符放进缓冲区然后触发中断，真正的数据搬移由中断处理程序完成。同时 xv6 也保留了 `uartputc_sync()` 作为轮询发送的回退——这个函数和 Cinux 的 `Serial::putc()` 几乎一模一样：查 LSR THRE 位，写 THR。Cinux 在当前阶段只用轮询模式，因为我们的内核还没有实现 IDT 和中断控制器。等这些基础设施到位后，完全可以像 xv6 一样切换到中断驱动模式。

Linux 的 8250 驱动就在另一个量级了。`drivers/tty/serial/8250/` 目录下有超过 20 个源文件，支持的 UART 芯片变体有十几种，处理了各种硬件 quirk（比如某些芯片的 FIFO 有 bug）、自动探测端口、运行时配置波特率、支持 DMA 传输……这些复杂度的来源是 Linux 需要在数千种硬件配置上工作。Cinux 只需要支持 QEMU 模拟的这一块芯片，所以我们的驱动只有不到 100 行——但核心逻辑（init 四步、轮询发送、LSR 查询）和 Linux 的早期初始化代码是一致的。

Linux 的 `earlycon`/`earlyprintk` 机制和 Cinux 的 kprintf 概念上很像：在完整的控制台子系统初始化之前，用一个简单的轮询 UART 驱动提供早期输出。`earlycon` 的输出函数也是直接查 LSR bit 5 然后写 THR——和我们的 `Serial::putc()` 一样。这说明不管操作系统有多复杂，启动阶段"往串口写字符"这件事的本质是不变的。

## 收尾

来验证一下串口驱动是否正常工作。构建并运行：

```bash
cd build && cmake -DCINUX_BUILD_TESTS=ON .. && make -j$(nproc)
make run
```

如果一切正确，你的终端（QEMU 串口输出）应该能看到类似这样的输出：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xffffffff80020000, kernel_phys_base=0x20000
Boot Memory Info: mmap_count=5
  [0] base=0x0000000000000000, length=0x000000000009fc00, type=1, acpi=1
  ...
```

如果你只看到 debug.log 里有输出但终端没有，检查一下 QEMU 参数里有没有 `-serial stdio`。如果终端输出是乱码，那可能是波特率不匹配——但在 QEMU 里这基本不会发生。

回头看这一篇，我们做的事情其实很简单：两个 inline 函数包装了 x86 的 I/O 指令，一个 Serial 类封装了 UART 的初始化和字符发送，一个全局对象让串口在 `main` 之前就可用。但"简单"不意味着"不重要"——串口是内核最底层的调试设施，后面所有的格式化输出、日志系统、调试信息都建立在它之上。

下一篇我们在这条"管道"上搭建 `kprintf` 格式化引擎，实现从数字到文字的转换——那才是真正有算法含量的部分。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 2A, Chapter 3 — IN 指令（约第 3-370 页）：描述 `IN AL, imm8` 和 `IN AL, DX` 两种形式，8 位端口可作为立即数或 DX 寄存器值。OUT 指令（约第 3-510 页）：描述 `OUT imm8, AL` 和 `OUT DX, AL` 两种形式。Cinux 的 `inb`/`outb` 函数直接映射到这些指令。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Chapter 18 — I/O Protection：IOPL 字段在 EFLAGS 中控制 IN/OUT 指令的特权级别。Cinux 内核运行在 Ring 0，IOPL = 0，所有 I/O 端口操作都是允许的。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- OSDev Wiki — Serial Ports (https://wiki.osdev.org/Serial_Ports)：UART 寄存器映射表、标准初始化序列、LSR 位定义、轮询发送示例代码。COM1 基地址 0x3F8，THR/LSR 寄存器偏移。Cinux 的初始化序列参考了这个页面。

- xv6 RISC-V `uart.c` (https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c)：MIT 教学操作系统的 UART 驱动。使用 MMIO（RISC-V 架构）而非 PMIO，支持中断驱动 + 轮询回退。`uartputc_sync()` 的逻辑和 Cinux 的 `Serial::putc()` 几乎一致。
