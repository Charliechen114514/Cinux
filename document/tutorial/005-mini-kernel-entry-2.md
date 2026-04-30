# 从数字到文字的魔法：手搓内核格式化输出

> 标签：kprintf, va_list, 可变参数, 格式化算法, 模板函数, INT64_MIN
> 前置：[005A 让内核开口说话：串口驱动](005-mini-kernel-entry-1.md)

## 写在前面

上一篇我们给内核装上了嘴巴——串口驱动可以逐字节发送字符了。但说实话，如果每次输出都要手写 `serial.putc('H'); serial.putc('e'); serial.putc('l'); serial.putc('l'); serial.putc('o');`，那调试体验比 debugcon 好不了多少。我们需要一个类似 `printf` 的格式化输出函数，写 `kprintf("Hello %s, count=%d\n", name, count)` 就能自动把变量值填进去。这就是 `kprintf` 的职责——Cinux 的内核格式化输出引擎。

这一篇我们涉及的内容比串口驱动深得多：可变参数的底层机制（`va_list` 到底是怎么工作的），三种数字转字符串的算法（十进制、十六进制、二进制），模板化的输出策略（一个 `vkprintf_impl`，多个输出后端），还有那个臭名昭著的 INT64_MIN 边界情况。最后我们会和 xv6 的 `printf.c`、Linux 的 `printk` 做设计对比，看看不同规模的内核如何解决同一个问题。

## 环境 & 背景

本篇覆盖的代码文件是 `kernel/mini/lib/` 目录下的格式化库：`private/format.h`（三个格式化函数的声明）、`private/format.cpp`（它们的实现）、`kprintf.h`（面向用户的 kprintf/kdebugf 声明）和 `kprintf.cpp`（格式化引擎核心）。`private/` 子目录是 C++ 惯用的"内部实现细节"标记——告诉使用者"别直接调这些函数"。

这些代码全部是纯算法——不依赖串口、不依赖 I/O 端口、不依赖任何内核特有的基础设施。正因如此，我们可以在 Host 端（Linux 用户态）直接测试它们，不需要启动 QEMU——这是 tag 005 测试架构的设计亮点，下一篇会展开讲。

## va_list：可变参数的底层机制

在讲 kprintf 之前，我们需要先搞清楚一个基础问题：`kprintf("count=%d, name=%s\n", 42, "hello")` 这个调用，函数体内部怎么拿到 `42` 和 `"hello"` 这两个参数？参数的个数和类型都是调用时才确定的，函数声明里只有一个 `...`。

答案在 C 语言的可变参数机制里。`va_list`、`va_start`、`va_arg`、`va_end` 这四个宏/类型构成了完整的可变参数接口。在 x86-64 的 System V ABI 下，`va_list` 本质上是一个描述"额外参数存储在哪里"的结构体——前几个整数参数通过寄存器传递（`%rdi`、`%rsi`、`rdx`、`%rcx`、`%r8`、`%r9`），超出寄存器容量的参数压到栈上。`va_start` 初始化 `va_list`，记录寄存器参数的位置；`va_arg(args, type)` 每次调用时根据 `type` 的大小从对应位置取出一个参数，并移动指针到下一个参数的位置。

这里面有一个非常容易忽略的规则：比 `int` 窄的整型在传递时会被自动提升为 `int`。也就是说，即使你传了一个 `char`，`va_arg` 那一端必须写 `va_arg(args, int)` 而不是 `va_arg(args, char)`。如果你写了后者，在某些平台上可能"恰好能工作"，但在 x86-64 上会读到错误的数据——因为调用者实际上在寄存器里放了一个 32 位的 int，你只取了其中的低 8 位，剩下 24 位留在了那里。Cinux 的 `%c` 格式符处理就用的是 `va_arg(args, int)` 然后转型为 `char`，这正是正确的做法。

## 三种数字转字符串的算法

格式化的核心是把数字变成字符串。我们实现了三个独立的函数：`format_decimal`（十进制）、`format_hex`（十六进制）和 `format_binary`（二进制），放在 `lib/private/format.cpp` 里。

为什么不写一个统一的 `format_int(value, base, buffer)` 函数？因为不同进制的算法有微妙的差异。十进制需要处理负号和 INT64_MIN 特殊情况，十六进制需要大小写切换，二进制需要从最高位开始扫描而不是低位。强行合并只会让每个分支都变得复杂，不如分开写，每个函数都清晰明了。

### 十进制：取模反转法与 INT64_MIN 陷阱

