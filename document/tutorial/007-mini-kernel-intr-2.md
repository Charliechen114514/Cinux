# 007-2 Tutorial: ISR Stub 与异常处理 — 触发异常不死机

> 标签：ISR, Interrupt Frame, IRETQ, AT&T 汇编, Page Fault, CR2, 断点异常
> 前置：[007-1 GDT 与 IDT](./007-mini-kernel-intr-1.md)

## 前言

上一篇我们把 GDT 和 IDT 的"表格数据"搭好了——两张表告诉 CPU 遇到异常该去哪里找处理程序。但 IDT 里指向的 ISR stub 还没写，也就是说，CPU 虽然知道该跳到哪里去，但跳过去之后发现那里什么都没有。这一篇我们要补齐最后几块拼图：用汇编编写 ISR stub 保存寄存器、用 C 编写异常处理函数打印信息、最后在 main 里用 `int $3` 做一次完整的点火测试。

这一篇涉及到内核开发中最"底层"的部分——你会在汇编层面看到 CPU 是如何响应异常的，栈帧是如何一步步构建出来的，以及 IRETQ 是如何让执行回到正轨的。这些知识不仅在调试内核问题时至关重要，理解它们也是理解 x86 架构中断机制的必经之路。

## 环境说明

和上一篇相同——GNU AS（AT&T 语法）+ GCC/G++ + CMake，x86_64 freestanding 环境，higher-half QEMU 配置。本篇的代码文件是 `interrupts.S`（ISR stub 汇编）、`exception_handlers.cpp`（C 异常处理函数）和 `main.cpp` 的变更部分。

## 点火——ISR Stub 汇编

ISR Stub 是整个中断处理链路中最"硬核"的部分，因为它直接操作 CPU 的栈和寄存器，没有任何抽象和保护。Cinux 的 `interrupts.S` 定义了两个宏——`ISR_NOERRCODE` 和 `ISR_ERRCODE`——用于生成不同类型异常的 stub。

两个宏的区别很小但很关键。对于 #BP（断点异常）这种没有硬件错误码的异常，CPU 在进入 ISR 时只压入了 SS、RSP、RFLAGS、CS、RIP 五个值（5×8=40 字节），但我们的 InterruptFrame 结构体中有一个 error_code 字段。如果 stub 不手动压入一个 0 来占位，后面保存的寄存器就会从错误的位置开始——C 处理函数读到的 `frame->error_code` 实际上是 CPU 压入的 RIP，所有字段的偏移全部错位。所以 ISR_NOERRCODE 宏在保存寄存器之前先 `pushq $0` 压一个伪错误码，保持栈帧统一。而 ISR_ERRCODE 宏则跳过这步，因为 CPU 已经帮你压入了错误码。

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

这里面有一个很容易踩的坑——寄存器的保存顺序。你可能注意到汇编中的 push 顺序是 rax → rbx → rcx → ... → r15，而 InterruptFrame 结构体的字段声明顺序是 r15 → ... → rcx → rbx → rax，两者看起来是"反过来"的。这不是笔误，而是栈和结构体在内存中的排列方向不同造成的：栈从高地址往低地址增长，结构体从低地址往高地址排列。先 push 的 rax 在栈的低地址端（栈顶），对应结构体中最后声明的 rax 字段（结构体的高地址端）。最后一个 push 的 r15 在栈的高地址端，但仍在结构体的低地址端，刚好对应结构体中第一个声明的 r15 字段。这样 pop 的时候 pop r15 刚好对应结构体的第一个字段——一切对齐。

这个顺序问题如果不仔细想清楚，会导致非常隐蔽的 bug：寄存器 dump 里显示的值全部串位，但不会立刻 crash。你可能会花好几个小时才意识到是 push 顺序搞反了。我就在这里翻过车。

`movq %rsp, %rdi` 把当前栈指针传给 C 处理函数作为第一个参数。在 x86_64 的 System V ABI 中，RDI 是第一个参数寄存器，此时 RSP 正好指向 InterruptFrame 的起始位置（r15 字段），所以 C 函数拿到的就是一个指向完整中断栈帧的指针。

ISR_ERRCODE 宏几乎一模一样，唯一区别是不压入伪错误码。恢复时仍然是 `addq $8, %rsp` 跳过错误码——只不过这次跳过的是 CPU 压的而不是 stub 压的。文件末尾用两个宏分别实例化了 isr_bp_stub 和 isr_pf_stub。

## 上号——异常处理函数

异常处理函数的代码在 `exception_handlers.cpp` 中，实现了两个 C 函数和一个辅助打印函数。

辅助函数 `dump_interrupt_frame` 把 InterruptFrame 的所有字段格式化打印到串口。打印内容包括异常名称和向量号、RIP/CS/RFLAGS/RSP/SS（CPU 自动压入的关键状态）、全部 15 个通用寄存器以及错误码。输出格式设计成表格样式，方便在串口终端上快速扫读。

