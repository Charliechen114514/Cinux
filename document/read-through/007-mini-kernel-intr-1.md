# 007-1 Read-through: GDT 与 IDT 实现

## 本章概览

这一篇我们通读 Cinux mini kernel 的 GDT 和 IDT 实现代码——gdt.hpp/cpp 和 idt.hpp/cpp 这四个文件。在整个 tag 007 的架构中，这两部分构成了中断处理链路的前半段：GDT 提供段寄存器配置，IDT 提供中断向量表，告诉 CPU 遇到异常时该去哪里找处理程序。后续的 ISR Stub 和异常处理函数（下一篇覆盖）则是链路的后半段——真正执行保存寄存器和打印信息的工作。

关键设计决策有三个。第一，GDT 只设三项（null/code64/data64），足够满足 long mode 内核的需求——分段机制在 64 位下已经被大幅弱化，base 和 limit 字段被硬件忽略，我们只需要让 CS 指向一个合法的 64 位代码段描述符就行。第二，IDT 只配两个向量（#BP 和 #PF），因为我们当前阶段只需要断点调试和缺页检测。第三，所有数据结构都用 `__attribute__((packed))` 确保内存布局与硬件期望完全一致，这在内核开发中是不可跳过的步骤。

和 xv6 对比的话，xv6 在进入内核的第一时间就配置了完整的 GDT 和 IDT，包含 TSS 段和所有 256 个中断向量。我们这里采取了更渐进的方式——先跑通最小集合，等后续 milestone 需要硬件中断和用户态切换的时候再逐步填充。这种"最小可用子集"的策略在 OS 教学项目中很常见，好处是每一步的复杂度可控，坏处是中途可能需要回头改之前的结构。

---

## 架构图

```
GDT/IDT 初始化链路：

mini_kernel_main()
       │
       ├── gdt_init()
       │     │
       │     ├── make_gdt_entry(null):    全零
       │     ├── make_gdt_entry(code64):  0x9A/0x0A, L=1
       │     ├── make_gdt_entry(data64):  0x92/0x0C
       │     ├── 构造 GdtPointer
       │     └── LGDT + lretq (刷新CS) + mov DS/ES/FS/GS/SS
       │
       ├── idt_init()
       │     │
       │     ├── 清空 256 个 IdtEntry
       │     ├── set_idt_entry(3, isr_bp_stub, 0x08, 0x8F)  ← Trap Gate
       │     ├── set_idt_entry(14, isr_pf_stub, 0x08, 0x8E) ← Interrupt Gate
       │     ├── 构造 IdtPointer
       │     └── LIDT
       │
       └── pmm::init() → int $3 测试


CPU 查找链路（中断触发时）：

CPU 异常 → 查 IDT[向量号] → 得到 selector(0x08) + handler 地址
                                    ↓
                             查 GDT[selector>>3] → 得到 code64 段属性
                                    ↓
                             压栈 SS/RSP/RFLAGS/CS/RIP → 跳转 handler
```

---

## 代码精讲

### GDT 头文件 — gdt.hpp

我们先来看 GDT 的头文件。整个文件包裹在 `cinux::mini::arch` 命名空间中，这是 Cinux 内核代码组织的惯例——按子系统（arch/mm/driver/lib）分命名空间，避免符号冲突。

首先是常量定义部分。`GDT_ENTRIES = 3` 表示我们的 GDT 只有三个条目，这是 long mode 下最小的可用配置——null descriptor 占第一个位置（x86 硬性要求），code64 给内核代码段用，data64 给内核数据段和栈段用。三个段选择子常量 `SEGMENT_NULL`、`SEGMENT_CODE64`、`SEGMENT_DATA64` 的值分别是 0、8、16（即索引乘以 8），这是因为段选择子的格式是 `[Index(13位) : TI(1位) : RPL(2位)]`，TI=0 表示 GDT，RPL=0 表示 ring 0，所以低 3 位全零，选择子的值就等于索引乘以 8。

```cpp
// gdt.hpp 常量定义
constexpr uint8_t GDT_ENTRIES = 3;
constexpr uint8_t GDT_NULL_INDEX  = 0;
constexpr uint8_t GDT_CODE64_INDEX = 1;
constexpr uint8_t GDT_DATA64_INDEX = 2;
constexpr uint16_t SEGMENT_NULL  = GDT_NULL_INDEX  * 8;   // 0x00
constexpr uint16_t SEGMENT_CODE64 = GDT_CODE64_INDEX * 8;  // 0x08
constexpr uint16_t SEGMENT_DATA64 = GDT_DATA64_INDEX * 8;  // 0x10
```

