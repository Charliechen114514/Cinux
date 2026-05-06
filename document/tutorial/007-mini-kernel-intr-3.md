# 007-3 Tutorial: 异常处理函数与 int $3 点火测试

> 标签：Page Fault, CR2, 断点异常, extern "C", kprintf, int $3
> 前置：[007-2 ISR Stub](./007-mini-kernel-intr-2.md)

## 前言

前两篇我们把 GDT、IDT 和 ISR stub 都搭好了——GDT 提供段寄存器配置，IDT 提供中断向量表，ISR stub 在汇编层面保存寄存器并构造 InterruptFrame。现在就差最后两块拼图了：C 处理函数和点火测试。

这一篇我们会实现 handle_bp 和 handle_pf 两个 C 函数，然后在 main 里用 `int $3` 触发一次完整的断点异常。如果你一直跟着做到这里，这一篇结束时你就能在串口看到完整的寄存器 dump，并且内核会继续运行不死机。这就是 milestone 007 的最终目标。

## 环境说明

和前两篇相同——GNU AS（AT&T 语法）+ GCC/G++ + CMake，x86_64 freestanding 环境，higher-half QEMU 配置。本篇的代码文件是 `exception_handlers.cpp`（C 异常处理函数）和 `main.cpp` 的变更部分。

## 上号——异常处理函数

异常处理函数的代码在 `exception_handlers.cpp` 中，实现了两个 C 函数和一个辅助打印函数。

辅助函数 `dump_interrupt_frame` 把 InterruptFrame 的所有字段格式化打印到串口。打印内容包括异常名称和向量号、RIP/CS/RFLAGS/RSP/SS（CPU 自动压入的关键状态）、全部 15 个通用寄存器（ISR stub 保存的）以及错误码。输出格式设计成表格样式，方便在串口终端上快速扫读。