```cpp
void dump_interrupt_frame(const InterruptFrame* frame, const char* vec_name, uint8_t vector) {
    kprintf("\n==== EXCEPTION: %s (vector %u) ====\n", vec_name, vector);
    kprintf("  RIP   = 0x%016x   CS  = 0x%04x\n", frame->rip, frame->cs);
    kprintf("  RFLAGS= 0x%016x\n", frame->rflags);
    kprintf("  RSP   = 0x%016x   SS  = 0x%04x\n", frame->rsp, frame->ss);
    kprintf("  RAX=0x%016x  RBX=0x%016x\n", frame->rax, frame->rbx);
    // ... 其余寄存器 ...
    kprintf("  ERROR CODE = 0x%016x\n", frame->error_code);
    kprintf("========================================\n");
}
```

`handle_bp` 非常简洁——调用 dump_interrupt_frame 打印寄存器快照，然后额外打印断点地址和提示信息。这两个处理函数都用 `extern "C"` 声明，这是因为 `interrupts.S` 中的 `call handle_bp` 使用 C 链接约定——C++ 编译器默认会做 name mangling，把 `handle_bp` 改写成类似 `_Z8handle_bpP16InterruptFrame` 的符号名，链接器就找不到匹配的符号了。

`handle_pf` 更有趣一些，因为它需要读取 CR2 寄存器和解析页错误码。CR2 是 x86 架构专门为页错误保留的寄存器——Intel SDM Vol.3A 的 "Interrupt 14—Page-Fault Exception (#PF)" 章节明确说明，CPU 在触发 #PF 时会自动把导致缺页的线性地址写入 CR2。所以处理函数第一时间把它读出来（用内联汇编 `movq %%cr2, %0`），然后在打印信息时显示出来。

```cpp
extern "C" void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile ("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;
    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    dump_interrupt_frame(frame, "#PF", 14);
    kprintf("[EXCEPTION] Page Fault: %s %s %s%s%s\n",
            present, access, mode, reserved, fetch);
    kprintf("[EXCEPTION] Faulting address (CR2) = 0x%016x\n", fault_addr);
}
```

错误码的每一位都有明确含义。bit 0 区分"页不存在"（P=0）和"权限冲突"（P=1），bit 1 区分读操作和写操作，bit 2 区分内核态和用户态，bit 3 表示页表保留位被设置了，bit 4 表示这是一次取指操作。这些信息在调试缺页问题的时候非常关键——尤其是 CR2 地址结合错误码可以精确定位是哪条指令访问了哪个地址导致了问题。

值得注意的是当前 `handle_pf` 只打印信息不做修复。这是有意为之的设计选择——因为 #PF 是 Fault 类型的异常，CPU 压入的 RIP 指向触发指令本身（而不是下一条指令），IRETQ 返回后会重新执行同一条访存指令。如果不修复页表就返回，就会形成无限循环触发 #PF。所以当前阶段 handle_pf 的主要作用是调试——告诉你哪里出了问题，而不是自动修复。等到后续加入 VMM 后，handle_pf 会变成缺页处理的核心。

## 整合——在 main 中点火测试

main.cpp 的变更有三个部分：include 新头文件、添加初始化调用、以及 `int $3` 测试。初始化序列的顺序是固定的：gdt_init() → idt_init() → pmm::init()。GDT 必须在 IDT 之前，因为 IDT 条目中的 selector 引用了 GDT 中的代码段——如果反过来，虽然 LIDT 本身不检查 selector 有效性，但中断触发时 CPU 用 selector 去查 GDT 会查到垃圾数据，直接 triple fault。

点火测试是整个 milestone 的高潮。`__asm__ volatile("int $3")` 触发 #BP 断点异常，如果整个中断链路配置正确，执行流程是：int $3 → CPU 查 IDT[3] 得到 isr_bp_stub 地址 → 查 GDT[1] 确认 code64 段有效 → 压栈 SS/RSP/RFLAGS/CS/RIP → 跳转到 isr_bp_stub → stub 保存寄存器 → 调用 handle_bp → 打印寄存器 dump → 返回 stub → 恢复寄存器 → IRETQ → 继续执行 int $3 后面的代码。

```cpp
kprintf("\n[TEST] Triggering breakpoint exception (int $3)...\n");
__asm__ volatile("int $3");
kprintf("[TEST] Breakpoint test passed! Execution continued after #BP.\n\n");
```

`int $3` 是 x86 的断点指令（opcode 0xCC），它触发 #BP 异常。Intel SDM Vol.3A Table 6-1 明确标注 #BP 是 Trap 类型——CPU 压入的 RIP 指向下一条指令（而不是触发指令本身），所以 IRETQ 返回后会继续执行后面的 kprintf 打印 "Breakpoint test passed!"。这正是我们想要验证的：触发异常不死机，能继续执行。

