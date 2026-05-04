# 016-1 通读：页表项结构与分页配置——x86-64 四级页表的"砖块"与"图纸"

## 概览

本文是 tag `016_mm_vmm` 三篇通读教程的第一篇，聚焦于虚拟内存管理器的底层基础设施：`PageEntry` 联合体、分页配置常量（`paging_config.hpp`）、TLB 刷新辅助函数以及 CR3 寄存器访问器。在整个 VMM 的架构中，它们是最基本的砖块——`PageEntry` 是软件与硬件 MMU 之间的"契约格式"，每一级页表的每一个入口都是一个 64 位的 `PageEntry`；`paging_config.hpp` 则是把散落在各处的魔数集中起来的"图纸"，定义了页大小、移位常量、标志位和索引提取宏。TLB 刷新和 CR3 读写则是我们操控硬件地址翻译缓存的手柄——页表改了，TLB 不知道，必须手动通知硬件。

我们从 `paging_config.hpp` 的常量定义开始，因为它是整个分页子系统的公共依赖；然后深入 `paging.hpp` 中的 `PageEntry` 联合体，看看 C++ 位域如何一对一映射到 Intel SDM 规定的 PTE 格式；最后讲解 TLB 刷新函数和 CR3 管理的内联函数，以及它们在 `paging.cpp` 的 MMIO 映射中如何替代原来硬编码的行内汇编。

## 架构图

```
    64-bit 虚拟地址 (canonical form)
    ┌────────┬──────┬──────┬──────┬──────┬──────────────┐
    │63   48 │47  39│38  30│29  21│20  12│11           0│
    │ sign   │PML4  │PDPT  │ PD   │ PT   │ page offset  │
    │ extend │index │index │index │index │              │
    └────────┴──────┴──────┴──────┴──────┴──────────────┘
         │       │      │      │      │          │
         │       ▼      ▼      ▼      ▼          │
         │    ┌─────┐ ┌─────┐ ┌────┐ ┌────┐      │
         │    │PML4 │→│PDPT │→│ PD │→│ PT │──────┘
         │    └─────┘ └─────┘ └────┘ └────┘
         │      ▲
         │      │ CR3 指向 PML4 物理地址
         │      │
    ┌────┴────────────────────────────────────────────┐
    │                 PageEntry (64-bit)              │
    │  ┌─┬─┬─┬─┬─┬─┬─┬─┬─┬───┬──────────┬───┬─┐    │
    │  │P│W│U│Pw│Pc│A│D│H│G│av │ addr:40  │av2│N│    │
    │  └─┴─┴─┴─┴─┴─┴─┴─┴─┴───┴──────────┴───┴─┘    │
    │   0 1 2  3  4 5 6 7 8 9-11  12-(M-1) 52-62 63  │
    └─────────────────────────────────────────────────┘

    paging_config.hpp 供给：
    ├── PAGE_SIZE = 4096, PT_ENTRIES = 512
    ├── 移位常量: PT_SHIFT=12, PD_SHIFT=21, PDPT_SHIFT=30, PML4_SHIFT=39
    ├── ADDR_MASK = 0x000FFFFFFFFFF000
    ├── FLAG_* 标志位定义
    └── PML4_INDEX / PDPT_INDEX / PD_INDEX / PT_INDEX 宏

    paging.hpp 提供：
    ├── PageEntry 联合体 (raw + 位域 + helper 方法)
    ├── flush_tlb(virt)   → INVLPG 单页失效
    ├── flush_tlb_all()    → CR3 重载全量失效
    ├── read_cr3() / write_cr3()
    └── map_mmio()  遗留 MMIO 映射
```

## 代码精讲

### paging_config.hpp —— 分页子系统的"字典"

在 tag 015 及之前，分页相关的常量散落在 `paging.cpp` 和其他文件里——`0x83` 这种魔数满天飞，`PT_ENTRIES = 512` 藏在 cpp 文件的匿名命名空间里，每次新增一个使用分页的模块都要重新定义一遍。这个 tag 我们做的第一件事就是把所有分页常量集中到一个头文件中。

