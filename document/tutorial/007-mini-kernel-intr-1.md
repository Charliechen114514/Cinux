---
title: 007-mini-kernel-intr-1 · 中断处理
---

# 007-1 Tutorial: GDT 与 IDT — 让内核学会"接电话"的基础设施

> 标签：GDT, IDT, 段描述符, 中断描述符表, x86_64, LGDT, LIDT
> 前置：[006 Mini Kernel PMM](./006-mini-kernel-pmm-1.md)

## 前言

说实话，写完上一章的 PMM 之后我心里有一种不太踏实的感觉——内核虽然能分配内存了，但它仍然是个"聋子"。CPU 遇到任何异常，不管是除零、非法指令还是缺页，唯一的反应就是 triple fault 然后重启，连个遗言都留不下。我辛辛苦苦写了一堆代码，跑起来直接黑屏，连出错的位置都不知道，这在内核开发阶段简直是家常便饭。我就在这里卡过好几次——明明只是一个小小的数组越界，结果整个 QEMU 直接 reset，连串口输出的最后一行都看不到。

所以这一章我们要给内核装上"耳朵和嘴巴"。具体来说，我们要做两件事：第一，重新搭建 GDT（全局描述符表），让段寄存器有东西可指；第二，搭建 IDT（中断描述符表），让 CPU 知道遇到异常该找谁处理。完成这两步之后，下一篇我们再加入 ISR Stub 和异常处理函数，就能在串口看到完整的寄存器 dump 了——触发异常不死机，能看到错误信息，这就是 milestone 007 的目标。

本章共三篇。本篇覆盖 GDT 和 IDT 的数据结构与初始化。第二篇覆盖 ISR Stub 汇编。第三篇覆盖 C 异常处理函数和 int $3 点火测试。

这一篇聚焦 GDT 和 IDT 的数据结构与初始化。整个中断处理链路的前半段——"告诉 CPU 去哪里找处理程序"——就在这两张表里。

## 环境说明

我们当前的开发环境是 GNU AS（AT&T 语法）+ GCC/G++ + CMake，目标是 x86_64 freestanding 环境。内核运行在 QEMU 的 higher-half 配置下（内核被加载到 -2GB 附近的虚拟地址）。mini kernel 是一个简化版的内核，不依赖标准库、不使用异常处理、不使用 RTTI，所有代码都是 ring 0 内核态执行。本篇涉及的所有代码都在 tag `007_mini_kernel_intr` 下，需要对照上一章的 `006_mini_kernel_pmm` 来理解增量变更。

一个很关键的约束是 `-mno-red-zone`——这个编译选项告诉我们不能使用 x86_64 的 red zone（栈顶以下 128 字节的"安全区域"），因为中断可能在任何时刻发生，如果用了 red zone，中断处理程序的栈操作会覆盖掉被中断函数的局部变量。在内核开发中这个选项是必须的。

## 从零开始——为什么 Bootloader 的 GDT 不够用

很多人会问：Bootloader 在进入 long mode 的时候不是已经设过 GDT 了吗？为什么内核还要再来一遍？这是个好问题，答案涉及到 x86 架构的一个不太优雅的设计——段寄存器的"隐藏部分"。

CPU 内部的每个段寄存器（CS、DS、SS 等）除了可见的选择子值之外，还有一个用户不可见的"描述符缓存"——它缓存了从 GDT 中读取的 base address、limit 和 access rights。当 Bootloader 执行 LGDT 加载自己的 GDT 并用 far jump 刷新 CS 之后，CS 的缓存就指向了 Bootloader GDT 中的代码段描述符。但当内核接管控制权后，Bootloader 的 GDT 所在的内存区域可能被覆盖或者地址映射发生变化——虽然 CPU 内部的缓存不会立刻失效，但任何后续的 LGDT/LIDT 操作或者中断触发的段寄存器重载都会重新从内存中读取 GDT。所以内核必须在可控的地址空间里建立自己的 GDT，确保任何时候 CPU 查找段描述符都能找到有效的数据。

