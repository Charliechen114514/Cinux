# 010-2 教程：IDT + ISR + 异常处理——让内核学会"遗言"

> 标签：x86_64, IDT, 中断描述符表, ISR, 异常处理, 中断栈帧, InterruptFrame, iretq
> 前置：[010-1 大内核 GDT 初始化](./010-big-kernel-gdt-idt-1.md)

## 前言

上一节我们给大内核配好了 GDT，七个描述符全部到位，段寄存器刷新正确，TSS 也加载了。说实话，配完 GDT 之后大内核的行为和之前没有任何区别——因为还没有 IDT，CPU 遇到异常时还是 triple fault 重启。GDT 只是地基，真正的"建筑"是本节要讲的 IDT（中断描述符表）和 ISR（中断服务例程）。

现在我们回过头来想一想：开发内核最痛苦的事情是什么？不是写代码，不是调试逻辑，而是"不知道发生了什么"。你写了一段代码，跑起来 QEMU 直接重启，连一行输出都没有——是除零了？缺页了？非法指令了？还是 GDT 配错了触发了 #GP？你不知道。CPU 知道，但它没有嘴——它只会通过 IDT 来"找"处理程序，如果 IDT 是空的或者对应的条目没配好，CPU 就只能默默 triple fault 然后重启。所以 IDT 本质上就是给 CPU 一张"电话簿"——"当你遇到向量 14 的事情时，打这个号码（处理程序地址），找这个人（C handler 函数）"。

本节完成后的效果是这样的：我们在 `kernel_main` 里执行一条 `int $3` 指令触发断点异常，串口会输出完整的寄存器 dump（RAX-R15、RIP、CS、RFLAGS、RSP、SS、Error Code），然后打印 "Breakpoint returned, continuing."——程序继续执行，不死机。这就是我们要达到的目标：触发异常不死机，能看到 CPU 当时的完整状态，然后程序还能继续执行。

## 环境说明

和上一节完全一致——WSL2 + GCC 13+ + CMake + QEMU，x86_64 freestanding 内核。本节的代码涉及汇编文件 `interrupts.S`（AT&T 语法，GNU AS），需要你对 AT&T 语法有基本的了解（操作数顺序 `source, destination`，寄存器前缀 `%`，立即数前缀 `$`）。

## 第一步——IDT 头文件：让魔术数字变成有意义的名字

我们先来创建 `kernel/arch/x86_64/idt.hpp`。这个头文件定义了 IDT 的所有类型：异常向量号的枚举、门类型和特权级的枚举、中断栈帧结构体、IDT class 本身。

异常向量号是 0-255 的数字，其中 0-31 被 Intel 保留给 CPU 异常（Intel SDM Vol.3A §6.2 Table 6-1）。如果直接用魔术数字——`set_handler(0, ...)` 配 #DE、`set_handler(14, ...)` 配 #PF——你每次都得翻 SDM 才能确认 14 到底是 #PF 还是 #GP。所以我们要做的第一件事是把这些魔术数字变成有意义的名字：

```cpp
enum class ExceptionVector : uint8_t {
    DE  = 0,    // #DE: Divide Error
    DB  = 1,    // #DB: Debug Exception
    NMI = 2,    // Non-maskable Interrupt
    BP  = 3,    // #BP: Breakpoint (INT3)
    OF  = 4,    // #OF: Overflow
    BR  = 5,    // #BR: BOUND Range Exceeded
    UD  = 6,    // #UD: Invalid Opcode
    NM  = 7,    // #NM: Device Not Available
    DF  = 8,    // #DF: Double Fault
    TS  = 10,   // #TS: Invalid TSS
    NP  = 11,   // #NP: Segment Not Present
    SS  = 12,   // #SS: Stack-Segment Fault
    GP  = 13,   // #GP: General Protection
    PF  = 14,   // #PF: Page Fault
};
```

向量 9 被跳过了——现代 x86_64 CPU 不再产生 Coprocessor Segment Overrun 异常，Intel 保留了这个向量号但不再使用。

