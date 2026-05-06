# 015-2 通读：PMM 初始化——保守标记与位图放置

## 概览

本文是 tag `015_mm_pmm` 三篇通读教程的第二篇，聚焦于 `PMM::init` 的完整实现。上一篇我们准备好了"零件"——`parse_memory_map` 和四个位图辅助函数，现在要把它们组装成一套完整的初始化流程。PMM 的初始化采用了一种"先全锁、再解锁"的保守策略：先把位图中所有页面标记为已用，然后只把 E820 确认可用的区域解锁为空闲，最后把内核映像、栈和位图自身重新锁回去。这种策略虽然笨拙，但它天然安全——任何被 E820 遗漏的保留区域都不会被误分配出去。

我们还会看到位图是如何被"放置"到物理内存中的——它紧跟内核栈之后，通过 higher-half 虚拟地址直接访问。整个 init 流程涉及八步操作和三个关键的地址转换，让我们逐步拆解。

## 架构图

```
    物理内存布局 (初始化后)
    ┌──────────────────────────────────────────────────────┐ 0x00000000
    │  IVT / BDA / EBDA / BIOS ROM  (永远不被 PMM 管理)    │
    ├──────────────────────────────────────────────────────┤ 0x00100000 (1MB)
    │  可用 RAM (E820 type=1)                               │
    │  ┌──────────────────────────────────────────────────┐│
    │  │  内核映像 (.text .rodata .data .bss)  ← 已用      ││
    │  │  kernel_phys_base .. __kernel_end                  ││
    │  ├──────────────────────────────────────────────────┤│
    │  │  内核栈                                  ← 已用    ││
    │  │  __kernel_end .. __kernel_stack_top               ││
    │  ├──────────────────────────────────────────────────┤│
    │  │  PMM 位图                               ← 已用    ││
    │  │  __kernel_stack_top (页对齐) .. bitmap_end        ││
    │  ├──────────────────────────────────────────────────┤│
    │  │  剩余可用 RAM                            ← 空闲    ││
    │  │  ...一直到 E820 报告的可用区域末尾                  ││
    │  └──────────────────────────────────────────────────┘│
    ├──────────────────────────────────────────────────────┤
    │  MMIO / ACPI / 保留区域                    ← 已用     │
    └──────────────────────────────────────────────────────┘

    虚拟地址 ↔ 物理地址转换:
    virt - KERNEL_VMA = phys
    KERNEL_VMA = 0xFFFFFFFF80000000
```

## 代码精讲

### 常量与链接器符号

```cpp
constexpr uint64_t PAGE_SIZE        = 4096;
constexpr uint64_t LOW_MEM_BOUNDARY = 0x100000;               // 1 MB
constexpr uint64_t KERNEL_VMA       = 0xFFFFFFFF80000000ULL;

extern "C" {
extern char __kernel_end;
extern char __kernel_stack_top;
}
```

三个常量定义了 PMM 的基本工作参数。`PAGE_SIZE` 是 x86_64 long mode 下最小的页大小——4KB，PMM 的分配粒度就固定在这个值上。`LOW_MEM_BOUNDARY` 是"第一兆字节"的边界线——所有低于 1MB 的物理内存都不归 PMM 管，因为那里住着 IVT、BDA、EBDA、视频内存、BIOS ROM 等一堆"祖宗级"的数据结构，碰哪个都得出事。`KERNEL_VMA` 是 Cinux higher-half 内核的虚拟地址基址，用于在虚拟地址和物理地址之间做加减法转换。

链接器符号的声明方式值得仔细看。这两个符号（`__kernel_end` 和 `__kernel_stack_top`）是在链接脚本 `kernel.ld` 中定义的地址常量——链接器把它们当作"在这个地址有一个东西"，但并不真的在那里分配内存。在 C/C++ 代码中，我们声明它们为 `extern "C" char`（用 `extern "C"` 是因为链接脚本是 C 级别的符号，不会经过 C++ 的 name mangling），然后通过取地址操作 `&__kernel_stack_top` 获得它们的地址值。这里有一个经典的陷阱：如果你写 `__kernel_stack_top` 不加 `&`，编译器会尝试读取那个地址处的一个字节——拿到的是一个随机值而不是地址本身。这个 bug 极其隐蔽，因为编译不报错，运行时只会产生错误的地址。

