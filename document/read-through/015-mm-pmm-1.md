---
title: 015-mm-pmm-1 · 物理内存管理
---

# 015-1 通读：E820 解析与位图辅助函数——物理内存的"眼睛"和"手指"

## 概览

本文是 tag `015_mm_pmm` 三篇通读教程的第一篇，聚焦于物理内存管理器的两个基础设施组件：E820 内存映射解析和位图辅助函数。在整个 PMM 的架构中，它们是最底层的砖块——`parse_memory_map` 是内核的"眼睛"，负责从 BIOS 留下的 E820 数据中看清楚"哪些物理内存是真正可用的"；而 `bm_set`/`bm_clear`/`bm_test`/`bm_find_first_free` 这四个函数是内核的"手指"，负责在位图上精确地标记和查找每一页的状态。没有这两组工具，后续的初始化、分配和释放都无从谈起。

我们按以下顺序展开讲解：先看 `pmm.hpp` 头文件中的 `MemoryRegion` 结构体、`parse_memory_map` 声明和 `PMM` 类的完整接口；然后深入 `pmm.cpp` 的实现，从常量定义和链接器符号开始，经过匿名命名空间中的四个位图辅助函数，最终走到 `parse_memory_map` 的四道过滤关卡。整篇文章涉及约 120 行核心实现代码，覆盖了从硬件数据格式到位图操作的全部底层细节。

## 架构图

```
    BootInfo (物理 0x7000)
        │
        ├── mmap_count: 32
        ├── mmap[0..31]: MemoryMapEntry (24 bytes each)
        │       ┌─────────────────────────────────┐
        │       │ base:    0x0000000000000000      │
        │       │ length:  0x000000000009FC00      │  type=1 (usable)
        │       │ type:    1                       │
        │       │ acpi:    0                       │
        │       └─────────────────────────────────┘
        │       ┌─────────────────────────────────┐
        │       │ base:    0x0000000000100000      │
        │       │ length:  0x0000000003FEE0000     │  type=1 (usable)
        │       │ type:    1                       │
        │       │ acpi:    0                       │
        │       └─────────────────────────────────┘
        │       ...
        │
        ▼
    parse_memory_map()           ← "眼睛"
        │
        │  四道关卡:
        │  ① type == 1?  ─── 过滤非可用类型
        │  ② base >= 1MB? ── 低内存保护
        │  ③ 4KB 对齐     ─── 页粒度规整
        │  ④ length >= 4KB? ─ 丢弃碎片
        │
        ▼
    MemoryRegion[]               ← 干净的可用区域列表
        [{base: 0x100000, length: 0x3FEE0000}, ...]

    ────────────────────────────────────────────

    位图辅助函数 (匿名命名空间)    ← "手指"
        │
        ├── bm_set(bm, idx)     →  置 1 (标记已用)
        ├── bm_clear(bm, idx)   →  清 0 (标记空闲)
        ├── bm_test(bm, idx)    →  查询 (是否已用)
        └── bm_find_first_free  →  64位批量扫描找空闲
                │
                ├── qword scan: bm64[i] != ~0ULL?
                │       └── __builtin_ctzll(~bm64[i]) 定位 bit
                └── tail scan:  逐字节逐 bit 兜底

    位图物理布局:
        ┌───┬───┬───┬───┬───┬─────┬───┐
        │ 0 │ 1 │ 2 │ 3 │ 4 │ ... │ N │  uint8_t 数组
        ├───┼───┼───┼───┼───┼─────┼───┤
        │11100011│10000001│...     │   每个 bit = 1 页
        │pg0..pg7│pg8..pg15│       │   1=已用, 0=空闲
        └───┴───┴───┴───┴───┴─────┴───┘
```

## 代码精讲

### pmm.hpp —— 公共接口与数据结构

我们从 PMM 的公共头文件开始。这个文件定义了三个东西：`MemoryRegion` 结构体、`parse_memory_map` 自由函数声明，以及 `PMM` 类的完整接口。使用者只需要 include 这一个头文件就能获得 PMM 的全部能力。

