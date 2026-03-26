# 从零手写 x64 小内核入口：串口输出与格式化打印

> 标签：x86-64, mini-kernel, serial-driver, kprintf, UART, C++, freestanding, Cinux

---

## 本章概览

前四章我们完成了从 MBR 到 Long Mode 的完整引导流程。当 bootloader 把控制权交给内核时，我们面对的是一片"黑暗"——没有 printf，没有日志，甚至不知道代码是否真的在运行。

本章的目标是点亮这片黑暗：实现内核的串口输出和格式化打印功能。这看似简单，但涉及多个底层子系统：

**关键实现一览**：

- NS16550A UART 串口驱动（COM1: 0x3F8）
- 从零实现的格式化打印函数（kprintf，支持 %d %x %p %b 等）
- 自研的轻量级测试框架（支持 Host 和 Kernel 双模式）
- C++ 运行时支持（全局构造函数、虚函数表）
- 完整的调试工作流（串口、DebugConsole、GDB、VSCode）

最终效果：QEMU 串口输出 `[MINI] Bootstrap kernel running @ 0x20000`，证明内核真正"活"了过来。

---

## 架构图

```
+---------------------------------------------------------------------+
|                        小内核启动流程                                 |
+---------------------------------------------------------------------+
|                                                                     |
|  Bootloader (Long Mode)                                             |
|  │                                                                  |
|  └─ jmp *0xFFFFFFFF80020000  ──────────────────────────────┐       |
|                                                                     |
|  Mini Kernel Entry (_start)                         │       |
│  │                                                              │       |
│  ├─ 禁用中断                                              │       |
│  ├─ 输出 '1' 到 debugcon (0xE9)                     │       |
│  ├─ 设置栈 (8KB @ kernel end)              │       |
│  ├─ 输出 '2' 到 debugcon                               │       |
│  ├─ 保存 BootInfo 指针 (.data 段!)                      │       |
│  ├─ 清零 BSS                                             │       |
│  ├─ 输出 '3' 到 debugcon                               │       |
│  ├─ 调用全局构造函数                                    │       |
│  ├─ 输出 '4' 到 debugcon                               │       |
│  └─ 跳转到 mini_kernel_main()                   │       |
│                                    │       │       │
│                                    │       │       ▼
│                                    │       │  Serial COM1 (0x3F8)
│                                    │       │  │
│                                    │       │  ├─ init() → 115200 8N1
│                                    │       │  └─ putc() → 轮询 LSR bit 5
│                                    │       │
│                                    │       ▼
│                                    │  kprintf() → format_decimal/hex/binary
│                                    │  │
│                                    │  └─ 输出到串口
│                                    │
│                                    ▼
│  QEMU -serial stdio → 终端显示                                |
│                                                                     |
+---------------------------------------------------------------------+

|                        测试框架架构                                   |
+---------------------------------------------------------------------+
|                                                                     |
|  Host 模式                   Kernel 模式                            |
│  │                          │                                      |
│  ├─ CINUX_HOST_TEST 宏定义  ├─ -ffreestanding                      |
│  ├─ 可用标准库               ├─ 串口输出                            |
│  ├─ 快速反馈                 ├─ 真实环境                            |
│  └─ CTest 集成               └─ isa-debug-exit 自动退出            |
│                                                                     |
+---------------------------------------------------------------------+
```

---

## 环境说明

- **平台**：WSL2 + QEMU system-x86_64 6.2+
- **工具链**：GNU AS（AT&T 语法）+ GCC/G++ + CMake
- **C++ 规范**：`-std=c++23 -ffreestanding -fno-exceptions -fno-rtti -fno-stack-protector -mno-red-zone -mcmodel=kernel`
- **串口配置**：COM1 (0x3F8), 115200 8N1
- **调试方式**：
  - 串口输出：`-serial stdio` 重定向到终端
  - DebugConsole：`-debugcon file:debug.log` 写入文件
  - GDB：`-s -S` 监听 localhost:1234
  - VSCode：F5 一键启动图形调试

---

## 第一阶段 —— 汇编入口：_start 的四步验证

内核入口点是整个系统最先执行的代码。我们需要在这里完成 CPU 状态的初始化，同时输出调试信息验证每个阶段。

