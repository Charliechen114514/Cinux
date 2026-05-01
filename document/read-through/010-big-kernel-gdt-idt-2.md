# 010-2 Read-through: 大内核 IDT + ISR + 异常处理——中断栈帧与寄存器 dump

## 概览

本文讲解 tag 010 中大内核 IDT（中断描述符表）、ISR（中断服务例程）汇编跳板和 C++ 异常处理函数的完整实现。这是整个 tag 的核心部分——把 GDT 的基础设施转化成"CPU 异常能被捕获、寄存器能被 dump、非致命异常后程序能继续执行"的实际能力。

本节涉及四个源文件：`kernel/arch/x86_64/idt.hpp`（122 行，IDT 类型和栈帧定义）、`kernel/arch/x86_64/idt.cpp`（127 行，IDT 初始化）、`kernel/arch/x86_64/interrupts.S`（162 行，ISR 汇编跳板）、`kernel/arch/x86_64/exception_handlers.cpp`（179 行，C++ 异常处理函数），以及集成入口 `kernel/main.cpp` 和测试代码。

关键设计决策一览：
- ExceptionVector scoped enum 替代魔术数字
- 数据驱动的路由表配置 IDT（vs 逐个手写 set_handler 调用）
- ISR_NOERRCODE/ISR_ERRCODE 两个 GAS 宏生成 stub
- InterruptFrame 字段顺序严格匹配汇编 push 顺序
- 致命异常 halt / 非致命异常 continue 的二分策略

---

## 架构图

```
CPU 异常触发
    │
    ├── CPU 自动压入: SS, RSP, RFLAGS, CS, RIP [, Error Code]
    │
    └── 跳转到 IDT[vector] 中指定的 ISR stub
            │
            ├── ISR_NOERRCODE: push $0 (伪 error code)
            │   ISR_ERRCODE:   (CPU 已 push error code)
            │
            ├── push rax, rbx, rcx, ..., r15 (15 个寄存器)
            ├── movq %rsp, %rdi  (InterruptFrame* 作第一参数)
            ├── call handle_xx   (C handler)
            ├── pop r15, r14, ..., rax (恢复寄存器)
            ├── addq $8, %rsp   (跳过 error code)
            └── iretq            (恢复 RIP/CS/RFLAGS/RSP/SS)
```

```
IDT::init() 的数据驱动路由:

Route 表:
  ┌──────────────────────────────────────────────────┐
  │ DE  → isr_de_stub  → Kernel/Interrupt           │
  │ DB  → isr_db_stub  → Kernel/Trap                │
  │ NMI → isr_nmi_stub → Kernel/Interrupt           │
  │ BP  → isr_bp_stub  → User/Trap      ← DPL=3    │
  │ OF  → isr_of_stub  → Kernel/Interrupt           │
  │ BR  → isr_br_stub  → Kernel/Interrupt           │
  │ UD  → isr_ud_stub  → Kernel/Interrupt           │
  │ NM  → isr_nm_stub  → Kernel/Interrupt           │
  │ DF  → isr_df_stub  → Kernel/Interrupt (errcode) │
  │ TS  → isr_ts_stub  → Kernel/Interrupt (errcode) │
  │ NP  → isr_np_stub  → Kernel/Interrupt (errcode) │
  │ SS  → isr_ss_stub  → Kernel/Interrupt (errcode) │
  │ GP  → isr_gp_stub  → Kernel/Interrupt (errcode) │
  │ PF  → isr_pf_stub  → Kernel/Interrupt (errcode) │
  └──────────────────────────────────────────────────┘
  遍历 → set_handler() → IDT 条目填充
  最后 → load() → lidt
```

---

## 代码精讲

### idt.hpp: ExceptionVector 枚举

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
    DF  = 8,    // #DF: Double Fault (has error code)
    TS  = 10,   // #TS: Invalid TSS (has error code)
    NP  = 11,   // #NP: Segment Not Present (has error code)
    SS  = 12,   // #SS: Stack-Segment Fault (has error code)
    GP  = 13,   // #GP: General Protection (has error code)
    PF  = 14,   // #PF: Page Fault (has error code)
};
```

向量 9（Coprocessor Segment Overrun）被跳过了——在现代 x86_64 CPU 上这个异常已经不再产生，Intel 保留了这个向量号但不再使用。注意向量号不是连续的（8 之后直接跳到 10），这在路由表中需要准确对应。

### idt.hpp: IDTGateType、IDTPrivilege 和 make_idt_attr

```cpp
enum class IDTGateType : uint8_t {
    Interrupt = 0x0E,  // 64-bit interrupt gate (clears IF)
    Trap      = 0x0F,  // 64-bit trap gate (preserves IF)
};

