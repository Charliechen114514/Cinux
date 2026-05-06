# 007-2 Read-through: IDT 实现

## 本章概览

这一篇我们通读 Cinux mini kernel 的 IDT 实现代码——idt.hpp 和 idt.cpp 这两个文件。在整个 tag 007 的架构中，IDT 是中断处理链路的第二环：GDT 提供段寄存器配置（上一篇），IDT 提供中断向量表（本篇），ISR Stub 和异常处理函数（下一篇）负责实际执行。

关键设计决策有三个。第一，IDT 只配两个向量（#BP 和 #PF），因为我们当前阶段只需要断点调试和缺页检测。第二，InterruptFrame 的字段布局严格匹配 ISR stub 的 push 顺序，确保 C 处理函数能正确读取寄存器值。第三，ISR stub 和 handler 函数用 `extern "C"` 声明，保证 C++ 和汇编之间的符号链接正确。

---

## 架构图

```
IDT 初始化链路：

mini_kernel_main()
       |
       +-- gdt_init()  (上一篇)
       |
       +-- idt_init()
             |
             +-- 清空 256 个 IdtEntry
             +-- set_idt_entry(3, isr_bp_stub, 0x08, 0x8F)  <- Trap Gate
             +-- set_idt_entry(14, isr_pf_stub, 0x08, 0x8E) <- Interrupt Gate
             +-- 构造 IdtPointer
             +-- LIDT


CPU 查找链路（中断触发时）：

CPU 异常 -> 查 IDT[向量号] -> 得到 selector(0x08) + handler 地址
                                    |
                             查 GDT[selector>>3] -> 得到 code64 段属性
                                    |
                             压栈 SS/RSP/RFLAGS/CS/RIP -> 跳转 handler
```

---

## 代码精讲

### IDT 头文件 — idt.hpp

IDT 的头文件比 GDT 大一些，因为除了 IDT 表本身的描述符结构外，还需要定义 InterruptFrame——这是整个中断处理链路中最关键的数据结构。

常量部分定义了 IDT 最大条目数（256）、本阶段需要配置的两个向量号（#BP=3, #PF=14）以及两种门类型（Interrupt Gate=0x0E, Trap Gate=0x0F）。这些常量在后续代码中多处引用，集中定义方便维护。

```cpp
// idt.hpp 常量
constexpr uint16_t IDT_MAX_ENTRIES = 256;
constexpr uint8_t IDT_VEC_BP = 3;
constexpr uint8_t IDT_VEC_PF = 14;
constexpr uint8_t IDT_TYPE_INTERRUPT_GATE = 0x0E;
constexpr uint8_t IDT_TYPE_TRAP_GATE = 0x0F;
```

`IdtEntry` 是 16 字节的 packed 结构，和 GDT 的 8 字节描述符相比大了一倍。这 16 字节中，handler 的 64 位地址被拆成了三段存放——`offset_low`（低 16 位）、`offset_mid`（中 16 位）和 `offset_high`（高 32 位）。这是 x86_64 的 IDT 格式决定的（Intel SDM Figure 6-8），没法用一条指令设置完整地址，必须手动拆分。`selector` 字段指向 GDT 中的代码段——这里填的就是 `SEGMENT_CODE64`（0x08），这就是 IDT 必须依赖 GDT 先初始化的原因。`ist` 字段是 IST（Interrupt Stack Table）偏移，填 0 表示不使用 IST 机制。`type_attr` 字段编码了 Present、DPL 和 Gate Type 三个信息。

```cpp
// idt.hpp IdtEntry 结构
struct IdtEntry {
    uint16_t offset_low;    // handler 地址 [0:15]
    uint16_t selector;      // 代码段选择子（CS）
    uint8_t  ist;           // IST 偏移（0 = 不用）
    uint8_t  type_attr;     // P | DPL | 0 | Gate Type
    uint16_t offset_mid;    // handler 地址 [16:31]
    uint32_t offset_high;   // handler 地址 [32:63]
    uint32_t reserved;
} __attribute__((packed));
```

