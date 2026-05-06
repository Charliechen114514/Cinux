# 005 通读版 · 串口驱动与 I/O 端口封装——让内核开口说话

## 概览

到 tag 004C 为止，我们的 bootloader 已经完成了全部使命：从 MBR 启动、加载 Stage2、穿越保护模式、建立页表、进入 Long Mode，最后把控制权交给小内核。但在跳转之后，内核就像一个黑盒子——它活着吗？在干什么？我们一无所知。如果内核是个人，那它现在就是个哑巴。

tag 005 的核心任务就是给内核装上嘴巴。具体来说，我们要做三件事：首先用 x86 的 `in`/`out` 指令封装一套类型安全的 I/O 端口操作函数，然后基于这些函数写一个 NS16550A UART 串口驱动，最后把页表和链接脚本里潜伏的两个致命 bug 一并修掉。完成之后，内核就能通过串口向外部世界输出文字了——这听起来简单，但如果你亲身走过从"内核跳转成功但无任何输出"到"终端终于打印出 `Cinux Mini Kernel v0.1.0`"的全过程，你就会理解为什么这一步值得单独用一个 tag 来做。

本文覆盖的代码文件包括 I/O 端口封装（`kernel/mini/driver/io.h`）、串口驱动头文件和实现（`serial.h` / `serial.cpp`）、页表索引修正（`boot/common/long_mode.S`）、启动阶段新增的映射验证（`boot/stage2.S`）、链接脚本 LMA 计算修复（`kernel/mini/linker.ld`），以及 C++ 运行时存根更新（`kernel/mini/arch/x86_64/crt_stub.cpp`）。

## 架构图

先看清楚这一层代码在整个系统中的位置。从内核的角度出发，数据流动路径是这样的：

```
                    Cinux Mini Kernel 输出路径
                    =========================

  kprintf("Hello %d\n", 42)              ← 内核代码调用
       │
       ▼
  vkprintf_impl(lambda{serial.putc})     ← 格式化引擎（下篇覆盖）
       │
       ▼
  Serial::putc('H')                      ← 本篇覆盖
       │
       ▼
  Serial::is_tx_ready()?                 ← 查询 LSR bit 5
       │    │
       │    └─ NO → __asm__("pause")     ← 自旋等待
       ▼   YES
  io::outb(0x3F8 + THR, 'H')            ← 写入发送保持寄存器
       │
       ▼
  [x86 OUT 指令] → UART 硬件
       │
       ▼
  QEMU 捕获 COM1 端口写入 (-serial stdio)
       │
       ▼
  终端显示: H
```

从启动链条的角度看，本 tag 修复的两个关键 bug 处于跳转之前：

```
  [Long Mode Entry]
       │
       ├── setup_page_tables()           ← Bug Fix: PDPT 索引 + 高 32 位清零
       │
       ├── 页表验证 (identity + HH)      ← 新增诊断代码
       │
       ├── jmp *%rax (→ 0xFFFFFFFF80020000)
       │
       ▼
  [Mini Kernel]
       ├── _start (boot.S)               ← linker.ld LMA 修复
       ├── _init_global_ctors()          ← 修复后 init_array 才有正确的函数指针
       ├── g_serial 构造 (Serial::Serial)
       │       └── Serial::init()
       │               ├── outb(IER, 0x00)
       │               ├── outb(LCR, 0x03)
       │               ├── outb(FCR, 0xC7)
       │               └── outb(MCR, 0x03)
       └── mini_kernel_main()
               └── kprintf("Cinux Mini Kernel v0.1.0\n")
```

## 代码精讲

### I/O 端口封装——`io.h`

x86 架构有两种和外设通信的方式：内存映射 I/O（MMIO）和端口映射 I/O（PMIO）。串口用的是后者——通过专门的 `in` 和 `out` 指令访问独立的 I/O 地址空间。C++ 编译器不会帮你生成 `in`/`out` 指令，所以我们必须用内联汇编来包装它们。

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

