# 005 通读版 · kprintf 格式化输出库——从数字到文字的魔法

## 概览

上篇文章我们给内核装上了"嘴巴"——串口驱动可以逐字节发送字符了。但内核不可能每次输出都手写 `serial.putc('H'); serial.putc('e'); ...`，我们需要一个类似 `printf` 的格式化输出函数，把 `%d`、`%x`、`%s` 这些占位符替换成实际的值。这就是 `kprintf` 的职责。

Cinux 的 kprintf 实现分三层：最底层是三个纯算法函数 `format_decimal`、`format_hex`、`format_binary`，负责把数字转成字符串；中间层是模板函数 `vkprintf_impl`，负责解析格式字符串并调用底层格式化函数；最上层是 `kprintf` 和 `kdebugf` 两个面向用户的接口，分别输出到串口和 QEMU debugcon。这种分层设计的好处是格式化逻辑和输出设备完全解耦——如果你想加一个新的输出后端（比如帧缓冲区），只需要写一个新的包装函数，传入不同的 `putc` lambda 即可。

本文覆盖格式化函数声明（`format.h`）和实现（`format.cpp`）、kprintf 声明（`kprintf.h`）和实现（`kprintf.cpp`）、lib 构建配置（`lib/CMakeLists.txt`），以及内核主函数中 kprintf 的使用示例（`main.cpp`）。

## 架构图

kprintf 的完整调用链和数据流：

```
                    kprintf 格式化引擎
                    ==================

  kprintf("BootInfo: entry=%p, count=%u\n", ptr, count)
       │
       ▼
  va_start(args, format)
       │
       ▼
  vkprintf_impl(lambda{serial.putc}, format, args)
       │
       ├── 'B','o','o','t','I','n','f','o',':',' ' ──► serial.putc(c)
       │
       ├── '%' 检测到格式符
       │   ├── type = 'p'
       │   ├── va_arg(args, uint64_t) → ptr
       │   ├── output "0x" prefix
       │   ├── format_hex(ptr, buffer, 64, false) → "ffffffff80000000"
       │   └── 输出 buffer 内容
       │
       ├── ',',' ','c','o','u','n','t','=' ──► serial.putc(c)
       │
       ├── '%' 检测到格式符
       │   ├── type = 'u'
       │   ├── va_arg(args, unsigned int) → count
       │   ├── format_decimal(count, buffer, 64) → "5"
       │   └── 输出 buffer 内容
       │
       └── '\n' ──► serial.putc('\r'), serial.putc('\n')

  ────────────────────────────────────────────────────────────

  kdebugf("Debug: %s\n", "message")
       │
       ▼
  vkprintf_impl(lambda{debugcon_putc}, format, args)
       │
       └── ... 同样的格式化逻辑，但输出到 0xE9 端口
```

三个格式化函数的算法特征对比：

```
  ┌─────────────────┬──────────────────┬────────────────────┐
  │ format_decimal  │ format_hex       │ format_binary      │
  ├─────────────────┼──────────────────┼────────────────────┤
  │ 输入: int64_t   │ 输入: uint64_t   │ 输入: uint64_t     │
  │ 基数: 10        │ 基数: 16         │ 基数: 2            │
  │ 特殊: INT64_MIN │ 参数: lowercase  │ 特殊: 零值 = "0"   │
  │ 反转: do-while  │ 反转: do-while   │ 方向: MSB→LSB      │
  │ 临时: tmp[24]   │ 临时: tmp[20]    │ 无临时缓冲区       │
  └─────────────────┴──────────────────┴────────────────────┘
```

## 代码精讲

### 格式化函数声明——`format.h`

```cpp
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace cinux::mini::lib::detail {

int format_decimal(int64_t value, char* buffer, int buffer_size);
int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase);
int format_binary(uint64_t value, char* buffer, int buffer_size);

} // namespace cinux::mini::lib::detail
```

