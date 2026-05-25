---
title: 010-big-kernel-gdt-idt-2 · GDT/IDT 重构
---

# 010-2 Read-through: 大内核 IDT + ISR 汇编跳板——中断描述符表与中断栈帧

## 概览

本文讲解 tag 010 中大内核 IDT（中断描述符表）和 ISR（中断服务例程）汇编跳板的完整实现。IDT 是中断分发的核心数据结构——CPU 遇到异常时，拿着向量号去 IDT 里查对应的处理程序地址，然后跳过去执行。ISR stub 是连接 CPU 硬件行为和 C 处理函数的"桥梁"——它负责保存/恢复寄存器、构造统一的 `InterruptFrame` 栈帧、调用 C handler。

本节涉及三个源文件：`kernel/arch/x86_64/idt.hpp`（122 行，IDT 类型和栈帧定义）、`kernel/arch/x86_64/idt.cpp`（127 行，IDT 初始化）、`kernel/arch/x86_64/interrupts.S`（162 行，ISR 汇编跳板）。C++ 异常处理函数和集成测试在下一节（010-3）中讲解。

关键设计决策一览：
- ExceptionVector scoped enum 替代魔术数字
- 数据驱动的路由表配置 IDT（vs 逐个手写 set_handler 调用）
- ISR_NOERRCODE/ISR_ERRCODE 两个 GAS 宏生成 stub
- InterruptFrame 字段顺序严格匹配汇编 push 顺序

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

3. **(2 星)** 参考 xv6 的 vectors.pl 自动生成方案——用脚本自动生成 256 个 ISR stub，避免手写宏实例化。

---

## 参考资料

- Intel SDM: Vol.3A §6.2 — Exception and Interrupt Vectors (Table 6-1, pp.6-4~6-6)
- Intel SDM: Vol.3A §6.12.1.3 — Flag Usage (p.6-17)
- Intel SDM: Vol.3A §6.14 — Exception/Interrupt Handling in 64-bit Mode (pp.6-20~6-23)
- Intel SDM: Vol.3A §6.14.1 — 64-Bit Mode IDT (Figure 6-8, p.6-20)
- OSDev Wiki: [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)
- OSDev Wiki: [Interrupts](https://wiki.osdev.org/Interrupts)
- xv6: [trap.c](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) — unified trap dispatch, vectors.pl
- Linux: [arch/x86/kernel/traps.c](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/traps.c) — early trap setup, IST usage
