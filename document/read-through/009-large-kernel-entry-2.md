# 009-2 Read-through: I/O 端口原语 + UART 串口驱动 + kprintf 格式化引擎

## 概览

本文是 tag 009 的第二篇 read-through，聚焦于大内核的输出基础设施。在上一篇文章里 boot.S 把 CPU 从 mini kernel 手里接过来后，我们进入了 C++ 世界，但内核还是个哑巴——没有输出能力。本文讲解的三个组件构成了一个三层抽象栈：最底层是 I/O 端口原语（直接操作硬件寄存器），中间层是串口驱动（UART 16550 的轮询模式封装），最上层是 kprintf 格式化引擎（printf 风格的内核输出接口）。

## 架构图

```
kernel_main()
    ↓
cinux::lib::kprintf("[BIG] ...")
    ↓
vkprintf_impl(lambda: g_serial.putc)  ← 格式化引擎，OutputFn 泛型
    ↓
cinux::drivers::Serial::putc(char)    ← 轮询等待 THR 空
    ↓
cinux::io::io_outb(port, value)       ← 内联汇编 outb 指令
    ↓
CPU → UART 16550 → COM1 → QEMU 串口 → stdio

数据流：
  kprintf("%p", addr)
    → format_hex(addr, ...) → buffer "1000000"
    → pad to 16 digits → "0000000001000000"
    → "0x" prefix → "0x0000000001000000"
    → g_serial.putc('0') → io_outb(0x3F8, '0')
```

## 代码精讲

### I/O 端口原语 kernel/arch/x86_64/io.hpp

这是大内核和硬件打交道最底层的接口，封装了 x86 的 `in/out` 指令。

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

每个函数都是一个薄薄的 inline 包装。`io_inb` 用 `"=a"(value)` 把 `%al` 的值取出来作为返回值，`"Nd"(port)` 把端口号放到 `%dx` 寄存器。AT&T 语法中 `inb %1, %0` 就是 `inb %dx, %al`——从端口读一个字节到 AL。`io_outb` 反过来，`outb %0, %1` 就是 `outb %al, %dx`——把 AL 的值写到端口。

所有函数都标记了 `volatile` 防止编译器优化掉"看似无用"的 I/O 操作，也加了 `"memory"` clobber 充当编译器屏障——I/O 操作可能影响设备状态（从而影响后续的内存映射 I/O），不允许编译器把 I/O 前后的内存访问重排。

同一模式重复三次，分别对应 byte/word/dword 宽度（`inb/outb`、`inw/outw`、`inl/outl`），数据类型对应 `uint8_t/uint16_t/uint32_t`。

```cpp
inline void io_wait() {
    io_outb(0x80, 0);
}

}  // namespace cinux::io
```

`io_wait` 写端口 0x80（POST 诊断端口）一个零字节。这个操作在真实硬件上大约需要 1 微秒，足以满足一些 ISA 设备的时序要求。QEMU 里基本是空操作，但保留它是好习惯。

### UART 16550 串口驱动 kernel/drivers/serial.hpp + serial.cpp

串口驱动分为头文件（类定义和常量）和实现文件。

**头文件 — 常量和类声明**

```cpp
namespace cinux::drivers {

constexpr uint16_t SERIAL_COM1 = 0x03F8;
constexpr uint16_t SERIAL_COM2 = 0x02F8;
constexpr uint16_t SERIAL_COM3 = 0x03E8;
constexpr uint16_t SERIAL_COM4 = 0x2E8;
```

四个标准 COM 端口的基地址，PC 兼容机的固定分配。

```cpp
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
}

namespace SerialLSR {
constexpr uint8_t RX_READY  = 0x01;
constexpr uint8_t TX_READY  = 0x20;
}
```

UART 寄存器偏移和 LSR 位掩码用 `constexpr` 定义在命名空间中。RBR 和 THR 共享偏移 0 但方向不同（读/写）。TX_READY 是 bit 5（0x20），LSR 的这个位为 1 时表示 THR 已空，可以写入下一个字节。