### 1.1 入口点代码详解

文件：`kernel/mini/arch/x86_64/boot.S`

```asm
.section .text
.code64

.global _start
.type _start, @function

_start:
    /* 禁用中断 */
    cli

    /* 输出 '1' 到 debugcon - _start reached */
    movb $0x31, %al              /* '1' */
    outb %al, $0xE9

    /* 设置栈 - 8KB stack at end of kernel */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* 输出 '2' 到 debugcon - stack ready */
    movb $0x32, %al              /* '2' */
    outb %al, $0xE9

    /* 保存 BootInfo 指针 BEFORE clearing BSS (BSS clear uses %rdi) */
    movq %rdi, __boot_info_ptr

    /* 清零 BSS section */
    movq $__bss_start, %rdi      /* destination */
    movq $__bss_end, %rcx        /* end */
    subq %rdi, %rcx              /* count = end - start */
    xorq %rax, %rax              /* value = 0 */
    rep stosb                    /* clear */

    /* 输出 '3' 到 debugcon - BSS cleared */
    movb $0x33, %al              /* '3' */
    outb %al, $0xE9

    /* 调用全局构造函数 (C++ runtime) */
    call _init_global_ctors

    /* 输出 '4' 到 debugcon - ctors done */
    movb $0x34, %al              /* '4' */
    outb %al, $0xE9

    /* 调用 C++ main */
    movq __boot_info_ptr, %rdi   /* first argument: BootInfo* */
    call mini_kernel_main

    /* 如果内核返回，停机 */
.halt:
    cli
    hlt
    jmp .halt
```

### 1.2 关键设计点

**为什么 BootInfo 指针要保存在 .data 段？**

注意代码中的注释：`BEFORE clearing BSS (BSS clear uses %rdi)`。BSS 清零使用 `rep stosb` 指令，这个指令会使用 `RDI` 作为目标地址。如果我们先清零 BSS，`%rdi` 中的 BootInfo 指针就会被覆盖。

更重要的是，BootInfo 指针必须在 `.data` 段而不是 `.bss` 段：

```asm
/* BootInfo Pointer Storage (.data section - NOT .bss!) */
.section .data
.global __boot_info_ptr
.align 8
__boot_info_ptr:
    .quad 0
```

如果放在 `.bss` 段，BSS 清零会把指针值也清掉，后续就无法访问 BootInfo 了。

**DebugCon 输出的四个数字**

- `'1'`：到达 _start，CPU 执行到了内核代码
- `'2'`：栈已设置，`%rsp` 指向 `__mini_stack_top`
- `'3'`：BSS 已清零，全局变量初始化为 0
- `'4'`：全局构造函数已执行，C++ 运行时就绪

这四个数字就像"心跳信号"，让我们确认内核启动的每个关键阶段都正常完成。

### 1.3 栈的配置

```asm
/* Stack Section */
.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

__mini_stack:
    .skip MINI_STACK_SIZE
__mini_stack_top:
```

栈大小 8KB，位于 BSS 段末尾。栈向下增长，`__mini_stack_top` 是栈的起始地址（最高地址）。

---

## 第二阶段 —— C++ 运行时支持

在 freestanding 环境下，没有标准库提供运行时支持。我们需要自己实现几个关键函数。

文件：`kernel/mini/arch/x86_64/crt_stub.cpp`

### 2.1 全局构造函数初始化

```cpp
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    for (void (**func)() = __init_array_start; func != __init_array_end; func++) {
        (*func)();
    }
}
```

编译器会把所有全局对象的构造函数放到 `.init_array` section。我们只需要遍历这个数组，依次调用每个函数。

**为什么这很重要？**

串口驱动使用了全局单例：

```cpp
// kernel/mini/driver/serial.cpp
static Serial g_serial(SERIAL_COM1);

Serial& get_initial_serial() {
    return g_serial;
}
```

如果构造函数没有被调用，`g_serial` 的 `init()` 不会执行，串口就无法工作。

### 2.2 纯虚函数调用处理

