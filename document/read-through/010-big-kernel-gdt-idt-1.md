---
title: 010-big-kernel-gdt-idt-1 · GDT/IDT 重构
---

# 010-1 Read-through: 大内核 GDT 初始化——段描述符工厂与 lgdt

## 概览

本文讲解 tag 010 中大内核 GDT（全局描述符表）的完整实现。GDT 是 IDT 和中断处理的前置依赖——IDT 里的每个条目都包含一个代码段选择子，指向 GDT 中的 code segment 描述符，如果 GDT 没配好，中断触发后 CPU 就找不到合法的代码段，直接 triple fault。

本节的代码涉及两个文件：`kernel/arch/x86_64/gdt.hpp`（142 行，头文件，包含类型定义和工厂函数）和 `kernel/arch/x86_64/gdt.cpp`（72 行，实现，包含初始化和硬件加载）。整个设计用 class 封装 GDT 状态，用 scoped enum 提供类型安全的描述符构建，用 constexpr 工厂函数在编译期生成描述符。

关键设计决策一览：
- GDT 状态封装为 class（vs mini kernel 的全局函数+全局数组）
- scoped enum 替代裸 uint8_t（类型安全的 access/flags 组合）
- constexpr 工厂函数（编译期生成描述符，零运行时开销）
- TSS 占位符（全零，为将来的用户态和 IST 做准备）

---

## 架构图

```
kernel_main()
    │
    ├── g_gdt.init()
    │       │
    │       ├── entries_[0] = null_entry()
    │       ├── entries_[1] = segment_entry(kernel code64)
    │       ├── entries_[2] = segment_entry(kernel data64)
    │       ├── entries_[3] = segment_entry(user code64)
    │       ├── entries_[4] = segment_entry(user data64)
    │       ├── entries_[5] = tss_low_entry(&tss_)
    │       ├── entries_[6] = tss_high_entry(&tss_)
    │       ├── gdtr_.limit = sizeof(entries_) - 1
    │       ├── gdtr_.base  = &entries_
    │       │
    │       └── load()
    │             │
    │             ├── lgdt [gdtr_]         ← 加载 GDTR
    │             ├── push CS; push RIP; lretq  ← 刷新 CS
    │             ├── mov DS/ES/FS/GS/SS   ← 刷新数据段寄存器
    │             └── ltr TSS_SEL          ← 加载任务寄存器
    │
    └── g_idt.init()   ← 下一节讲解
```

---

## 代码精讲

### gdt.hpp: 段选择子常量

```cpp
namespace cinux::arch {

constexpr uint16_t GDT_KERNEL_CODE = 0x08;
constexpr uint16_t GDT_KERNEL_DATA = 0x10;
constexpr uint16_t GDT_USER_CODE   = 0x1B;
constexpr uint16_t GDT_USER_DATA   = 0x23;
constexpr uint16_t GDT_TSS         = 0x28;
```

五个段选择子常量定义在命名空间 `cinux::arch` 中。选择子的计算方式是 `索引 * 8 + RPL`（Requested Privilege Level），其中 RPL 在内核态为 0，在用户态为 3。`GDT_KERNEL_CODE = 0x08` 意味着索引 1（第二项，因为第一项是 null）乘以 8，不加 RPL；`GDT_USER_CODE = 0x1B` 是索引 3 乘以 8 再加 3，即 24+3=27=0x1B。这些常量在整个内核中会被广泛使用——IDT 配置时需要指定代码段选择子，上下文切换时需要加载用户态段寄存器，系统调用入口需要验证 CS 值，所以定义在一个统一的头文件里是最合适的。

### gdt.hpp: SegmentAccess 和 SegmentFlags 枚举

```cpp
enum class SegmentAccess : uint8_t {
    Present    = 1u << 7,
    Ring0      = 0u << 5,
    Ring3      = 3u << 5,
    CodeData   = 1u << 4,
    Executable = 1u << 3,
    ReadWrite  = 1u << 1,
    TSS64Avail = 0x09,
};

constexpr SegmentAccess operator|(SegmentAccess a, SegmentAccess b) {
    return static_cast<SegmentAccess>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

enum class SegmentFlags : uint8_t {
    Granularity4K = 1u << 3,
    LongMode      = 1u << 1,
    Size32        = 1u << 2,
};

constexpr SegmentFlags operator|(SegmentFlags a, SegmentFlags b) {
    return static_cast<SegmentFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
```

