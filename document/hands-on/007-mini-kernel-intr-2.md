---
title: 007-mini-kernel-intr-2 · 中断处理
---

# 007-2 Hands-on: IDT — 中断向量表

> 标签：IDT, 中断描述符, LIDT, Interrupt Gate, Trap Gate, InterruptFrame
> 前置：[007-1 GDT](./007-mini-kernel-intr-1.md)

## 导语

上一篇我们把 GDT 搭好了——三项描述符（null/code64/data64）加上 LGDT 加载和段寄存器刷新，内核的段寄存器终于有了正确的指向。这一篇我们继续搭建中断处理的第二环：IDT（中断描述符表）。

IDT 是 x86 架构的"中断电话簿"——CPU 遇到异常时，会拿着向量号去 IDT 里查对应的处理程序地址。我们当前只需要配置两个向量：#BP（向量 3）和 #PF（向量 14）。完成本篇后，IDT 就能告诉 CPU "遇到 #BP 去找 isr_bp_stub，遇到 #PF 去找 isr_pf_stub"——虽然 stub 本身下一篇才写，但表格先配好。

本篇完成后你会新增两个文件：`kernel/mini/arch/x86_64/idt.hpp`（头文件，包含 IdtEntry、InterruptFrame 等关键结构）和 `kernel/mini/arch/x86_64/idt.cpp`（实现，包含 set_idt_entry 和 idt_init）。

---

## 概念精讲

### IDT 是什么？为什么只需要配两个向量？

IDT（Interrupt Descriptor Table）是 x86 架构的中断描述表，最多可以放 256 个条目（向量 0-255），每个条目告诉 CPU 遇到对应中断时该跳到哪里去执行处理程序。CPU 遇到异常或收到外部中断时，会拿着一个向量号去 IDT 里查对应的处理程序地址。

对我们这个 milestone 来说，只需要配置两个向量就够了：向量 3（#BP，断点异常，`int $3` 指令触发）和向量 14（#PF，页错误异常，访问无效内存地址时触发）。#BP 方便我们测试——触发后能安全返回继续执行；#PF 则是未来调试内存问题最重要的工具。其余 254 个向量暂时保持清零状态（Present=0），访问它们会触发 General Protection Fault，这符合我们"最小可用子集"的渐进策略。

将来需要更多向量时（比如 tag 011 添加硬件中断），只需要多调几次 set_idt_entry 就行了。256 个条目的框架已经搭好，扩展成本非常低。

### Interrupt Gate 和 Trap Gate 的区别

IDT 里有两种门：中断门（Interrupt Gate，类型值 0xE）和陷阱门（Trap Gate，类型值 0xF）。它们在硬件层面的区别只有一个：跳转到中断门处理程序时，CPU 会自动清除 RFLAGS.IF 标志（关中断），而陷阱门不会。

对于 #BP 这种调试用的异常，我们用陷阱门，这样断点处理期间仍然能响应其他中断——比如时钟中断不会被屏蔽；对于 #PF，我们用中断门，因为页错误的处理过程不应该被其他中断打断——防止在页表操作中途被另一个中断打断导致状态不一致。

type_attr 字段的编码方式是：高 4 位中 bit 7 是 Present 位（必须为 1，否则 CPU 触发 #GP），bits 6-5 是 DPL（我们用 00 表示 ring 0），bit 4 固定为 0，低 4 位是 Gate Type。所以 #BP 的 type_attr = 0x8F（Present=1, DPL=00, Trap Gate=0xF），#PF 的 type_attr = 0x8E（Present=1, DPL=00, Interrupt Gate=0xE）。

### 64 位 IDT 条目为什么是 16 字节？

在 x86_64 下，每个 IDT 条目 16 字节（而不是 32 位模式下的 8 字节），因为需要存放 64 位的处理程序地址。地址被拆成了三段：offset_low（低 16 位）、offset_mid（中 16 位）、offset_high（高 32 位）。这是 Intel SDM Figure 6-8 中定义的 64 位 IDT gate 描述符格式，没法用一条指令设置完整地址，必须手动拆分。