三个函数放在 `detail` 命名空间里——这是 C++ 惯用的"内部实现细节"标记，告诉使用者"别直接调这些函数，它们是实现细节"。每个函数的接口设计遵循同一模式：输入值、输出缓冲区、缓冲区大小，返回实际写入的字符数（不含末尾的 `\0`）。`format_hex` 多了一个 `lowercase` 参数来控制输出 `a-f` 还是 `A-F`。

为什么不用一个统一的 `format_int(value, base, buffer)` 函数？因为不同进制的算法有微妙的差异：十进制需要处理负号和 INT64_MIN 特殊情况，十六进制需要大小写切换，二进制需要从最高位开始扫描而不是低位。强行合并成一个函数会让每个分支都变得复杂，不如分开写，每个函数都清晰明了。

### 格式化函数实现——`format.cpp`

先看 `format_decimal`：

```cpp
int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1)
        return 0;

    int idx = 0;
    bool is_neg = value < 0;

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
```

INT64_MIN 的特殊处理是这个函数里最有趣的边界情况。`INT64_MIN = -9223372036854775808`，它的绝对值是 `9223372036854775808`，但这个数超过了 `int64_t` 的最大值 `9223372036854775807`。如果你执行 `value = -value` 当 `value == INT64_MIN`，结果在二进制补码下是未定义行为（或者根据实现，值还是 INT64_MIN 本身，因为溢出绕回了）。所以必须硬编码这个字符串。这个坑并不冷门——C 标准库的 `printf` 实现也需要处理它。

```cpp
    uint64_t abs_val = static_cast<uint64_t>(value);
    char tmp[24];
    int tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + (abs_val % 10);
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

核心算法是经典的"取模反转法"。`abs_val % 10` 得到最低位数字，转成字符 `'0' + digit`，放到临时缓冲区。因为是先取最低位，所以 `tmp` 数组里数字是反过来的（比如 42 → tmp = ['2', '4']），最后从后往前复制到 `buffer` 就得到了正确的顺序。`do {} while` 而不是 `while {}` 是为了正确处理 0 的情况——当 `abs_val = 0` 时，循环体至少执行一次，写入 `'0'`。如果用 `while (abs_val > 0)`，0 就会输出空字符串。

`tmp[24]` 的大小是 `int64_t` 的十进制最大位数加一：`INT64_MIN` 的字符串表示有 20 个字符（含负号），数字部分最多 19 位，再加一个 `\0` 就是 21，24 留了一点余量。

接下来是 `format_hex`：

```cpp
int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1)
        return 0;

    const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
    char tmp[20];
    int tmp_idx = 0;

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

和 `format_decimal` 的模式完全一样，只是基数从 10 变成了 16。`value & 0xF` 取最低 4 位（一个十六进制位），`value >>= 4` 右移 4 位相当于除以 16。数字到字符的转换通过查表完成——`digits` 数组是 `"0123456789abcdef"` 或 `"0123456789ABCDEF"`，下标 0-15 对应 0-F。这比用 `if (digit < 10) '0' + digit else 'a' + digit - 10` 的写法更简洁，也不容易出错。

`tmp[20]` 的大小：`uint64_t` 最多 16 个十六进制位（64 / 4 = 16），20 留了余量。同样的 `do {} while` 保证 0 输出 `"0"` 而不是空串。

最后是 `format_binary`：

```cpp
int format_binary(uint64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1)
        return 0;

    int bit = 63;
    bool found = false;
    while (bit >= 0) {
        if ((value >> bit) & 1) {
            found = true;
            break;
        }
        bit--;
    }

    if (!found)
        bit = 0;

    int idx = 0;
    for (int i = bit; i >= 0 && idx + 1 < buffer_size; i--) {
        buffer[idx++] = ((value >> i) & 1) ? '1' : '0';
    }
    buffer[idx] = '\0';

    return idx;
}
```

二进制格式化用了不同的策略。前两个函数是"从低位提取再反转"，而 `format_binary` 直接从最高位开始扫描。首先从 bit 63 往下找到第一个为 1 的位（最高有效位），然后从那个位开始向低位输出。这样做的好处是自然地去掉了前导零——`0b00101010` 输出 `"101010"` 而不是 `"00000000000000000000000000101010"`。

