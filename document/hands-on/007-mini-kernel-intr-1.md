# 007-1 Hands-on: GDT 与 IDT — 内核的段寄存器配置与中断向量表

> 标签：GDT, IDT, 段描述符, 中断描述符, LIDT, LGDT
> 前置：[006 Mini Kernel PMM](./006-mini-kernel-pmm-1.md)

## 导语

上一章我们搞定了物理内存管理器，内核能分配和释放内存了。但说实话，目前这个内核还是一个"聋子"——CPU 遇到异常的时候，比如除零、缺页、断点，只会茫然地 triple fault 然后重启，连个遗言都留不下。你写了半天的代码跑起来直接黑屏，连哪里出的问题都不知道，这在开发阶段简直是灾难。

所以这一章我们要给内核装上"耳朵和嘴巴"——先搭 GDT（全局描述符表）让段寄存器有东西可指，再搭 IDT（中断描述符表）让 CPU 知道遇到异常该找谁处理。完成本篇后，我们会在 main 里用一条断点指令故意触发异常，然后在串口看到完整的寄存器 dump。触发异常不死机，能看到错误信息，这就是本篇和下一篇共同的目标。

本篇聚焦 GDT 和 IDT 的数据结构与初始化。下一篇覆盖 ISR Stub、异常处理函数和整合测试。前置知识是上一章的 PMM 初始化流程，以及对 x86 段寄存器的基本了解。

---

## 概念精讲

### 为什么需要 GDT？Bootloader 不是已经设过了吗？

这是个好问题。Bootloader 在进入 long mode 时确实设置过一次 GDT，但那是 Bootloader 自己的 GDT，放在低地址区域。现在内核已经接管了控制权，我们需要在自己的地址空间里重新建立一套 GDT，确保段寄存器指向正确的描述符。

你可以把 GDT 理解为一张"段寄存器配置表"。在 x86_64 的 long mode 下，分段机制已经被大幅弱化——基地址和限长基本被硬件忽略了，但 CS/DS/SS 这些段寄存器仍然需要指向有效的 GDT 条目，CPU 才能正常工作。尤其是中断处理这一步，IDT 里的每个条目都有一个"代码段选择子"字段，它指向 GDT 中的 code segment——如果 GDT 没配好，中断一触发就 triple fault。

CPU 触发异常时的查找链路是这样的：CPU 异常发生 → 用向量号查 IDT 得到 selector + handler 地址 → 用 selector 查 GDT 得到 code segment 属性 → 跳转到 handler 执行。这条链路上任何一环出了问题都会导致 triple fault，所以 GDT 必须在 IDT 之前配好。

### IDT 是什么？为什么只需要配两个向量？

IDT（Interrupt Descriptor Table）是 x86 架构的"中断电话簿"，最多可以放 256 个条目（向量 0-255），每个条目告诉 CPU 遇到对应中断时该跳到哪里去执行处理程序。CPU 遇到异常或收到外部中断时，会拿着一个向量号去 IDT 里查对应的处理程序地址。

对我们这个 milestone 来说，只需要配置两个向量就够了：向量 3（#BP，断点异常，`int $3` 指令触发）和向量 14（#PF，页错误异常，访问无效内存地址时触发）。#BP 方便我们测试——触发后能安全返回继续执行；#PF 则是未来调试内存问题最重要的工具。其余 254 个向量暂时保持清零状态（Present=0），访问它们会触发 General Protection Fault，这符合我们"最小可用子集"的渐进策略。

### Interrupt Gate 和 Trap Gate 的区别

IDT 里有两种门：中断门（Interrupt Gate，类型值 0xE）和陷阱门（Trap Gate，类型值 0xF）。它们在硬件层面的区别只有一个：跳转到中断门处理程序时，CPU 会自动清除 RFLAGS.IF 标志（关中断），而陷阱门不会。对于 #BP 这种调试用的异常，我们用陷阱门，这样断点处理期间仍然能响应其他中断；对于 #PF，我们用中断门，因为页错误的处理过程不应该被其他中断打断。这个区别虽然小，但在复杂的中断嵌套场景下会变得很重要。

### Access Byte 和 Flags 的编码