```cpp
/**
 * @file kernel/arch/x86_64/paging_config.hpp
 * @brief x86_64 paging configuration constants
 *
 * Page size, shift amounts, index extraction macros, and address masks
 * used throughout the paging subsystem.
 *
 * Namespace: cinux::arch
 */

#pragma once

#include <stdint.h>

namespace cinux::arch {

constexpr uint64_t PAGE_SIZE  = 4096;
constexpr uint32_t PAGE_SHIFT = 12;
constexpr uint32_t PT_ENTRIES = 512;
```

开头的三个常量是整个分页系统的基本参数。x86-64 的标准页大小是 4096 字节（4KB），也就是 2 的 12 次方，所以 `PAGE_SHIFT = 12`。每一级页表包含 512 个条目（因为每级索引占 9 个 bit，2 的 9 次方 = 512），这也解释了为什么每个页表恰好占一整页：512 × 8 字节 = 4096 字节。这三个数字构成了四级页表遍历的数学基础，后续所有的移位、掩码、索引计算都从它们推导出来。

接下来是每一级页表索引的移位量：

```cpp
// Bits to shift for each paging level index
constexpr uint32_t PT_SHIFT   = 12;
constexpr uint32_t PD_SHIFT   = 21;
constexpr uint32_t PDPT_SHIFT = 30;
constexpr uint32_t PML4_SHIFT = 39;
```

这四个移位常量直接对应 x86-64 虚拟地址的位域划分。PT_SHIFT = 12 意味着 PT 索引从虚拟地址的第 12 位开始提取（bit 20:12）；PD_SHIFT = 21 说明 PD 索引在 bit 29:21；PDPT_SHIFT = 30 对应 bit 38:30；PML4_SHIFT = 39 对应 bit 47:39。每一级比上一级多 9 个 bit（因为 512 = 2^9），所以移位量每次增加 9。这些数字不是随意定的，它们是 Intel SDM Vol.3A Section 4.5.4 明确规定的硬件分页结构。

然后是地址掩码和标志位掩码：

```cpp
// Address and flag masks for page table entries
constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t FLAG_MASK = 0xFFF0000000000FFFULL;
```

`ADDR_MASK` 提取 PTE 中的物理地址字段——bit 51:12，共 40 位。为什么不是 bit 63:12 呢？因为 bit 63 是 NX（No eXecute）位，bit 62:52 是可用位/保留位，只有 bit 51:12 才是真正的物理页帧号。在支持 5 级分页（LA57）的 CPU 上，物理地址可以扩展到 bit 55:12，但 Cinux 目前只支持 4 级分页，所以 ADDR_MASK 的定义是正确的。`FLAG_MASK` 则是 ADDR_MASK 的补集——提取所有非地址的位（低 12 位标志 + 高位标志），主要用于 map 时分离物理地址和标志。

标志位定义采用了 `1ULL << N` 的写法，每一位都有明确的名字和含义：

```cpp
// Page table flag bits
constexpr uint64_t FLAG_PRESENT  = 1ULL << 0;
constexpr uint64_t FLAG_WRITABLE = 1ULL << 1;
constexpr uint64_t FLAG_USER     = 1ULL << 2;
constexpr uint64_t FLAG_PWT      = 1ULL << 3;
constexpr uint64_t FLAG_PCD      = 1ULL << 4;
constexpr uint64_t FLAG_ACCESSED = 1ULL << 5;
constexpr uint64_t FLAG_DIRTY    = 1ULL << 6;
constexpr uint64_t FLAG_HUGE     = 1ULL << 7;
constexpr uint64_t FLAG_GLOBAL   = 1ULL << 8;
constexpr uint64_t FLAG_COW      = 1ULL << 9;  // Available bit 9: Copy-On-Write marker
constexpr uint64_t FLAG_NX       = 1ULL << 63;
```