`InterruptFrame` 的布局需要特别仔细地理解。前 15 个字段（r15 到 rax）由 ISR stub 手动保存，error_code 也是 stub 处理的——有硬件错误码的异常就保留 CPU 压入的值，没有的就压一个 0。最后 5 个字段（rip 到 ss）是 CPU 自动压入的。这个布局必须和 interrupts.S 中的 push 顺序严格对应——任何错位都会导致 C 处理函数读到错误的寄存器值，而且这种 bug 非常难发现，因为它不会立刻 crash。

```cpp
// idt.hpp InterruptFrame 结构
struct InterruptFrame {
    uint64_t r15, r14, r13, r12;
    uint64_t r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rdx;
    uint64_t rcx, rbx, rax;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};
```

为什么字段的声明顺序和 push 顺序是"反过来"的？因为栈从高地址往低地址增长，而结构体从低地址往高地址排列。先 push 的值在栈的低地址端，对应结构体的前面成员。所以汇编中先 push rax（最后面），最后 push r15（最前面），这样 pop 的时候 pop r15 刚好对应结构体的第一个字段。这个 push/pop 的顺序问题在下一篇讲 ISR Stub 时会更详细地分析。

### IDT 实现 — idt.cpp

IDT 的实现文件和 GDT 类似，也是静态全局变量加辅助函数加公开接口的模式。

ISR stub 和异常处理函数的声明用 `extern "C"` 包裹，确保使用 C 链接约定。这是因为 interrupts.S 中的 `call handle_bp` 是 C 链接的符号名——如果用 C++ 的 name mangling，`handle_bp` 会被编译器改写成类似 `_Z8handle_bpP16InterruptFrame` 的名字，链接器就找不到匹配的符号了。这是内核开发中混合 C/C++/汇编时的标准做法。

```cpp
// idt.cpp - 外部声明
extern "C" void isr_bp_stub();
extern "C" void isr_pf_stub();
extern "C" void handle_bp(InterruptFrame* frame);
extern "C" void handle_pf(InterruptFrame* frame);
```

辅助函数 `set_idt_entry` 把 handler 的 64 位地址拆成三段填入 IdtEntry。地址拆分的位操作非常直观：低 16 位用 `addr & 0xFFFF`，中 16 位用 `(addr >> 16) & 0xFFFF`，高 32 位用 `(addr >> 32) & 0xFFFFFFFF`。每次设置都会把 reserved 字段清零、ist 和 type_attr 设为传入的参数。

```cpp
// idt.cpp - set_idt_entry 辅助函数
static void set_idt_entry(uint8_t vector, void* handler, uint16_t selector,
                          uint8_t type_attr, uint8_t ist) {
    uint64_t addr = reinterpret_cast<uint64_t>(handler);
    s_idt[vector].offset_low  = addr & 0xFFFF;
    s_idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    s_idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    s_idt[vector].selector   = selector;
    s_idt[vector].ist        = ist;
    s_idt[vector].type_attr  = type_attr;
    s_idt[vector].reserved   = 0;
}
```

`idt_init()` 的实现很直白。先清空全部 256 个条目（全零意味着 Present=0，访问时触发 #GP），然后只配置两个向量。#BP 用陷阱门（type_attr=0x8F），这意味着进入处理程序时 IF 不被清除，允许在断点处理期间响应其他中断；#PF 用中断门（0x8E），CPU 会自动关中断。两者都是 Present=1、DPL=0，只有 ring 0 能触发。最后构造 IDTR 并执行 LIDT——和 LGDT 不同的是，LIDT 之后不需要任何刷新操作，因为 IDTR 不是段寄存器，它的内容在下一次中断触发时自然生效。

---

## 别人怎么做的

### xv6 的 IDT

xv6 在 `tvinit()` 中一次性配置全部 256 个 IDT 条目，使用 Perl 脚本（vectors.pl）在构建时自动生成 256 个 ISR stub。所有向量默认使用 Interrupt Gate（istrap=0），只有系统调用向量 T_SYSCALL 使用 Trap Gate 并设置 DPL=DPL_USER 允许用户态触发。

和 Cinux 的 16 字节 IdtEntry 相比，xv6 的 `gatedesc` 只有 8 字节（32 位 IDT 条目），因为 32 位模式下处理程序地址只需要 32 位，不需要拆成三段。这是 32 位和 64 位 x86 在中断机制上的一个关键差异。

### SerenityOS 的 IDT

