# 007-2 Tutorial: ISR Stub 汇编 — 用汇编接住异常

> 标签：ISR, Interrupt Frame, AT&T 汇编, IRETQ, 伪错误码, 栈帧对齐
> 前置：[007-1 GDT 与 IDT](./007-mini-kernel-intr-1.md)

## 前言

上一篇我们把 GDT 和 IDT 的"表格数据"搭好了——两张表告诉 CPU 遇到异常该去哪里找处理程序。但 IDT 里指向的 ISR stub 还没写，也就是说，CPU 虽然知道该跳到哪里去，但跳过去之后发现那里什么都没有。这一篇我们要补齐中断处理链路的中间一环：用汇编编写 ISR stub 保存寄存器、构造 InterruptFrame、调用 C 处理函数。

这一篇涉及到内核开发中最"硬核"的部分——你会在汇编层面看到 CPU 是如何响应异常的，栈帧是如何一步步构建出来的，IRETQ 是如何让执行回到正轨的。这些知识不仅在调试内核问题时至关重要，理解它们也是理解 x86 架构中断机制的必经之路。下一篇我们会写 C 异常处理函数并用 `int $3` 做点火测试。

## 环境说明

和上一篇相同——GNU AS（AT&T 语法）+ GCC/G++ + CMake，x86_64 freestanding 环境，higher-half QEMU 配置。本篇的代码文件是 `kernel/mini/arch/x86_64/interrupts.S`（ISR stub 汇编），约 152 行。文件包含两个宏和两个 stub 实例化。

## AT&T 汇编语法速查

在阅读 ISR stub 之前，先快速回顾一下 AT&T 语法的几个关键点（我们的 ISR stub 用 GNU AS 编写）：

- 操作数顺序是"源, 目标"（和 Intel 语法相反）：`movq %rax, %rbx` 是把 RAX 的值放到 RBX
- 寄存器前缀 `%`，立即数前缀 `$`：`pushq $0` 是压入数字 0
- 内存寻址格式 `offset(base)`：`8(%rsp)` 是访问 RSP+8 处的内存
- 宏参数用反斜杠引用：`\name` 引用宏参数 name
- `.global \name` 导出符号让链接器能看到，`.type \name, @function` 标注类型
- 注释用 `#` 或 `/* */`

## 点火——ISR Stub 汇编

ISR Stub 是整个中断处理链路中最"硬核"的部分，因为它直接操作 CPU 的栈和寄存器，没有任何抽象和保护。Cinux 的 `interrupts.S` 定义了两个宏——`ISR_NOERRCODE` 和 `ISR_ERRCODE`——用于生成不同类型异常的 stub。

两个宏的区别很小但很关键。对于 #BP（断点异常）这种没有硬件错误码的异常，CPU 在进入 ISR 时只压入了 SS、RSP、RFLAGS、CS、RIP 五个值（5x8=40 字节），但我们的 InterruptFrame 结构体中有一个 error_code 字段。如果 stub 不手动压入一个 0 来占位，后面保存的寄存器就会从错误的位置开始——C 处理函数读到的 `frame->error_code` 实际上是 CPU 压入的 RIP，所有字段的偏移全部错位。所以 ISR_NOERRCODE 宏在保存寄存器之前先 `pushq $0` 压一个伪错误码，保持栈帧统一。而 ISR_ERRCODE 宏则跳过这步，因为 CPU 已经帮你压入了错误码。

```asm
.macro ISR_NOERRCODE name vector handler
.global \name
.type \name, @function
\name:
    pushq $0              /* 伪错误码 */
    pushq %rax            /* 保存所有 15 个通用寄存器 */
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi       /* InterruptFrame* 作为第一个参数 */
    call \handler         /* 调用 C 处理函数 */
    popq %r15             /* 反向恢复寄存器 */
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $8, %rsp         /* 弹出伪错误码 */
    iretq                 /* 中断返回 */
.endm
```