你会发现这组标志位和 Intel SDM Vol.3A Table 4-20 定义的 PTE 格式一一对应。bit 0 的 Present 位决定这一页是否在物理内存中——当 P=0 时，MMU 不会做地址翻译，而是直接触发 #PF 异常，这正是后面按需分页（demand paging）的基础。bit 1 的 Writable 控制写权限，bit 2 的 User 控制用户态（Ring 3）访问权限。bit 3-4 的 PWT（Page-level Write-Through）和 PCD（Page-level Cache Disable）控制这一页的缓存策略，在设备 MMIO 映射时特别有用——如果你映射的是帧缓冲区这样的写合并（write-combining）区域，就需要配合 MTRR/PAT 来设置正确的缓存属性。bit 5-6 的 Accessed 和 Dirty 是由 CPU 硬件自动设置的"粘滞位"——CPU 每次通过某个 PTE 做地址翻译就置 Accessed，每次写入就置 Dirty，而且 CPU 永远不会清零这两位，这为未来的页面置换算法提供了最基本的热度信息。

bit 7 的 Huge 在不同层级有不同含义：在 PDPT 层置位表示 1GB 大页，在 PD 层置位表示 2MB 大页，在 PT 层则变成 PAT（Page Attribute Table）位。Cinux 统一用 `huge` 这个名字，因为它在非叶节点上的语义确实就是"这是一个大页而非指向下一级页表的指针"。bit 8 的 Global 标志防止该 PTE 在 CR3 重载时被刷出 TLB——内核代码页通常会设这个位以减少 TLB miss。bit 9 是 Available 位之一，Cinux 将它征用为 `FLAG_COW`（Copy-On-Write 标记），为未来的 fork() 实现做准备。bit 63 的 NX（No eXecute）禁止在这个页上执行代码，是 W^X 安全策略的硬件基础。

最后是四个索引提取函数，它们是页表遍历的"导航工具"：

```cpp
// Index extraction for each paging level
constexpr uint64_t PML4_INDEX(uint64_t virt) {
    return (virt >> PML4_SHIFT) & 0x1FF;
}
constexpr uint64_t PDPT_INDEX(uint64_t virt) {
    return (virt >> PDPT_SHIFT) & 0x1FF;
}
constexpr uint64_t PD_INDEX(uint64_t virt) {
    return (virt >> PD_SHIFT) & 0x1FF;
}
constexpr uint64_t PT_INDEX(uint64_t virt) {
    return (virt >> PT_SHIFT) & 0x1FF;
}

// True if the virtual address falls in the canonical lower half (user space).
// x86_48 user space: bit 47 = 0, i.e. 0x0000000000000000 .. 0x00007FFFFFFFFFFF.
constexpr bool is_user_vaddr(uint64_t virt) {
    return !(virt & (1ULL << 47));
}
```

每个函数做的事情完全一样：右移对应位数，然后和 `0x1FF`（9 个 1）做与运算，提取出 9 位的索引值。`constexpr` 修饰意味着编译器可以在编译期计算出常量虚拟地址的索引——对于内核的静态映射来说，这省掉了一堆运行时计算。`is_user_vaddr` 是一个便利函数，通过检查虚拟地址的第 47 位来判断地址属于用户空间还是内核空间。在 x86-64 的 48 位虚拟地址空间中，bit 47 = 0 表示下半部分（用户空间 0 到 128TB），bit 47 = 1 表示上半部分（内核空间），这就是所谓的"规范地址"（canonical address）约束。

### PageEntry 联合体——软件眼中的 PTE

有了常量定义，我们来看 `PageEntry`——它是直接映射到硬件 PTE 格式的 C++ 数据结构。

```cpp
// ============================================================
// PageEntry union
// ============================================================

union PageEntry {
    uint64_t raw;

    struct {
        uint64_t present  : 1;
        uint64_t writable : 1;
        uint64_t user     : 1;
        uint64_t pwt      : 1;
        uint64_t pcd      : 1;
        uint64_t accessed : 1;
        uint64_t dirty    : 1;
        uint64_t huge     : 1;
        uint64_t global   : 1;
        uint64_t _avail   : 3;
        uint64_t addr     : 40;
        uint64_t _avail2  : 11;
        uint64_t nx       : 1;
    };

    uint64_t phys_addr() const { return raw & ADDR_MASK; }

    void set_phys_addr(uint64_t phys) { raw = (raw & ~ADDR_MASK) | (phys & ADDR_MASK); }

    bool is_present() const { return (raw & FLAG_PRESENT) != 0; }
};

static_assert(sizeof(PageEntry) == 8, "PageEntry must be 8 bytes");
```

