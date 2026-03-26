# 005 - Mini Kernel Entry - 串口输出与内核启动

> **本章目标**：实现 mini kernel 的入口函数，建立串口驱动和格式化输出功能，最终通过串口输出内核启动信息

**本章 git tag**：`005_mini_kernel_entry`，上一章 tag：`004_boot_load_mini_kernel_C`

---

## 1. 本章概览

到现在为止，我们已经完成了很多基础设施：bootloader 能够从磁盘加载 ELF 格式的 mini kernel，设置好页表，跳转到内核入口。但老实说，内核还什么都没做——之前的 entry point 只是一个简单的无限循环。这一章，我们要让内核真正"活过来"，能够输出信息告诉我们它的状态。

### 本章实现的关键功能

- **串口驱动**（Serial Port Driver）：实现基于 x86 I/O 端口的 UART 16550 驱动，支持轮询模式的字符发送和接收
- **格式化输出库**（kprintf）：实现类似 printf 的格式化输出函数，支持 %d、%u、%x、%X、%s、%p、%c 等格式符
- **内核主函数**（mini_kernel_main）：从启动信息中读取 BootInfo，输出内核版本和启动参数
- **测试基础设施**：建立独立的测试内核，用于验证 C++ 运行时和格式化输出功能

### 关键设计决策

- 采用轮询（polling）而非中断驱动的串口 I/O，简化早期内核实现
- 使用模板化的 `vkprintf_impl` 实现，支持不同的输出目标（串口、debugcon）
- 全局单例模式的串口实例，在静态构造阶段自动初始化
- 测试内核与生产内核共享完全相同的编译选项和链接配置

### 与同类 OS 的对比

相比 xv6 直接使用 console 输出，我们选择了更通用的串口驱动——这样既能在 QEMU 中调试，也为真实硬件调试做准备。相比 Linux 早期版本复杂的 console 层，我们的实现非常简化，专注于把数据送出去。

---

## 2. 架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Mini Kernel 启动流程                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  Bootloader (stage2)                                                │
│       │                                                             │
│       ├── Load ELF → Physical 0x20000                              │
│       ├── Setup Page Tables (identity + higher-half)                │
│       ├── Pass BootInfo* in %rdi                                    │
│       └── Jump to _start                                            │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    boot.S (Assembly Entry)                   │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │  1. cli (disable interrupts)                                 │    │
│  │  2. Setup stack (8KB at kernel end)                          │    │
│  │  3. Save BootInfo* → __boot_info_ptr                         │    │
│  │  4. Clear BSS                                                │    │
│  │  5. Call _init_global_ctors()                                │    │
│  │  6. Call mini_kernel_main(BootInfo*)                         │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                           │                                          │
│                           ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │              crt_stub.cpp (C++ Runtime)                      │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │  • _init_global_ctors() - 遍历 .init_array 调用构造函数     │    │
│  │  • __cxa_pure_virtual() - 纯虚函数调用处理                  │    │
│  │  • __stack_chk_fail() - 栈保护失败处理                       │    │
│  │  • operator new/delete - 动态内存桩函数（当前 halt）         │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                           │                                          │
│                           ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │              main.cpp (Kernel Entry Point)                   │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │  mini_kernel_main(BootInfo*)                                 │    │
│  │       │                                                        │    │
│  │       ├── kprintf("Cinux Mini Kernel v0.1.0\n")               │    │
│  │       └── Output boot info → Halt                             │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                           │                                          │
│                           ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                   lib/kprintf.cpp                            │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │  kprintf(fmt, ...)                                           │    │
│  │       │                                                        │    │
│  │       ├── va_start                                            │    │
│  │       ├── vkprintf_impl(putc, fmt, args)                     │    │
│  │       └── va_end                                              │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                           │                                          │
│                           ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                 driver/serial.cpp                            │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │  Serial::putc(c)                                             │    │
│  │       │                                                        │    │
│  │       ├── Wait for TX_READY (LSR bit 5)                      │    │
│  │       └── outb(THR, c)                                       │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                           │                                          │
│                           ▼                                          │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    Hardware (UART 16550)                     │    │
│  ├─────────────────────────────────────────────────────────────┤    │
│  │  COM1: Base 0x3F8                                            │    │
│  │  Registers: THR (0), IER (1), LCR (3), LSR (5)              │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 3. 关键代码精讲