除了地址外，条目还包含 selector（代码段选择子，指向 GDT 中的 code segment——这里填的就是上一篇设置的 SEGMENT_CODE64 即 0x08）、ist（IST 偏移，填 0 表示不用）、type_attr（类型属性）和 reserved（保留位，必须为 0）。

### InterruptFrame 结构体

InterruptFrame 是整个中断处理链路中最关键的数据结构。前 15 个字段（r15 到 rax）由 ISR stub 手动保存，error_code 也是 stub 处理的——有硬件错误码的异常就保留 CPU 压入的值，没有的就压一个 0。最后 5 个字段（rip 到 ss）是 CPU 自动压入的。

这个布局必须和 interrupts.S 中的 push 顺序严格对应。结构体字段从上到下的声明顺序和 push 顺序是"反过来"的：结构体第一个字段是 r15，但 push 是最后才 push r15。这是因为栈从高地址往低地址增长，而结构体从低地址往高地址排列——先 push 的值在栈的低地址端（栈顶），对应结构体的高地址端（后面声明的字段）。这个顺序问题在下一篇讲 ISR Stub 时会更详细地分析。

---

## 动手实现

### Step 1: 搭建 IDT 头文件

**目标**：创建 IDT 的数据结构、InterruptFrame 和接口声明。

**设计思路**：除了 IDT 描述符结构体外，还需要定义 InterruptFrame——这是中断处理的核心数据结构，前 15 个通用寄存器字段由 ISR stub 保存，error_code 由 stub 处理，最后 5 个字段（rip, cs, rflags, rsp, ss）由 CPU 自动压入。

**实现约束**：定义 IDT 常量：IDT_MAX_ENTRIES = 256（uint16_t），IDT_VEC_BP = 3（uint8_t），IDT_VEC_PF = 14（uint8_t），IDT_TYPE_INTERRUPT_GATE = 0x0E（uint8_t），IDT_TYPE_TRAP_GATE = 0x0F（uint8_t）。IdtEntry 结构体包含 offset_low（uint16_t）、selector（uint16_t）、ist（uint8_t）、type_attr（uint8_t）、offset_mid（uint16_t）、offset_high（uint32_t）、reserved（uint32_t），packed。IdtPointer 结构体包含 limit（uint16_t）和 base（uint64_t），packed。InterruptFrame 的字段顺序是 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rdx, rcx, rbx, rax（ISR stub 保存的），然后 error_code（uint64_t），最后 rip, cs, rflags, rsp, ss（CPU 压的，全是 uint64_t）。公开接口声明 `void idt_init()`。所有代码放在 `cinux::mini::arch` 命名空间中。

**踩坑预警**：InterruptFrame 的字段顺序必须和汇编中的 push 顺序严格对应。结构体的第一个字段（r15）对应栈的最低地址（最后 push 的值），最后一个字段（ss）对应栈的最高地址（最先 push 的值）。任何错位都会导致 C 处理函数读到错误的寄存器值。这一点下一篇讲到 ISR Stub 时会再强调。

**验证**：编译通过即可。

### Step 2: 实现 set_idt_entry 辅助函数

**目标**：写一个辅助函数把 handler 的 64 位地址拆成三段填入 IdtEntry。

**设计思路**：地址拆分的位操作非常直观——低 16 位用 `addr & 0xFFFF`，中 16 位用 `(addr >> 16) & 0xFFFF`，高 32 位用 `(addr >> 32) & 0xFFFFFFFF`。同时设置 selector、ist、type_attr 和 reserved 字段。

**实现约束**：函数签名为 `static void set_idt_entry(uint8_t vector, void* handler, uint16_t selector, uint8_t type_attr, uint8_t ist)`。handler 地址用 `reinterpret_cast<uint64_t>(handler)` 转换后拆分。声明为 static。