这个联合体提供了两种访问同一个 64 位数据的方式：`raw` 可以整体读写，位域结构体可以按名字访问每一个字段。之所以用联合体而不是纯粹的位域结构体，是因为在页表操作中我们经常需要同时操作"整个条目"和"某个字段"——比如设置新的映射时，我们需要把物理地址和标志位拼成一个 64 位的 raw 值写进去；读取时又需要提取物理地址或检查 present 位。如果只有位域，拼装 raw 就要逐字段赋值，既繁琐又有编译器位域布局不确定的风险；如果只有 raw，那每次检查标志位都要手写掩码，代码可读性差。

位域的排列顺序严格遵循 Intel SDM Vol.3A Table 4-20 规定的 PTE 格式，从 bit 0 到 bit 63。这里有一个值得说道的细节：`addr : 40` 字段对应的是 bit 51:12 的物理页帧号。由于 x86-64 小端序下 C++ 位域从低位到高位排列，GCC/Clang 会把 `addr` 正确地放在 bit 12 到 bit 51 的位置——这依赖于编译器对位域布局的约定。`static_assert(sizeof(PageEntry) == 8)` 就是用来验证这个假设：如果编译器的位域布局和硬件 PTE 格式不匹配，`PageEntry` 的大小就会不是 8 字节，编译直接报错。

三个 helper 方法简化了最常用的操作。`phys_addr()` 用 `ADDR_MASK` 从 raw 中提取物理地址——这比通过位域的 `addr` 字段访问更安全，因为位域访问需要编译器正确理解 40 位宽字段的语义。`set_phys_addr()` 在保留现有标志位的同时替换物理地址，用 `raw & ~ADDR_MASK` 保留非地址位，然后用 OR 拼入新的物理地址。`is_present()` 检查 Present 位——这是页表遍历中最频繁的操作，每走一级都要检查一次。

你可能会问：既然位域能直接访问 `present`，为什么 `is_present()` 要用 `raw & FLAG_PRESENT` 而不是直接返回 `present`？原因是性能和正确性：位域访问需要编译器生成移位和掩码指令，而 `raw & FLAG_PRESENT` 是一次简单的 AND 操作，编译器可以生成更优化的代码。而且在某些边缘情况下（比如需要 volatile 语义的 MMIO 操作），直接操作 raw 更可靠。

### TLB 刷新辅助函数——告诉硬件"页表改了"

修改了页表条目之后，CPU 内部的 TLB（Translation Lookaside Buffer）还缓存着旧的翻译结果。如果不清除缓存，CPU 会继续使用过时的地址映射——你可能把虚拟地址 0x200000 映射到了新的物理页，但 CPU 依然往旧物理页上写数据，而且不会报任何错误。这种 bug 非常隐蔽，因为"看起来程序在正常执行"，只是数据写到了错误的地方。

Cinux 提供了三个粒度的 TLB 刷新函数：

```cpp
// ============================================================
// TLB flush helpers
// ============================================================

inline void flush_tlb(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

inline void flush_tlb_all() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
```

`flush_tlb` 用 `invlpg` 指令使指定虚拟地址的 TLB 条目失效。这是 Intel SDM Vol.3A Section 4.10 推荐的单页刷新方式——它只影响一个虚拟地址对应的 TLB 条目（以及相关的 paging-structure cache 条目），不会影响其他页的翻译缓存。在 VMM 的 `map` 和 `unmap` 操作中，我们只需要刷新被修改的那一个页，所以用 `invlpg` 是最高效的选择。

