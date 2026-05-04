# 015-3 分配、释放与测试：位图分配器的运行时

> 标签：PMM, alloc_page, free_page, alloc_pages, 双轨测试
> 前置：[015-2 PMM 初始化](015-mm-pmm-2.md)

## 前言

前两章我们把 PMM 的数据结构和初始化流程搭建完毕——位图就位了，内核区域被保护了，E820 可用区域被正确解锁了。现在位图里躺着成千上万个 0 bit（空闲页），等着被分配出去。这一章要做的就是实现"借出"和"归还"的运行时接口，然后通过双轨测试来验证整个 PMM 的正确性。

说实话，分配和释放的逻辑比初始化简单多了——毕竟不需要处理地址转换和链接器符号这些"环境问题"。但这里有自己的坑：有符号/无符号混合使用、double-free 的防御、OOM 的处理。我们会一一拆解。

## 环境说明

- QEMU 默认配置（128MB 内存），E820 通常报告 1-2 个可用区域
- PMM 管理约 32000 个物理页，空闲约 31000 页
- 测试双轨：host 端（Linux 用户空间）+ QEMU in-kernel

## 单页分配：扫描、标记、返回

`alloc_page` 的工作极其直观——调用 `bm_find_first_free` 找到第一个空闲 bit，把它置为 1，返回物理地址。如果位图全满就返回 0 作为 OOM 标志。

```cpp
uint64_t PMM::alloc_page() {
    int64_t idx = bm_find_first_free(bitmap_, highest_page_, bitmap_size_);
    if (idx < 0) return 0;

    bm_set(bitmap_, static_cast<uint64_t>(idx));
    free_pages_--;
    return static_cast<uint64_t>(idx) * PAGE_SIZE;
}
```

这里有一个必须注意的类型转换陷阱。`bm_find_first_free` 返回 `int64_t`——用 -1 表示"没找到"。在把 `idx` 传给 `bm_set` 之前，必须先检查 `idx < 0`。如果你不小心写了 `uint64_t idx = bm_find_first_free(...)` 然后直接用，-1 被强转为 `uint64_t` 后变成 `0xFFFFFFFFFFFFFFFF`，`bm_set` 试图设置位图在偏移 `0xFFFFFFFFFFFFFFFF` 处的 bit——这会写到离位图很远很远的内存位置，大概率 triple fault。

物理地址 0 被用作 OOM 标志。这是一个很自然的选择：物理地址 0 是 IVT 的位置，它永远不应该被分配给普通用途。我们的 PMM 管理的物理内存从 1MB 开始，所以物理地址 0 根本不在管理范围内。调用者只需要 `if (addr != 0)` 就能区分成功和 OOM。

## 带防御的页释放

`free_page` 实现了三层防御，每一层对应一种调用者可能犯的错误：

```cpp
void PMM::free_page(uint64_t phys) {
    if (phys == 0) return;                     // 防御 1: OOM 标志
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= highest_page_) return;           // 防御 2: 越界
    if (!bm_test(bitmap_, idx)) return;         // 防御 3: double-free

    bm_clear(bitmap_, idx);
    free_pages_++;
}
```

第一层拦截对 OOM 返回值的错误释放。第二层拦截超出 PMM 管理范围的地址（比如 MMIO 地址）。第三层是防止 double-free 的关键——检查目标 bit 是否确实为 1（已用），如果不是 1 说明这个页已经是空闲的了。

这三层防御保证了即使调用者犯了错误，PMM 的内部状态也不会被破坏。这是一种"宽容接收、严格管理"的设计哲学——在一个教学 OS 中，内核不会因为一个无害的编程错误就崩溃，但 `free_pages_` 计数始终保持精确。

## 连续多页分配：First-Fit 扫描

有些场景需要连续的物理页——比如大页映射或者 DMA 缓冲区。`alloc_pages(count)` 用 first-fit 策略扫描位图：

```cpp
uint64_t PMM::alloc_pages(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return alloc_page();  // 快速路径！

    uint64_t run   = 0;
    uint64_t start = 0;

    for (uint64_t p = 0; p < highest_page_; p++) {
        if (!bm_test(bitmap_, p)) {
            if (run == 0) start = p;
            run++;
            if (run >= count) {
                for (uint64_t i = start; i < start + count; i++) {
                    bm_set(bitmap_, i);
                }
                free_pages_ -= count;
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }

    return 0;
}
```