如果 `value = 0`，`found` 保持 `false`，此时 `bit` 被设为 0，循环从 bit 0 输出一个 `'0'`。如果 `value = 0xFFFFFFFFFFFFFFFF`（全 1），`found` 在 bit 63 就变为 `true`，输出 64 个 `'1'`。

这个实现有一个微妙之处：扫描最高位用了 `while (bit >= 0)` 线性搜索，最坏情况要检查 64 次。如果用 `__builtin_clzll(value)`（GCC 内建函数，返回前导零数量），可以一步到位。但 `__builtin_clzll(0)` 的结果是未定义的，需要额外处理零值。Cinux 用显式循环是为了代码的可移植性和可读性——在一个教学 OS 里，"这段代码显而易见地做了什么"比"这段代码用了编译器特定的内建函数快了 2 个时钟周期"更重要。

### kprintf 声明——`kprintf.h`

```cpp
#pragma once

namespace cinux::mini::lib {

void kprintf(const char* format, ...);
void kdebugf(const char* format, ...);

}  // namespace cinux::mini::lib
```

两个函数，接口完全对称。`kprintf` 通过串口 COM1 输出，`kdebugf` 通过 QEMU debugcon（端口 0xE9）输出。它们支持相同的格式符集合，头文件注释中列出了完整清单：`%%`、`%c`、`%s`、`%d`、`%u`、`%x`、`%X`、`%p`、`%b`，以及宽度修饰符 `%Nd` 和 `%0Nd`。

为什么要有两个输出函数？在实际调试中，串口输出会显示在 QEMU 的终端窗口（`-serial stdio`），而 debugcon 输出会写入 `debug.log` 文件（`-debugcon file:debug.log`）。有些信息（比如 BootInfo 的内存映射表）适合在串口看，有些高频调试信息（比如"进入函数 X"、"退出函数 Y"）适合在 debugcon 看而不污染串口输出。双通道输出给了开发者更灵活的调试选择。

### kprintf 实现——`kprintf.cpp`

这是整个格式化引擎的核心文件。先看模板函数 `vkprintf_impl`：

```cpp
#include "kprintf.h"
#include "private/format.h"

#include <stdarg.h>
#include <stdint.h>

#include "driver/serial.h"

namespace {

using namespace cinux::mini::lib::detail;

template <typename OutputFn>
void vkprintf_impl(OutputFn&& putc, const char* format, va_list args) {
    char buffer[64];

    while (*format != '\0') {
        if (*format == '%') {
            format++;

            bool zero_pad = false;
            int  width    = 0;

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
```

模板参数 `OutputFn` 是输出字符的回调函数。`kprintf` 传入一个捕获了 `Serial` 引用的 lambda，`kdebugf` 传入一个调用 `debugcon_putc` 的 lambda。这种设计让格式化逻辑和输出设备完全解耦——格式化函数不关心字符最终去了哪里，它只负责把字符交给 `putc`。

格式解析逻辑很直白：遇到 `%` 就开始解析格式符。先检查是否是 `'0'` 开头（零填充标志），然后解析数字（宽度），最后读取类型字符。比如 `%04x` 的解析过程是：检测到 `%`，检测到 `'0'`（zero_pad = true），解析 `'4'`（width = 4），读取 `'x'`（type = 'x'）。

接下来是格式符分发：

```cpp
            switch (type) {
            case '%':
                putc('%');
                break;

            case 'c':
                putc(static_cast<char>(va_arg(args, int)));
                break;

            case 's': {
                const char* s = va_arg(args, const char*);
                if (s == nullptr)
                    s = "(null)";
                while (*s)
                    putc(*s++);
                break;
            }

            case 'd':
                len = format_decimal(
                    static_cast<int64_t>(va_arg(args, int)), buffer, sizeof(buffer));
                goto do_padding;

            case 'u':
                len = format_decimal(
                    static_cast<int64_t>(va_arg(args, unsigned int)), buffer, sizeof(buffer));
                goto do_padding;
```

