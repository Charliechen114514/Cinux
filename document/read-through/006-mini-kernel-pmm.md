# 006 - Mini Kernel PMM - 物理内存管理器

> **本章目标**：实现基于 Bitmap 的物理内存分配器，解析 E820 内存映射，管理 4KB 物理页面的分配与释放

**本章 git tag**：`006_mini_kernel_pmm`，上一章 tag：`005_mini_kernel_entry`

---

## 1. 本章概览

上一章我们让内核真正"活"了过来，能够通过串口输出信息告诉我们它的状态。但老实说，内核还处在"裸奔"状态——没有任何内存管理能力，无法动态分配内存，更别提实现更复杂的内核功能了。这一章，我们要建立物理内存管理的基础设施。

### 本章实现的关键功能

- **物理内存分配器**（Physical Memory Manager, PMM）：基于 Bitmap 的 4KB 页面分配器，支持 alloc_page/free_page 操作
- **E820 内存图解析**：从 BootInfo 中解析 BIOS 提供的内存映射，识别可用内存区域
- **内存区域保护**：自动标记内核自身、低 1MB 保留区域为已用，防止被意外分配
- **内存统计输出**：输出系统总内存和可用内存信息，格式为 `[MINI] PMM: Total XMB, Free XMB`

### 关键设计决策

- 采用 Bitmap 而非链表结构管理物理页，简化实现并减少内存开销
- Bitmap 静态分配在内核数据段末尾，避免动态分配带来的"鸡生蛋"问题
- 过滤低 1MB 内存区域，保留给 BIOS 和 bootloader 使用
- 使用链接器符号 `__kernel_size` 动态计算内核占用的物理页数

### 与同类 OS 的对比

相比 Linux 早期版本复杂的 buddy system 和 zone 管理，我们的实现非常简化——只管理单一 zone、只支持 4KB 页面分配。这足以支撑小内核的需求，同时保持代码可读性。相比 xv6 直接在启动时初始化 kalloc，我们选择将 PMM 作为独立模块，为将来扩展（如支持不同页面大小）做准备。

---

## 2. 架构图

```
    PMM Architecture Overview

    ┌─────────────────────────────────────────────────────────────────┐
    │                         BootInfo (from bootloader)               │
    │  ┌────────────────────────────────────────────────────────────┐ │
    │  │  Memory Map (E820)                                          │ │
    │  │  ┌──────────┬───────────┬──────────┬────────────┐          │ │
    │  │  │ Entry 0  │ 0x000000  │ 0x09FC00 │ Type 1     │          │ │
    │  │  │ Entry 1  │ 0x09FC00  │ 0x000400 │ Type 2     │          │ │
    │  │  │ Entry 2  │ 0x0010000 │ 0x7F0000 │ Type 1     │          │ │
    │  │  │ ...      │ ...       │ ...      │ ...        │          │ │
    │  │  └──────────┴───────────┴──────────┴────────────┘          │ │
    │  └────────────────────────────────────────────────────────────┘ │
    └─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │                     PMM::init(BootInfo*)                        │
    │  ┌────────────────────────────────────────────────────────────┐ │
    │  │  Step 1: Initialize Bitmap                                  │ │
    │  │    - Mark all pages as used (0xFF)                          │ │
    │  │    - Reset counters                                         │ │
    │  └────────────────────────────────────────────────────────────┘ │
    │  ┌────────────────────────────────────────────────────────────┐ │
    │  │  Step 2: Parse E820 Memory Map                              │ │
    │  │    - Filter low 1MB (reserved)                              │ │
    │  │    - Mark usable regions as free                            │ │
    │  │    - Calculate highest page                                 │ │
    │  └────────────────────────────────────────────────────────────┘ │
    │  ┌────────────────────────────────────────────────────────────┐ │
    │  │  Step 3: Mark Kernel Region as Used                         │ │
    │  │    - Read __kernel_size from linker                         │ │
    │  │    - Mark kernel pages as used                              │ │
    │  └────────────────────────────────────────────────────────────┘ │
    │  ┌────────────────────────────────────────────────────────────┐ │
    │  │  Step 4: Mark Bootloader Region (0x0-0x10000) as Used        │ │
    │  └────────────────────────────────────────────────────────────┘ │
    └─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │                      Bitmap Storage                             │
    │  ┌────────────────────────────────────────────────────────────┐ │
    │  │  uint8_t s_bitmap[BITMAP_SIZE] = {0}                       │ │
    │  │                                                             │ │
    │  │  ┌─Byte 0─┬─Byte 1─┬─Byte 2─┬─Byte 3─┬─Byte 4─┬─ ...      │ │
    │  │  │11111111│00000000│00000000│00000000│11111111│  ...      │ │
    │  │  └────────┴────────┴────────┴────────┴────────┴──────────┘ │ │
    │  │   ^Used    ^Free    ^Free    ^Free    ^Used                 │ │
    │  │   Page 0   Page 8   Page 16  Page 24  Page 32              │ │
    │  └────────────────────────────────────────────────────────────┘ │
    └─────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │                     PMM API Functions                           │
    │  ┌────────────────────────────────────────────────────────────┐ │
    │  │  alloc_page() → uint64_t (physical address, 0=OOM)         │ │
    │  │    └─> find_first_free() → set_bit() → return phys        │ │
    │  │                                                             │ │
    │  │  free_page(uint64_t phys)                                  │ │
    │  │    └─> validate → clear_bit() → increment counter         │ │
    │  │                                                             │ │
    │  │  free_page_count() → uint64_t                              │ │
    │  │  total_page_count() → uint64_t                             │ │
    │  └────────────────────────────────────────────────────────────┘ │
    └─────────────────────────────────────────────────────────────────┘

    Memory Layout After PMM Initialization:

    ┌────────────────────────────────────────────────────────────────┐
    │ 0x00000000                                                       │
    │ ├─ Low 1MB (Reserved)                                          │
    │ │  ├─ 0x00000 - 0x9FC00: Conventional Memory (marked used)     │
    │ │  └─ 0x9FC00 - 0x100000: BIOS/Data (marked used)             │
    ├─ 0x00100000                                                      │
    │ ├─ Available Memory (starts here)                               │
    │ │  ├─ Page 256 (0x100000): First usable page                   │
    │ │  ├─ ...                                                        │
    │ │  └─ Page N: Kernel loads here                                │
    ├─ 0x20000 (Kernel Physical Base)                                 │
    │ ├─ Mini Kernel Image                                           │
    │ │  ├─ .text, .rodata, .data, .bss                              │
    │ │  └─ Bitmap storage (at __mini_kernel_end, aligned)           │
    ├─ 0x20000 + kernel_size                                          │
    │ └─ More Available Memory                                        │
    └────────────────────────────────────────────────────────────────┘
```

