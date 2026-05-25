---
title: 009-large-kernel-entry-2 · 大内核入口
---

# 009-2 大内核基础设施 — I/O 端口原语 + 串口驱动 + kprintf

## 章节导语

上一章我们把大内核的骨架搭好了——链接脚本把代码安排到了 higher-half 虚拟地址，boot.S 负责从 mini kernel 手里接管 CPU，crt_stub.cpp 补齐了 C++ 运行时。现在大内核能启动了，但它还是个哑巴——没有输出能力，你根本不知道它跑到哪一步了。

这一章我们要给大内核装上"嘴巴"。具体来说，我们需要实现三个层次的抽象：最底层是 x86 I/O 端口原语（io.hpp），封装 `in/out` 指令；中间层是 UART 16550 串口驱动（serial.hpp/cpp），提供按字符和按字符串的输出能力；最上层是 kprintf 格式化输出引擎（kprintf.hpp/cpp），让内核代码能用类似 printf 的方式打印调试信息。完成本章后，大内核就能通过串口输出格式化的诊断信息了——这是后续所有调试工作的基础。

本章的前置知识是上一章（009-1）的大内核启动流程，因为 kprintf 会在 kernel_main 里被调用。

---

## 概念精讲

### x86 I/O 端口编程

x86 架构有两条独立的地址空间：一条是内存地址空间（通过 `mov` 等指令访问），另一条是 I/O 地址空间（通过 `in` 和 `out` 指令访问）。I/O 端口是一个 16 位地址，对应硬件设备的一个寄存器。串口控制器（UART 16550）的寄存器就是通过 I/O 端口访问的——COM1 的基地址是 `0x3F8`，各个寄存器从基地址开始依次偏移。

`in` 指令从指定端口读取数据，`out` 指令向指定端口写入数据。这两个指令支持 byte（8位）、word（16位）、dword（32位）三种宽度，分别对应 `inb/outb`、`inw/outw`、`inl/outl`。在 GCC 内联汇编里，端口编号放在 `%dx` 寄存器（约束 `"Nd"`），数据通过 `%al/ax/eax` 传递（约束 `"a"`）。

还有一个细节：I/O 指令是同步操作，但编译器不知道这一点。如果不加 `"memory"` clobber，编译器可能会把 I/O 操作前后的内存访问重排。所以所有 I/O 函数都加了 `"memory"` clobber，充当编译器屏障。

### UART 16550 串口控制器

PC 兼容机上最常见的串口控制器是 16550 UART。它有 8 个寄存器（通过 I/O 端口偏移 0-7 访问），其中我们最关心的几个是：THR（Transmit Holding Register，偏移 0，写入数据发送字节）、LCR（Line Control Register，偏移 3，配置数据位/校验/停止位）、LSR（Line Status Register，偏移 5，查询发送/接收状态）。

发送一个字节的过程很简单：自旋等待 LSR 的 bit 5（THRE，Transmit Holding Register Empty）变为 1，表示 THR 已空闲可以写入，然后往 THR 写入要发送的字节。这就是所谓的"轮询模式"——不需要中断，不需要 DMA，适合早期内核的简单输出需求。

初始化序列大致是：禁用中断（IER = 0）、设置 8N1 格式（LCR = 0x03）、启用 FIFO（FCR = 0xC7）、设置 MCR（MCR = 0x03，DTR + RTS）。QEMU 的虚拟 UART 默认就是 115200 波特率，所以我们不需要编程除数锁存器（Divisor Latch）。

### kprintf 的设计哲学

内核的 printf 和用户态的 printf 有本质区别：没有堆、没有浮点、不需要线程安全（内核启动时是单核单线程）。所以我们只需要一个简洁的格式化引擎，支持最常用的格式化占位符就够了。

一个有意思的设计选择是把格式化引擎模板化：核心函数接受一个泛型的字符输出函数（`OutputFn`），这样同一个引擎可以输出到串口、QEMU debug console（0xE9 端口）、甚至测试中的 mock buffer，而不需要写多套代码。

---

## 动手实现

### Step 1: 实现 I/O 端口原语 (io.hpp)

**目标**：封装 x86 的 `in/out` 指令，提供类型安全的 I/O 端口访问接口。

**设计思路**：

创建一个头文件 `kernel/arch/x86_64/io.hpp`，在命名空间 `cinux::io` 中定义一组 inline 函数。每种宽度一个读函数和一个写函数：`io_inb/io_outb`（8位）、`io_inw/io_outw`（16位）、`io_inl/io_outl`（32位）。再加一个 `io_wait()` 函数，往端口 0x80 写入任意值产生约 1 微秒延迟——这在某些 ISA 设备（如 PIT、PIC）的操作之间是必要的。

**实现约束**：

