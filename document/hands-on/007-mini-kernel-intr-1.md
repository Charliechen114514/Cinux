---
title: 007-mini-kernel-intr-1 · 中断处理
---

# 007-1 Hands-on: GDT — 内核的段寄存器配置表

> 标签：GDT, 段描述符, LGDT, 段选择子, Access Byte, lretq
> 前置：[006 Mini Kernel PMM](./006-mini-kernel-pmm-1.md)

## 导语

上一章我们搞定了物理内存管理器，内核能分配和释放内存了。但说实话，目前这个内核还是一个"聋子"——CPU 遇到异常的时候，比如除零、缺页、断点，只会茫然地 triple fault 然后重启，连个遗言都留不下。你写了半天的代码跑起来直接黑屏，连哪里出的问题都不知道，这在开发阶段简直是灾难。

所以这一章我们要给内核装上"耳朵和嘴巴"——先搭 GDT（全局描述符表）让段寄存器有东西可指，再搭 IDT（中断描述符表）让 CPU 知道遇到异常该找谁处理。完成整个 chapter 后，我们会在 main 里用一条断点指令故意触发异常，然后在串口看到完整的寄存器 dump。触发异常不死机，能看到错误信息，这就是 milestone 007 的目标。

本篇聚焦 GDT 的数据结构与初始化——这是整个中断处理链路的第一环。前置知识是上一章的 PMM 初始化流程，以及对 x86 段寄存器的基本了解。

本篇完成后你会新增两个文件：`kernel/mini/arch/x86_64/gdt.hpp`（头文件）和 `kernel/mini/arch/x86_64/gdt.cpp`（实现）。

### 本章三篇的分工

| 篇 | 聚焦 | 关键产出 |
|----|------|---------|
| 007-1（本篇） | GDT 数据结构与初始化 | gdt.hpp / gdt.cpp，LGDT 加载，段寄存器刷新 |
| 007-2 | IDT 数据结构与初始化 | idt.hpp / idt.cpp，InterruptFrame，LIDT 加载 |
| 007-3 | ISR Stub + 异常处理 + 整合测试 | interrupts.S / exception_handlers.cpp，int $3 点火测试 |

---

## 概念精讲

### 为什么需要 GDT？Bootloader 不是已经设过了吗？

这是个好问题。Bootloader 在进入 long mode 时确实设置过一次 GDT，但那是 Bootloader 自己的 GDT，放在低地址区域。现在内核已经接管了控制权，我们需要在自己的地址空间里重新建立一套 GDT，确保段寄存器指向正确的描述符。

CPU 内部的每个段寄存器（CS、DS、SS 等）除了可见的选择子值之外，还有一个用户不可见的"描述符缓存"——它缓存了从 GDT 中读取的 base address、limit 和 access rights。当 Bootloader 执行 LGDT 加载自己的 GDT 并用 far jump 刷新 CS 之后，CS 的缓存就指向了 Bootloader GDT 中的代码段描述符。但当内核接管控制权后，Bootloader 的 GDT 所在的内存区域可能被覆盖或者地址映射发生变化——虽然 CPU 内部的缓存不会立刻失效，但任何后续的 LGDT/LIDT 操作或者中断触发的段寄存器重载都会重新从内存中读取 GDT。所以内核必须在可控的地址空间里建立自己的 GDT。

你可以把 GDT 理解为一张"段寄存器配置表"。在 x86_64 的 long mode 下，分段机制已经被大幅弱化——基地址和限长基本被硬件忽略了，但 CS/DS/SS 这些段寄存器仍然需要指向有效的 GDT 条目，CPU 才能正常工作。尤其是中断处理这一步，IDT 里的每个条目都有一个"代码段选择子"字段，它指向 GDT 中的 code segment——如果 GDT 没配好，中断一触发就 triple fault。

CPU 触发异常时的查找链路是这样的：CPU 异常发生 -> 用向量号查 IDT 得到 selector + handler 地址 -> 用 selector 查 GDT 得到 code segment 属性 -> 跳转到 handler 执行。这条链路上任何一环出了问题都会导致 triple fault，所以 GDT 必须在 IDT 之前配好。

### 段选择子的编码

段选择子是一个 16 位值，格式是 `[Index(13位) : TI(1位) : RPL(2位)]`。TI=0 表示查 GDT，TI=1 表示查 LDT（Local Descriptor Table，我们在内核中不用）。RPL（Requested Privilege Level）是请求特权级，ring 0 填 0，ring 3 填 3。因为 TI=0 且 RPL=0，所以低 3 位全零，选择子的值就等于索引乘以 8。