---

## 3. 关键代码精讲

### 3.1 内存字面量与基础定义

在实现 PMM 之前，我们首先建立了一套内存相关的定义和字面量操作符。这些工具函数会在整个内存管理子系统中反复使用。

```cpp
// kernel/mini/mm/memory_literals.h
namespace cinux::mini::mm::literals {

constexpr uint64_t operator""_KB(unsigned long long value) {
    return value * 1024ULL;
}

constexpr uint64_t operator""_MB(unsigned long long value) {
    return value * 1024ULL * 1024ULL;
}

constexpr uint64_t operator""_GB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace cinux::mini::mm::literals
```

这里我们使用了 C++11 引入的用户定义字面量（user-defined literals）特性。通过定义 `""_KB`、`""_MB`、`""_GB` 后缀操作符，我们可以在代码中直接写 `4_KB`、`1_MB` 这样的字面量，编译器会在编译期计算出对应的字节数。这比传统的 `4096` 或 `0x1000` 写法更具可读性，而且因为是 `constexpr`，计算完全在编译期完成，没有任何运行时开销。

### 3.2 PMM 接口定义

PMM 的公共接口非常简洁，只暴露了初始化、分配、释放和统计这几个核心功能。

```cpp
// kernel/mini/mm/pmm.h
namespace cinux::mini::mm::pmm {

// ============================================================
// Constants
// ============================================================
constexpr uint64_t PAGE_SIZE			  = 4_KB;			// 4KB pages
constexpr uint64_t MAX_MEMORY			= 4_GB;			// 4GB max supported
constexpr uint64_t MAX_PAGES			= MAX_MEMORY / PAGE_SIZE;		// 1M pages max
constexpr uint64_t BITMAP_SIZE		  = MAX_PAGES / 8;				// 128KB bitmap
constexpr uint64_t LOW_MEMORY_BOUNDARY  = 1_MB; // 1MB

// ============================================================
// Initialization
// ============================================================
void init(const void* boot_info);

// ============================================================
// Page Allocation
// ============================================================
uint64_t alloc_page();
void free_page(uint64_t phys);

// ============================================================
// Statistics
// ============================================================
uint64_t free_page_count();
uint64_t total_page_count();

} // namespace cinux::mini::mm::pmm
```

这里有几个设计决策值得说明。首先是 `MAX_MEMORY = 4_GB` 的选择——x86-64 架构理论上支持巨大的物理地址空间，但对于小内核来说，4GB 已经绰绰有余。这个限制主要由 Bitmap 大小决定：每个页面对应一个 bit，4GB 内存需要 1M 个页面，也就是 128KB 的 Bitmap。如果把 Bitmap 放在内核数据段中，这个大小是可以接受的。

另一个关键常量是 `LOW_MEMORY_BOUNDARY = 1_MB`。这是 x86 平台的传统保留区域，前 1MB 包含了 BIOS 数据区、IVT（中断向量表）、以及一些遗留的硬件映射区域。我们选择将这个区域整个标记为已用，避免复杂的内存碎片问题。

### 3.3 Bitmap 内部实现

Bitmap 的核心操作是设置、清除和测试单个 bit。这些操作虽然简单，但正确实现位运算需要一些技巧。

```cpp
// kernel/mini/mm/pmm.cpp
namespace {

void set_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] |= (1U << bit_idx);
}

void clear_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] &= ~(1U << bit_idx);
}

bool test_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    return (s_bitmap[byte_idx] & (1U << bit_idx)) != 0;
}

int64_t find_first_free() {
    // Scan bitmap for first zero bit
    for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
        if (s_bitmap[byte_idx] != 0xFF) {
            // Found a byte with at least one free bit
            uint8_t byte = s_bitmap[byte_idx];
            for (uint64_t bit_idx = 0; bit_idx < 8; bit_idx++) {
                if ((byte & (1U << bit_idx)) == 0) {
                    return static_cast<int64_t>(byte_idx * 8 + bit_idx);
                }
            }
        }
    }
    return -1;	// No free pages
}

} // anonymous namespace
```