`flush_tlb_all` 通过重载 CR3 来刷新所有非全局的 TLB 条目。它的原理是：CPU 检测到 CR3 被写入后，会认为页表结构可能完全变了，于是把所有非 Global 的 TLB 条目全部作废。注意这里不是真的"切换"页表——我们先读出 CR3 的值，再原样写回去，唯一的效果就是触发 TLB 刷新。在 `map_mmio` 中映射大范围 MMIO 区域后，用 `flush_tlb_all` 一次清除所有可能的过时缓存。

两个函数的 `__asm__ volatile` 中的 `"memory"` clobber 告诉编译器：这条内联汇编可能读写内存，不要把之前的内存操作优化掉。这非常重要——如果编译器把 `pt[idx].raw = ...` 的写操作优化到 `invlpg` 之后，那 TLB 刷新时页表还没更新，刷新就白做了。`volatile` 关键字确保编译器不会把整个 `invlpg` 优化掉——毕竟它没有输出操作数，优化器可能会认为它是"无用的"。

### CR3 访问器——页表根的读与写

```cpp
inline uint64_t read_cr3() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

inline void write_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
```

CR3 寄存器保存着当前页表根（PML4）的物理地址。`read_cr3` 在 VMM 初始化时用来读取 bootloader 设置的内核 PML4 地址，然后保存到 `VMM::kernel_pml4_` 成员中。`write_cr3` 则用于未来的进程切换——切换地址空间时需要把 CR3 指向新进程的 PML4。要注意的是，`write_cr3` 会隐式触发全量 TLB 刷新（和 `flush_tlb_all` 效果相同），这也是为什么进程切换是 TLB miss 的主要来源之一。

### paging.cpp 的重构——魔数消亡史

有了 `paging_config.hpp` 和 TLB 辅助函数，`paging.cpp` 里那些硬编码的魔数终于可以退休了。我们来看看重构后的完整代码：

```cpp
/**
 * @file kernel/arch/x86_64/paging.cpp
 * @brief Minimal paging implementation for the big kernel
 *
 * Manipulates the page tables at their known virtual addresses
 * (set up by the bootloader and extended by the mini kernel) to
 * map MMIO regions such as the framebuffer.
 */

#include "kernel/arch/x86_64/paging.hpp"
#include <stdint.h>
#include "kernel/arch/x86_64/paging_config.hpp"

namespace cinux::arch {

namespace {

constexpr uint64_t PD_HUGE_PAGE_FLAGS  = FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE;
constexpr uint64_t PDPT_1GB_PAGE_FLAGS = FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE;

constexpr uint64_t PAGE_2MB_SIZE = 0x200000;
constexpr uint64_t PAGE_1GB_SIZE = 0x40000000ULL;

constexpr uint64_t PD_VIRT_ADDR   = 0xFFFFFFFF80003000ULL;
constexpr uint64_t PDPT_VIRT_ADDR = 0xFFFFFFFF80002000ULL;

// PDPT[0] points to PD -- do not overwrite
constexpr uint32_t PDPT_PD_ENTRY = 0;

bool has_1gb_pages() {
    uint32_t eax = 0x80000001;
    uint32_t edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=d"(edx) : : "ebx", "ecx");
    return (edx & (1u << 26)) != 0;
}

}  // anonymous namespace
```

对比重构前，你会发现原来的 `PD_HUGE_PAGE_FLAGS = 0x83`（即 Present + Writable + Huge = 1 + 2 + 128 = 131 = 0x83）现在变成了 `FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE`，一眼就能看出每个标志位的含义。原来的 `reload_cr3()` 函数被完全删除，因为它和 `flush_tlb_all()` 做的事情一模一样。

