---
title: 015-mm-pmm-2 · 物理内存管理
---

# 015-2 位图管理器初始化：保守标记与地址转换

> 标签：PMM init, 保守标记, higher-half, linker symbol
> 前置：[015-1 E820 解析与位图基础](015-mm-pmm-1.md)

## 前言

上一章我们把 E820 解析器和位图辅助函数准备好了——零件都齐了，现在要把它们组装成一套完整的 PMM 初始化流程。这一章要回答的核心问题是：位图应该放在物理内存的哪个位置？初始化时如何保证不把内核、栈、位图自身所在的内存页分配出去？

说实话，PMM 初始化是整个 tag 015 中最容易翻车的地方。不是因为算法复杂——算法非常简单——而是因为涉及物理地址和虚拟地址的来回转换、链接器符号的正确访问、以及"到底哪些内存不能碰"的完整枚举。这一章里我们会把这些坑一个一个踩过去。

## 环境说明

- higher-half 内核，VMA = 0xFFFFFFFF80000000
- 内核 ELF 被 bootloader 加载到物理地址 0x1000000（16MB），由 BootInfo.kernel_phys_base 记录
- 链接脚本导出 `__kernel_end` 和 `__kernel_stack_top` 两个地址常量
- 内核栈大小 64KB，紧跟 .bss 段之后

## "先全锁、再解锁"——为什么要这么笨？

设想一下，如果我们用"乐观"策略——先假设所有内存都可用，然后逐个标记已知被占用的区域。你需要知道哪些区域被占用了。内核映像占用的区域你可以通过 linker symbol 算出来，BootInfo 结构体的位置你知道在 0x7000，但还有多少隐含的被占用区域？bootloader 的实模式栈、BIOS 的临时数据结构、MMIO 映射区域……你永远无法穷举。更危险的是，某些被占用区域你可能根本不知道它们的存在——比如某些 BIOS 实现会在低内存中存放 APM（高级电源管理）的数据结构，但 E820 不一定会报告这些区域。

所以 Cinux 采用了一种看似笨拙但绝对安全的策略——先把位图全部填为 0xFF（所有页面标记为已用），然后只把 E820 确认可用的区域解锁为空闲，最后再把内核映像、栈和位图自身重新锁回去。

这种策略的妙处在于：即使 E820 报告有遗漏，那些区域也不会被分配出去，因为它们本来就在"全锁"状态。安全代价是可能浪费一些确实可用但没被 E820 报告的内存——但这对教学 OS 来说完全可以接受。Linux 的 bootmem 分配器也采用了完全相同的保守策略，证明这是一个经过实战检验的方案。

现在让我们看具体的代码实现。PMM 的 init 函数包含八步操作，每一步都严格依赖前面步骤的结果。

## mark_region_used/free：位图的"画笔"

在深入 init 的八步流程之前，我们先看两个被 init 频繁调用的辅助方法。它们是位图的"画笔"——在位图上涂抹一段连续的物理内存区域。init 的八步操作中，有四步（4、5、6、7）都在调用这两个方法。

```cpp
void PMM::mark_region_used(uint64_t phys, uint64_t length) {
    uint64_t start = phys / PAGE_SIZE;
    uint64_t end   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = start; p < end && p < highest_page_; p++) {
        if (!bm_test(bitmap_, p)) {
            bm_set(bitmap_, p);
            free_pages_--;
        }
    }
}
```

这两个方法接收物理地址和长度，把它们转换为页索引范围（起始页向下取整、结束页向上取整），然后逐页遍历。有一个关键细节：它们都是"先检查状态再翻转"——`mark_region_used` 只对当前为空闲的页调用 `bm_set`，`mark_region_free` 只对当前为已用的页调用 `bm_clear`。这种设计保证了 `free_pages_` 计数的精确性：即使在 init 流程中存在区域重叠，重复操作也不会导致计数错误。

边界检查 `p < highest_page_` 也是必须的——传入的物理地址可能超出 PMM 管理的范围（比如高地址的 MMIO 区域），不做检查就会越界访问位图数组，大概率 triple fault。

另一个值得注意的设计选择是"先检查状态再翻转"的防御策略。虽然这增加了每次循环的一次 `bm_test` 调用，但保证了即使在 init 流程中存在区域重叠或者重复操作，`free_pages_` 计数也不会出错。这是一种"低成本高回报"的防御措施，在内核开发中特别值得投入。

## 八步初始化流程