`find_first_free()` 函数实现了首次适应（first-fit）分配策略。它首先扫描字节数组，找到第一个不全是 1 的字节（即至少有一个空闲页面），然后逐位检查找到第一个零位。这种实现的时间复杂度在最坏情况下是 O(N)，其中 N 是页面总数。对于 4GB 内存来说，最坏情况需要扫描 128KB 的 Bitmap，这在启动阶段是可以接受的。

如果要优化这个函数，可以使用 `__builtin_ctz()`（count trailing zeros）内建函数来快速找到第一个零位：

```cpp
// 优化版本的 find_first_free
int64_t find_first_free() {
    for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
        if (s_bitmap[byte_idx] != 0xFF) {
            // 反转字节，使第一个 0 变成第一个 1
            uint8_t inverted = ~s_bitmap[byte_idx];
            // __builtin_ctz 返回末尾 0 的个数，也就是第一个 1 的位置
            int bit_idx = __builtin_ctz(inverted);
            return static_cast<int64_t>(byte_idx * 8 + bit_idx);
        }
    }
    return -1;
}
```

### 3.4 内存区域标记

`mark_region_used()` 和 `mark_region_free()` 函数用于批量标记一段连续物理内存区域的页面状态。这两个函数在初始化阶段会被频繁调用。

```cpp
// kernel/mini/mm/pmm.cpp
void mark_region_used(uint64_t phys, uint64_t length) {
    uint64_t start_page = phys / PAGE_SIZE;
    uint64_t end_page	= (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t page = start_page; page < end_page; page++) {
        if (page < MAX_PAGES && !test_bit(page)) {
            set_bit(page);
            s_free_pages--;
        }
    }
}

void mark_region_free(uint64_t phys, uint64_t length) {
    uint64_t start_page = phys / PAGE_SIZE;
    uint64_t end_page	= (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t page = start_page; page < end_page; page++) {
        if (page < MAX_PAGES) {
            clear_bit(page);
            s_free_pages++;
        }
    }
}
```

这里有个细节需要注意：计算 `end_page` 时使用了向上取整的公式 `(phys + length + PAGE_SIZE - 1) / PAGE_SIZE`。这确保了即使长度不是页面大小的整数倍，我们也会正确标记部分占用的最后一页。

另一个细节是 `mark_region_used()` 中检查 `!test_bit(page)`。这个检查避免了重复标记同一页面导致的 `s_free_pages` 计数错误。实际上，这个检查在正确使用的情况下不是必需的，但作为一个防御性编程的手段，它能帮助我们在初始化逻辑出错时更容易发现问题。

### 3.5 PMM 初始化流程

PMM 的初始化是本章最复杂的部分，需要按正确顺序执行多个步骤。

```cpp
// kernel/mini/mm/pmm.cpp
void init(const void* boot_info) {
    const BootInfo* info = static_cast<const BootInfo*>(boot_info);

    // Step 1: Initialize bitmap - mark all pages as used
    for (uint64_t i = 0; i < BITMAP_SIZE; i++) {
        s_bitmap[i] = 0xFF;
    }
    s_total_pages  = 0;
    s_free_pages   = 0;
    s_highest_page = 0;

    // Step 2: Parse E820 memory map and mark available regions as free
    for (uint32_t i = 0; i < info->mmap_count; i++) {
        const MemoryMapEntry* entry = &info->mmap[i];

        // Only process usable memory (type = 1)
        if (entry->type != 1) {
            continue;
        }

        uint64_t base	= entry->base;
        uint64_t length = entry->length;

        // Update highest page
        uint64_t end_page = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
        if (end_page > s_highest_page) {
            s_highest_page = end_page;
            if (s_highest_page > MAX_PAGES) {
                s_highest_page = MAX_PAGES;
            }
        }

        // Filter out low 1MB (reserved by bootloader)
        if (base < LOW_MEMORY_BOUNDARY) {
            if (length <= LOW_MEMORY_BOUNDARY - base) {
                // Entire region is in low 1MB, skip it
                continue;
            }
            // Partial overlap: adjust base and length
            length -= (LOW_MEMORY_BOUNDARY - base);
            base = LOW_MEMORY_BOUNDARY;
        }

        // Align to page boundaries
        uint64_t aligned_base	= (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t aligned_length = length - (aligned_base - base);

        if (aligned_length < PAGE_SIZE) {
            continue;
        }

        // Mark pages as free
        mark_region_free(aligned_base, aligned_length);
    }

    s_total_pages = s_highest_page;

    // Step 3: Mark kernel region as used
    // Use linker-provided __kernel_size (note: &__kernel_size gives the value)
    uint64_t kernel_phys = info->kernel_phys_base;
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
    lib::kprintf("[MINI] PMM: kernel_phys=0x%x, kernel_size=0x%x (%u pages)\n", kernel_phys,
                 kernel_size, (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE);
    mark_region_used(kernel_phys, kernel_size);

    // Step 4: Mark bootloader regions as used (0x0 - 0x10000)
    lib::kprintf("[MINI] PMM: marking bootloader 0x0-0x10000 used (%u pages)\n",
                 0x10000 / PAGE_SIZE);
    mark_region_used(0x0, 0x10000);

    // Debug output
    lib::kprintf("[MINI] PMM: Total %u pages (%u MB), Free %u pages (%u MB)\n", s_total_pages,
                 (s_total_pages * PAGE_SIZE) / 1_MB, s_free_pages,
                 (s_free_pages * PAGE_SIZE) / 1_MB);
}
```

