---
title: 007-mini-kernel-intr-1 · 中断处理
---

# 007-1 Read-through: GDT 实现

## 本章概览

这一篇我们通读 Cinux mini kernel 的 GDT 实现代码——gdt.hpp 和 gdt.cpp 这两个文件。在整个 tag 007 的架构中，GDT 构成了中断处理链路的第一环：它提供段寄存器配置，告诉 CPU CS/DS/SS 这些段寄存器应该指向哪里。后续的 IDT（下一篇覆盖）和 ISR Stub（第三篇覆盖）都依赖于 GDT 先正确初始化。

关键设计决策有三个。第一，GDT 只设三项（null/code64/data64），足够满足 long mode 内核的需求——分段机制在 64 位下已经被大幅弱化，base 和 limit 字段被硬件忽略，我们只需要让 CS 指向一个合法的 64 位代码段描述符就行。第二，所有数据结构都用 `__attribute__((packed))` 确保内存布局与硬件期望完全一致，这在内核开发中是不可跳过的步骤。第三，使用 lretq 而不是 ljmp 来刷新 CS，因为在 higher-half kernel 中 lretq 可以使用 RIP 相对地址。

和 xv6 对比的话，xv6 在进入内核的第一时间就配置了完整的 GDT，包含 TSS 段和用户态段。我们这里采取了更渐进的方式——先跑通最小集合，等后续 milestone 需要硬件中断和用户态切换的时候再逐步填充。

---

## 架构图

```
GDT 初始化链路：

mini_kernel_main()
       |
       +-- gdt_init()
             |
             +-- make_gdt_entry(null):    全零
             +-- make_gdt_entry(code64):  0x9A/0x0A, L=1
             +-- make_gdt_entry(data64):  0x92/0x0C
             +-- 构造 GdtPointer
             +-- LGDT + lretq (刷新CS) + mov DS/ES/FS/GS/SS


CPU 查找链路（中断触发时）：

CPU 异常 -> 查 IDT[向量号] -> 得到 selector(0x08) + handler 地址
                                    |
                             查 GDT[selector>>3] -> 得到 code64 段属性
                                    |
                             压栈 SS/RSP/RFLAGS/CS/RIP -> 跳转 handler
```

---

## 代码精讲

### GDT 头文件 — gdt.hpp

我们先来看 GDT 的头文件。整个文件包裹在 `cinux::mini::arch` 命名空间中，这是 Cinux 内核代码组织的惯例——按子系统（arch/mm/driver/lib）分命名空间，避免符号冲突。

首先是常量定义部分。`GDT_ENTRIES = 3` 表示我们的 GDT 只有三个条目，这是 long mode 下最小的可用配置——null descriptor 占第一个位置（x86 硬性要求），code64 给内核代码段用，data64 给内核数据段和栈段用。

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

三个段选择子常量的值分别是 0、8、16（即索引乘以 8），这是因为段选择子的格式是 `[Index(13位) : TI(1位) : RPL(2位)]`，TI=0 表示 GDT，RPL=0 表示 ring 0，所以低 3 位全零，选择子的值就等于索引乘以 8。这些常量在 IDT 初始化时也会用到——IDT 条目中的 selector 字段直接填 `SEGMENT_CODE64`（0x08），这就是为什么 GDT 必须先于 IDT 初始化的底层原因。

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

`gdt_init()` 的主体非常直白——三个 `make_gdt_entry` 调用填写三项描述符，然后构造 GdtPointer 并用内联汇编加载。

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

---

## 别人怎么做的

### xv6 的 GDT

xv6 是 32 位系统，GDT 有 6 项（null + KCODE + KDATA + UCODE + UDATA + TSS）。和我们相比多了三个：UCODE/UDATA 用于用户态（ring 3），TSS 用于特权级切换时的栈保存。xv6 在 `seginit()` 中初始化 GDT，使用和 Cinux 类似的 makeSegdesc 辅助函数。