这些常量在 IDT 初始化时也会用到——IDT 条目中的 selector 字段直接填 `SEGMENT_CODE64`（0x08），这就是为什么 GDT 必须先于 IDT 初始化的底层原因。

接下来是数据结构定义。`GdtEntry` 是 8 字节的 packed 结构，对应 x86 的 64 位段描述符格式。字段排列看起来有些奇怪——limit 和 base 各被拆成了好几段——这是 80286 时代的遗留设计，为了向后兼容一直保留到现在。不过在 long mode 下，base 和 limit 字段被硬件忽略（除了 GS/FS 的 base 通过 MSR 设置），我们真正关心的只有 access 和 flags_limit_high 这两个字节。

```cpp
// gdt.hpp 数据结构
struct GdtEntry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;           // 访问权限字节
    uint8_t  flags_limit_high; // 高4位flags + 低4位limit高4位
    uint8_t  base_high;
} __attribute__((packed));

struct GdtPointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
```

`GdtPointer` 是给 LGDT 指令用的操作数格式：2 字节 limit（GDT 字节数减 1）加 8 字节 base address（GDT 的线性地址）。注意在 64 位模式下 GDTR 是 80 位的（16+64），而在 32 位模式下是 48 位的（16+32），这也是为什么 `base` 字段用 uint64_t。

### GDT 实现 — gdt.cpp

现在来看 GDT 的实现。文件中维护两个静态全局变量：`s_gdt` 是 GDT 表实例，`s_gdt_pointer` 是 GDTR 加载用的指针结构。把它们声明为 static 是为了限制作用域在当前编译单元内——内核的其他模块不需要直接操作 GDT 表，只需要调用 `gdt_init()` 就行。

核心辅助函数 `make_gdt_entry` 接受四个参数（base、limit、access、flags），按 x86 格式把值拆分到对应字段。这个函数的实现是纯位操作——limit 的低 16 位直接赋值，高 4 位和 flags 拼成一个字节（`((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F)`），base 拆成三段分别赋值。在 long mode 下 base 和 limit 的值其实无所谓（被硬件忽略），我们填 base=0、limit=0xFFFFF（配合 G=1 等于 4GB）是出于惯例。

```cpp
// gdt.cpp - make_gdt_entry 辅助函数
static GdtEntry make_gdt_entry(uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    GdtEntry entry;
    entry.limit_low        = limit & 0xFFFF;
    entry.base_low         = base & 0xFFFF;
    entry.base_middle      = (base >> 16) & 0xFF;
    entry.access           = access;
    entry.flags_limit_high = ((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F);
    entry.base_high        = (base >> 24) & 0xFF;
    return entry;
}
```

`gdt_init()` 的主体非常直白——三个 `make_gdt_entry` 调用填写三项描述符，然后构造 GdtPointer 并用内联汇编加载。三项描述符的关键值需要仔细看一下。

第一项 null descriptor 全零，这是 x86 架构的硬性要求——CPU 不允许使用索引 0 对应的段，访问选择子 0x00 的段会触发 #GP。第二项 code64 的 access 是 `0x9A`，拆开来看是二进制 `10011010`——Present=1 表示段在内存中，DPL=00 表示 ring 0 内核态，S=1 表示代码/数据段（不是系统段），Type 位 `1010` 表示可执行且可读的代码段。flags 是 `0x0A`，即 G=1（4KB 粒度）和 L=1（64-bit long mode 标志）——这个 L 位是整个 GDT 中最关键的一个 bit，它告诉 CPU 这是一个 64 位代码段，这是 long mode 正常工作的前提条件。

```cpp
// gdt.cpp - 三项 GDT 配置
s_gdt[GDT_NULL_INDEX]  = make_gdt_entry(0, 0, 0, 0);           // null
s_gdt[GDT_CODE64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x9A, 0x0A); // code64
s_gdt[GDT_DATA64_INDEX] = make_gdt_entry(0, 0xFFFFF, 0x92, 0x0C); // data64
```

data segment 的 access 是 `0x92`，和 code 的区别在于 bit 3（Executable）是 0，表示这是一个数据段。flags 是 `0x0C`，即 G=1 和 D/B=1，注意 data segment 的 L 位被硬件忽略，D/B 位也基本被忽略，所以填 0x0C 是惯例。