接下来是门类型和特权级的枚举，以及一个组合函数：

```cpp
enum class IDTGateType : uint8_t {
    Interrupt = 0x0E,  // 64-bit interrupt gate (clears IF)
    Trap      = 0x0F,  // 64-bit trap gate (preserves IF)
};

enum class IDTPrivilege : uint8_t {
    Kernel = 0x00,  // Ring 0 only
    User   = 0x60,  // Ring 3 (DPL=3, already shifted: 3 << 5)
};

constexpr uint8_t make_idt_attr(IDTPrivilege priv, IDTGateType gate) {
    return 0x80 | static_cast<uint8_t>(priv) | static_cast<uint8_t>(gate);
}
```

`make_idt_attr` 把 Present 位（0x80）、DPL 和 Gate Type 打包进一个字节。四种组合的结果：
- Kernel + Interrupt = 0x8E：致命异常的标准配置，关中断 + 只有内核能触发
- Kernel + Trap = 0x8F：调试异常 #DB，保持中断开启
- User + Trap = 0xEF：断点异常 #BP，允许用户态 `int $3` 触发
- User + Interrupt = 0xEE：本项目未使用

门类型的选择策略基于 Intel SDM Vol.3A §6.12.1.3 的描述——Interrupt Gate 在跳转时自动清除 RFLAGS.IF（关中断），Trap Gate 不动 IF。对于致命异常（除零、非法指令、保护错误等），处理过程中不应该被新的中断打断，所以用 Interrupt Gate。对于调试相关的异常（#BP 断点、#DB 调试），它们本身就是在调试流程中触发的，关掉中断反而会让调试器无法正常工作，所以用 Trap Gate。DPL 设为 3 是因为 `int $3` 是调试器常用的软件断点指令，用户态程序也可能触发它（比如 GDB 的断点实现就是用 `int $3` 替换指令的第一个字节），所以必须允许 Ring 3 的代码通过这个向量进入内核。

然后是整个中断处理子系统中最微妙的部分——中断栈帧结构体：

```cpp
struct [[gnu::packed]] InterruptFrame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};
```

这个结构体的字段顺序必须和 ISR 汇编中 push 的顺序**完全一致**。从栈顶（低地址）到栈底（高地址）：R15 最先被 ISR stub push（在栈上最深，结构体最前面）→ ... → RAX 最后被 push（在栈上最浅）→ error_code → RIP → CS → RFLAGS → RSP → SS（由 CPU 自动 push，在栈上最高位）。RSP 在 `call handle_xx` 之前指向 R15 的位置，所以 `InterruptFrame*` 指向的第一个字段就是 R15。

这个坑说实话真的很难排查——如果你的 RDI 和 RBP 字段写反了，程序不会崩溃（因为都是 uint64_t，大小一样），只是打印出来的寄存器值全部是错的。你可能会花半天时间怀疑是 CPU 的问题而不是代码的问题。所以这里我强烈建议在写完之后做一个简单的测试：在 `int $3` 之前给几个寄存器设置已知的值（比如 `movq $0xDEADBEEF, %rax`），然后看 dump 出来的 RAX 是不是 `0xDEADBEEF`。如果不是，字段顺序就搞反了。

IDT class 本体的设计和 GDT class 类似——`Entry` 结构体 16 字节，`Pointer` 结构体 10 字节，全局实例 `g_idt`：

```cpp
class IDT {
public:
    using Handler = void (*)(InterruptFrame*);
    using Stub = void (*)();

    void init();
    void set_handler(ExceptionVector vector, Stub stub,
                     uint16_t selector, uint8_t type_attr, uint8_t ist = 0);

private:
    struct [[gnu::packed]] Entry {
        uint16_t offset_low;
        uint16_t selector;
        uint8_t  ist;
        uint8_t  type_attr;
        uint16_t offset_mid;
        uint32_t offset_high;
        uint32_t reserved;
    };
    static_assert(sizeof(Entry) == 16, "IDT entry must be 16 bytes");

    static constexpr uint16_t kMaxEntries = 256;
    Entry entries_[kMaxEntries]{};
    Pointer idtr_{};

    void load();
};

extern IDT g_idt;
```