### 3.1 I/O 端口操作基础

x86 架构提供了特殊的 `in` 和 `out` 指令用于与硬件设备通信，这些操作不通过内存，而是通过独立的 I/O 地址空间。串口（UART）就是通过这种方式控制的。

```cpp
// kernel/mini/driver/io.h
namespace cinux::mini::io {

inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

}
```

这里使用了 GCC 的内联汇编语法。`inb` 指令从指定端口读取一个字节到 `al` 寄存器（`"=a"` 约束），`outb` 指令将 `al` 寄存器的值写入指定端口。`Nd` 约束表示立即数或 `dx` 寄存器（这是 `in/out` 指令的要求）。`volatile` 关键字告诉编译器不要优化掉这条指令，因为硬件 I/O 有副作用。

### 3.2 串口硬件初始化

UART 16550 是经典的串口控制器，COM1 端口的基地址是 0x3F8。我们需要配置它工作在 115200 波特率、8 位数据位、无校验、1 位停止位（8N1）的模式——这是 QEMU 的默认配置，也是真实硬件的常用配置。

```cpp
// kernel/mini/driver/serial.cpp
void Serial::init() {
    // 禁用中断
    io::outb(base_port + SerialReg::IER, 0x00);

    // LCR = 0x03: 8 bits, no parity, 1 stop bit
    io::outb(base_port + SerialReg::LCR, 0x03);

    // 启用 FIFO，清除缓冲区，设置 14 字节阈值
    io::outb(base_port + SerialReg::FCR, 0xC7);

    // 设置 Modem 控制寄存器 (RTS + DTR)
    io::outb(base_port + SerialReg::MCR, 0x03);
}
```

这里我们依次配置四个关键寄存器：
- **IER**（Interrupt Enable Register）：设为 0 禁用所有中断，因为我们使用轮询模式
- **LCR**（Line Control Register）：设为 0x03 选择 8N1 格式
- **FCR**（FIFO Control Register）：设为 0xC7 启用 16 字节 FIFO，清除缓冲区，设置 14 字节触发阈值
- **MCR**（Modem Control Register）：设为 0x03 启用 RTS 和 DTR 信号

### 3.3 串口字符发送

发送字符到串口需要等待发送保持寄存器（THR）空闲，然后写入字符。我们通过检查 Line Status Register（LSR）的第 5 位来判断 THR 是否为空。

```cpp
// kernel/mini/driver/serial.cpp
void Serial::putc(char c) {
    // 等待发送缓冲区就绪
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }

    // 写入字符到 THR
    io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}

bool Serial::is_tx_ready() const {
    return (io::inb(base_port + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
}
```

轮询等待时使用 `pause` 指令是一个优化技巧——它告诉 CPU 我们在自旋等待，CPU 可以降低功耗或优化指令流水线。

### 3.4 格式化输出实现

kprintf 的核心是一个模板化的 `vkprintf_impl` 函数，它接受一个输出回调函数和格式化参数。这种设计让我们可以轻松切换不同的输出目标——串口、debugcon、未来的 framebuffer 控制台等。