只有两个函数，但每个细节都有讲究。先看 `inb`：内联汇编模板是 `"inb %1, %0"`，这是 AT&T 语法，操作数顺序是源在前、目的在后，意思是"从 `%1`（端口）读到 `%0`（value）"。约束 `"=a"(value)` 告诉编译器输出放在 `al` 寄存器里（`a` 约束对应 `eax`/`rax`，但因为是 `uint8_t` 所以只用低 8 位 `al`），`"Nd"(port)` 表示输入可以是 8 位立即数（`N` 约束，0-255）或者 `dx` 寄存器（`d` 约束）——x86 的 `in`/`out` 指令只接受这两种形式的端口号。

`volatile` 关键字在这里绝对不能省掉。编译器的优化器可能会认为"两次连续的端口读操作结果相同，去掉后面那次"，但 I/O 端口的值每次读都可能变化——状态寄存器、数据寄存器都是如此。`volatile` 告诉编译器：别动这条指令，别重排，别删。

`outb` 的约束模式对称：`"a"(value)` 是输入操作数放在 `al`，`"Nd"(port)` 是端口号。输出部分为空（`out` 指令不产生输出），只有输入和 clobber。

使用 `inline` 函数而不是宏（`#define outb(port, val) ...`）有几个好处：类型检查由编译器执行，参数求值顺序确定，调试时能看到函数名而不是一坨展开后的汇编。在 `-O1` 以上优化级别下，`inline` 函数会被内联掉，运行时开销和宏完全一样。

参考：Intel SDM Vol. 2A，IN 指令描述（约第 3-370 页），OUT 指令描述（约第 3-510 页）。`inb` 对应 `IN AL, imm8` 或 `IN AL, DX` 形式。

### 串口驱动——`serial.h`

头文件定义了 UART 硬件的寄存器布局和一个 `Serial` 类。

```cpp
#pragma once

#include <stdint.h>

#include "io.h"

namespace cinux::mini::serial {

constexpr uint16_t SERIAL_COM1 = 0x03F8;
constexpr uint16_t SERIAL_COM2 = 0x02F8;
constexpr uint16_t SERIAL_COM3 = 0x03E8;
constexpr uint16_t SERIAL_COM4 = 0x02E8;

namespace SerialReg {
constexpr uint8_t RBR = 0;  // Receive Buffer Register (read)
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
constexpr uint8_t RX_READY = 0x01;
constexpr uint8_t TX_READY = 0x20;
}  // namespace SerialLSR
```

PC 标准的四个 COM 端口基址是固定的：COM1 = 0x3F8，COM2 = 0x2F8，COM3 = 0x3E8，COM4 = 0x2E8。QEMU 默认把 COM1 重定向到 stdio（通过 `-serial stdio` 参数），所以我们只用 COM1。

UART 的寄存器通过基址加偏移访问。RBR 和 THR 共用偏移 0——读操作访问 RBR（接收缓冲），写操作访问 THR（发送保持），硬件根据 `in`/`out` 指令自动区分。LSR 的 bit 5（TX_READY）是发送侧最关键的状态位：当它为 1 时，表示 THR 已空，可以写入下一个字节。

这些常量全部用 `constexpr` 定义而不是 `#define`。`constexpr` 变量有类型、有作用域、可以被调试器识别，而且在编译期求值，运行时零开销。

```cpp
class Serial {
private:
    uint16_t base_port;

    bool is_tx_ready() const {
        return (io::inb(base_port + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
    }

    bool is_rx_ready() const {
        return (io::inb(base_port + SerialReg::LSR) & SerialLSR::RX_READY) != 0;
    }

public:
    explicit Serial(uint16_t port = SERIAL_COM1);

    void putc(char c);
    char getc();
    bool has_data() const { return is_rx_ready(); }
    void puts(const char* s);
    void init();
};

Serial& get_initial_serial();

}  // namespace cinux::mini::serial
```

`Serial` 类只有一个成员变量 `base_port`，所有操作都通过它加偏移来定位寄存器。`is_tx_ready()` 和 `is_rx_ready()` 是私有方法，分别检查 LSR 的 bit 5 和 bit 0——前者用于发送前等待缓冲区就绪，后者用于接收前检查是否有数据。

