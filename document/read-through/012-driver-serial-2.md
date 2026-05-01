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

逻辑和 `format_decimal` 类似，只是基数从 10 换成了 16，每一位通过 `value & 0xF` 取出。`lowercase` 参数控制十六进制字母的大小写——`%x` 用小写 `a-f`，`%X` 用大写 `A-F`。`tmp` 数组最多 20 个元素（64 位最多 16 个十六进制位，留了一些余量）。

这两个函数用 `inline` 修饰，是因为它们定义在头文件中且需要被多个编译单元包含。没有 `inline` 的话，如果两个 `.cpp` 文件都 include 了这个头文件，链接时会出现 ODR（One Definition Rule）冲突。

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

模板参数 `OutputFn` 是整个设计的精髓——它可以是 lambda、函数指针、或者任何可调用对象，只要接受一个 `char` 参数就行。这意味着同样的格式化引擎可以输出到串口、输出到 QEMU debugcon、或者输出到测试里的 mock 缓冲区。`OutputFn&&` 是万能引用（forwarding reference），既能接受左值也能接受右值。

主循环逐字符扫描格式字符串，普通字符直接输出，遇到 `%` 进入格式说明符解析。

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

`%s` 的处理有两个重点。第一是 `nullptr` 安全——如果传入空指针，输出 `"(null)"` 而不是崩溃。这是 Linux 内核的做法，比直接 segfault 友好太多了。第二是宽度与对齐：先测量字符串长度 `slen`，然后根据 `left_align` 决定是先输出字符串再补空格（左对齐），还是先补空格再输出字符串（右对齐）。

举个例子，`%-10s` 格式化 `"hi"` 会输出 `"hi        "`（先 hi 再 8 个空格），而 `%10s` 格式化 `"hi"` 会输出 `"        hi"`（先 8 个空格再 hi）。

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

`%d` 是整个格式化引擎里最复杂的部分，因为负数加零填充的组合需要特殊处理。想想看，`%06d` 格式化 `-42` 应该输出什么？如果是简单的"先补零再输出内容"，你会得到 `000-42`——负号跑到中间去了，这显然是错误的。正确的输出应该是 `-00042`，即负号在最前面，然后是前导零，最后是数字。

代码用三个分支处理所有情况。第一个分支 `!left_align && zero_pad && has_sign` 处理"右对齐 + 零填充 + 有符号"的情况——先输出负号，再补零，再输出数字部分。注意这里 `width - 1` 是因为负号已经占了一个字符位。第二个分支 `!left_align` 处理右对齐的一般情况——根据 `zero_pad` 选填充字符，先补填充再输出完整内容。第三个分支处理左对齐——先输出内容再补空格。

`va_arg(args, int)` 取出的参数被强制转换成 `int64_t` 传给 `format_decimal`，这是因为我们的格式化引擎内部统一使用 64 位运算。

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

`%u`、`%x`、`%X` 的处理逻辑非常统一，因为它们都不涉及负数符号问题。对于右对齐，先补填充字符再输出内容；对于左对齐，先输出内容再补空格。`%u` 复用 `format_decimal`（传入的是无符号值所以永远不会是负数），`%x` 和 `%X` 调用 `format_hex`，区别只在 `lowercase` 参数。

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

`%p` 比较特殊——它不走通用的填充逻辑，而是硬编码为 `"0x"` + 恰好 16 位十六进制数字。这使得指针输出总是固定宽度 18 字符（`0x` + 16 位），非常适合对齐显示。先用 `format_hex` 把 64 位地址转成十六进制字符串，然后补前导零到 16 位，最后输出实际数字。`%p` 不支持宽度修饰符和左对齐——在 64 位系统上 16 位十六进制已经是固定格式了。

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

整个文件从之前的两百多行缩减到不到 50 行有效代码。所有的格式化逻辑都在 `vkprintf_impl.hpp` 里，这里只做两件事：提供一个全局的 `Serial` 单例，以及把 `vkprintf_impl` 的输出通过 lambda 转发给串口驱动。

lambda `[&](char c) { g_serial.putc(c); }` 就是我们说的"输出适配器"——它捕获 `g_serial` 的引用，每收到一个字符就调用 `Serial::putc()` 发送出去。`kpanic` 在输出完成后进入死循环禁中断暂停，保证系统停住而不是继续执行到更糟糕的状态。

### 单元测试的彻底重写

重构带来的最大好处之一就是测试可以真正测试"生产代码"了。之前 `tests/unit/test_kprintf.cpp` 需要把 `format_decimal`、`format_hex` 和格式解析逻辑全部复制一遍——测试的是复制品而不是真正的引擎。重构后：

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

`MockOutput` 类用 `std::string` 捕获输出字符。`do_printf` 是一个简洁的测试辅助函数——调用 `vkprintf_impl`（真正的大内核格式化引擎），把输出重定向到 mock 对象，然后返回结果字符串。测试用例变成了极其简洁的一行断言：

```cpp
TEST("kprintf: %d positive") {
    ASSERT_EQ(do_printf("%d", 42), "42");
}

TEST("kprintf: %06d negative zero-pad") {
    ASSERT_EQ(do_printf("[%06d]", -42), "[-00042]");
}

TEST("kprintf: %-10d left-align decimal") {
    ASSERT_EQ(do_printf("[%-10d]", 42), "[42        ]");
}

TEST("kprintf: %s with width left-align") {
    ASSERT_EQ(do_printf("[%-10s]", "hi"), "[hi        ]");
}
```