enum class IDTPrivilege : uint8_t {
    Kernel = 0x00,  // Ring 0 only
    User   = 0x60,  // Ring 3 (DPL=3)
};

constexpr uint8_t make_idt_attr(IDTPrivilege priv, IDTGateType gate) {
    return 0x80 | static_cast<uint8_t>(priv) | static_cast<uint8_t>(gate);
}
```

`make_idt_attr` 把三个信息打包进一个字节：Present 位（0x80）、DPL（0x00 或 0x60）、Gate Type（0x0E 或 0x0F）。组合结果是：
- Kernel + Interrupt = 0x8E（最常用的配置，用于 #DE/#UD/#GP/#PF 等致命异常）
- Kernel + Trap = 0x8F（用于 #DB 调试异常，保持中断开启）
- User + Trap = 0xEF（用于 #BP 断点，允许用户态 `int $3` 触发）
- User + Interrupt = 0xEE（本项目未使用）

### idt.hpp: InterruptFrame 结构体

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

这个结构体的字段顺序是整个中断处理子系统中最微妙的部分。从低地址到高地址（也就是从栈顶向下），布局是：r15（最先被 ISR stub push，在栈上最深，结构体最前面）→ r14 → ... → rax（最后被 push，在栈上最浅）→ error_code（由 ISR stub push $0 或 CPU 自动 push）→ rip（由 CPU 自动 push）→ cs → rflags → rsp → ss（由 CPU 自动 push，在栈上最高位）。

RSP 在 `call handle_xx` 之前指向 r15 的位置，也就是 `InterruptFrame*` 的值。C handler 通过这个指针可以访问所有寄存器的值。如果字段顺序和汇编 push 顺序不匹配——比如 rdi 和 rbp 写反了——程序不会崩溃（因为都是 uint64_t，大小一样），但打印出来的寄存器值全部是错的，你可能会花半天时间怀疑是 CPU 的问题。

### idt.hpp: IDT class

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

    struct [[gnu::packed]] Pointer {
        uint16_t limit;
        uint64_t base;
    };

    static constexpr uint16_t kMaxEntries = 256;

    Entry entries_[kMaxEntries]{};
    Pointer idtr_{};

    void load();
};

extern IDT g_idt;
```

`Entry` 结构体 16 字节，严格按照 Intel SDM Vol.3A §6.14.1 Figure 6-8 的 64 位 IDT gate descriptor 格式定义。地址被拆成三段：`offset_low`（bit 0-15）、`offset_mid`（bit 16-31）、`offset_high`（bit 32-63），这是因为 x86 架构的历史遗留——从 286 的 24 位地址到 386 的 32 位地址再到 x86_64 的 64 位地址，每次扩展都在原有结构上追加字段，而不是重新设计。

`entries_` 数组 256 个条目全部 zero-initialized（`{}`），所以未配置的向量条目 type_attr 为 0（Present 位未设置），CPU 访问这些条目时触发 #GP。这比留下未初始化的垃圾值安全得多——垃圾值可能恰好有 Present=1，CPU 会跳到一个随机地址执行。

### idt.cpp: IDT::set_handler()

```cpp
void IDT::set_handler(ExceptionVector vector, Stub stub,
                      uint16_t selector, uint8_t type_attr, uint8_t ist) {
    const auto vec  = static_cast<uint8_t>(vector);
    const auto addr = reinterpret_cast<uint64_t>(stub);

    entries_[vec].offset_low  = static_cast<uint16_t>(addr & 0xFFFF);
    entries_[vec].offset_mid  = static_cast<uint16_t>((addr >> 16) & 0xFFFF);
    entries_[vec].offset_high = static_cast<uint32_t>((addr >> 32) & 0xFFFFFFFF);
    entries_[vec].selector    = selector;
    entries_[vec].ist         = ist;
    entries_[vec].type_attr   = type_attr;
    entries_[vec].reserved    = 0;
}
```

