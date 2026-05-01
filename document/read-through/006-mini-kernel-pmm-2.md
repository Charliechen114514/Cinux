# 006-2 PMM 核心实现代码走读

## 概览

本文走读 `pmm.cpp` 的完整实现代码——这是 tag 006 最核心的文件。它包含位图操作的底层实现、E820 内存映射的解析逻辑、保守初始化策略的具体执行，以及页分配和释放的完整流程。我们会按功能段拆分代码，逐段讲解设计意图和实现细节。

关键设计决策一览：保守初始化（先锁定全部再开放）、低 1MB 过滤、链接器符号通过取地址访问、朴素 first-fit 扫描、O(1) 分配/释放配合 O(N) 查找。

---

## 架构图

```
                    ┌─────────────┐
                    │  main.cpp   │
                    │  调用 init() │
                    └──────┬──────┘
                           │
                    ┌──────▼──────┐
                    │   pmm::init │
                    │  四步初始化  │
                    └──────┬──────┘
                           │
          ┌────────────────┼────────────────┐
          │                │                │
   ┌──────▼──────┐  ┌──────▼──────┐  ┌──────▼──────┐
   │ 1.全标已用   │  │ 2.E820解析  │  │ 3.标内核已用 │
   │ bitmap=0xFF  │  │ 标可用为空闲│  │ __kernel_size│
   └─────────────┘  └─────────────┘  └─────────────┘
                                            │
                                    ┌───────▼───────┐
                                    │4.标bootloader │
                                    │0x0-0x10000    │
                                    └───────────────┘

运行时路径:
  alloc_page() → find_first_free() → set_bit() → 返回地址
  free_page()  → 计算索引 → test_bit() → clear_bit()
```

---

## 代码精讲

### 内部状态与链接器符号声明

```cpp
namespace cinux::mini::mm::pmm {

static uint64_t s_total_pages      = 0;
static uint64_t s_free_pages       = 0;
static uint64_t s_highest_page     = 0;
static uint8_t  s_bitmap[BITMAP_SIZE] = {0};

extern "C" {
    extern char __kernel_size;
    extern char __mini_kernel_end;
}
```

模块内部维护四个静态变量。`s_total_pages` 记录系统总共管理多少页（等于 `s_highest_page`），`s_free_pages` 记录当前空闲页数，`s_highest_page` 是 E820 中发现的最高物理地址对应的页索引，`s_bitmap` 就是那个 128KB 的位数组。所有变量都是 `static` 的，模块外部无法直接访问——只能通过公开的接口函数。

链接器符号的声明方式是整个 PMM 中最容易出错的地方。我们用 `extern "C"` 包裹，避免 C++ 的 name mangling 把符号名改掉。类型选 `char` 不是因为它真的是一个字符——事实上链接器根本没有为这些符号分配内存——而是因为 `char` 是最小的可取地址类型，我们只关心它的地址。当你写 `&__kernel_size` 时，编译器把这个符号的地址作为值给你，而这个地址恰好就是链接器脚本中计算好的内核大小数值。

### 位图操作函数

```cpp
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
```

这三个函数是位图的原子操作，放在匿名命名空间里限制为文件内部可见。逻辑完全对称：把页号分解为字节索引和位偏移，然后用位掩码做 SET/CLEAR/TEST 操作。`1U << bit_idx` 构造一个只有目标位为 1 的掩码，OR 操作设置该位，AND + 取反清除该位，AND 测试该位。

这些操作的时间复杂度都是 O(1)。位运算在硬件层面只需要一次内存访问和一个 ALU 操作，非常高效。

### find_first_free — 第一个空闲页的扫描

```cpp
int64_t find_first_free() {
    for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
        if (s_bitmap[byte_idx] != 0xFF) {
            uint8_t byte = s_bitmap[byte_idx];
            for (uint64_t bit_idx = 0; bit_idx < 8; bit_idx++) {
                if ((byte & (1U << bit_idx)) == 0) {
                    return static_cast<int64_t>(byte_idx * 8 + bit_idx);
                }
            }
        }
    }
    return -1;
}
```