`get_initial_serial()` 返回一个全局单例的引用。这个单例通过全局构造函数在 `main` 之前初始化——后面看到 `serial.cpp` 的实现时你会发现它甚至会在构造函数的前后往 debugcon 输出标记字符，以便在调试日志中追踪初始化过程。

### 串口驱动——`serial.cpp`

现在来看实现。构造函数和初始化：

```cpp
#include "serial.h"
#include "driver/io.h"

namespace cinux::mini::serial {

Serial::Serial(uint16_t port) : base_port(port) {
    init();
}

void Serial::init() {
    io::outb(base_port + SerialReg::IER, 0x00);  // 禁用中断
    io::outb(base_port + SerialReg::LCR, 0x03);  // 8N1
    io::outb(base_port + SerialReg::FCR, 0xC7);  // 启用 FIFO
    io::outb(base_port + SerialReg::MCR, 0x03);  // RTS + DTR
    io::inb(base_port + SerialReg::LSR);          // 读 LSR 确认可访问
}
```

构造函数直接调用 `init()`。之前版本中散落在各处的 `outb %%al, $0xE9` debugcon 标记在代码稳定后被移除了——它们在开发调试阶段很有用（你可以在 `debug.log` 中看到类似 `\[1234'` 的序列来追踪初始化进度），但在最终代码中只是噪音。如果你在调试串口初始化问题，可以自行加回这些标记。

init 的四步对应 UART 的标准初始化序列。第一步禁用中断（IER = 0x00），因为我们用轮询模式发送数据，不依赖 UART 的中断机制。第二步设置线路参数（LCR = 0x03），即 8 位数据位、无奇偶校验、1 位停止位，通常简写为"8N1"——这是串口通信中最常用的配置。第三步启用 FIFO（FCR = 0xC7），0xC7 这个值的含义是：bit 7-6 = 11 表示设置 14 字节的 FIFO 触发阈值，bit 2 = 1 表示清空发送 FIFO，bit 1 = 1 表示清空接收 FIFO，bit 0 = 1 表示启用 FIFO。第四步设置调制解调器控制（MCR = 0x03），即 RTS（Request To Send）和 DTR（Data Terminal Ready）信号拉高——告诉对端"我准备好了，可以通信"。最后读一次 LSR 确认寄存器可访问。

你可能会问：为什么没设置波特率？物理 UART 需要通过 DLAB（Divisor Latch Access Bit）和除数寄存器来设置波特率，但 QEMU 的模拟 UART 默认就是 115200 bps，不需要手动配置。在真实硬件上，你需要在 LCR 中设置 DLAB 位（LCR bit 7 = 1），然后往偏移 0 和 1 写入除数值。但 Cinux 目前只针对 QEMU 开发，所以这一步可以省掉。

接下来是字符发送：

```cpp
void Serial::putc(char c) {
    uint32_t wait_count = 0;
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
        wait_count++;
        if (wait_count > 100000) {
            wait_count = 0;  // Timeout - serial port may be broken
        }
    }
    io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}
```

这是一个典型的忙等待（busy-wait）发送模式。循环查询 LSR 的 bit 5，直到 THR 空闲才写入。`__asm__ volatile("pause")` 是 x86 的 `PAUSE` 指令，在自旋锁循环中插入它有两个作用：一是提示 CPU 这是一个自旋等待，可以降低功耗；二是减少总线上的访存冲突。在超线程处理器上，`PAUSE` 能让出执行资源给另一个逻辑线程。实际的代码还加了一个 `wait_count` 超时计数器——当轮询超过 100000 次时重置计数。在 QEMU 中 THRE 位始终为 1，这个计数器永远不会触发，但在真实硬件上它提供了一层防御。

