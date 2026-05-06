# 015-3 通读：分配、释放与测试——位图分配器的运行时

## 概览

本文是 tag `015_mm_pmm` 三篇通读教程的第三篇，聚焦于 PMM 的运行时接口（`alloc_page`/`free_page`/`alloc_pages`/`free_pages`）和双轨测试策略。前两篇已经把位图初始化完毕——数以万计的空闲页躺在位图里等着被分配出去。现在我们要实现"借出"和"归还"的逻辑，然后通过两套测试来验证整个 PMM 的正确性。

PMM 提供两套分配 API：单页分配利用上一篇实现的 64 位加速扫描，时间复杂度从 O(N) 降到 O(N/64)；连续多页分配使用 first-fit 线性扫描，总时间复杂度为 O(N)（外层循环扫描 N 个页，内层循环最多执行一次标记 M 个页后返回，O(N+M) = O(N)）。测试方面，Cinux 采用了"双轨"策略：host 端单元测试覆盖算法边界情况，QEMU in-kernel 集成测试验证真实环境下的端到端行为。

## 数据流图

```
    alloc_page()                              free_page(phys)
    ┌─────────────────┐                       ┌─────────────────┐
    │ bm_find_first_  │                       │ idx = phys/4096  │
    │ free() 扫描位图  │                       │                  │
    │ (64-bit 加速)   │                       │ if idx >= max:   │
    └────────┬────────┘                       │   return (越界)  │
             │                                │                  │
        ┌────┴────┐                           │ if !bm_test:     │
        │ idx < 0 │──是──► return 0 (OOM)     │   return (双重释放)│
        └────┬────┘                           │                  │
             │ 否                             │ bm_clear(idx)    │
     bm_set(idx)                              │ free_pages_++    │
     free_pages_--                            └─────────────────┘
     return idx * 4096

    alloc_pages(count)
    ┌──────────────────────────────────────────────────────────┐
    │ if count == 0: return 0                                  │
    │ if count == 1: return alloc_page()  ← 快速路径           │
    │                                                          │
    │ for p = 0 .. highest_page_:                              │
    │   if 页 p 空闲:                                          │
    │     if run == 0: start = p                               │
    │     run++                                                │
    │     if run == count:                                     │
    │       bm_set(start..start+count-1)                       │
    │       free_pages_ -= count                               │
    │       return start * 4096                                │
    │   else: run = 0                                          │
    │                                                          │
    │ return 0 (OOM)                                           │
    └──────────────────────────────────────────────────────────┘
```

## 代码精讲

### alloc_page：单页分配

```cpp
uint64_t PMM::alloc_page() {
    int64_t idx = bm_find_first_free(bitmap_, highest_page_, bitmap_size_);
    if (idx < 0) return 0;

    bm_set(bitmap_, static_cast<uint64_t>(idx));
    free_pages_--;
    return static_cast<uint64_t>(idx) * PAGE_SIZE;
}
```

`alloc_page` 的实现极其简洁——三部曲：找到空闲页、标记为已用、返回物理地址。`bm_find_first_free` 上一篇已经详细讲过：它用 64 位整数一次扫描 64 个 bit，找到第一个值为 0 的 bit 后返回其索引；如果位图全满则返回 -1。

这里有一个类型转换的细节需要注意。`bm_find_first_free` 返回 `int64_t`——用负数表示"没找到"是 C 语言的传统约定。但后续的 `bm_set` 需要一个 `uint64_t` 的索引。在转换之前必须先检查 `idx < 0`，否则 -1 被强转为 `uint64_t` 后变成 `0xFFFFFFFFFFFFFFFF`（一个极大的正数），`bm_set` 试图设置位图在偏移 0xFFFFFFFFFFFFFFFF 处的 bit，大概率访问到完全无关的内存区域，导致 triple fault。

返回的物理地址是 `idx * PAGE_SIZE`——因为位图中第 N 个 bit 对应的就是第 N 个物理页，物理地址就是 N * 4096。0 被用作 OOM 标志：物理地址 0 是 IVT 的位置，永远不会被 PMM 分配（1MB 以下的区域在 `parse_memory_map` 中就被过滤掉了），所以调用者可以安全地用 `if (addr != 0)` 来区分成功和 OOM。

### free_page：带防御的页释放

```cpp
void PMM::free_page(uint64_t phys) {
    if (phys == 0) return;
    uint64_t idx = phys / PAGE_SIZE;
    if (idx >= highest_page_) return;
    if (!bm_test(bitmap_, idx)) return;

    bm_clear(bitmap_, idx);
    free_pages_++;
}
```

`free_page` 实现了三层防御，每一层都对应一种调用者可能犯的错误。第一层：`phys == 0` 直接返回——物理地址 0 是 OOM 标志，不是一个有效的物理页。如果调用者错误地对 `alloc_page` 的 OOM 返回值调用了 `free_page`，这层防御就会拦截。