`SegmentAccess` 的每个成员对应 access byte 中的一个或几个 bit，按照 Intel SDM Vol.3A §3.4.5 的定义：`Present`（bit 7）表示描述符有效，`CodeData`（bit 4）区分代码/数据段和系统段，`Executable`（bit 3）表示段可执行，`ReadWrite`（bit 1）表示段可读写。`Ring0` 和 `Ring3` 分别是 DPL=0 和 DPL=3。`TSS64Avail = 0x09` 是一个完整的 access byte 值（bit 3 置 1 表示 64 位 TSS，bit 0 置 1 表示 Available），不是按 bit 组合的——它直接作为 TSS 描述符的 access byte 使用。

重载的 `operator|` 允许你像写 `SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData` 这样组合标志。虽然 scoped enum 不能隐式转换为整数，但通过 `static_cast` 到底层类型再按位或，最后 `static_cast` 回枚举类型，就实现了类型安全的位运算——你不可能意外地用 `SegmentFlags` 的值去组合 `SegmentAccess`，因为它们的类型不同。

`SegmentFlags` 对应描述符第 6 字节的高 4 位（也叫 flags nibble）：`Granularity4K`（bit 3）表示 limit 的粒度是 4KB，`LongMode`（bit 1）表示这是一个 64 位代码段（L 位），`Size32`（bit 2）是 D/B 位，表示默认操作数是 32 位。这里有一个经常让人迷惑的地方——内核数据段的 flags 用的是 `Size32` 而不是 `LongMode`。原因是 64 位模式下只有代码段才需要设置 L 位，数据段的 L 位被 Intel 保留（必须为 0），数据段只需要设置 D/B 位为 1 即可。如果你把数据段的 L 位也置 1，CPU 行为是未定义的。

### gdt.hpp: GDT class 和内部结构体

```cpp
class GDT {
public:
    void init();

private:
    struct [[gnu::packed]] Entry {
        uint16_t limit_low;
        uint16_t base_low;
        uint8_t  base_middle;
        uint8_t  access;
        uint8_t  flags_limit_high;
        uint8_t  base_high;
    };
    static_assert(sizeof(Entry) == 8, "GDT entry must be 8 bytes");

    struct [[gnu::packed]] Pointer {
        uint16_t limit;
        uint64_t base;
    };

    struct [[gnu::packed]] TaskStateSegment {
        uint32_t reserved0;
        uint64_t rsp[3];
        uint64_t reserved1;
        uint64_t ist[7];
        uint64_t reserved2;
        uint16_t reserved3;
        uint16_t iomap_base;
    };
    static_assert(sizeof(TaskStateSegment) == 104, "TSS must be 104 bytes");
```

`Entry` 结构体严格按照 Intel SDM Vol.3A §3.4.5 Figure 3-8 的布局定义——6 个字段共 8 字节，没有位域，没有抽象。`[[gnu::packed]]` 告诉编译器不要在字段之间插入 padding，`static_assert(sizeof(Entry) == 8)` 在编译期验证。这两个防护措施非常必要——如果哪天你把 `access` 从 `uint8_t` 改成了 `uint32_t`，`static_assert` 会立刻报错，而不是等到运行时 triple fault 才发现。

`Pointer` 结构体 10 字节（2 字节 limit + 8 字节 base），对应 `lgdt` 指令从内存读取的 GDTR 格式。注意 64 位模式下 GDTR 的 base 是 8 字节（vs 32 位模式的 4 字节），limit 始终是 2 字节。

`TaskStateSegment` 严格按照 Intel SDM Vol.3A §7.7 Table 7-8 的 64 位 TSS 布局定义，总共 104 字节。字段包括 reserved0（4 字节）、RSP 数组（3 个 64 位值，对应 Ring 0/1/2 的栈指针）、reserved1（8 字节）、IST 数组（7 个 64 位值，对应 7 个 Interrupt Stack Table）、reserved2（8 字节）、reserved3（2 字节）和 iomap_base（2 字节）。`static_assert(sizeof(TaskStateSegment) == 104)` 保证编译期大小正确。