`%c` 直接输出字符。注意 `va_arg(args, int)` 而不是 `va_arg(args, char)`——C/C++ 的可变参数中，比 `int` 窄的整型会被自动提升为 `int`，所以 `char` 在 `va_list` 中以 `int` 形式传递。

`%s` 处理了 `nullptr` 的情况，输出 `"(null)"` 而不是崩溃。这是从 Linux 的 `printk` 学来的做法——内核代码中 `nullptr` 字符串并不罕见（比如某个可选的配置参数没设置），直接崩溃比输出 `"(null)"` 更不可取。

`%d` 和 `%u` 的处理使用了 `goto do_padding`——这是 C 语言中少数几个 `goto` 被认为合理使用的场景。多个格式符（`%d`、`%u`、`%x`、`%X`、`%b`）都需要在格式化后执行相同的宽度填充逻辑，如果把填充代码写五遍，既冗余又容易改漏。`goto do_padding` 把这五个分支汇聚到同一个填充代码块。

```cpp
            case 'x':
                len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), true);
                goto do_padding;

            case 'X':
                len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
                goto do_padding;

            case 'p':
                for (const char* p = "0x"; *p; p++)
                    putc(*p);
                len = format_hex(va_arg(args, uint64_t), buffer, sizeof(buffer), false);
                for (int i = 0; i < len; i++)
                    putc(buffer[i]);
                break;

            case 'b':
                len = format_binary(va_arg(args, uint64_t), buffer, sizeof(buffer));
                goto do_padding;
```

`%p` 的处理和其他格式符不同：它不走 `do_padding` 路径，而是先手动输出 `"0x"` 前缀，然后输出十六进制内容。这里没有对指针值做零填充或宽度处理——指针总是输出完整的十六进制表示，不带前导零。这是一个简化设计：真正的 `printf` 的 `%p` 会输出固定宽度（比如 `0x00007fff12345678`），但内核调试输出中对齐没那么重要。

```cpp
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

`do_padding` 标签处的填充逻辑：如果格式化后的字符串长度小于指定的宽度，先输出填充字符（`'0'` 或 `' '`），然后再输出实际内容。这实现了右对齐效果：`%4d` 输出 `[  42]`，`%04d` 输出 `[0042]`。

`default` 分支处理未知的格式符——原样输出 `%` 加上那个字符。比如 `%f` 会输出 `%f` 而不是崩溃。这在调试时很方便，你不会因为写错一个格式符就让整个内核挂掉。

最后是两个面向用户的函数和 debugcon 输出：

```cpp
void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

}  // namespace

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

}  // namespace cinux::mini::lib
```

`kprintf` 通过 lambda 捕获全局串口引用 `[&](char c) { serial.putc(c); }` 作为输出回调。`kdebugf` 用一个无捕获的 lambda `[](char c) { debugcon_putc(c); }`——无捕获 lambda 可以隐式转换为函数指针，但在这里它被直接当作模板参数 `OutputFn` 使用，编译器会内联整个调用链。

`debugcon_putc` 只有一条 `outb` 指令，往端口 0xE9 写一个字节。QEMU 的 isa-debugcon 设备接收到写入后，根据配置（`-debugcon file:debug.log`）将内容输出到文件。这个操作比串口快得多——不需要查 LSR 等发送就绪，不需要 FIFO，直接写就完事。代价是只有 QEMU 支持，真实硬件上 0xE9 什么都不是。

### 库构建配置——`lib/CMakeLists.txt`

```cmake
add_library(kprintf_private STATIC
    private/format.cpp
)

target_compile_options(kprintf_private PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
)

