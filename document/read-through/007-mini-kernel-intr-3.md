# 007-3 Read-through: ISR Stub 与异常处理

## 本章概览

这一篇我们通读 Cinux mini kernel 的 ISR Stub 汇编、异常处理函数和主函数整合部分——interrupts.S、exception_handlers.cpp 和 main.cpp 的变更。在整个 tag 007 的架构中，这三部分构成了中断处理链路的后半段：ISR Stub 负责在汇编层面接住异常、保存寄存器、构造 InterruptFrame、调用 C 处理函数；异常处理函数负责打印寄存器 dump 和错误信息；main.cpp 负责把所有组件串起来做最终的点火测试。

前两篇我们看了 GDT 和 IDT 的"表格数据"——它们告诉 CPU 去哪里找处理程序。这一篇我们看到的是处理程序本身——真正执行工作的代码。从整个调用链路来看：CPU 异常 -> 查 IDT -> 查 GDT -> 压栈 -> 跳转 ISR Stub -> 保存寄存器 -> 调用 C handler -> 打印信息 -> 恢复寄存器 -> IRETQ -> 继续执行。

---

## 架构图

```
异常触发与处理完整流程（以 int $3 为例）：

mini_kernel_main()
       |
       | asm volatile("int $3")
       v
   +-- CPU ---------------------------------------------------+
   | 1. 查 IDT[3] -> selector=0x08, offset=isr_bp_stub       |
   | 2. 查 GDT[1] -> code64 segment 验证通过                  |
   | 3. 压栈: SS, RSP, RFLAGS, CS, RIP (5x8=40字节)          |
   | 4. 跳转到 isr_bp_stub                                    |
   +----------------------------------------------------------+
       |
       v
   +-- isr_bp_stub (interrupts.S) ----------------------------+
   | pushq $0          (伪错误码，保持栈对齐)                  |
   | pushq %rax ~ %r15 (保存 15 个通用寄存器)                  |
   | movq %rsp, %rdi   (InterruptFrame* -> RDI)               |
   | call handle_bp    (跳转 C 处理函数)                       |
   | popq %r15 ~ %rax  (恢复寄存器)                            |
   | addq $8, %rsp     (弹出伪错误码)                          |
   | iretq             (返回被中断代码)                         |
   +----------------------------------------------------------+
       |
       v
   +-- handle_bp (exception_handlers.cpp) ---------------------+
   | dump_interrupt_frame() -> 串口打印寄存器                   |
   | kprintf 断点地址和提示信息                                 |
   | return -> 回到 isr_bp_stub                                |
   +----------------------------------------------------------+
       |
       v
   继续执行 mini_kernel_main 后续代码


栈帧布局（InterruptFrame，从高地址到低地址）：

高地址 ---------------------------------------------------
  SS              | CPU 自动压入
  RSP             | CPU 自动压入
  RFLAGS          | CPU 自动压入
  CS              | CPU 自动压入
  RIP             | CPU 自动压入
  --------------- |
  Error Code      | #PF: CPU压入; #BP: stub压入$0
  --------------- | ISR stub 保存
  RAX             | pushq %rax (最先push)
  RBX             | pushq %rbx
  RCX             | pushq %rcx
  RDX             | pushq %rdx
  RBP             | pushq %rbp
  RSI             | pushq %rsi
  RDI             | pushq %rdi
  R8              | pushq %r8
  R9              | pushq %r9
  R10             | pushq %r10
  R11             | pushq %r11
  R12             | pushq %r12
  R13             | pushq %r13
  R14             | pushq %r14
  R15             | pushq %r15 (最后push)
低地址 ----------- RSP 指向这里 -----------------------------
```

---

## 代码精讲

### ISR Stub 汇编 — interrupts.S

这个文件定义了两个宏——`ISR_NOERRCODE` 和 `ISR_ERRCODE`——以及用它们实例化的两个 stub。整个文件使用 AT&T 语法（GNU AS 默认），操作数顺序是"源, 目标"，寄存器前缀 `%`，立即数前缀 `$`。

先看 `ISR_NOERRCODE` 宏，这是给 #BP 这种没有硬件错误码的异常用的。宏接受三个参数：stub 名称（name）、向量号（vector，虽然当前代码中没用到，但保留了给将来可能的扩展）和 C 处理函数名（handler）。

```asm
.macro ISR_NOERRCODE name vector handler
.global \name
.type \name, @function
\name:
    pushq $0              /* 压入伪错误码 0，保持栈帧统一 */
    pushq %rax
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
    call \handler
    popq %r15
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
    iretq
.endm
```