```cpp
// kernel/mini/lib/kprintf.cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc, const char* format, va_list args) {
    char buffer[64];

    while (*format != '\0') {
        if (*format == '%') {
            format++;

            // 解析宽度和填充
            bool zero_pad = false;
            int width = 0;

            if (*format == '0') {
                zero_pad = true;
                format++;
            }

            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                format++;
            }

            char type = *format++;
            int len = 0;

            switch (type) {
            case '%':
                putc('%');
                break;
            case 'c':
                putc(static_cast<char>(va_arg(args, int)));
                break;
            case 's': {
                const char* s = va_arg(args, const char*);
                if (s == nullptr) s = "(null)";
                while (*s) putc(*s++);
                break;
            }
            case 'd':
                len = format_decimal(static_cast<int64_t>(va_arg(args, int)), buffer, sizeof(buffer));
                goto do_padding;
            case 'u':
                len = format_decimal(static_cast<int64_t>(va_arg(args, unsigned int)), buffer, sizeof(buffer));
                goto do_padding;
            case 'x':
                len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), true);
                goto do_padding;
            case 'X':
                len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
                goto do_padding;
            case 'p':
                for (const char* p = "0x"; *p; p++) putc(*p);
                len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
                for (int i = 0; i < len; i++) putc(buffer[i]);
                break;
            case 'b':
                len = format_binary(va_arg(args, uint64_t), buffer, sizeof(buffer));
                goto do_padding;

do_padding:
                if (len < width) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int i = width - len; i > 0; i--)
                        putc(pad);
                }
                for (int i = 0; i < len; i++)
                    putc(buffer[i]);
                break;

            default:
                putc('%');
                putc(type);
                break;
            }
        } else {
            putc(*format++);
        }
    }
}
```

这个实现有几个值得注意的设计点：

1. **格式化缓冲区**：我们使用栈上的 64 字节缓冲区来存储数字转换结果，避免了动态内存分配
2. **goto 的合理使用**：`do_padding` 标签让多个格式符共享填充逻辑，减少代码重复
3. **宽度和零填充**：支持 `%04d` 这样的格式，这对于输出对齐的十六进制地址很实用

### 3.5 数字转换函数

数字到字符串的转换由 `format_decimal`、`format_hex` 和 `format_binary` 实现。这些函数的手写实现避免了依赖标准库。

```cpp
// kernel/mini/lib/private/format.cpp
int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1)
        return 0;

    int idx = 0;
    bool is_neg = value < 0;

    // 特殊处理 INT64_MIN，因为 -INT64_MIN 会溢出
    if (is_neg) {
        if (value == INT64_MIN) {
            const char* min_str = "-9223372036854775808";
            int len = 0;
            while (min_str[len] != '\0' && idx < buffer_size - 1) {
                buffer[idx++] = min_str[len++];
            }
            buffer[idx] = '\0';
            return idx;
        }
        value = -value;
    }

    uint64_t abs_val = static_cast<uint64_t>(value);
    char tmp[24];
    int tmp_idx = 0;

    // 从低位到高位提取数字
    do {
        tmp[tmp_idx++] = '0' + (abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    // 添加负号
    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    // 反转数字
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

这里有个容易踩的坑——`INT64_MIN` 的绝对值无法用 `int64_t` 表示，所以需要特殊处理。否则 `-INT64_MIN` 会触发未定义行为（在补码表示下会溢出回 `INT64_MIN`）。

### 3.6 启动流程与 C++ 运行时

内核的真正入口是 `boot.S` 中的 `_start`，它在调用 C++ 代码之前需要完成几项关键的设置工作。

```asm
# kernel/mini/arch/x86_64/boot.S
_start:
    /* Disable interrupts */
    cli

    /* Output '1' to debugcon - _start reached */
    movb $0x31, %al              /* '1' */
    outb %al, $0xE9

    /* Setup stack - 8KB stack at end of kernel */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Output '2' to debugcon - stack ready */
    movb $0x32, %al              /* '2' */
    outb %al, $0xE9

    /* Save BootInfo pointer BEFORE clearing BSS (BSS clear uses %rdi) */
    movq %rdi, __boot_info_ptr

    /* Clear BSS section */
    movq $__bss_start, %rdi      /* destination */
    movq $__bss_end, %rcx        /* end */
    subq %rdi, %rcx              /* count = end - start */
    xorq %rax, %rax              /* value = 0 */
    rep stosb                    /* clear */

    /* Output '3' to debugcon - BSS cleared */
    movb $0x33, %al              /* '3' */
    outb %al, $0xE9

    /* Call global constructors (C++ runtime) */
    call _init_global_ctors

    /* Output '4' to debugcon - ctors done */
    movb $0x34, %al              /* '4' */
    outb %al, $0xE9

    /* Call C++ main */
    movq __boot_info_ptr, %rdi   /* first argument: BootInfo* */
    call mini_kernel_main

    /* Halt if kernel returns */