```cpp
#pragma once

#include <stdint.h>

#include "boot/boot_info.h"

namespace cinux::mm {

/// A usable physical memory region extracted from the E820 map.
struct MemoryRegion {
    uint64_t base;
    uint64_t length;
};
```

`MemoryRegion` 极其精简，只有 `base` 和 `length` 两个 `uint64_t` 字段。它是 `parse_memory_map` 的输出类型——经过过滤和对齐后的"干净"内存区域。之所以不用 E820 的原始 `MemoryMapEntry`（那个有 base + length + type + acpi 四个字段），是因为经过解析后，所有输出的区域都保证是 type=1（可用）、4KB 对齐的，type 和 acpi 字段已经没有意义了。这种"尽早丢弃无用信息"的做法让后续代码不需要关心 E820 的复杂性。

接下来是 `parse_memory_map` 的声明：

```cpp
uint32_t parse_memory_map(const BootInfo& info,
                          MemoryRegion* regions,
                          uint32_t max_regions);
```

这个函数不依赖任何 PMM 内部状态——它是一个纯函数，接收 BootInfo 引用作为输入、写入调用者提供的数组作为输出，返回值是实际写入的区域数量。把它设计成自由函数而不是 `PMM` 类的成员方法是有意为之的：host 端单元测试可以独立测试 E820 解析逻辑而不需要构造一个完整的 PMM 对象。

然后是 `PMM` 类的完整接口，这是整个物理内存管理器的门面：

```cpp
class PMM {
public:
    /** Initialise from the bootloader-provided memory map. */
    void init(const BootInfo& info);

    /** Allocate a single 4 KB page.  Returns physical address, 0 on OOM. */
    uint64_t alloc_page();

    /** Free a single page (no-op if phys is 0 or already free). */
    void free_page(uint64_t phys);

    /** Allocate @p count contiguous pages.  Returns base phys addr, 0 on OOM. */
    uint64_t alloc_pages(uint64_t count);

    /** Free @p count contiguous pages starting at @p phys. */
    void free_pages(uint64_t phys, uint64_t count);

    /** Current number of free pages. */
    uint64_t free_page_count() const;

    /** Total number of pages managed. */
    uint64_t total_page_count() const;

private:
    void mark_region_used(uint64_t phys, uint64_t length);
    void mark_region_free(uint64_t phys, uint64_t length);

    uint8_t* bitmap_{};
    uint64_t total_pages_{};
    uint64_t free_pages_{};
    uint64_t highest_page_{};
    uint64_t bitmap_size_{};
};

/// Global PMM instance.
extern PMM g_pmm;
```

关于这个类设计，有几个值得注意的决策。所有状态都是实例成员而不是 `static`——这意味着理论上你可以创建多个 PMM 实例。现在整个内核只有一个 `g_pmm` 全局实例，但实例化设计让测试变得更容易（虽然当前测试没有利用这一点，因为 host 端测试直接重写了算法，而不是链接内核代码）。位图指针用 `uint8_t*` 而不是 `uint64_t*`——虽然 64 位扫描时需要 `reinterpret_cast`，但基础操作（set/clear/test）以字节为单位更直观，也更容易正确处理尾部字节。

私有成员全部使用了 C++11 的类内初始化（`bitmap_{}`、`total_pages_{}` 等）。这意味着默认构造的 PMM 对象所有指针为 `nullptr`、所有整数为 0，不需要额外的构造函数。这在内核环境中特别重要——全局对象 `g_pmm` 的构造发生在静态初始化阶段，没有自定义构造函数就不会有"static initialization order fiasco"。

最后是全局实例声明 `extern PMM g_pmm`。整个内核通过 `cinux::mm::g_pmm` 访问唯一的 PMM。

### pmm.cpp —— 常量与链接器符号

现在我们进入实现文件。`pmm.cpp` 的开头定义了三个核心常量和两个链接器符号：

```cpp
#include "kernel/mm/pmm.hpp"

#include <stddef.h>

#include "kernel/lib/kprintf.hpp"

namespace cinux::mm {

// ============================================================
// Constants
// ============================================================

constexpr uint64_t PAGE_SIZE        = 4096;
constexpr uint64_t LOW_MEM_BOUNDARY = 0x100000;               // 1 MB
constexpr uint64_t KERNEL_VMA       = 0xFFFFFFFF80000000ULL;
```

