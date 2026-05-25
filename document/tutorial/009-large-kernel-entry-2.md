---
title: 009-large-kernel-entry-2 · 大内核入口
---

# 009-2 Tutorial: 大内核的嘴巴 — I/O 端口、串口驱动与 kprintf

> 标签：x86 I/O port, UART 16550, serial driver, kprintf, printf, kernel output
> 前置：[009-1 大内核启动](./009-large-kernel-entry-1.md)

## 前言

上一章我们让大内核"站起来了"——从链接脚本到 boot.S 到 C++ 入口点，整个启动链条完整了。但说实话，一个只会 `kprintf("[BIG] Big kernel running...")` 的内核，和只会说一句话的鹦鹉没什么区别。这一章我们要给大内核装上完整的"嘴巴"——从最底层的 I/O 端口原语，到中间层的串口驱动，再到最上层的 kprintf 格式化引擎。这三个层次的抽象构成了内核输出的完整基础设施，之后所有的调试信息、panic 消息、驱动诊断都依赖它。

你可能会想：不就是往串口写字节嘛，至于拆三层？至于。因为教学 OS 的一大教训就是——输出基础设施一旦没打好，后续所有驱动的调试都会变成噩梦。没有可靠的 printf，你连一个 bug 都没法定位。

## 环境说明

QEMU 虚拟机带 `-serial stdio` 参数，串口输出直接显示在终端。UART 16550 虚拟硬件默认 115200 波特率、8N1 格式。大内核运行在 identity mapping 下，I/O 端口通过 `in/out` 指令直接访问。

## 第一步——I/O 端口原语：和硬件打交道的最底层

