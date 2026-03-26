# 005 Mini Kernel Entry - 串口输出与调试基建

## 章节导语

上一章我们完成了 Bootloader 到 Mini Kernel 的加载跳转，内核终于开始运行了。但说实话，现在的内核就像一个黑盒子——它在跑，但我们什么都看不到，什么都不知道。这对于内核开发来说是致命的，调试时需要知道"程序跑到了哪里""变量是多少""出错了没"。

所以这一章我们要给内核装上"嘴巴"——串口驱动和格式化输出函数。串口是内核开发中最基础也是最重要的调试手段，它简单可靠，不需要复杂的图形驱动，只要几行 I/O 端口操作就能输出字符。完成本章后，你将在 QEMU 串口看到 `[MINI] Bootstrap kernel running @ 0x20000` 这样的输出，证明内核真的活过来了。

本章的前置知识是上一章的 Bootloader 加载流程，以及对 x86 I/O 端口的基本了解。

---

## 概念精讲

### 串口（UART）是什么

串口是一种古老的通信接口，但它在内核开发中地位不可撼动。你可以把它理解为一根"虚拟电线"：内核往这头写数据，QEMU 在那头把数据转给你看。

```
内核 --[outb 0x3F8]--> UART硬件 <--[QEMU捕获]--> 你的终端
```

x86 平台上标准的 COM1 串口基地址是 `0x3F8`，我们通过 I/O 端口指令 `inb`/`outb` 来读写它。QEMU 默认会把串口输出重定向到标准输出（`-serial stdio`），所以你在终端里能直接看到。

**为什么不用 printf 直接输出到屏幕？** VGA 文本模式确实可以，但它需要操作显存（0xB8000），还得处理光标位置、滚动等一堆琐事。串口就简单多了——写一个字节，发送。而且串口输出可以被 QEMU 捕获保存，对自动化测试很友好。

### I/O 端口与内存映射的区别

在 x86 上访问硬件有两种方式：内存映射 I/O（MMIO）和端口映射 I/O（PMIO）。串口用的是后者，通过专门的 `in`/`out` 指令访问。

```c
// 内存映射：直接读写地址
*ptr = value;         // 像访问 RAM 一样

// 端口映射：使用专用指令
outb(0x3F8, value);   // 显式指定端口
```

内联汇编中的 `inb`/`outb` 格式是 AT&T 语法：操作数顺序是 `源, 目的`，所以 `inb %1, %0` 表示"从端口 %1 读到 %0"。

### AT&T 汇编语法速查

GNU AS 使用 AT&T 语法，和 Intel 语法有些差异：

| 特性 | AT&T | Intel |
|------|------|-------|
| 操作数顺序 | `源, 目的` | `目的, 源` |
| 寄存器 | `%rax` | `rax` |
| 立即数 | `$1` | `1` |
| 立即数作为地址 | `($0x3F8)` | `[0x3F8]` |
| 操作数后缀 | `movb` (byte), `movl` (long) | `mov` (自动推断) |

I/O 端口指令示例：
```asm
outb %al, $0xE9    ; 把 al 的值写入端口 0xE9 (debugcon)
inb $0x3F8, %al    ; 从端口 0x3F8 读取一个字节到 al
```

---

## 动手实现

### Step 1：创建 I/O 端口封装层

**目标**：提供 `inb()`/`outb()` 内联函数，封装 x86 的 I/O 端口操作。

**代码**（文件路径：`kernel/mini/driver/io.h`）：
```cpp
#pragma once
#include <stdint.h>

namespace cinux::mini::io {

// 从端口读取一个字节
inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

// 向端口写入一个字节
inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

}
```

**解释**：
这里用了 `inline` 函数而不是宏，既保证性能又保持类型安全。`volatile` 告诉编译器"别乱优化这个 I/O 操作"。约束部分 `"=a"(value)` 表示输出使用 `eax/rax` 寄存器，`"Nd"(port)` 表示输入可以是 8 位立即数或 `dx` 寄存器（x86 约定端口号放在 dx）。

**验证**：这一步没有可执行代码，编译通过即可。

---

### Step 2：实现串口驱动

**目标**：封装 UART 初始化和字符输出，提供 `Serial::putc()` 和 `Serial::puts()`。