```cpp
char Serial::getc() {
    while (!is_rx_ready()) {
        __asm__ volatile("pause");
    }
    return static_cast<char>(io::inb(base_port + SerialReg::RBR));
}

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

`getc` 和 `putc` 是对称的忙等待实现，就不多说了。`puts` 里有一个非常容易踩的坑：`\n` 到 `\r\n` 的转换。Unix 系统用 `\n`（LF，0x0A）表示换行，但串口终端（以及很多 Windows 程序）期望 `\r\n`（CR+LF，0x0D 0x0A）。如果你不转换，QEMU 串口输出会变成"阶梯状"——每行的开头紧跟在上一行的末尾后面，而不是从新行的最左侧开始。所以每次遇到 `\n`，先发一个 `\r` 把光标挪到行首，再发 `\n` 把光标挪到下一行。

最后是全局单例：

```cpp
static Serial g_serial(SERIAL_COM1);

Serial& get_initial_serial() {
    return g_serial;
}

}  // namespace cinux::mini::serial
```

`g_serial` 是一个全局静态对象，它的构造函数会在 `main` 之前通过 `.init_array` 机制被调用——这意味着串口在小内核的 `mini_kernel_main` 执行之前就已经初始化完毕了。`get_initial_serial()` 返回这个全局对象的引用，其他模块（比如 `kprintf`）通过它来访问串口。

这里用全局构造函数来初始化串口是有意为之的设计决策。串口不依赖任何动态内存分配，不依赖其他全局对象，而且越早可用越好。如果改用"延迟初始化"（第一次调用时才 init），你就要处理并发问题和重入问题，复杂度不值得。唯一需要注意的是：如果 `.init_array` 中的函数指针是垃圾值（后面会讲到链接脚本的 LMA bug），`g_serial` 的构造函数根本不会被调用，然后 `kprintf` 第一次输出就会跳转到一个非法地址——你会收获一个完全无声的内核，debugcon 也帮不了你太多忙。

### 页表索引修正——`long_mode.S`

上面说的那个 bug，第一个就出在页表设置上。先来看修正后的代码：

```asm
    // ============================================================
    // Setup Higher-Half Kernel Mapping
    // ============================================================
    // We need to map 0xFFFFFFFF80000000 -> 0x00000000
    // The kernel is loaded at 0x20000, which is in the first 2MB (PD[0])
    //
    // For 0xFFFFFFFF80000000:
    //   - PML4[511] (bits 47:39 = 0x1FF = 511)
    //   - PDPT[510]  (bits 38:30 = 0x1FE = 510)  <- bit 30 = 0!
    //   - PD[0]      (bits 29:21 = 0x000 = 0)   <- maps to 0x00000000 physical
    //
    // IMPORTANT: x86-64 page table entries are 64 bits!
    // We must write both low 32 bits AND clear high 32 bits.
    // High 32 bits must be zero (except reserved bits) for valid entries.

    // PML4[511] -> PDPT (same as PML4[0])
    movl $PDPT_PHYS_ADDR, %eax
    orl $0x03, %eax
    movl %eax, PML4_PHYS_ADDR + (511 * 8)  // PML4[511] low 32 bits
    movl $0, PML4_PHYS_ADDR + (511 * 8) + 4  // PML4[511] high 32 bits = 0

    // PDPT[510] -> PD (NOT 511! 0x80000000 has bit 30 = 0)
    movl $PD_PHYS_ADDR, %eax
    orl $0x03, %eax
    movl %eax, PDPT_PHYS_ADDR + (510 * 8)  // PDPT[510] low 32 bits
    movl $0, PDPT_PHYS_ADDR + (510 * 8) + 4  // PDPT[510] high 32 bits = 0
```

这里修了两个问题。第一个是 PDPT 索引：之前的代码写的是 `PDPT[511]`，正确的应该是 `PDPT[510]`。我们重新拆解一下虚拟地址 `0xFFFFFFFF80000000` 的索引路径：

```
  虚拟地址: 0xFFFFFFFF80000000
  二进制:   1111...1111 10000000 00000000 00000000 00000000

  PML4 索引 (bits 47:39):  111111111 = 0x1FF = 511
  PDPT 索引 (bits 38:30):  100000000 = 0x1FE = 510
  PD   索引 (bits 29:21):  000000000 = 0x000 = 0