前两行是一个重要的优化——`count == 1` 直接走 `alloc_page` 的 64 位加速路径，不走 first-fit 的逐 bit 扫描。first-fit 的核心是两个变量：`run` 记录连续空闲页数量，`start` 记录连续区域起始。遇到空闲页 `run++`，遇到已用页 `run` 归零。当 `run` 达到 `count` 时找到了足够长的连续区域。

最坏时间复杂度是 O(N*M)，但对于 QEMU 默认的 128MB（32000 页）来说，即使最坏情况也只需要几万次位运算。如果将来需要更高效的连续分配，应该考虑 buddy 分配器。

## 双轨测试策略

Cinux 的 PMM 测试分两条轨道，互补覆盖不同层面的正确性。

### Host 端单元测试

Host 端测试（`test/unit/test_pmm.cpp`，345 行）在 Linux 用户空间编译运行，迭代速度快。它重新实现了一个 `TestPMM` 类来复制 PMM 的核心算法，覆盖了 12 个测试场景：

E820 解析测试覆盖了 5 个边界情况：类型过滤（只保留 type=1）、低内存过滤、跨边界截断、4KB 对齐、过小区域丢弃。位图分配器测试覆盖了 7 个场景：单页分配返回对齐地址、1000 次循环后计数恢复、OOM 返回 0、`free_page(0)` 是 no-op、double-free 是 no-op、连续 4 页分配、碎片化内存上连续分配失败。

碎片化测试特别有趣——它构造了一个"棋盘"位图（偶数页空闲、奇数页已用），然后尝试分配 2 个连续页。因为没有任何两个相邻的空闲页，所以 `alloc_pages(2)` 应该返回 0。这种极端场景在 QEMU 的真实 E820 映射中几乎不可能复现，但 host 端测试可以轻松构造。

### QEMU in-kernel 集成测试

QEMU 测试（`kernel/test/test_pmm.cpp`，144 行）使用真实的 BootInfo 数据和真正的位图，验证端到端的正确性。6 个测试用例覆盖了 PMM 最容易出错的三个方面：初始化统计、分配释放循环的计数一致性、以及 `free_page(0)` 和 double-free 的边界条件。

## xv6 和 Linux 的对比

xv6 的 `kalloc.c` 用链表实现 O(1) 分配释放——`kalloc` 从链表头 pop 一个空闲页，`kfree` push 回去。但它不支持连续分配——没有 `alloc_pages` 的等价物。在 xv6 的设计中，如果需要连续物理内存，只能在初始化阶段分配（因为初始时所有空闲页是连续的）。Cinux 的 first-fit 虽然性能不如链表，但至少支持了运行时的连续分配。

Linux 的 buddy 分配器是这三种方案中最强大的——支持 O(log N) 的连续分配和释放，通过 buddy 合并自动消除外部碎片。但实现复杂度远高于位图——SerenityOS 的 `PhysicalZone` 有上千行代码，而 Cinux 的整个 PMM 只有 267 行。对于教学 OS 来说，位图的简洁性比 buddy 的高效性更有价值。

## 收尾

现在 PMM 已经完全可用了——从 E820 解析到位图初始化，从单页分配到连续分配，从防御性释放到双轨测试，所有功能都已实现并验证。在 QEMU 中启动内核，你会看到 `[PMM] Total: 128MB, Free: 126MB`，随后内核继续正常初始化 Framebuffer、Console 和 Keyboard。

下一站是 VMM（虚拟内存管理器）——它需要 PMM 来分配页表页面，然后构建完整的四级页表映射。

## 参考资料

- Intel SDM: Vol.3A Section 4.4-4.5 — PAE 和 4KB 页对齐
- [OSDev Wiki - Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — bitmap/stack/buddy 方案比较
- xv6 `kalloc.c`: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c) — 链表分配器，零额外内存开销
- Linux bootmem: [LWN](https://lwn.net/Articles/761215/) — bootmem 到 memblock 的演变
- SerenityOS `PhysicalZone.h`: [GitHub](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/PhysicalZone.h) — 生产级 buddy 分配器