这三个常量是 PMM 全部计算的基石。`PAGE_SIZE = 4096` 对应 x86_64 long mode 的标准 4KB 页大小，Intel SDM Vol.3A Section 4.5 规定页表项中物理基地址的低 12 位必须为零，因为它们被用作标志位。`LOW_MEM_BOUNDARY = 0x100000`（1MB）是物理地址空间中"神圣不可侵犯"的分界线——低于 1MB 的区域充满了 IVT、BDA、视频内存、BIOS ROM 等固件数据结构。这些布局是 PC 兼容机的固件约定，详见 OSDev Wiki "Memory Map (x86)"。`KERNEL_VMA = 0xFFFFFFFF80000000ULL` 是 Cinux 内核的虚拟地址基址，在 higher-half 内核中，虚拟地址和物理地址之间有一个固定的偏移量，这个值必须和链接脚本 `kernel.ld` 中设定的 VMA 完全一致。

这里有一个很容易踩的坑：如果你在链接脚本里改了 VMA 但忘了同步这里的 `KERNEL_VMA` 常量，后面所有涉及虚拟-物理地址转换的计算都会出错。更糟糕的是，这种错误在初始化阶段不会 crash（位图只是被写到了"错误的物理内存位置"），但在分配出的页面被用于页表构建后会引发莫名其妙的 page fault，排查起来非常痛苦。所以每次修改链接脚本后，一定要确认这个常量是否还同步。

这些常量被放在 `.cpp` 文件而不是头文件中，是因为它们是实现细节，不需要暴露给 PMM 的使用者。虽然 `constexpr` 变量在 C++ 中是内联链接的，放进头文件也不会导致 ODR 违规，但从 API 设计的角度来说，`kernel/mm/pmm.hpp` 的 include 者不需要知道 `KERNEL_VMA` 是多少。

接下来是链接器符号的声明：

```cpp
// ============================================================
// Linker symbols
// ============================================================

extern "C" {
extern char __kernel_end;
extern char __kernel_stack_top;
}
```

这两个符号来自链接脚本 `kernel.ld`，它们不是真正的变量，而是地址常量。正确的访问模式是取它们的地址：`reinterpret_cast<uintptr_t>(&__kernel_stack_top)`，而不是直接读 `__kernel_stack_top` 的值——直接读会从符号的地址处读取一段垃圾数据，这个坑在 tag 006 的笔记里有详细记录。`__kernel_end` 标记内核映像的末尾，`__kernel_stack_top` 标记内核栈顶。PMM 使用 `__kernel_stack_top` 来确定位图的放置位置——位图紧挨着内核栈后面，页对齐后开始。

然后是全局实例的定义：

```cpp
// ============================================================
// Global instance
// ============================================================

PMM g_pmm;
```

因为 PMM 类依赖类内初始化的默认值，所以 `g_pmm` 在静态初始化阶段就被正确地置为零状态（`bitmap_ = nullptr`、所有计数器 = 0），不需要显式构造函数调用。

### 位图辅助函数 —— bm_set / bm_clear / bm_test

现在我们来看位图操作的核心。这四个函数被放在匿名命名空间中，这意味着它们具有内部链接性——只有 `pmm.cpp` 内部能访问它们，不会污染全局命名空间。

位图的本质是一个 `uint8_t` 数组，每个字节包含 8 个 bit，每个 bit 代表一个 4KB 物理页。给定一个页索引 `idx`，它所在的字节是 `bm[idx / 8]`，在该字节中的位偏移是 `idx % 8`。图示如下：

```
索引:    0   1   2   3   4   5   6   7  |  8   9  10  11 ...
         ^                               |  ^
         bm[0]                           |  bm[1]
位:     b0  b1  b2  b3  b4  b5  b6  b7  | b0  b1  b2  b3 ...
```