这八步的顺序是严格固定的——你不能先解锁可用区域再计算位图大小（因为位图还没创建），也不能先保护内核区域再初始化位图为全 1（因为全 1 初始化会把保护操作覆盖掉）。每一步都依赖于前面步骤的结果。

现在让我们逐行看 `PMM::init` 的八步流程：

```cpp
void PMM::init(const BootInfo& info) {
    // Step 1: Extract usable memory regions
    MemoryRegion regions[32];
    uint32_t region_count = parse_memory_map(info, regions, 32);
```

第一步，调用上一章实现的 `parse_memory_map`。输出到一个栈上的局部数组——32 个条目足够覆盖任何硬件配置，即使是最复杂的 BIOS 也不会返回超过 20 个 E820 条目。这个数组只在 init 过程中使用，init 返回后就自动释放了。

```cpp
    // Step 2: Determine highest physical address -> bitmap size
    uint64_t max_addr = 0;
    for (uint32_t i = 0; i < region_count; i++) {
        uint64_t end = regions[i].base + regions[i].length;
        if (end > max_addr) max_addr = end;
    }

    highest_page_ = max_addr / PAGE_SIZE;
    total_pages_  = highest_page_;
    bitmap_size_  = (highest_page_ + 7) / 8;
```

第二步，找出最高的物理地址来决定位图需要覆盖多大范围。`highest_page_` 存储的是"总页数"（即最大页索引 + 1），后续被用作循环上界。`bitmap_size_` 用向上取整公式计算位图的字节数。

这里有一个设计上的权衡：位图覆盖了从物理地址 0 到最高地址的全部范围，但物理地址 0 到 1MB 之间的页面永远不会被分配（因为它们在 parse_memory_map 中就被过滤掉了）。这意味着位图的低部分（前 256 个 bit）永远是全 1——浪费了 32 字节。但这点浪费完全可以忽略，换来的是代码的简洁性。

```cpp
    // Step 3: Place bitmap after kernel stack, page-aligned
    uintptr_t stack_top_virt = reinterpret_cast<uintptr_t>(&__kernel_stack_top);
    uintptr_t bm_virt = (stack_top_virt + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    bitmap_ = reinterpret_cast<uint8_t*>(bm_virt);
```

第三步是整个 init 中最关键的地址计算。取 `__kernel_stack_top` 的地址值（注意那个 `&` 操作符——这是 linker symbol 的经典陷阱，不加 `&` 读到的是地址处的一个字节值而不是地址本身），向上对齐到页边界，这就是位图的虚拟起始地址。位图放在内核栈之后有几个好处：和内核映像在物理内存中连续，不制造碎片；通过 higher-half 映射直接可访问，不需要额外的页表操作。

```cpp
    // Step 4: Initialise bitmap -- all pages marked as used
    for (uint64_t i = 0; i < bitmap_size_; i++) {
        bitmap_[i] = 0xFF;
    }
    free_pages_ = 0;
```

第四步，保守策略的核心——全填 0xFF，`free_pages_` 归零。从这一刻起，位图中每一个 bit 都是 1（已用），除非后续步骤主动把它清零。这就是"先全锁"的含义——位图处于完全锁定的状态，没有任何页面可用。接下来第五步的解锁操作会精确地把 E820 确认可用的页面释放出来。对于 128MB 的 QEMU 配置，位图大小是 4096 字节（恰好一个页），这一步写了 4096 个字节的 0xFF——非常快速。

```cpp
    // Step 5: Clear bits for each usable region
    for (uint32_t i = 0; i < region_count; i++) {
        mark_region_free(regions[i].base, regions[i].length);
    }
```

第五步，解锁 E820 可用区域。这一步结束后，所有 type=1 的物理页都被标记为空闲了。此时 `free_pages_` 应该等于所有可用区域的页数之和——在 QEMU 的 128MB 配置中，这大约是 32700 页左右。

```cpp
    // Step 6: Re-mark kernel image + stack as used
    uint64_t used_phys_start = info.kernel_phys_base;
    uint64_t used_phys_end   = bm_virt - KERNEL_VMA;
    mark_region_used(used_phys_start, used_phys_end - used_phys_start);
```