```cpp
[[noreturn]] void __cxa_pure_virtual() {
    // 无限停机 - 纯虚函数调用是编程错误
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

如果有人试图调用纯虚函数（通常是抽象类的析构函数），内核会停机。这是一种"优雅崩溃"的设计。

### 2.3 operator new/delete

```cpp
void* operator new(unsigned long size) {
    // 停机 - new 不支持
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void operator delete(void* ptr) noexcept {
    // 停机 - delete 不支持
    (void)ptr;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

我们暂时不支持动态内存分配，所以 `new` 和 `delete` 直接停机。这防止链接错误，同时强制开发者使用静态分配。

---

## 第三阶段 —— 串口驱动实现

串口是裸机开发最常用的调试输出方式。我们使用 NS16550A/8250 兼容的 UART，通过 COM1 端口（0x3F8）发送数据。

### 3.1 硬件基础

NS16550A UART 有 8 个寄存器，通过 I/O 端口访问：

| 偏移 | 读寄存器 | 写寄存器 | 简写 | 说明 |
|------|----------|----------|------|------|
| +0 | RBR | THR | - | 接收/发送缓冲区 |
| +1 | - | IER | IER | 中断使能 |
| +2 | - | FCR | FCR | FIFO 控制 |
| +3 | - | LCR | LCR | 线路控制 |
| +4 | - | MCR | MCR | Modem 控制 |
| +5 | LSR | - | LSR | 线路状态 |
| +6 | MSR | - | MSR | Modem 状态 |
| +7 | SCR | SCR | SCR | 暂存器 |

**LSR（线路状态寄存器）位定义**：

- Bit 0 (DR)：数据就绪（RBR 有数据可读）
- Bit 5 (THRE)：发送保持寄存器为空（可以发送）

### 3.2 串口初始化

文件：`kernel/mini/driver/serial.cpp`

```cpp
void Serial::init() {
    // 1. 禁用中断
    io::outb(base_port + SerialReg::IER, 0x00);

    // 2. 设置 8N1（8 位数据，无奇偶校验，1 位停止位）
    // LCR = 0x03
    io::outb(base_port + SerialReg::LCR, 0x03);

    // 3. 启用 FIFO，清除缓冲区，14 字节阈值
    io::outb(base_port + SerialReg::FCR, 0xC7);

    // 4. 设置 RTS + DTR（Modem 控制）
    io::outb(base_port + SerialReg::MCR, 0x03);
}
```

QEMU 默认串口配置已经是 115200 8N1，所以不需要设置波特率。我们只需要确认 LCR 为 0x03（8 位数据，无奇偶校验，1 位停止位）。

### 3.3 发送单个字符

```cpp
void Serial::putc(char c) {
    // 等待发送缓冲区就绪
    while (!is_tx_ready()) {
        __asm__ volatile("pause");  // 降低 CPU 占用
    }

    // 写入发送保持寄存器
    io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}

bool Serial::is_tx_ready() const {
    return (io::inb(base_port + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
}
```

这是一个轮询（polling）实现：不断检查 LSR 的 bit 5，直到 THR 为空，然后写入字符。`pause` 指令告诉 CPU 我们在自旋等待，可以降低功耗。

### 3.4 发送字符串

```cpp
void Serial::puts(const char* s) {
    if (s == nullptr) return;

    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');  // 换行前发送回车
        }
        putc(*s);
        s++;
    }
}
```

注意 `\n` 前自动插入 `\r`，这是终端的标准行为。

---

## 第四阶段 —— 格式化打印：kprintf 的实现

kprintf 是内核的调试输出函数，支持类似 printf 的格式化语法。我们从头实现了整个格式化系统，包括数字转换和格式解析。

### 4.1 格式化函数：数字到字符串

文件：`kernel/mini/lib/private/format.cpp`

#### 十进制格式化

```cpp
int format_decimal(int64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    int idx = 0;
    bool is_neg = value < 0;

    // INT64_MIN 特殊处理（无法直接取负）
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

    // 反向提取数字（低位到高位）
    uint64_t abs_val = static_cast<uint64_t>(value);
    char tmp[24];
    int tmp_idx = 0;

    do {
        tmp[tmp_idx++] = '0' + (abs_val % 10);
        abs_val /= 10;
    } while (abs_val > 0 && tmp_idx < 24);

    // 添加符号
    if (is_neg && idx < buffer_size - 1) {
        buffer[idx++] = '-';
    }

    // 反向复制到输出
    while (tmp_idx > 0 && idx < buffer_size - 1) {
        buffer[idx++] = tmp[--tmp_idx];
    }
    buffer[idx] = '\0';

    return idx;
}
```

**算法流程**：

1. 判断符号，如果是负数先取绝对值
2. 反向提取数字（低位到高位），存入临时数组
3. 添加负号
4. 反向复制到输出缓冲区

**INT64_MIN 的特殊处理**：

`INT64_MIN = -9223372036854775808`，其绝对值无法用 `int64_t` 表示（会溢出）。所以我们直接复制字符串。

#### 十六进制格式化

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

十六进制更简单：每次取低 4 位，转换为对应数字，然后右移 4 位。

#### 二进制格式化

```cpp
int format_binary(uint64_t value, char* buffer, int buffer_size) {
    if (buffer_size < 1) return 0;

    // 找到最高位的 1
    int bit = 63;
    bool found = false;
    while (bit >= 0) {
        if ((value >> bit) & 1) {
            found = true;
            break;
        }
        bit--;
    }

    if (!found) bit = 0;  // value = 0

    // 输出二进制字符串
    int idx = 0;
    for (int i = bit; i >= 0 && idx + 1 < buffer_size; i--) {
        buffer[idx++] = ((value >> i) & 1) ? '1' : '0';
    }
    buffer[idx] = '\0';

    return idx;
}
```

二进制有个特点：压缩前导零。比如 `0b00101` 输出 `"101"` 而不是 `"00000101"`。这通过先找到最高位的 `1` 来实现。

### 4.2 kprintf 主函数

文件：`kernel/mini/lib/kprintf.cpp`

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
            case 's': /* ... 字符串处理 ... */ break;
            case 'd':
                len = format_decimal(static_cast<int64_t>(va_arg(args, int)), buffer, sizeof(buffer));
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
            }

do_padding:
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

**模板设计**：

`vkprintf_impl` 是一个模板函数，接受一个"输出函数"作为参数。这个设计实现了格式化逻辑与输出设备的解耦：

```cpp
// 串口输出
void kprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    auto& serial = serial::get_initial_serial();
    vkprintf_impl([&](char c) { serial.putc(c); }, format, args);
    va_end(args);
}

// DebugConsole 输出
void kdebugf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vkprintf_impl([](char c) { debugcon_putc(c); }, format, args);
    va_end(args);
}
```

### 4.3 支持的格式符

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

宽度修饰符：
- `%Nd`：最小宽度 N，空格右对齐
- `%0Nd`：最小宽度 N，零填充

---

## 第五阶段 —— 内核主函数：mini_kernel_main

现在所有基础设施都就绪，我们可以写内核主函数了。

文件：`kernel/mini/main.cpp`

```cpp
extern "C" {
extern uint64_t __boot_info_ptr;
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    // 内核入口点
    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n",
            boot_info->entry_point, boot_info->kernel_phys_base);

    // TODO: 初始化内核子系统
    // TODO: 启动调度器

    // 停机
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

**运行效果**：

```bash
$ make run
qemu-system-x86_64 -m 512M -serial stdio -no-reboot \
    -debugcon file:debug.log -global isa-debugcon.iobase=0xe9 \
    -drive file=build/cinux.img,format=raw,index=0,media=disk

1234Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xffffffff80020000, kernel_phys_base=0x20000
```

- `1234`：debugcon 输出（启动各阶段）
- `Cinux Mini Kernel v0.1.0`：kprintf 串口输出
- `BootInfo: ...`：格式化输出测试

---

## 第六阶段 —— 测试框架：双模式验证

写内核代码不能"写了就跑"，我们需要测试来验证正确性。Cinux 使用自研的轻量级测试框架，支持 Host 和 Kernel 双模式。

### 6.1 测试框架设计

文件：`test/framework/test_framework.h`

```cpp
// 测试注册宏
TEST("smoke: 1+1=2") {
    ASSERT_EQ(1 + 1, 2);
}

// 断言宏
ASSERT_EQ(actual, expected);
ASSERT_NE(actual, expected);
ASSERT_TRUE(expr);
ASSERT_FALSE(expr);
// ... 更多断言
```

**编译期宏注册**：

每个 `TEST()` 宏会：
1. 生成一个唯一的测试函数
2. 创建一个静态注册对象
3. 在程序启动前自动注册到测试表

### 6.2 Host 端测试

Host 测试在 Linux 宿主机上运行，使用标准库：

```bash
# 构建测试
cmake -DCMAKE_BUILD_TYPE=Debug -DCINUX_BUILD_TESTS=ON -B build
make -C build

# 运行 Host 测试
make test_host
# 或
ctest --output-on-failure
```

**测试文件示例**：

文件：`test/unit/test_kprintf_format.cpp`

```cpp
TEST("kprintf: decimal positive") {
    char buffer[64];
    int len = format_decimal(42, buffer, sizeof(buffer));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(std::string(buffer), "42");
}

TEST("kprintf: decimal INT64_MIN") {
    char buffer[64];
    int len = format_decimal(INT64_MIN, buffer, sizeof(buffer));
    ASSERT_EQ(len, 20);
    ASSERT_EQ(std::string(buffer), "-9223372036854775808");
}
```

### 6.3 Kernel 端测试

Kernel 测试在 QEMU 中运行，使用与生产内核完全相同的编译选项：

文件：`kernel/mini/test/main_test.cpp`

```cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    // kprintf/kdebugf 测试
    kprintf("=== kprintf Test ===\n");
    kprintf("String: %s\n", "Hello, Cinux!");
    kprintf("Decimal: %d\n", -12345);
    kprintf("Hex: %x\n", 0xDEADBEEF);
    kprintf("Pointer: %p\n", 0xFFFFFFFF80020000);

    // C++ 运行时测试
    run_cpp_tests();

    // 安全退出
    __asm__ volatile("outl %0, $0xf4" : : "a"(0));  // QEMU isa-debug-exit
}
```

**运行 Kernel 测试**：

```bash
# 自动退出模式
make run-kernel-test

