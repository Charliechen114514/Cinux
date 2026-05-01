# 踩坑实录：链接器符号与入口点——两个差点让我放弃的 Bug

> 标签：链接器符号、入口点、全局构造函数、debugcon、对象库
> 前置：[006-1 从零开始的物理内存管理](./006-mini-kernel-pmm-1.md)

## 前言

说实话，这一章写起来最痛苦的部分不是 PMM 本身——位图分配器逻辑清晰、代码直白，一个下午就能搞定。真正让我血压拉满的是两个隐藏极深的坑：链接器符号的访问陷阱，以及 CMake 对象库引发的入口点偏移 bug。第一个坑让我对着 `kernel_size = 0` 的串口输出发呆了两个小时，第二个坑更离谱——串口完全没输出，我一度以为是 QEMU 坏了或者 bootloader 把内核加载到了错误的地方。

这两个 bug 的共同特点是：代码编译完全通过，没有任何警告，但运行时行为完全错误。它们不属于"语法错误"或"逻辑错误"这种教科书式的分类，而是横跨 C++、链接器、CMake 三个系统的"跨界 Bug"。如果你也在写内核、也在用链接器符号、也在用 CMake 对象库，这篇文章可能帮你省掉好几个不眠之夜。

## 环境说明

这个 debug 过程在 QEMU 128MB 配置下进行，使用 `-debugcon file:debug.log` 捕获 debugcon 端口输出，`-serial stdio` 获取串口输出。内核链接在 higher-half 地址 `0xFFFFFFFF80000000`，物理加载地址 `0x20000`。bootloader 硬编码跳转到 `0xFFFFFFFF80020000`（虚拟地址），这个地址被假定是 `_start` 函数的位置。调试工具只有 `nm`（查看符号表）、`objdump`（反汇编）、`readelf`（查看 ELF 头）和 debugcon 字符输出。

## 第一个坑：链接器符号不是变量

### 事情是怎么开始的

PMM 的初始化流程中需要知道内核自身占用了多少物理内存，以便把相应的页标记为"已用"。链接器脚本 `linker.ld` 中定义了 `__kernel_size` 符号，计算方式是 `__mini_kernel_end - (KERNEL_Virt_BASE + KERNEL_PHYS_BASE)`。我用 `nm` 确认了符号的值是 `0x42F0`（约 17KB），看起来没问题。

然后我在 C++ 代码里这么写的：

```cpp
extern "C" {
    extern uint64_t __kernel_size;
}

uint64_t size = __kernel_size;  // 读到的竟然是 0！
```

编译通过，链接通过，没有任何错误和警告。但串口输出显示 `kernel_size = 0`。我反复确认链接器脚本的计算是正确的，`nm` 也显示符号值确实是 `0x42F0`。那为什么 C++ 代码读到的是 0？

### 根因：链接器符号是地址常量

问题的根源在于对链接器符号本质的误解。链接器脚本中的符号（如 `__kernel_size = expr`）不是变量——链接器不会在内存中分配空间来存储它的值。它只是符号表中的一个条目，记录了一个地址值。当你声明 `extern uint64_t __kernel_size` 并访问 `__kernel_size` 时，C++ 编译器生成的代码是这样的：去符号表找到 `__kernel_size` 的地址，然后读取该地址处的 8 字节内存内容。

但这个地址处根本没有任何有效数据！链接器没有在这里存放任何东西。这个地址可能落在 `.text` 段的某个位置、也可能落在 `.bss` 段的某个位置，里面存的是什么完全取决于这段内存碰巧被初始化成了什么。在我的案例中，它恰好是 0（因为那个地址落在 BSS 段，BSS 被 `rep stosb` 清零了）。

正确的做法是声明为 `char` 类型（最小可取地址类型），然后取地址：

```cpp
extern "C" {
    extern char __kernel_size;     // 类型不重要，能取地址就行
}

uint64_t size = reinterpret_cast<uint64_t>(&__kernel_size);  // 取地址才是值
```