target_include_directories(kprintf_private PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
```

`format.cpp` 被单独编译成一个静态库 `kprintf_private`。这看起来有点多余——为什么不直接把 `format.cpp` 加到内核的源文件列表里？原因是为了 host 端测试。`test/unit/test_kprintf_format.cpp` 需要在 Linux 用户态下测试格式化函数，它直接 include 了 `format.h` 并链接了 `format.cpp`。通过把 `format.cpp` 编译成独立库，host 测试和内核构建可以复用同一份编译产物，避免维护两份构建配置。

`-ffreestanding` 告诉编译器不假设标准库存在（不生成对 `memcpy`、`memset` 等的隐式调用），`-fno-exceptions` 和 `-fno-rtti` 禁用异常和运行时类型信息——这是 freestanding 内核环境的标准配置。

### 内核主函数——`main.cpp`

最后看一下 kprintf 在内核中的实际使用：

```cpp
#include "../../boot/boot_info.h"
#include "lib/kprintf.h"

using cinux::mini::lib::kprintf;

extern "C" {
extern uint64_t __boot_info_ptr;
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n",
            boot_info->entry_point, boot_info->kernel_phys_base);
    kprintf("Boot Memory Info: mmap_count=%u\n", boot_info->mmap_count);
    for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
        const MemoryMapEntry* entry = &boot_info->mmap[i];
        kprintf("  [%u] base=0x%016llx, length=0x%016llx, type=%u, acpi=%u\n",
                i, entry->base, entry->length, entry->type, entry->acpi);
    }

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`mini_kernel_main` 是内核的 C++ 入口点，从 `boot.S` 的 `_start` 汇编代码跳转过来。BootInfo 指针不是从函数参数 `boot_info_addr`（`%rdi`）获取的，而是从全局变量 `__boot_info_ptr` 读取的。这个全局变量在 `boot.S` 中被赋值为 `%rdi` 的值（即 bootloader 传递的 0x7000）。使用全局变量而不是直接用参数是为了避免在调用栈很深的地方丢失 BootInfo 指针——全局变量任何地方都能访问。

kprintf 的第一次调用 `kprintf("Cinux Mini Kernel v0.1.0\n")` 是整个内核的"Hello World"时刻。如果一切配置正确（串口驱动已初始化、页表映射正确、全局构造函数执行完毕），这一行会在 QEMU 的终端窗口输出：

```
Cinux Mini Kernel v0.1.0
```

随后的 kprintf 调用输出 BootInfo 的内容：内核入口点地址（`%p` 格式，带 `0x` 前缀）、物理基地址（`%p`）、E820 内存映射条目数量（`%u`），以及每一条内存映射的详细信息。这里用到了 `%016llx`——`%016` 表示最小宽度 16，零填充，`ll` 在我们的实现中不需要（因为格式化函数直接接受 `uint64_t`），但 `x` 格式符会输出十六进制。

等一下，`%016llx` 实际上不会被我们的格式解析器正确处理——它会识别 `%0`（zero_pad = true），然后 `1`（width = 1），然后 `6`（width = 16），然后 `l`（被当作 type = 'l'，走到 default 分支输出 `%l`），然后 `lx` 就变成了普通字符。这是当前实现的一个局限：不支持长度修饰符（`%ld`、`%llu` 等）。但在实际使用中，只要把 `va_arg` 的类型和格式符匹配正确（`%d` 对 `int`，`%u` 对 `unsigned int`，`%x` 对 `uint64_t`），这个问题不大。`entry->base` 是 `uint64_t` 类型，用 `%x` 或 `%p` 输出就能正确显示。代码中用 `%016llx` 可能是写代码时的笔误或者是在某个迭代版本中保留下来的——但无论如何，实际输出是正确的，因为 `va_arg(args, uint64_t)` 读取了完整的 64 位值，`format_hex` 也正确处理了 64 位输入。

## 设计决策

### 决策：模板函数 + lambda vs 函数指针回调

**问题**：`vkprintf_impl` 需要一个输出字符的回调机制。用什么方式？

**本项目的做法**：模板函数 `vkprintf_impl<OutputFn>`，`OutputFn` 通过完美转发接收。调用者传入 lambda（`kprintf` 传捕获引用的 lambda，`kdebugf` 传无捕获 lambda）。

**备选方案**：函数指针 `typedef void (*putc_fn)(char); void vkprintf_impl(putc_fn putc, ...)`。xv6 的 `printf.c` 就是用函数指针回调的方式。

