---
title: 012-driver-serial-2 · 串口驱动
---

# kprintf 格式化引擎重构与左对齐修饰符

## 概览

这是 tag 012 里改动量最大、影响面最广的部分。之前大内核和迷你内核各自维护一份完整的格式化引擎代码——`format_decimal`、`format_hex`、以及 `vkprintf_impl` 模板函数——两份代码逻辑几乎相同，只是风格和命名略有差异。这种重复在工程上是一个定时炸弹：每次修格式化逻辑都要同步改两个地方，忘了改一个就会出现行为不一致。这次重构把核心格式化引擎抽成独立的 header-only 模板文件，大内核用 `kernel/lib/private/vkprintf_impl.hpp`，迷你内核用 `kernel/mini/lib/private/vkprintf_impl.h`，各自只保留薄薄的"输出适配层"。同时，我们在这次重构中加入了 `%-Nd` 和 `%-Ns` 左对齐修饰符，以及负数零填充时符号前置的正确处理。

## 架构图

```
重构前:

  kernel/lib/kprintf.cpp           kernel/mini/lib/kprintf.cpp
  +---------------------------+    +---------------------------+
  | format_decimal()          |    | format_decimal()          |
  | format_hex()              |    | format_hex()              |
  | format_binary()           |    | format_binary()           |
  | vkprintf_impl<OutputFn>() |    | vkprintf_impl<OutputFn>() |
  | Serial::putc adapter      |    | debugcon/serial adapter   |
  +---------------------------+    +---------------------------+
        ^ 重复代码 ^                      ^ 重复代码 ^

重构后:

  kernel/lib/private/vkprintf_impl.hpp     (大内核，inline + namespace)
  +-----------------------------------+
  | format_decimal()                  |
  | format_hex()                      |
  | vkprintf_impl<OutputFn>()         |
  +-----------------------------------+
          ^                    ^
          | include            | include (test 直接引用)
          |                    |
  kernel/lib/kprintf.cpp       tests/unit/test_kprintf.cpp
  +-----------------------+    +-----------------------+
  | g_serial singleton    |    | MockOutput + do_printf|
  | kprintf/kvprintf/     |    | 35+ TEST() cases      |
  | kpanic adapters       |    +-----------------------+
  +-----------------------+

  kernel/mini/lib/private/vkprintf_impl.h   (迷你内核，include format.h)
  +-----------------------------------+
  | vkprintf_impl<OutputFn>()         |
  +-----------------------------------+
          ^
          | include
          |
  kernel/mini/lib/kprintf.cpp
  +-----------------------+
  | kprintf adapter       |
  | kdebugf adapter       |
  +-----------------------+
```

## 代码精讲

### 大内核的 vkprintf_impl.hpp —— 数值格式化辅助函数

我们先来看 `kernel/lib/private/vkprintf_impl.hpp` 中 `format_decimal` 和 `format_hex` 的完整实现。

```cpp
// kernel/lib/private/vkprintf_impl.hpp

#pragma once

#include <stdarg.h>
#include <stdint.h>

namespace cinux::lib::detail {
```

文件头声明了 `#pragma once` 防止重复包含，引入 `stdarg.h`（va_list 支持）和 `stdint.h`（int64_t 等类型），然后进入 `cinux::lib::detail` 命名空间。这个命名空间的选择是有意的——`detail` 是 C++ 社区约定俗成的"内部实现细节"命名空间，告诉使用者"别直接依赖这里面的东西"。

```cpp
inline int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) {
        return 0;
    }

    int  idx    = 0;
    bool is_neg = value < 0;

    if (is_neg) {
        if (value == static_cast<int64_t>(0x8000000000000000ULL)) {
            // INT64_MIN special case -- cannot negate
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
```

`format_decimal` 把一个 `int64_t` 转成十进制字符串。这里有一个很关键的细节——INT64_MIN 的特殊处理。`INT64_MIN = -9223372036854775808`，它的绝对值 `9223372036854775808` 超出了 `int64_t` 的表示范围（最大正值是 `9223372036854775807`），所以 `value = -value` 会导致溢出，C++ 中这是未定义行为。解决方法就是直接硬编码这个字符串。说实话这种 corner case 在内核代码里经常被遗忘，但只要你用 `%d` 打印过 `INT_MIN`，你就会感谢这个分支的存在。