初始化流程分为四个步骤，每个步骤都有其特定的目的：

**第一步：初始化 Bitmap 为全 1**

我们采用"先全部标记为已用，再释放可用区域"的策略，而不是相反。这样做的好处是，即使 E820 内存图有错误或者遗漏，默认行为是保守的——不会错误地把不可用的内存标记为可用。

**第二步：解析 E820 内存图**

E820 是 BIOS 提供的内存映射接口，通过 INT 15h AX=E820h 获取。bootloader 已经替我们完成了这个工作，并将结果整理成 `MemoryMapEntry` 数组放在 `BootInfo` 中。每个条目包含基地址、长度和类型：

| Type | 含义 |
|------|------|
| 1 | 可用内存（Usable）|
| 2 | 保留区域（Reserved）|
| 3 | ACPI Reclaim |
| 4 | ACPI NVS |
| 其他 | 各种特定用途 |

我们只关心 Type 1 的可用内存。对于每个可用区域，我们：

1. 更新 `s_highest_page`，记录管理的最高页面号
2. 过滤掉低 1MB 区域
3. 对齐到 4KB 页面边界
4. 调用 `mark_region_free()` 标记为可用

低 1MB 的过滤有个有趣的边界情况：如果某个 E820 条目跨越了 1MB 边界，我们需要调整基地址和长度，只处理 1MB 以上的部分。代码中的 `length -= (LOW_MEMORY_BOUNDARY - base)` 就是用来计算调整后的长度。

**第三步：标记内核区域为已用**

这里我们使用了链接器符号 `__kernel_size`。这个符号由链接器脚本 `linker.ld` 计算得出：

```ld
/* Kernel end markers (global for PMM to use) */
__mini_kernel_end = .;
PROVIDE(__kernel_size = (__mini_kernel_end - (KERNEL_Virt_BASE + KERNEL_PHYS_BASE)));
```

重要的一点是，访问链接器符号必须使用 `&__kernel_size` 取地址，而不是直接访问 `__kernel_size`。这是因为链接器符号本质上是地址常量，而不是变量。直接访问会读取该地址处的内存内容（可能是垃圾值），而取地址得到的才是符号本身的值。

**第四步：标记 bootloader 区域为已用**

bootloader 代码位于物理地址 0x0 - 0x10000（64KB）。虽然我们已经过滤了低 1MB，但 bootloader 的具体位置需要显式标记为已用，防止将来分配器在启用低 1MB 后错误分配。

### 3.6 页面分配与释放

初始化完成后，分配和释放页面的实现就相对简单了。

```cpp
// kernel/mini/mm/pmm.cpp
uint64_t alloc_page() {
    int64_t page_idx = find_first_free();
    if (page_idx < 0) {
        return 0;  // OOM
    }

    set_bit(static_cast<uint64_t>(page_idx));
    s_free_pages--;

    return static_cast<uint64_t>(page_idx) * PAGE_SIZE;
}

void free_page(uint64_t phys) {
    if (phys == 0) {
        return;	 // Null address, ignore
    }

    uint64_t page_idx = phys / PAGE_SIZE;
    if (page_idx >= MAX_PAGES) {
        return;	 // Invalid address
    }

    if (test_bit(page_idx)) {
        clear_bit(page_idx);
        s_free_pages++;
    }
}
```

`alloc_page()` 使用首次适应策略找到第一个空闲页面，标记为已用，并返回物理地址。如果没有可用页面（OOM），返回 0。在 x86-64 上，物理地址 0 是保留的，所以用 0 表示错误是安全的。

`free_page()` 需要处理几个边界情况：

1. **空地址检查**：`phys == 0` 时直接返回，避免对空指针进行操作
2. **地址越界检查**：确保 `page_idx < MAX_PAGES`，防止越界访问 Bitmap
3. **重复释放检查**：只有在页面确实已用（`test_bit` 返回 true）时才执行释放，避免计数错误

### 3.7 链接器符号访问的陷阱

实现 PMM 过程中，我踩了一个关于链接器符号的坑，值得单独拿出来讲一下。

链接器脚本中定义的符号（如 `__kernel_size`）不是普通的变量。它们只是地址常量，链接器不会为它们分配内存。当你声明 `extern uint64_t __kernel_size` 时，编译器会认为这是一个外部变量，生成读取该地址内存的代码。但该地址处可能什么都没有，或者包含的是其他数据。

正确的做法是声明为 `char` 类型，然后取地址：

```cpp
// 错误写法
extern "C" {
    extern uint64_t __kernel_size;  // 声明为 uint64_t 变量
}
uint64_t size = __kernel_size;  // ❌ 读取该地址的内存，得到垃圾值

// 正确写法
extern "C" {
    extern char __kernel_size;     // 声明为 char 类型
}
uint64_t size = reinterpret_cast<uint64_t>(&__kernel_size);  // ✓ 取地址得到值
```