```cpp
void map_mmio(uint64_t phys, uint64_t size) {
    auto* pd   = reinterpret_cast<volatile uint64_t*>(PD_VIRT_ADDR);
    auto* pdpt = reinterpret_cast<volatile uint64_t*>(PDPT_VIRT_ADDR);

    uint64_t end = phys + size;

    // Part 1: PD entries for range within first 1GB (2MB pages)
    uint64_t cur = phys & ~(PAGE_2MB_SIZE - 1);
    while (cur < end && cur < PAGE_1GB_SIZE) {
        uint32_t idx = static_cast<uint32_t>(cur / PAGE_2MB_SIZE);
        if (idx < PT_ENTRIES && pd[idx] == 0) {
            pd[idx] = cur | PD_HUGE_PAGE_FLAGS;
            flush_tlb(cur);
        }
        cur += PAGE_2MB_SIZE;
    }

    // Part 2: PDPT entries for range >= 1GB (1GB pages)
    if (end > PAGE_1GB_SIZE && has_1gb_pages()) {
        uint64_t cur1g = phys & ~(PAGE_1GB_SIZE - 1);
        if (cur1g < PAGE_1GB_SIZE)
            cur1g = PAGE_1GB_SIZE;

        while (cur1g < end) {
            uint32_t n = static_cast<uint32_t>(cur1g / PAGE_1GB_SIZE);
            if (n < PT_ENTRIES && n != PDPT_PD_ENTRY && pdpt[n] == 0) {
                pdpt[n] = cur1g | PDPT_1GB_PAGE_FLAGS;
            }
            cur1g += PAGE_1GB_SIZE;
        }
        flush_tlb_all();
    }
}

}  // namespace cinux::arch
```

`map_mmio` 是遗留的 MMIO 映射函数，用于帧缓冲区等设备的物理地址映射。它直接操作 bootloader 设置好的 PD 和 PDPT，用 2MB 或 1GB 大页把物理地址映射到虚拟地址空间。两处改动很显眼：原来手写的 `__asm__ volatile("invlpg (%0)" : : "r"(cur))` 现在变成了 `flush_tlb(cur)`，原来手写的 CR3 重载循环变成了 `flush_tlb_all()`。这不仅是代码整洁度的提升——更重要的是，这些辅助函数成为了统一的 TLB 操作入口，后续如果需要加日志、加 SMP 的 IPI 广播 TLB 刷新、或者统计 TLB miss 频率，只需要改一个地方。

## 设计决策

### Decision: 用联合体 + 位域映射 PTE，而非纯 raw 操作

**问题**：x86-64 的页表条目是 64 位整数，硬件按位域解释（Present 在 bit 0、Writable 在 bit 1、物理地址在 bit 51:12 等）。软件需要既方便地访问单个字段，又高效地读写整个条目。

**本项目的做法**：定义 `union PageEntry { uint64_t raw; struct { ... 位域 ... }; }`，同时提供 `raw` 整体访问和命名位域访问，并用 helper 方法（`phys_addr`、`is_present`）封装最常用操作。

**备选方案**：Linux 的做法——完全不用位域，只操作 raw 值，用宏/内联函数做掩码操作（`pte_val(pte) & _PAGE_PRESENT`）。

**为什么不选备选方案**：Linux 需要支持多种架构（x86、ARM、RISC-V 等），每种架构的 PTE 格式不同，位域无法跨架构统一。Cinux 只面向 x86-64，不存在跨架构问题。位域的可读性优势在这里充分体现——`entry.present` 比 `(entry >> 0) & 1` 或 `entry & FLAG_PRESENT` 直观得多。而且 `static_assert(sizeof(PageEntry) == 8)` 在编译期验证了位域布局的正确性，消除了"编译器可能不按预期排列位域"的顾虑。

**如果要扩展/改进，应该怎么做**：如果未来要支持 5 级分页（LA57），物理地址字段从 40 位扩展到 49 位，需要修改 `addr : 40` 为 `addr : 49`，同时更新 `ADDR_MASK`。如果编译器的位域布局出现问题，可以退化为 Linux 风格的纯 raw 操作——helper 方法已经封装了访问接口，内部实现可以自由切换。

### Decision: 将分页常量集中在 paging_config.hpp，而非散落各处

**问题**：分页常量（页大小、移位量、标志位、索引宏）被多个模块使用：`paging.cpp` 的 MMIO 映射、`vmm.cpp` 的页表遍历、`exception_handlers.cpp` 的缺页处理。这些常量应该放在哪里？

**本项目的做法**：创建独立的 `paging_config.hpp`，只放常量和 constexpr 函数，不放任何数据结构或运行时代码。所有使用分页的模块都 include 这个头文件。