### gdt.hpp: Constexpr 工厂函数

```cpp
    static constexpr Entry null_entry() { return {0, 0, 0, 0, 0, 0}; }

    static constexpr Entry segment_entry(SegmentAccess access, SegmentFlags flags) {
        return {
            .limit_low        = 0xFFFF,
            .base_low         = 0,
            .base_middle      = 0,
            .access           = static_cast<uint8_t>(access),
            .flags_limit_high = static_cast<uint8_t>((static_cast<uint8_t>(flags) << 4) | 0x0F),
            .base_high        = 0,
        };
    }

    static constexpr Entry tss_low_entry(uint64_t base, uint32_t limit) {
        auto b = static_cast<uint32_t>(base & 0xFFFFFFFF);
        return {
            .limit_low        = static_cast<uint16_t>(limit & 0xFFFF),
            .base_low         = static_cast<uint16_t>(b & 0xFFFF),
            .base_middle      = static_cast<uint8_t>((b >> 16) & 0xFF),
            .access           = static_cast<uint8_t>(SegmentAccess::Present | SegmentAccess::TSS64Avail),
            .flags_limit_high = static_cast<uint8_t>((limit >> 16) & 0x0F),
            .base_high        = static_cast<uint8_t>((b >> 24) & 0xFF),
        };
    }

    static constexpr Entry tss_high_entry(uint64_t base) {
        auto hi = static_cast<uint32_t>(base >> 32);
        return {
            .limit_low        = static_cast<uint16_t>(hi & 0xFFFF),
            .base_low         = static_cast<uint16_t>((hi >> 16) & 0xFFFF),
            .base_middle      = 0,
            .access           = 0,
            .flags_limit_high = 0,
            .base_high        = 0,
        };
    }
```

`null_entry()` 返回全零描述符——Intel 要求 GDT 的第一项必须是 null descriptor。

`segment_entry()` 是最核心的工厂函数。它把传入的 `access` 和 `flags` 组合成一个 8 字节描述符。`flags_limit_high` 字段的构造方式是 `(flags << 4) | 0x0F`——高 4 位放 flags（左移 4 位），低 4 位放 limit 的高 4 位（固定 0x0F）。配合 `limit_low = 0xFFFF`，总 limit 为 `0xFFFFF`，再配合 `Granularity4K`，覆盖完整的 4GB 地址空间。Base 全部填 0——在 long mode 下硬件忽略 base 和 limit，但填 0 是最安全的做法。

`tss_low_entry()` 生成 TSS 描述符的前 8 字节。TSS 是系统段描述符，access byte 固定为 `Present | TSS64Avail`（即 0x89）。它把 64 位 TSS 地址的低 32 位拆分到 base_low/base_middle/base_high 三个字段，limit 设为 `sizeof(TaskStateSegment) - 1`（即 103）。

`tss_high_entry()` 生成 TSS 描述符的后 8 字节——存放 TSS 地址的高 32 位。这个 8 字节的其他字段全零，因为 64 位 TSS 描述符的第二个 8 字节只存放 base[63:32]，其余保留。这个设计是 x86_64 特有的——32 位模式下 TSS 描述符只有 8 字节，因为地址只有 32 位。

### gdt.hpp: GDT class 成员与全局实例

```cpp
    static constexpr auto kEntryCount = 7;
    Entry entries_[kEntryCount]{};
    Pointer gdtr_{};
    TaskStateSegment tss_{};

    void load();
};

extern GDT g_gdt;

}  // namespace cinux::arch
```

`kEntryCount = 7` 对应 5 个段描述符 + TSS 占 2 个 slot = 7。`entries_`、`gdtr_`、`tss_` 都使用了 `{}` 初始化（zero-initialized），确保在 `init()` 被调用之前所有字段都是 0。`g_gdt` 是全局 GDT 实例，放在 BSS 段（zero-initialized），整个内核共享这一个实例——将来如果要做 SMP，需要改成 per-CPU 实例。

### gdt.cpp: GDT::init()——填充描述符