对于链接器符号来说，地址即值。`&__kernel_size` 得到的地址就是符号的值，而类型 `char` 只是为了满足语法要求，实际不重要。

---

## 4. 设计决策深度分析

### 决策一：Bitmap vs 链表分配器

**问题**：物理内存分配器应该使用 Bitmap 结构还是链表结构？

**本项目的做法**：采用 Bitmap，每个 4KB 页面对应一个 bit，1 表示已用，0 表示可用。

**备选方案**：
- 链表结构（如 Linux 的 buddy system）
- 栈式分配器（适用于单一区域连续分配）
- AVL/红黑树管理的空闲区

**为什么不选链表结构**：

1. **内存开销**：链表每个节点需要存储 prev/next 指针和大小信息，对于大量小块内存来说开销显著。Bitmap 的开销是固定的 128KB（4GB 内存），与页面数量线性相关。

2. **实现复杂度**：链表分配器需要处理节点合并、分裂、边界条件等问题，实现和调试都比较复杂。Bitmap 的操作更直观，不容易出错。

3. **分配策略确定性**：首次适应（first-fit）在 Bitmap 上实现简单，性能可预测。链表分配器可能需要遍历整个链表，最坏情况下的性能较差。

**Bitmap 的限制**：
- 只支持固定大小的页面分配（4KB），无法高效处理不同大小的分配请求
- 释放页面时不进行合并（因为页面大小固定），无法产生更大的连续块
- 对于内存碎片问题较敏感，容易出现外部碎片

**如果要扩展**：当需要支持不同页面大小（如 2MB、1GB 大页）时，可以考虑分层 Bitmap 或混合结构：
- 为 2MB 页面维护一个单独的 Bitmap（每个 bit 代表 2MB）
- 为 4KB 页面维护详细 Bitmap
- 分配时优先尝试大页，降级到小页

### 决策二：Bitmap 存储位置

**问题**：Bitmap 数据结构应该放在哪里？

**本项目的做法**：静态分配在内核数据段的 `s_bitmap` 数组中，位于 `__mini_kernel_end` 附近。

**备选方案**：
- 放在 BSS 段（静态全局变量）
- 动态分配在可用内存的某个位置
- 放在专门保留的内存区域

**为什么不选动态分配**：

1. **鸡生蛋问题**：动态分配需要 PMM 工作，但 PMM 初始化需要 Bitmap 存在。这形成了一个循环依赖。

2. **简单性**：静态分配不需要任何初始化逻辑，编译器会确保它在 `.data` 段中有正确的空间。

3. **确定性**：静态分配的地址在编译时确定，便于调试和问题排查。

**静态分配的注意事项**：
- Bitmap 大小是固定的（128KB 对于 4GB），即使实际内存更小也会占用这个空间
- 如果实际内存超过 4GB，需要增加 `MAX_MEMORY` 常量
- Bitmap 本身占用的页面需要在初始化时标记为已用（虽然这在我们的实现中是隐式的，因为 Bitmap 在内核数据段内）

**如果要扩展**：当需要支持大于 4GB 的内存时，可以：
1. 增加 `MAX_MEMORY` 常量（如 16GB、64GB）
2. 检测实际内存大小，动态调整 Bitmap 大小
3. 将 Bitmap 放在更高的物理地址，避免占用低位内存

### 决策三：低 1MB 内存处理策略

**问题**：如何处理低 1MB 的保留内存区域？

**本项目的做法**：在初始化时过滤掉所有低 1MB 的 E820 条目，将该区域视为已用。

**备选方案**：
- 精确解析 E820，只标记明确保留的区域
- 允许分配低于 1MB 的某些可用内存（如 640KB-1MB 之间）
- 完全忽略低 1MB，让调用者自行处理

**为什么不选精确解析**：

1. **复杂性 vs 收益**：低 1MB 的布局复杂，包含 BIOS 数据区、IVT、显卡显存映射等。精确解析需要大量平台特定代码，但收益有限——只有几百 KB 的内存。

2. **兼容性**：某些平台可能对低 1MB 有特殊要求，保守地全部标记为已用更安全。

3. **简化分配逻辑**：分配器不需要处理"是否可以分配低 1MB"的判断，统一从 1MB 以上开始分配。

**过滤的实现细节**：

```cpp
// Filter out low 1MB (reserved by bootloader)
if (base < LOW_MEMORY_BOUNDARY) {
    if (length <= LOW_MEMORY_BOUNDARY - base) {
        // Entire region is in low 1MB, skip it
        continue;
    }
    // Partial overlap: adjust base and length
    length -= (LOW_MEMORY_BOUNDARY - base);
    base = LOW_MEMORY_BOUNDARY;
}
```

这段代码处理了三种情况：
- 条目完全在 1MB 以下：直接跳过
- 条目跨越 1MB 边界：调整 base 到 1MB，减少 length
- 条目完全在 1MB 以上：无需调整

**如果要扩展**：当需要启用低 1MB 的某些可用内存时（如 640KB-1MB 的 UMA），可以：
1. 精确解析 E820，只跳过真正保留的区域
2. 在分配器中添加"低位内存分配"的专用接口
3. 添加配置选项，允许在运行时选择是否启用低位内存分配

---

## 5. 常见变体与扩展方向