```cpp
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

类设计很简洁：构造时存端口地址，`init()` 配置硬件，`putc()` 发一个字节，`puts()` 发一个字符串。`base_port_` 是唯一的成员变量。

**实现文件 — init()**

```cpp
Serial::Serial(uint16_t port)
    : base_port_(port) {
}

void Serial::init(uint16_t /*port*/, uint32_t /*baud*/) {
    io_outb(base_port_ + SerialReg::IER, 0x00);  // 禁用中断
    io_outb(base_port_ + SerialReg::LCR, 0x03);  // 8N1
    io_outb(base_port_ + SerialReg::FCR, 0xC7);  // 启用 FIFO，清空缓冲
    io_outb(base_port_ + SerialReg::MCR, 0x03);  // DTR + RTS
    io_inb(base_port_ + SerialReg::LSR);          // 空读清除状态
}
```

初始化序列是 UART 16550 的标准配置。IER=0 禁用所有中断（我们用轮询模式不需要中断）。LCR=0x03 设置 8 个数据位、无校验、1 个停止位（即 "8N1"）。FCR=0xC7 启用 FIFO 并清空收发缓冲。MCR=0x03 设置 DTR（Data Terminal Ready）和 RTS（Request To Send）信号。最后空读一次 LSR 清除可能残留的状态位。

注意到 `init()` 的参数 `port` 和 `baud` 都被注释掉了——构造函数已经设置了端口，而 QEMU 的虚拟 UART 默认就是 115200 波特率，不需要编程除数锁存器。

**实现文件 — putc() 和 puts()**

```cpp
void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }
    io_outb(base_port_ + SerialReg::THR, static_cast<uint8_t>(c));
}
```

自旋等待 THR 空后写入。`pause` 指令是 spin-wait 的性能提示——告诉 CPU 这是一个忙等待循环，CPU 可以降低功耗、减少总线争用。

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

遍历字符串逐字符发送。关键细节：遇到 `\n` 时先发一个 `\r`（回车），把光标移回行首，再发 `\n`（换行）。串口终端需要 `\r\n` 才能正确换行回车，只发 `\n` 的话光标会在下一行的非行首位置。

### kprintf 格式化引擎 kernel/lib/kprintf.hpp + kprintf.cpp

这是大内核最"厚重"的用户空间移植代码——一个完整的 printf 实现，支持常用的格式化占位符。

**数字格式化辅助函数**

```cpp
static int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == (-9223372036854775807LL - 1)) {
            // INT64_MIN special case
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
    // ... 反转写入 buffer
}
```

十进制格式化需要特殊处理 INT64_MIN——因为 `-INT64_MIN` 会溢出（`INT64_MAX + 1` 在 64 位有符号数里不存在），直接取负是未定义行为。所以硬编码了 `"-9223372036854775808"` 字符串。其他负数先取绝对值再格式化。用 do-while 保证 `value=0` 时也输出 "0"。数字先生成到临时数组（逆序），再反转写入 buffer。

```cpp
static int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char  tmp[20];
    int   tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);
    // ... 反转写入 buffer
}
```

十六进制更简单——每次取最低 4 位查表得到字符，然后右移 4 位。

**格式化引擎模板**

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];

    while (*fmt != '\0') {
        if (*fmt != '%') {
            putc_fn(*fmt++);
            continue;
        }
        fmt++;  // 跳过 '%'

        bool zero_pad = false;
        int  width    = 0;
        if (*fmt == '0') { zero_pad = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
```

核心引擎是一个状态机：逐字符扫描格式字符串，非 `%` 字符直接输出；遇到 `%` 后解析可选的零填充标志和宽度，然后根据类型字符分发处理。

```cpp
        case 'p':
            putc_fn('0');
            putc_fn('x');
            len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
            for (int i = len; i < 16; i++) {
                putc_fn('0');
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;
```