你可以把整个查找链路想象成一次函数调用：中断触发 -> CPU 用向量号查 IDT 得到 selector 和 handler 地址 -> 用 selector 查 GDT 得到 code segment 属性 -> 跳转执行。这条链路上任何一环出了问题都会 triple fault，所以 GDT 必须在 IDT 之前配好，而且内容必须正确。

## 上号——搭建 GDT 数据结构

我们先来看 GDT 的头文件。Cinux 把 GDT 相关的代码放在 `kernel/mini/arch/x86_64/gdt.hpp` 中，整个文件包裹在 `cinux::mini::arch` 命名空间里。

常量定义部分很简单——GDT 只有三项条目，对应的段选择子分别是 0x00（null）、0x08（code64）和 0x10（data64）。选择子的值等于索引乘以 8，这是因为段选择子的格式是 `[Index(13位) : TI(1位) : RPL(2位)]`，TI=0 表示 GDT，RPL=0 表示 ring 0，低 3 位全零。

```cpp
constexpr uint8_t GDT_ENTRIES = 3;
constexpr uint8_t GDT_NULL_INDEX  = 0;
constexpr uint8_t GDT_CODE64_INDEX = 1;
constexpr uint8_t GDT_DATA64_INDEX = 2;
constexpr uint16_t SEGMENT_NULL  = GDT_NULL_INDEX  * 8;   // 0x00
constexpr uint16_t SEGMENT_CODE64 = GDT_CODE64_INDEX * 8;  // 0x08
constexpr uint16_t SEGMENT_DATA64 = GDT_DATA64_INDEX * 8;  // 0x10
```

数据结构部分有两个结构体。`GdtEntry` 是 8 字节的 packed 结构，对应 x86 的 64 位段描述符。字段排列看起来有些奇怪——limit 和 base 各被拆成了好几段——这是 80286 时代的遗留设计，Intel 为了向后兼容一直保留到现在。不过在 long mode 下 base 和 limit 被硬件忽略（除了 GS/FS 的 base 通过 MSR 设置），我们真正关心的只有 access 和 flags_limit_high 这两个字节。`GdtPointer` 是给 LGDT 指令用的操作数格式：2 字节 limit（GDT 字节数减 1）加 8 字节 base address。

```cpp
struct GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed));

struct GdtPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
```

这两个结构体都用了 `__attribute__((packed))`，这是内核开发中不可跳过的步骤——如果不加 packed，编译器可能会在字段之间插入填充字节来满足对齐要求，导致内存布局和硬件期望的格式不一致。packed 的一个副作用是未对齐访问可能稍慢，但在内核中和硬件交互时，packed 是必须的，因为硬件期望的格式不允许有填充。

## 爆改——GDT 初始化实现

`gdt.cpp` 的核心是 `gdt_init()` 函数，它负责填写三项描述符、构造 GdtPointer、然后用内联汇编执行 LGDT 加载并刷新段寄存器。

三项描述符的关键值值得仔细拆解。第一项 null descriptor 全零，x86 架构硬性要求索引 0 的描述符必须是全零——CPU 不允许使用选择子 0x00 访问任何段，如果尝试访问会触发 #GP。第二项 code64 的 access 是 `0x9A`，拆开来看是二进制 `10011010`：Present=1（段在内存中）、DPL=00（ring 0 内核态）、S=1（代码/数据段，不是系统段）、Type=`1010`（可执行且可读的代码段）。Flags 是 `0x0A`，即 G=1（4KB 粒度）和 L=1（64-bit long mode 标志）——这个 L 位是整个 GDT 中最关键的一个 bit，它告诉 CPU 这是一个 64 位代码段，这是 long mode 正常工作的前提条件。Intel SDM Vol.3A 3.4.5 对此有明确说明：当 L=1 时 D/B 必须为 0，否则 CPU 会触发 #GP。

```cpp
s_gdt[GDT_NULL_INDEX]  = make_gdt_entry(0, 0, 0, 0);
s_gdt[GDT_CODE64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A);
s_gdt[GDT_DATA64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C);
```