.halt:
    cli
    hlt
    jmp .halt
```

这里有几个重要的细节：

1. **BootInfo 保存**：`%rdi` 寄存器包含 bootloader 传递的 BootInfo 指针（System V AMD64 ABI 的第一个整数参数）。我们需要在清零 BSS 之前保存它，因为清零操作会使用 `%rdi` 寄存器。

2. **BSS 清零**：`rep stosb` 指令重复存储 `al` 的值到 `[rdi]`，`rcx` 次。这会清零整个 BSS 段。

3. **全局构造函数调用**：C++ 的全局对象构造函数需要在使用前调用。这些函数指针由编译器放入 `.init_array` 段。

4. **Debugcon 输出**：向端口 0xE9 写入字符可以输出到 QEMU 的 debugcon（用 `-debugcon` 选项启用）。这对于调试早期启动问题非常有用，因为串口可能还没初始化。

### 3.7 C++ 运行时支持

由于我们使用 `-nostdlib` 和 `-ffreestanding` 编译选项，需要提供一些 C++ 运行时函数的实现。

```cpp
// kernel/mini/arch/x86_64/crt_stub.cpp
extern "C" {

// 纯虚函数调用处理
[[noreturn]] void __cxa_pure_virtual() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// 栈保护失败处理
[[noreturn]] void __stack_chk_fail() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// Atexit 处理（最小实现）
int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

// 全局构造函数初始化
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    for (void (**func)() = __init_array_start; func != __init_array_end; func++) {
        (*func)();
    }
}

}