`entries_` 数组 256 个条目全部 zero-initialized。未配置的向量条目 type_attr 为 0（Present 位未设置），CPU 访问这些条目时触发 #GP——这比留下未初始化的垃圾值安全得多。

### xv6 怎么做的

xv6 的中断处理方案和我们的设计思路高度相似，但实现细节有不少差异。xv6 使用 Perl 脚本 `vectors.pl` 自动生成 256 个 ISR stub——脚本遍历向量 0-255，对于有错误码的向量（8, 10-14）直接 push CPU 的错误码，对于没有错误码的向量 push 0 作为伪错误码。所有 stub 最终跳转到统一的 `alltraps` 入口，在那里保存 DS/ES/FS/GS + pushal（pushal 是 32 位指令，一次性 push EAX/ECX/EDX/EBX/ESP/EBP/ESI/EDI 八个 32 位寄存器），然后调用 `trap(struct trapframe *tf)` 进行统一的异常/中断分发。

xv6 的 `tvinit()` 函数在启动时配置全部 256 个 IDT 条目，所有条目默认使用 Interrupt Gate（`SETGATE` 的 `istrap=0`），只有 `T_SYSCALL` 使用 Trap Gate + DPL_USER。`trap()` 函数内部用 switch 语句按向量号分发——`case T_SYSCALL` 处理系统调用，`case T_PGFLT` 处理页错误，等等。这种"一步到位"的设计减少了后续修改的需要，但增加了初学者的认知负担——你第一次看代码就要面对 256 个 IDT 条目和完整的 trap 分发逻辑。

对比 Cinux 的方案：我们只配置 15 个异常向量（0-8, 10-14），ISR stub 用手写汇编宏（不是脚本生成），handler 是独立的 C 函数（不是统一的 trap() + switch）。好处是每个 milestone 的复杂度可控——你只需要理解当前需要处理的异常类型。代价是后续需要回头扩展（比如硬件 IRQ 的处理需要在 011 章再加）。

### Linux 怎么做的

Linux 的中断处理架构是 xv6 和 Cinux 的"完全体"。Linux 的 IDT 条目通过 `idt_init_desc()` 函数设置（`arch/x86/include/asm/desc.h`），该函数把 64 位地址拆分到 IDT 条目的三个 offset 字段——和我们的 `set_handler()` 做的事情完全一样。Linux 在 x86_64 下所有 IDT 条目的 segment 固定为 `__KERNEL_CS`（等同于我们的 `GDT_KERNEL_CODE = 0x08`），IST 字段在 `pack_gate()` 中按需设置——比如 #DF 和 #MC（Machine Check）使用 IST 栈，确保这些关键中断在栈损坏的情况下仍然能正常处理。

Linux 的 ISR 入口代码在 `arch/x86/entry/entry_64.S` 中，使用 `idtentry` 宏为每个异常生成入口。这个宏做的事情比我们的 `ISR_NOERRCODE`/`ISR_ERRCODE` 多得多——它不仅保存通用寄存器，还处理 IST 栈切换、paravirt hook、KVM 虚拟化回调等。宏的参数包括向量号、C handler 函数名、是否有错误码、是否使用 IST 等。一个典型的实例化看起来像这样：`idtentry X86_TRAP_PF page_fault do_page_fault has_error_code=1`。

Linux 的异常处理函数在 `arch/x86/kernel/traps.c` 中，采用了一种有趣的"早期处理 + 后续分析"的分层架构。`early_fixup_exception()` 在极端早期（甚至 before boot CPU 初始化完成）就进行异常处理，用于捕获启动阶段的页错误。正式的 `do_page_fault()`、`do_general_protection()` 等函数则会进行详细的错误分析、发送信号（SIGSEGV/SIGBUS）给用户态进程、或者直接 panic（如果是内核态触发的致命异常）。