SerenityOS 的 IDT 配置更为复杂——它使用 Local APIC + I/O APIC（而不是传统 8259 PIC），支持 256 个全部可用向量。每个 CPU 有独立的 IDTR，AP 启动时从 BSP 复制 IDT。SerenityOS 还使用了 IST 机制为 #DF 和 #MC 配置独立栈。

---

## 代码精讲（续）

```cpp
// idt.cpp - idt_init
void idt_init() {
    for (uint16_t i = 0; i < IDT_MAX_ENTRIES; i++)
        s_idt[i] = IdtEntry{};

    set_idt_entry(IDT_VEC_BP, reinterpret_cast<void*>(isr_bp_stub),
                  SEGMENT_CODE64, 0x8F, 0);
    set_idt_entry(IDT_VEC_PF, reinterpret_cast<void*>(isr_pf_stub),
                  SEGMENT_CODE64, 0x8E, 0);

    s_idt_pointer.limit = static_cast<uint16_t>(sizeof(s_idt) - 1);
    s_idt_pointer.base  = reinterpret_cast<uint64_t>(&s_idt);
    __asm__ volatile ("lidt %[idtr]" : : [idtr] "m" (s_idt_pointer) : "memory");
}
```

---

## 设计决策

### 决策：IDT 只配两个向量

**问题**: IDT 配多少向量——256 个全配还是最小子集。

**本项目的做法**: 只配 #BP(3) 和 #PF(14)。

**备选方案**: 配全部 32 个 CPU 异常向量。

**为什么不选备选方案**: 一是当前不需要——其他异常（如 #DE 除零、#UD 非法指令）的处理逻辑和 #BP 本质相同，配了也只是多几行代码；二是硬件中断（IRQ 0-15）需要先配 PIC，PIC 编程本身就是一个独立的主题（tag 011）；三是保持每个 milestone 的复杂度可控，让学习者能聚焦在核心概念上。

### 决策：#BP 用陷阱门，#PF 用中断门

**问题**: 两种异常分别使用什么门类型。

**本项目的做法**: #BP 用陷阱门（0x8F，不清除 IF），#PF 用中断门（0x8E，清除 IF）。

**理由**: #BP 是调试用的断点异常，处理过程中如果禁止其他中断，可能会错过时序敏感的事件（比如时钟中断），所以用陷阱门更合理。#PF 涉及页表操作，这是一个需要原子性的过程——如果处理到一半被另一个中断打断，可能导致页表状态不一致，所以用中断门保护。Intel 手册也是这么建议的。

---

## 扩展方向

- 配置全部 32 个 CPU 异常向量（难度：低）——体力活，每个向量只需要一个 ISR stub 和一个处理函数
- 为 IDT 添加 #GP(13) 向量（难度：低）——#GP 是内核开发中最常见的异常之一，几乎所有的段/权限错误都会触发它
- 使用 IST 机制为 #DF 配置独立栈（难度：较高）——需要先添加 TSS，然后在 IDT 条目中设置 ist 字段

---

## 文件清单

| 文件 | 职责 | 行数（大约） |
|------|------|-------------|
| `kernel/mini/arch/x86_64/idt.hpp` | IDT 常量、IdtEntry、InterruptFrame、接口声明 | ~112 行 |
| `kernel/mini/arch/x86_64/idt.cpp` | set_idt_entry + idt_init 实现 | ~115 行 |

idt.hpp 是 tag 007 中最重要的头文件，因为 InterruptFrame 结构体在整个内核的中断处理中都会被使用。后续 tag 010（完整 IDT）和 tag 011（硬件中断）都会复用这个结构体。

---

## 参考资料

- Intel SDM: Vol.3A 6.10 (Interrupt Descriptor Table) -- IDT 格式
- Intel SDM: Vol.3A 6.14.1 (pp.6-20) -- 64-Bit Mode IDT Gate Descriptors (Figure 6-8)
- Intel SDM: Vol.3A 2.4.3 (Figure 2-6) -- IDTR Register
- Intel SDM: Vol.3A 6.12.1.3 (p.6-17) -- Interrupt Gate vs Trap Gate 对 IF 标志的影响
- OSDev Wiki: [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)
- xv6: [trap.c / mmu.h](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) -- 32-bit IDT setup