**验证**：编译通过即可。

### Step 3: 声明 ISR stub 和 handler 函数

**目标**：在 idt.cpp 中用 extern "C" 声明汇编文件中的 ISR stub 和 C 处理函数。

**设计思路**：因为 interrupts.S 中的符号使用 C 链接约定，必须用 `extern "C"` 声明才能被 C++ 代码引用。ISR stub 符号是 `void isr_bp_stub()` 和 `void isr_pf_stub()`（没有参数，因为它们是被 CPU 直接跳转的，不是被 C 代码调用的），C 处理函数是 `void handle_bp(InterruptFrame* frame)` 和 `void handle_pf(InterruptFrame* frame)`（有参数，由 ISR stub 通过 RDI 传入）。

ISR stub 和 handler 函数的定义不在 idt.cpp 中——stub 在 interrupts.S，handler 在 exception_handlers.cpp。idt.cpp 只做前向声明，以便在 idt_init 中引用它们的地址。

**踩坑预警**：如果忘了 `extern "C"`，C++ 编译器会做 name mangling，把 `handle_bp` 改写成类似 `_Z8handle_bpP16InterruptFrame` 的符号名，链接器就找不到匹配的符号了。这是内核开发中混合 C/C++/汇编时的标准做法，不能省略。

**验证**：编译通过即可。

### Step 4: 实现 idt_init 初始化函数

**目标**：清空 256 项 IDT，配置 #BP(3) 和 #PF(14) 两个向量，执行 LIDT 加载。

**设计思路**：先清空 256 个条目（用循环赋值 `IdtEntry{}`，全零意味着 Present=0，访问时触发 #GP），然后只配置两个向量。#BP 用陷阱门（type_attr=0x8F，即 Present=1 + DPL=00 + Trap Gate=0xF），#PF 用中断门（type_attr=0x8E，即 Present=1 + DPL=00 + Interrupt Gate=0xE）。selector 都填 SEGMENT_CODE64（0x08），ist 都填 0。最后构造 IDTR 并执行 LIDT。

清空所有条目的原因有两点：一是确保未配置的向量触发时会产生 #GP 而不是随机跳转到垃圾地址，二是防止 BSS 段的残留数据被误读为有效的 IDT 条目。虽然全局变量默认零初始化，但显式清零是好习惯。

**实现约束**：两个静态全局变量——`s_idt`（IdtEntry 数组，256 项）和 `s_idt_pointer`（IdtPointer 结构）。set_idt_entry 调用时，ISR stub 地址需要用 `reinterpret_cast<void*>(isr_bp_stub)` 转换（因为函数指针不能隐式转换为 void*）。IDTR 的 limit 是 `static_cast<uint16_t>(sizeof(s_idt) - 1)`（等于 256*16-1 = 4095），base 是 `reinterpret_cast<uint64_t>(&s_idt)`。LIDT 用内联汇编 `"lidt %[idtr]"` 执行，使用命名操作数 `[idtr] "m" (s_idt_pointer)`，clobber 列表包含 "memory"。

**踩坑预警**：IDT 初始化必须在 GDT 初始化之后！虽然 LIDT 本身不检查 selector 的有效性，但中断触发时 CPU 会用 IDT 条目中的 selector 去查 GDT——如果此时 GDT 还是空的，查到的就是全零的 null descriptor（Present=0），直接触发 #GP。初始化顺序如果搞反了，你会收获一个没有任何输出的 triple fault 重启。和 LGDT 不同的是，LIDT 之后不需要任何刷新操作——IDTR 不是段寄存器，它的内容在下一次中断触发时自然生效。

**验证**：构建运行后应该看到串口输出 `[INIT] IDT loaded successfully.` 的信息。

### Step 5: 更新 CMakeLists.txt 和 main.cpp

**目标**：把 idt.cpp 加入构建系统，在 mini_kernel_main 中 gdt_init 之后调用 idt_init。