```cpp
namespace cinux::arch {

GDT g_gdt;

void GDT::init() {
    entries_[0] = null_entry();

    entries_[1] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[2] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring0 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);

    entries_[3] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::LongMode);

    entries_[4] = segment_entry(
        SegmentAccess::Present | SegmentAccess::Ring3 |
        SegmentAccess::CodeData | SegmentAccess::ReadWrite,
        SegmentFlags::Granularity4K | SegmentFlags::Size32);
```

这段代码的可读性比直接写十六进制数值好太多了——`SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite` 一眼就能看出"这是一个 Present 的、Ring 0 的、代码/数据段的、可执行的、可读的描述符"，而 `0x9A` 就没那么直观了。

对比 kernel code64 和 kernel data64 的 flags：code64 用 `LongMode`（L=1, D=0），data64 用 `Size32`（L=0, D/B=1）。这是 x86_64 的规范——只有代码段才设 L 位，数据段不设。对比 kernel 和 user 版本：唯一的区别是 `Ring0` vs `Ring3`，这意味着 user 版本的 DPL=3，允许 Ring 3 的代码使用这些段。

```cpp
    const auto tss_addr = reinterpret_cast<uint64_t>(&tss_);
    entries_[5] = tss_low_entry(tss_addr, sizeof(TaskStateSegment) - 1);
    entries_[6] = tss_high_entry(tss_addr);

    gdtr_.limit = sizeof(entries_) - 1;
    gdtr_.base  = reinterpret_cast<uint64_t>(entries_);

    load();
}
```

TSS 的地址通过 `reinterpret_cast<uint64_t>(&tss_)` 获取，然后分别传给 `tss_low_entry` 和 `tss_high_entry` 生成两个 8 字节描述符。GDTR 的 limit 是整个 entries_ 数组的字节数减 1（7 * 8 = 56 字节，limit = 55），base 指向 entries_ 数组的首地址。然后调用 `load()` 把这些设置加载到硬件。

### gdt.cpp: GDT::load()——lgdt + 段寄存器刷新 + ltr

```cpp
void GDT::load() {
    __asm__ volatile(
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
        : [gdtr] "m"(gdtr_), [cs] "i"(GDT_KERNEL_CODE), [ds] "i"(GDT_KERNEL_DATA)
        : "rax", "memory");
```

这段内联汇编做了三件事，我们来逐行拆解。

第一件：`lgdt %[gdtr]` 从内存加载 GDTR。操作数用 `"m"` 约束，告诉编译器 `gdtr_` 是一个内存操作数——`lgdt` 需要从内存读取 10 字节（2 字节 limit + 8 字节 base），不能从寄存器读取。

第二件：`pushq %[cs]` + `leaq 1f(%%rip), %%rax` + `pushq %%rax` + `lretq`。这是一个"伪造的 far return"——在栈上先压入新的 CS 值（0x08），再压入返回地址（`1f` 标签的位置），然后执行 `lretq`。`lretq` 从栈上弹出 RIP（恢复到 `1:` 标签处）和 CS（加载为 0x08），完成 CS 的刷新。这一步不能用 `mov` 指令替代——CS 只能通过 far jump、far call、far return 或 IRET 来加载。`leaq 1f(%%rip)` 用的是 RIP-relative 寻址，这在 long mode 下是标准的地址计算方式。

第三件：`movw %[ds], %%ax` + 五个 `movw %%ax, %%段寄存器`。把 DS/ES/FS/GS/SS 全部设为 `GDT_KERNEL_DATA`（0x10）。先加载到 AX 再从 AX 复制到各段寄存器，是因为不能把立即数直接 `mov` 到段寄存器——必须经过通用寄存器中转。

```cpp
    const uint16_t tss_sel = GDT_TSS;
    __asm__ volatile("ltr %[sel]\n\t" : : [sel] "r"(tss_sel) : "memory");
}

}  // namespace cinux::arch
```

`ltr` 加载任务寄存器，把 TSS 选择子（0x28）加载到 TR。`ltr` 会自动把 GDT 中对应位置的 TSS 描述符标记为 "busy"（把 access byte 的 type 字段从 0x09 Available 改为 0x0B Busy），这是硬件自动完成的。之所以用 `"r"` 约束而不是 `"i"`，是因为 `ltr` 不接受立即数操作数——必须先把选择子放到一个通用寄存器里。