# 交互式模式（Ctrl+C 退出）
make run-kernel-test-interactive

# 调试模式
make run-kernel-test-debug
```

### 6.4 测试对比

| 特性 | Host 测试 | Kernel 测试 |
|---|---|---|
| 运行环境 | Linux 用户态 | QEMU 内核模式 |
| 标准库 | 可用 | 不可用 |
| 输出方式 | stdout | 串口 |
| 调试难度 | 低（GDB/Valgrind） | 高（QEMU GDB） |
| 启动时间 | 毫秒级 | 秒级 |
| 真实性 | 中（模拟环境） | 高（真实内核环境） |

---

## 第七阶段 —— 调试工作流：从串口到 VSCode

内核开发需要多种调试手段。我们提供了从简单到复杂的完整工作流。

### 7.1 串口输出调试（最简单）

```bash
make run
```

输出直接显示在终端，适合快速验证。

### 7.2 DebugConsole 调试

```bash
make run
cat build/debug.log
```

DebugConsole（端口 0xE9）是 QEMU 私有的调试端口，输出速度极快，适合早期启动调试。

### 7.3 GDB 命令行调试

```bash
# Terminal 1: 启动 QEMU 调试模式
make run-debug

# Terminal 2: 启动 GDB
gdb build/kernel/mini/mini_kernel
(gdb) target remote :1234
(gdb) break mini_kernel_main
(gdb) continue
(gdb) print __boot_info_ptr
(gdb) x/10i $pc
```

### 7.4 VSCode 图形调试（推荐）

**配置文件**：`.vscode/launch.json`

```json
{
    "name": "QEMU 调试 (mini kernel)",
    "type": "cppdbg",
    "request": "launch",
    "program": "${workspaceFolder}/build/kernel/mini/mini_kernel",
    "MIMode": "gdb",
    "miDebuggerPath": "gdb",
    "miDebuggerServerAddress": "localhost:1234"
}
```

**使用方式**：

1. 打开项目
2. 按 `F5` 或选择 "Run and Debug" → "QEMU 调试 (mini kernel)"
3. VSCode 会自动构建、启动 QEMU、连接 GDB
4. 设置断点、单步执行、查看变量

**常用快捷键**：

| 快捷键 | 功能 |
|--------|------|
| `F5` | 启动调试 |
| `F10` | 单步跳过 |
| `F11` | 单步进入 |
| `Shift+F11` | 跳出函数 |
| `Shift+F5` | 停止调试 |

---

## 常见问题排查

### 问题 1：无串口输出

**检查清单**：

1. QEMU 是否使用 `-serial stdio`
2. `Serial::init()` 是否被调用（全局构造函数）
3. 端口地址是否正确（0x3F8）
4. 等待 THRE 的循环是否退出（检查 LSR）

**排查方法**：

```bash
# 检查 debugcon
cat build/debug.log
# 应该看到 "1234"