**代码**（文件路径：`kernel/mini/driver/serial.h`）：
```cpp
#pragma once
#include <stdint.h>
#include "io.h"

namespace cinux::mini::serial {

// COM1 标准基地址
constexpr uint16_t SERIAL_COM1 = 0x03F8;

// UART 寄存器偏移
namespace SerialReg {
    constexpr uint8_t THR = 0;  // 发送保持寄存器（写）
    constexpr uint8_t IER = 1;  // 中断使能寄存器
    constexpr uint8_t FCR = 2;  // FIFO 控制寄存器
    constexpr uint8_t LCR = 3;  // 线路控制寄存器
    constexpr uint8_t MCR = 4;  // 调制解调控制寄存器
    constexpr uint8_t LSR = 5;  // 线路状态寄存器
}

// LSR 状态位
namespace SerialLSR {
    constexpr uint8_t TX_READY = 0x20;  // 发送缓冲区空
}

class Serial {
    uint16_t base_port;

    // 检查是否可以发送
    bool is_tx_ready() const {
        return (io::inb(base_port + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
    }

public:
    explicit Serial(uint16_t port = SERIAL_COM1);
    void init();           // 初始化为 115200 8N1
    void putc(char c);     // 发送单个字符（阻塞轮询）
    void puts(const char* s);  // 发送字符串
};

// 获取全局串口实例（单例）
Serial& get_initial_serial();

}
```

**实现**（文件路径：`kernel/mini/driver/serial.cpp`）：
```cpp
#include "serial.h"

namespace cinux::mini::serial {

void Serial::init() {
    // 禁用中断（我们用轮询模式）
    io::outb(base_port + SerialReg::IER, 0x00);

    // LCR = 0x03: 8位数据，无校验，1停止位（8N1）
    io::outb(base_port + SerialReg::LCR, 0x03);

    // 启用 FIFO，清除缓冲区，设置 14 字节阈值
    io::outb(base_port + SerialReg::FCR, 0xC7);

    // 设置 RTS + DTR（告诉终端"我准备好了"）
    io::outb(base_port + SerialReg::MCR, 0x03);
}

Serial::Serial(uint16_t port) : base_port(port) {
    init();
}

void Serial::putc(char c) {
    // 等待发送缓冲区就绪
    while (!is_tx_ready()) {
        __asm__ volatile("pause");  // 降低功耗，提示 CPU 这是个自旋锁
    }
    io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}

void Serial::puts(const char* s) {
    if (s == nullptr) return;

    while (*s != '\0') {
        // 处理换行符：输出 \r\n（串口标准）
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}

// 全局单例
static Serial g_serial(SERIAL_COM1);

Serial& get_initial_serial() {
    return g_serial;
}

}
```

**解释**：
串口初始化的核心是设置 LCR 为 `0x03`（8N1 模式）。QEMU 默认波特率就是 115200，所以我们不用折腾分频锁存器（DLAB）。`putc()` 用的是轮询方式——一直查 LSR 的 bit 5，直到发送缓冲区空。这在中断系统建立之前是最简单可靠的方式。

**常见陷阱**：忘记把 `\n` 转成 `\r\n` 会导致输出只有第一行。这是因为很多终端期望 `\r\n` 作为换行序列。

**验证**：还没有被调用，但编译应该没问题。

---

### Step 3：实现格式化函数（数字转字符串）

**目标**：提供 `format_decimal()`/`format_hex()`/`format_binary()` 函数，把数字转成字符串。

**代码**（文件路径：`kernel/mini/lib/private/format.h`）：
```cpp
#pragma once
#include <stdint.h>

namespace cinux::mini::lib::detail {

// 有符号十进制转字符串
int format_decimal(int64_t value, char* buffer, int buffer_size);

// 十六进制转字符串（lowercase 控制大小写）
int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase);

// 二进制转字符串
int format_binary(uint64_t value, char* buffer, int buffer_size);

}
```

**实现**（文件路径：`kernel/mini/lib/private/format.cpp`）：
```cpp
#include "format.h"
#include <limits.h>

namespace cinux::mini::lib::detail {

int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int idx = 0;
    bool is_neg = value < 0;

    // 特殊处理 INT64_MIN（它的绝对值无法表示）
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

    // 数字转字符（倒序）
    uint64_t abs_val = static_cast<uint64_t>(value);
    char tmp[24];
    int tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + (abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    // 添加负号
    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    // 反转回来
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}

int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1) return 0;

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char tmp[20];
    int tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);

    // 反转
    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}

int format_binary(uint64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    // 找到最高有效位（跳过前导零）
    int bit = 63;
    bool found = false;
    while (bit >= 0) {
        if ((value >> bit) & 1) {
            found = true;
            break;
        }
        bit--;
    }

    if (!found) bit = 0;  // 全零的情况

    // 输出二进制串
    int idx = 0;
    for (int i = bit; i >= 0 && idx + 1 < buffer_size; i--) {
        buffer[idx++] = ((value >> i) & 1) ? '1' : '0';
    }
    buffer[idx] = '\0';

    return idx;
}

}
```