```cpp
int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

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

INT64_MIN 的特殊处理是这个函数里最有趣的边界情况，也是一个经典的陷阱。`INT64_MIN = -9223372036854775808`，它的绝对值是 `9223372036854775808`。问题在于这个数超过了 `int64_t` 的最大值 `INT64_MAX = 9223372036854775807`。在二进制补码表示中，负数范围比正数范围多一个数——这是一个不对称性。如果你执行 `value = -value` 而 `value == INT64_MIN`，结果是未定义行为（在大多数实现中值还是 INT64_MIN 本身，因为溢出绕回了）。

所以必须硬编码这个字符串。这个坑并不冷门——C 标准库的 `printf` 实现也需要处理它。Linux 内核的 `vsnprintf` 用了不同的方法：先把 `int64_t` 强制转型为 `unsigned long long` 然后取负，绕过了溢出问题。Cinux 选择硬编码字符串是因为它最直观——你一眼就能看出这个值是对的。

过了 INT64_MIN 这个坎之后，就是经典的取模反转法了：

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
```

核心思路是 `abs_val % 10` 得到最低位数字，转成字符 `'0' + digit` 放到临时缓冲区。因为是先取最低位，所以 `tmp` 数组里的数字是反过来的——比如 42 得到 `tmp = ['2', '4']`，最后从后往前复制到 `buffer` 就得到了正确的 `"42"`。

这里用 `do {} while` 而不是 `while {}` 是为了正确处理 0 的情况。当 `abs_val = 0` 时，循环体至少执行一次，写入 `'0'`。如果用 `while (abs_val > 0)`，0 就会输出空字符串。`tmp[24]` 的大小是 `int64_t` 十进制最大位数加余量——`INT64_MIN` 的数字部分有 19 位，24 绰绰有余。

### 十六进制：查表法与大小写

```cpp
int format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase) {
    if (buffer_size < 1) return 0;

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

和十进制的模式完全一样，只是基数从 10 变成了 16。`value & 0xF` 取最低 4 位（一个十六进制位），`value >>= 4` 右移 4 位相当于除以 16。数字到字符的转换通过查表完成——`digits` 数组是 `"0123456789abcdef"` 或 `"0123456789ABCDEF"`，下标 0-15 对应 0-F。这比用 `if (digit < 10) '0' + digit else 'a' + digit - 10` 的写法更简洁，也不容易出岔子。

`tmp[20]` 的大小：`uint64_t` 最多 16 个十六进制位（64 / 4 = 16），20 留了余量。同样的 `do {} while` 保证 0 输出 `"0"` 而不是空串。

### 二进制：从最高位开始扫描

```cpp
int format_binary(uint64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int bit = 63;
    bool found = false;
    while (bit >= 0) {
        if ((value >> bit) & 1) {
            found = true;
            break;
        }
        bit--;
    }

    if (!found) bit = 0;

    int idx = 0;
    for (int i = bit; i >= 0 && idx + 1 < buffer_size; i--) {
        buffer[idx++] = ((value >> i) & 1) ? '1' : '0';
    }
    buffer[idx] = '\0';
    return idx;
}
```

二进制用了和前两个不同的策略。前两个是"从低位提取再反转"，而二进制直接从最高位开始扫描。首先从 bit 63 往下找到第一个为 1 的位（最高有效位），然后从那个位开始向低位输出。这样做的好处是自然地去掉了前导零——`0b00101010` 输出 `"101010"` 而不是 `"00000000000000000000000000101010"`。

如果 `value = 0`，`found` 保持 `false`，此时 `bit` 被设为 0，循环从 bit 0 输出一个 `'0'`。

这里有一个可以优化的地方：扫描最高位用了线性搜索，最坏情况要检查 64 次。如果用 GCC 内建函数 `__builtin_clzll(value)`（返回前导零数量），可以一步到位。但 `__builtin_clzll(0)` 的结果是未定义的，需要额外处理零值。Cinux 用显式循环是为了代码的可移植性和可读性——在一个教学 OS 里，"这段代码显而易见地做了什么"比"这段代码用了编译器内建函数快了 2 个时钟周期"更重要。

## 模板化输出策略：一个引擎，多个后端

三个格式化函数就位后，接下来是把它们粘合到一起的格式化引擎。核心是 `kprintf.cpp` 里的模板函数 `vkprintf_impl`：

```cpp
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
                len = format_decimal(
                    static_cast<int64_t>(va_arg(args, int)), buffer, sizeof(buffer));
                goto do_padding;
            // ... %u, %x, %X, %p, %b 类似处理 ...

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