这是整个 PMM 中最慢的操作——O(N) 扫描。外层循环遍历位图的每个字节，先检查字节是否等于 `0xFF`（全 1 = 8 个页全部已用）。如果不是全 1，说明至少有一个空闲位，内层循环逐位检查找到第一个 0。

实现是朴素的，没有使用任何优化技巧。一个明显的改进是使用 `__builtin_ctzll`（Count Trailing Zeros）指令——它可以一次找到 64 位中最低的 0 位对应的索引，一个 64 位字一次就能跳过 64 个页的检查。但对 128KB 的位图来说，朴素扫描在现代 CPU 上也就几微秒的事，对于教学内核来说完全可以接受。

### 区域标记函数

```cpp
void mark_region_used(uint64_t phys, uint64_t length) {
    uint64_t start_page = phys / PAGE_SIZE;
    uint64_t end_page   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t page = start_page; page < end_page; page++) {
        if (page < MAX_PAGES && !test_bit(page)) {
            set_bit(page);
            s_free_pages--;
        }
    }
}

void mark_region_free(uint64_t phys, uint64_t length) {
    uint64_t start_page = phys / PAGE_SIZE;
    uint64_t end_page   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t page = start_page; page < end_page; page++) {
        if (page < MAX_PAGES) {
            clear_bit(page);
            s_free_pages++;
        }
    }
}
```

这两个函数负责批量标记一个连续物理地址范围内的所有页。物理地址转页号的公式是 `phys / PAGE_SIZE`，结束页号的计算用了向上取整的整数除法：`(phys + length + PAGE_SIZE - 1) / PAGE_SIZE`。

注意两者在边界检查上的差异：`mark_region_used` 会检查 `!test_bit(page)` 防止重复标记（避免空闲计数变成负数），而 `mark_region_free` 不检查这个——它在初始化阶段使用，此时保守策略保证所有页都是已用状态，所以不需要防重复。不过这意味着如果你在运行时误用它来释放一个已经空闲的区域，空闲计数会虚高。

### 初始化流程

```cpp
void init(const void* boot_info) {
    const BootInfo* info = static_cast<const BootInfo*>(boot_info);

    // Step 1: Initialize bitmap - mark all pages as used
    for (uint64_t i = 0; i < BITMAP_SIZE; i++) {
        s_bitmap[i] = 0xFF;
    }
    s_total_pages  = 0;
    s_free_pages   = 0;
    s_highest_page = 0;
```

初始化的第一步把整个 128KB 位图填充为 `0xFF`——每个 bit 都是 1，意味着所有页都被标记为已用。计数器全部归零。这是保守策略的起点：在最坏情况下，即使 E820 解析出了问题，PMM 也不会错误地把保留内存分配出去。

```cpp
    // Step 2: Parse E820 memory map
    for (uint32_t i = 0; i < info->mmap_count; i++) {
        const MemoryMapEntry* entry = &info->mmap[i];

        if (entry->type != 1) {
            continue;
        }

        uint64_t base   = entry->base;
        uint64_t length = entry->length;

        // Update highest page
        uint64_t end_page = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
        if (end_page > s_highest_page) {
            s_highest_page = end_page;
            if (s_highest_page > MAX_PAGES) {
                s_highest_page = MAX_PAGES;
            }
        }

        // Filter out low 1MB
        if (base < LOW_MEMORY_BOUNDARY) {
            if (length <= LOW_MEMORY_BOUNDARY - base) {
                continue;
            }
            length -= (LOW_MEMORY_BOUNDARY - base);
            base = LOW_MEMORY_BOUNDARY;
        }

        // Align to page boundaries
        uint64_t aligned_base   = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t aligned_length = length - (aligned_base - base);

        if (aligned_length < PAGE_SIZE) {
            continue;
        }

        mark_region_free(aligned_base, aligned_length);
    }

    s_total_pages = s_highest_page;
```