**解释**：
这三个函数是纯算法实现，不依赖任何硬件。`format_decimal` 采用了"先倒序存到临时数组，再反转"的技巧，比递归省栈空间。`INT64_MIN` 的特殊处理是因为它的绝对值无法表示（64 位有符号整数范围不对称）。

**验证**：还没有被调用，编译通过即可。

---

### Step 4：实现 kprintf（格式化输出函数）

**目标**：实现类似 `printf` 的内核输出函数 `kprintf`，支持 `%d %u %x %X %s %p %c %%` 等占位符。

**代码**（文件路径：`kernel/mini/lib/kprintf.h`）：
```cpp
#pragma once

namespace cinux::mini::lib {

// 内核格式化输出（通过串口）
// 支持的占位符：
//   %% - 百分号
//   %c - 字符
//   %s - 字符串（nullptr 打印为 "(null)"）
//   %d - 有符号十进制
//   %u - 无符号十进制
//   %x - 小写十六进制
//   %X - 大写十六进制
//   %p - 指针（带 0x 前缀）
//   %Nd/%0Nd - 最小宽度 N，空格/零填充
void kprintf(const char* format, ...);

// Debug Console 输出（端口 0xE9，QEMU 特有）
void kdebugf(const char* format, ...);

}
```

**实现**（文件路径：`kernel/mini/lib/kprintf.cpp`）：
```cpp
#include "kprintf.h"
#include "private/format.h"
#include <stdarg.h>
#include <stdint.h>
#include "driver/serial.h"

namespace {

using namespace cinux::mini::lib::detail;

// 通用格式化输出
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc, const char* format, va_list args) {
    char buffer[64];

    while (*format != '\0') {
        if (*format == '%') {
            format++;

            // 解析宽度选项（如 %04d）
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
                // 指针：先输出 "0x"，再输出十六进制
                for (const char* p = "0x"; *p; p++) putc(*p);
                len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
                for (int i = 0; i < len; i++) putc(buffer[i]);
                break;

            case 'b':
                len = format_binary(va_arg(args, uint64_t), buffer, sizeof(buffer));
                goto do_padding;

do_padding:
                // 处理宽度填充
                if (len < width) {
                    char pad = zero_pad ? '0' : ' ';
                    for (int i = width - len; i > 0; i--) putc(pad);
                }
                for (int i = 0; i < len; i++) putc(buffer[i]);
                break;

            default:
                // 未知占位符，原样输出
                putc('%');
                putc(type);
                break;
            }
        } else {
            putc(*format++);
        }
    }
}

void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

} // namespace

namespace cinux::mini::lib {

void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    auto& serial = serial::get_initial_serial();
    vkprintf_impl([&](char c) { serial.putc(c); }, format, args);

    va_end(args);
}

void kdebugf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    vkprintf_impl([](char c) { debugcon_putc(c); }, format, args);

    va_end(args);
}

}
```

**解释**：
`kprintf` 的核心是 `vkprintf_impl` 模板函数，它接受一个"字符输出回调"，这样同一个格式化逻辑可以复用到串口和 debug console。`goto do_padding` 虽然看起来"不文明"，但在这里是合理的——多个 case 共同一段后处理逻辑，比复制粘贴要好。

**为什么用模板而不是虚函数？** 内核禁用 RTTI，而且模板在编译期展开，零运行时开销。

**验证**：编译通过即可，还没被调用。

---

### Step 5：更新 CMakeLists.txt

**目标**：把新文件加入构建系统。

**代码**（文件路径：`kernel/mini/CMakeLists.txt`）：
```cmake
add_executable(mini_kernel
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    main.cpp
    driver/serial.cpp       # 新增
    lib/kprintf.cpp         # 新增
    lib/private/format.cpp  # 新增
)

target_include_directories(mini_kernel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}  # 确保 #include "driver/io.h" 能找到
)
```