这里面有一个很容易踩的坑——寄存器的保存顺序。你可能注意到汇编中的 push 顺序是 rax -> rbx -> rcx -> ... -> r15，而 InterruptFrame 结构体的字段声明顺序是 r15 -> ... -> rcx -> rbx -> rax，两者看起来是"反过来"的。这不是笔误，而是栈和结构体在内存中的排列方向不同造成的：栈从高地址往低地址增长，结构体从低地址往高地址排列。先 push 的 rax 在栈的低地址端（栈顶），对应结构体中最后声明的 rax 字段（结构体的高地址端）。最后一个 push 的 r15 在栈的高地址端，但仍在结构体的低地址端，刚好对应结构体中第一个声明的 r15 字段。这样 pop 的时候 pop r15 刚好对应结构体的第一个字段——一切对齐。

这个顺序问题如果不仔细想清楚，会导致非常隐蔽的 bug：寄存器 dump 里显示的值全部串位，但不会立刻 crash。你可能会花好几个小时才意识到是 push 顺序搞反了。我就在这里翻过车。

`movq %rsp, %rdi` 把当前栈指针传给 C 处理函数作为第一个参数。在 x86_64 的 System V ABI 中，RDI 是第一个参数寄存器，此时 RSP 正好指向 InterruptFrame 的起始位置（r15 字段），所以 C 函数拿到的就是一个指向完整中断栈帧的指针。

ISR_ERRCODE 宏几乎一模一样，唯一区别是不压入伪错误码。恢复时仍然是 `addq $8, %rsp` 跳过错误码——只不过这次跳过的是 CPU 压的而不是 stub 压的。这里有个容易混淆的地方：两个宏的恢复逻辑看起来完全一样（都是 addq $8 跳过 8 字节），但跳过的东西不一样——NOERRCODE 跳过的是 stub 自己压的伪值，ERRCODE 跳过的是 CPU 压的真值。效果是一样的：栈指针回到 CPU 压入 RIP 的位置，然后 IRETQ 弹出 5 个值恢复执行。

文件末尾用两个宏分别实例化了 isr_bp_stub 和 isr_pf_stub：

```asm
ISR_NOERRCODE isr_bp_stub, 3, handle_bp
ISR_ERRCODE   isr_pf_stub, 14, handle_pf
```

## 别人怎么做的

### xv6 的 Trap 处理

xv6 的 `alltraps`（trapasm.S）是所有中断的统一入口。它先 push DS/ES/FS/GS 四个段寄存器，再执行 `pushal`（一条指令保存 eax/ecx/edx/ebx/old_esp/ebp/esi/edi 共 8 个通用寄存器），然后切换到内核数据段并调用 `trap(struct trapframe *tf)`。trap() 函数内部用 switch 语句按向量号分发。

xv6 的 vectors.pl 脚本生成的 256 个 stub 也采用了"伪错误码"的策略——对于没有硬件错误码的向量，stub 会 `pushl $0` 压一个 0，然后 `pushl $vector_number` 压入向量号。这样 alltraps 看到的栈布局就是统一的，不需要条件判断。这和 Cinux 的 ISR_NOERRCODE 宏做的事情完全一样，只是 xv6 额外压入了向量号（Cinux 不需要，因为每个 stub 直接 call 对应的 handler）。

另一个有趣的差异是 xv6 的 trap() 使用 `cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n", ...)` 打印未处理的异常——和 Cinux 的 dump_interrupt_frame 类似，但 xv6 的输出更简洁，只有向量号和几个关键寄存器，而不是完整的寄存器 dump。

### SerenityOS 的中断分发

SerenityOS 的中断处理路径更长，涉及更多抽象层。硬件中断到达后，CPU 跳转到 IDT 中注册的汇编入口点（类似 xv6 的 vectors），汇编代码保存寄存器后调用 C++ 的分发函数，分发函数通过 `GenericInterruptHandler` 的虚函数表找到对应的 handler 对象，调用其 `handle_interrupt()` 方法。每个 handler 对象还负责管理自己的 EOI（End of Interrupt）信号——通过 APIC 的 EOI 寄存器通知中断控制器当前中断处理完毕。