x86 架构有两条独立的地址空间——内存空间和 I/O 空间。内存空间通过 `mov` 等指令访问，I/O 空间通过 `in` 和 `out` 指令访问。串口控制器的寄存器就在 I/O 空间里——COM1 的基地址是 `0x3F8`，各寄存器从基地址依次偏移 0-7。Intel SDM Vol.2A 里有 `IN` 和 `OUT` 指令的完整描述，简单来说就是：`IN` 从端口读数据到 AL/AX/EAX，`OUT` 把 AL/AX/EAX 的数据写到端口。

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
```

每个函数都是 `inline` 的——GCC 内联汇编的薄包装。`"=a"(value)` 把 AL 的值取出来，`"Nd"(port)` 把端口号放到 DX 寄存器。`volatile` 防止编译器优化掉"看似无用"的 I/O 操作，`"memory"` clobber 充当编译器屏障——I/O 操作可能改变设备状态，不允许编译器把前后的内存访问重排到 I/O 操作的另一侧。

`io_inw/io_outw`（16位）和 `io_inl/io_outl`（32位）是同样的模式，只是把 `inb/outb` 换成 `inw/outw` 和 `inl/outl`。`io_wait()` 往端口 0x80（POST 诊断端口）写一个零字节，在真实硬件上产生约 1 微秒延迟——这对某些 ISA 设备的时序要求是必要的。

你会发现 xv6 的 I/O 端口封装几乎一模一样——也是一个 `inb` 一个 `outb`，只是 xv6 把它们放在 `x86.h` 里用 C 的 `static inline`，我们用 C++ 的 `namespace` + `inline`。Linux 的实现更复杂——它区分了 `inb/outb`（指令级）和 `ioread8/iowrite8`（MMIO 抽象），因为 Linux 需要同时支持 I/O 端口映射和内存映射 I/O。SerenityOS 也有类似的分层：`IOAddress` 类封装端口地址，`MMIOAddress` 类封装内存映射地址，统一接口但底层实现不同。

## 第二步——串口驱动：UART 16550 的轮询模式封装

有了 I/O 端口原语，接下来就是封装 UART 16550 串口控制器了。PC 兼容机上最常见的串口芯片就是它——8 个寄存器通过 I/O 端口偏移 0-7 访问，我们最关心的是 THR（Transmit Holding Register，偏移 0，写入字节发送）、LCR（Line Control Register，偏移 3，配置数据格式）、LSR（Line Status Register，偏移 5，查询发送状态）。OSDev Wiki 的 Serial Ports 页面有完整的寄存器映射表。

```cpp
namespace cinux::drivers {

constexpr uint16_t SERIAL_COM1 = 0x03F8;

namespace SerialReg {
constexpr uint8_t THR = 0;
constexpr uint8_t IER = 1;
constexpr uint8_t FCR = 2;
constexpr uint8_t LCR = 3;
constexpr uint8_t MCR = 4;
constexpr uint8_t LSR = 5;
}

namespace SerialLSR {
constexpr uint8_t TX_READY  = 0x20;
}

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
```

类设计很简洁——构造时存端口地址，`init()` 配置硬件，`putc()` 发一个字节，`puts()` 发一个字符串。`base_port_` 是唯一的成员变量。用 `constexpr` 命名空间代替 `#define` 宏——这是现代 C++ 的做法，有类型安全、有作用域、调试器能看到名字。

```cpp
void Serial::init(uint16_t /*port*/, uint32_t /*baud*/) {
    io_outb(base_port_ + SerialReg::IER, 0x00);  // 禁用中断
    io_outb(base_port_ + SerialReg::LCR, 0x03);  // 8N1
    io_outb(base_port_ + SerialReg::FCR, 0xC7);  // 启用 FIFO，清空缓冲
    io_outb(base_port_ + SerialReg::MCR, 0x03);  // DTR + RTS
    io_inb(base_port_ + SerialReg::LSR);          // 空读清除状态
}
```

初始化序列是 UART 16550 的标准配置。IER=0 禁用所有中断——我们用轮询模式不需要中断。LCR=0x03 设置 8 个数据位、无校验、1 个停止位。FCR=0xC7 启用 FIFO 并清空缓冲。MCR=0x03 设置 DTR 和 RTS 信号。QEMU 的虚拟 UART 默认就是 115200，不需要编程除数锁存器。

```cpp
void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }
    io_outb(base_port_ + SerialReg::THR, static_cast<uint8_t>(c));
}

void Serial::puts(const char* s) {
    if (s == nullptr) return;
    while (*s != '\0') {
        if (*s == '\n') putc('\r');
        putc(*s++);
    }
}
```

`putc` 自旋等待 THR 空（`LSR & TX_READY` 非 0），然后写入字节。`pause` 指令是 spin-wait 的性能提示。`puts` 遍历字符串逐字符发送，遇到 `\n` 先发 `\r`——串口终端需要 `\r\n` 才能正确换行回车。

Linux 的串口驱动（8250_core.c）比我们的复杂了几个数量级——支持中断驱动收发、FIFO 深度自动检测、modem 状态监控、多种 UART 芯片兼容。xv6 的 console.c 也很简洁，和我们类似——轮询模式、自旋等待、`\n` → `\r\n` 转换。SerenityOS 的 SerialDevice 类支持中断驱动模式，可以非阻塞地读写串口数据。

## 第三步——kprintf 格式化引擎：内核的 printf

有了串口驱动，最后一步是格式化输出引擎。这就是内核版的 printf——支持 `%d %u %x %X %s %p %c %%`，有宽度修饰符和零填充。

```cpp
static int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == (-9223372036854775807LL - 1)) {
            const char* min_str = "-9223372036854775808";
            // ... 直接拷贝硬编码字符串
        }
        value = -value;
    }

    uint64_t abs_val = static_cast<uint64_t>(value);
    char     tmp[24];
    int      tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + static_cast<char>(abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    if (is_neg && idx < buffer_size - 1) buffer[idx++] = '-';
    while (tmp_idx > 0 && idx < buffer_size - 1) buffer[idx++] = tmp[--tmp_idx];
    buffer[idx] = '\0';
    return idx;
}
```

十进制格式化有一个必须处理的特殊情况——INT64_MIN。因为 `-INT64_MIN` 会溢出（补码的不对称性：`|INT64_MIN| > INT64_MAX`），直接取负是未定义行为。硬编码字符串是最简单的正确解法。其他负数先取绝对值再格式化。

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];

    while (*fmt != '\0') {
        if (*fmt != '%') { putc_fn(*fmt++); continue; }
        fmt++;

        bool zero_pad = false;
        int  width    = 0;
        if (*fmt == '0') { zero_pad = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char type = *fmt++;
        int  len  = 0;

        switch (type) {
        case '%': putc_fn('%'); break;
        case 'c': putc_fn(static_cast<char>(va_arg(args, int))); break;
        case 's': {
            const char* s = va_arg(args, const char*);
            if (s == nullptr) s = "(null)";
            while (*s) putc_fn(*s++);
            break;
        }
        case 'd':
            len = format_decimal(static_cast<int64_t>(va_arg(args, int)),
                                 buffer, sizeof(buffer));
            goto do_padding;
        case 'x':
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), true);
            goto do_padding;
        case 'p':
            putc_fn('0'); putc_fn('x');
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            for (int i = len; i < 16; i++) putc_fn('0');
            for (int i = 0; i < len; i++) putc_fn(buffer[i]);
            break;

        do_padding:
            if (len < width) {
                char pad = zero_pad ? '0' : ' ';
                for (int i = width - len; i > 0; i--) putc_fn(pad);
            }
            for (int i = 0; i < len; i++) putc_fn(buffer[i]);
            break;
        }
    }
}
```

这是格式化引擎的核心——一个状态机逐字符扫描格式字符串。非 `%` 字符直接输出，遇到 `%` 后解析零填充标志和宽度，然后按类型分发。`%p` 输出 "0x" + 16 位十六进制（64 位地址的完整表示），`do_padding` 标签通过 `goto` 从多个 case 共享——在这里 goto 是合理的，避免了代码重复。

模板参数 `OutputFn` 是一个泛型字符输出函数——lambda、函数指针都行。这样同一个格式化引擎可以在内核中输出到串口，在测试中输出到 mock buffer，不需要写两套代码。

```cpp
static Serial g_serial(SERIAL_COM1);