1. **支持 2MB 大页分配** ⭐
   - 维护一个单独的 2MB 页面 Bitmap
   - 实现 `alloc_large_page()` 接口
   - 在初始化时优先尝试 2MB 对齐的区域
   - 注意：需要处理 4KB Bitmap 和 2MB Bitmap 的同步问题

2. **优化查找算法** ⭐
   - 使用 `__builtin_ctz()` 替代逐位扫描
   - 维护一个"首个空闲页面的缓存"，避免重复扫描
   - 实现分层 Bitmap（如每 64 页一个块，先扫描块再扫描位）

3. **支持内存回收策略** ⭐⭐
   - 实现 `defragment()` 函数，移动可移动的页面以减少碎片
   - 支持页面热/冷分类，优先分配"冷"页面
   - 实现页面分配统计，用于性能分析和优化

4. **支持动态内存热插拔** ⭐⭐⭐
   - 检测新加入的内存区域
   - 动态扩展 Bitmap 大小
   - 重新平衡已分配页面到新内存
   - 实现内存热移除时的页面迁移

5. **添加内存调试功能** ⭐⭐
   - 实现 `dump_bitmap()` 输出当前分配状态
   - 添加页面分配跟踪（谁分配了哪一页）
   - 检测内存泄漏（分配但未释放的页面）
   - 实现 `poison_page()` 用于检测野指针写入

---

## 6. 参考资料

### Intel/AMD 手册
- Intel SDM Vol. 3: Chapter 14 - Memory Map and BIOS
- Intel SDM Vol. 3: Chapter 15 - Advanced Programmable Interrupt Controller (APIC)
- AMD64 Architecture Programmer's Manual: Vol. 2 - System Programming

### OSDev Wiki
- Physical Memory Manager: https://wiki.osdev.org/PMM
- E820: https://wiki.osdev.org/Detecting_Memory_(x86)
- Page Tables: https://wiki.osdev.org/Paging

### 其他资源
- "The Design and Implementation of the 4.4BSD Operating System" - 内存管理章节
- "Understanding the Linux Kernel" - Buddy System 实现
- QEMU Documentation - `-m` 选项和内存配置

---

## 7. 验证步骤

要验证本章实现，可以运行以下命令：

```bash
# 构建内核
cmake -B build -DCINUX_BUILD_TESTS=ON
cmake --build build

# 运行生产内核
qemu-system-x86_64 \
    -drive format=raw,file=build/image/cinux.img \
    -serial stdio \
    -m 128M

# 运行测试内核
qemu-system-x86_64 \
    -drive format=raw,file=build/image/cinux_test.img \
    -serial stdio \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -m 128M
```

预期输出：

生产内核应该显示：
```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xFFFFFFFF80020000, kernel_phys_base=0x20000
Boot Memory Info: mmap_count=3
  [0] base=0x0000000000000000, length=0x000000000009fc00, type=1, acpi=1
  [1] base=0x000000000009fc00, length=0x0000000000000400, type=2, acpi=1
  [2] base=0x0000000000100000, length=0x0000000007ef0000, type=1, acpi=1
[MINI] PMM: kernel_phys=0x20000, kernel_size=0x42f0 (17 pages)
[MINI] PMM: marking bootloader 0x0-0x10000 used (16 pages)
[MINI] PMM: Total 32256 pages (126 MB), Free 32222 pages (125 MB)
```

测试内核应该完整运行所有 PMM 单元测试，包括：
- 链接器符号访问测试
- PMM 初始化测试
- 单页分配/释放测试
- 多页分配测试
- 边界条件测试
- OOM 处理测试

---

## 8. 完整源码

### 8.1 `kernel/mini/mm/pmm.h`

```cpp
/**
 * @file kernel/mini/mm/pmm.h
 * @brief Physical Memory Manager (PMM) - Bitmap Allocator
 *
 * Simple bitmap-based physical page allocator for the mini kernel.
 * Uses one bit per 4KB page to track available/used memory.
 */

#pragma once

#include <stdint.h>
#include "mm_defines.h"

namespace cinux::mini::mm::pmm {

// ============================================================
// Constants
// ============================================================
constexpr uint64_t PAGE_SIZE			  = 4_KB;			// 4KB pages
constexpr uint64_t MAX_MEMORY			= 4_GB;			// 4GB max supported
constexpr uint64_t MAX_PAGES			= MAX_MEMORY / PAGE_SIZE;		// 1M pages max
constexpr uint64_t BITMAP_SIZE		  = MAX_PAGES / 8;				// 128KB bitmap
constexpr uint64_t LOW_MEMORY_BOUNDARY  = 1_MB; // 1MB

// ============================================================
// Initialization
// ============================================================
/**
 * @brief Initialize the PMM from BootInfo
 * @param boot_info Pointer to BootInfo structure from bootloader
 *
 * Parses the E820 memory map, initializes the bitmap, and marks
 * reserved regions (kernel, bitmap itself, low 1MB) as used.
 */
void init(const void* boot_info);

// ============================================================
// Page Allocation
// ============================================================
/**
 * @brief Allocate a single physical page
 * @return Physical address of allocated page, 0 if OOM
 */
uint64_t alloc_page();

/**
 * @brief Free a single physical page
 * @param phys Physical address of page to free
 */
void free_page(uint64_t phys);

// ============================================================
// Statistics
// ============================================================
/**
 * @brief Get total number of free pages
 */
uint64_t free_page_count();

/**
 * @brief Get total number of pages in system
 */
uint64_t total_page_count();

} // namespace cinux::mini::mm::pmm
```