```cpp
namespace {

void bm_set(uint8_t* bm, uint64_t idx) {
    bm[idx / 8] |= static_cast<uint8_t>(1U << (idx % 8));
}

void bm_clear(uint8_t* bm, uint64_t idx) {
    bm[idx / 8] &= static_cast<uint8_t>(~(1U << (idx % 8)));
}

bool bm_test(const uint8_t* bm, uint64_t idx) {
    return (bm[idx / 8] & (1U << (idx % 8))) != 0;
}
```

这三个函数的逻辑可以用一句话概括：用位运算在字节数组中定位、修改或读取一个 bit。`bm_set` 用 OR 把对应 bit 置 1，`bm_clear` 用 AND 把对应 bit 清零，`bm_test` 用 AND 检查对应 bit 是否为 1。全部是 O(1) 操作——一次数组访问加一次位运算。

有一个细节值得注意：`static_cast<uint8_t>` 包裹了所有的位运算表达式。这不是多此一举——C++ 的整型提升规则会把 `1U << (idx % 8)` 提升为 `int` 或 `unsigned int`（取决于平台），然后 OR/AND 操作也是在 `int` 宽度上进行的。虽然最终赋值回 `uint8_t` 时会隐式截断，但显式 cast 让意图更清晰，也避免了某些编译器在有符号/无符号混合运算时发出的警告。

### 位图辅助函数 —— bm_find_first_free（64 位加速扫描）

这是整个位图分配器中技术含量最高的函数，也是单页分配操作的核心。它的任务是在位图中找到第一个值为 0 的 bit（第一个空闲页），返回对应的页索引。

```cpp
/// Scan 64 bits at a time using __builtin_ctzll for the first free bit.
int64_t bm_find_first_free(const uint8_t* bm, uint64_t highest_page,
                           uint64_t bitmap_size) {
    const auto* bm64 = reinterpret_cast<const uint64_t*>(bm);
    uint64_t qword_count = bitmap_size / sizeof(uint64_t);
```

函数接收三个参数：位图指针 `bm`、最高页号 `highest_page`（用于边界检查）、位图字节数 `bitmap_size`（用于计算扫描范围）。返回类型是 `int64_t`——找到时返回非负的页索引，找不到时返回 -1。

核心优化在于把 `uint8_t` 数组重新解释为 `uint64_t` 数组，每次检查 64 个 bit 而不是 8 个。`reinterpret_cast<const uint64_t*>(bm)` 要求位图缓冲区的起始地址是 8 字节对齐的——我们的位图放在页边界上，页大小 4096 是 8 的倍数，所以天然满足这个条件。`qword_count = bitmap_size / sizeof(uint64_t)` 算出能组成多少个完整的 64 位组。

接下来是主扫描循环：

```cpp
    for (uint64_t i = 0; i < qword_count; i++) {
        if (bm64[i] != ~0ULL) {
            int bit = __builtin_ctzll(~bm64[i]);
            uint64_t idx = i * 64 + static_cast<uint64_t>(bit);
            if (idx < highest_page) return static_cast<int64_t>(idx);
        }
    }
```

这段代码的精髓在于 `~0ULL` 和 `__builtin_ctzll` 的配合使用。`~0ULL` 是全 1 的 64 位值——如果一个 qword 等于 `~0ULL`，说明它对应的 64 页全部已用，直接跳过。如果不等于全 1，说明其中至少有一个 0 bit（至少一页空闲），这时 `~bm64[i]` 把 0 变成 1、1 变成 0，然后 `__builtin_ctzll`（count trailing zeros for long long）告诉我们最低位的 1 在哪个位置——也就是原来第一个 0 的位置。这相当于一次操作检查了 64 页，理论上比逐 bit 扫描快 64 倍。

这里有一个必须强调的安全前提：`__builtin_ctzll` 在输入为 0 时是未定义行为。但由于我们在调用它之前已经检查了 `bm64[i] != ~0ULL`，所以 `~bm64[i]` 保证非零——如果 64 个 bit 全是 1，条件不满足，根本不会走到 `ctzll` 调用。这个守卫条件绝对不能删掉，否则在位图全满时会触发 UB，而且这种 bug 在大多数情况下不会 crash，只是返回一个错误的结果，特别难排查。