我们的 code64 段在 GDT 索引 1，所以选择子是 0x08；data64 段在索引 2，选择子是 0x10。这些值后续在 IDT 初始化时会直接引用——IDT 条目中的 selector 字段填的就是 0x08。

### Access Byte 的逐位解析

Access Byte 告诉 CPU 这个段是什么类型、什么权限级别。以 code64 段为例，Access Byte 值为 0x9A，拆开来看是二进制 10011010：

- Bit 7 - Present（1）：段在内存中。如果为 0，CPU 访问这个段会触发 #NP（Segment Not Present）
- Bits 6-5 - DPL（00）：ring 0 内核态。只有 ring 0 的代码能访问这个段
- Bit 4 - Descriptor Type（1）：代码/数据段。0 表示系统段（如 TSS、LDT），1 表示普通代码/数据段
- Bit 3 - Executable（1）：代码段。0 表示数据段
- Bit 2 - Direction/Conforming（0）：对于代码段，0 表示只能从同特权级调用；对于数据段，0 表示向上扩展
- Bit 1 - Read/Write（1）：对于代码段，1 表示可读（代码段永远不可写）；对于数据段，1 表示可写
- Bit 0 - Accessed（0）：CPU 访问段时自动置 1，我们不用管

data segment 的 Access Byte 是 0x92（10010010），和 code 的区别在于 Bit 3（Executable）是 0，Bit 1 仍然是 1（可读写）。

### Flags 的编码

Flags 字段只有高 4 位有效（低 4 位是 limit 的高 4 位）。三个关键标志位：

- Bit 3 - G（Granularity）：0 = 字节粒度，1 = 4KB 粒度。G=1 时 limit 的单位是 4KB，所以 0xFFFFF * 4KB = 4GB
- Bit 2 - D/B（Default operation size）：在 long mode 下对 code segment 必须为 0
- Bit 1 - L（Long mode）：这是 64-bit code segment 的标志，必须为 1。Intel SDM Vol.3A 3.4.5 明确说明：当 L=1 时 D/B 必须为 0，否则 CPU 会触发 #GP

所以 code64 的 flags = 0x0A（二进制 1010，G=1 + L=1），data64 的 flags = 0x0C（二进制 1100，G=1 + D/B=1）。在 long mode 下 data segment 的 L 位和 D/B 位基本被硬件忽略，填 0x0C 是大多数内核项目的惯例。

### 为什么用 lretq 而不是 ljmp？

单纯执行 LGDT 只修改了 GDTR 寄存器，但 CPU 内部的 CS 缓存不会因此更新。段寄存器的隐藏部分（base、limit、access rights）只有在加载新选择子时才会从 GDT 重新读取。所以我们需要一次"远跳转"来强制 CPU 重新加载 CS。

选择 lretq（far return）而不是 ljmp（far jump）的原因是：在 higher-half kernel 里 ljmp 需要一个绝对地址，而 lretq 可以用栈上的 RIP 相对地址（`leaq 1f(%%rip), %%rax`），对位置无关代码更友好。DS/ES/FS/GS/SS 不需要这种技巧，直接用 mov 赋值就行。

---

## 动手实现

### Step 1: 搭建 GDT 头文件

**目标**：创建 GDT 的数据结构和接口声明。

**设计思路**：GDT 是一个数组，每个条目 8 字节（对应 x86 的段描述符格式），我们需要一个描述符结构体和一个指针结构体（给 LGDT 指令用）。段选择子的值等于索引乘以 8。

**实现约束**：你需要定义常量——GDT 条目数量（3 项，用 uint8_t 类型，命名为 GDT_ENTRIES）、三个索引常量（GDT_NULL_INDEX=0, GDT_CODE64_INDEX=1, GDT_DATA64_INDEX=2，用 uint8_t）、三个段选择子常量（SEGMENT_NULL=0x00, SEGMENT_CODE64=0x08, SEGMENT_DATA64=0x10，用 uint16_t）。选择子的值等于索引乘以 8。

描述符结构体 GdtEntry 需要包含 6 个字段：limit_low（uint16_t）、base_low（uint16_t）、base_middle（uint8_t）、access（uint8_t）、flags_limit_high（uint8_t，高4位flags + 低4位limit高位）和 base_high（uint8_t），使用 `__attribute__((packed))`。指针结构体 GdtPointer 包含 limit（uint16_t，GDT 字节数减1）和 base（uint64_t，GDT 的线性地址），也是 packed。公开接口声明 `void gdt_init()`。所有代码放在 `cinux::mini::arch` 命名空间中，头文件用 `#pragma once` 保护。

**踩坑预警**：结构体必须用 `__attribute__((packed))` 标注，否则编译器可能添加填充字节导致内存布局与硬件期望不一致。limit 和 base 的拆分看起来有些奇怪——这是 80286 时代的遗留设计，为了向后兼容一直保留到现在。所有数值类型用 `<stdint.h>` 中的精确宽度类型（uint16_t, uint8_t, uint64_t）。