```

关键在 PDPT 索引：`0x80000000` 的 bit 30 是 0。bits 38:30 = `111111110` = 510。如果你顺手写了个 511（因为"511 看起来更像是最高位"），那 CPU 在翻译 `0xFFFFFFFF80000000` 这个地址时会查找 PDPT[511]，而我们只设了 PDPT[510]，PDPT[511] 是空的（全零，not present），于是触发 Page Fault。这个 bug 的狡猾之处在于 identity mapping 完全正常——用 0x20000 访问内核没问题，只有高半核地址会炸。如果你不主动测试高半核地址，可能很久都不会发现这个 bug。

第二个问题更隐蔽：x86-64 的页表项是 64 位的，但 `setup_page_tables` 运行在 32 位保护模式下，`movl` 只能写 32 位。之前只写了低 32 位，高 32 位保持为 `rep stosl` 清零后的 0——这恰好是正确的，因为我们的物理地址都低于 4GB。但问题在于，如果你在清零和设置之间有任何可能写入高 32 位的操作（比如设了一个超过 4GB 的地址），高 32 位就会带上垃圾。所以修正后的代码显式地把高 32 位清零：`movl $0, offset + 4`。这是一种"安全编程"的习惯——不依赖初始化时的零值，显式设置你期望的值。

### 启动阶段映射验证——`stage2.S`

为了不再被类似的页表 bug 偷袭，005 在跳转到内核之前新增了映射验证代码：

```asm
    // Test 1: Verify identity mapping to kernel
    movq $0x20000, %rax             // Identity address
    movb (%rax), %al                // Read first byte (should be 0xFA = 'cli')
    outb %al, $DEBUGCON_PORT        // Output: should be 0xFA

    // Test 2: Verify higher-half mapping
    movq $0xFFFFFFFF80020000, %rax  // Higher-half address
    movb (%rax), %al                // Read first byte
    outb %al, $DEBUGCON_PORT        // Output: should be 0xFA if mapping works

    // Test 3: Jump to kernel using higher-half mapping
    movb $0x48, %al                 // 'H' for Higher-half jump
    outb %al, $DEBUGCON_PORT

    movq $0x7000, %rdi              // First argument: BootInfo*
    movq $0xFFFFFFFF80020000, %rax  // Entry point: _start (higher-half)
    jmp *%rax