`idx < highest_page` 是边界检查——位图的大小是按 `(highest_page + 7) / 8` 向上取整计算的，所以位图的最后几个 bit 可能对应不存在的页号。这个检查确保我们不会返回一个超出管理范围的页索引。

然后是尾部扫描，处理 qword 扫描覆盖不到的剩余字节：

```cpp
    // Handle tail bytes not covered by the qword scan
    for (uint64_t byte = qword_count * 8; byte < bitmap_size; byte++) {
        if (bm[byte] != 0xFF) {
            for (uint64_t bit = 0; bit < 8; bit++) {
                uint64_t idx = byte * 8 + bit;
                if (idx < highest_page && !(bm[byte] & (1U << bit))) {
                    return static_cast<int64_t>(idx);
                }
            }
        }
    }

    return -1;
}
```

当 `bitmap_size` 不是 8 的整数倍时（比如管理 100 页需要 13 字节，13 / 8 = 1 个 qword 覆盖前 8 字节 = 64 页，剩余 5 字节 = 40 页需要逐字节扫描），这个尾部循环就派上用场了。逻辑是朴素的逐字节逐 bit 检查：先跳过全 1 的字节（`0xFF` 表示 8 页全部已用），然后在非全 1 的字节中逐 bit 找第一个 0。

至此，`bm_find_first_free` 的完整实现就讲完了。如果所有 qword 和尾部字节扫描完都没找到空闲页，返回 -1 表示内存已满。

```cpp
}  // anonymous namespace
```

### parse_memory_map —— E820 数据的四道过滤关卡

现在我们来看 `parse_memory_map` 的实现，这是连接 BIOS 硬件数据和内核内存管理的桥梁。

```cpp
uint32_t parse_memory_map(const BootInfo& info,
                          MemoryRegion* regions,
                          uint32_t max_regions) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < info.mmap_count && count < max_regions; i++) {
        const auto& entry = info.mmap[i];
        if (entry.type != 1) continue;
```

函数的骨架是一个双重约束的循环：`i < info.mmap_count` 保证不越界读 E820 条目，`count < max_regions` 保证不越界写输出数组。循环体首先检查 `entry.type != 1`，这是第一道关卡——类型过滤。E820 的 type 字段含义如下：1 = Usable RAM，2 = Reserved，3 = ACPI Reclaimable，4 = ACPI NVS，5 = Unusable。我们只关心 type=1 的条目，其他一律跳过。

接下来是第二道关卡——低内存过滤：

```cpp
        uint64_t base   = entry.base;
        uint64_t length = entry.length;

        // Filter: everything below 1 MB is reserved
        if (base < LOW_MEM_BOUNDARY) {
            if (base + length <= LOW_MEM_BOUNDARY) continue;
            length -= LOW_MEM_BOUNDARY - base;
            base = LOW_MEM_BOUNDARY;
        }
```

低 1MB 区域在 PC 架构中有特殊地位。这些布局是 PC 兼容机的固件约定（详见 OSDev Wiki "Memory Map (x86)"），而非 Intel 架构规范的一部分：0x00000-0x003FF 是中断向量表（IVT），0x00400-0x004FF 是 BIOS 数据区（BDA），0x05000-0x05FFF 是 Cinux 的 E820 缓冲区，0x06400-0x06FFF 是 VESA 信息区，0x07000-0x07FFF 是 BootInfo 结构体本身，0xA0000-0xBFFFF 是 VGA 文本/图形缓冲区，0xF0000-0xFFFFF 是 BIOS ROM。这些区域虽然 E820 可能报告为 type=1（可用），但实际上不能碰。

所以处理策略是：如果区域基地址低于 1MB，分两种情况。如果整个区域都在 1MB 以下（`base + length <= LOW_MEM_BOUNDARY`），直接跳过整条。如果区域跨越了 1MB 边界，把基地址截断到 1MB，同时缩短长度——`length -= LOW_MEM_BOUNDARY - base` 先算出"低 1MB 部分占了多少"，然后从总长度里减去。这种截断方式虽然保守——我们可能白白浪费掉传统低内存中确实空闲的某些部分——但换来了绝对的安全。在一个教学 OS 里，这一点点浪费完全值得。