这里有几个关键细节需要逐一理解。

第一，`pushq $0` 压入伪错误码的作用是保持栈帧统一。对于 #BP 这种没有硬件错误码的异常，CPU 只压入了 SS/RSP/RFLAGS/CS/RIP 五个值（5x8=40 字节），而 InterruptFrame 结构体中有一个 error_code 字段。如果不手动压入一个 0 来占位，后面 push 的寄存器就会从错误的位置开始，C 处理函数读到的 `frame->error_code` 实际上是 CPU 压入的 RIP，所有字段的偏移全部错位。

第二，寄存器的保存顺序（rax -> rbx -> rcx -> ... -> r15）必须和 InterruptFrame 结构体中字段的声明顺序（r15 -> ... -> rax）"反过来"。这是因为栈从高地址往低地址增长，而结构体从低地址往高地址排列——先 push 的值在栈的低地址端，对应结构体的前面成员。所以第一个 push 的 rax 对应结构体中最后声明的 rax 字段，最后一个 push 的 r15 对应结构体中第一个声明的 r15 字段。

第三，`movq %rsp, %rdi` 把当前栈指针作为第一个参数传给 C 处理函数。在 x86_64 的 System V ABI 中，第一个参数通过 RDI 传递，此时 RSP 正好指向 InterruptFrame 结构体的起始位置（即 r15 字段），所以 C 函数收到的就是一个指向完整中断栈帧的指针。

`ISR_ERRCODE` 宏几乎一模一样，唯一区别是不压入伪错误码——因为 CPU 在进入 ISR 之前已经帮你压入了错误码。恢复时仍然是 `addq $8, %rsp` 跳过错误码，只不过这次跳过的是 CPU 压的而不是 stub 压的。

```asm
.macro ISR_ERRCODE name vector handler
.global \name
.type \name, @function
\name:
    /* 错误码已在栈上，直接保存寄存器 */
    pushq %rax
    pushq %rbx
    /* ... 其余同 ISR_NOERRCODE ... */
    pushq %r15
    movq %rsp, %rdi
    call \handler
    popq %r15
    /* ... 反向恢复 ... */
    popq %rax
    addq $8, %rsp        /* 跳过 CPU 压入的错误码 */
    iretq
.endm
```

文件末尾用这两个宏实例化了两个 stub：

```asm
ISR_NOERRCODE isr_bp_stub, 3, handle_bp
ISR_ERRCODE   isr_pf_stub, 14, handle_pf
```

### 异常处理函数 — exception_handlers.cpp

这个文件实现了两个 C 处理函数和一个辅助打印函数。文件先用匿名命名空间的 using 声明简化了类型引用。

辅助函数 `dump_interrupt_frame` 负责把 InterruptFrame 的所有字段格式化打印到串口。打印内容包括异常名称和向量号、RIP/CS/RFLAGS/RSP/SS（CPU 自动压入的关键状态）、全部 15 个通用寄存器（ISR stub 保存的）以及错误码。这个函数被两个处理函数共用。

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

`handle_bp` 的实现非常简洁——先调用 dump_interrupt_frame 打印完整的寄存器快照，然后额外打印断点地址和提示信息。注意这个函数用 `extern "C"` 声明，这是因为 interrupts.S 中的 `call handle_bp` 使用 C 链接约定——如果用 C++ 的 name mangling，链接器会找不到符号。

```cpp
extern "C" void handle_bp(InterruptFrame* frame) {
    dump_interrupt_frame(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint triggered at RIP=0x%x\n", frame->rip);
    kprintf("[EXCEPTION] This is a software breakpoint, continuing...\n");
}
```

`handle_pf` 的实现更有趣，因为它需要读取 CR2 寄存器和解析页错误码。CR2 是 x86 架构专门为页错误保留的寄存器——CPU 在触发 #PF 时会自动把导致缺页的线性地址写入 CR2，所以我们在处理函数里第一时间把它读出来（用内联汇编 `movq %%cr2, %0`）。必须第一时间读取的原因是，kprintf 内部的任何内存操作理论上都可能触发另一个页错误（虽然在这个简单的 mini kernel 里不太可能），覆盖 CR2 的值。

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

错误码的解析也非常有用。bit 0 区分"页不存在"（P=0）和"权限冲突"（P=1），bit 1 区分读操作和写操作，bit 2 区分内核态和用户态，bit 3 表示页表保留位被设置了，bit 4 表示这是一次取指操作（指令缓存缺页）。这些信息在调试缺页问题的时候非常关键，尤其是 CR2 地址结合错误码可以精确定位是哪条指令访问了哪个地址导致了问题。