第六步，重新保护内核映像和栈。物理结束地址用 `bm_virt - KERNEL_VMA` 计算——这是 higher-half 转换，虚拟地址减去 VMA 偏移得到物理地址。这里用的是位图虚拟地址而不是 `__kernel_end`，因为栈在 `__kernel_end` 之后、位图之前。位图虚拟地址减去 KERNEL_VMA 才是"内核映像 + 栈"整体的物理结束位置。如果你用了 `__kernel_end`，栈占用的页面就不会被保护，后续分配器可能会把栈所在的物理页分配出去——后果不堪设想。这种"差一个区域"的 bug 在内核开发中特别阴险，因为它不会立刻 crash，而是在分配器恰好把栈页分配出去后才触发，而且触发时机完全不可预测。

```cpp
    // Step 7: Mark bitmap itself as used
    uint64_t bm_phys       = bm_virt - KERNEL_VMA;
    uint64_t bm_pages_bytes = ((bitmap_size_ + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    mark_region_used(bm_phys, bm_pages_bytes);

    // Step 8: Print statistics
    uint64_t total_mb = total_pages_ * PAGE_SIZE / (1024 * 1024);
    uint64_t free_mb  = free_pages_ * PAGE_SIZE / (1024 * 1024);
    cinux::lib::kprintf("[PMM] Total: %uMB, Free: %uMB\n", total_mb, free_mb);
}
```

第七步把位图自身也锁住——位图不能把自己分配出去。位图的物理地址用 `bm_virt - KERNEL_VMA` 计算（又一次 higher-half 转换），占用的字节数向上取整到页边界。第八步打印统计，在 QEMU（128MB）中你会看到类似 `[PMM] Total: 128MB, Free: 126MB` 的输出。

## Linker Symbol 的经典陷阱

第三步中的 `&__kernel_stack_top` 值得展开讲讲。链接脚本中定义的符号（比如 `__kernel_end`、`__kernel_stack_top`）本质上是地址常量——链接器把它们当作"在这个地址有一个东西"，但并不真的在那里分配内存。

正确的访问模式是：声明为 `extern "C" char __kernel_stack_top`（用 `extern "C"` 是因为链接脚本是 C 级别的符号，不会经过 C++ 的 name mangling），然后通过取地址操作 `&__kernel_stack_top` 获得地址值。如果你直接访问 `__kernel_stack_top`（不加 &），编译器会尝试读取那个地址处的一个字节——拿到的是一个随机值而不是地址本身。这个 bug 极其隐蔽，因为编译不报错，运行时只会产生错误的地址。

假设链接脚本中 `__kernel_stack_top` 的值是 `0xFFFFFFFF80110000`。正确做法 `&__kernel_stack_top` 返回 `0xFFFFFFFF80110000`——这就是栈顶的虚拟地址。错误做法（不加 &）会读取地址 `0xFFFFFFFF80110000` 处的一个字节——可能是 0x00 也可能是任何残留数据，然后把这个字节值当成地址来用。后果可想而知。笔者在这里被坑了整整一个下午，最后是在 GDB 里打印两个表达式的值才发现差异的。

之所以声明为 `char` 而不是 `char[]` 或者其他类型，是因为 `char` 的大小恰好是 1 字节——对它取地址时，得到的地址就是符号本身的地址，不会因为类型大小而产生偏移。`extern "C"` 则是为了避免 C++ 的 name mangling——链接脚本是 C 级别的符号。这种"链接器符号取地址"的模式在 OS 开发中无处不在：Linux 用 `_text` 和 `_end` 标记内核映像范围，xv6 用 `end` 确定第一个空闲页的位置。

## SerenityOS 怎么做的？

SerenityOS 的 PMM 实现比 Cinux 复杂得多——它使用了一个分层的设计：`PhysicalRegion`（连续物理内存范围）包含多个 `PhysicalZone`（buddy-block 分配器），支持 13 个 order（从 4KB 到 16MB），每个 order 维护独立的 freelist。物理页面通过 `PhysicalRAMPage` 进行引用计数（使用 `RefPtr`），支持 COW（Copy-on-Write）和共享内存。

Cinux 的位图 PMM 和 SerenityOS 的 buddy PMM 代表了两个极端。位图适合启动阶段——代码简洁（267 行）、内存开销小（4KB for 128MB）、初始化逻辑清晰。Buddy 适合运行时——O(log N) 连续分配、按 order 分组的 freelist、引用计数支持。Linux 的做法是在启动阶段用 bootmem（类似位图），启动完成后切换到 buddy。Cinux 的未来路径也很可能类似。

