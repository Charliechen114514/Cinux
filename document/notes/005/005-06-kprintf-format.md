---
title: kprintf 格式化
---

# 005-06: kprintf 格式化实现

## 概述

`kprintf` 是 Cinux 内核的核心调试输出函数，支持类似 `printf` 的格式化输出，通过串口 COM1 发送数据。其设计采用模板函数模式，实现了格式化逻辑与输出设备的解耦。

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                     kprintf 架构                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  kprintf(format, ...)                                       │
│       │                                                     │
│       ▼                                                     │
│  va_list ─────────────────────────────────────┐            │
│       │                                        │            │
│       ▼                                        ▼            │
│  vkprintf_impl(lambda{serial.putc})   vkprintf_impl(lambda{debugcon_putc})│
│       │                                        │            │
│       ▼                                        ▼            │
│  format_decimal/hex/binary              format_decimal/hex/binary│
│       │                                        │            │
│       ▼                                        ▼            │
│  Serial::putc(c)                      debugcon_putc(c)      │
│  └─> io::outb(0x3F8, c)                └─> outb(0xE9, c)   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

## 核心实现

### 1. vkprintf_impl 模板函数

文件：[`kernel/mini/lib/kprintf.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/kprintf.cpp)

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc, const char* format, va_list args) {
    char buffer[64];

    while (*format != '\0') {
        if (*format == '%') {
            format++;

            // 解析宽度修饰符
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

            // 格式化分支
            switch (type) {
            case '%': putc('%'); break;
            case 'c': putc(static_cast<char>(va_arg(args, int))); break;
            case 's': /* ... */ break;
            case 'd': len = format_decimal(...); goto do_padding;
            case 'u': len = format_decimal(...); goto do_padding;
            case 'x': len = format_hex(..., true); goto do_padding;
            case 'X': len = format_hex(..., false); goto do_padding;
            case 'p': /* 指针特殊处理 */ break;
            case 'b': len = format_binary(...); goto do_padding;
            }

do_padding:
            // 宽度填充
            if (len < width) {
                char pad = zero_pad ? '0' : ' ';
                for (int i = width - len; i > 0; i--)
                    putc(pad);
            }
            for (int i = 0; i < len; i++)
                putc(buffer[i]);
        } else {
            putc(*format++);
        }
    }
}
```

### 2. kprintf - 串口输出

```cpp
void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    auto& serial = serial::get_initial_serial();
    vkprintf_impl([&](char c) { serial.putc(c); }, format, args);

    va_end(args);
}
```

### 3. kdebugf - DebugConsole 输出

```cpp
void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

void kdebugf(const char* format, ...) {
    va_list args;
    va_start(args, format);

    vkprintf_impl([](char c) { debugcon_putc(c); }, format, args);

    va_end(args);
}
```

## 支持的格式符

| 格式符 | 类型 | 说明 | 示例 |
|--------|------|------|------|
| `%%` | - | 百分号本身 | `%%` → `%` |
| `%c` | int | 单个字符 | `%c` → `A` |
| `%s` | char* | 字符串（null 输出 "(null)"） | `%s` → `Hello` |
| `%d` | int64_t | 有符号十进制 | `%d` → `-12345` |
| `%u` | uint64_t | 无符号十进制 | `%u` → `42` |
| `%x` | uint64_t | 小写十六进制（无前缀） | `%x` → `deadbeef` |
| `%X` | uint64_t | 大写十六进制（无前缀） | `%X` → `DEADBEEF` |
| `%p` | uint64_t | 指针（带 "0x" 前缀） | `%p` → `0xffffffff80000000` |
| `%b` | uint64_t | 二进制（无前缀，压缩前导零） | `%b` → `101010` |

## 宽度修饰符

| 格式 | 说明 | 示例输入 | 示例输出 |
|------|------|----------|----------|
| `%Nd` | 最小宽度 N，空格右对齐 | `[%4d]`, `7` | `[   7]` |
| `%0Nd` | 最小宽度 N，零填充 | `[%04d]`, `7` | `[0007]` |

## 与硬件的接口

### Serial::putc 调用链

```
kprintf()
  └─> Serial::putc(c)
       └─> Serial::is_tx_ready()  // 检查 LSR bit 5
       └─> io::outb(0x3F8 + THR, c)  // 写入发送保持寄存器
```

### debugcon_putc 调用链

```
kdebugf()
  └─> debugcon_putc(c)
       └─> outb %al, $0xE9  // 直接写入 QEMU debugcon 端口
```

## 文件清单

| 文件 | 描述 |
|------|------|
| [`kernel/mini/lib/kprintf.h`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/kprintf.h) | 函数声明 |
| [`kernel/mini/lib/kprintf.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/kprintf.cpp) | 核心实现 |
| [`kernel/mini/lib/private/format.h`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/private/format.h) | 格式化函数声明 |
| [`kernel/mini/lib/private/format.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/lib/private/format.cpp) | 格式化函数实现 |
| [`kernel/mini/driver/serial.h`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/driver/serial.h) | 串口驱动 |
| [`kernel/mini/driver/io.h`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/driver/io.h) | I/O 端口操作 |

## 使用示例

```cpp
// 基本输出
kprintf("Hello, Cinux!\n");

// 格式化输出
kprintf("Value: %d, Hex: %x\n", -42, 0xDEADBEEF);

// 指针输出
kprintf("Address: %p\n", ptr);

// 宽度格式化
kprintf("[%04d] [%4d]\n", 7, 7);  // [0007] [   7]

// 调试输出（仅 QEMU debugcon）
kdebugf("Debug: %s\n", "message");
```

## 相关链接

- [005-07: 格式化算法详解](005-07-format-algorithms.md) - format_decimal/hex/binary 实现
- [005-10: 串口调试详解](005-10-serial-debug.md) - UART 驱动与寄存器