值得注意的是当前 `handle_pf` 只打印信息不做修复——因为这是 Fault 类型的异常，IRETQ 返回后会重新执行触发缺页的指令。如果不修复页表就返回，就会形成无限循环触发 #PF。等到后续加入 VMM（虚拟内存管理器）后，handle_pf 会变成缺页处理的核心。

### 主函数变更 — main.cpp

main.cpp 的变更集中在三个方面：include 新头文件、添加初始化调用、以及 `int $3` 测试。

头文件引入了 gdt.hpp 和 idt.hpp，这两个头文件提供了 `gdt_init()` 和 `idt_init()` 的声明。初始化调用按固定顺序排列：先 gdt_init()（必须在最前面，因为 IDT 依赖 GDT），然后 idt_init()，最后 pmm::init()。每步之间都有 kprintf 打印状态信息，这在调试的时候非常方便——如果某个初始化步骤出了问题 triple fault 了，你至少能看到是哪一步之前的最后一条输出。

```cpp
// main.cpp - 初始化序列
kprintf("[INIT] Setting up GDT...\n");
cinux::mini::arch::gdt_init();
kprintf("[INIT] GDT loaded successfully.\n");

kprintf("[INIT] Setting up IDT...\n");
cinux::mini::arch::idt_init();
kprintf("[INIT] IDT loaded successfully.\n");

cinux::mini::mm::pmm::init(boot_info);
```

点火测试部分是整个 tag 007 的高潮。`__asm__ volatile("int $3")` 触发 #BP 断点异常，如果整个中断链路配置正确的话，CPU 会跳转到 isr_bp_stub -> handle_bp -> 打印寄存器 dump -> 返回 stub -> IRETQ -> 继续执行后面的 kprintf 打印 "Breakpoint test passed!"。

```cpp
// main.cpp - int $3 测试
kprintf("\n[TEST] Triggering breakpoint exception (int $3)...\n");
__asm__ volatile("int $3");
kprintf("[TEST] Breakpoint test passed! Execution continued after #BP.\n\n");
```

`int $3` 是 x86 的断点指令（opcode 0xCC），它触发 #BP 异常。这是一个 Trap 类型的异常，CPU 压入的 RIP 指向下一条指令——也就是说 IRETQ 返回后会继续执行后面的 kprintf，打印 "Breakpoint test passed"。这正是我们要验证的：触发异常不死机，能继续执行。

---

## 设计决策

### 决策：ISR Stub 用宏生成而不是手写每个向量

**问题**: 需要决定 ISR stub 的编写方式——为每个向量手写一个 stub，还是用宏批量生成。

**本项目的做法**: 定义 ISR_NOERRCODE 和 ISR_ERRCODE 两个宏，每个向量一行实例化。

**备选方案**: 像 xv6 那样用 Perl 脚本生成 256 个 stub。

**为什么不选备选方案**: 一是当前只有 2 个向量，用脚本反而更复杂；二是手写宏让学习者能直接看到每个 stub 的汇编代码，比生成文件更有教学价值；三是宏和脚本在本质上做的事情一样——为每个向量生成一个入口点——只是规模不同。

### 决策：handle_pf 只打印不修复

**问题**: #PF 处理函数是只打印信息还是尝试修复页表。

**本项目的做法**: 只打印 CR2 和错误码，然后 IRETQ 返回。

**备选方案**: 在 handle_pf 中实现简单的 demand paging——分配一个新页映射到 CR2 地址。

**为什么不选备选方案**: 一是当前还没有 VMM（虚拟内存管理器），实现 demand paging 需要页表操作的支持；二是 PMM 只提供物理页分配，不提供虚拟地址映射；三是在 mini kernel 阶段，handle_pf 的主要作用是调试——告诉你哪里访问了不该访问的地址，而不是自动修复。

---

## 参考资料

- Intel SDM: Vol.3A 6.12.1.3 (p.6-17) -- Flag Usage by Handler
- Intel SDM: Vol.3A 6.13 (pp.6-18~6-19) -- Error Code Format
- Intel SDM: Vol.3A 6.14.2 (p.6-21) -- 64-Bit Mode Stack Frame
- Intel SDM: Vol.3A 6.14.3 (pp.6-21~6-22) -- IRET in IA-32e Mode
- OSDev Wiki: [Interrupts](https://wiki.osdev.org/Interrupts)
- xv6: [trapasm.S](https://github.com/mit-pdos/xv6-public/blob/master/trapasm.S) -- alltraps entry point