GDT 描述符中最核心的字段是 Access Byte 和 Flags。Access Byte 告诉 CPU 这个段是什么类型、什么权限级别。以 code64 段为例，Access Byte 值为 0x9A，拆开来看是二进制 10011010——从高位到低位依次是 Present（1，段在内存中）、DPL（00，ring 0 内核态）、Descriptor Type（1，代码/数据段）、Executable（1，代码段）、Direction/Conforming（0）、ReadWrite（1，可读）、Accessed（0，CPU 自动设置）。

Flags 中最关键的是 L 位（Long mode），这是 64-bit code segment 的标志，必须为 1，否则 CPU 不知道这是 64 位代码。data segment 则不需要 L 位，它用 D/B 位。这些位的编码你不用死记硬背，但理解它们的含义对于调试 GDT 相关的问题非常重要。

---

## 动手实现

### Step 1: 搭建 GDT 头文件

**目标**：创建 GDT 的数据结构和接口声明。

**设计思路**：GDT 是一个数组，每个条目 8 字节，我们需要一个描述符结构体和一个指针结构体（给 LGDT 指令用）。段选择子的值等于索引乘以 8（因为 TI=0 表示 GDT，RPL=0 表示 ring 0）。

**实现约束**：你需要定义三个常量——GDT 条目数量（3 项）、code64 段选择子（索引 1 × 8 = 0x08）和 data64 段选择子（索引 2 × 8 = 0x10）。描述符结构体需要包含 limit_low（16位）、base_low（16位）、base_middle（8位）、access（8位）、flags_limit_high（8位，高4位flags + 低4位limit高位）和 base_high（8位），使用 packed 属性。指针结构体包含 limit（16位，GDT 字节数减1）和 base（64位，GDT 的线性地址），也是 packed。

**踩坑预警**：结构体必须用 `__attribute__((packed))` 标注，否则编译器可能添加填充字节导致内存布局与硬件期望不一致。所有数值类型用 `<stdint.h>` 中的精确宽度类型（uint16_t, uint32_t, uint64_t）。

**验证**：此步没有运行时验证，编译通过即可。

### Step 2: 实现 GDT 初始化函数

**目标**：填写三项 GDT（null / code64 / data64），执行 LGDT 加载，刷新所有段寄存器。

**设计思路**：用一个辅助函数构造描述符条目——把 base、limit、access、flags 四个参数按 x86 格式拆分到对应字段。然后填写三项：null 全零、code64（access=0x9A, flags=0x0A，关键是 L=1）、data64（access=0x92, flags=0x0C）。最后构造 GDTR 并用内联汇编执行 LGDT。

**实现约束**：加载 GDTR 后需要刷新 CS 寄存器，方法是用 far return（lretq）的技巧——先把新的 CS 选择子压栈，再把返回地址压栈，执行 lretq，CPU 就从栈上弹出新的 CS 和 RIP。DS/ES/FS/GS/SS 直接用 mov 赋值即可。

**踩坑预警**：千万别忘了刷新 CS！单纯执行 LGDT 只修改了 GDTR 寄存器，CPU 内部的 CS 缓存并不会因此更新。如果不刷新 CS，后续中断触发时 CPU 用旧的 CS 去查 GDT 会查到错误的数据。另外注意在 higher-half kernel 里，GDT 的 base 地址应该是虚拟地址而不是物理地址。

**验证**：构建运行后应该看到串口输出 GDT loaded successfully 的信息。

### Step 3: 搭建 IDT 头文件

**目标**：创建 IDT 的数据结构、InterruptFrame 和接口声明。

**设计思路**：x86_64 下每个 IDT 条目 16 字节（而不是 8 字节），因为 handler 地址是 64 位的，被拆成了三段存放。除了描述符结构体外，还需要定义 InterruptFrame——这是中断处理的核心数据结构，前 15 个通用寄存器字段由 ISR stub 保存，error_code 由 stub 处理，最后 5 个字段（rip, cs, rflags, rsp, ss）由 CPU 自动压入。

**实现约束**：IDT 条目需要 offset_low（16位）、selector（16位）、ist（8位）、type_attr（8位）、offset_mid（16位）、offset_high（32位）、reserved（32位），packed。InterruptFrame 的字段顺序是 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rdx, rcx, rbx, rax（ISR stub 保存的），然后 error_code（stub 填的或 CPU 压的），最后 rip, cs, rflags, rsp, ss（CPU 压的）。