// operator new/delete（桩函数）
void operator delete(void* ptr) noexcept {
    (void)ptr;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这些函数在 freestanding 环境中通常不需要完整实现。对于纯虚函数调用和 new/delete，我们直接停机——这些在内核开发中通常是编程错误，需要尽早发现。

全局构造函数的遍历是唯一真正需要的实现——没有它，我们的全局 `Serial` 实例就不会被初始化，串口输出就不会工作。

### 3.8 内核主函数

终于到了内核的真正入口点。这里我们输出一些启动信息，证明内核已经成功启动。

```cpp
// kernel/mini/main.cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n",
            boot_info->entry_point, boot_info->kernel_phys_base);

    // TODO: Initialize kernel subsystems
    // TODO: Start scheduler

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这里我们使用 `%p` 格式符输出指针，它会自动添加 `0x` 前缀并使用大写十六进制。`entry_point` 应该是 `0xFFFFFFFF80020000`（higher-half 内核入口），`kernel_phys_base` 应该是 `0x20000`（物理加载地址）。

---

## 4. 设计决策深度分析

### 决策一：串口驱动的轮询模式 vs 中断模式

**问题**：串口驱动应该使用轮询（polling）还是中断驱动的 I/O？

**本项目的做法**：采用轮询模式，在 `putc` 中自旋等待 `TX_READY` 标志。

**备选方案**：使用中断驱动模式，设置 UART 发送中断，在中断处理程序中填充发送队列。

**为什么不选中断模式**：

1. **简化早期开发**：在内核开发的早期阶段，我们还没有完整的中断框架（IDT 设置、中断处理程序、中断栈等）。轮询模式不依赖这些基础设施，可以快速实现基本功能。

2. **调试便利性**：轮询模式的串口输出可以在中断系统工作之前就使用，这对于调试早期启动问题非常关键。如果串口本身依赖中断，那就陷入了一个先有鸡还是先有蛋的问题。

3. **性能不是问题**：对于启动日志输出，串口的带宽（115200 bps ≈ 14 KB/s）远低于我们的输出需求，轮询带来的性能损失可以忽略。

**如果要扩展**：当内核建立完整的中断框架后，可以重构串口驱动为中断模式。建议的实现方式是：
- 保留轮询模式的 `putc` 用于早期调试
- 实现基于环形缓冲区的中断驱动输出
- 提供配置选项在两种模式间切换

### 决策二：全局单例串口实例 vs 显式传递

**问题**：串口实例应该作为全局单例，还是显式传递给需要它的代码？

**本项目的做法**：使用全局单例 `get_initial_serial()`，串口在静态构造阶段自动初始化。

**备选方案**：
- 使用依赖注入，每个需要输出的函数接收 `Serial&` 参数
- 使用函数局部静态变量（懒加载单例）
- 使用纯 C 风格的函数接口，隐藏类实现

**为什么不选显式传递**：

1. **简化调用链**：`kprintf("...")` 比起 `kprintf(serial, "...")` 更简洁，更符合标准 printf 的习惯。
2. **单一输出目标**：在当前阶段，我们只需要一个串口输出，不存在多个输出目标切换的需求。
3. **避免循环依赖**：显式传递需要在很多函数签名中添加 `Serial&` 参数，这会导致修改传播。

**全局单例的注意事项**：
- 确保在第一次使用前构造函数已被调用（通过 `.init_array` 保证）
- 避免在构造函数中使用其他全局对象（构建顺序问题）

**如果要扩展**：当需要支持多个输出目标时，可以重构为：
- 保持 `kprintf` 使用默认目标
- 提供 `vkprintf(output_fn, ...)` 用于自定义输出
- 实现输出流的抽象类（类似 C++ 的 ostream 概念）

### 决策三：格式化输出的自实现 vs 使用现有库

**问题**：格式化输出应该自己实现，还是移植现有的 printf 实现？

**本项目的做法**：自实现简化版 kprintf，支持常用的格式符。

**备选方案**：
- 移植 FreeBSD 的 `printf` 实现
- 使用 TinyPrintf 等轻量级实现
- 完全依赖编译器的内置函数

**为什么不选现有库**：

1. **学习价值**：实现 printf 是理解格式化字符串、可变参数、数字转换的好练习。
2. **精简依赖**：自实现可以精确控制代码大小和功能范围，避免引入不需要的功能。
3. **freestanding 兼容**：某些现有实现依赖标准库函数，需要额外移植工作。

**自实现的代价**：
- 功能不完整（不支持浮点、宽字符等）
- 可能存在边界情况未处理
- 需要充分测试确保正确性

**如果要扩展**：当前实现已经支持整数类型的常用格式。如需支持浮点，可以：
- 移植现有的 `vdprintf` 实现（较复杂）
- 使用简化的定点浮点输出（适合内核调试）
- 直接使用十六进制输出位模式（临时方案）

---

## 5. 调试排查指南

内核开发中遇到问题时，系统的调试方法能帮你快速定位原因。本节介绍两个常见的启动失败问题和对应的排查流程。

### 5.1 页表 PDPT 索引计算错误

**症状**：

- 输出停留在 bootloader 的 `OPL` 后，没有后续的内核输出 `1234`
- 使用 identity mapping (0x20000) 可以正常执行
- 使用 higher-half mapping (0xFFFFFFFF80000000) 时发生页面错误

**根本原因**：

在 x86-64 分页机制中，虚拟地址 `0xFFFFFFFF80000000` 的索引计算容易出错：

```
虚拟地址: 0xFFFFFFFF80000000
二进制:   1111111111111111111111111111111110000000000000000000000000000000
          ^^^^^^^^^ bits 47:39 (PML4 索引)
                     ^^^^^^^^ bits 38:30 (PDPT 索引)
```

正确计算：
- **PML4 索引** (bits 47:39) = `0x1FF` = **511** ✓
- **PDPT 索引** (bits 38:30) = `0x1FE` = **510** ← 这里容易写错！
- **PD 索引** (bits 29:21) = `0x000` = **0** ✓

如果错误地使用 PDPT[511]，CPU 会查找一个不存在的页表项（not present），触发页面错误。

**为什么是 510？**

`0x80000000` 的 bit 30 = 0，所以：
```
bits 38:30 = 0b111111110 = 0x1FE = 510
```

**验证方法**：

添加调试代码输出页表项：

```assembly
// 在进入 long mode后，读取并输出页表项
movq %cr3, %rax                 // 获取 PML4 地址
movq 0xFF8(%rax), %rbx          // 读取 PML4[511]
// 输出 %rbx，应该是 0x0000000000002003

movq $0x2000, %rax              // PDPT 地址
movq 0xFF8(%rax), %rbx          // 读取 PDPT[511] - 应该是 0！
movq 0xFF0(%rax), %rbx          // 读取 PDPT[510] - 应该是 0x3003
```

**预防措施**：

1. **使用宏定义**：定义清晰的页表索引常量

```assembly
// x86-64 higher-half 页表索引
.set HH_PML4_INDEX,    511  // 0xFFFFFFFF80000000 的 PML4 索引
.set HH_PDPT_INDEX,    510  // ← 注意：是 510，不是 511！
.set HH_PD_INDEX,      0    // PD 索引
```

### 5.2 链接器脚本 LMA 计算错误

**症状**：

- `__init_array` 中的构造函数指针是 `0xffffffff` (垃圾值)
- 调用全局构造函数时崩溃
- 数据段内容不正确

**根本原因**：

使用 `SIZEOF()` 累加计算 LMA 时，**没有考虑段间对齐填充**。

#### 错误的链接器脚本

```ld
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    .text : AT(KERNEL_PHYS_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) {  // ← 错误！
        *(.data .data.*)
    }

    .init_array : AT(KERNEL_PHYS_BASE + SIZEOF(.text) + SIZEOF(.data)) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }
}
```

#### 问题分析

```
内存布局（VMA）:
0xFFFFFFFF80020000  .text 开始
0xFFFFFFFF80020xxx  .text 结束（可能有对齐填充）
0xFFFFFFFF80021xxx  .data 开始  ← 对齐填充导致偏移！
0xFFFFFFFF80021yyy  .data 结束
0xFFFFFFFF80021zzz  .init_array 开始