data segment 的 access 是 `0x92`，和 code 的区别在于 bit 3（Executable）是 0，表示这是一个数据段。flags 是 `0x0C`（G=1 + D/B=1），在 long mode 下 data segment 的 L 位和 D/B 位基本被硬件忽略，填 0x0C 是大多数内核项目的惯例。

加载 GDT 的汇编部分用了 far return 的技巧来刷新 CS。单纯执行 LGDT 只修改了 GDTR 寄存器，但 CPU 内部的 CS 缓存不会自动更新。所以我们需要一次"远跳转"来强制 CPU 重新加载 CS 的描述符缓存。这里选择 `lretq` 而不是 `ljmp`，是因为在 higher-half kernel 里 `ljmp` 需要一个绝对地址，而 `lretq` 可以用栈上的 RIP 相对地址（`leaq 1f(%%rip), %%rax`），对位置无关代码更友好。DS/ES/FS/GS/SS 则不需要这种技巧，直接用 mov 赋值就行。

```cpp
__asm__ volatile (
    "lgdt %[gdtr]\n\t"
    "pushq %[cs]\n\t"
    "leaq 1f(%%rip), %%rax\n\t"
    "pushq %%rax\n\t"
    "lretq\n\t"
    "1:\n\t"
    "movw %[ds], %%ax\n\t"
    "movw %%ax, %%ds\n\t"
    "movw %%ax, %%es\n\t"
    "movw %%ax, %%fs\n\t"
    "movw %%ax, %%gs\n\t"
    "movw %%ax, %%ss\n\t"
    :
    : [gdtr] "m" (s_gdt_pointer), [cs] "i" (SEGMENT_CODE64), [ds] "i" (SEGMENT_DATA64)
    : "rax", "memory"
);
```

## 下一步——搭建 IDT

GDT 搭好之后，接下来是 IDT。IDT 的数据结构比 GDT 大一些——每个条目 16 字节（而不是 8 字节），因为需要存放 64 位的处理程序地址。地址被拆成了三段（offset_low 16位 + offset_mid 16位 + offset_high 32位），这是 Intel SDM Figure 6-8 中定义的 64 位 IDT gate 描述符格式。

`IdtEntry` 的 selector 字段指向 GDT 中的代码段——填的就是我们刚才设置的 `SEGMENT_CODE64`（0x08）。`type_attr` 字段编码了 Present（bit 7）、DPL（bits 6-5）和 Gate Type（bits 3-0）。我们给 #BP 配陷阱门（type_attr=0x8F），这意味着进入处理程序时 IF 不被清除，允许在断点处理期间响应其他中断；给 #PF 配中断门（0x8E），CPU 会自动关中断，防止页错误的处理过程被其他中断打断。Intel SDM Vol.3A 6.12.1.3 对这两种门的区别有精确描述：中断门自动清除 EFLAGS.IF，陷阱门不影响 IF。

`InterruptFrame` 是整个中断处理链路中最关键的数据结构。前 15 个字段（r15 到 rax）由 ISR stub 手动保存，error_code 由 stub 处理（有硬件错误码的就保留 CPU 压入的值，没有的就压一个 0 填位），最后 5 个字段（rip 到 ss）由 CPU 自动压入。这个布局必须和 interrupts.S 中的 push 顺序严格对应——任何错位都会导致 C 处理函数读到错误的寄存器值，而且这种 bug 非常隐蔽，因为它不会立刻 crash，而是让你的寄存器 dump 全部显示错误的值。

`idt_init()` 的实现很直白：先清空 256 个条目，然后只配置两个向量。#BP 用陷阱门（0x8F），#PF 用中断门（0x8E）。最后构造 IDTR 并执行 LIDT。和 LGDT 不同的是，LIDT 之后不需要刷新操作——IDTR 不是段寄存器，它的内容在下一次中断触发时自然生效。清空全部 256 个条目的原因有两点：一是确保未配置的向量触发时会产生 #GP（而不是随机跳转到垃圾地址），二是防止 BSS 段残留数据被误读为有效的 IDT 条目。