**解释**：
确保 `driver/` 和 `lib/` 目录都在 include 路径里。`kprintf.cpp` 依赖 `serial.h`，`serial.cpp` 依赖 `io.h`，所以头文件搜索路径要正确。

**验证**：`cmake --build build` 应该成功编译。

---

### Step 6：在 main.cpp 中使用 kprintf

**目标**：在内核入口点输出欢迎信息。

**代码**（文件路径：`kernel/mini/main.cpp`）：
```cpp
extern "C" {
#include <stdint.h>
}

#include "../../boot/boot_info.h"
#include "lib/kprintf.h"

using cinux::mini::lib::kprintf;

extern "C" {
extern uint64_t __boot_info_ptr;
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    // 串口输出（此时串口已由全局构造函数初始化）
    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("[MINI] Bootstrap kernel running @ %p\n",
            (void*)boot_info->kernel_phys_base);

    // TODO: 初始化内核子系统
    // TODO: 启动调度器

    // 停机
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

**解释**：
这里有个关键点——串口在 `kprintf` 调用前就已经初始化了。因为 `Serial` 的全局实例 `g_serial` 在 `.data` 段，它的构造函数会在 `_init_global_ctors()` 中被调用（见 `crt_stub.cpp`）。而 `_init_global_ctors` 在 `boot.S` 的 `_start` 中早于 `mini_kernel_main` 执行。

**验证**：现在可以运行了！

---

### Step 7：构建与运行

```bash
# 从当前 tag 开始
git checkout 004_boot_load_mini_kernel_C

# 配置构建
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..

# 编译
make

# 运行
make run
```

QEMU 参数说明：
- `-m 512M`：分配 512MB 内存
- `-serial stdio`：串口重定向到标准输出
- `-no-reboot`：崩溃时不自动重启
- `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9`：debug console 输出到文件
- `-drive file=cinux.img,format=raw,...`：挂载我们的磁盘镜像

**预期输出**：
```
1234Cinux Mini Kernel v0.1.0
[MINI] Bootstrap kernel running @ 0x20000
```

前四位的 `1234` 是 `boot.S` 中的 debugcon 输出，证明启动流程正常。后面的串口输出证明 `kprintf` 工作正常。

---

## 调试技巧

### 常见问题排查

**问题 1：串口没有输出**
- 检查 QEMU 启动参数是否包含 `-serial stdio`
- 确认 `Serial::init()` 被调用（全局构造函数）
- 用 `kdebugf()` 替代 `kprintf()` 测试——如果 `debug.log` 有内容说明串口初始化有问题

**问题 2：输出只有一个字符**
- 通常是 `putc()` 的轮询逻辑有问题，检查 `is_tx_ready()` 的实现
- 可能忘记启用 UART 的 FIFO（`FCR` 寄存器）

**问题 3：编译报错 undefined reference to `kprintf`**
- 检查 CMakeLists.txt 是否包含所有 `.cpp` 文件
- 确认头文件搜索路径正确

### 页表映射与链接脚本调试排查

在实现 higher-half 内核映射时，可能会遇到两个关键问题导致内核无法正常启动。虽然这些是在 bootloader 阶段建立的，但理解它们对内核调试很重要。

#### 问题 A：页表 PDPT 索引计算错误

**症状**：
- 输出停留在 `OPL` 后（bootloader 的输出），没有后续的 `1234`
- 使用 identity mapping (0x20000) 可以正常执行
- 使用 higher-half mapping 时发生页面错误

**根本原因**：

在 x86-64 分页机制中，虚拟地址 `0xFFFFFFFF80000000` 的 PDPT 索引容易算错：

```
虚拟地址: 0xFFFFFFFF80000000
二进制:   1111111111111111111111111111111110000000000000000000000000000000
          ^^^^^^^^^ bits 47:39 (PML4 索引)
                     ^^^^^^^^ bits 38:30 (PDPT 索引)
```

正确计算：
- **PML4 索引** (bits 47:39) = `0x1FF` = **511**
- **PDPT 索引** (bits 38:30) = `0x1FE` = **510** ← 这里容易写错！
- **PD 索引** (bits 29:21) = `0x000` = **0**

如果使用 PDPT[511]，CPU 会查找一个不存在的页表项（not present），触发页面错误。

**验证方法**：

在进入 long mode 后添加页表验证代码（bootloader 阶段）：

```assembly
// 读取 PML4[511]
movq %cr3, %rax
movq 0xFF8(%rax), %rbx
// 输出 %rbx，应该是 0x0000000000002003

