---
title: 015-mm-pmm-3 · 物理内存管理
---

# 015-3 分配、释放与测试：位图分配器的运行时

> 标签：PMM, alloc_page, free_page, alloc_pages, 双轨测试
> 前置：[015-2 PMM 初始化](015-mm-pmm-2.md)

## 前言

前两章我们把 PMM 的数据结构和初始化流程搭建完毕——位图就位了，内核区域被保护了，E820 可用区域被正确解锁了。现在位图里躺着成千上万个 0 bit（空闲页），等着被分配出去。这一章要做的就是实现"借出"和"归还"的运行时接口，然后通过双轨测试来验证整个 PMM 的正确性。

说实话，分配和释放的逻辑比初始化简单多了——毕竟不需要处理地址转换和链接器符号这些"环境问题"。但这里有自己的坑：有符号/无符号混合使用、double-free 的防御、OOM 的处理。我们会一一拆解。

这一章涉及的核心代码只有约 40 行（四个分配/释放函数），但它们是整个内核后续运行时使用频率最高的接口之一。每一页页表的创建都需要调用 `alloc_page`，每一次地址空间的销毁都需要调用 `free_page`。所以这些函数的正确性和性能都非常重要。

## 环境说明

- QEMU 默认配置（128MB 内存），E820 通常报告 1-2 个可用区域
- PMM 管理约 32000 个物理页，空闲约 31000 页
- 测试双轨：host 端（Linux 用户空间）+ QEMU in-kernel
- 本章节涵盖了 tag 015 的最后部分：分配/释放接口和测试策略

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

这种有符号/无符号混合使用的场景在内核开发中非常常见。一个好的编程习惯是：在任何 `int64_t` 到 `uint64_t` 的强转之前，先检查负数。或者在函数签名层面就避免混用——但 `bm_find_first_free` 需要用 -1 来表示"没找到"，所以混用是不可避免的。

物理地址 0 被用作 OOM 标志。这是一个很自然的选择：物理地址 0 是 IVT 的位置，它永远不应该被分配给普通用途。我们的 PMM 管理的物理内存从 1MB 开始，所以物理地址 0 根本不在管理范围内。调用者只需要 `if (addr != 0)` 就能区分成功和 OOM。

值得注意的是，`alloc_page` 返回的物理地址保证是 4KB 对齐的——因为 `idx * PAGE_SIZE` 中 `PAGE_SIZE = 4096`，任何整数乘以 4096 都是 4096 的倍数。这个性质在后续的页表映射中非常重要——页表项中的物理基地址必须是 4KB 对齐的。

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

第一层拦截对 OOM 返回值的错误释放。第二层拦截超出 PMM 管理范围的地址（比如 MMIO 地址或者计算错误的地址）。第三层是防止 double-free 的关键——检查目标 bit 是否确实为 1（已用），如果不是 1 说明这个页已经是空闲的了。

这三层防御保证了即使调用者犯了错误，PMM 的内部状态也不会被破坏。这是一种"宽容接收、严格管理"的设计哲学——在一个教学 OS 中，内核不会因为一个无害的编程错误就崩溃，但 `free_pages_` 计数始终保持精确。如果你将来要在生产级内核中使用这个 PMM，可以考虑在 double-free 时打印一条警告日志，帮助开发者定位问题。

这种"宽容接收"的策略和 xv6 形成了鲜明的对比——xv6 的 `kfree` 在释放时会检查地址是否页对齐、是否在合法范围内，如果检查失败就直接 panic。对于教学系统来说，两种策略都是合理的——xv6 的 panic 策略帮助开发者尽早发现问题，Cinux 的静默策略让系统在面对小错误时更鲁棒。选择哪种策略取决于你的设计哲学。

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

前两行是一个重要的优化——`count == 1` 直接走 `alloc_page` 的 64 位加速路径，不走 first-fit 的逐 bit 扫描。如果你忘记了这个优化，`alloc_pages(1)` 也会走 first-fit，结果正确但性能差了最多 64 倍。