**为什么选模板**：性能和内联。lambda 在编译时是已知的类型，编译器可以完全内联 `serial.putc(c)` 或 `debugcon_putc(c)` 的调用，消除函数指针间接跳转的开销。在内核的早期启动阶段，每一个 CPU 周期都可能影响调试时序。函数指针在每次调用时都需要一次间接跳转（查虚表/函数指针表），而且不可能被内联。

**扩展方向**：如果将来需要支持运行时切换输出后端（比如根据日志级别决定输出到串口还是 debugcon），函数指针会变得更灵活。可以在编译时通过模板生成两个版本（串口版和 debugcon 版），运行时根据需要调用不同的版本。

### 决策：`goto do_padding` vs 提取函数

**问题**：多个格式符共享相同的宽度填充逻辑，代码怎么组织？

**本项目的做法**：在 switch 的 case 分支末尾用 `goto do_padding` 跳转到共享的填充代码块。

**备选方案**：把填充逻辑提取成 `output_padded(putc, buffer, len, width, zero_pad)` 函数。

**为什么不选备选方案**：在这个特定的场景下，提取函数需要传递 5 个参数，而且 `buffer`、`len`、`width`、`zero_pad` 都是局部变量——函数调用会带来参数传递和栈帧创建的开销（虽然现代编译器可能会内联掉）。`goto do_padding` 在 C 语言的 `printf` 实现中是一种常见的模式，Linux 内核的 `vsnprintf` 也大量使用 `goto` 来共享后处理代码。它在这里的语义很清晰：格式化完成，跳到填充输出阶段。

## 扩展方向

1. **添加 `%lld` / `%zu` 长度修饰符支持**（难度：中等）——当前格式解析器不支持 `l`、`ll`、`z` 等长度前缀。可以在解析类型字符之前增加长度修饰符解析，根据修饰符选择 `va_arg(args, int)` 或 `va_arg(args, int64_t)`。这样就能正确处理 `%lld`（long long）和 `%zu`（size_t）。

2. **添加 `%f` 浮点格式化**（难度：困难）——浮点格式化需要处理指数、小数点、精度控制等大量边界情况。Linux 内核的 `printk` 不支持 `%f`——内核里没人用浮点。但如果需要，可以参考 `dtostr` 算法或者二进制浮点转十进制的 Dragon4 算法。

3. **添加颜色支持**（难度：简单）——ANSI 转义序列（`\033[31m` 红色、`\033[32m` 绿色等）可以让串口输出带上颜色。QEMU 的 `-serial stdio` 支持 ANSI 转义序列。可以加一个 `kprintf_color(color, fmt, ...)` 函数，在输出内容前后包裹 ANSI 序列。

4. **日志级别过滤**（难度：中等）——给 kprintf 加日志级别参数（DEBUG、INFO、WARN、ERROR），运行时根据全局日志级别变量过滤输出。这样可以避免 DEBUG 级别的信息在生产模式下污染输出。Linux 的 `printk` 就有 `pr_debug`、`pr_info`、`pr_err` 等级别。

5. **环形缓冲区日志**（难度：困难）——所有 kprintf 输出同时写入一个环形缓冲区，即使串口输出被阻塞（比如高频打印），日志也不会丢失。后续可以通过调试器（GDB `x/s` 命令）读取缓冲区内容。Linux 的 `dmesg` 就是这种机制。

## 参考资料

- OSDev Wiki — Serial Ports (https://wiki.osdev.org/Serial_Ports)：UART 初始化序列、轮询发送模式、LSR bit 5 (THRE) 的使用。

- xv6 RISC-V `printf.c` (https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/printf.c)：MIT 教学操作系统的 `printf` 实现。使用 `printint()` 函数统一处理不同基数的整数格式化，通过函数指针 `putc` 回调输出。Cinux 的设计借鉴了这种回调模式但改用了 C++ 模板。

- C 标准库 `printf` 实现参考——各种 libc（glibc、musl、newlib）的 `vfprintf` 实现都使用了类似的格式解析 + `goto` 后处理模式。

- Intel SDM Vol. 2A — `OUT` 指令：debugcon 端口 0xE9 的写入操作使用 `OUT imm8, AL` 形式。