所有函数使用 GCC 内联汇编，端口参数类型为 `uint16_t`（I/O 端口地址空间是 16 位），数据类型对应宽度。输出操作 (`in*`) 使用 `"=a"` 约束把结果从 `%al/ax/eax` 取出，输入操作 (`out*`) 使用 `"a"` 约束把数据放入 `%al/ax/eax`、`"Nd"` 约束把端口放入 `%dx`。所有函数都加 `"memory"` clobber 作为编译器屏障。

**踩坑预警**：AT&T 语法中 `inb` 的操作数顺序是 `inb %dx, %al`（源在前，目的在后），`outb` 是 `outb %al, %dx`。别搞反了。

**验证**：

```bash
# 构建，确认编译通过
cmake --build build --target big_kernel
# 预期：编译成功，无汇编错误
```

### Step 2: 实现 UART 16550 串口驱动 (serial.hpp/cpp)

**目标**：封装 UART 硬件操作，提供按字符和按字符串的串口输出接口。

**设计思路**：

创建 `kernel/drivers/serial.hpp` 和 `kernel/drivers/serial.cpp`，在命名空间 `cinux::drivers` 中实现一个 `Serial` 类。

**数据结构描述**：

`Serial` 类持有一个 `uint16_t base_port_` 成员，表示 UART 的基地址。同时定义一组编译期常量：COM1-COM4 的端口地址（0x3F8、0x2F8、0x03E8、0x02E8），UART 寄存器偏移（RBR/THR=0、IER=1、FCR=2、LCR=3、MCR=4、LSR=5），以及 LSR 的关键位掩码（RX_READY=0x01、TX_READY=0x20）。

**接口描述**：

构造函数接受一个端口号并存储到 `base_port_`，但不配置硬件。`init()` 函数做实际的 UART 配置：先禁中断（IER=0），设 8N1 格式（LCR=0x03），启 FIFO（FCR=0xC7），设 MCR（MCR=0x03），然后空读一次 LSR 清除可能的状态。`putc(char c)` 是核心输出函数：自旋等待 `LSR & TX_READY` 非 0，然后往 THR 写入字符。`puts(const char* s)` 遍历字符串逐个 putc，遇到 `\n` 时先输出 `\r`。`is_ready()` 返回 THR 是否为空。

**踩坑预警**：`putc` 的等待循环里建议加 `__asm__ volatile("pause")` 提示 CPU 这是一个 spin-wait，可以降低功耗和总线压力。`\n` → `\r\n` 转换是必须的——串口终端通常需要 `\r\n` 才能正确换行，只发 `\n` 的话光标会跳到下一行但不会回到行首。

**验证**：

```bash
# 配合 kprintf 构建，运行后检查串口输出
cmake --build build && cd build && make run
# 预期：看到 [BIG] Big kernel running @ 0x1000000
```

### Step 3: 实现 kprintf 格式化引擎 (kprintf.hpp/cpp)

**目标**：为内核提供类似 printf 的格式化输出能力，支持常用格式化占位符。

**设计思路**：

创建 `kernel/lib/kprintf.hpp` 和 `kernel/lib/kprintf.cpp`，在命名空间 `cinux::lib` 中实现。内部结构分三层：文件局部的数字格式化辅助函数（`format_decimal`、`format_hex`）、模板化的格式化引擎（`vkprintf_impl`）、以及公开的包装函数（`kprintf`、`kvprintf`、`kpanic`）。

**辅助函数描述**：

`format_decimal(int64_t value, char* buffer, int buffer_size)` 把有符号 64 位整数格式化为十进制字符串，处理负号和 INT64_MIN 特殊情况。`format_hex(uint64_t value, char* buffer, int buffer_size, bool lowercase)` 把无符号 64 位整数格式化为十六进制字符串，通过 `lowercase` 参数控制 a-f 还是 A-F。两个函数都返回写入的字符数（不含 NUL）。

**格式化引擎描述**：

`vkprintf_impl<OutputFn>(OutputFn&& putc_fn, const char* fmt, va_list args)` 是核心。它逐字符扫描格式字符串，遇到非 `%` 字符直接输出。遇到 `%` 后解析可选的 `0` 前缀（零填充标志）和宽度数字，然后根据类型字符处理。支持类型：`%`（字面百分号）、`c`（字符）、`s`（字符串，NULL 输出 "(null)"）、`d`（有符号十进制）、`u`（无符号十进制）、`x`（小写十六进制）、`X`（大写十六进制）、`p`（指针，输出 "0x" + 16 位十六进制）。宽度和零填充只对数字格式有效。

**全局串口实例**：

文件局部持有一个 `static Serial g_serial(SERIAL_COM1)` 单例。`kprintf_init()` 调用 `g_serial.init()` 初始化。`kprintf` 用 lambda 捕获 `g_serial.putc` 作为 OutputFn 传给 `vkprintf_impl`。