SerenityOS 还使用了 IST（Interrupt Stack Table）机制——Intel SDM Vol.3A 6.14.5 描述的这个特性允许为特定中断向量指定独立的栈，即使在内核栈被污染的情况下也能安全处理 NMI、Double Fault 和 Machine Check 等关键异常。这是 Cinux 后续可以借鉴的重要安全特性——在 tag 010 添加 TSS 时可以考虑为 #DF 配置 IST 栈。

## 为什么 ISR stub 不能用 C 写？

你可能会问：为什么要用汇编写 ISR stub，而不是用 C？答案是 C 编译器生成的函数入口代码不适合做 ISR stub，原因有三个：

1. **栈帧格式不对**：C 函数的入口代码通常会 push rbp + mov rsp,rbp 来建立栈帧，还会在需要时分配局部变量空间（sub rsp, N）。这些操作会破坏 InterruptFrame 的布局。ISR stub 需要精确控制压栈的每一个字节。

2. **寄存器保存不完整**：C 函数只保存 callee-saved 寄存器（rbx, rbp, r12-r15），不保存 caller-saved 寄存器（rax, rcx, rdx, rsi, rdi, r8-r11）。但 ISR stub 需要保存所有 15 个通用寄存器，因为异常可能在任何时刻发生，此时所有寄存器的值都是"有用的"。

3. **无法直接执行 IRETQ**：C 函数用 ret 返回，而 ISR stub 需要用 iretq 返回。iretq 会从栈上弹出 RIP/CS/RFLAGS/RSP/SS 五个值（而不是 ret 只弹出一个 RIP），这是硬件要求的。C 编译器无法生成 iretq 指令。

所以 ISR stub 必须用汇编写，这几乎是所有 x86 内核的共同选择。Linux、xv6、SerenityOS 的 ISR 入口都是汇编代码。

## 常见调试场景

**ISR stub 里寄存器 dump 显示错误的值**

这是最常见的问题。如果你的 handle_bp 打印出来的 RAX 实际上是 RBX 的值，RBX 是 RCX 的值，说明 push 顺序和 InterruptFrame 结构体的字段顺序不匹配。排查方法：数一下从 r15 到 rax 的偏移量。r15 在偏移 0，r14 在偏移 8，...，rax 在偏移 14*8=112，error_code 在偏移 15*8=120，rip 在偏移 16*8=128。如果 GDB 中 `print frame->rip` 的值看起来像 RFLAGS（0x46 或 0x202），说明 push 顺序搞反了 8 字节。

**用 GDB 单步跟踪 ISR stub**

在 QEMU 调试模式下，你可以在 ISR stub 的符号上设断点：

```
(gdb) break isr_bp_stub
(gdb) continue
# int $3 触发后，断在 isr_bp_stub 入口
(gdb) si
# 单步执行每个 pushq，观察 RSP 的变化
(gdb) print/x $rsp
# 每次 push 后 RSP 应该减少 8
```

这个过程可以帮助你验证 push 的顺序和数量是否正确。15 个 push 后，RSP 应该减少了 128 字节（加上之前的伪错误码 8 字节，总共 136 字节）。

## 概念补充：异常的分类与栈帧差异

Intel SDM Table 6-1 把 x86 异常分为三类，理解这个分类对理解 ISR stub 的设计非常重要：

| 类型 | 含义 | RIP 指向 | 是否有错误码 | 典型例子 |
|------|------|----------|-------------|---------|
| Fault | 可恢复的错误 | 触发指令本身 | 部分有 | #PF(14), #GP(13) |
| Trap | 调试用异常 | 下一条指令 | 无 | #BP(3) |
| Abort | 严重错误 | 不确定 | 无 | #DF(8) |

Fault 和 Trap 的 RIP 差异直接影响 ISR stub 的恢复行为。#PF（Fault）的 RIP 指向触发指令，IRETQ 返回后会重新执行——如果修复了页表就正常了，如果没修复就死循环。#BP（Trap）的 RIP 指向下一条指令，IRETQ 返回后安全继续执行——这就是为什么我们选择用 `int $3` 来测试。