第二步是 E820 解析的主体。对每个 type=1 的可用内存条目，依次做以下处理：更新最高页索引（带 `MAX_PAGES` 上限检查）、过滤低 1MB（处理完全在 1MB 以下和跨越 1MB 边界两种情况）、4KB 对齐、长度检查、最后标记为空闲。

低 1MB 过滤的跨边界处理值得仔细看。如果 `base < 1MB` 且 `length > 1MB - base`（区域延伸到了 1MB 以上），我们就把 base 调到 1MB，同时缩短 length。数学上是：新 length = 原 length - (1MB - 原 base)。这样 1MB 以下的部分被截掉，1MB 以上的部分被保留。

对齐处理用了手写的位运算而不是前面定义的 `align_up` 函数，这在实际项目中有点不一致，但逻辑是等价的。

```cpp
    // Step 3: Mark kernel region as used
    uint64_t kernel_phys = info->kernel_phys_base;
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
    lib::kprintf("[MINI] PMM: kernel_phys=0x%x, kernel_size=0x%x (%u pages)\n",
                 kernel_phys, kernel_size, (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE);
    mark_region_used(kernel_phys, kernel_size);

    // Step 4: Mark bootloader regions as used
    lib::kprintf("[MINI] PMM: marking bootloader 0x0-0x10000 used (%u pages)\n",
                 0x10000 / PAGE_SIZE);
    mark_region_used(0x0, 0x10000);

    lib::kprintf("[MINI] PMM: Total %u pages (%u MB), Free %u pages (%u MB)\n",
                 s_total_pages, (s_total_pages * PAGE_SIZE) / 1_MB,
                 s_free_pages, (s_free_pages * PAGE_SIZE) / 1_MB);
}
```

第三步和第四步把不应该被分配的区域重新标为已用。内核自身的物理范围由 `kernel_phys_base`（BootInfo 中的加载地址）和 `__kernel_size`（链接器符号，取地址获得）确定。bootloader 占用的 0x0-0x10000 区域也需要标记。注意这里的 `mark_region_used` 内部会检查 `!test_bit(page)`——由于低 1MB 已经在第二步被过滤掉了，而内核基地址 `0x20000` 也在 1MB 以下，所以这些页本来就在"已用"状态，`mark_region_used` 实际上不会改变它们的状态。但 bootloader 区域（0x0-0x10000）的低地址部分可能需要显式标记，因为某些 E820 条目可能在第二步中被标记为空闲（虽然低 1MB 过滤应该已经处理了这种情况）。

`reinterpret_cast<uint64_t>(&__kernel_size)` 就是链接器符号正确访问的核心写法。`&` 取地址得到链接器赋予的值，`reinterpret_cast` 转为整数类型。

### 页分配与释放

```cpp
uint64_t alloc_page() {
    int64_t page_idx = find_first_free();
    if (page_idx < 0) {
        return 0;
    }

    set_bit(static_cast<uint64_t>(page_idx));
    s_free_pages--;

    return static_cast<uint64_t>(page_idx) * PAGE_SIZE;
}
```

分配逻辑直截了当：找第一个空闲页，标记为已用，递减计数，返回物理地址。`find_first_free` 返回 -1 时表示没有空闲页了，此时返回 0 作为 OOM 信号。返回的物理地址天然是 4KB 对齐的——因为 `page_idx * 4096` 的结果低 12 位永远是 0。

```cpp
void free_page(uint64_t phys) {
    if (phys == 0) {
        return;
    }

    uint64_t page_idx = phys / PAGE_SIZE;
    if (page_idx >= MAX_PAGES) {
        return;
    }

    if (test_bit(page_idx)) {
        clear_bit(page_idx);
        s_free_pages++;
    }
}
```