加载 GDT 的汇编部分是整个 `gdt_init` 中最精巧的地方。单纯执行 `lgdt` 只修改了 GDTR 寄存器，但 CPU 内部的 CS 缓存不会因此更新——段寄存器的隐藏部分（base、limit、access rights）只有在加载新选择子时才会从 GDT 重新读取。所以我们用了一个 far return 的技巧来刷新 CS：先把新的 CS 选择子压栈，再把返回地址压栈，执行 `lretq`，CPU 就会从栈上弹出新的 CS 和 RIP，强制重新加载 CS 的描述符缓存。

```cpp
// gdt.cpp - LGDT + 刷新段寄存器
__asm__ volatile (
    "lgdt %[gdtr]\n\t"
    "pushq %[cs]\n\t"
    "leaq 1f(%%rip), %%rax\n\t"
    "pushq %%rax\n\t"
    "lretq\n\t"
    "1:\n\t"
    "movw %[ds], %%ax\n\t"
    "movw %%ax, %%ds\n\t"
    "movw %%ax, %%es\n\t"
    "movw %%ax, %%fs\n\t"
    "movw %%ax, %%gs\n\t"
    "movw %%ax, %%ss\n\t"
    :
    : [gdtr] "m" (s_gdt_pointer),
      [cs]   "i" (SEGMENT_CODE64),
      [ds]   "i" (SEGMENT_DATA64)
    : "rax", "memory"
);
```

这里选择 `lretq` 而不是 `ljmp` 是因为在 higher-half kernel 里 `ljmp` 需要一个绝对地址，而 `lretq` 可以用栈上的 RIP 相对地址（`leaq 1f(%%rip)`），对位置无关代码更友好。DS/ES/FS/GS/SS 则不需要这种技巧，直接用 mov 赋值就行，因为数据段寄存器没有代码段那种"缓存不更新"的问题。

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

### 决策：GDT 只设三项

**问题**: 需要决定 GDT 配置多少条目——是像 xv6 那样一开始就配完整（含 TSS 和用户态段），还是只配最小集合。

**本项目的做法**: 只配 3 项（null/code64/data64），TSS 和用户态段留给后续 tag 010（big kernel GDT/IDT）和 tag 022（ring3 用户态）。

**备选方案**: 在 tag 007 就配 6 项（加上 TSS、UCODE、UDATA）。

**为什么不选备选方案**: 一是每个 milestone 的目标应该尽可能聚焦——tag 007 的目标是"触发异常不死机"，不是"完整的段管理"；二是 TSS 需要额外定义 128 位的系统段描述符和实际的 TSS 结构体，代码量会翻倍；三是用户态段在当前阶段完全没有用途，配了也只是占位。

**如果要扩展**: tag 010 会添加 TSS（用于 ring 3 → ring 0 的栈切换），tag 022 会添加用户态 code/data 段。届时 GDT 会从 3 项扩展到 6 项。

### 决策：IDT 只配两个向量

**问题**: IDT 配多少向量——256 个全配还是最小子集。

**本项目的做法**: 只配 #BP(3) 和 #PF(14)。

**备选方案**: 配全部 32 个 CPU 异常向量。

**为什么不选备选方案**: 一是当前不需要——其他异常（如 #DE 除零、#UD 非法指令）的处理逻辑和 #BP 本质相同，配了也只是多几行代码；二是硬件中断（IRQ 0-15）需要先配 PIC，PIC 编程本身就是一个独立的主题（tag 011）；三是保持每个 milestone 的复杂度可控，让学习者能聚焦在核心概念上。

---

## 扩展方向

- 为 GDT 添加 TSS 段描述符和实际的 TSS 结构体（难度 ⭐⭐）
- 配置全部 32 个 CPU 异常向量（难度 ⭐）
- 使用 IST 机制为 #DF 和 #MC 配置独立的栈（难度 ⭐⭐⭐）
- 为 IDT 添加动态注册处理函数的接口（难度 ⭐⭐）

## 参考资料

- Intel SDM: Vol.3A §6.14.1-6.14.2 (pp.6-20~6-22) — 64-Bit Mode IDT and Stack Frame
- Intel SDM: Vol.3A §2.4.3 (Figure 2-6) — IDTR Register
- OSDev Wiki: [Interrupt Descriptor Table](https://wiki.osdev.org/Interrupt_Descriptor_Table)
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)
- xv6: [trap.c / mmu.h](https://github.com/mit-pdos/xv6-public/blob/master/trap.c) — 32-bit IDT and GDT setup