void kprintf_init() { g_serial.init(); }

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}

[[noreturn]] void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
    while (1) { __asm__ volatile("cli; hlt"); }
}
```

公开接口通过 lambda 捕获 `g_serial.putc` 传给模板引擎。`kpanic` 带 `[[noreturn]]` 属性——格式化输出后直接 halt，这是内核的"不可恢复错误"处理。

xv6 的 printf.c 和我们的结构很像——也是逐字符解析格式字符串，也是分别处理 `%d/%x/%s/%c`。但 xv6 用的是函数指针而非模板，因为 xv6 是 C 代码。Linux 的 printk 是另一个量级的东西——支持日志级别（KERN_INFO、KERN_WARN、KERN_ERR）、环形缓冲区、dmesg、速率限制、多 CPU 并发安全。SerenityOS 也有类似的分层——基础的 `putchar` 到格式化的 `printf` 再到带级别的 `dbgln`。

## 收尾

到这里大内核的输出基础设施就完整了——三层抽象栈从 `in/out` 指令到 kprintf 格式化输出。现在我们可以在内核代码的任何位置用 `kprintf` 打印调试信息，用 `kpanic` 报告致命错误。下一章我们要处理一个不那么光鲜但极其重要的问题——ELF 加载器在真实场景下的 bug 修复和压力测试。

## 参考资料

- Intel SDM: Vol.2A — `IN`/`OUT` 指令参考
- OSDev Wiki: [Serial Ports](https://wiki.osdev.org/Serial_Ports) — UART 16550 寄存器
- OSDev Wiki: [Kernel Printf](https://wiki.osdev.org/Printing_To_Screen)
- Linux: [8250 serial driver](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/8250/8250_core.c)
- xv6: [printf.c](https://github.com/mit-pdos/xv6-public/blob/master/printf.c)
- SerenityOS: [SerialDevice](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/ISABus/Serial16550.cpp)