first-fit 的核心是两个变量：`run` 记录连续空闲页数量，`start` 记录连续区域起始。遇到空闲页 `run++`，遇到已用页 `run` 归零。当 `run` 达到 `count` 时找到了足够长的连续区域。

时间复杂度方面，外层循环扫描 N 个页（执行 N 次 `bm_test`），找到连续区域后内层循环标记 M 个页（执行 M 次 `bm_set`）。内层循环最多执行一次（因为找到后函数就 return 了），所以总时间复杂度是 O(N)（N 是总页数，M 不超过 N，O(N+M) = O(N)）。对于 QEMU 默认的 128MB（32000 页）来说，即使最坏情况也只需要几万次位运算。

first-fit 算法的一个已知问题是外部碎片化——随着分配和释放的进行，空闲页面会被分割成越来越多的小片段，导致大块的连续分配失败。但在 Cinux 的使用场景中（PMM 主要为页表分配页面，每次只需要少量连续页），这个问题几乎不会出现。如果你将来需要管理更大的物理内存（比如 4GB = 100 万页），应该考虑 buddy 分配器——它通过 buddy 合并自动消除外部碎片。

`free_pages(phys, count)` 的实现极其简单——一个循环，对每一页调用 `free_page`。由于 `free_page` 自带三层防御，所以即使传入的范围中某些页本来就是空闲的（比如调用者错误地释放了从未分配的页），也不会破坏 PMM 的内部状态——那些页会被静默跳过。这种"组合原语"的设计方式在内核开发中很常见：`free_pages` 不需要自己实现防御逻辑，而是把安全性委托给底层的 `free_page`。

## 统计方法：调试的基石

```cpp
uint64_t PMM::free_page_count() const { return free_pages_; }
uint64_t PMM::total_page_count() const { return total_pages_; }
```

两个纯查询方法，直接返回成员变量的值。在当前的单核内核中不需要加锁——没有并发访问的问题。但在调试和监控中它们非常有用：几乎所有的测试断言都以 `free_page_count()` 作为"操作是否生效"的判据。

这两个方法虽然简单，但在 PMM 的生命周期中扮演着"健康指示器"的角色。你可以随时调用它们来检查 PMM 的状态是否一致——比如分配 N 页后 `free_page_count()` 应该减少 N，释放后应该恢复。如果计数不匹配，说明有内存泄漏或者 double-free。

## 双轨测试策略

Cinux 的 PMM 测试分两条轨道，互补覆盖不同层面的正确性。双轨策略的设计借鉴了 Linux 内核的测试方法——Linux 的 bootmem 也有类似的 host 端和内核端测试。Host 端测试覆盖算法边界情况（迭代速度快、可以构造极端场景），QEMU 测试验证端到端行为（使用真实硬件数据、不存在 mock 偏差）。两条轨道互补，任何一条单独使用都会留下盲区。

### Host 端单元测试

Host 端测试（`test/unit/test_pmm.cpp`，345 行）在 Linux 用户空间编译运行，迭代速度快。它重新实现了一个 `TestPMM` 类来复制 PMM 的核心算法，覆盖了 13 个测试场景：

E820 解析测试覆盖了 5 个边界情况：类型过滤（只保留 type=1）、低内存过滤、跨边界截断、4KB 对齐、过小区域丢弃。位图分配器测试覆盖了 7 个场景：单页分配返回对齐地址、1000 次循环后计数恢复、OOM 返回 0、`free_page(0)` 是 no-op、double-free 是 no-op、连续 4 页分配、碎片化内存上连续分配失败。最后还有一个 mark 计数正确性测试。

碎片化测试特别有趣——它构造了一个"棋盘"位图（偶数页空闲、奇数页已用），然后尝试分配 2 个连续页。因为没有任何两个相邻的空闲页，所以 `alloc_pages(2)` 应该返回 0。这种极端场景在 QEMU 的真实 E820 映射中几乎不可能复现，但 host 端测试可以轻松构造。

