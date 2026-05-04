# 015-1 从 E820 到位图：让内核看清物理内存的家底

> 标签：PMM, E820, 位图分配器, 物理内存管理
> 前置：[014 键盘驱动](014-driver-keyboard-1.md)

## 前言

到 tag 014 为止，我们的内核已经是一个"能跑能说话"的小家伙了——串口能打印、屏幕能显示、键盘能输入。但如果你问它一个最基本的问题："这台机器到底有多少物理内存？"它只能一脸茫然。我们的内核从第一天起就一直在"裸奔"——没有任何动态内存管理能力，所有空间要么是编译期静态分配的，要么是 bootloader 预留的。从这一章开始，我们要给内核装上一套完整的"记账系统"，让它知道自己有多少家底，哪些能花、哪些不能动。

这一章是整个内存管理子系统的地基。我们要做的第一件事就是搞清楚物理内存的布局——通过解析 BIOS E820 调用留下的数据，建立一张"内存地图"，然后基于这张地图设计一个位图分配器的数据结构。PMM 是后续所有内存管理的基础：没有它，VMM（虚拟内存管理器）无法分配页表页面，堆分配器无法获得内存池，进程地址空间无从谈起。

## 环境说明

- 平台：x86_64, QEMU 默认配置（128MB 内存）
- 工具链：GCC 13+ cross-compiler, CMake + Ninja
- 内核模式：higher-half（VMA = 0xFFFFFFFF80000000）
- 特殊约束：内核禁用标准库和异常，所有内存管理代码在 freestanding 环境下运行

## E820：BIOS 给我们的"财产清单"

在 x86 PC 架构中，物理内存远不是"从 0 开始有一大块连续 RAM"那么简单。真实的物理地址空间充满了各种固定用途的区域——中断向量表在 0x00000000、BIOS 数据区在 0x00000400、VGA 文本缓冲区在 0x000B8000、BIOS ROM 在 0x000F0000，到了高地址还有 PCI MMIO 窗口、ACPI 表等等。BIOS 通过 INT 15h AX=0xE820 这个接口，把这些信息整理成一份"财产清单"交给操作系统。

Cinux 的 bootloader 在实模式阶段已经调用了 E820，把结果填入了 BootInfo 结构体的 `mmap[]` 数组中——最多 32 条记录，每条包含 base（起始地址）、length（长度）、type（类型）和 acpi（ACPI 属性）四个字段。我们最关心的是 type=1 的条目，它表示"可用 RAM"——这才是内核可以自由使用的物理内存。

现在我们来看 `parse_memory_map` 是如何把 E820 的原始数据转换成内核可以直接使用的干净数据结构的：

```cpp
uint32_t parse_memory_map(const BootInfo& info,
                          MemoryRegion* regions,
                          uint32_t max_regions) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < info.mmap_count && count < max_regions; i++) {
        const auto& entry = info.mmap[i];
        if (entry.type != 1) continue;
```

函数签名接收三个参数：BootInfo 引用（数据源）、MemoryRegion 输出数组（结果）和数组容量。返回值是实际解析出的区域数量。循环遍历 BootInfo 中的所有 E820 条目，第一道关卡就是类型过滤——只放行 type == 1（可用 RAM）的条目，其他值一律跳过。

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

第二道关卡是低内存过滤。`LOW_MEM_BOUNDARY` 设为 0x100000（1MB），Intel SDM Vol.3A Chapter 3 详细描述了第一兆字节的布局——那里充满了 IVT、BDA、EBDA 等固件数据结构，内核不应该碰。如果区域完全在 1MB 以下，直接跳过；如果跨越 1MB 边界，截断到 1MB 起始。

```cpp
        // Align base up, length down to 4 KB
        uint64_t aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        length -= (aligned_base - base);
        length &= ~(PAGE_SIZE - 1);

        if (length < PAGE_SIZE) continue;
        regions[count++] = {aligned_base, length};
    }

    return count;
}
```

第三和第四道关卡是 4KB 对齐。Intel SDM Vol.3A Section 4.5 明确规定 long mode 的页表项中物理基地址必须是 4KB 对齐的（低 12 位用作标志位）。所以我们的 PMM 也必须以 4KB 为最小粒度——基地址向上对齐，长度向下截断，不足一页的区域直接丢弃。

这四道关卡看似简单，但它们共同保证了输出数组的每一条记录都是"干净"的——type 正确、地址在 1MB 以上、4KB 对齐、长度至少一页。后续的 init 流程可以放心地使用这些数据，不需要再做任何额外的边界处理。

## 位图：一人一页的"签到簿"