```cpp
void idt_init() {
    for (uint16_t i = 0; i < 256; i++)
        s_idt[i] = IdtEntry{};

    set_idt_entry(3, reinterpret_cast<void*>(isr_bp_stub), SEGMENT_CODE64, 0x8F, 0);
    set_idt_entry(14, reinterpret_cast<void*>(isr_pf_stub), SEGMENT_CODE64, 0x8E, 0);

    s_idt_pointer.limit = static_cast<uint16_t>(sizeof(s_idt) - 1);
    s_idt_pointer.base  = reinterpret_cast<uint64_t>(&s_idt);
    __asm__ volatile ("lidt %[idtr]" : : [idtr] "m" (s_idt_pointer) : "memory");
}
```

## 别人怎么做的

### xv6 的方式

xv6 在 `tvinit()` 中一次性配置全部 256 个 IDT 条目，使用 Perl 脚本（vectors.pl）在构建时自动生成 256 个 ISR stub。所有向量默认使用 Interrupt Gate（istrap=0），只有系统调用向量 T_SYSCALL 使用 Trap Gate 并设置 DPL=DPL_USER 允许用户态触发。所有 stub 最终汇入同一个 `alltraps` 入口点，由统一的 `trap(struct trapframe *tf)` 函数进行分发。

这种"一步到位"的方案在代码组织上很优雅——一个分发函数处理所有情况，添加新的中断类型只需要在 switch 里加一个 case。但对初学者来说信息量很大。Cinux 选择了更渐进的方式——tag 007 只配 2 个向量，ISR stub 手写汇编宏，先跑通核心机制再说。等到 tag 010 需要 256 个向量时，可以回头参考 xv6 的 vectors.pl 方案做扩展。

数据结构层面也有差异：xv6 是 32 位 x86 系统，`gatedesc` 只有 8 字节（32 位 IDT 条目），Cinux 的 `IdtEntry` 是 16 字节（64 位 IDT 条目，地址分三段）。xv6 的 GDT 有 6 项（含 TSS 和用户态段），Cinux 只有 3 项（null/code64/data64）。

### SerenityOS 的方式

SerenityOS 代表了 x86_64 OS 中断系统的"完整形态"。它使用了完整的面向对象中断框架——`GenericInterruptHandler` 基类允许动态注册和注销中断处理函数，每种中断类型一个子类。中断控制器使用 Local APIC + I/O APIC（而不是传统 8259 PIC），支持 SMP 多核中断路由。每个 CPU 有独立的 GDTR/IDTR，AP 启动时从 BSP 复制 GDT/IDT。

SerenityOS 的设计在工程上很成熟，但抽象层次很高。对于想要理解 x86 中断硬件机制的人来说，面向对象的 Handler 基类、动态注册机制和 APIC 集成反而遮蔽了底层发生了什么。Cinux 保持裸机级别的直接操作——手写汇编 ISR stub、静态填写 IDT 条目、直接调用 C 函数——让学习者能清楚地看到 CPU 触发异常后每一个指令在做什么。

两者共享的核心机制是完全相同的：IDT gate 描述符 -> ISR stub 保存寄存器 -> 构造 interrupt frame -> C 处理函数 -> IRETQ 返回。区别在于 SerenityOS 在这之上添加了大量的抽象层，而 Cinux 选择保持最小化。

## GDT 在 x86_64 下的角色变迁

理解 GDT 在 x86_64 下的"退化"过程，有助于我们理解为什么只需要 3 项就足够了。

在 16 位实模式下，GDT 是内存管理的唯一手段——每个段寄存器通过 GDT 获得基地址，实际地址 = 基地址 + 偏移量。分段是强制性的，没有分页。

在 32 位保护模式下，分页机制引入了，但分段仍然存在且有效。操作系统可以选择"平坦内存模型"（base=0, limit=4GB），让分段形同虚设，也可以利用分段做额外的隔离。

到了 64 位 long mode，Intel 基本放弃了分段——除了 GS/FS 的 base 通过 MSR 设置（用于 per-CPU 数据或线程本地存储），所有其他段的 base 和 limit 都被硬件忽略，强制为 0。但 CS 的 L 位（Long mode flag）仍然必须正确设置，否则 CPU 不知道这是 64 位代码。

