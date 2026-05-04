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

设想一下，如果我们用"乐观"策略——先假设所有内存都可用，然后逐个标记已知被占用的区域。你需要知道哪些区域被占用了。内核映像占用的区域你可以通过 linker symbol 算出来，BootInfo 结构体的位置你知道在 0x7000，但还有多少隐含的被占用区域？bootloader 的实模式栈、BIOS 的临时数据结构、MMIO 映射区域……你永远无法穷举。

所以 Cinux 采用了一种看似笨拙但绝对安全的策略——先把位图全部填为 0xFF（所有页面标记为已用），然后只把 E820 确认可用的区域解锁为空闲，最后再把内核映像、栈和位图自身重新锁回去。这种策略的妙处在于：即使 E820 报告有遗漏，那些区域也不会被分配出去，因为它们本来就在"全锁"状态。安全代价是可能浪费一些确实可用但没被 E820 报告的内存——但这对教学 OS 来说完全可以接受。

现在让我们逐行看 `PMM::init` 的八步流程：

```cpp
void PMM::init(const BootInfo& info) {
    // Step 1: Extract usable memory regions
    MemoryRegion regions[32];
    uint32_t region_count = parse_memory_map(info, regions, 32);
```

第一步，调用上一章实现的 `parse_memory_map`。输出到一个栈上的局部数组——32 个条目足够覆盖任何硬件配置。

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

第二步，找出最高的物理地址来决定位图需要覆盖多大范围。`bitmap_size_` 用 `(highest_page_ + 7) / 8` 向上取整到字节边界——这是位操作中经典的"除法向上取整"公式。

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

第四步，保守策略的核心——全填 0xFF，`free_pages_` 归零。

```cpp
    // Step 5: Clear bits for each usable region
    for (uint32_t i = 0; i < region_count; i++) {
        mark_region_free(regions[i].base, regions[i].length);
    }
```

第五步，解锁 E820 可用区域。

```cpp
    // Step 6: Re-mark kernel image + stack as used
    uint64_t used_phys_start = info.kernel_phys_base;
    uint64_t used_phys_end   = bm_virt - KERNEL_VMA;
    mark_region_used(used_phys_start, used_phys_end - used_phys_start);
```

第六步，重新保护内核映像和栈。物理结束地址用 `bm_virt - KERNEL_VMA` 计算——这是 higher-half 转换，虚拟地址减去 VMA 偏移得到物理地址。这里用的是位图虚拟地址而不是 `__kernel_end`，因为栈在 `__kernel_end` 之后、位图之前。

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

第七步把位图自身也锁住——位图不能把自己分配出去。第八步打印统计，在 QEMU（128MB）中你会看到类似 `[PMM] Total: 128MB, Free: 126MB` 的输出。

## mark_region_used/free 的"先查再改"策略

这两把"刷子"是 init 流程中的核心工具，它们的实现有一个值得注意的细节——"先检查状态再翻转"。`mark_region_used` 只对当前为空闲的页执行 set 操作，`mark_region_free` 只对当前为已用的页执行 clear 操作。这种设计保证了 `free_pages_` 计数的精确性：即使在 init 流程中存在区域重叠（比如 E820 报告了两个有交叉的可用区域），重复操作也不会导致计数错误。

## SerenityOS 怎么做的？

SerenityOS 的 PMM 实现比 Cinux 复杂得多——它使用了一个分层的设计：`PhysicalRegion`（连续物理内存范围）包含多个 `PhysicalZone`（buddy-block 分配器），支持 13 个 order（从 4KB 到 16MB），每个 order 维护独立的 freelist。物理页面通过 `PhysicalRAMPage` 进行引用计数（使用 `RefPtr`），支持 COW（Copy-on-Write）和共享内存。

Cinux 的位图 PMM 和 SerenityOS 的 buddy PMM 代表了两个极端。位图适合启动阶段——代码简洁（267 行）、内存开销小（4KB for 128MB）、初始化逻辑清晰。Buddy 适合运行时——O(log N) 连续分配、按 order 分组的 freelist、引用计数支持。Linux 的做法是在启动阶段用 bootmem（类似位图），启动完成后切换到 buddy。Cinux 的未来路径也很可能类似。

## 到这里

这一章我们把 PMM 的初始化流程走完了——位图就位了，内核区域被保护了，E820 可用区域被正确解锁了。串口输出 `[PMM] Total: 128MB, Free: 126MB` 告诉我们一切正常。下一章要实现分配和释放的运行时接口，然后通过双轨测试来验证整个 PMM 的正确性。

## 参考资料

- Intel SDM: Vol.3A Section 2.5 — CR3 保存物理地址，PMM 为 VMM 提供页表页面
- Intel SDM: Vol.3A Section 4.5 — 4KB 页对齐要求
- [OSDev Wiki - Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 虚拟-物理地址偏移
- [OSDev Wiki - Writing A Page Frame Allocator](https://wiki.osdev.org/Writing_A_Page_Frame_Allocator) — linker symbol `endkernel` 访问模式
- SerenityOS `PhysicalZone.h`: [GitHub](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/PhysicalZone.h) — 13 级 buddy 分配器
- Linux bootmem: [LWN](https://lwn.net/Articles/761215/) — 同样的保守位图策略