```

Test 1 读 identity mapping 地址 0x20000 的第一个字节。内核的 `_start` 入口是一条 `cli` 指令，对应的机器码是 0xFA，所以第一次 `outb` 应该在 debugcon 输出 0xFA。Test 2 读高半核地址 0xFFFFFFFF80020000 的同一个字节——如果页表映射正确，读到的也应该是 0xFA。Test 3 输出字符 'H' 作为"即将通过高半核地址跳转"的信号。

这样在 `debug.log` 中你会看到 `OPL`（之前的 Stage2/PM/LM 标记），然后是两个 0xFA 字节，接着是 'H'。如果只看到一个 0xFA，说明 identity mapping 正常但高半核映射有问题；如果连第一个 0xFA 都没有，说明内核本身没被正确加载到 0x20000。

### 链接脚本 LMA 修复——`linker.ld`

第二个致命 bug 出在链接脚本里。

```ld
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __mini_kernel_end = .;

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
```

关键改动在 `AT()` 表达式上。之前用的是 `AT(KERNEL_PHYS_BASE + SIZEOF(.text))` 这种累加方式来计算每个段的 LMA（Load Memory Address，即段在磁盘/内存映像中的物理地址）。问题在于 `SIZEOF()` 只返回段内容本身的大小，不包含段之间的对齐填充（alignment padding）。比如 `.text` 段可能在 VMA 0xFFFFFFFF80020000 到 0xFFFFFFFF80020FF0，大小为 0xFF0 字节，但因为 `.data` 需要 16 字节对齐，VMA 会跳到 0xFFFFFFFF80021000。此时 `SIZEOF(.text)` 还是 0xFF0，但 `.data` 的实际 LMA 应该是 0x21000 - 0xFFFFFFFF80000000 = 0x21000，而不是 0x20000 + 0xFF0 = 0x20FF0。

这种偏移偏差会导致什么后果？`.init_array` 段存放的是全局构造函数的指针，LMA 算错意味着 bootloader 加载的映像中，`.init_array` 指针的数据对不上——你读到的可能是 `.data` 段中间的某个位置，那里恰好是 0xFFFFFFFF（或者别的垃圾值）。`_init_global_ctors()` 尝试调用 0xFFFFFFFF 这个地址，直接 Page Fault。

修正后的公式是 `AT(ADDR(.section) - KERNEL_Virt_BASE)`。`ADDR(.section)` 返回段的实际 VMA（包含了所有对齐填充后的正确地址），减去 `KERNEL_Virt_BASE`（0xFFFFFFFF80000000）就得到正确的物理地址。无论中间有多少对齐填充，这个关系始终成立：LMA = VMA - KERNEL_Virt_BASE。

你可以用 `readelf -l build/kernel/mini/mini_kernel` 来验证。查看 LOAD 段的 VirtAddr 和 PhysAddr，确保 PhysAddr 是从 0x20000 开始的连续物理地址，而 VirtAddr 是从 0xFFFFFFFF80020000 开始的高半核地址。

### C++ 运行时存根——`crt_stub.cpp`

最后看一下 C++ 运行时支持的更新：

```cpp
extern "C" {

[[noreturn]] void __cxa_pure_virtual() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

[[noreturn]] void __stack_chk_fail() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    void (**start)() = __init_array_start;
    void (**end)() = __init_array_end;

    for (void (**func)() = start; func != end; func++) {
        void (*ctor)() = *func;
        if (ctor != nullptr) {
            ctor();
        }
    }
}

}  // extern "C"
```

`_init_global_ctors` 遍历 `.init_array` 段中的函数指针数组，逐个调用。这里有一个 005 修正的地方：之前的代码有一个 `hh_to_identity()` 辅助函数试图把高半核地址转换成 identity mapping 地址，但修正后不再需要了——因为内核现在运行在高半核地址空间，`__init_array_start/end` 本身就是正确的高半核地址，直接用就行。之前加 `hh_to_identity()` 是因为当时对"代码运行在哪个地址空间"还没有完全搞清楚。

数组遍历中的 `nullptr` 检查是一个防御措施。如果 `.init_array` 中有空洞（未被填充的条目），跳过它而不是尝试调用空指针。在正常情况下不应该出现空指针，但内核代码的防御性编程意识总比没有好。

`__cxa_pure_virtual` 是虚函数表中的纯虚函数占位符。如果有人在一个抽象类对象上调用纯虚函数（编程错误），就会走到这里。在标准环境中它会打印错误信息然后 `abort()`，但内核没有标准库，所以只能 `cli; hlt` 无限循环。`__stack_chk_fail` 类似，用于栈保护机制检测到栈溢出时调用。`__cxa_atexit` 返回 0 表示"注册成功"但不实际注册——内核没有进程退出的概念，析构函数永远不会被调用。

## 设计决策

### 决策：inline 函数 vs 宏封装 I/O 端口

**问题**：`inb`/`outb` 操作需要在编译时内联展开，否则函数调用开销会影响 I/O 时序。宏还是 inline 函数？

**本项目的做法**：用 `inline` 函数 + `__asm__ volatile`。

**备选方案**：`#define outb(port, val) __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port))`。很多 OS 教学项目和 OSDev Wiki 示例代码用的就是宏。

**为什么选 inline 函数**：类型安全（编译器会检查 `uint16_t` 和 `uint8_t`）、单次求值（宏的参数可能被求值多次，比如 `outb(port++, val)`）、作用域正确（宏不遵守命名空间）。在现代编译器（GCC/Clang `-O1` 以上）中，`inline` 函数和宏生成的代码完全一致。

**扩展方向**：如果将来需要支持 16 位和 32 位的 I/O 操作（`inw`/`outw`/`inl`/`outl`），可以用相同的模式扩展。也可以考虑加内存屏障语义（`mfence`）以确保 I/O 操作的顺序性。

### 决策：全局构造函数初始化串口 vs 手动初始化

**问题**：串口驱动应该在什么时候初始化？

**本项目的做法**：通过 `static Serial g_serial(SERIAL_COM1)` 全局对象在 `.init_array` 阶段自动初始化。