### 8.2 `kernel/mini/mm/pmm.cpp`

```cpp
/**
 * @file kernel/mini/mm/pmm.cpp
 * @brief Physical Memory Manager (PMM) - Bitmap Allocator Implementation
 */

#include "pmm.h"

#include <stddef.h>
#include <stdint.h>

#include "../../../boot/boot_info.h"
#include "lib/kprintf.h"

namespace cinux::mini::mm::pmm {

// ============================================================
// Internal State
// ============================================================
static uint64_t s_total_pages		  = 0;	  // Total pages in system
static uint64_t s_free_pages		  = 0;	  // Free pages available
static uint64_t s_highest_page		  = 0;	  // Highest page index managed
static uint8_t	s_bitmap[BITMAP_SIZE] = {0};  // Bitmap storage

// External symbols from linker (use &symbol to get the value)
extern "C" {
	extern char __kernel_size;     // Kernel size in bytes (from linker.ld)
	extern char __mini_kernel_end; // End of kernel (from linker.ld)
}

namespace {

// ============================================================
// Bitmap Operations
// ============================================================
void set_bit(uint64_t index) {
	uint64_t byte_idx = index / 8;
	uint64_t bit_idx  = index % 8;
	s_bitmap[byte_idx] |= (1U << bit_idx);
}

void clear_bit(uint64_t index) {
	uint64_t byte_idx = index / 8;
	uint64_t bit_idx  = index % 8;
	s_bitmap[byte_idx] &= ~(1U << bit_idx);
}

bool test_bit(uint64_t index) {
	uint64_t byte_idx = index / 8;
	uint64_t bit_idx  = index % 8;
	return (s_bitmap[byte_idx] & (1U << bit_idx)) != 0;
}

int64_t find_first_free() {
	// Scan bitmap for first zero bit
	for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
		if (s_bitmap[byte_idx] != 0xFF) {
			// Found a byte with at least one free bit
			uint8_t byte = s_bitmap[byte_idx];
			for (uint64_t bit_idx = 0; bit_idx < 8; bit_idx++) {
				if ((byte & (1U << bit_idx)) == 0) {
					return static_cast<int64_t>(byte_idx * 8 + bit_idx);
				}
			}
		}
	}
	return -1;	// No free pages
}

// ============================================================
// Memory Region Management
// ============================================================
void mark_region_used(uint64_t phys, uint64_t length) {
	uint64_t start_page = phys / PAGE_SIZE;
	uint64_t end_page	= (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

	for (uint64_t page = start_page; page < end_page; page++) {
		if (page < MAX_PAGES && !test_bit(page)) {
			set_bit(page);
			s_free_pages--;
		}
	}
}

void mark_region_free(uint64_t phys, uint64_t length) {
	uint64_t start_page = phys / PAGE_SIZE;
	uint64_t end_page	= (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

	for (uint64_t page = start_page; page < end_page; page++) {
		if (page < MAX_PAGES) {
			clear_bit(page);
			s_free_pages++;
		}
	}
}

}  // anonymous namespace

// ============================================================
// Initialization
// ============================================================
void init(const void* boot_info) {
	const BootInfo* info = static_cast<const BootInfo*>(boot_info);

	// Step 1: Initialize bitmap - mark all pages as used
	for (uint64_t i = 0; i < BITMAP_SIZE; i++) {
		s_bitmap[i] = 0xFF;
	}
	s_total_pages  = 0;
	s_free_pages   = 0;
	s_highest_page = 0;

	// Step 2: Parse E820 memory map and mark available regions as free
	for (uint32_t i = 0; i < info->mmap_count; i++) {
		const MemoryMapEntry* entry = &info->mmap[i];

		// Only process usable memory (type = 1)
		if (entry->type != 1) {
			continue;
		}

		uint64_t base	= entry->base;
		uint64_t length = entry->length;

		// Update highest page
		uint64_t end_page = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
		if (end_page > s_highest_page) {
			s_highest_page = end_page;
			if (s_highest_page > MAX_PAGES) {
				s_highest_page = MAX_PAGES;
			}
		}

		// Filter out low 1MB (reserved by bootloader)
		if (base < LOW_MEMORY_BOUNDARY) {
			if (length <= LOW_MEMORY_BOUNDARY - base) {
				// Entire region is in low 1MB, skip it
				continue;
			}
			// Partial overlap: adjust base and length
			length -= (LOW_MEMORY_BOUNDARY - base);
			base = LOW_MEMORY_BOUNDARY;
		}

		// Align to page boundaries
		uint64_t aligned_base	= (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		uint64_t aligned_length = length - (aligned_base - base);

		if (aligned_length < PAGE_SIZE) {
			continue;
		}

		// Mark pages as free
		mark_region_free(aligned_base, aligned_length);
	}

	s_total_pages = s_highest_page;

	// Step 3: Mark kernel region as used
	// Use linker-provided __kernel_size (note: &__kernel_size gives the value)
	uint64_t kernel_phys = info->kernel_phys_base;
	uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
	lib::kprintf("[MINI] PMM: kernel_phys=0x%x, kernel_size=0x%x (%u pages)\n", kernel_phys,
				 kernel_size, (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE);
	mark_region_used(kernel_phys, kernel_size);

	// Step 4: Mark bootloader regions as used (0x0 - 0x10000)
	lib::kprintf("[MINI] PMM: marking bootloader 0x0-0x10000 used (%u pages)\n",
				 0x10000 / PAGE_SIZE);
	mark_region_used(0x0, 0x10000);

	// Debug output
	lib::kprintf("[MINI] PMM: Total %u pages (%u MB), Free %u pages (%u MB)\n", s_total_pages,
				 (s_total_pages * PAGE_SIZE) / 1_MB, s_free_pages,
				 (s_free_pages * PAGE_SIZE) / 1_MB);
}

// ============================================================
// Page Allocation
// ============================================================
uint64_t alloc_page() {
	int64_t page_idx = find_first_free();
	if (page_idx < 0) {
		return 0;  // OOM
	}

	set_bit(static_cast<uint64_t>(page_idx));
	s_free_pages--;

	return static_cast<uint64_t>(page_idx) * PAGE_SIZE;
}

void free_page(uint64_t phys) {
	if (phys == 0) {
		return;	 // Null address, ignore
	}

	uint64_t page_idx = phys / PAGE_SIZE;
	if (page_idx >= MAX_PAGES) {
		return;	 // Invalid address
	}

	if (test_bit(page_idx)) {
		clear_bit(page_idx);
		s_free_pages++;
	}
}

// ============================================================
// Statistics
// ============================================================
uint64_t free_page_count() {
	return s_free_pages;
}

uint64_t total_page_count() {
	return s_total_pages;
}

}  // namespace cinux::mini::mm::pmm
```