```cpp
    uint64_t abs_val = static_cast<uint64_t>(value);
    char     tmp[24];
    int      tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + static_cast<char>(abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

数字到字符串的转换用的是经典的"取模 + 反转"算法。先把每一位数字逆序存到 `tmp` 数组里（个位在最前面），然后再从后往前复制到 `buffer` 中实现反转。负号在反转之后、数字之前插入。`do ... while` 确保即使 `value` 是 0 也会输出 "0" 而不是空字符串。

接下来是 `format_hex`：

```cpp
inline int format_hex(uint64_t value, char* buffer, int buffer_size,
                      bool lowercase) {
    if (buffer_size < 1) {
        return 0;
    }

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char  tmp[20];
    int   tmp_idx = 0;

    do {
        tmp[tmp_idx++] = digits[value & 0xF];
        value >>= 4;
    } while (value > 0 && tmp_idx < 20);

    int idx = 0;
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

逻辑和 `format_decimal` 类似，基数从 10 换成 16。每一位通过 `value & 0xF` 取出，`lowercase` 控制大小写。`tmp` 最多 20 个元素（64 位最多 16 个十六进制位，留余量）。

两个函数用 `inline` 修饰是因为定义在头文件中且需要被多个编译单元包含，否则链接时会出现 ODR 冲突。

### vkprintf_impl 模板 —— 格式化引擎核心

现在我们来看真正的格式解析引擎。

```cpp
template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc_fn, const char* fmt, va_list args) {
    char buffer[64];

    while (*fmt != '\0') {
        if (*fmt != '%') {
            putc_fn(*fmt++);
            continue;
        }

        // Consume '%'
        fmt++;
```

模板参数 `OutputFn` 是设计精髓——可以是 lambda、函数指针或任何可调用对象。`OutputFn&&` 是万能引用。主循环逐字符扫描，遇到 `%` 进入格式说明符解析。

```cpp
        // Parse optional left-align flag '-'
        bool left_align = false;
        if (*fmt == '-') {
            left_align = true;
            fmt++;
        }

        // Parse optional zero-pad flag '0'
        bool zero_pad = false;
        if (*fmt == '0') {
            zero_pad = true;
            fmt++;
        }

        // Parse optional width
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
```

格式说明符的解析顺序是 `-` 标志 -> `0` 标志 -> 宽度数字 -> 类型字符。这个顺序和标准 C 的 printf 格式字符串规范一致。`left_align` 和 `zero_pad` 是互斥的两个布尔标志——左对齐时填充字符只能是空格，零填充只在右对齐时有意义。宽度用简单的 `width * 10 + digit` 累加计算，支持任意多位数的宽度值。

接下来看各格式说明符的处理。`%%`、`%c` 和纯文本比较简单，我们重点看 `%s`、`%d`、`%x`/`%X`、和 `%p`。

#### %s —— 字符串格式化（含左右对齐）

```cpp
        case 's': {
            const char* s = va_arg(args, const char*);
            if (s == nullptr) {
                s = "(null)";
            }

            // Measure string length
            int slen = 0;
            while (s[slen] != '\0') {
                slen++;
            }

            if (left_align) {
                // Print string first, then pad with spaces
                for (int i = 0; i < slen; i++) {
                    putc_fn(s[i]);
                }
                for (int i = slen; i < width; i++) {
                    putc_fn(' ');
                }
            } else {
                // Pad first, then string
                for (int i = slen; i < width; i++) {
                    putc_fn(' ');
                }
                for (int i = 0; i < slen; i++) {
                    putc_fn(s[i]);
                }
            }
            break;
        }
```

`%s` 有两个重点：`nullptr` 安全（输出 `"(null)"` 而非崩溃），以及宽度与对齐——先测量字符串长度 `slen`，再根据 `left_align` 决定先输出字符串还是先补空格。

#### %d —— 有符号十进制（含负数零填充的符号处理）

```cpp
        case 'd': {
            len = format_decimal(static_cast<int64_t>(va_arg(args, int)),
                                 buffer, sizeof(buffer));

            // Determine if the formatted string starts with a sign
            bool has_sign = (len > 0 && buffer[0] == '-');
            int digits_len = has_sign ? len - 1 : len;

            if (!left_align && zero_pad && has_sign) {
                // Sign first, then zero-pad, then digits
                putc_fn('-');
                for (int i = digits_len; i < width - 1; i++) {
                    putc_fn('0');
                }
                for (int i = 1; i < len; i++) {
                    putc_fn(buffer[i]);
                }
            } else if (!left_align) {
                // Right-align: pad before entire content
                char pad = zero_pad ? '0' : ' ';
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
                for (int i = 0; i < len; i++) {
                    putc_fn(buffer[i]);
                }
            } else {
                // Left-align: content first, then spaces
                for (int i = 0; i < len; i++) {
                    putc_fn(buffer[i]);
                }
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }
```

`%d` 是引擎最复杂的部分——`%06d` 格式化 `-42` 必须得到 `-00042`（负号在前），而不是 `000-42`。三个分支覆盖所有情况：`!left_align && zero_pad && has_sign` 先输出负号再补零再输出数字；`!left_align` 根据填充字符先补填充再输出完整内容；左对齐先输出内容再补空格。`width - 1` 是因为负号已占一个字符位。`va_arg(args, int)` 转成 `int64_t` 传给 `format_decimal`，引擎内部统一 64 位运算。

#### %u / %x / %X —— 无符号数值格式化

```cpp
        case 'u': {
            len = format_decimal(
                static_cast<int64_t>(va_arg(args, unsigned int)),
                buffer, sizeof(buffer));

            char pad = zero_pad ? '0' : ' ';
            if (!left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(pad);
                }
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            if (left_align) {
                for (int i = len; i < width; i++) {
                    putc_fn(' ');
                }
            }
            break;
        }
```

`%u`、`%x`、`%X` 逻辑统一——不涉及符号问题，右对齐先填充再输出，左对齐先输出再填充。`%u` 复用 `format_decimal`，`%x`/`%X` 调用 `format_hex`。

#### %p —— 指针格式化

```cpp
        case 'p': {
            // Always output "0x" + 16-digit zero-padded uppercase hex
            putc_fn('0');
            putc_fn('x');
            len = format_hex(va_arg(args, uint64_t), buffer,
                             sizeof(buffer), false);

            // Pad to 16 digits with leading zeros
            for (int i = len; i < 16; i++) {
                putc_fn('0');
            }
            for (int i = 0; i < len; i++) {
                putc_fn(buffer[i]);
            }
            break;
        }
```

`%p` 不走通用填充逻辑，硬编码为 `"0x"` + 16 位十六进制。指针输出固定宽度 18 字符，适合对齐显示。`%p` 不支持宽度和左对齐修饰符。

### kprintf.cpp —— 重构后的薄包装层

重构后的 `kernel/lib/kprintf.cpp` 变得极其简洁：

```cpp
// kernel/lib/kprintf.cpp

#include "kernel/lib/kprintf.hpp"
#include <stdarg.h>
#include <stdint.h>
#include "kernel/drivers/serial.hpp"
#include "kernel/lib/private/vkprintf_impl.hpp"

namespace {

using cinux::drivers::Serial;
using cinux::drivers::SERIAL_COM1;
using cinux::lib::detail::vkprintf_impl;

static Serial g_serial(SERIAL_COM1);

}  // anonymous namespace

namespace cinux::lib {

void kprintf_init() {
    g_serial.init();
}

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
}

void kvprintf(const char* fmt, va_list args) {
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
}

void kpanic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) { g_serial.putc(c); }, fmt, args);
    va_end(args);
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

}  // namespace cinux::lib
```

整个文件从两百多行缩减到不到 50 行有效代码。lambda `[&](char c) { g_serial.putc(c); }` 捕获 `g_serial` 的引用，每收到一个字符就调用 `Serial::putc()` 发出去。`kpanic` 输出完成后进入 `cli; hlt` 死循环。

### 单元测试的彻底重写

重构前测试复制了格式化代码——测试的是复制品而非真正的引擎。重构后直接 include 生产引擎：

```cpp
// tests/unit/test_kprintf.cpp

#include <string>
#include <stdarg.h>
#include "lib/private/vkprintf_impl.hpp"

using namespace cinux::lib::detail;

class MockOutput {
public:
    void putc(char c) { buffer_.push_back(c); }
    std::string result() const { return buffer_; }
    void        clear() { buffer_.clear(); }
private:
    std::string buffer_;
};

static std::string do_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    MockOutput mock;
    vkprintf_impl([&](char c) { mock.putc(c); }, fmt, args);
    va_end(args);
    return mock.result();
}
```

`MockOutput` 用 `std::string` 捕获输出。`do_printf` 调用真正的 `vkprintf_impl`，输出重定向到 mock 对象，返回结果字符串。测试用例极其简洁：

```cpp
ASSERT_EQ(do_printf("%d", 42), "42");
ASSERT_EQ(do_printf("[%06d]", -42), "[-00042]");
ASSERT_EQ(do_printf("[%-10d]", 42), "[42        ]");
ASSERT_EQ(do_printf("[%-10s]", "hi"), "[hi        ]");
```

35 个测试用例覆盖所有格式说明符、宽度修饰符、左对齐、负数零填充、`nullptr`、空字符串、混合格式、未知说明符回退等。

### kernel_main 中的格式化回归测试

除了主机端的单元测试，我们还在大内核的 `kernel_main()` 中加入了一组格式化回归测试：

```cpp
// kernel/main.cpp

// -- kprintf format regression test (after serial + GDT + IDT are up) --
cinux::lib::kprintf("[KPRINTF] %%d: %d\n", 42);
cinux::lib::kprintf("[KPRINTF] %%d negative: %d\n", -123);
cinux::lib::kprintf("[KPRINTF] %%u: %u\n", 4294967295u);
cinux::lib::kprintf("[KPRINTF] %%x: %x\n", 0xDEADBEEFu);
cinux::lib::kprintf("[KPRINTF] %%X: %X\n", 0xDEADBEEFu);
cinux::lib::kprintf("[KPRINTF] %%08x: %08x\n", 0xDEADu);
cinux::lib::kprintf("[KPRINTF] %%10d: %10d\n", 42);
cinux::lib::kprintf("[KPRINTF] %%-10d: %-10d|\n", 42);
cinux::lib::kprintf("[KPRINTF] %%s: %s\n", "hello");
cinux::lib::kprintf("[KPRINTF] %%-10s: %-10s|\n", "hi");
cinux::lib::kprintf("[KPRINTF] %%p: %p\n", (void*)0x1234ABCD5678ull);
cinux::lib::kprintf("[KPRINTF] %%c: %c\n", 'Z');
cinux::lib::kprintf("[KPRINTF] %%%%: %%\n");
cinux::lib::kprintf("[KPRINTF] %%010u: %010u\n", 42u);
cinux::lib::kprintf("[KPRINTF] mix: %s n=%d hex=%08x ptr=%p\n",
                    "test", 99, 0xCAFEBABEu, (void*)0x1ull);
cinux::lib::kprintf("[KPRINTF] all format tests done.\n");
```

这些测试运行在真正的内核环境里，验证端到端数据通路（vkprintf_impl -> Serial::putc -> io_outb -> QEMU UART）。和主机端单元测试互补：单元测试验证格式化逻辑，内核回归测试验证格式化引擎与硬件输出的集成。

## 设计决策

### Decision: header-only 模板 vs 编译单元分离

**问题**: 格式化引擎应该放在 `.cpp` 编译单元还是 header-only 模板中？

**本项目的做法**: header-only 模板——`vkprintf_impl.hpp` 包含完整的实现，所有函数用 `inline` 修饰。测试文件只需 `#include` 一个头文件即可使用，不需要额外的编译配置。

**备选方案**: 把 `format_decimal`/`format_hex` 放在独立 `.cpp` 中编译，或用显式模板实例化。header-only 更简单，`inline` 函数在现代编译器中不会有性能问题。

### Decision: OutputFn 模板参数 vs 虚函数接口

**问题**: 如何将格式化引擎与输出设备解耦？

**本项目的做法**: 函数模板 `template <typename OutputFn>`，接受任何可调用对象。

**备选方案**: 定义抽象基类 `class OutputDevice { virtual void putc(char) = 0; }`。虚函数需要 vtable，有运行时开销。内核启动早期的全局对象可能在 vtable 还没设置好之前就被使用。C++ 模板提供零开销抽象——lambda 编译期内联后和直接调用一样高效。

### Decision: 两个 vkprintf_impl 文件 vs 统一一个

**问题**: 为什么大内核和迷你内核各有一个 `vkprintf_impl` 文件？

**本项目的做法**: 大内核用 `kernel/lib/private/vkprintf_impl.hpp`（自包含，`inline` 函数），迷你内核用 `kernel/mini/lib/private/vkprintf_impl.h`（依赖 `format.h`，支持 `%b` 二进制格式）。两个内核有不同的需求，强行统一会增加不必要的耦合。

## 扩展方向

- **添加 %lld / %zu 长度修饰符**: 解析 `%ll` 前缀并在 `va_arg` 时使用正确的类型。（难度：⭐⭐）
- **精度修饰符 %.Ns**: 支持 `%.5s`、`%.3d`，向标准 printf 看齐。（难度：⭐⭐）
- **统一的格式化引擎**: 合并大内核和迷你内核的 `vkprintf_impl`，消除最后一点代码重复。（难度：⭐⭐）
- **缓冲输出**: 行缓冲——积累字符到 `\n` 再 flush，减少 I/O 操作次数。（难度：⭐⭐）
- **%f 浮点格式化**: 用整数运算模拟浮点转换，参考 Linux 内核的 `vsnprintf`。（难度：⭐⭐⭐）

## 参考资料

- OSDev Wiki: [Kernel printf](https://wiki.osdev.org/Printing_To_Screen) — 内核格式化输出的一般指导，格式说明符解析，变参处理
- Linux `lib/vsprintf.c` — Linux 内核的完整 `vsnprintf` 实现，支持几乎所有标准 printf 格式，以及 `%p` 扩展（`%pI4` IP 地址、`%pE` 转义字符串等）
- xv6 `printf.c` — 极简的内核 printf，用单一 `printint()` 函数配合 base 参数处理所有进制