对比 Cinux：我们的 handler 直接在 `dump_registers` 后要么继续（#BP/#DB）要么 halt，没有信号机制，没有进程上下文，也没有错误恢复。这对于当前阶段的内核来说是合理的——我们还没有用户态进程，没有内存管理，没有信号系统。但随着后续 tag 的推进，我们的 handler 会逐步接近 Linux 的架构。

### SerenityOS 怎么做的

SerenityOS 的中断系统代表了另一种设计思路——面向对象的框架。SerenityOS 定义了 `GenericInterruptHandler` 基类，每种中断类型有自己的子类（`SpuriousInterruptHandler`、`APICInterruptHandler` 等），通过 `register_interrupt_handler()` 动态注册到 IDT。AP 启动时复制 BSP 的 GDTR/IDTR（`ap_cpu_gdtr` / `ap_cpu_idtr`），确保每个 CPU 的中断配置一致。

SerenityOS 使用 APIC（Advanced Programmable Interrupt Controller）而非传统 8259 PIC，中断向量分配策略也不同——硬件中断向量从 `IRQ_VECTOR_BASE`（通常是 0x20）开始偏移，高端向量号（0xfc-0xff）保留给 APIC 特殊中断（Timer、IPI、Error、Spurious）。SerenityOS 的 `InterruptsAPIC.cpp` 展示了如何在一个完整的 OS 中管理中断路由、核间中断和伪中断处理。

这种 OOP 方案的好处是扩展性极好——添加新的中断类型只需要继承 `GenericInterruptHandler` 并注册，不需要修改核心 IDT 代码。代价是增加了间接层（虚函数调用）和代码复杂度，对于教学系统来说可能过重了。

## 第二步——ISR 汇编跳板：push 顺序是生死攸关的

现在我们来写 `kernel/arch/x86_64/interrupts.S`——ISR（Interrupt Service Routine）的汇编跳板代码。这段代码是整个中断处理链路中"最底层"的部分——CPU 触发异常后第一个执行的就是这里的代码，它负责保存寄存器、调用 C 处理函数、恢复寄存器、然后 iretq 返回。

我们定义两个 GAS 宏：`ISR_NOERRCODE` 和 `ISR_ERRCODE`。它们的区别只有一处——`ISR_NOERRCODE` 在保存寄存器之前先 `pushq $0` 填一个假的 error code 0，`ISR_ERRCODE` 不需要（CPU 已经 push 了真的 error code）。

```asm
.macro ISR_NOERRCODE name handler
.global \name
.type \name, @function
\name:
    pushq $0                          # push dummy error code 0
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

    movq %rsp, %rdi                   # InterruptFrame* as first arg
    call \handler                     # call C handler

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

    addq $8, %rsp                     # skip error code
    iretq                             # interrupt return
.endm
```

push 的顺序是 RAX→RBX→RCX→RDX→RBP→RSI→RDI→R8→...→R15。为什么是这个顺序而不是 R15→R8→RDI→...→RAX？其实从功能上来说，任何顺序都可以——只要 push 和 pop 的顺序对称（先 push 的后 pop），寄存器就能正确保存和恢复。但顺序必须和 `InterruptFrame` 结构体的字段顺序一致。push 的顺序决定了值在栈上的排列——先 push 的在栈上地址更高（更远离栈顶），后 push 的在栈上地址更低（更靠近栈顶）。RSP 指向最后 push 的 R15，所以 `InterruptFrame` 的第一个字段（最低地址）是 R15。

pop 的顺序和 push 完全相反——R15 先 pop（因为它最后 push，在栈顶），RAX 最后 pop。然后 `addq $8, %rsp` 跳过栈上的 error code（8 字节），`iretq` 从栈上依次弹出 RIP、CS、RFLAGS、RSP、SS，恢复到被中断的代码。`addq $8, %rsp` 不能省略——error code 不会被 `iretq` 自动弹出（Intel SDM §6.13 明确说 "the error code is not popped from the stack by IRET"），必须手动跳过。

然后用这两个宏实例化 15 个 stub——8 个无错误码（向量 0-7），7 个有错误码（向量 8, 10-14）：