第二层：`idx >= highest_page_` 直接返回——传入的物理地址可能超出了 PMM 管理的范围。比如调用者传入了一个 MMIO 地址（通常在高地址区域）或者一个计算错误的地址，这层防御会拦截。

第三层：`!bm_test(bitmap_, idx)` 直接返回——目标页已经是空闲的了。这是防止 double-free 的关键检查。如果没有这层，连续两次 `free_page` 同一个地址会导致 `free_pages_` 被多加一次，PMM 的空闲页计数就会超过实际数量，后续的分配可能会返回一个"幽灵页"（bit 为 0 但实际已被使用）。

这三层防御共同保证了 `free_pages_` 计数始终精确。在一个教学 OS 中，"宽容接收、严格管理"的设计哲学远比 `panic` 或者 `assert` 实用——内核不会因为一个无害的编程错误就崩溃，但内部状态始终保持一致。

### alloc_pages：连续多页分配

```cpp
uint64_t PMM::alloc_pages(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return alloc_page();

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

`alloc_pages` 的前两行是一个值得注意的优化：`count == 0` 返回 0（没有意义的请求），`count == 1` 直接委托给 `alloc_page`。后者利用了 `bm_find_first_free` 的 64 位加速扫描——一次检查 64 个 bit，速度远快于下面逐 bit 扫描的 first-fit 算法。如果你忘记了这个优化，`alloc_pages(1)` 也会走 first-fit，结果正确但性能差了最多 64 倍。

first-fit 算法的核心是两个变量：`run` 记录当前连续空闲页的数量，`start` 记录当前连续空闲区域的起始页号。扫描逻辑很直观——遇到空闲页时 `run++`，如果 `run` 刚从 0 变成 1 就顺便记录 `start`；遇到已用页时 `run` 归零。当 `run` 达到请求的 `count` 时，从 `start` 开始逐页标记为已用，减少 `free_pages_` 计数，返回起始物理地址。

这个算法的最坏时间复杂度是 O(N)（N 是总页数；外层循环执行 N 次 bm_test，内层循环最多执行一次标记 M 个页后返回，M 不超过 N，所以 O(N+M) = O(N)）。对于 QEMU 默认的 128MB 内存（32768 页），即使是最坏情况也只需要几万次位运算，延迟完全可以忽略。但如果将来需要管理更大的物理内存（比如 4GB = 100 万页），就应该考虑 buddy 分配器——SerenityOS 的 `PhysicalZone` 就是用 13 级 buddy 来实现 O(log N) 的连续分配。

### free_pages：批量释放

```cpp
void PMM::free_pages(uint64_t phys, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) {
        free_page(phys + i * PAGE_SIZE);
    }
}
```

`free_pages` 的实现极其简单——一个循环，对每一页调用 `free_page`。由于 `free_page` 自带三层防御，所以即使传入的范围中某些页本来就是空闲的（比如调用者错误地释放了从未分配的页），也不会破坏 PMM 的内部状态——那些页会被静默跳过。

### 统计方法

```cpp
uint64_t PMM::free_page_count() const { return free_pages_; }
uint64_t PMM::total_page_count() const { return total_pages_; }
```

两个纯查询方法，直接返回成员变量的值。在当前的单核内核中不需要加锁——没有并发访问的问题。但在调试和监控中它们非常有用：几乎所有的测试断言都以 `free_page_count()` 作为"操作是否生效"的判据。

### QEMU in-kernel 集成测试

```cpp
// test_pmm.cpp — 在 QEMU 内核环境中运行
extern "C" void run_pmm_tests() {
    TEST_SECTION("PMM Tests (015)");

    RUN_TEST(test_pmm_init::test_init_and_stats);
    RUN_TEST(test_pmm_alloc::test_alloc_free_cycle);
    RUN_TEST(test_pmm_bulk::test_bulk_alloc_free);
    RUN_TEST(test_pmm_contiguous::test_alloc_pages_contiguous);
    RUN_TEST(test_pmm_edge::test_free_zero_noop);
    RUN_TEST(test_pmm_edge::test_double_free_noop);

    TEST_SUMMARY();
}
```

QEMU 集成测试有 6 个用例，全部在真实的内核环境中运行（使用真实的 BootInfo 数据和真正的位图）。第一个 `test_init_and_stats` 验证初始化后的基本统计——total > 0、free > 0、free <= total。第二个 `test_alloc_free_cycle` 验证单页的分配释放循环——分配后 free 减少 1，释放后恢复。第三个 `test_bulk_alloc_free` 批量分配 16 页后全部释放，验证计数恢复。第四个 `test_alloc_pages_contiguous` 分配 4 个连续页并验证计数。第五和第六个测试验证 `free_page(0)` 和 double-free 都是 no-op。

这些测试虽然简单，但覆盖了 PMM 最容易出错的三个方面：OOM 处理、计数一致性和边界条件。

### Host 端单元测试

Host 端测试（`test/unit/test_pmm.cpp`，345 行）在 Linux 用户空间编译运行，不依赖任何内核代码。它重新实现了一个 `TestPMM` 类来复制 PMM 的核心算法，并覆盖了更多边界情况。

E820 解析部分有 5 个测试：类型过滤（只保留 type=1）、低内存过滤（低于 1MB 的区域被跳过）、跨边界截断（从 0x80000 到 0x280000 被截断为 0x100000 到 0x280000）、4KB 对齐（base 0x100123 向上对齐到 0x101000）、过小区域丢弃（对齐后不足一页则丢弃）。

位图分配器部分有 7 个测试：单页分配返回对齐地址、1000 次循环后计数恢复、OOM 返回 0、`free_page(0)` 是 no-op、double-free 是 no-op、连续 4 页分配、碎片化内存上连续分配失败（棋盘模式）。

mark 计数部分有 1 个测试：验证 `mark_free` 和 `mark_used` 的计数正确，重复操作是 no-op。

Host 测试最大的优势是迭代速度快——修改 PMM 算法后直接编译运行，不需要启动 QEMU。而且可以方便地构造极端场景（比如棋盘模式的碎片化内存），这些场景在 QEMU 的真实 E820 映射中很难复现。

## 设计决策

### 决策：位图 vs 链表 vs Buddy

**问题**: 物理内存管理的核心数据结构应该选哪种？

**本项目的做法**: 位图（bitmap），1 bit per page。

**备选方案**: xv6 使用链表（free list）——每个空闲页的前几个字节存储 `next` 指针，零额外内存开销，O(1) 分配释放。Linux/SerenityOS 使用 buddy 系统——支持 O(log N) 连续分配和按 order 分组的 freelists。

**为什么不选备选方案**: 链表方案的致命问题是无法回答"页 X 是否已分配？"——必须遍历整个链表才能确定，这在调试 double-free 时非常痛苦。而且链表要求每个空闲页的前几个字节可以被覆盖（用来存指针），这在某些场景下可能不合适（比如刚释放的页可能还残留着有用的数据）。Buddy 系统功能强大但实现复杂度高——SerenityOS 的 `PhysicalZone` 有上千行代码，对于一个教学 OS 来说过早了。位图在简洁性和功能性之间取得了很好的平衡。

**如果要扩展/改进**: 当需要高效的连续分配时（比如为 DMA 或者大页映射分配连续物理页），可以在位图之上叠加一个 buddy 系统——位图用于基本的页状态追踪，buddy 用于连续分配。Linux 的 bootmem 就是这样做的：启动阶段用位图，运行时切换到 buddy。

### 决策：双轨测试策略

**问题**: PMM 的测试应该怎么组织？

**本项目的做法**: Host 端单元测试 + QEMU in-kernel 集成测试，两条轨道并行。

**备选方案**: 只在 QEMU 中测试（省去 host 端重实现的工作量），或者只在 host 端测试（省去 QEMU 启动的开销）。

**为什么不选备选方案**: 只在 QEMU 中测试的问题是迭代速度慢——每次修改都要编译内核、制作镜像、启动 QEMU，一个循环至少十几秒。而且 QEMU 测试很难构造极端场景（比如位图全满、碎片化内存）。只在 host 端测试的问题是可能存在"host 和内核行为不一致"的盲区——比如 linker symbol 的取地址方式、内存对齐的差异、编译器优化的不同等。双轨策略互补：host 覆盖算法边界，QEMU 验证端到端。

**如果要扩展/改进**: 可以考虑在 host 端直接 include 内核的 `pmm.cpp`（而不是重实现），通过 mock 掉 kprintf 和 linker symbol 来编译。这样既保持了 host 端的快速迭代，又消除了重实现带来的"host 和内核代码不一致"风险。

## 扩展方向

- ⭐ **释放时 memset 填充垃圾数据**: 像 xv6 那样，在 `free_page` 时把释放的页面填充为 `0x01`（或者其他非零值），帮助检测 use-after-free——如果某个模块在页释放后仍然访问它，读到的不是原始数据而是一堆 0x01。
- ⭐⭐ **per-page 引用计数**: 给每个物理页维护一个引用计数（而不是简单的 used/free bit），支持共享页面（比如 COW fork 后父子进程共享物理页）。
- ⭐⭐⭐ **从位图迁移到 buddy 分配器**: 实现一个 bootmem -> buddy 的两阶段 PMM。bootmem 阶段用当前的位图分配器为 buddy 系统分配元数据，然后切换到 buddy 作为运行时分配器。

## 参考资料

- Intel SDM: Vol.3A Section 4.4-4.5 — PAE 和 4KB 页对齐
- [OSDev Wiki - Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — bitmap/stack/buddy 方案比较
- xv6 `kalloc.c`: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c) — 链表分配器，零额外内存开销
- Linux bootmem: [LWN](https://lwn.net/Articles/761215/) — 位图 bootmem 与 Cinux 原理相同
- SerenityOS `PhysicalZone.h`: [GitHub](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/PhysicalZone.h) — 13 级 buddy 分配器