**备选方案**：在 `mini_kernel_main` 开头手动调用 `serial_init()`。Linux 的 `earlycon` 就是这种方式。

**为什么选全局构造函数**：串口是最底层的调试设施，越早可用越好。如果等 `mini_kernel_main` 才初始化，那从 `_start` 到 `mini_kernel_main` 之间的所有代码（包括 `_init_global_ctors` 本身）出了问题都无法通过串口诊断。全局构造函数的执行顺序由链接器控制——`serial.cpp` 的 `.init_array` 条目会被安排在 `mini_kernel_main` 的 `.init_array` 条目之前（因为 `g_serial` 在 `serial.cpp` 中定义，而 `mini_kernel_main` 不在 `.init_array` 中，它由 `boot.S` 直接触发）。实际上，`g_serial` 只依赖 I/O 端口操作（不依赖堆分配、不依赖其他全局对象），所以"初始化顺序"这个经典 C++ 问题在这里不存在。

**扩展方向**：当内核引入更多驱动后，可能需要显式控制初始化顺序。一个做法是用 priority 属性（`__attribute__((init_priority(N)))`）给 `.init_array` 条目标记优先级。

## 扩展方向

1. **支持中断驱动的串口收发**（难度：中等）——当前串口用的是忙等待轮询模式，CPU 时间全部浪费在 `pause` 循环上。当内核的 IDT 和中断控制器（8259A/APIC）就绪后，可以改为 UART 发送空中断触发发送，释放 CPU 时间。xv6 的 `uart.c` 就是中断驱动 + 轮询回退的模式。

2. **添加波特率配置支持**（难度：简单）——当前省略了 DLAB 分频器设置，因为 QEMU 默认 115200。如果要在真实硬件上运行，需要在 `init()` 中加入 DLAB 设置步骤：先设 LCR bit 7 = 1，然后写入除数（115200 / 目标波特率），最后清 LCR bit 7。

3. **环形缓冲区发送**（难度：中等）——即使在中断驱动模式下，如果发送速度跟不上生产速度（比如高频 `kprintf`），也需要一个环形缓冲区暂存待发送字符。Linux 的 `printk` 就用了环形缓冲区，并且支持在中断上下文中安全调用。

4. **多串口支持**（难度：简单）——当前硬编码 COM1。可以扩展为根据 BootInfo 或命令行参数选择不同的 COM 端口，或者同时往多个端口输出以提高调试信息可靠性。

5. **MMIO UART 支持（RISC-V / ARM 移植准备）**（难度：困难）——当前驱动基于 PMIO（x86 的 `in`/`out` 指令）。如果将来移植到其他架构，UART 会变成 MMIO 设备——读写普通内存地址而不是 I/O 端口。可以把 `io::inb`/`io::outb` 抽象成一个 `IoBackend` 接口，然后分别实现 PMIO 和 MMIO 后端。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 2A, Chapter 3 — IN/OUT 指令描述：`IN AL, imm8` / `IN AL, DX` 格式，8 位端口作为立即数或 DX 寄存器值。约第 3-370 页（IN）和第 3-510 页（OUT）。

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Chapter 4 — 4-Level Paging：PML4 -> PDPT -> PD -> PT 四级页表结构，2MB 大页的 PS bit 设置。`0xFFFFFFFF80000000` 的 PDPT 索引计算：bits 38:30 = 0x1FE = 510（bit 30 = 0）。

- OSDev Wiki — Serial Ports (https://wiki.osdev.org/Serial_Ports)：UART 寄存器映射、初始化序列、LSR 位定义。COM1 基地址 0x3F8，THR/LSR 寄存器偏移。

- xv6 RISC-V `uart.c` (https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/uart.c)：MIT 教学操作系统 的 UART 驱动实现，使用 MMIO（RISC-V 架构）而非 PMIO，但寄存器布局和初始化逻辑与 Cinux 高度相似。

- OSDev Wiki — Calling Global Constructors (https://wiki.osdev.org/Calling_Global_Constructors)：`.init_array` 段的工作机制，GCC 如何为全局对象生成构造函数条目。