---

## 设计决策

### 决策：class 封装 vs 全局函数+全局数组

**问题**: GDT 状态如何组织——用 class 封装还是用全局函数+全局数组？

**本项目的做法**: `GDT` class 封装所有状态（entries_、gdtr_、tss_），对外只暴露 `init()`。全局实例 `g_gdt`。

**备选方案**: 像 mini kernel 那样，`GDTEntry gdt[3]` 是全局数组，`gdt_init()` 是全局函数，`gdtr` 是全局结构体。

**为什么不选备选方案**: 全局变量没有访问控制——任何代码都能直接修改 `gdt` 数组的内容。class 封装把 entries_ 设为 private，只有 init() 和 load() 能操作它们。更重要的是，如果将来要做 per-CPU GDT（像 Linux 那样），只需要把 `GDT g_gdt` 变成 `PER_CPU GDT g_gdt`，而全局数组方案需要把所有全局变量都改成 per-CPU 变量，改动量大多了。

**如果要扩展/改进**: 将来做 SMP 时，把 `g_gdt` 变成 per-CPU 变量（通过 Linux 的 `DEFINE_PER_CPU` 或类似机制），每个 CPU 在启动时调用自己的 `g_gdt.init()`。

### 决策：scoped enum vs 裸 uint8_t

**问题**: 描述符的 access byte 和 flags byte 如何表达？

**本项目的做法**: `enum class SegmentAccess : uint8_t` 和 `enum class SegmentFlags : uint8_t`，配合 `operator|` 重载。

**备选方案**: 直接用 `uint8_t` 常量，比如 `constexpr uint8_t GDT_ACCESS_CODE64 = 0x9A`。

**为什么不选备选方案**: `0x9A` 是什么意思？你得翻 Intel SDM 才知道。而 `SegmentAccess::Present | SegmentAccess::Ring0 | SegmentAccess::CodeData | SegmentAccess::Executable | SegmentAccess::ReadWrite` 是自文档化的。另外，scoped enum 提供类型安全——你不能意外地把 `SegmentFlags` 的值传给期望 `SegmentAccess` 的参数。

**如果要扩展/改进**: 可以增加编译期 `static_assert` 验证工厂函数的输出值（比如 `static_assert(segment_entry(kernel_code_access, kernel_code_flags).access == 0x9A)`）。

---

## 扩展方向

1. **(1 星)** 验证工厂函数输出值——用 `static_assert` 在编译期检查 `segment_entry()` 生成的各字段值是否与预期完全一致。

2. **(2 星)** 填充 TSS 的 RSP0 字段——分配一个独立的内核异常栈，设置到 TSS.RSP0，将来进入 Ring 3 后中断处理时 CPU 会自动切换到这个栈。

3. **(2 星)** 使用 IST（Interrupt Stack Table）——为 Double Fault (#DF) 分配一个专用栈，设到 TSS.IST1，然后在 IDT 的 #DF 条目中设置 IST=1。

4. **(3 星)** Per-CPU GDT——参考 Linux 的 `DEFINE_PER_CPU_PAGE_ALIGNED` 机制，为每个 CPU 分配独立的 GDT 页，AP 启动时初始化自己的 GDT。

5. **(2 星)** GDT 只读保护——把 GDT 映射到只读页面，需要修改时临时映射为可写。参考 Linux 的 fixmap GDT 方案。

---

## 参考资料

- Intel SDM: Vol.3A §3.4.5 — Segment Descriptors (Figure 3-8, p.3-16~3-18)
- Intel SDM: Vol.3A §2.4.1 — GDTR/LGDT (Figure 2-6)
- Intel SDM: Vol.3A §7.7 — Task State Segment in 64-bit Mode (Table 7-8)
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)
- OSDev Wiki: [Task State Segment](https://wiki.osdev.org/Task_State_Segment)
- Linux: [arch/x86/include/asm/desc.h](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/desc.h) — per-CPU GDT, TSS descriptor setup
- Linux: [arch/x86/include/asm/processor.h](https://github.com/torvalds/linux/blob/master/arch/x86/include/asm/processor.h) — `x86_hw_tss` struct definition
- xv6: [bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S) — flat GDT + protected mode switch