**注意**：此时链接阶段会报 undefined reference to isr_bp_stub 和 handle_bp。这是预期行为——这些符号在下一篇的 interrupts.S 和 exception_handlers.cpp 中定义。当前阶段可以先忽略链接错误，只要 idt.cpp 本身编译通过就行。等下一篇所有文件都就位后，链接自然会成功。

**设计思路**：在 CMakeLists.txt 的源文件列表中添加 `arch/x86_64/idt.cpp`。在 main.cpp 中 include "arch/x86_64/idt.hpp"，在 gdt_init() 之后调用 `cinux::mini::arch::idt_init()`，前后各打印一条状态信息。

**重要**：idt_init() 必须在 gdt_init() 之后调用。如果反过来，虽然 LIDT 本身不会报错，但后续中断触发时 CPU 拿 IDT 条目中的 selector（0x08）去查 GDT，如果 GDT 还是空的就会查到 null descriptor，触发 #GP，然后 #GP 也没配处理，最终 triple fault。

**验证**：完整构建并运行后应该依次看到 Setting up GDT / GDT loaded successfully / Setting up IDT / IDT loaded successfully / PMM 信息。如果第二步之后 triple fault 了，说明 IDT 的 base 地址可能不对——检查 s_idt_pointer.base 是否是有效的虚拟地址。

---

## 概念补充：x86 异常分类

Intel SDM Table 6-1 把 x86 异常分为三类，理解这个分类对后续编写 ISR stub 非常重要：

| 类型 | 含义 | RIP 指向 | 是否有错误码 | 典型例子 |
|------|------|----------|-------------|---------|
| Fault | 可恢复的错误 | 触发指令（需要重新执行） | 部分有 | #PF(14), #GP(13) |
| Trap | 调试用异常 | 触发指令的下一条 | 无 | #BP(3) |
| Abort | 严重错误，不可恢复 | 不确定 | 无 | #DF(8), #MC(18) |

Fault 类型的异常（如 #PF），CPU 压入的 RIP 指向触发异常的指令本身。这样设计的目的是：当处理程序修复了问题（比如映射了缺页）之后，IRETQ 返回可以重新执行同一条指令，这次就不再出错了。但如果处理程序不做修复就返回，就会形成无限循环——这是 #PF 测试时需要特别注意的。

Trap 类型的异常（如 #BP），CPU 压入的 RIP 指向下一条指令。所以 IRETQ 返回后执行不会重复触发——这正是我们选择用 `int $3` 来测试中断链路的原因：安全，不会死循环。

部分异常（如 #PF、#GP、#DF）会在压入 RIP 之后额外压入一个错误码（error code），提供关于异常原因的额外信息。其他异常不压入错误码，ISR stub 需要手动压一个 0 来保持栈帧统一。下一篇讲 ISR stub 时会详细展开。

---