**文件路径**：`kernel/mini/arch/x86_64/gdt.hpp`

**验证**：此步没有运行时验证，编译通过即可。

### Step 2: 实现 make_gdt_entry 辅助函数

**目标**：写一个辅助函数把 base、limit、access、flags 四个参数按 x86 格式拆分到 GdtEntry 的对应字段。

**设计思路**：这个函数是纯位操作——limit 的低 16 位用 `limit & 0xFFFF` 赋给 limit_low，limit 的高 4 位和 flags 拼成一个字节 `((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F)` 赋给 flags_limit_high，base 拆成三段分别赋给 base_low（低16位 `base & 0xFFFF`）、base_middle（中8位 `(base >> 16) & 0xFF`）、base_high（高8位 `(base >> 24) & 0xFF`）。

在 long mode 下 base 和 limit 的值其实无所谓（被硬件忽略），我们填 base=0、limit=0xFFFFF（配合 G=1 等于 4GB）是出于惯例。实际上 CPU 只检查 access 和 flags。

**实现约束**：函数签名为 `static GdtEntry make_gdt_entry(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)`，返回填充好的 GdtEntry。声明为 static，限制作用域在当前编译单元内。放在 `cinux::mini::arch` 命名空间中。

**文件路径**：`kernel/mini/arch/x86_64/gdt.cpp`

**验证**：编译通过即可。

### Step 3: 实现 gdt_init 初始化函数

**目标**：填写三项 GDT（null / code64 / data64），执行 LGDT 加载，刷新所有段寄存器。

**设计思路**：维护两个静态全局变量——`s_gdt`（GdtEntry 数组，大小 GDT_ENTRIES）和 `s_gdt_pointer`（GdtPointer 结构）。用 make_gdt_entry 填写三项。null 全零（x86 架构硬性要求索引 0 不可用）。code64 的 access=0x9A, flags=0x0A（关键是 L=1）。data64 的 access=0x92, flags=0x0C。然后构造 GDTR 并用内联汇编执行 LGDT。

**实现约束**：加载 GDTR 后需要刷新 CS 寄存器，方法是用 far return（lretq）的技巧——先把新的 CS 选择子压栈，再把返回地址压栈（用 `leaq 1f(%%rip), %%rax`），执行 lretq，CPU 就从栈上弹出新的 CS 和 RIP。DS/ES/FS/GS/SS 直接用 mov 赋值即可。内联汇编使用命名操作数：`[gdtr] "m" (s_gdt_pointer)` 给 lgdt 用，`[cs] "i" (SEGMENT_CODE64)` 给 pushq 用，`[ds] "i" (SEGMENT_DATA64)` 给 movw 用。clobber 列表包含 "rax" 和 "memory"。

**踩坑预警**：千万别忘了刷新 CS！单纯执行 LGDT 只修改了 GDTR 寄存器，CPU 内部的 CS 缓存并不会因此更新。如果不刷新 CS，后续中断触发时 CPU 用旧的 CS 去查 GDT 会查到错误的数据。另外注意在 higher-half kernel 里，GDT 的 base 地址应该是虚拟地址而不是物理地址。`s_gdt_pointer.base` 的值应该是 `reinterpret_cast<uint64_t>(&s_gdt)`，这里取到的是 higher-half 虚拟地址，正是我们需要的。

**验证**：构建运行后应该看到串口输出 `[INIT] GDT loaded successfully.` 的信息。

### Step 4: 更新 CMakeLists.txt 和 main.cpp

**目标**：把 gdt.cpp 加入构建系统，在 mini_kernel_main 中调用 gdt_init。

**设计思路**：在 `kernel/mini/CMakeLists.txt` 的 `add_library(mini_kernel_common OBJECT ...)` 列表中添加 `arch/x86_64/gdt.cpp`。在 main.cpp 中 include "arch/x86_64/gdt.hpp"，在 PMM 初始化之前调用 `cinux::mini::arch::gdt_init()`，前后各打印一条状态信息方便调试——如果 triple fault 了，你至少能看到是哪一步之前的最后一条输出。

**注意**：gdt_init() 的调用位置必须在 PMM 初始化之前，因为 PMM 初始化本身可能需要正确的段寄存器设置（虽然在当前的 mini kernel 里不太会触发这个问题，但养成好习惯总是对的）。

**验证**：构建运行后应该依次看到 `[INIT] Setting up GDT...` 和 `[INIT] GDT loaded successfully.`。

---

## 概念补充：GDT 在 x86_64 下的角色变迁