SIZEOF(.text) 只返回 .text 段内容的大小，不包含对齐填充
所以 KERNEL_PHYS_BASE + SIZEOF(.text) 计算出的 LMA 位置错误！
```

#### 正确的链接器脚本

```ld
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
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
}
```

**为什么 `ADDR() - KERNEL_Virt_BASE` 有效？**

```
LMA = VMA - KERNEL_Virt_BASE

对于任何地址：
- VMA = 0xFFFFFFFF80020000
- LMA = 0xFFFFFFFF80020000 - 0xFFFFFFFF80000000 = 0x20000

无论段间有多少对齐填充，这个关系始终成立！
```

**验证方法**：

使用 `readelf` 检查段布局：

```bash
# 查看 ELF 段的 LMA (Load Address)
readelf -l build/kernel/mini/mini_kernel.elf | grep -A 20 "Program Headers"

# 输出示例：
# LOAD           0x0000000000002000 0xffffffff80020000 0xffffffff80020000
#                0x0000000000001000 0x0000000000001000  R E
```

关键检查项：
- **VMA** (虚拟地址) 应该是 higher-half 地址
- **LMA** (加载地址) 应该是物理地址（0x20000 起始）
- **File Size** 和 **Mem Size** 应该匹配（对于非 .bss 段）

### 5.3 故障排查流程图

```
内核启动失败
    │
    ├─> 输出停留在 "O" 之前
    │   └─> 检查磁盘加载、MBR 执行
    │
    ├─> 输出停留在 "OP" 之间
    │   └─> 检查保护模式切换、GDT 加载
    │
    ├─> 输出停留在 "PL" 之间
    │   └─> 检查 long mode 切换、PAE 启用
    │
    ├─> 输出有 "L" 但没有后续
    │   └─> 检查页表设置
    │       │
    │       ├─> 验证 PML4[511] = 0x2003
    │       ├─> 验证 PDPT[510] = 0x3003  ← 注意是 510！
    │       └─> 验证 PD[0] = 0x83
    │
    ├─> 输出有 "1" 但没有 "2"
    │   └─> 检查栈指针设置
    │
    ├─> 输出有 "2" 但没有 "3"
    │   └─> 检查 BSS 清零
    │
    ├─> 输出有 "3" 但没有 "4"
    │   └─> 检查 __init_array
    │       │
    │       ├─> 验证 __init_array_start < __init_array_end
    │       ├─> 验证构造函数指针有效（不是 0 或 0xffffffff）
    │       └─> 检查链接器脚本 LMA 计算
    │
    └─> 输出有 "4" 但没有内核输出
        └─> 检查 mini_kernel_main 入口