测试覆盖了所有格式说明符（`%d`、`%u`、`%x`、`%X`、`%s`、`%p`、`%c`、`%%`）、宽度修饰符（`%08x`、`%4d`）、左对齐（`%-10d`、`%-10s`）、负数零填充（`%06d` with -42）、`nullptr` 字符串、空字符串、混合格式、未知说明符回退、以及空格式字符串等边界情况——总共 35 个测试用例，全部直接运行真正的 `vkprintf_impl` 模板。

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

这些运行在真正的内核环境里——通过 QEMU 串口输出——可以验证整个数据通路（vkprintf_impl -> Serial::putc -> io_outb -> QEMU UART）端到端的工作。和主机端的单元测试互补：单元测试验证格式化逻辑的正确性，内核回归测试验证格式化引擎与硬件输出的集成。每次内核启动都会跑一遍这 15 个测试用例，如果某个格式化行为出了问题，串口输出会立刻暴露。

## 设计决策

### Decision: header-only 模板 vs 编译单元分离

**问题**: 格式化引擎应该放在 `.cpp` 编译单元还是 header-only 模板中？

**本项目的做法**: header-only 模板——`vkprintf_impl.hpp` 包含完整的实现，所有函数用 `inline` 修饰。

**备选方案**: 把 `format_decimal` 和 `format_hex` 放在独立的 `.cpp` 文件中编译，只把 `vkprintf_impl` 模板放在头文件里。或者用显式模板实例化（explicit template instantiation）来控制编译单元。

**为什么不选备选方案**: header-only 是最简单的方案——测试文件只需要 `#include` 一个头文件就能直接使用格式化引擎，不需要额外的编译配置和链接步骤。在内核项目中，减少构建复杂度本身就有价值。`inline` 函数在现代编译器中不会产生性能问题，编译器会根据需要决定是否真正内联。

### Decision: OutputFn 模板参数 vs 虚函数接口

**问题**: 如何将格式化引擎与输出设备解耦？

**本项目的做法**: 函数模板 `template <typename OutputFn>`，接受任何可调用对象（lambda、函数指针等）。

**备选方案**: 定义一个抽象基类 `class OutputDevice { virtual void putc(char) = 0; }`，格式化引擎接受 `OutputDevice&`。这是典型的面向对象设计方案。

**为什么不选备选方案**: 虚函数需要 vtable，而内核启动早期的全局对象可能在 vtable 还没设置好之前就被使用。更重要的是，虚函数有运行时开销（间接调用），而我们追求的是零开销抽象——lambda 在编译期内联后和直接调用一样高效。C++ 模板在这里提供了比虚函数更好的方案。

### Decision: 两个 vkprintf_impl 文件 vs 统一一个

**问题**: 为什么大内核和迷你内核各有一个 `vkprintf_impl` 文件，而不是共享同一份？

**本项目的做法**: 大内核用 `kernel/lib/private/vkprintf_impl.hpp`（自包含，内部定义 `format_decimal`/`format_hex`），迷你内核用 `kernel/mini/lib/private/vkprintf_impl.h`（依赖 `format.h` 中的函数声明，支持 `%b` 二进制格式）。

**备选方案**: 两个内核共享完全相同的 `vkprintf_impl` 头文件。

**为什么不选备选方案**: 两个内核有不同的需求——迷你内核需要 `%b`（二进制格式化）且使用外部链接的 `format_decimal`/`format_hex`/`format_binary`（定义在 `format.cpp` 中），大内核只需要标准的格式说明符且使用 `inline` 函数。强行统一会增加不必要的耦合。不过长期来看，如果两个引擎的差距继续缩小，合并成一个文件是值得考虑的优化。

## 扩展方向

- **添加 %lld / %zu 长度修饰符**: 目前 `%d` 固定取 `int` 宽度的参数，不支持 `long long` 或 `size_t`。解析 `%ll` 前缀并在 `va_arg` 时使用正确的类型可以扩展引擎的能力。（难度：⭐⭐）

- **精度修饰符 %.Ns**: 支持 `%.5s`（最多输出 5 个字符）和 `%.3d`（至少 3 位数字），向标准 printf 的功能看齐。（难度：⭐⭐）

- **统一的格式化引擎**: 将大内核和迷你内核的 `vkprintf_impl` 合并成一个文件，用 `#ifdef` 或模板特化处理 `%b` 等差异。消除最后一点代码重复。（难度：⭐⭐）

- **缓冲输出**: 在 `kprintf` 层面加入行缓冲——积累字符直到遇到 `\n` 才一次性 flush 到串口。这可以减少 I/O 操作次数，特别是在打印长字符串时。（难度：⭐⭐）

- **%f 浮点格式化**: 在没有 `libm` 的内核中实现简单的浮点数输出。可以参考 Linux 内核的 `vsnprintf` 实现，用整数运算模拟浮点转换。（难度：⭐⭐⭐）

## 参考资料

- OSDev Wiki: [Kernel printf](https://wiki.osdev.org/Kernel_printf) — 内核格式化输出的一般指导，格式说明符解析，变参处理
- Linux `lib/vsprintf.c` — Linux 内核的完整 `vsnprintf` 实现，支持几乎所有标准 printf 格式，以及 `%p` 扩展（`%pI4` IP 地址、`%pE` 转义字符串等）
- xv6 `printf.c` — 极简的内核 printf，用单一 `printint()` 函数配合 base 参数处理所有进制