// 读取 PDPT[510]
movq $0x2000, %rax
movq 0xFF0(%rax), %rbx  // PDPT[510] 在偏移 510*8 = 0xFF0
// 输出 %rbx，应该是 0x0000000000003003
```

#### 问题 B：链接器脚本 LMA 计算错误

**症状**：
- `__init_array` 中的构造函数指针是 `0xffffffff` (垃圾值)
- 调用全局构造函数时崩溃
- 数据段内容不正确

**根本原因**：

使用 `SIZEOF()` 累加计算 LMA 时，**没有考虑段间对齐填充**。

```ld
/* 错误的做法 */
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    .text : AT(KERNEL_PHYS_BASE) { *(.text) }
    .data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) {  /* ← 错误！ */
        *(.data)
    }
    .init_array : AT(KERNEL_PHYS_BASE + SIZEOF(.text) + SIZEOF(.data)) {
        /* ↑ 错误！SIZEOF 不包含对齐填充 */
        __init_array_start = .;
        KEEP(*(.init_array))
        __init_array_end = .;
    }
}
```

**正确的链接器脚本**：

```ld
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text .text.*)
    }

    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }
}
```

**为什么 `ADDR() - KERNEL_Virt_BASE` 有效？**

无论段间有多少对齐填充，这个关系始终成立：
```
LMA = VMA - KERNEL_Virt_BASE
```

**验证方法**：

```bash
# 查看 ELF 段的 LMA
readelf -l build/kernel/mini/mini_kernel.elf | grep -A 20 "Program Headers"

# 检查符号地址
nm build/kernel/mini/mini_kernel.elf | grep __init_array
```

### 故障排查流程图

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
    │       ├─> 验证构造函数指针有效
    │       └─> 检查链接器脚本 LMA 计算
    │
    └─> 输出有 "4" 但没有内核输出
        └─> 检查 mini_kernel_main 入口
```

### GDB 调试示例

```bash
# 终端 1：启动 QEMU 调试模式
make run-debug

# 终端 2：连接 GDB
gdb build/kernel/elf
(gdb) target remote :1234
(gdb) break mini_kernel_main
(gdb) continue
(gdb) print boot_info->kernel_phys_base
```

### 串口断言技巧

在内核测试中可以用"串口断言"——如果断言失败就卡死，配合 QEMU 的 `isa-debug-exit` 设备实现自动退出：

```cpp
#define KERNEL_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            kprintf("[ASSERT FAIL] %s at %s:%d\n", #cond, __FILE__, __LINE__); \
            __asm__ volatile("outl %0, $0xf4" : : "a"(0xF));  // QEMU 退出码异常 \
        } \
    } while(0)
```

---

## 本章小结

### 新增关键函数一览

| 函数 | 功能 | 所在文件 |
|------|------|----------|
| `io::inb(port)` | 从 I/O 端口读取一个字节 | `driver/io.h` |
| `io::outb(port, val)` | 向 I/O 端口写入一个字节 | `driver/io.h` |
| `Serial::init()` | 初始化串口为 115200 8N1 | `driver/serial.cpp` |
| `Serial::putc(c)` | 发送单个字符（阻塞） | `driver/serial.cpp` |
| `Serial::puts(s)` | 发送字符串 | `driver/serial.cpp` |
| `format_decimal()` | 整数转十进制字符串 | `lib/private/format.cpp` |
| `format_hex()` | 整数转十六进制字符串 | `lib/private/format.cpp` |
| `kprintf()` | 格式化输出到串口 | `lib/kprintf.cpp` |
| `kdebugf()` | 格式化输出到 debug console | `lib/kprintf.cpp` |

### UART 寄存器速查

| 偏移 | 名称 | 读/写 | 说明 |
|------|------|-------|------|
| 0 | THR/RBR | 写/读 | 发送/接收缓冲区 |
| 1 | IER | R/W | 中断使能（我们设为 0） |
| 3 | LCR | R/W | 线路控制（0x03 = 8N1） |
| 5 | LSR | 只读 | 线路状态（bit 5 = TX 就绪） |

### 下一章预告

现在内核能"说话"了，下一章（`005_mini_kernel_memory`）我们要给内核装上"记忆"——物理内存管理器（PMM）。这会涉及 E820 内存映射解析、位图分配算法，以及页表的初始化。

调试基建和测试基建已就绪，之后的开发将事半功倍。