```asm
ISR_NOERRCODE isr_de_stub,  handle_de       /* #DE(0) */
ISR_NOERRCODE isr_db_stub,  handle_db       /* #DB(1) */
ISR_NOERRCODE isr_nmi_stub, handle_nmi      /* NMI(2) */
ISR_NOERRCODE isr_bp_stub,  handle_bp       /* #BP(3) */
ISR_NOERRCODE isr_of_stub,  handle_of       /* #OF(4) */
ISR_NOERRCODE isr_br_stub,  handle_br       /* #BR(5) */
ISR_NOERRCODE isr_ud_stub,  handle_ud       /* #UD(6) */
ISR_NOERRCODE isr_nm_stub,  handle_nm       /* #NM(7) */

ISR_ERRCODE   isr_df_stub,  handle_df       /* #DF(8) */
ISR_ERRCODE   isr_ts_stub,  handle_ts       /* #TS(10) */
ISR_ERRCODE   isr_np_stub,  handle_np       /* #NP(11) */
ISR_ERRCODE   isr_ss_stub,  handle_ss       /* #SS(12) */
ISR_ERRCODE   isr_gp_stub,  handle_gp       /* #GP(13) */
ISR_ERRCODE   isr_pf_stub,  handle_pf       /* #PF(14) */
```

对比 xv6 用 Perl 脚本生成 256 个 stub 的方式，我们手写的 15 个 stub 虽然覆盖面小，但每一行都清楚明了。如果你想要 xv6 风格的自动化，可以写一个简单的脚本遍历 0-255，判断每个向量是否有错误码，然后选择对应的宏——但这对于当前阶段的教学目标来说不是必要的。

## 第三步——异常处理函数：致命的 halt，不致命的 continue

异常处理函数在 `kernel/arch/x86_64/exception_handlers.cpp` 中实现。所有函数都声明为 `extern "C"` linkage——这是因为 ISR stub 中的 `call \handler` 使用的是 C 风格的函数调用（不经过 C++ name mangling），所以 handler 的符号名必须和汇编中的一致。

核心辅助函数 `dump_registers` 负责格式化输出所有寄存器的值：

```cpp
void dump_registers(const InterruptFrame* frame,
                    const char* name, uint8_t vector) {
    kprintf("\n");
    kprintf("==== EXCEPTION: %s (vector %u) ====\n", name, vector);
    kprintf("  RIP   = %p   CS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rip),
            static_cast<unsigned>(frame->cs));
    kprintf("  RFLAGS= %p\n",
            reinterpret_cast<void*>(frame->rflags));
    kprintf("  RSP   = %p   SS  = 0x%04x\n",
            reinterpret_cast<void*>(frame->rsp),
            static_cast<unsigned>(frame->ss));
    kprintf("  RAX=%p  RBX=%p\n", ...);
    kprintf("  ... (all GP registers)\n");
    kprintf("  ERROR CODE = %p\n",
            reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}
```

用 `%p` 格式输出寄存器值——虽然 `%p` 的标准语义是打印指针地址，但在内核调试场景下，把寄存器值当指针打印是最直观的方式。`reinterpret_cast<void*>` 是为了满足 `%p` 的类型要求。

然后是 15 个 handler 函数。它们分为两类：非致命的（#BP 和 #DB）打印完后直接返回，ISR stub 中的 iretq 会恢复执行；致命的打印完后调用 `fatal_halt()` 进入 `cli; hlt` 死循环。

非致命的 #BP handler 是我们验证整个链路的关键：

```cpp
void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint at RIP=%p\n",
            reinterpret_cast<void*>(frame->rip));
    kprintf("[EXCEPTION] Continuing...\n");
}
```

打印完信息后直接返回——ISR stub 中的 pop + addq + iretq 会把 CPU 恢复到触发 `int $3` 的下一条指令。这就是 "breakpoint returned, continuing" 的意思。