**kpanic 描述**：

`kpanic` 的签名带 `[[noreturn]]` 属性，先格式化输出到串口，然后进入 `cli; hlt` 死循环。这是内核的"不可恢复错误"处理——调用 kpanic 就意味着内核承认自己挂了。

**踩坑预警**：`%p` 的行为要特别注意——它输出 "0x" 前缀后，先把十六进制字符串格式化到 buffer，然后补前导零到 16 位，最后输出 buffer 内容。顺序不能错：先输出前导零，再输出实际数字。另外，`%d` 和 `%u` 的 `va_arg` 类型分别是 `int` 和 `unsigned int`，而 `%x` 和 `%X` 的类型是 `uint64_t`——这里的大小差异是有意的，`%x` 需要能格式化完整的 64 位地址。

**实现约束**：格式化引擎是模板函数，定义在 .cpp 文件中——因为我们只在自己文件内部使用它，不需要在头文件中暴露。模板参数 `OutputFn` 是一个可调用对象（lambda、函数指针都行），接受一个 `char` 参数。

**验证**：

```bash
# 运行内核，确认格式化输出正确
cd build && make run
# 预期：看到 [BIG] Big kernel running @ 0x1000000
# 格式化输出的 0x1000000 就是 %p 格式化的结果（如果用 %p 的话）

# 运行 kprintf 的 host 单元测试
cmake --build build && cd build && ctest --test-dir build -R test_kprintf -V
# 预期：所有 kprintf 格式化测试通过（mock Serial::putc，验证输出字符串）
```

### Step 4: 编写 kprintf 的 host 端单元测试

**目标**：在 host 端验证 kprintf 的格式化逻辑，不需要真实的串口硬件。

**设计思路**：

创建 `tests/unit/test_kprintf.cpp`，mock `Serial::putc` 函数，把输出捕获到一个 buffer 中，然后验证各种格式化占位符的输出是否正确。

**测试用例描述**：

需要覆盖的测试场景至少包括：纯字符串输出、`%d` 正数和负数、`%d` 零和 INT_MIN、`%u` 无符号整数、`%x` 小写十六进制、`%X` 大写十六进制、`%p` 指针格式（16 位 + "0x" 前缀）、`%s` 字符串和 NULL 字符串、`%c` 字符、`%%` 百分号转义、`%0Nd` 零填充宽度。每个测试用例 mock 掉 Serial::putc，收集输出字符到 buffer，用字符串比较验证结果。

**验证**：

```bash
cmake --build build && cd build && ctest -R test_kprintf -V
# 预期：所有测试 PASS
```

---

## 构建与运行

```bash
# 完整构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 运行内核
cd build && make run
# 预期串口输出包含：
# [MINI] ... (mini kernel 加载日志)
# [BIG] Big kernel running @ 0x1000000

# 运行 host 单元测试
cd build && ctest -R test_kprintf -V
```

---

## 调试技巧

### 常见 Bug 1: kprintf 输出为空

检查是否调用了 `kprintf_init()`。大内核的 kprintf 持有自己的 Serial 单例，必须显式初始化。如果忘调 init()，串口硬件处于未配置状态，putc 里的 LSR 轮询可能永远不返回。

### 常见 Bug 2: %p 输出只有部分十六进制位

检查 `%p` 的实现是否正确地先格式化到 buffer，然后补零到 16 位，最后输出。如果先输出了 buffer 内容再补零，数字的顺序就反了。

### 常见 Bug 3: %d 对负数输出不正确

INT64_MIN 的特殊处理是必要的——直接取负会溢出（`-INT64_MIN` 是未定义行为）。需要硬编码字符串 `"-9223372036854775808"`。

### 串口调试技巧

如果 QEMU 里看不到串口输出，确认启动参数里有 `-serial stdio`。也可以在 QEMU monitor 里用 `info serial` 查看串口状态。如果需要同时看 debug console 输出（0xE9 端口），检查启动参数里的 `-debugcon file:debug.log`。

---

## 本章小结

| 组件 | 要点 |
|------|------|
| io.hpp | 内联汇编封装 in/out，byte/word/dword 三种宽度，"memory" clobber |
| io_wait() | 写端口 0x80，~1us 延迟 |
| Serial 类 | 轮询模式，构造后调 init()，putc 自旋等 THR 空 |
| UART 初始化 | IER=0, LCR=0x03(8N1), FCR=0xC7, MCR=0x03 |
| kprintf | 模板化引擎 + OutputFn，支持 %d %u %x %X %s %p %c %% |
| %p | 输出 "0x" + 16 位十六进制，前导零补齐 |
| kpanic | [[noreturn]]，输出后 cli;hlt 死循环 |
| 单元测试 | mock Serial::putc，buffer 收集输出字符串验证 |