理解 GDT 在 x86_64 下的"退化"过程，有助于我们写出正确的代码。

在 16 位实模式下，GDT 是内存管理的唯一手段——每个段寄存器通过 GDT 获得基地址，实际地址 = 基地址 + 偏移量。分段是强制性的，没有分页。

在 32 位保护模式下，分页机制引入了，但分段仍然存在且有效。操作系统可以选择"平坦内存模型"（base=0, limit=4GB），让分段形同虚设，也可以利用分段做额外的隔离。

到了 64 位 long mode，Intel 基本放弃了分段——除了 GS/FS 的 base 通过 MSR 设置（用于 per-CPU 数据或线程本地存储），所有其他段的 base 和 limit 都被硬件忽略，强制为 0。但 CS 的 L 位（Long mode flag）仍然必须正确设置，否则 CPU 不知道这是 64 位代码。

所以我们的 GDT 虽然只有 3 项，但每一项都有其存在的理由：null 是架构要求，code64 提供了 L=1 标志和权限检查，data64 给 DS/ES/FS/GS/SS 提供了有效的选择子。

---

## 构建与运行

```bash
# 从项目根目录
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
```

**期望输出**：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xFFFFFFFF80020000, kernel_phys_base=0x20000
...
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
```

注意目前 IDT 和 `int $3` 测试还没有接入（后续篇章完成后才会启用），所以这一步之后内核会继续执行 PMM 初始化然后进入 halt 循环。

---

## 调试技巧

**Triple fault 直接重启，没有任何输出**

这是最常见的"灾难"，意味着在处理第一个异常的过程中又触发了异常（通常是 #GP），CPU 直接 reset。排查方法是用 `make run-debug` 启动 QEMU 调试模式，连 GDB 后在 gdt_init 处打断点，单步跟踪。也可以检查 s_gdt_pointer.base 的值是否正确——在 higher-half kernel 里，GDT 地址需要是虚拟地址。

**LGDT 之后段寄存器值不对**

检查 lretq 前后的压栈操作是否正确。正确的顺序是：pushq SEGMENT_CODE64 -> leaq 标号地址 -> pushq 该地址 -> lretq。如果顺序错了，CS 会被刷新成错误的值。可以在 GDB 中用 `info registers` 查看 CS/DS/SS 的值——CS 应该是 0x0008，DS/SS 应该是 0x0010。

**编译报错 "error: expected unqualified-id"**

检查命名空间是否正确包裹。gdt.hpp 中的所有声明都应该在 `cinux::mini::arch` 命名空间内，gdt.cpp 中的实现也需要在同一个命名空间（或用 `namespace cinux::mini::arch { ... }`）。

---

## 本章小结

| 组件 | 关键概念 | 说明 |
|------|----------|------|
| GDT | GdtEntry, GdtPointer, lgdt, lretq | 3 项表（null/code64/data64），L=1 是 64 位代码段标志 |
| 段选择子 | index * 8 + RPL | 0x08 = code64, 0x10 = data64 |
| Access Byte | P/DPL/S/Type | 0x9A = 代码段, 0x92 = 数据段 |
| Flags | G/L/D/B | 0x0A = long mode code, 0x0C = data |
| lretq 技巧 | push CS + push RIP + lretq | 刷新 CS 描述符缓存的唯一方法 |
| 初始化顺序 | GDT 必须在 IDT 之前 | IDT 的 selector 依赖 GDT |

## 延伸思考

- **GDT 能不能配更多项？** 当然可以。后续 tag 010 会添加 TSS（Task State Segment），tag 022 会添加用户态 code/data 段。届时 GDT 会从 3 项扩展到 6 项以上。TSS 需要两个 GDT slot（因为 64-bit TSS 描述符占 16 字节，是普通描述符的两倍）。
- **为什么不用 SGDT 读取 Bootloader 的 GDT？** 理论上可以，但 Bootloader 的 GDT 可能被后续内存操作覆盖。自己建立一套 GDT 更安全、更可控，也符合教学目的——理解 GDT 的结构比复用 Bootloader 的更有价值。
- **long mode 下 GDT 有什么用？** 严格来说，long mode 下分段机制几乎完全被分页取代。GDT 的存在主要是为了兼容性和提供 code/data segment 的基本属性（DPL、executable 等）。真正的内存保护由页表（PML4 -> PDPT -> PD -> PT）负责。
- **packed 属性有什么副作用？** `__attribute__((packed))` 阻止编译器对结构体进行对齐填充。这意味着对未对齐字段的访问可能稍慢（某些架构甚至会触发异常，但 x86_64 硬件处理未对齐访问）。在内核中和硬件交互时，packed 是必须的，因为硬件期望的格式不允许有填充。