**备选方案**：把这些常量放在 `paging.hpp` 里，和 `PageEntry` 定义在一起。

**为什么不选备选方案**：`paging_config.hpp` 只包含常量，没有数据结构依赖，也没有 include 其他头文件（除了 `<stdint.h>`），所以它的编译开销极小。而 `paging.hpp` 依赖了 `paging_config.hpp`，如果测试代码只需要常量而不需要 `PageEntry`（比如 host 端测试中 mirror 了常量定义），直接 include `paging_config.hpp` 就够了。这种"常量头文件"的模式在大型 C++ 项目中很常见——把编译慢的头文件的公共常量提取出来，让轻量级消费者不需要拉入整个重量级头文件。

**如果要扩展/改进，应该怎么做**：如果未来支持 5 级分页，可以在 `paging_config.hpp` 中加入 `P5_SHIFT = 48` 和 `P5_INDEX` 宏，并通过运行时检测 LA57 支持来选择使用 4 级还是 5 级常量。常量头文件的轻量级特性使得这种扩展不会引入不必要的编译依赖。

## 扩展方向

1. **实现 PAT (Page Attribute Table) 支持** (⭐⭐)：当前 `PageEntry` 的 bit 7 在 PT 层是 PAT 位而不是 Huge 位，但代码没有区分。研究 Intel SDM 的 PAT 机制，为 `PageEntry` 添加正确的 PAT 索引支持，实现 write-combining 缓存策略。

2. **5 级分页（LA57）探测与适配** (⭐⭐⭐)：阅读 Intel SDM Vol.3A Section 4.5 关于 5 级分页的描述，在 `paging_config.hpp` 中添加运行时 LA57 检测和条件编译，扩展 `ADDR_MASK` 到 49 位地址宽度。

3. **TLB 统计与性能分析** (⭐)：在 `flush_tlb` 和 `flush_tlb_all` 中加入计数器，统计 TLB 刷新频率。思考：什么场景下单页刷新（`invlpg`）比全量刷新（CR3 重载）更高效？用 benchmark 验证。

4. **大页（2MB/1GB）直接映射优化** (⭐⭐)：研究 Linux 的 `hugetlbfs` 机制，思考如何在 VMM 中支持混合页大小——部分地址空间用 4KB 页（按需分页友好），部分用 2MB 大页（TLB 覆盖率高）。

5. **PCID (Process-Context Identifier) 支持** (⭐⭐⭐)：阅读 Intel SDM Vol.3A Section 4.10.1，在 CR3 的低 12 位中编码 PCID，实现进程切换时避免全量 TLB 刷新。这需要先了解 CR3 的完整格式（bit 63:52 保留、bit 51:12 是 PML4 物理地址、bit 11:0 是 PCID）。

## 参考资料

- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：4 级分页完整描述，PML4/PDPT/PD/PT 的索引方式和地址翻译流程。Cinux 的四级移位常量和索引提取宏直接来源于此。
- Intel SDM Vol.3A Table 4-20, p.4-31：PTE 格式定义，每一位的含义和编号。Cinux 的 `PageEntry` 位域与此表一一对应。
- Intel SDM Vol.3A Section 4.10, pp.4-43 to 4-52：TLB 缓存和失效机制。INVLPG 指令的语义、CR3 重载对 TLB 的影响、Global 位的作用。
- Intel SDM Vol.3A Section 2.5, pp.2-14 to 2-16：CR3 寄存器格式，PML4 物理基地址在 CR3 中的位置。
- OSDev Wiki: [Paging (64-bit)](https://wiki.osdev.org/Paging) — x86-64 四级分页结构概述，虚拟地址位域划分。
- OSDev Wiki: [TLB](https://wiki.osdev.org/TLB) — TLB 刷新方法对比（INVLPG vs CR3 重载 vs 全局页）。
- xv6 RISC-V `vm.c`：xv6 的 PTE 操作使用类似的位域 + 掩码方式，可以对比 RISC-V Sv39 和 x86-64 四级分页的 PTE 格式差异。