### 8.3 `kernel/mini/mm/mm_defines.h`

```cpp
/**
 * @file kernel/mini/mm/mm_defines.h
 * @brief Memory Management Common Definitions
 *
 * Central header for memory-related utilities and literals.
 * Include this file to access memory size literals and common definitions.
 */

#pragma once

#include <stdint.h>

// Import memory literals (_KB, _MB, _GB, _TB)
#include "memory_literals.h"

namespace cinux::mini::mm {

// ============================================================
// Common Page Size Definitions
// ============================================================
constexpr uint64_t PAGE_SIZE_4K = 4_KB;   // 4096 bytes
constexpr uint64_t PAGE_SIZE_2M = 2_MB;   // 2097152 bytes
constexpr uint64_t PAGE_SIZE_1G = 1_GB;   // 1073741824 bytes

// ============================================================
// Memory Alignment Helpers
// ============================================================
/**
 * @brief Align address up to specified alignment
 * @param addr Address to align
 * @param align Alignment boundary (must be power of 2)
 * @return Aligned address
 */
constexpr uint64_t align_up(uint64_t addr, uint64_t align) {
	return (addr + align - 1) & ~(align - 1);
}

/**
 * @brief Align address down to specified alignment
 * @param addr Address to align
 * @param align Alignment boundary (must be power of 2)
 * @return Aligned address
 */
constexpr uint64_t align_down(uint64_t addr, uint64_t align) {
	return addr & ~(align - 1);
}

/**
 * @brief Check if address is aligned to specified boundary
 * @param addr Address to check
 * @param align Alignment boundary
 * @return true if aligned, false otherwise
 */
constexpr bool is_aligned(uint64_t addr, uint64_t align) {
	return (addr & (align - 1)) == 0;
}

} // namespace cinux::mini::mm
```

### 8.4 `kernel/mini/mm/memory_literals.h`

```cpp
/**
 * @file kernel/mini/mm/memory_literals.h
 * @brief Custom Literal Operators for Memory Sizes
 *
 * Provides constexpr user-defined literal operators for KB, MB, GB, TB.
 * Freestanding-compatible - requires no standard library.
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::mm::literals {

/**
 * @brief Kilobyte literal operator
 * @param value Numeric value in kilobytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 4_KB → 4096
 */
constexpr uint64_t operator""_KB(unsigned long long value) {
	return value * 1024ULL;
}

/**
 * @brief Megabyte literal operator
 * @param value Numeric value in megabytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 1_MB → 1048576
 */
constexpr uint64_t operator""_MB(unsigned long long value) {
	return value * 1024ULL * 1024ULL;
}

/**
 * @brief Gigabyte literal operator
 * @param value Numeric value in gigabytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 4_GB → 4294967296
 */
constexpr uint64_t operator""_GB(unsigned long long value) {
	return value * 1024ULL * 1024ULL * 1024ULL;
}

/**
 * @brief Terabyte literal operator
 * @param value Numeric value in terabytes
 * @return Equivalent bytes as uint64_t
 *
 * Example: 1_TB → 1099511627776
 */
constexpr uint64_t operator""_TB(unsigned long long value) {
	return value * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace cinux::mini::mm::literals

// Import literals into cinux::mini::mm namespace for convenience
namespace cinux::mini::mm {
	using namespace literals;
}
```

---

到这里，006 章节的物理内存管理器就完成了。我们建立了一个简单但可靠的 Bitmap 分配器，能够正确解析 E820 内存图，保护关键内存区域，并提供基本的页面分配/释放功能。下一章，我们将在这个基础上建立中断处理框架，让内核能够响应硬件事件和处理异常。