所以我们的 GDT 虽然只有 3 项，但每一项都有其存在的理由：null 是架构要求，code64 提供了 L=1 标志和权限检查，data64 给 DS/ES/FS/GS/SS 提供了有效的选择子。

## IDT 在 x86_64 下的变化

和 32 位模式相比，64 位模式下的 IDT 有几个关键差异需要了解：

1. **条目大小翻倍**：32 位 IDT 条目（gatedesc）是 8 字节，64 位是 16 字节。因为处理程序地址从 32 位变成了 64 位，需要拆成三段存放。
2. **IST 字段新增**：64 位 IDT 条目新增了 IST（Interrupt Stack Table）偏移字段，允许为特定中断指定独立栈。
3. **IRETQ 代替 IRET**：64 位模式下中断返回必须用 IRETQ（弹出 64 位的 RIP/CS/RFLAGS/RSP/SS），32 位用 IRET（弹出 32 位的 EIP/CS/EFLAGS/ESP/SS）。
4. **SS:RSP 无条件压入**：64 位模式下，即使特权级没有变化（比如 ring 0 到 ring 0），CPU 也会无条件压入 SS 和 RSP。32 位模式下只有在特权级变化时才压入 SS:ESP。
5. **TSS 描述符占两个 slot**：64 位 TSS 描述符是 128 位，需要两个连续的 GDT slot。

这些差异使得 64 位的中断处理代码无法直接从 32 位内核复制——每一个数据结构和汇编指令都需要适配。这也是为什么 Cinux 的 IdtEntry 是 16 字节而不是 8 字节，以及为什么我们使用 IRETQ 而不是 IRET。

## 文件清单

本篇涉及的源文件如下：

| 文件 | 职责 | 关键函数/结构 |
|------|------|-------------|
| `kernel/mini/arch/x86_64/gdt.hpp` | GDT 头文件 | GdtEntry, GdtPointer, gdt_init() |
| `kernel/mini/arch/x86_64/gdt.cpp` | GDT 实现 | make_gdt_entry(), gdt_init() |
| `kernel/mini/arch/x86_64/idt.hpp` | IDT 头文件 | IdtEntry, IdtPointer, InterruptFrame, idt_init() |
| `kernel/mini/arch/x86_64/idt.cpp` | IDT 实现 | set_idt_entry(), idt_init() |
| `kernel/mini/CMakeLists.txt` | 构建配置 | 添加 gdt.cpp 和 idt.cpp |
| `kernel/mini/main.cpp` | 主函数 | 添加 gdt_init() 和 idt_init() 调用 |

总共约 423 行新增代码（85+111+112+115），加上 main.cpp 的变更约 30 行。

## 收尾

到这里 GDT 和 IDT 都已经搭好了。验证方式很简单——构建运行后应该依次看到 "Setting up GDT" / "GDT loaded successfully" / "Setting up IDT" / "IDT loaded successfully" 的串口输出。如果中间某一步 triple fault 了，多半是 GDT 的 Access Byte 编码错误或者 IDT 的 selector 不对。

不过现在 IDT 指向的 ISR stub 还没写——下一篇我们会编写汇编 ISR stub、C 异常处理函数，最后用 `int $3` 做一次完整的点火测试。等到那时，我们才能在串口看到完整的寄存器 dump，验证整个中断处理链路是通的。

## 参考资料

- Intel SDM: Vol.3A 3.4.5 (Segment Descriptors) -- Access Byte 和 Flags 编码
- Intel SDM: Vol.3A 6.14.1 (pp.6-20) -- 64-Bit Mode IDT Gate Descriptors (Figure 6-8)
- Intel SDM: Vol.3A 6.12.1.3 (p.6-17) -- Interrupt Gate vs Trap Gate 对 IF 标志的影响
- Intel SDM: Vol.3A 2.4.3 (Figure 2-6) -- IDTR Register
- OSDev Wiki: [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)
- xv6: [trap.c](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) -- tvinit/idtinit/trap