**踩坑预警**：InterruptFrame 的字段顺序必须和汇编中的 push 顺序严格对应。结构体的第一个字段对应栈的最低地址（最后 push 的值），最后一个字段对应栈的最高地址（最先 push 的值）。任何错位都会导致 C 处理函数读到错误的寄存器值。这一点下一篇讲到 ISR Stub 时会再强调。

**验证**：编译通过即可。

### Step 4: 实现 IDT 初始化函数

**目标**：清空 256 项 IDT，配置 #BP(3) 和 #PF(14) 两个向量，执行 LIDT 加载。

**设计思路**：用一个辅助函数设置 IDT 条目——把 handler 的 64 位地址拆成三段，设置 selector 为 code64 段选择子、IST 为 0、type_attr 为对应的门类型。#BP 用陷阱门（type_attr=0x8F），#PF 用中断门（type_attr=0x8E）。最后构造 IDTR 并用 LIDT 加载。

**实现约束**：IDT 条目中的 selector 字段必须填 GDT 中 code segment 的选择子（0x08）。type_attr 的高 4 位中 Present 位必须为 1，否则 CPU 会触发 #GP。ISR stub 的地址用 `reinterpret_cast<uint64_t>` 转换后拆分。

**踩坑预警**：IDT 初始化必须在 GDT 初始化之后！虽然 LIDT 本身不检查 selector 的有效性，但中断触发时 CPU 会用 IDT 条目中的 selector 去查 GDT——如果此时 GDT 还是空的，查到的就是全零的 null descriptor（Present=0），直接触发 #GP。初始化顺序如果搞反了，你会收获一个没有任何输出的 triple fault 重启。

**验证**：构建运行后应该看到串口输出 IDT loaded successfully 的信息。

### Step 5: 更新 CMakeLists.txt 和 main.cpp

**目标**：把新文件加入构建系统，在 mini_kernel_main 中按正确顺序调用 gdt_init 和 idt_init。

**设计思路**：main.cpp 中先调用 gdt_init()（必须在最前面），然后 idt_init()，最后 pmm::init()。每步之间打印状态信息方便调试。

**验证**：完整构建并运行后应该依次看到 Setting up GDT / GDT loaded successfully / Setting up IDT / IDT loaded successfully / PMM 信息。

---

## 构建与运行

```bash
# 从项目根目录
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
```

**期望输出（前半部分）**：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xFFFFFFFF80020000, kernel_phys_base=0x20000
...
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[MINI] PMM: Total 131040 pages (511 MB), Free 130784 pages (510 MB)
```

注意目前 `int $3` 测试还没有接入（下一篇完成 ISR Stub 后才会启用），所以这一步之后内核会直接进入 halt 循环。

---

## 调试技巧

**Triple fault 直接重启，没有任何输出**

这是最常见的"灾难"，意味着在处理第一个异常的过程中又触发了异常（通常是 #GP），CPU 直接 reset。排查方法是用 `make run-debug` 启动 QEMU 调试模式，连 GDB 后在 gdt_init 和 idt_init 处打断点，单步跟踪。也可以检查 s_gdt_pointer.base 的值是否正确——在 higher-half kernel 里，GDT 地址需要是虚拟地址。

**LGDT/LIDT 之后一触发异常就 crash**

检查 IDT 条目中的 selector 是否与 GDT 一致。如果 code segment 不在 GDT 索引 1，或者 GDT 没有正确加载，selector 查到的就是 null descriptor（全零，Present=0），直接触发 #GP。

---

## 本章小结

| 组件 | 关键概念 | 说明 |
|------|----------|------|
| GDT | GdtEntry, GdtPointer, lgdt, lretq | 3 项表（null/code64/data64），L=1 是 64 位代码段标志 |
| IDT | IdtEntry, IdtPointer, lidt | 256 槽，只配 #BP(3) Trap Gate 和 #PF(14) Interrupt Gate |
| 段选择子 | index × 8 + RPL | 0x08 = code64, 0x10 = data64 |
| Access Byte | P/DPL/S/Type | 0x9A = 代码段, 0x92 = 数据段 |
| 初始化顺序 | GDT → IDT → PMM | IDT 的 selector 依赖 GDT |