这个函数把 64 位处理程序地址拆成三段存入 IDT 条目。位操作虽然看起来啰嗦，但每一行都是精确的——`addr & 0xFFFF` 取低 16 位，`(addr >> 16) & 0xFFFF` 取中 16 位，`(addr >> 32) & 0xFFFFFFFF` 取高 32 位。在大内核的 higher-half 地址空间（`0xFFFFFFFF80000000`）中，高 32 位是 `0xFFFFFFFF`，中 16 位是 `0x8000`，低 16 位是具体偏移。如果你不小心用错了位移量（比如 `>> 15` 而不是 `>> 16`），CPU 就会跳到一个错了一位的地址，大概率 triple fault。

### idt.cpp: IDT::init()——数据驱动的路由表

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
        {ExceptionVector::OF,  isr_of_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::BR,  isr_br_stub, IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::UD,  isr_ud_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::NM,  isr_nm_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::DF,  isr_df_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::TS,  isr_ts_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::NP,  isr_np_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::SS,  isr_ss_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::GP,  isr_gp_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
        {ExceptionVector::PF,  isr_pf_stub,  IDTPrivilege::Kernel, IDTGateType::Interrupt},
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

先清零全部 256 个条目（防御性编程，虽然 `{}` 初始化已经是全零了），然后定义路由表。这个路由表是本节最精巧的设计——14 条路由，每条包含了向量号、ISR stub 地址、特权级和门类型的完整信息，一目了然。对比 xv6 的 `tvinit()` 函数用循环 + switch 语句逐个配置 256 个 IDT 条目的方式，我们的路由表更紧凑、更容易维护。如果将来要添加新的异常处理（比如向量 17 的 #AC Alignment Check），只需要在路由表中加一行，不用改任何其他代码。

门类型策略：只有 #BP 和 #DB 用 Trap Gate（保持中断开启），其余全部 Interrupt Gate（关中断）。特权级策略：只有 #BP 和 #DB 设为 User（DPL=3，允许 `int $3` 从 Ring 3 触发），其余全部 Kernel（DPL=0，只有硬件/内核能触发）。

所有条目的 selector 固定为 `GDT_KERNEL_CODE`（0x08）——因为所有异常处理代码都在内核态执行，需要使用内核代码段。IST 全部为 0，表示不使用 IST 栈切换——将来可以为 #DF 分配专用 IST 栈。

### idt.cpp: IDT::load()

```cpp
void IDT::load() {
    __asm__ volatile("lidt %[idtr]\n\t"
                     : : [idtr] "m"(idtr_) : "memory");
}
```

比 GDT 的 `load()` 简单得多——`lidt` 只需要从内存读取 10 字节（2 字节 limit + 8 字节 base）并加载到 IDTR。不需要像 GDT 那样刷新段寄存器，因为 IDT 本身不涉及段选择子的改变。

### interrupts.S: ISR_NOERRCODE 宏

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

    movq %rsp, %rdi                   # pass InterruptFrame* as first arg
    call \handler                     # call C exception handler

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

这个宏在功能上做了五步操作。第一步，`pushq $0` 在栈上填一个假的 error code 0，保证所有异常的栈帧布局统一。第二步，push 15 个通用寄存器（RAX 先 push，R15 最后 push），构造 InterruptFrame 的上半部分。第三步，`movq %rsp, %rdi` 把栈指针作为第一个参数传给 C handler——此时 RSP 指向 R15 在栈上的位置，正好是 `InterruptFrame*` 指向的地址。第四步，`call \handler` 调用对应的 C 处理函数。第五步，恢复 15 个寄存器（pop 顺序和 push 相反：R15 先 pop，RAX 最后 pop），然后 `addq $8, %rsp` 跳过栈上的 error code（不管它是真的还是假的），最后 `iretq` 从栈上恢复 RIP/CS/RFLAGS/RSP/SS，回到被中断的代码。

push 顺序之所以是 RAX→R15（而不是 R15→RAX），是因为 push 是从先到后压入栈的——先 push 的在栈上位置更高（地址更大），后 push 的在栈上位置更低（地址更小）。RSP 指向最后 push 的 R15（最低地址），所以 InterruptFrame 的第一个字段是 R15。这和结构体从上到下的顺序正好一致。

### interrupts.S: ISR_ERRCODE 宏

```asm
.macro ISR_ERRCODE name handler
.global \name
.type \name, @function
\name:
    /* CPU already pushed error code — no dummy push needed */
    pushq %rax
    pushq %rbx
    ... (same register save/restore as ISR_NOERRCODE)
    addq $8, %rsp
    iretq
.endm
```

唯一的区别是没有 `pushq $0`——CPU 在触发这些异常时已经自动 push 了真正的 error code。比如 #PF（向量 14）的 error code 包含了缺页的原因信息（读/写、用户/内核、页不存在/权限违反等），#GP（向量 13）的 error code 包含了触发异常的段选择子索引。

### interrupts.S: 15 个 stub 实例化

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

8 个无错误码 + 7 个有错误码 = 15 个 stub。向量 9 被跳过（现代 CPU 不用），向量 15-31 暂不处理（将来需要时再加）。对比 xv6 用 Perl 脚本生成 256 个 stub 的方式，我们手写的 15 个 stub 虽然覆盖面小，但每一行都清楚明了，教学价值更高。

### exception_handlers.cpp: dump_registers 和 fatal_halt

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
    kprintf("  ... (all registers)\n");
    kprintf("  ERROR CODE = %p\n",
            reinterpret_cast<void*>(frame->error_code));
    kprintf("========================================\n");
}