#PF handler 是最复杂的——它不仅打印寄存器 dump，还从 CR2 读出触发缺页的虚拟地址，然后解码 error code 的各个 bit：

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;
    const char* present  = (err & 0x01) ? "protection violation" : "page not present";
    const char* access   = (err & 0x02) ? "write" : "read";
    const char* mode     = (err & 0x04) ? "user" : "kernel";
    const char* reserved = (err & 0x08) ? ", reserved bits" : "";
    const char* fetch    = (err & 0x10) ? ", instruction fetch" : "";

    dump_registers(frame, "#PF", 14);
    kprintf("[FATAL] Page Fault: %s %s %s%s%s\n",
            present, access, mode, reserved, fetch);
    kprintf("[FATAL] Faulting address (CR2) = %p -- halting.\n",
            reinterpret_cast<void*>(fault_addr));
    fatal_halt();
}
```

#PF 的 error code 格式（Intel SDM Vol.3A §6.13）是专用的——bit 0 表示"页不存在"还是"权限违反"，bit 1 表示读还是写，bit 2 表示内核态还是用户态。CR2 寄存器保存了触发缺页的虚拟地址，这是 CPU 在触发 #PF 时自动设置的。这些信息在后续调试内存管理相关的 bug 时非常有价值。

## 第四步——IDT 初始化：数据驱动的路由表

`kernel/arch/x86_64/idt.cpp` 负责把 ISR stub 和 C handler 连接到 IDT 条目中。核心设计是"数据驱动的路由表"——一个 `Route` 结构体数组，每个元素包含 {向量号, stub 地址, 特权级, 门类型}，遍历数组配置 IDT：

```cpp
void IDT::init() {
    for (auto& entry : entries_) {
        entry = Entry{};
    }

    struct Route {
        ExceptionVector vector;
        Stub stub;
        IDTPrivilege priv;
        IDTGateType gate;
    };

    const Route routes[] = {
        {ExceptionVector::DE,  isr_de_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::DB,  isr_db_stub,  IDTPrivilege::Kernel, IDTGateType::Trap},
        {ExceptionVector::NMI, isr_nmi_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::BP,  isr_bp_stub,  IDTPrivilege::User,   IDTGateType::Trap},
        /* ... vectors 4-14 ... */
    };

    for (const auto& r : routes) {
        set_handler(r.vector, r.stub, GDT_KERNEL_CODE,
                    make_idt_attr(r.priv, r.gate), 0);
    }

    idtr_.limit = static_cast<uint16_t>(sizeof(entries_) - 1);
    idtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}
```

这个路由表一目了然——14 条路由，每条包含了向量号、ISR stub 地址、特权级和门类型的完整信息。对比 xv6 的 `tvinit()` 用循环 + `SETGATE` 宏逐个配置 256 个条目的方式，我们的路由表更紧凑。对比 Linux 的 `idt_setup_traps()` + `idt_setup_apic_and_irq_gates()` 分阶段设置的方式，我们的一次性路由表更简单。当然，随着内核功能的扩展（硬件 IRQ、系统调用等），我们可能需要把路由表拆分成多个阶段，但当前阶段这样已经足够了。

`set_handler()` 函数把 64 位处理程序地址拆成三段存入 IDT 条目（offset_low 16 位 + offset_mid 16 位 + offset_high 32 位），selector 固定为 `GDT_KERNEL_CODE`（0x08），IST 为 0。

`load()` 就一行——`lidt %[idtr]`，从内存加载 IDTR。比 GDT 的 `load()` 简单得多，因为不需要刷新段寄存器。

## 第五步——集成到 kernel_main：int $3 触发验证

最后一步是把所有东西集成到 `kernel_main` 中：

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    cinux::arch::g_gdt.init();
    cinux::lib::kprintf("[BIG] GDT loaded.\n");

    cinux::arch::g_idt.init();
    cinux::lib::kprintf("[BIG] IDT loaded.\n");

    cinux::lib::kprintf("[BIG] Triggering int $3 breakpoint...\n");
    __asm__ volatile("int $3");
    cinux::lib::kprintf("[BIG] Breakpoint returned, continuing.\n");

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

初始化顺序是 `kprintf_init` → `g_gdt.init` → `g_idt.init`——串口先初始化，GDT 在 IDT 之前（因为 IDT 的 selector 依赖 GDT），IDT 在 `int $3` 之前。注意我们**没有**调用 `sti`——因为没有配置 IRQ 处理程序，PIT 定时器会在 `sti` 后的几毫秒内触发 IRQ 0，跳到 IDT[32]（空条目），导致 #GP → Double Fault → Triple Fault。

`int $3` 是一字节指令（opcode 0xCC），被设计成一字节是有原因的——调试器可以在任意位置插入断点（只需覆盖一个字节），而不需要考虑指令对齐问题。不过对我们来说，`int $3` 和 `int $N`（N != 3）的效果是一样的——都是触发 IDT[3] 对应的处理程序。

## 验证

运行大内核，串口输出应该是这样的：

```
[BIG] Big kernel running @ 0x1000000
[BIG] GDT loaded.
[BIG] IDT loaded.
[BIG] Triggering int $3 breakpoint...