Host 测试最大的优势是迭代速度快——修改 PMM 算法后直接编译运行，不需要启动 QEMU。而且可以方便地构造极端场景（棋盘模式、全满位图等）。缺点是 TestPMM 是算法的重实现，如果内核代码改了但忘了同步测试，测试就会测"错误的代码"。但在实践中，这种函数一旦写好就很少改动，所以风险可控。

Host 测试的一个技术选择值得一提：为什么选择"重实现"而不是"直接 include 内核源码"？内核的 `pmm.cpp` include 了 `kernel/lib/kprintf.hpp`，还引用了链接器符号——这些东西在 host 端根本不存在。虽然可以通过 mock 和桩函数来解决，但为了一次性的单元测试搞一套 mock 框架未免太重了。相反，`parse_memory_map` 和位图分配器的逻辑是纯算术的——不涉及任何硬件操作，复制到测试文件里只需要改一下类型名。

### QEMU in-kernel 集成测试

QEMU 测试（`kernel/test/test_pmm.cpp`，144 行）使用真实的 BootInfo 数据和真正的位图，验证端到端的正确性。6 个测试用例覆盖了 PMM 最容易出错的三个方面：初始化统计（total > 0, free > 0, free <= total）、分配释放循环的计数一致性、以及 `free_page(0)` 和 double-free 的边界条件。

集成测试在 `main_test.cpp` 中被调度——先初始化 PMM（`g_pmm.init(*boot_info)`），然后调用 `run_pmm_tests()`。这保证了 PMM 在测试运行之前已经被正确初始化，使用的是 QEMU 提供的真实 E820 数据。

集成测试的典型输出如下：

```
=== PMM Tests (015) ===
  [PASS] test_init_and_stats
  [PASS] test_alloc_free_cycle
  [PASS] test_bulk_alloc_free
  [PASS] test_alloc_pages_contiguous
  [PASS] test_free_zero_noop
  [PASS] test_double_free_noop
=== 6 passed, 0 failed ===
```

如果任何测试失败，输出会显示期望值和实际值的差异，帮助你快速定位问题。

运行完整的测试流程：

```
cmake --build build && cd build && ctest --output-on-failure
```

所有测试标签（smoke、kprintf_format、gdt_idt、ata、elf_loader、big_kernel_loader、kprintf、pic、pit、font、console、framebuffer、keyboard、pmm）应该全部通过。特别关注 `pmm` 标签下的 12 个 host 端测试和 QEMU 内核测试中的 6 个 PMM 集成测试。

## xv6 和 Linux 的对比

说了这么多实现细节，让我们退后一步，从更高的视角来比较三种物理内存管理方案。

xv6 的 `kalloc.c` 用链表实现 O(1) 分配释放——`kalloc` 从链表头 pop 一个空闲页，`kfree` push 回去。但它不支持连续分配——没有 `alloc_pages` 的等价物。在 xv6 的设计中，如果需要连续物理内存，只能在初始化阶段分配（因为初始时所有空闲页是连续的）。Cinux 的 first-fit 虽然性能不如链表，但至少支持了运行时的连续分配——这是很多教学 OS（包括 xv6）都缺少的能力。

链表方案的另一个问题是无法查询页状态——你想知道"页 X 是不是已经被分配了？"，必须遍历整个链表。这意味着 xv6 的 `kfree` 无法检测 double-free（除非遍历链表确认页确实已分配），而 Cinux 的 `free_page` 可以用一次 `bm_test` 就完成检测。在生产级系统中，这种查询能力对于诊断内存错误至关重要。

Linux 的 buddy 分配器是这三种方案中最强大的——支持 O(log N) 的连续分配和释放，通过 buddy 合并自动消除外部碎片。但实现复杂度远高于位图——SerenityOS 的 `PhysicalZone` 有上千行代码，而 Cinux 的整个 PMM 只有 267 行。对于教学 OS 来说，位图的简洁性比 buddy 的高效性更有价值。而且 Cinux 的 PMM 保留了未来升级到 buddy 的可能性——Linux 的做法是在启动阶段用 bootmem（位图），启动完成后切换到 buddy。bootmem 阶段为 buddy 分配器分配元数据（freelist 数组等），然后切换到 buddy 作为运行时分配器。Cinux 可以在未来实现类似的迁移路径——这正是渐进式开发的精髓。