# 使用 GDB 检查串口初始化
gdb build/kernel/mini/mini_kernel
(gdb) break Serial::init
(gdb) continue
```

### 问题 2：输出乱码

**原因**：波特率不匹配

**解决**：

QEMU 默认 115200 8N1，驱动无需配置。如果使用真实硬件，需要设置波特率：

```cpp
// 波特率配置（真实硬件需要）
void set_baudrate(uint16_t divisor) {
    io::outb(base_port + SerialReg::LCR, 0x80);  // DLAB = 1
    io::outb(base_port + 0, divisor & 0xFF);     // 低字节
    io::outb(base_port + 1, divisor >> 8);       // 高字节
    io::outb(base_port + SerialReg::LCR, 0x03);  // DLAB = 0, 8N1
}
```

### 问题 3：格式化输出错误

**排查方法**：

```bash
# 运行 Host 测试（直接在 Linux 上测试格式化）
make test_host

# 如果通过，说明格式化算法正确
# 问题可能在串口发送
```

### 问题 4：页表映射导致内核无法启动

**症状**：

- 输出停留在 bootloader 的 `OPL` 后，没有后续的内核输出 `1234`
- 使用 identity mapping (0x20000) 可以正常执行
- 使用 higher-half mapping 时发生页面错误

**根本原因**：

页表 PDPT 索引计算错误。对于虚拟地址 `0xFFFFFFFF80000000`：

```
bits 47:39 (PML4 索引) = 0x1FF = 511 ✓
bits 38:30 (PDPT 索引) = 0x1FE = 510 ← 容易写错成 511！
bits 29:21 (PD 索引)   = 0x000 = 0   ✓
```

如果错误使用 PDPT[511]，会触发页面错误。

**预防**：

```assembly
// 使用清晰的宏定义
.set HH_PML4_INDEX,    511  // 0xFFFFFFFF80000000 的 PML4 索引
.set HH_PDPT_INDEX,    510  // ← 注意：是 510，不是 511！
.set HH_PD_INDEX,      0    // PD 索引
```

### 问题 5：全局构造函数指针是垃圾值

**症状**：

- 调用全局构造函数时崩溃
- `__init_array` 中的指针是 `0xffffffff`

**根本原因**：

链接器脚本使用 `SIZEOF()` 累加计算 LMA，没有考虑段间对齐填充。

**正确的写法**：

```ld
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }
}
```

使用 `ADDR() - KERNEL_Virt_BASE` 可以自动处理对齐填充。

### 启动故障排查流程

```
内核启动失败
    │
    ├─> 输出停留在 "O" 之前 → 检查磁盘加载
    ├─> 输出停留在 "OP" 之间 → 检查保护模式切换
    ├─> 输出停留在 "PL" 之间 → 检查 long mode 切换
    ├─> 输出有 "L" 但没有后续 → 检查页表设置
    │                              验证 PDPT[510] = 0x3003
    ├─> 输出有 "1" 但没有 "2" → 检查栈指针设置
    ├─> 输出有 "2" 但没有 "3" → 检查 BSS 清零
    ├─> 输出有 "3" 但没有 "4" → 检查 __init_array
    │                              验证链接器脚本 LMA 计算
    └─> 输出有 "4" 但没有内核输出 → 检查 mini_kernel_main