这里要特别小心截断时的整数运算。正确的做法是先减后赋值（`length -= LOW_MEM_BOUNDARY - base; base = LOW_MEM_BOUNDARY`），这个顺序很重要——如果先改了 `base`，原来的 `base` 就丢了，没法算 `LOW_MEM_BOUNDARY - base` 了。

第三道关卡是 4KB 对齐：

```cpp
        // Align base up, length down to 4 KB
        uint64_t aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        length -= (aligned_base - base);
        length &= ~(PAGE_SIZE - 1);
```

Intel 的 4 级分页机制使用 4KB 作为最小的页大小，页表项中的物理基地址必须是 4KB 对齐的——低 12 位被用作标志位（Present、ReadWrite、UserSupervisor 等），不能是地址的一部分。Intel SDM Vol.3A Section 4.5 对此有明确规定。所以我们的 PMM 也必须以 4KB 为最小分配粒度。

基地址向上对齐使用的是经典的 `(base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)` 公式。对于 4KB 对齐，`PAGE_SIZE - 1 = 0xFFF`，`~0xFFF = 0xFFFFF000`，这个 AND 操作把低 12 位清零——但先加了 `PAGE_SIZE - 1`，所以如果 `base` 本身不是 4KB 对齐的，它会"进位"到下一个对齐边界。例如 `0x100123` 对齐后变成 `0x101000`。

长度先减去对齐带来的偏移量（`aligned_base - base`），这表示对齐操作"吃掉"了前面那段不对齐的空间。然后 `length &= ~(PAGE_SIZE - 1)` 把长度向下截断到 4KB 边界——不足一个页的尾部碎片被丢弃。

运算符优先级在这里需要格外小心：`~(PAGE_SIZE - 1)` 中，`~` 的优先级低于 `-`，所以先算 `PAGE_SIZE - 1 = 0xFFF`，再取反得到 `0xFFFFF000`，这是正确的。但如果你不小心写成了 `& ~PAGE_SIZE - 1`，那就变成了先取反 `~4096 = 0xFFFFF000...FFFFEFF`（假设 int 是 32 位），再减 1，结果完全错误——这种运算符优先级错误是位操作中最常见的 bug 之一。

第四道关卡是大小检查：

```cpp
        if (length < PAGE_SIZE) continue;
        regions[count++] = {aligned_base, length};
    }

    return count;
}
```

经过对齐后，如果剩余长度不足一个页（4096 字节），这个区域就被丢弃。不到 4KB 的碎片对页分配器来说没有意义——我们不可能分配"半个页"。通过了四道关卡的条目被写入输出数组，用聚合初始化 `{aligned_base, length}` 构造 `MemoryRegion`。

我们可以用一个具体的例子来走一遍完整的流程。假设 E820 返回了一条条目：base=0x80000, length=0x200000, type=1。这道条目从 512KB 开始，跨越 2MB，到 2.5MB 结束。第一道关卡：type=1，通过。第二道关卡：base=0x80000 < 0x100000，且 base+length=0x280000 > 0x100000，所以截断——length 变为 0x200000 - (0x100000 - 0x80000) = 0x180000，base 变为 0x100000。第三道关卡：0x100000 已经是 4KB 对齐的，aligned_base = 0x100000，偏移量为 0，length 不变仍为 0x180000。第四道关卡：0x180000 = 1.5MB > 4096，通过。最终输出 `{base: 0x100000, length: 0x180000}`。

## 设计决策

### 决策：低 1MB 全部过滤 vs 精确排除已知占用区域

**问题**：E820 报告的低 1MB 区域中有些确实是空闲的（比如 0x08000-0x4FFFF 这段空间），精确排除已知占用的区域可以回收这些内存，但实现复杂度更高。

**本项目的做法**：使用 `LOW_MEM_BOUNDARY = 0x100000`，一刀切地把整个低 1MB 都过滤掉。

**备选方案**：维护一个"已知占用区域"列表（IVT、BDA、BootInfo、E820 缓冲区、VESA 信息、视频内存、BIOS ROM），在解析时精确排除这些区域，保留确实空闲的部分。