位图方案的另一个优势是调试友好——你可以把整个位图 dump 出来，用肉眼看出哪些页是空闲的、哪些是已用的。一个全 1 的区域意味着"全部已用"，一个全 0 的区域意味着"全部空闲"。这种直观的可视化在链表方案中是不可能的（链表的遍历需要指针追踪），在 buddy 方案中也不太直观（需要理解 order 和 buddy 的配对关系）。

总结一下三种方案的对比：链表（xv6）简洁但不支持连续分配和页状态查询；位图（Cinux）支持连续分配和 O(1) 查询，但分配是 O(N)；Buddy（Linux/SerenityOS）支持 O(log N) 连续分配和自动碎片整理，但实现复杂度远高于前两者。对于教学 OS 来说，位图是最佳选择。

这三种方案也不是互斥的——Linux 就同时使用了 memblock（启动阶段的区段管理器）和 buddy（运行时分配器），两者协同工作。Cinux 的未来路径也很可能类似：用当前的位图 PMM 作为 bootmem，等基础设施成熟后再叠加 buddy 分配器。

## 收尾

现在 PMM 已经完全可用了——从 E820 解析到位图初始化，从单页分配到连续分配，从防御性释放到双轨测试，所有功能都已实现并验证。在 QEMU 中启动内核，你会看到 `[PMM] Total: 128MB, Free: 126MB`，随后内核继续正常初始化 Framebuffer、Console 和 Keyboard。

tag 015 的 PMM 实现虽然只有 267 行代码，但它覆盖了物理内存管理的核心需求：知道有多少内存（E820 解析）、跟踪每一页的状态（位图）、保护关键区域（内核映像 + 栈 + 位图）、分配和释放页面（单页和连续分配）、以及验证正确性（双轨测试）。这些是任何一个操作系统内核的物理内存管理器都必须具备的基本能力。

如果你想验证 PMM 在你的环境中是否工作正常，最简单的方式是运行正常的 big_kernel（不是 test 版本），看串口输出中是否有 `[PMM] Total: XXMB, Free: YYMB` 这行，以及后续的初始化是否正常继续。如果 Total 和 Free 的值看起来合理（Free 应该比 Total 小几 MB），说明 PMM 初始化成功。

PMM 是整个内存管理子系统的基石。在 Cinux 的整个架构中，PMM 是 VMM 的唯一物理页面来源——VMM 调用 `alloc_page` 获取空闲页面来填充页表项，调用 `free_page` 回收不再使用的页表页面。堆分配器则是 VMM 的上层消费者——它向 VMM 申请虚拟地址空间，VMM 再向 PMM 申请物理页面来支持这些映射。

下一站是 VMM（虚拟内存管理器）——它需要 PMM 来分配页表页面，然后构建完整的四级页表映射。有了 PMM 提供的可靠物理页面供给，VMM 的实现会顺畅得多。

## 参考资料

- Intel SDM: Vol.3A Section 4.4-4.5 — PAE 和 4KB 页对齐
- [OSDev Wiki - Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — bitmap/stack/buddy 方案比较
- xv6 `kalloc.c`: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c) — 链表分配器，零额外内存开销
- Linux bootmem: [LWN](https://lwn.net/Articles/761215/) — bootmem 到 memblock 的演变
- SerenityOS `PhysicalZone.h`: [GitHub](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/PhysicalZone.h) — 生产级 buddy 分配器

> 两个统计方法 `free_page_count()` 和 `total_page_count()` 是 const 方法——纯查询不修改状态。在当前的单核内核中不需要加锁。几乎所有测试断言都以 `free_page_count()` 作为操作生效的判据。