```

---

## 本章踩坑总结

1. **BootInfo 指针必须放在 .data 段**：BSS 清零会把 .bss 段的内容也清掉
2. **BSS 清零使用 %rdi**：保存 BootInfo 指针必须在清零之前
3. **全局构造函数必须调用**：串口驱动依赖全局单例的构造函数
4. **INT64_MIN 无法直接取负**：需要特殊处理
5. **DebugCon 输出顺序**：'1' '2' '3' '4' 必须按顺序出现
6. **串口轮询必须检查 LSR**：不能直接写 THR，必须等待 bit 5 置位
7. **测试框架的双模式设计**：Host 测试快速验证，Kernel 测试真实环境
8. **higher-half 页表索引**：0xFFFFFFFF80000000 的 PDPT 索引是 510，不是 511
9. **链接器脚本 LMA 计算**：使用 `ADDR() - KERNEL_Virt_BASE` 而不是 `SIZEOF()` 累加

---

## 收尾

到这里，005 章的内容就结束了。我们实现了内核的串口输出和格式化打印功能，搭建了完整的测试和调试基础设施。

**核心要点**：

1. **汇编入口四步验证**：'1' '2' '3' '4' 确认启动各阶段正常
2. **C++ 运行时支持**：全局构造函数、纯虚函数、operator new/delete
3. **NS16550A 串口驱动**：轮询方式，COM1 端口，115200 8N1
4. **从零实现 kprintf**：格式化函数（十进制/十六进制/二进制）+ 模板设计
5. **双模式测试框架**：Host 快速验证，Kernel 真实环境
6. **完整调试工作流**：串口、DebugConsole、GDB、VSCode

最终效果：QEMU 串口输出 `[MINI] Bootstrap kernel running @ 0x20000`，内核真正"活"了过来。

下一章，我们将实现内核的内存管理和虚拟内存系统，为后续的进程管理打下基础。

---

## 最重要三条认知（必须记住）

**格式化输出是内核的"眼睛"**：没有 kprintf，我们就是"盲人摸象"。串口输出看起来简单，但涉及 UART 驱动、格式化算法、模板设计等多个子系统，是内核开发的必备基础设施。

**测试必须双模式**：Host 测试快速反馈（毫秒级），Kernel 测试真实环境（秒级）。两者互补，缺一不可。算法逻辑先在 Host 验证，然后再在 Kernel 测试，这是最高效的开发方式。

**调试工具要分层**：串口输出（最简单）→ DebugConsole（快速）→ GDB 命令行（精确）→ VSCode 图形（推荐）。不同场景使用不同工具，不要"杀鸡用牛刀"。

---

## 参考资料

### 硬件文档
- NS16550A Datasheet：https://pdf.datasheetcatalog.com/datasheet2/national/447461.pdf
- Intel SDM Vol. 3A, Chapter 9: Processor Management and Initialization

### OSDev Wiki
- https://wiki.osdev.org/Serial_Ports
- https://wiki.osdev.org/Printing_to_Serial
- https://wiki.osdev.org/Testing_Devices

### 调试工具
- QEMU GDB documentation：https://qemu.readthedocs.io/en/v9.1.3/system/gdb.html
- Building a VS Code Debugging Workflow for a Custom x64 OS：https://www.sqlpassion.at/archive/2025/07/22/building-a-vs-code-debugging-workflow-for-a-custom-x64-operating-system-with-qemu-and-gdb/

### 项目内部文档
- `document/notes/005/005-06-kprintf-format.md`：kprintf 格式化实现详解
- `document/notes/005/005-07-format-algorithms.md`：格式化算法详解
- `document/notes/005/005-10-serial-debug.md`：串口调试详解
- `document/notes/005/005-11-debug-workflows.md`：调试工作流总结
- `document/notes/005/005-12-mistake-check.md`：页表映射与链接脚本调试排查指南

---

**文件路径汇总**：

- `kernel/mini/arch/x86_64/boot.S` - 汇编入口
- `kernel/mini/arch/x86_64/crt_stub.cpp` - C++ 运行时支持
- `kernel/mini/driver/serial.h` / `serial.cpp` - 串口驱动
- `kernel/mini/lib/kprintf.h` / `kprintf.cpp` - 格式化打印
- `kernel/mini/lib/private/format.h` / `format.cpp` - 格式化函数
- `kernel/mini/main.cpp` - 内核主函数
- `kernel/mini/test/main_test.cpp` - 内核测试
- `test/framework/test_framework.h` - 测试框架
- `.vscode/launch.json` - VSCode 调试配置