比较一下数字就能感受到差距：Cinux 的 PMM 总共 267 行代码，支持单页分配和连续分配；SerenityOS 的 `PhysicalZone` 一个文件就有上千行代码，但支持 O(log N) 的连续分配、按 order 分组的 freelist、引用计数、COW 等高级特性。对于教学 OS 来说，267 行的位图方案在简洁性和功能性之间取得了很好的平衡。而且位图的"扁平"特性（一个连续数组）对 cache 友好性来说比链表和 buddy 的多级数据结构要好得多——扫描一个连续数组时，CPU 的 prefetcher 可以高效地预取下一批数据。

## 物理内存布局一览

初始化完成后，物理内存的布局大致如下：

```
0x00000000 ┌─────────────────────────────┐
           │ IVT / BDA / EBDA / BIOS ROM  │  不被 PMM 管理
0x00100000 ├─────────────────────────────┤
           │ 可用 RAM (E820 type=1)        │
           │ ┌───────────────────────────┐│
           │ │ 内核映像 (.text .rodata   ││  已用
           │ │   .data .bss)             ││
           │ ├───────────────────────────┤│
           │ │ 内核栈 (64KB)             ││  已用
           │ ├───────────────────────────┤│
           │ │ PMM 位图 (4KB-32KB)       ││  已用
           │ ├───────────────────────────┤│
           │ │ 剩余可用 RAM              ││  空闲
           │ └───────────────────────────┘│
           ├─────────────────────────────┤
           │ MMIO / ACPI / 保留区域      │  已用
           └─────────────────────────────┘
```

这种布局保证了"内核映像 + 栈 + 位图"在物理内存中是连续的——只占用一段连续的物理地址范围，不会在可用内存中间制造碎片。剩余的可用 RAM 从位图结束后一直延伸到 E820 报告的可用区域末尾，是一大段连续的空闲空间。

虚拟地址和物理地址之间的转换公式是 `phys = virt - KERNEL_VMA`，其中 `KERNEL_VMA = 0xFFFFFFFF80000000`。PMM init 中总共出现了三次这个转换——确定位图物理地址、计算内核占用区域的物理结束地址、以及计算位图自身的物理地址。任何一次计算出错都会导致静默的位图错误——位图可能被写到了错误的物理位置，或者错误的页面被保护了。这种"静默错误"在初始化阶段通常不会 crash，但在后续的页表构建中会引发莫名其妙的 page fault。所以如果你遇到"init 输出看起来正常但后续页表映射总是出错"的情况，第一个检查的就是 KERNEL_VMA 常量和链接脚本是否一致。

## 到这里

这一章我们把 PMM 的初始化流程走完了——位图就位了，内核区域被保护了，E820 可用区域被正确解锁了。串口输出 `[PMM] Total: 128MB, Free: 126MB` 告诉我们一切正常。Free 值等于 Total 减去内核映像、栈和位图占用的那几 MB——如果你看到的 Free 值异常小或者等于 Total，说明第六步或第七步的 mark_region_used 没有正确生效。

八步操作看似繁杂，但核心逻辑只有一条线：先全锁、再按 E820 解锁、最后把内核区域和位图锁回去。真正容易出错的地方在于三次 higher-half 地址转换和两个链接器符号的取地址操作。init 是整个 PMM 中最容易翻车的函数，所以如果你在调试 init 相关的问题，请优先检查这三个转换和两个符号访问。

> 调试小技巧：在 init 完成后加一条 kprintf 打印位图的虚拟地址和物理地址来验证放置是否正确。虚拟地址应该在栈顶之后、物理地址应该在内核映像之后。确认无误后记得删掉这个调试打印。也可以打印 `info.kernel_phys_base` 和 `free_pages_` 的值来确认内核物理基地址和空闲页数是否合理。

下一章要实现分配和释放的运行时接口，然后通过双轨测试来验证整个 PMM 的正确性。有了这一章的基础，分配和释放的实现会顺畅得多——因为所有棘手的地址计算和链接器符号处理都已经在 init 中搞定了。

## 参考资料

- Intel SDM: Vol.3A Section 2.5 — CR3 保存物理地址，PMM 为 VMM 提供页表页面
- Intel SDM: Vol.3A Section 4.5 — 4KB 页对齐要求
- [OSDev Wiki - Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 虚拟-物理地址偏移
- [OSDev Wiki - Writing A Page Frame Allocator](https://wiki.osdev.org/Writing_A_Page_Frame_Allocator) — linker symbol `endkernel` 访问模式
- SerenityOS `PhysicalZone.h`: [GitHub](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/PhysicalZone.h) — 13 级 buddy 分配器
- Linux bootmem: [LWN](https://lwn.net/Articles/761215/) — 同样的保守位图策略