模板参数 `OutputFn` 是输出字符的回调函数。这个设计让格式化逻辑和输出设备完全解耦——格式化函数不关心字符最终去了哪里，它只负责把字符交给 `putc`。`kprintf` 传入一个捕获了 `Serial` 引用的 lambda，`kdebugf` 传入一个调用 `debugcon_putc` 的 lambda。

格式解析逻辑很直白：遇到 `%` 就开始解析格式符，先检查是否是 `'0'` 开头（零填充标志），然后解析数字（宽度），最后读取类型字符。比如 `%04x` 的解析过程是：检测到 `%`，检测到 `'0'`（zero_pad = true），解析 `'4'`（width = 4），读取 `'x'`（type = 'x'）。

`%s` 处理了 `nullptr` 的情况，输出 `"(null)"` 而不是直接崩溃——这是从 Linux 的 `printk` 学来的做法。内核代码中 `nullptr` 字符串并不罕见（比如某个可选的配置参数没设置），直接崩溃比输出 `"(null)"` 更不可取。

你可能注意到 `%d` 和 `%u` 的处理使用了 `goto do_padding`。这是 C 语言中少数几个 `goto` 被认为合理使用的场景——多个格式符（`%d`、`%u`、`%x`、`%X`、`%b`）都需要在格式化后执行相同的宽度填充逻辑，如果把填充代码写五遍，既冗余又容易改漏。Linux 内核的 `vsnprintf` 也大量使用 `goto` 来共享后处理代码。它在这里的语义很清晰：格式化完成，跳到填充输出阶段。

`default` 分支处理未知的格式符——原样输出 `%` 加上那个字符。比如 `%f` 会输出 `%f` 而不是崩溃。这在调试时很方便，你不会因为写错一个格式符就让整个内核挂掉。

最后是两个面向用户的函数：

```cpp
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
```

`kprintf` 通过 lambda 捕获全局串口引用 `[&](char c) { serial.putc(c); }` 作为输出回调。`kdebugf` 用一个无捕获的 lambda——无捕获 lambda 可以隐式转换为函数指针，但在这里它被直接当作模板参数 `OutputFn` 使用，编译器会内联整个调用链。

为什么用模板而不是函数指针回调？答案是性能。lambda 在编译时是已知的类型，编译器可以完全内联 `serial.putc(c)` 或 `debugcon_putc(c)` 的调用，消除函数指针间接跳转的开销。在内核的早期启动阶段，每一个 CPU 周期都可能影响调试时序。xv6 的 `printf.c` 用的是函数指针回调（`consputc`），对于教学项目来说两种方式都可以，但 C++ 模板在编译器优化上有天然优势。

## 支持的格式符一览

| 格式符 | 参数类型 | 说明 | 示例 |
|--------|----------|------|------|
| `%%` | 无 | 百分号本身 | `%%` -> `%` |
| `%c` | int | 单个字符 | `%c` -> `X` |
| `%s` | char* | 字符串（null 输出 "(null)"） | `%s` -> `Hello` |
| `%d` | int | 有符号十进制 | `%d` -> `-12345` |
| `%u` | unsigned int | 无符号十进制 | `%u` -> `42` |
| `%x` | uint64_t | 小写十六进制 | `%x` -> `deadbeef` |
| `%X` | uint64_t | 大写十六进制 | `%X` -> `DEADBEEF` |
| `%p` | uint64_t | 指针（带 "0x" 前缀） | `%p` -> `0xffffffff80000000` |
| `%b` | uint64_t | 二进制（压缩前导零） | `%b` -> `101010` |
| `%4d` | int | 最小宽度 4，空格填充 | `%4d` -> `[   7]` |
| `%04d` | int | 最小宽度 4，零填充 | `%04d` -> `[0007]` |

当前实现不支持长度修饰符（`%ld`、`%llu` 等）和浮点（`%f`）。对于内核调试来说这些不是必需的——Linux 内核的 `printk` 也不支持 `%f`，内核里没人用浮点。如果将来需要，可以在格式解析阶段增加长度修饰符的处理。

## 与 xv6 printf / Linux printk / SerenityOS 的对比

xv6 的 `printf.c`（https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/printf.c）用了一个统一的 `printint()` 函数处理所有进制的整数格式化，通过 base 参数区分十进制和十六进制。这比 Cinux 的"三个独立函数"更紧凑，但 `printint()` 内部需要根据 base 做不同处理（比如十六进制需要 a-f 字母，十进制需要处理负号），函数体比较复杂。xv6 还支持 `%l` 和 `%ll` 长度修饰符，这在 Cinux 的当前实现中是缺失的。另外 xv6 用 spinlock 保护并发 `printf` 调用——Cinux 在当前阶段是单线程的，不需要考虑这个问题，但将来引入多线程后需要加上。