```

### 5.4 调试工具和方法

#### 使用 QEMU debugcon

最简单的调试方法：

```bash
qemu-system-x86_64 -drive file=cinux.img,format=raw -debugcon file:debug.log
```

在代码中添加调试输出：

```assembly
// 输出单个字符
movb $0x41, %al    // 'A'
outb %al, $0xE9    // 写入 debugcon 端口
```

```cpp
// C/C++ 中使用
__asm__ volatile("movb $0x42, %%al; outb %%al, $0xE9" ::: "eax");  // 'B'
```

#### GDB 调试

```bash
# 启动 QEMU with GDB server
qemu-system-x86_64 -drive file=cinux.img,format=raw -s -S

# 在另一个终端连接 GDB
gdb build/kernel/mini/mini_kernel.elf
(gdb) target remote :1234
(gdb) break _start
(gdb) continue
```

#### 检查符号表

```bash
# 查看内核符号地址
nm build/kernel/mini/mini_kernel.elf | grep -E "__init_array|_start"

# 预期输出：
# ffffffff80020000 T _start
# ffffffff80020xxx A __init_array_start
# ffffffff80020xxx A __init_array_end
```

---

## 6. 常见变体与扩展方向

1. **添加颜色输出支持** ⭐
   - 在 ANSI 转义序列前添加前缀（如 `\033[31m` 红色）
   - 实现简单的 `kprintf_red()` 等辅助函数
   - 注意：需要终端支持 ANSI，串口工具需要配置

2. **实现环形缓冲区输出** ⭐⭐
   - 使用固定大小的循环队列缓存输出字符
   - 在中断处理程序中异步发送缓冲区内容
   - 可以提高系统响应性，减少 CPU 自旋等待

3. **支持多串口输出** ⭐⭐
   - 扩展 Serial 类支持 COM1-COM4
   - 实现日志级别路由（错误→COM1，调试→COM2）
   - 添加串口选择 API

4. **实现完整的 printf 功能** ⭐⭐⭐
   - 添加浮点格式符 `%f`、`%e`、`%g`
   - 支持宽度和精度的小数部分
   - 实现位置参数（`%1$d`）

5. **添加日志框架** ⭐⭐
   - 实现日志级别（DEBUG、INFO、WARN、ERROR）
   - 支持模块化的日志源标识
   - 可配置的运行时日志过滤

---

## 6. 参考资料

### Intel/AMD 手册
- Intel SDM Vol. 3: Chapter 10 - I/O Ports
- Intel SDM Vol. 3: Chapter 17 - ISA Compatibility
- Intel SDM Vol. 4: Appendix H - UART 16550

### OSDev Wiki
- Serial Ports: https://wiki.osdev.org/Serial_Ports
- Printf: https://wiki.osdev.org/Printing_to_Serial
- Inline Assembly: https://wiki.osdev.org/Inline_Assembly/Examples

### 其他资源
- 16550 UART Datasheet (National Semiconductor)
- "The Art of Writing Portable Libraries" - printf 实现参考
- QEMU Documentation - `-serial` 和 `-debugcon` 选项

---

## 验证步骤

要验证本章实现，可以运行以下命令：

```bash
# 构建内核
cmake -B build -DCINUX_BUILD_TESTS=ON
cmake --build build

# 运行生产内核
qemu-system-x86_64 \
    -drive format=raw,file=build/image/cinux.img \
    -serial stdio

# 运行测试内核（需要使用测试构建目标）
qemu-system-x86_64 \
    -drive format=raw,file=build/image/cinux_test.img \
    -serial stdio \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
```

预期输出：
```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xFFFFFFFF80020000, kernel_phys_base=0x20000
```

对于测试内核，应该看到完整的格式化测试输出和 C++ 运行时测试结果。