释放函数有三道防线：空地址直接忽略、越界地址直接忽略、非已用状态不做操作。第三道防线 `test_bit(page_idx)` 是防止双重释放的关键——如果你释放一个已经空闲的页，`test_bit` 返回 false，整个 if 不执行，计数器不会虚增。虽然不报错也不告警（内核里没有日志基础设施来做这件事），但至少保证了数据一致性。

### 统计查询

```cpp
uint64_t free_page_count() {
    return s_free_pages;
}

uint64_t total_page_count() {
    return s_total_pages;
}
```

两个 getter 函数，直接返回内部计数器。没有加锁——因为目前的 mini kernel 是单 CPU 运行的，不存在并发问题。将来引入 SMP 支持时，这些函数（以及所有修改位图的操作）都需要加自旋锁保护。

---

## 设计决策

### 决策：保守初始化 vs 乐观初始化

**问题**：位图初始状态应该全部标记为空闲（乐观）还是全部标记为已用（保守）？

**本项目的做法**：保守初始化——先全部标记已用（`0xFF`），然后只释放 E820 确认为可用的区域。

**备选方案**：乐观初始化——先全部标记为空闲（`0x00`），然后标记已知保留区域为已用。

**为什么不选备选方案**：乐观策略的风险在于你必须完整列举所有"不可用"区域——内核、bootloader、位图本身、BIOS 区域、MMIO 区域。如果漏掉了某个保留区域（比如某个 BIOS 保留的 MMIO 段），PMM 就会把它当成可用内存分配出去，导致不可预测的硬件行为。保守策略只需要列举"可用"区域，而 E820 已经帮你做了这件事——type=1 的条目就是 BIOS 确认可用的内存，漏掉最多是少用一些内存，不会出硬件故障。

**如果要扩展/改进**：在标记内核自身占用时，当前代码没有标记位图数组本身的占用。因为位图是内核 BSS 段的一部分（静态数组），而 `__kernel_size` 已经包含了 BSS 段的大小，所以实际上是覆盖了的。但如果将来位图改为动态分配，就需要显式标记位图占用的页。

### 决策：朴素扫描 vs 优化扫描

**问题**：`find_first_free` 应该用朴素逐位扫描还是用位操作优化？

**本项目的做法**：朴素的逐字节-逐位扫描。

**备选方案**：使用 `__builtin_ctzll`（GCC/Clang 内建函数）做 64 位字级别的快速扫描，或者缓存上一次分配位置避免从头扫。

**为什么不选备选方案**：对于 128KB 位图，朴素扫描在最坏情况下需要检查 128K 个字节，现代 CPU 的 L1 cache（通常 32-64KB）可以装下一大半位图，扫描速度极快。优化扫描带来的性能提升在这个规模下微不足道，反而增加了代码复杂度。教学内核应该优先保证代码易读。

**如果要扩展/改进**：如果将来管理范围扩展到 64GB 或更大（位图 2MB+），可以考虑添加位置缓存——记录上一次分配的位号，下次从该位置继续扫描。Linux 的 bootmem 就用了类似的优化。更进一步的优化是用 `__builtin_ctzll` 一次跳过 64 位。

---

## 扩展方向

- **位图位置缓存**：在 `alloc_page` 中记录上次分配位置，下次从该位置继续扫描，难度低
- **`__builtin_ctzll` 优化**：把 `find_first_free` 改为按 64 位字扫描，使用 CTZ 指令快速定位空闲位，难度中等
- **连续多页分配**：添加 `alloc_pages(count)` 接口，在位图中查找连续 N 个空闲位，难度中等
- **页状态查询接口**：添加 `is_page_used(phys)` 对外暴露 `test_bit` 的功能，难度低

---

## 参考资料

- Intel SDM: Vol.3A Chapter 3 — 物理地址空间布局，前 1MB 遗留区域（IVT/BDA/Video/BIOS）
- Intel SDM: Vol.3A Section 4.5 — 4KB 页对齐要求，bit 11:0 必须为零
- OSDev Wiki: [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) — E820 INT 15h 接口详细说明
- OSDev Wiki: [Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — 位图/栈/伙伴系统对比