Linux 的 `printk` 是另一个量级的工具。它不仅有格式化输出，还有日志级别（`KERN_EMERG` 到 `KERN_DEBUG` 共 8 级）、环形缓冲区（所有 `printk` 输出同时写入一个 `log_buf` 环形缓冲区，通过 `dmesg` 查看）、速率限制（防止高频日志淹没系统）、以及通过 `console_lock` 实现的并发安全。Cinux 的 `kprintf` 只是一个最简单的"格式化然后往串口写字符"的实现，和 `printk` 比起来约等于原始人和现代人之间的差距。但起点就是这样——先把基本功能跑通，再逐步增加复杂度。

SerenityOS 的 `Serial` 和 `Console` 抽象提供了一个有趣的中间点。它有一层 `Console` 接口（类似于 Cinux 的 `OutputFn` 模板参数），多个输出后端可以同时注册到 `Console` 上。输出时，所有注册的后端都会收到字符。这种"广播"模式比 Cinux 的"一个函数一个后端"更灵活，但实现复杂度也更高。Cinux 目前通过 `kprintf`（串口）和 `kdebugf`（debugcon）两个独立函数实现了类似的效果——不够优雅，但足够实用。

## 内核的 Hello World

所有代码就位后，`main.cpp` 里的第一次 `kprintf` 调用就是整个内核的 Hello World 时刻：

```cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n",
            boot_info->entry_point, boot_info->kernel_phys_base);
    kprintf("Boot Memory Info: mmap_count=%u\n", boot_info->mmap_count);

    for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
        const MemoryMapEntry* entry = &boot_info->mmap[i];
        kprintf("  [%u] base=0x%x, length=0x%x, type=%u\n",
                i, entry->base, entry->length, entry->type);
    }

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

如果一切配置正确（串口驱动已初始化、页表映射正确、全局构造函数执行完毕、链接脚本 LMA 正确），这一行会在终端输出：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xffffffff80020000, kernel_phys_base=0x20000
Boot Memory Info: mmap_count=5
  [0] base=0x0, length=0x9fc00, type=1, acpi=1
  [1] base=0x9fc00, length=0x400, type=2, acpi=1
  ...
```

从 debugcon 一个字符一个字符猜内核状态，到终于能看到格式化的数字和指针——如果你亲身走过这个过程，你就会理解为什么这个"Hello World"比任何其他项目的 Hello World 都更让人激动。

## 收尾

这一篇我们手搓了一个完整的内核格式化输出引擎。三个格式化函数用不同的算法处理十进制、十六进制、二进制的数字转字符串；`vkprintf_impl` 模板函数通过格式字符串解析和 `va_list` 可变参数把一切粘合到一起；`kprintf` 和 `kdebugf` 两个面向用户的接口分别输出到串口和 debugcon。INT64_MIN 的特殊处理是一个经典的边界情况，`goto do_padding` 是 C 语言格式化实现中的惯用模式。

但说实话，写完代码不等于代码就能跑——tag 005 最大的两个 bug 都不是出在新写的代码里，而是出在已有的页表映射和链接脚本中。它们导致了全局构造函数无法执行、串口驱动无法初始化、kprintf 第一次调用就跳转到垃圾地址。下一篇我们来讲这两个幽灵 bug 的侦探故事。

## 参考资料

- xv6 RISC-V `printf.c` (https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/printf.c)：MIT 教学操作系统的 `printf` 实现。使用 `printint()` 函数统一处理不同基数的整数格式化，通过函数指针 `putc` 回调输出，支持 `%l`/`%ll` 长度修饰符，spinlock 保护并发调用。Cinux 的设计借鉴了这种回调模式但改用了 C++ 模板。

- Linux Kernel `kernel/printk/printk.c`：Linux 的 `printk` 实现。日志级别系统（`KERN_EMERG` 到 `KERN_DEBUG`）、环形缓冲区 `log_buf`、`console_lock` 并发保护、速率限制机制。Cinux 的 `kprintf` 是 `printk` 的极简版本，只有格式化输出没有日志管理。

- C 标准库 `printf` 实现参考——glibc、musl、newlib 的 `vfprintf` 实现都使用了类似的格式解析 + `goto` 后处理模式。INT64_MIN 的特殊处理在各种实现中都有出现，方法包括硬编码字符串（Cinux 的做法）和转型为 unsigned 再取负（Linux 的做法）。

- Intel SDM Vol. 2A — `OUT` 指令：debugcon 端口 0xE9 的写入操作使用 `OUT imm8, AL` 形式。Cinux 的 `debugcon_putc` 函数直接映射到这条指令。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