**为什么不选备选方案**：教学 OS 的目标物理内存通常在 32-128MB 范围，低 1MB 中能回收的内存最多几百 KB，对整体可用内存的影响不到 1%。而精确排除需要额外的数据结构和更复杂的解析逻辑，增加了出错的可能性。保守策略虽然浪费了一点内存，但换来了代码的简洁和安全。

**如果要扩展/改进**：可以定义一个 `ReservedRegion` 数组，列出所有已知占用区域的地址范围，在 `parse_memory_map` 中用类似"打孔"的方式精确排除。这在生产级 OS 中是必要的，但在教学 OS 中优先级很低。

### 决策：64 位 qword 扫描 vs 逐字节扫描

**问题**：`bm_find_first_free` 的性能对分配操作至关重要。逐字节扫描每次检查 8 页，64 位扫描每次检查 64 页。

**本项目的做法**：用 `reinterpret_cast<const uint64_t*>` 把位图视为 `uint64_t` 数组，配合 `__builtin_ctzll` 一次定位空闲页，理论上比逐 bit 扫描快 64 倍。

**备选方案**：使用更简单的逐字节扫描，每次检查 `bm[byte] != 0xFF`，找到非全 1 字节后再逐 bit 检查。

**为什么不选备选方案**：64 位扫描的代码量只比逐字节扫描多了约 10 行（qword 循环 + 尾部循环），但性能提升显著。对于 128MB 物理内存（32768 页 = 4096 字节位图 = 512 个 qword），全满扫描只需 512 次 qword 比较，而逐字节扫描需要 4096 次。`__builtin_ctzll` 是 GCC/Clang 的内置函数，会被编译为一条 `tzcnt` 或 `bsf` 指令，开销极低。OSDev Wiki 的 Page Frame Allocation 页面也提到了这种优化思路。

**如果要扩展/改进**：可以进一步使用 SIMD 指令（SSE/AVX）做 128 位或 256 位批量扫描，或者使用二级索引（每 N 页一个汇总字节，标记该组是否全满）来跳过大段已用区域。Linux 的 bootmem 和 buddy 分配器使用了类似的多级加速策略。

## 扩展方向

- **E820 条目合并** (⭐)：当 BIOS 返回的相邻条目有重叠或紧邻时，实现一个合并步骤，将它们合并为一个大区域。这能处理某些 BIOS 返回无序或重复条目的情况。

- **可配置的低内存边界** (⭐)：把 `LOW_MEM_BOUNDARY` 从硬编码常量改为可通过 BootInfo 或编译选项配置的值，方便在不同硬件平台上调整策略。

- **SIMD 加速扫描** (⭐⭐)：使用 SSE2 的 `_mm_cmpeq_epi8` 一次比较 16 字节（128 页），或者用 AVX2 的 256 位比较一次检查 256 页。需要处理对齐和尾部情况。

- **二级索引加速** (⭐⭐)：在位图之上维护一个"汇总层"——每 64 页对应一个 bit，标记这组是否全满。扫描时先检查汇总层，全满的组直接跳过。这将分配的最坏情况从 O(N) 降低到 O(N/64)。

- **parse_memory_map 排序输出** (⭐)：对解析后的 MemoryRegion 数组按 base 地址排序，便于后续的分配器做更高效的范围查询和碎片分析。

## 参考资料

- OSDev Wiki: [Memory Map (x86)](https://wiki.osdev.org/Memory_Map_(x86)) — PC 低 1MB 物理内存布局约定：IVT (0x0000-0x03FF)、BDA (0x0400-0x04FF)、视频内存 (0xA0000-0xBFFFF)、BIOS ROM (0xF0000-0xFFFFF)
- Intel SDM Vol.3A Section 4.5 (Paging) — 4KB 页的 12 位对齐要求，页表项格式
- OSDev Wiki: [Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — 位图、栈、buddy 等分配器设计的比较
- OSDev Wiki: [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) — E820 INT 15h 接口，type-1 = usable RAM
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — higher-half 内核的地址空间布局
- xv6 `kalloc.c`: https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c — 使用链表分配器的不同策略对比