另外需要注意哪些异常有硬件错误码。根据 Intel SDM Table 6-1，只有以下异常会自动压入错误码：#DF(8)、#TS(10)、#NP(11)、#SS(12)、#GP(13)、#PF(14)。其余所有异常（包括 #BP）都没有硬件错误码，ISR stub 需要手动压入 0 来保持栈帧统一。这就是 ISR_NOERRCODE 和 ISR_ERRCODE 两个宏存在的根本原因。

## 栈帧布局的详细分析

让我们完整走一遍 `int $3` 触发后的栈帧变化过程，这对理解 ISR stub 的每一步操作至关重要。

**异常触发前**：内核代码正常运行，RSP 指向内核栈的某个位置。此时栈上只有正常的函数调用帧。

**CPU 自动压栈**：执行 `int $3` 后，CPU 硬件自动执行以下操作（注意顺序是从高地址到低地址）：先压 SS（当前栈段），再压 RSP（当前栈指针），再压 RFLAGS（当前标志寄存器），再压 CS（当前代码段），最后压 RIP（下一条指令的地址）。此时 RSP 已经向下移动了 40 字节（5 个 64 位值）。

**ISR stub 压入伪错误码**：`pushq $0` 在栈上压入一个 0，RSP 再下移 8 字节。现在栈上有 48 字节。

**ISR stub 保存寄存器**：15 个 pushq 操作保存 rax~r15，RSP 再下移 120 字节（15 x 8）。现在栈上总共 168 字节，RSP 指向栈顶的 r15 值。

**调用 C 函数**：`movq %rsp, %rdi` 把当前 RSP 传给 C 函数。C 函数收到的 InterruptFrame* 指针从 r15 开始，依次是 r14, r13, ..., rax, error_code(0), rip, cs, rflags, rsp, ss。每个字段的偏移量都是确定的——比如 `frame->rip` 的偏移量是 15*8 + 8 = 128 字节。

**恢复过程**：C 函数返回后，stub 逆序 pop 恢复寄存器（120 字节），addq $8 跳过伪错误码（8 字节），最后 IRETQ 弹出 CPU 压入的 5 个值（40 字节），RSP 恢复到异常触发前的位置。整个过程是精确对称的。

## 收尾

到这一步 ISR stub 的汇编代码就完成了。在下一篇中，我们会实现 exception_handlers.cpp（C 异常处理函数），并用 `int $3` 在 main 中做一次完整的点火测试。到时候你就能在串口看到完整的寄存器 dump，验证整个中断处理链路是通的。

## 文件清单

本篇涉及的源文件：

| 文件 | 职责 | 关键内容 |
|------|------|---------|
| `kernel/mini/arch/x86_64/interrupts.S` | ISR stub 汇编 | ISR_NOERRCODE/ISR_ERRCODE 宏，isr_bp_stub/isr_pf_stub |
| `kernel/mini/CMakeLists.txt` | 构建配置 | 添加 interrupts.S |

interrupts.S 约 152 行，其中两个宏各约 40 行，实例化部分 2 行。CMakeLists.txt 只需要添加一行 `arch/x86_64/interrupts.S`。

## 参考资料

- Intel SDM: Vol.3A 6.14.2 (p.6-21) -- 64-Bit Mode Stack Frame Layout
- Intel SDM: Vol.3A 6.14.3 (pp.6-21~6-22) -- IRET in IA-32e Mode
- Intel SDM: Vol.3A Table 6-1 (pp.6-4~6-6) -- Exception and Interrupt Vector Assignments
- OSDev Wiki: [Interrupts](https://wiki.osdev.org/Interrupts)
- xv6: [trapasm.S](https://github.com/mit-pdos/xv6-public/blob/master/trapasm.S) -- alltraps/unified entry
- SerenityOS: [APIC.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp) -- GenericInterruptHandler framework