32 位和 64 位的 GDT 有一个重要差异：64 位模式下 TSS 描述符占 16 字节（两个 GDT slot），而 32 位模式下只占 8 字节。所以 Cinux 的 GDT 在添加 TSS 后会变成 5 项（null + code64 + data64 + TSS_low + TSS_high），而不是 xv6 那样的 6 项。

### SerenityOS 的 GDT

SerenityOS 在每个 CPU 核心上维护独立的 GDT 和 GDTR。AP（应用处理器）启动时从 BSP（引导处理器）复制 GDT。SerenityOS 的 GDT 包含了完整的 TSS、用户态段和 GS/FS base 的 MSR 设置（用于 per-CPU 数据和线程本地存储）。

---

## 设计决策

### 决策：GDT 只设三项

**问题**: 需要决定 GDT 配置多少条目——是像 xv6 那样一开始就配完整（含 TSS 和用户态段），还是只配最小集合。

**本项目的做法**: 只配 3 项（null/code64/data64），TSS 和用户态段留给后续 tag 010（big kernel GDT/IDT）和 tag 022（ring3 用户态）。

**备选方案**: 在 tag 007 就配 6 项（加上 TSS、UCODE、UDATA）。

**为什么不选备选方案**: 一是每个 milestone 的目标应该尽可能聚焦——tag 007 的目标是"触发异常不死机"，不是"完整的段管理"；二是 TSS 需要额外定义 128 位的系统段描述符和实际的 TSS 结构体，代码量会翻倍；三是用户态段在当前阶段完全没有用途，配了也只是占位。

**如果要扩展**: tag 010 会添加 TSS（用于 ring 3 到 ring 0 的栈切换），tag 022 会添加用户态 code/data 段。届时 GDT 会从 3 项扩展到 6 项以上。

### 决策：用 lretq 而不是 ljmp 刷新 CS

**问题**: 加载 GDT 后需要刷新 CS，选择 lretq 还是 ljmp。

**本项目的做法**: 使用 lretq（far return），通过 `leaq 1f(%%rip)` 获取返回地址。

**备选方案**: 使用 ljmp（far jump）到绝对地址。

**为什么不选备选方案**: 在 higher-half kernel 中，ljmp 需要一个绝对地址。而我们的内核运行在 0xFFFFFFFF80000000 起始的虚拟地址空间，这个地址在编译时可能不确定（取决于链接器的布局）。lretq 通过 `leaq 1f(%%rip)` 使用 RIP 相对寻址，对位置无关代码更友好，也不需要知道具体的链接地址。

**注意事项**: 两种方式在功能上完全等价——都是触发 CPU 重新加载 CS 的描述符缓存。选择哪种纯粹是代码可维护性的考量。

---

## 扩展方向

- 为 GDT 添加 TSS 段描述符和实际的 TSS 结构体（难度：中等）——tag 010 的内容
- 为 IDT 添加动态注册处理函数的接口，避免每次新增异常都要修改 idt.cpp（难度：中等）
- 使用 IST 机制为 #DF 和 #MC 配置独立的栈，提高关键异常的可靠性（难度：较高）

---

## 文件清单

| 文件 | 职责 | 行数（大约） |
|------|------|-------------|
| `kernel/mini/arch/x86_64/gdt.hpp` | GDT 常量、结构体、接口声明 | ~85 行 |
| `kernel/mini/arch/x86_64/gdt.cpp` | make_gdt_entry + gdt_init 实现 | ~111 行 |

这两个文件总共约 200 行代码，是 tag 007 新增代码中相对较小的部分（但也是最基础的部分——没有正确的 GDT，后续的 IDT 和中断处理都无法工作）。

---

## 参考资料

- Intel SDM: Vol.3A 3.4.4 (Segment Descriptors) -- Access Byte 和 Flags 编码
- Intel SDM: Vol.3A 3.5.2 (Segment Selection) -- 段选择子格式
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)
- xv6: [mmu.h](https://github.com/mit-pdos/xv6-public/blob/master/mmu.h) -- 32-bit GDT and segment definitions