看到串口输出 `==== EXCEPTION: #BP (vector 3) ====` 和完整的寄存器 dump，后面跟着 `[TEST] Breakpoint test passed!`，就说明整个链路通了。

## 别人怎么做的

### xv6 的 Trap 处理

xv6 的 `alltraps`（trapasm.S）是所有中断的统一入口。它先 push DS/ES/FS/GS 四个段寄存器，再执行 `pushal`（一条指令保存 eax/ecx/edx/ebx/old_esp/ebp/esi/edi 共 8 个通用寄存器），然后切换到内核数据段并调用 `trap(struct trapframe *tf)`。trap() 函数内部用 switch 语句按向量号分发：T_SYSCALL 走系统调用路径，T_IRQ0+IRQ_TIMER 处理时钟中断，T_IRQ0+IRQ_KBD 处理键盘中断，其余走 default 分支。

xv6 的 vectors.pl 脚本生成的 256 个 stub 也采用了"伪错误码"的策略——对于没有硬件错误码的向量，stub 会 `pushl $0` 压一个 0，然后 `pushl $vector_number` 压入向量号。这样 alltraps 看到的栈布局就是统一的，不需要条件判断。这和 Cinux 的 ISR_NOERRCODE 宏做的事情完全一样，只是 xv6 额外压入了向量号（Cinux 不需要，因为每个 stub 直接 call 对应的 handler）。

另一个有趣的差异是 xv6 的 trap() 使用 `cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n", ...)` 打印未处理的异常——和 Cinux 的 dump_interrupt_frame 类似，但 xv6 的输出更简洁，只有向量号和几个关键寄存器，而不是完整的寄存器 dump。这是合理的取舍——xv6 是生产级教学 OS，默认行为应该是安静地 panic 或者杀进程，而不是每次异常都打印大量信息。

### SerenityOS 的中断分发

SerenityOS 的中断处理路径更长，涉及更多抽象层。硬件中断到达后，CPU 跳转到 IDT 中注册的汇编入口点（类似 xv6 的 vectors），汇编代码保存寄存器后调用 C++ 的分发函数，分发函数通过 `GenericInterruptHandler` 的虚函数表找到对应的 handler 对象，调用其 `handle_interrupt()` 方法。每个 handler 对象还负责管理自己的 EOI（End of Interrupt）信号——通过 APIC 的 EOI 寄存器通知中断控制器当前中断处理完毕。

SerenityOS 还使用了 IST（Interrupt Stack Table）机制——Intel SDM Vol.3A §6.14.5 描述的这个特性允许为特定中断向量指定独立的栈，即使在内核栈被污染的情况下也能安全处理 NMI、Double Fault 和 Machine Check 等关键异常。这是 Cinux 后续可以借鉴的重要安全特性——在 tag 010 添加 TSS 时可以考虑为 #DF 配置 IST 栈。

## 收尾

到这里整个 tag 007 的中断处理链路就全部打通了。构建运行的完整输出应该是：先看到 GDT/IDT 初始化成功，然后 PMM 初始化信息，接着 `int $3` 触发 #BP 异常打印完整的寄存器 dump，最后 "Breakpoint test passed!" 表示执行安全返回。

这个 milestone 虽然只配了两个异常向量，但它建立了整个中断处理的骨架——GDT 提供段寄存器配置，IDT 提供中断向量表，ISR Stub 保存寄存器，C handler 处理具体逻辑，IRETQ 安全返回。后续的硬件中断（tag 011 的 PIC 编程）、系统调用（tag 023）、用户态异常（tag 022）都是在这个骨架上扩展的。理解了这套机制，后面的扩展就只是"往表里填更多条目"的问题了。

下一章（008）我们会实现 ATA PIO 磁盘驱动和 ELF 加载器，让 mini kernel 能从磁盘加载大内核并跳转执行。届时这里已经配好的异常处理会继续发挥作用——大内核加载过程中的任何内存问题都能被 #PF 捕获并报告。

## 参考资料

- Intel SDM: Vol.3A §6.14.2 (p.6-21) — 64-Bit Mode Stack Frame Layout
- Intel SDM: Vol.3A §6.14.3 (pp.6-21~6-22) — IRET in IA-32e Mode
- Intel SDM: Vol.3A §6.13 (pp.6-18~6-19) — Error Code Format (#PF special format)
- Intel SDM: Vol.3A Table 6-1 (pp.6-4~6-6) — Exception and Interrupt Vector Assignments
- OSDev Wiki: [Interrupts](https://wiki.osdev.org/Interrupts)
- xv6: [trapasm.S](https://github.com/mit-pdos/xv6-public/blob/master/trapasm.S) — alltraps/unified entry
- SerenityOS: [APIC.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp) — GenericInterruptHandler framework