[[noreturn]] void fatal_halt() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`dump_registers` 用 `%p` 格式输出寄存器值——虽然 `%p` 的标准语义是打印指针地址，但在内核调试场景下，把寄存器值当指针打印是最直观的方式，因为内核地址和指针地址本来就是同一套表示。`reinterpret_cast<void*>` 是为了满足 `%p` 的类型要求（`void*`），虽然这在技术上是一个 implementation-defined 的转换，但在 x86_64 的 freestanding 环境下，`uint64_t` 和 `void*` 的二进制表示是一致的。

`fatal_halt` 标记为 `[[noreturn]]`——告诉编译器这个函数永远不会返回，编译器可以据此优化调用点的代码（比如不需要在 `fatal_halt()` 调用之后放任何指令）。`cli; hlt` 放在循环里是防御性编程——虽然 `cli` 关了可屏蔽中断，但 NMI 仍然能唤醒 `hlt` 的 CPU。

### exception_handlers.cpp: #BP handler（非致命）

```cpp
void handle_bp(InterruptFrame* frame) {
    dump_registers(frame, "#BP", 3);
    kprintf("[EXCEPTION] Breakpoint at RIP=%p\n",
            reinterpret_cast<void*>(frame->rip));
    kprintf("[EXCEPTION] Continuing...\n");
}
```

#BP 是非致命异常的典范——打印完信息后直接返回，ISR stub 中的 `iretq` 会恢复 CPU 到触发断点的下一条指令。这也是为什么 #BP 使用 Trap Gate——如果用 Interrupt Gate，IF 被清除意味着中断被关闭了，如果后续代码依赖中断（比如定时器中断驱动调度），系统就会卡死。当然，我们当前还没有开中断（sti），所以这个区别暂时没有实际影响，但设计上是正确的。

### exception_handlers.cpp: #PF handler（致命 + CR2 + error code 解码）

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

#PF 的 error code 格式和其他异常不同——它有自己的专用格式（Intel SDM Vol.3A §6.13 Figure 6-8）。bit 0 表示是"页不存在"还是"权限违反"，bit 1 表示是读还是写，bit 2 表示是内核态还是用户态，bit 3 表示是否设置了保留位，bit 4 表示是否是指令取指。CR2 寄存器保存了触发缺页的虚拟地址——这是 CPU 在触发 #PF 时自动设置的，不需要在中断处理中做任何额外操作。

这些信息在调试内存管理相关的 bug 时非常有价值。比如你看到一个 "page not present read kernel" 的错误，就知道是内核态代码读了一个未映射的地址；如果看到 "protection violation write user"，就知道是用户态代码试图写一个只读的页面。

### main.cpp: kernel_main 集成

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

初始化顺序是 `kprintf_init` → `g_gdt.init` → `g_idt.init`——串口先初始化（后续的 kprintf 需要它），GDT 在 IDT 之前（因为 IDT 的 selector 依赖 GDT 中的 code segment），IDT 在 `int $3` 之前（否则中断触发时找不到处理程序）。注意我们**没有**调用 `sti`——因为还没有配置 IRQ 处理程序，PIT 定时器会在 `sti` 后的几毫秒内触发 IRQ 0，跳到 IDT[32]（一个空条目），导致 #GP → Double Fault → Triple Fault。

### test/test_gdt_idt.cpp: 集成测试