`%p` 的处理比较有特色：先输出 "0x" 前缀，格式化后补零到 16 位（64 位地址的完整十六进制表示），然后输出实际数字。顺序很重要——先补零再输出数字，这样 `0x0000000001000000` 才是正确的。

```cpp
        do_padding:
            if (len < width) {
                char pad = zero_pad ? '0' : ' ';
                for (int i = width - len; i > 0; i--) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;
```

数字格式的公共处理：如果实际长度小于指定宽度，用零或空格填充。`do_padding` 标签通过 `goto` 从 `%d`、`%u`、`%x`、`%X` 四个 case 跳转过来——在这里用 goto 是合理的，避免了代码重复。

**公开接口**

```cpp
static Serial g_serial(SERIAL_COM1);

void kprintf_init() {
    g_serial.init();
}

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}
```

文件局部持有一个 `static Serial g_serial(SERIAL_COM1)` 单例。`kprintf` 用 lambda 捕获 `g_serial.putc` 作为 OutputFn 传给模板引擎。模板化设计让同一个格式化逻辑可以在测试中用 mock buffer 替换真实串口。

```cpp
[[noreturn]] void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`kpanic` 先输出消息再死循环——内核的"不可恢复错误"处理。`[[noreturn]]` 属性告诉编译器这个函数永远不返回，优化器可以利用这个信息。

## 设计决策

### 决策：模板化格式化引擎

**问题**：格式化逻辑需要在内核输出和单元测试中复用，如何避免重复代码？

**本项目的做法**：模板化引擎 `vkprintf_impl<OutputFn>`，接受泛型字符输出函数。

**备选方案**：用函数指针 `void (*putc_fn)(char)` 替代模板。

**为什么不选备选方案**：函数指针无法内联，每次字符输出都有一次间接调用开销。模板在编译时就能确定具体的输出函数，可以完全内联。虽然内核 printf 的性能不是瓶颈，但模板方案也更容易在测试中用 lambda 捕获状态（比如 mock buffer 的写入位置）。

**如果要扩展/改进**：可以增加更多格式化占位符（`%lld`、`%#x` 带前缀、`%-Nd` 左对齐），或者添加颜色控制码支持（`\033[31m` 红色错误信息）。

### 决策：轮询模式 vs 中断驱动

**问题**：串口驱动用轮询还是中断模式？

**本项目的做法**：轮询模式——自旋等待 THR 空。

**备选方案**：中断驱动模式，在 IER 中启用 THRE 中断，发送字符后做其他事，中断来时再发下一个。

**为什么不选备选方案**：内核启动阶段是单线程的，没有"其他事"可做。而且中断驱动需要 IDT 和中断处理机制，大内核还没有设置这些。轮询模式足够简单且正确。

**如果要扩展/改进**：等后续实现了 IDT 和中断框架后，可以在控制台驱动中使用中断驱动模式来提高效率。

## 扩展方向

- 添加 `%lld`、`%-Nd`（左对齐）、`%#x`（0x 前缀）等格式化支持（难度 ⭐）
- 实现 printk 级别（KERN_INFO、KERN_WARN、KERN_ERR），不同级别不同颜色（难度 ⭐⭐）
- 添加环形日志缓冲区，kpanic 时 dump 最近 N 条日志（难度 ⭐⭐）
- 实现 QEMU debug console（0xE9 端口）的并行输出（难度 ⭐）
- 用中断驱动 UART 接收（读取用户输入）（难度 ⭐⭐⭐）

## 参考资料

- Intel SDM: Vol.2A — `IN`/`OUT` 指令参考
- OSDev Wiki: [Serial Ports](https://wiki.osdev.org/Serial_Ports) — UART 16550 寄存器映射
- OSDev Wiki: [Kernel Printf](https://wiki.osdev.org/Kernel_printf) — 内核 printf 实现参考
- Linux: [8250 serial driver](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/8250/8250_core.c) — 生产级 UART 驱动
- xv6: [printf.c](https://github.com/mit-pdos/xv6-public/blob/master/printf.c) — 简化的内核 printf