有了干净的内存区域列表，接下来需要一个数据结构来跟踪每一页的状态。我们选择了最直观的方案——位图（bitmap）：物理内存中的每一页（4KB）对应位图中的一个 bit，1 表示已占用，0 表示空闲。

位图的四个基础操作极其简洁。`bm_set` 和 `bm_clear` 分别把对应 bit 置 1 和清 0，`bm_test` 检查 bit 是否为 1——这三个都是 O(1) 的单次数组访问加一次位运算。

真正有意思的是 `bm_find_first_free`——线性扫描位图找第一个空闲 bit。如果我们逐 bit 扫描，最坏情况下需要检查位图中的每一个 bit，在 128MB 内存的情况下就是 32768 个 bit，虽然也不算慢，但我们可以做得更好。Cinux 的做法是把位图数组重新解释为 `uint64_t` 数组，每次检查 64 个 bit：

```cpp
int64_t bm_find_first_free(const uint8_t* bm, uint64_t highest_page,
                           uint64_t bitmap_size) {
    const auto* bm64 = reinterpret_cast<const uint64_t*>(bm);
    uint64_t qword_count = bitmap_size / sizeof(uint64_t);

    for (uint64_t i = 0; i < qword_count; i++) {
        if (bm64[i] != ~0ULL) {
            int bit = __builtin_ctzll(~bm64[i]);
            uint64_t idx = i * 64 + static_cast<uint64_t>(bit);
            if (idx < highest_page) return static_cast<int64_t>(idx);
        }
    }
    // ... tail bytes handling ...
    return -1;
}
```

核心技巧是 `__builtin_ctzll(~bm64[i])`——先对 64 位值取反（把 0 变成 1、1 变成 0），然后用 `ctzll`（count trailing zeros）找到最低位的 1 在哪个位置。编译器会把 `ctzll` 编译为 x86 的 BSF 指令，只需要一个时钟周期。这样一次操作就检查了 64 页，扫描速度最多提升 64 倍。

## xv6 做了什么不同？

说了这么多位图的实现，我们来看看别的 OS 是怎么做的。xv6 用了一种完全不同的方案——链表（free list）。每个空闲页的前几个字节被当作一个 `struct run { struct run *next; }` 指针，所有空闲页串成一个单向链表。分配就是从链表头 pop 一个节点，释放就是 push 回去——O(1) 的时间复杂度，零额外内存开销。

这个设计非常巧妙：不需要额外的位图或者数组来跟踪页状态，空闲页本身就是元数据的载体。但它有一个致命的缺点——无法回答"页 X 是否已分配？"。如果你想检查某个物理页是不是被 double-free 了，你必须遍历整个链表。在 xv6 这种小规模系统里这不是问题，但在一个有上百万页的生产系统中，这个操作是不可接受的。

位图方案正好相反——查询是 O(1) 的，但分配是 O(N) 的。不过通过 64 位扫描加速，实际开销已经被大幅降低。而且位图的内存开销很小：128MB 内存只需要 4KB 的位图（恰好一个页），即使 4GB 内存也才 128KB。

## Linux 的选择

Linux 的早期启动内存管理经历了一个有趣的演变过程。2.3 之前的版本用最原始的"指针碰撞"分配器——维护一个指针，每次分配就把指针往后移，释放就不管了。2.3.23pre3 引入了 bootmem 位图分配器——和 Cinux 的原理几乎一模一样：一个 bit 对应一个页，保守初始化后按 E820 解锁。bootmem 的设计者选择位图的原因和我们一样——启动阶段代码需要简洁可靠，位图是最不容易出错的方案。

从 4.17 开始，Linux 用 memblock 替代了 bootmem——memblock 是一个基于区段的分配器，不再用位图而是用"保留区域列表"和"可用区域列表"来管理内存。启动完成后，Linux 切换到 buddy 分配器作为运行时的物理内存管理器。Cinux 的 PMM 在概念上就对应 Linux 的 bootmem 阶段——简洁的位图分配器，为后续更复杂的管理器铺路。

## 到这里

这一章我们搞定了两件事：从 E820 原始数据中提取干净的可用内存区域，以及设计了位图的核心操作函数。下一章要把这些零件组装成一套完整的初始化流程——位图放在哪里、如何保护内核占用的内存、如何处理 higher-half 地址转换。

## 参考资料

- Intel SDM: Vol.3A Chapter 3 — 物理地址空间布局
- Intel SDM: Vol.3A Section 4.5 — 4KB 页对齐要求
- [OSDev Wiki - Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) — E820 接口详情
- [OSDev Wiki - Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — bitmap/stack/buddy 方案比较
- xv6 `kalloc.c`: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c) — 链表分配器
- Linux bootmem: [LWN](https://lwn.net/Articles/761215/) — bootmem 到 memblock 的演变