==== EXCEPTION: #BP (vector 3) ====
  RIP   = 0x...   CS  = 0x0008
  RFLAGS= 0x...
  RSP   = 0x...   SS  = 0x0010
  RAX=0x...  RBX=0x...
  ... (all registers)
  ERROR CODE = 0x0
========================================
[EXCEPTION] Breakpoint at RIP=0x...
[EXCEPTION] Continuing...
[BIG] Breakpoint returned, continuing.
```

自动化的 QEMU 测试（8 个测试全部 PASS）：

```bash
cmake --build build --target run-big-kernel-test
```

## 收尾

到这里，大内核终于不再是"聋哑人"了。CPU 遇到异常时，它会通过 IDT 找到我们的处理程序，ISR stub 保存寄存器，C handler 打印完整的寄存器 dump，非致命异常后程序还能继续执行。这是内核开发中最基础但也最关键的基础设施——没有它，你后续写的任何代码都可能在任何时刻被 CPU 异常杀死，而你连出问题的地方都不知道。

但这只是开始。目前我们只处理了 CPU 异常（向量 0-14），硬件中断（IRQ 0-15，向量 32-47）还没有处理。下一章（011）我们要配置 8259 PIC（可编程中断控制器），把硬件中断映射到 IDT 的向量 32-47，然后实现定时器中断——让串口每秒输出一个 `[TICK] uptime: Ns`。那才是内核真正"活"起来的时刻。

## 参考资料

- Intel SDM: Vol.3A §6.2 — Exception and Interrupt Vectors (Table 6-1, pp.6-4~6-6)
- Intel SDM: Vol.3A §6.12.1.3 — Flag Usage (p.6-17)
- Intel SDM: Vol.3A §6.13 — Error Codes (pp.6-18~6-19)
- Intel SDM: Vol.3A §6.14 — Exception/Interrupt Handling in 64-bit Mode (pp.6-20~6-23)
- Intel SDM: Vol.3A §6.14.1 — 64-Bit Mode IDT (Figure 6-8, p.6-20)
- Intel SDM: Vol.3A §6.14.2 — 64-Bit Mode Stack Frame (p.6-21)
- OSDev Wiki: [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)
- OSDev Wiki: [Interrupts](https://wiki.osdev.org/Interrupts)
- xv6: [trap.c](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) — tvinit, trap dispatch
- xv6: [trapasm.S](https://github.com/mit-pdos/xv6-public/blob/master/trapasm.S) — alltraps, trap frame
- xv6: [vectors.pl](https://github.com/mit-pdos/xv6-public/blob/master/vectors.pl) — ISR stub generator
- Linux: [arch/x86/kernel/traps.c](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/traps.c) — trap setup, IST for DF/MC
- Linux: [arch/x86/entry/entry_64.S](https://github.com/torvalds/linux/blob/master/arch/x86/entry/entry_64.S) — idtentry macro
- SerenityOS: [Kernel/Arch/x86_64/Interrupts/APIC.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp) — OOP interrupt framework