```cpp
void dump_interrupt_frame(const InterruptFrame* frame, const char* vec_name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", vec_name, vector);
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

```cpp
extern "C" void handle_bp(InterruptFrame* frame) {
    dump_interrupt_frame(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint triggered at RIP=0x%x\n", frame->rip);
    kprintf("[EXCEPTION] This is a software breakpoint, continuing...\n");
}
```

`handle_pf` 更有趣一些，因为它需要读取 CR2 寄存器和解析页错误码。CR2 是 x86 架构专门为页错误保留的寄存器——Intel SDM Vol.3A 的 "Interrupt 14--Page-Fault Exception (#PF)" 章节明确说明，CPU 在触发 #PF 时会自动把导致缺页的线性地址写入 CR2。所以处理函数第一时间把它读出来（用内联汇编 `movq %%cr2, %0`），然后在打印信息时显示出来。

为什么必须第一时间读取 CR2？因为 kprintf 内部的任何内存操作理论上都可能触发另一个页错误（虽然在这个简单的 mini kernel 里不太可能），如果发生的话 CR2 的值就会被覆盖。虽然当前 mini kernel 几乎不会遇到这种情况，但养成在处理函数入口第一时间读 CR2 的好习惯非常重要——在后续加入 VMM 后，handle_pf 会变成缺页处理的核心，到时候 CR2 的值就是分配新页的关键输入。

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
    kprintf("[EXCEPTION] Continuing execution...\n");
}
```

错误码的每一位都有明确含义。bit 0 区分"页不存在"（P=0）和"权限冲突"（P=1），bit 1 区分读操作和写操作，bit 2 区分内核态和用户态，bit 3 表示页表保留位被设置了，bit 4 表示这是一次取指操作。这些信息在调试缺页问题的时候非常关键——尤其是 CR2 地址结合错误码可以精确定位是哪条指令访问了哪个地址导致了问题。

值得注意的是当前 `handle_pf` 只打印信息不做修复。这是有意为之的设计选择——因为 #PF 是 Fault 类型的异常，CPU 压入的 RIP 指向触发指令本身（而不是下一条指令），IRETQ 返回后会重新执行同一条访存指令。如果不修复页表就返回，就会形成无限循环触发 #PF。所以当前阶段 handle_pf 的主要作用是调试——告诉你哪里出了问题，而不是自动修复。等到后续加入 VMM 后，handle_pf 会变成缺页处理的核心。

## 整合——在 main 中点火测试

main.cpp 的变更有三个部分：include 新头文件、添加初始化调用、以及 `int $3` 测试。初始化序列的顺序是固定的：gdt_init() -> idt_init() -> pmm::init()。GDT 必须在 IDT 之前，因为 IDT 条目中的 selector 引用了 GDT 中的代码段——如果反过来，虽然 LIDT 本身不检查 selector 有效性，但中断触发时 CPU 用 selector 去查 GDT 会查到垃圾数据，直接 triple fault。

点火测试是整个 milestone 的高潮。`__asm__ volatile("int $3")` 触发 #BP 断点异常，如果整个中断链路配置正确，执行流程是：int $3 -> CPU 查 IDT[3] 得到 isr_bp_stub 地址 -> 查 GDT[1] 确认 code64 段有效 -> 压栈 SS/RSP/RFLAGS/CS/RIP -> 跳转到 isr_bp_stub -> stub 保存寄存器 -> 调用 handle_bp -> 打印寄存器 dump -> 返回 stub -> 恢复寄存器 -> IRETQ -> 继续执行 int $3 后面的代码。

```cpp
kprintf("\n[TEST] Triggering breakpoint exception (int $3)...\n");
__asm__ volatile("int $3");
kprintf("[TEST] Breakpoint test passed! Execution continued after #BP.\n\n");
```

`int $3` 是 x86 的断点指令（opcode 0xCC），它触发 #BP 异常。Intel SDM Vol.3A Table 6-1 明确标注 #BP 是 Trap 类型——CPU 压入的 RIP 指向下一条指令（而不是触发指令本身），所以 IRETQ 返回后会继续执行后面的 kprintf 打印 "Breakpoint test passed!"。这正是我们想要验证的：触发异常不死机，能继续执行。

同时别忘了更新 CMakeLists.txt，把新增的源文件加进去：

```cmake
# kernel/mini/CMakeLists.txt 中添加
arch/x86_64/gdt.cpp
arch/x86_64/idt.cpp
arch/x86_64/interrupts.S
arch/x86_64/exception_handlers.cpp
```

## 上板验证

构建运行后，串口输出的关键部分长这样：

```
[INIT] Setting up GDT...
[INIT] GDT loaded successfully.
[INIT] Setting up IDT...
[INIT] IDT loaded successfully.
[MINI] PMM: Total 131040 pages (511 MB), Free 130784 pages (510 MB)

[TEST] Triggering breakpoint exception (int $3)...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0xffffffff80020224   CS  = 0x0008
  RFLAGS= 0x0000000000000046
  RSP   = 0xffffffff80025100   SS  = 0x0010
  RAX=0x0000000000000000  RBX=0x0000000000004118
  ...（其余寄存器）
  ERROR CODE = 0x0000000000000000
========================================
[EXCEPTION] Breakpoint triggered at RIP=0xffffffff80020224
[EXCEPTION] This is a software breakpoint, continuing...
[TEST] Breakpoint test passed! Execution continued after #BP.
```

CS = 0x0008 对应 GDT 索引 1（0x08 / 8 = 1），就是我们设的 code64 segment。SS = 0x0010 对应索引 2，是 data64 segment。RIP 指向 `int $3` 下一条指令的地址——因为 #BP 是陷阱类型，CPU 压入的 RIP 已经指向下一条指令了。看到 `[TEST] Breakpoint test passed!` 就说明整个链路通畅：GDT -> IDT -> ISR stub -> C handler -> iretq -> 继续执行。

## 踩坑总结

写这个 milestone 的过程中踩了几个坑，记一下免得以后再踩：

**栈帧错位**。如果 #BP 的 ISR stub 忘了压伪错误码，`InterruptFrame` 的 error_code 字段会读到 CPU 压入的 RIP，然后 rip 字段读到 CS，整个 dump 全是错的。debug 方法是对照汇编 push 顺序和 C 结构体字段顺序，一个一个数偏移量。

**GDT 必须在 IDT 之前初始化**。如果反过来，`lidt` 能成功但中断触发时 CPU 拿 selector 去查 GDT 会查到 null descriptor（Present=0），触发 #GP，然后 #GP 也没配处理，就 triple fault 了。这个顺序在代码里看起来很显然，但在重构的时候不小心调换顺序就会中招。

**higher-half 地址**。GDT 和 IDT 的 `GdtPointer.base` / `IdtPointer.base` 需要填虚拟地址（因为内核运行在 higher-half），不是物理地址。如果填了物理地址，CPU 尝试在物理地址空间读取描述符表，在 higher-half 映射下这个物理地址可能不可访问，直接 triple fault。

**extern "C" 遗漏**。如果 exception_handlers.cpp 中的 handle_bp 和 handle_pf 忘了加 `extern "C"`，链接时会报 undefined reference——因为汇编里的 `call handle_bp` 使用 C 链接约定的符号名，而 C++ 编译器默认使用 mangled 名字（类似 `_Z8handle_bpP16InterruptFrame`）。

## 完整的初始化流程回顾

把整个 tag 007 的初始化流程串起来看，mini_kernel_main 中的执行顺序是这样的：

```
1. 串口初始化 + BootInfo 解析（之前的 tag）
2. kprintf("[INIT] Setting up GDT...")
3. cinux::mini::arch::gdt_init()
   - 填写 null/code64/data64 三项
   - LGDT 加载
   - lretq 刷新 CS
   - mov DS/ES/FS/GS/SS
4. kprintf("[INIT] GDT loaded successfully.")
5. kprintf("[INIT] Setting up IDT...")
6. cinux::mini::arch::idt_init()
   - 清空 256 个 IDT 条目
   - set_idt_entry(3, isr_bp_stub, 0x08, 0x8F, 0)
   - set_idt_entry(14, isr_pf_stub, 0x08, 0x8E, 0)
   - LIDT 加载
7. kprintf("[INIT] IDT loaded successfully.")
8. cinux::mini::mm::pmm::init(boot_info)
9. kprintf("[TEST] Triggering breakpoint exception...")
10. asm volatile("int $3")
    -> CPU 查 IDT[3] -> 查 GDT[1] -> 压栈 -> isr_bp_stub
    -> push $0 + push 寄存器 -> call handle_bp
    -> dump_interrupt_frame + kprintf
    -> pop 寄存器 + addq $8 -> iretq
11. kprintf("[TEST] Breakpoint test passed!")
12. while(1) { cli; hlt; }
```

步骤 3 必须在步骤 6 之前（GDT 先于 IDT），步骤 6 必须在步骤 10 之前（IDT 先于中断触发）。如果任何一步出错，后续的 triple fault 会让你什么都看不到——所以每步之间的 kprintf 非常重要，它告诉你内核死在了哪一步之前。

## 收尾

到这里整个 tag 007 的中断处理链路就全部打通了。构建运行的完整输出应该是：先看到 GDT/IDT 初始化成功，然后 PMM 初始化信息，接着 `int $3` 触发 #BP 异常打印完整的寄存器 dump，最后 "Breakpoint test passed!" 表示执行安全返回。

这个 milestone 虽然只配了两个异常向量，但它建立了整个中断处理的骨架——GDT 提供段寄存器配置，IDT 提供中断向量表，ISR Stub 保存寄存器，C handler 处理具体逻辑，IRETQ 安全返回。后续的硬件中断（tag 011 的 PIC 编程）、系统调用（tag 023）、用户态异常（tag 022）都是在这个骨架上扩展的。理解了这套机制，后面的扩展就只是"往表里填更多条目"的问题了。

## 别人怎么做的

### xv6 的 Trap 处理

xv6 的 `alltraps`（trapasm.S）是所有中断的统一入口。它先 push DS/ES/FS/GS 四个段寄存器，再执行 `pushal`（一条指令保存 eax/ecx/edx/ebx/old_esp/ebp/esi/edi 共 8 个通用寄存器），然后切换到内核数据段并调用 `trap(struct trapframe *tf)`。trap() 函数内部用 switch 语句按向量号分发：T_SYSCALL 走系统调用路径，T_IRQ0+IRQ_TIMER 处理时钟中断，T_IRQ0+IRQ_KBD 处理键盘中断，其余走 default 分支。

xv6 的 trap() 使用 `cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n", ...)` 打印未处理的异常——和 Cinux 的 dump_interrupt_frame 类似，但 xv6 的输出更简洁，只有向量号和几个关键寄存器。这是合理的取舍——xv6 是生产级教学 OS，默认行为应该是安静地 panic 或者杀进程，而不是每次异常都打印大量信息。

### SerenityOS 的中断分发

SerenityOS 的中断处理路径更长，涉及更多抽象层。硬件中断到达后，CPU 跳转到 IDT 中注册的汇编入口点，汇编代码保存寄存器后调用 C++ 的分发函数，分发函数通过 `GenericInterruptHandler` 的虚函数表找到对应的 handler 对象，调用其 `handle_interrupt()` 方法。每个 handler 对象还负责管理自己的 EOI（End of Interrupt）信号。

下一章（008）我们会实现 ATA PIO 磁盘驱动和 ELF 加载器，让 mini kernel 能从磁盘加载大内核并跳转执行。届时这里已经配好的异常处理会继续发挥作用——大内核加载过程中的任何内存问题都能被 #PF 捕获并报告。

## 参考资料

- Intel SDM: Vol.3A 6.12.1.3 (p.6-17) -- Flag Usage by Handler
- Intel SDM: Vol.3A 6.13 (pp.6-18~6-19) -- Error Code Format (#PF special format)
- Intel SDM: Vol.3A 6.14.2 (p.6-21) -- 64-Bit Mode Stack Frame
- Intel SDM: Vol.3A Table 6-1 (pp.6-4~6-6) -- Exception and Interrupt Vector Assignments
- OSDev Wiki: [Interrupts](https://wiki.osdev.org/Interrupts)
- xv6: [trapasm.S](https://github.com/mit-pdos/xv6-public/blob/master/trapasm.S) -- alltraps/unified entry
- SerenityOS: [APIC.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp) -- GenericInterruptHandler framework