```cpp
namespace test_gdt_segments {
void test_cs_register() {
    uint16_t cs = 0;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    TEST_ASSERT_EQ(cs, GDT_KERNEL_CODE);
}
// ... DS, SS, ES 同理
}

namespace test_bp_exception {
void test_bp_continues_execution() {
    volatile int marker_before = 0x1234;
    __asm__ volatile("int $3");
    volatile int marker_after = 0x5678;
    TEST_ASSERT_EQ(marker_before, 0x1234);
    TEST_ASSERT_EQ(marker_after, 0x5678);
}
}

namespace test_multiple_exceptions {
void test_multiple_bp() {
    volatile uint64_t canary = 0xCAFEBABEDEADC0DEULL;
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULL);
    __asm__ volatile("int $3");
    TEST_ASSERT_EQ(canary, 0xCAFEBABEDEADC0DEULLULL);
}
}

namespace test_idt_policy {
void test_bp_gate_is_user_trap() {
    uint8_t attr = cinux::arch::make_idt_attr(
        cinux::arch::IDTPrivilege::User,
        cinux::arch::IDTGateType::Trap);
    TEST_ASSERT_EQ(attr, 0xEFu);
}
// ... Kernel/Interrupt = 0x8E
}
```

四个测试用例覆盖了 GDT/IDT 的核心功能：段寄存器值验证（CS=0x08, DS/SS/ES=0x10）、单次 #BP 触发和恢复、多次 #BP 的栈帧完整性（canary 值不变说明 ISR 没有破坏栈）、以及 IDT gate type policy 的编译期验证。

---

## 设计决策

### 决策：数据驱动路由表 vs 逐个手写 set_handler

**问题**: IDT 条目如何配置——路由表还是逐个调用？

**本项目的做法**: 定义 `Route` 结构体数组，遍历配置。

**备选方案**: 在 `init()` 中逐个调用 `set_handler(ExceptionVector::DE, isr_de_stub, GDT_KERNEL_CODE, 0x8E, 0)` 等等 14 次。

**为什么不选备选方案**: 14 次 `set_handler` 调用中有大量重复（selector 固定 0x08、IST 固定 0），只有向量号、stub 地址和 type_attr 不同。路由表把这些差异集中在一起，一眼就能看出每个异常的门类型和特权级配置。

### 决策：ISR_NOERRCODE 推 dummy 0 vs 在 C handler 里区分

**问题**: 如何统一有/无 error code 的异常栈帧？

**本项目的做法**: 在 ISR stub 里对无 error code 的异常 push $0，让所有异常到达 C handler 时栈帧布局统一。

**备选方案**: 不推 dummy 0，在 C handler 里根据向量号判断是否有 error code，用不同的结构体偏移读取。

**为什么不选备选方案**: 这样做需要两个版本的 InterruptFrame 结构体、两个版本的 handler、或者在每个 handler 里加条件判断。统一栈帧布局的方案虽然多 push 了一个 0（8 字节），但大大简化了 C 端的处理逻辑。

---

## 扩展方向

1. **(1 星)** 为 #DF（Double Fault）分配 IST 栈——在 TSS 的 IST1 字段设置一个专用栈地址，然后在 IDT 的 #DF 条目中设置 IST=1。

2. **(2 星)** 添加更多异常向量——配置向量 17（#AC Alignment Check）和向量 32+（硬件 IRQ），扩展路由表。

3. **(2 星)** 在 #PF handler 中添加符号解析——维护一个函数地址范围表，#PF 时根据 RIP 查找对应的函数名并打印。

4. **(3 星)** 实现中断嵌套——在 Trap Gate handler 中短暂开中断，允许高优先级中断打断当前处理。

5. **(3 星)** 参考 xv6 的 vectors.pl 自动生成方案——用脚本自动生成 256 个 ISR stub，避免手写宏实例化。

---

## 参考资料

- Intel SDM: Vol.3A §6.2 — Exception and Interrupt Vectors (Table 6-1, pp.6-4~6-6)
- Intel SDM: Vol.3A §6.12.1.3 — Flag Usage (p.6-17)
- Intel SDM: Vol.3A §6.13 — Error Codes (pp.6-18~6-19)
- Intel SDM: Vol.3A §6.14 — Exception/Interrupt Handling in 64-bit Mode (pp.6-20~6-23)
- OSDev Wiki: [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)
- OSDev Wiki: [Interrupts](https://wiki.osdev.org/Interrupts)
- xv6: [trap.c](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) — unified trap dispatch, vectors.pl
- Linux: [arch/x86/kernel/traps.c](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/traps.c) — early trap setup, IST usage
- SerenityOS: [Kernel/Arch/x86_64/Interrupts/APIC.cpp](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/Interrupts/APIC.cpp) — OOP interrupt framework