`&__kernel_size` 获取的是符号的地址——对于链接器符号来说，地址本身就是它所代表的值。这是 OSDev Wiki 的 [Writing A Page Frame Allocator](https://wiki.osdev.org/Writing_A_Page_Frame_Allocator) 页面也提到的经典模式：链接器符号 `endkernel` 的访问方式是取地址而非直接读取。你可以用 `nm` 验证这一点：

```bash
nm build/kernel/mini/mini_kernel | grep __kernel_size
# 输出: 00000000000042f0 A __kernel_size
#                ^^^^^^^^ 这就是你要的值
#                          ^ A = absolute symbol
```

类型 `A` 表示这是一个绝对符号，它的值不随重定位改变。这也印证了它不是一个真正的变量——变量会有类型 `B`（BSS 段）或 `D`（数据段）。

### 这个坑为什么特别危险

这个 bug 之所以难以发现，是因为它在大多数情况下不会导致崩溃。如果链接器符号恰好落在 `.text` 段内，直接访问会读到指令编码的一部分——一个看起来像随机数的非零值。你可能会误以为 PMM 工作正常，但内核大小是错的，导致某些页被错误地标记为可用或已用。只有在符号恰好落在 BSS 段（被清零）时，你才会得到 0 这个明显的错误值。所以如果你看到内核大小是 0 或者一个离谱的巨大数字，第一时间检查链接器符号的访问方式。

## 第二个坑：CMake 对象库的链接顺序

### 事情是怎么开始的

在解决了链接器符号的问题后，PMM 代码终于能编译了。我满怀期待地启动 QEMU——然后什么都没有。串口没有输出，debugcon 日志是空的。我甚至一度怀疑是不是忘记编译了，反复检查了构建命令。最后我把问题简化到最小：注释掉 PMM 代码，只保留最基本的串口输出。结果还是没输出。

这意味着问题不在 PMM 代码本身，而在更底层的地方。

### 用 debugcon 逐步追踪

QEMU 的 debugcon 端口（`0xE9`）是内核调试的救命稻草——它不依赖任何硬件初始化，CPU 执行的第一条指令就可以用它。我在 `boot.S` 的 `_start` 函数中放了四个字符标记：`'1'`（进入 _start）、`'2'`（栈就绪）、`'3'`（BSS 清零完成）、`'4'`（全局构造函数调用完成）。在 `_init_global_ctors` 中放了 `'{'` 和 `'}'`。在 Serial 构造函数中放了 `'C'`。

启动 QEMU 后检查 debugcon 日志——完全空白。这意味着 `_start` 根本没有执行。

### 发现 entry point 偏移

bootloader 在 `stage2.S` 中硬编码了跳转地址：

```asm
movq $0xFFFFFFFF80020000, %rax
call *%rax
```

它期望 `0xFFFFFFFF80020000` 处是 `_start`。我用 `readelf` 检查内核的 entry point：

```bash
readelf -h build/kernel/mini/mini_kernel | grep Entry
# 输出: Entry point address: 0xffffffff8002012a
```

`0x8002012a` 而不是 `0x80020000`！差了 `0x12a` 字节。bootloader 跳到了 `0x80020000`，但那里已经不是 `_start` 了。我反汇编看了一下那个地址：

```bash
objdump -d build/kernel/mini/mini_kernel | grep "80020000:"
# 输出: ffffffff80020000:  55  push   %rbp
```

`push %rbp` 是函数序言的典型指令。这个地址处是 `mini_kernel_main` 的开头——bootloader 跳过了 `_start`，直接跑进了 `mini_kernel_main`。于是 BSS 没被清零、全局构造函数没被调用、`g_serial` 对象的 `base_port` 成员是 0，串口驱动卡在死循环里。

### 根因：CMake 对象库改变了链接顺序

之前代码用的是 CMake 静态库（STATIC library），链接器从 `.a` 文件中提取 `.o` 时，`_start` 所在的 `boot.o` 恰好被排在了最前面。切换到对象库（OBJECT library）后，CMake 传递 `.o` 文件给链接器的顺序变了，`boot.o` 不再是第一个，导致 `_start` 不在 `.text` 段的开头。

这个 bug 的诡异之处在于：内核代码"能运行"——它跑进了 `mini_kernel_main`，E820 信息可以打印，看起来一切正常。但全局构造函数没被调用，所有依赖构造函数初始化的全局对象都处于未初始化状态。这个 bug 只在特定的链接顺序下才会触发，而且不一定会导致崩溃——可能只是某些全局变量值不对，藏在暗处等你发现。

### 修复：专用段保证入口点位置

修复分两步。第一步在链接器脚本中添加一个专用的 `.text.start` 段：

```ld
.text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
    *(.text.start)        /* _start 必须在最前面! */
    *(.text .text.*)
    *(.rodata .rodata.*)
}
```

链接器按通配符出现的顺序处理输入段，所以 `.text.start` 中的内容永远被放在 `.text` 输出段的最前面。

第二步在 `boot.S` 中把 `_start` 放进这个专用段：

```asm
.section .text.start, "ax"
.code64

.global _start
.type _start, @function

_start:
    cli
    /* ... */
```

修复后验证：

```bash
readelf -h build/kernel/mini/mini_kernel | grep "Entry point"
# 输出: Entry point address: 0xffffffff80020000
# 现在对了！

objdump -d build/kernel/mini/mini_kernel | head -3
# ffffffff80020000 <_start>:
# ffffffff80020000:  fa  cli
# _start 确实在正确地址
```

debugcon 日志也恢复了正常：`1234{...}C...J...`，完整的启动流程标记都在。

### 和其他项目的对比

这个问题在 OS 社区中并不罕见。Linux 内核通过在链接器脚本中显式指定 `.head.text` 段来保证内核入口点（`startup_64`）的位置，和我们的 `.text.start` 方案在思路上完全一致。xv6 更简单——它的入口点直接写在 `entry.S` 中，链接器脚本用 `ENTRY(entry)` 指定入口符号，bootloader（`bootmain.c`）从 ELF header 的 `e_entry` 字段读取入口地址，所以链接顺序不会造成问题。SerenityOS 的 bootloader（`Bootloader/BootDDRELFLoader.cpp`）同样解析 ELF header 获取入口点，避免了硬编码地址的脆弱性。

说到底，我们的问题的根源是 bootloader 硬编码了跳转地址。更健壮的做法是让 bootloader 解析 ELF header 中的 `e_entry` 字段——这是 xv6 和 SerenityOS 都采用的方式。但在当前的 flat binary 加载模式下（`objcopy -O binary` 把 ELF 转成了裸二进制），ELF header 在运行时已经不存在了，所以硬编码地址是唯一的选择。`.text.start` 段的修复方案在当前约束下是最合理的。

## 收尾

这两个 bug 给我上了生动的一课：内核开发中，"编译通过"完全不等于"代码正确"。链接器符号的访问陷阱和对象库的链接顺序问题，都不会在编译期产生任何错误或警告。它们只在运行时表现出诡异的行为——一个让你读到 0 的内核大小，一个让你完全没输出的串口。debugcon 端口（`outb %al, $0xE9`）在这种时候就是救命工具，它让你能在没有任何基础设施的情况下追踪执行流程。

## 参考资料

- OSDev Wiki: [Writing A Page Frame Allocator](https://wiki.osdev.org/Writing_A_Page_Frame_Allocator) — 链接器符号 `endkernel` 的正确访问方式
- OSDev Wiki: [Linker Scripts](https://wiki.osdev.org/Linker_Scripts) — 链接脚本中段放置规则
- LD Manual: [Simple Assignments](https://sourceware.org/binutils/docs/ld/Simple-Assignments.html) — 链接器符号赋值的语义
- LD Manual: [SECTIONS Command](https://sourceware.org/binutils/docs/ld/SECTIONS.html) — 段通配符的匹配顺序
- Linux kernel: `arch/x86/kernel/vmlinux.lds.S` — `.head.text` 段的使用
- xv6: `kernel/entry.S` + `kernel/kernel.ld` — `ENTRY(entry)` 指定入口符号