### mark_region_used 和 mark_region_free

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

void PMM::mark_region_free(uint64_t phys, uint64_t length) {
    uint64_t start = phys / PAGE_SIZE;
    uint64_t end   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t p = start; p < end && p < highest_page_; p++) {
        if (bm_test(bitmap_, p)) {
            bm_clear(bitmap_, p);
            free_pages_++;
        }
    }
}
```

这两个方法是 init 流程的"笔刷"——它们负责在位图上涂抹一段连续的物理内存区域。接收的参数是物理地址和长度，内部先把它转换成页索引范围：起始页是 `phys / PAGE_SIZE`（整数除法自动向下取整），结束页是 `(phys + length + PAGE_SIZE - 1) / PAGE_SIZE`（向上取整的经典公式）。然后逐页遍历这个范围，但有两个关键细节。

第一个细节是"先检查再操作"——`mark_region_used` 只对当前为空闲的页调用 `bm_set`，`mark_region_free` 只对当前为已用的页调用 `bm_clear`。这种设计保证了 `free_pages_` 计数的精确性：如果对同一个页连续调用两次 `mark_region_free`，第二次不会多加——因为第一次已经把 bit 清零了，第二次的 `bm_test` 检查会返回 false，直接跳过。这是一个很实用的防御措施，尤其是在 init 流程中"先全锁再解锁"的策略下，区域之间可能有重叠或者重复操作。

第二个细节是边界检查 `p < highest_page_`——传入的物理地址可能超出 PMM 管理的范围（比如高地址的 MMIO 区域），如果不做检查就会越界访问位图数组，大概率 triple fault。

### PMM::init 主流程

```cpp
void PMM::init(const BootInfo& info) {
    // Step 1: Extract usable memory regions
    MemoryRegion regions[32];
    uint32_t region_count = parse_memory_map(info, regions, 32);
```

第一步调用上一篇实现的 `parse_memory_map`，从 BootInfo 中提取经过过滤和对齐的可用内存区域。输出到一个栈上的局部数组——32 个条目足以覆盖任何实际硬件配置。这个数组只在 init 过程中使用，init 返回后就自动释放了。

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

第二步遍历所有可用区域，找出最高的物理地址。这个值决定了位图需要覆盖多大的范围——位图必须能索引从物理地址 0 到最高地址的所有页。`highest_page_` 是总页数（即最大页索引 + 1），后续代码中它被用作 `p < highest_page_` 的循环上界。`total_pages_` 等于它（两者含义相同）。`bitmap_size_` 是位图需要的字节数，用 `(highest_page_ + 7) / 8` 向上取整到字节边界。

这里有一个设计上的权衡：位图覆盖了从物理地址 0 到最高地址的全部范围，但物理地址 0 到 1MB 之间的页面永远不会被分配（因为它们在第一步就被过滤掉了）。这意味着位图的低部分（前 256 个 bit，对应前 1MB）永远是全 1——浪费了 32 字节。但这点浪费完全可以忽略，换来的是代码的简洁性——不需要处理"位图中有空洞"的复杂逻辑。

```cpp
    // Step 3: Place bitmap after kernel stack, page-aligned
    uintptr_t stack_top_virt = reinterpret_cast<uintptr_t>(&__kernel_stack_top);
    uintptr_t bm_virt = (stack_top_virt + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    bitmap_ = reinterpret_cast<uint8_t*>(bm_virt);
```

第三步确定位图的放置位置。取 `__kernel_stack_top` 的地址值（注意 `&` 操作符），向上对齐到页边界——这就是位图的虚拟起始地址。`bitmap_` 指针直接指向这个虚拟地址，因为 Cinux 的 higher-half 映射已经把这个虚拟地址映射到了对应的物理地址，后续通过 `bitmap_` 读写位图就是直接操作物理内存。

为什么要对齐到页边界？因为 `bm_find_first_free` 中 64 位扫描需要位图起始地址是 8 字节对齐的（`reinterpret_cast<const uint64_t*>(bm)` 要求），而页边界（4096 字节）是 8 的倍数，天然满足。更大的好处是位图的起始地址和页边界对齐，方便计算位图自身占用的页数。

```cpp
    // Step 4: Initialise bitmap -- all pages marked as used
    for (uint64_t i = 0; i < bitmap_size_; i++) {
        bitmap_[i] = 0xFF;
    }
    free_pages_ = 0;
```

第四步是保守策略的核心——把位图的每个字节都设为 `0xFF`（8 个 bit 全为 1，表示所有页面已用），同时把 `free_pages_` 计数器归零。此时，位图中没有任何空闲页。

```cpp
    // Step 5: Clear bits for each usable region
    for (uint32_t i = 0; i < region_count; i++) {
        mark_region_free(regions[i].base, regions[i].length);
    }
```

第五步"解锁"——遍历 `parse_memory_map` 输出的每个可用区域，调用 `mark_region_free` 把对应的 bit 清零。这一步结束后，所有 E820 报告为 type=1 的物理页都被标记为空闲了。注意这些区域已经经过了对齐处理，所以不需要担心页边界问题。

```cpp
    // Step 6: Re-mark kernel image + stack as used
    uint64_t used_phys_start = info.kernel_phys_base;
    uint64_t used_phys_end   = bm_virt - KERNEL_VMA;
    mark_region_used(used_phys_start, used_phys_end - used_phys_start);
```

第六步是"回锁"的第一部分——重新保护内核映像和栈。物理范围从 `info.kernel_phys_base`（bootloader 把内核 ELF 加载到的物理地址，通常是 0x1000000 即 16MB）到位图虚拟地址减去 `KERNEL_VMA` 得到的物理地址。这里用 `bm_virt - KERNEL_VMA` 而不是 `__kernel_end` 是有讲究的：`__kernel_end` 标记的是内核映像段的结束位置，不包括栈——而位图是放在栈之后的，所以位图起始虚拟地址对应的物理地址才是"内核映像 + 栈"整体的物理结束位置。

```cpp
    // Step 7: Mark bitmap itself as used
    uint64_t bm_phys       = bm_virt - KERNEL_VMA;
    uint64_t bm_pages_bytes = ((bitmap_size_ + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    mark_region_used(bm_phys, bm_pages_bytes);
```

第七步把位图自身也标记为已用——位图总不能把自己分配出去。位图的物理地址是 `bm_virt - KERNEL_VMA`（又一次 higher-half 转换），占用的字节数向上取整到页边界。这里用了 `((bitmap_size_ + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE` 来计算——先算需要几页，再乘以页大小，得到实际占用的字节数。

```cpp
    // Step 8: Print statistics
    uint64_t total_mb = total_pages_ * PAGE_SIZE / (1024 * 1024);
    uint64_t free_mb  = free_pages_ * PAGE_SIZE / (1024 * 1024);
    cinux::lib::kprintf("[PMM] Total: %uMB, Free: %uMB\n", total_mb, free_mb);
}
```

第八步打印统计信息，让开发者一眼就能确认 PMM 的初始化是否正常。在 QEMU（128MB 内存）中，你应该看到类似 `[PMM] Total: 128MB, Free: 126MB` 的输出。Free 值等于 Total 减去内核映像、栈和位图占用的那几 MB。

### kernel_main 中的集成

```cpp
// 在 kernel/main.cpp 中的初始化序列
// Step 7: Physical Memory Manager
cinux::mm::g_pmm.init(*boot_info);
```

PMM init 被插入在 BootInfo breakpoint 之后、Framebuffer init 之前。这个位置的选择有两个考量：PMM 不依赖任何输出设备（只需要 BootInfo 和 linker symbol），而后续的 VMM 初始化需要 PMM 来分配页表页面，所以 PMM 越早越好。

## 设计决策

### 决策：保守标记 vs 乐观标记

**问题**: 初始化位图时，应该先假设所有页面都可用（乐观），还是先假设所有页面都不可用（保守）？

**本项目的做法**: 保守标记——先全填 0xFF，再按 E820 逐个解锁。

**备选方案**: 乐观标记——先全填 0x00（所有页面空闲），然后只标记已知被占用的区域（内核映像、栈、MMIO 等）。

**为什么不选备选方案**: 乐观标记要求你完整枚举所有被占用的区域。内核映像你可以通过 linker symbol 算出来，但 BIOS 的临时数据结构、ACPI 表、PCI MMIO 窗口等隐含的被占用区域呢？你永远无法穷举。遗漏一个区域就意味着那个区域被错误分配出去了——可能导致写入 MMIO 寄存器（触发硬件行为不可预测）、覆盖 ACPI 表（导致后续 ACPI 解析出错）、甚至写入 BIOS ROM（如果它是可写的 flash）。保守标记则天然安全：任何被 E820 遗漏的区域都保持"已用"状态，永远不会被分配。

**如果要扩展/改进**: 对于高性能场景，可以考虑只初始化 E820 可用区域对应的位图范围（跳过 0 到 1MB 的浪费部分），或者在 init 完成后把位图从全量覆盖改为按 region 分段的 bitmap array。

### 决策：位图放置在栈之后

**问题**: 位图本身需要占用物理内存，应该放在哪里？

**本项目的做法**: 紧跟内核栈顶（`__kernel_stack_top`），页对齐。

**备选方案**: 在可用内存中找一个独立的区域放置位图（比如最高可用地址的末尾），或者在 init 结束后把位图迁移到通过 PMM 自身分配的页面中。

**为什么不选备选方案**: 把位图放在内核映像之后、栈之后，保证了"内核映像 + 栈 + 位图"在物理内存中是连续的——只占用一段连续的物理地址范围，不会在可用内存中间制造碎片。而且位图通过 higher-half 虚拟地址直接访问，不需要额外的页表映射。如果把位图放在可用内存的末尾，虽然不制造碎片，但增加了地址计算的复杂度。自分配方案则存在"鸡和蛋"的问题——位图还没初始化完怎么分配页面？

**如果要扩展/改进**: 当位图大小增长到需要多个页时（比如管理 4GB 物理内存需要 128KB = 32 页），可以考虑用 buddy allocator 的 bootmem 阶段来管理位图自身的分配，而不是固定放在栈后面。

## 扩展方向

- ⭐ **在 init 后打印详细区域信息**: 遍历 parse_memory_map 的输出，打印每个可用区域的 base、length，以及内核/位图保护的范围。帮助调试。
- ⭐⭐ **多区域位图管理**: 如果 E820 报告了两个不连续的可用区域（中间有 MMIO 洞），当前的位图会把中间的洞也覆盖进去（全 1，不可分配）。可以改为使用多个 bitmap 实例，每个管理一个连续区域，减少位图内存浪费。
- ⭐⭐ **NUMA 感知 PMM**: 在多处理器系统中，物理内存可能分布在不同 NUMA node 上。可以给每个 node 一个独立的 PMM 实例，`alloc_page` 增加 node 参数。

## 参考资料

- OSDev Wiki: [Memory Map (x86)](https://wiki.osdev.org/Memory_Map_(x86)) — PC 低 1MB 物理内存布局约定
- Intel SDM: Vol.3A Section 4.5 — 4KB 页对齐要求
- Intel SDM: Vol.3A Section 2.5 — CR3 保存物理地址，PMM 为 VMM 提供页表页面
- [OSDev Wiki - Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 虚拟-物理地址偏移转换
- [OSDev Wiki - Writing A Page Frame Allocator](https://wiki.osdev.org/Writing_A_Page_Frame_Allocator) — linker symbol 访问模式
- Linux bootmem: [The Linux Bootmem Framework](https://lwn.net/Articles/761215/) — 同样使用保守位图策略