## 构建与运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -S .
cmake --build build -j$(nproc)
cd build && make run
```

**期望输出**：

```
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[MINI] PMM: Total 131040 pages (511 MB), Free 130784 pages (510 MB)
```

注意目前 ISR stub 还没写，所以 IDT 条目中指向的地址还不可用。如果此时触发异常，会跳转到未定义地址导致 triple fault。下一篇完成 ISR Stub 后，整个链路才真正贯通。

---

## 调试技巧

**LIDT 之后一触发异常就 crash**

检查 IDT 条目中的 selector 是否与 GDT 一致。如果 code segment 不在 GDT 索引 1，或者 GDT 没有正确加载，selector 查到的就是 null descriptor（全零，Present=0），直接触发 #GP。

**LIDT 本身 triple fault**

检查 s_idt_pointer.base 的值——在 higher-half kernel 里，IDT 的地址也需要是虚拟地址。如果填了物理地址，CPU 尝试在物理地址空间读取 IDT，在 higher-half 映射下这个地址可能不可访问。

**编译报错 undefined reference to isr_bp_stub**

这是因为 isr_bp_stub 定义在 interrupts.S 中，而 .S 文件还没加入 CMakeLists.txt。这个错误在下一篇添加 interrupts.S 后会自动解决。当前阶段可以先不管——只要 idt.cpp 编译通过就行，链接错误等所有文件都就位后再解决。

**清空 IDT 为什么要用循环赋值而不是 memset？**

两种方式都可以。`for (uint16_t i = 0; i < IDT_MAX_ENTRIES; i++) s_idt[i] = IdtEntry{};` 使用 C++ 的值初始化（所有字段置零），语义更清晰。memset 需要额外 include string.h 且对 C++ 结构体不如值初始化安全（比如将来如果 IdtEntry 有非 POD 字段，memset 会出错）。

**GDB 中检查 IDT 状态**

在 QEMU 调试模式下，你可以用以下方式验证 IDT 是否正确加载：

1. `info registers` 查看 IDTR 的值（base 和 limit）
2. `x/2xg &s_idt` 查看前几个 IDT 条目的内存布局
3. `print s_idt[3]` 查看 #BP 向量的 IDT 条目——检查 offset_low、offset_mid、offset_high 拼起来是否是 isr_bp_stub 的地址

---

## 本章小结

| 组件 | 关键概念 | 说明 |
|------|----------|------|
| IDT | IdtEntry, IdtPointer, lidt | 256 槽，只配 #BP(3) Trap Gate 和 #PF(14) Interrupt Gate |
| InterruptFrame | r15~rax + error_code + rip~ss | 中断栈帧，字段顺序必须和 push 顺序严格对应 |
| type_attr | 0x8F / 0x8E | 0x8F = Trap Gate (BP), 0x8E = Interrupt Gate (PF) |
| selector | SEGMENT_CODE64 (0x08) | 指向 GDT 中的 code64 段 |
| 初始化顺序 | GDT -> IDT | IDT 的 selector 依赖 GDT |
| extern "C" | C 链接约定 | ISR stub 和 handler 必须用 C 链接才能被汇编引用 |

## 延伸思考

- **为什么不配更多的异常向量？** x86 有 32 个 CPU 保留异常向量（0-31），比如 #DE（除零，向量 0）、#GP（一般保护错误，向量 13）、#DF（双重错误，向量 8）等。当前只配 #BP 和 #PF 是因为我们的目标是"最小可用"——先跑通核心机制，后续按需添加。#GP 尤其有用，它能捕获几乎所有类型的段/权限错误。
- **LIDT 和 LGDT 的区别？** 两者操作类似（都是加载一个 limit+base 的指针结构），但有一个关键区别：LGDT 之后需要手动刷新 CS（用 lretq 或 ljmp），而 LIDT 不需要——IDTR 不是段寄存器，它的内容在下一次中断触发时自然生效。
- **IDT 的 limit 为什么是 4095？** limit = sizeof(s_idt) - 1 = 256 * 16 - 1 = 4095。limit 字段的含义是"IDT 的最后一个有效字节的偏移"，不是"条目数"。所以 IDT 有 N 个条目时，limit = N * 16 - 1。如果 limit 设小了，超出范围的向量号触发时会触发 #GP。
- **IST 是什么？** IST（Interrupt Stack Table）是 x86_64 提供的一种机制，允许特定中断向量使用独立的栈（而不是当前 RSP）。这对 #DF（双重错误）和 #MC（Machine Check）特别重要——这些异常可能发生在内核栈已经损坏的情况下，如果继续用内核栈处理异常就会再次出错。我们当前不使用 IST（ist 字段填 0），但后续添加 TSS 时可以考虑。
- **256 个 IDT 条目占用多少内存？** 每个条目 16 字节，256 个就是 4096 字节（恰好一页）。这些内存是静态分配在 BSS 段中的，不会浪费动态内存。
