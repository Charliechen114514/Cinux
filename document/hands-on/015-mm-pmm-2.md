# 015-2 PMM 初始化与位图放置

## 导语

上一章我们把 E820 解析器和位图辅助函数准备好了，现在要把这些零件组装成一个完整的 PMM 初始化流程。这一章要回答的核心问题是：位图应该放在物理内存的哪个位置？初始化时如何保证不把内核、栈、位图自身所在的内存页分配出去？

PMM 的初始化采用的是一种"先全锁、再解锁"的保守策略——先把所有页面标记为已用（位图全填 0xFF），然后逐个解锁 E820 报告的可用区域，最后再把内核映像、栈、位图自身重新标记回已用。这种方式虽然笨拙，但有一个巨大的好处：如果你遗漏了某个需要保护的区域，最坏的结果是那个区域被当成空闲页分配出去了——但如果你反过来用"先全空、再标记已用"的策略，遗漏一个区域就意味着某个本该空闲的页被错误地保护了，而且你很难发现这种"少了可用内存"的 bug。

知识前置：上一章的 `parse_memory_map` 和位图辅助函数必须已经实现。你需要理解 linker symbol 的访问模式（在 tag 006 中我们遇到过——`extern char __kernel_stack_top; addr = &__kernel_stack_top`，取地址而不是直接读值），以及 higher-half 内核的虚拟-物理地址转换。

## 概念精讲

### 保守标记策略："先全锁、再解锁"

设想一下，如果你用"乐观"策略——先假设所有内存都可用，然后逐个标记已知被占用的区域——你需要知道哪些区域被占用了。内核映像占用的区域你可以通过 linker symbol 算出来，BootInfo 结构体的位置你知道在 0x7000，但还有多少隐含的被占用区域？bootloader 的实模式栈、BIOS 的临时数据结构、MMIO 映射区域……你永远无法穷举。

所以保守策略的做法是：第一步，把位图全部填为 0xFF（所有页面标记为已用）。第二步，遍历 `parse_memory_map` 的输出，把每个可用区域的对应 bit 清零（标记为空闲）。注意，位图初始是全 1 的，所以 `mark_region_free` 只会把 E820 确认可用的区域释放出来——任何 E820 没有标记为 type=1 的区域，会自动保持"已用"状态。第三步，在已释放的可用区域内，把内核映像、内核栈和位图本身重新标记为已用——因为这些结构虽然位于"可用 RAM"中，但它们显然不能被分配出去。

这种策略的妙处在于：即使 E820 报告有遗漏（比如某个新硬件的 MMIO 区域没被正确标记为 reserved），那些区域也不会被分配出去，因为它们本来就在"全锁"状态。安全代价是可能浪费一些确实可用但没被 E820 报告的内存——但这对教学 OS 来说完全可以接受。

### 位图的物理放置

位图本身也需要占用物理内存，而且这块内存在初始化完成之前就必须可访问。那么位图放在哪里？

Cinux 的方案是：把位图放在内核栈的紧后面。链接脚本定义了 `__kernel_stack_top` 符号——它标记了内核栈的最高地址（虚拟地址）。位图的起始地址取 `__kernel_stack_top` 向上对齐到页边界后的值。这样做有几个好处：位图和内核映像在物理内存中是连续的（或者紧挨着的），不会浪费碎片空间；位图在 higher-half 虚拟地址空间中可以直接通过虚拟地址访问，不需要额外的映射；对齐到页边界保证了位图不会和栈顶重叠。

位图占用的物理内存大小是 `(highest_page + 7) / 8` 字节（向上取整到字节边界）。对于一个有 128MB 内存的 QEMU 虚拟机，这是 4096 字节——恰好一个页。对于 1GB 内存，大约 32KB，8 个页。位图本身占用的页面也需要在初始化完成后被标记为已用——位图总不能把自己分配出去。

### Higher-Half 地址转换

Cinux 的内核运行在虚拟地址 `0xFFFFFFFF80000000` 以上的空间，但 PMM 操作的是物理地址。所以我们需要在虚拟地址和物理地址之间做转换。

转换公式很简单：物理地址 = 虚拟地址 - KERNEL_VMA。比如位图的虚拟地址是 `__kernel_stack_top` 对齐后的值，减去 `0xFFFFFFFF80000000` 就得到位图的物理地址。同理，内核映像的虚拟结束地址（也就是位图起始虚拟地址）减去 KERNEL_VMA 就得到内核映像的物理结束地址。

内核映像的物理起始地址是 `info.kernel_phys_base`——这是 BootInfo 中记录的值，告诉我们 bootloader 把内核 ELF 加载到了物理内存的哪个位置（通常是 0x1000000 即 16MB）。所以"内核映像 + 栈 + 位图"占用的物理内存范围是从 `kernel_phys_base` 到 `(位图虚拟地址 - KERNEL_VMA)`。

> 参考：[OSDev Wiki - Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
> 参考：Intel SDM Vol.3A Section 2.5 (Control Registers) -- CR3 保存的是物理地址，PMM 提供的物理页面将用于构建页表

### Linker Symbol 访问模式

这是我们在 tag 006 中踩过的大坑，这里再强调一遍。链接脚本中定义的符号（比如 `__kernel_end`、`__kernel_stack_top`）本质上是地址常量——链接器把它们当作"在这个地址有一个东西"，但并不真的在那里分配了内存。在 C/C++ 代码中访问这些符号的正确方式是：

声明为 `extern char __kernel_stack_top`（注意是 char，不是 char 数组或者函数），然后通过取地址操作 `&__kernel_stack_top` 获得它的地址值。如果你直接访问 `__kernel_stack_top`（不加 &），编译器会试图读取那个地址处的内存内容——你拿到的是一个随机值而不是地址本身。这个陷阱极其隐蔽，因为编译不会报错，运行时只会产生错误的地址。

> 参考：[OSDev Wiki - Writing A Page Frame Allocator](https://wiki.osdev.org/Writing_A_Page_Frame_Allocator) -- 使用 `endkernel` 符号的经典模式

## 动手实现

### Step 1: 实现内部辅助方法 mark_region_used 和 mark_region_free

**目标**: 编写两个内部方法，用于在位图中标记一段物理内存区域的占用状态。

**设计思路**: 这两个方法接受物理地址和长度，把它们转换为页索引范围，然后逐页调用位图辅助函数。`mark_region_used` 把范围内的每个空闲页标记为已用（只处理当前状态为"空闲"的页，避免重复计数），同时减少 `free_pages_` 计数。`mark_region_free` 正相反，把范围内的每个已用页标记为空闲，同时增加 `free_pages_` 计数。

页索引的计算方式：起始页 = 物理地址 / PAGE_SIZE（向下取整），结束页 = (物理地址 + 长度 + PAGE_SIZE - 1) / PAGE_SIZE（向上取整）。然后遍历从起始页到结束页（不含）的每一页，检查当前状态后再翻转——这样做是为了保证 `free_pages_` 计数的精确性。如果不做"检查当前状态"这一步，对同一个页连续调用两次 `mark_region_free` 会导致 `free_pages_` 被多加一次。

遍历时还需要一个边界检查：`p < highest_page_`。因为传入的物理地址可能超出 PMM 管理的范围（比如 MMIO 区域在高地址），如果不做检查就会越界访问位图。

**实现约束**: 两个方法都是 PMM 类的 private 成员。使用上一章定义的 `bm_test`、`bm_set`、`bm_clear` 辅助函数。放在 `cinux::mm` 命名空间中。

**踩坑预警**: 结束页的计算是向上取整——`(phys + length + PAGE_SIZE - 1) / PAGE_SIZE`。如果你不小心写成了 `(phys + length) / PAGE_SIZE`，那么当一个区域恰好结束在某页边界上时（比如物理地址 0x100000 + 长度 0x1000），最后一页会被漏掉——那页的 bit 没被 set，可能被后续的分配器分配出去，然后两个系统组件就会"共享"同一个物理页，后果不堪设想。

**验证**: 这个方法在 `init` 流程中被调用，暂时没有独立的验证手段。它的正确性会在最终的 PMM 统计输出中体现——如果 `free_pages_` 和可用内存的总量吻合，说明标记逻辑是对的。

### Step 2: 实现 PMM::init 主流程

**目标**: 完成从 BootInfo 到完整位图的八步初始化序列。

**设计思路**: init 函数的整体流程可以概括为八步。

第一步，调用上一章实现的 `parse_memory_map`，从 BootInfo 中提取可用内存区域列表。输出到一个局部的 `MemoryRegion` 数组（最多 32 个条目），这个数组是在栈上分配的，init 完成后就丢弃了。

第二步，遍历所有可用区域，找出最高的物理地址。这个值决定了位图需要覆盖多大的范围——位图必须能索引从 0 到最高地址的所有页。`highest_page_ = max_addr / PAGE_SIZE`，`total_pages_ = highest_page_`（总页数等于最高页号，因为页号从 0 开始），`bitmap_size_ = (highest_page_ + 7) / 8`（位图字节数，向上取整到字节边界）。

第三步，确定位图的放置位置。取 `__kernel_stack_top` 的地址值，向上对齐到页边界，这就是位图的虚拟起始地址。`bitmap_` 指针就指向这个虚拟地址——因为 Cinux 的 higher-half 映射已经把这个虚拟地址映射到了对应的物理地址，所以后续通过 `bitmap_` 指针读写位图就是直接操作物理内存。

第四步，保守初始化——把位图的每个字节都设为 0xFF。此时 `free_pages_ = 0`，所有页面都被标记为已用。

第五步，解锁可用区域——遍历 parse_memory_map 的输出，对每个区域调用 `mark_region_free`。这一步结束后，所有 E820 报告为 type=1 的物理页都被标记为空闲了。

第六步，重新保护内核映像和栈。物理范围是从 `info.kernel_phys_base`（内核映像物理起始）到 `(位图虚拟地址 - KERNEL_VMA)`（内核映像 + 栈的物理结束）。调用 `mark_region_used` 把这个范围内的所有页重新标记为已用。

第七步，标记位图自身。位图的物理地址 = 位图虚拟地址 - KERNEL_VMA。位图占用的字节数向上取整到页边界。调用 `mark_region_used` 标记这些页。

第八步，打印统计信息——总内存大小和空闲内存大小。

**实现约束**: 整个函数在 `pmm.cpp` 中实现，是 PMM 类的 public 方法。需要引用两个 linker symbol——`__kernel_end` 和 `__kernel_stack_top`——通过 `extern "C"` 声明（因为链接脚本是 C 级别的符号），然后用 `reinterpret_cast<uintptr_t>(&symbol)` 取地址值。`KERNEL_VMA` 常量在 `pmm.cpp` 中定义为 `0xFFFFFFFF80000000ULL`。统计输出使用 `kprintf`。

**踩坑预警**: 这里最关键的坑是 linker symbol 的取地址操作。`__kernel_stack_top` 声明为 `extern "C" char __kernel_stack_top`，你必须用 `&__kernel_stack_top` 获取地址。如果你不小心写了 `reinterpret_cast<uintptr_t>(__kernel_stack_top)`——没有取地址符——你会读到栈顶地址处的一个字节值（比如 0x00），然后把它当成一个地址来用。这个 bug 在 QEMU 中可能碰巧不 crash（因为 0x00 附近可能碰巧有映射），但在真实硬件上几乎一定会 triple fault。另一个容易忽略的点是第六步中"内核映像 + 栈"的物理结束地址的计算——应该是 `bm_virt - KERNEL_VMA`，而不是 `&__kernel_end - KERNEL_VMA`。`__kernel_end` 标记的是内核映像（.text + .rodata + .data + .bss）的结束位置，但不包括栈。而位图是放在栈之后的，所以位图起始虚拟地址减去 KERNEL_VMA 才是整个"内核占用的物理内存"的结束位置。

**验证**: init 完成后会打印统计信息。在 QEMU 中（默认 128MB 内存），你应该看到类似这样的输出：

```
[PMM] Total: 128MB, Free: 126MB
```

具体的 Free 值取决于内核映像大小和栈大小，通常应该是 Total 减去几 MB。如果 Free 值异常小（比如只有几 MB），说明 `mark_region_used` 覆盖了过大的范围；如果 Free 等于 Total，说明内核区域没有被正确保护。

### Step 3: 在 kernel_main 中集成 PMM 初始化

**目标**: 将 PMM init 插入到内核启动流程的正确位置。

**设计思路**: 回顾当前的 kernel_main 初始化序列：Serial -> GDT -> IDT -> PIC -> IRQ handlers -> PIT -> BootInfo breakpoint -> Framebuffer -> Font -> Console -> Keyboard -> unmask + sti。PMM 应该插在哪里？

答案是：在 BootInfo breakpoint 之后、Framebuffer 之前。具体来说，顺序变成：BootInfo breakpoint -> **PMM init** -> Framebuffer -> Font -> Console -> Keyboard -> unmask + sti。之所以放在 Framebuffer 之前，有两个原因。第一，PMM 的初始化不依赖任何输出设备——它只需要 BootInfo 和 linker symbol。第二，将来的 VMM（虚拟内存管理器）初始化需要 PMM 来分配页表页面，而 VMM 的初始化可能会在很早的位置（比如 Framebuffer 的 MMIO 映射就需要页表），所以 PMM 越早越好。

`boot_info` 指针在之前的 breakpoint 代码中已经通过 `reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS)` 获取了。我们只需要在这个指针有效之后、进入图形初始化之前，调用 `cinux::mm::g_pmm.init(*boot_info)` 就行了。

**实现约束**: 在 `kernel/main.cpp` 中，include `kernel/mm/pmm.hpp`，然后在使用 `boot_info` 的第一个位置之后插入 `cinux::mm::g_pmm.init(*boot_info)` 调用。同时更新文件顶部的注释中的初始化步骤编号——PMM 变成第 7 步，后面的步骤依次后移。注意不要在 init 前后加多余的空行——保持和现有代码风格一致。

**踩坑预警**: 千万不要在 `sti` 之后再调用 PMM init。虽然当前的 PMM init 不涉及中断操作，但 init 里面会大量写入位图内存——如果此时有定时器中断或者键盘中断在异步触发，虽然它们不会直接干扰位图（因为中断 handler 不碰 PMM），但保持"所有硬件初始化在关中断状态下完成"是一个好习惯。另外，`boot_info` 指针必须在 init 之前就已经被正确设置——如果你不小心把 `auto* boot_info = ...` 那行移到了 init 调用之后，init 就会读到一个未初始化的指针，大概率 triple fault。

**验证**: 构建并在 QEMU 中启动内核，串口输出应该包含 PMM 的统计行：

```
[PMM] Total: 128MB, Free: 126MB
```

而且这行应该出现在 Framebuffer 初始化日志之前。如果内核在 PMM init 处 crash，用 GDB 在 `PMM::init` 入口设断点，单步跟踪看是哪一步出了问题——最可能的原因是 linker symbol 取地址错误或者位图写入了不可访问的地址。

## 构建与运行

确保 `kernel/CMakeLists.txt` 中 `big_kernel_common` 库包含了 `mm/pmm.cpp`，`kernel/main.cpp` 中 include 了 `kernel/mm/pmm.hpp` 并调用了 `g_pmm.init`。构建并运行 QEMU：

```
cmake --build build && ./build/big_kernel.elf
```

串口输出中应该看到 `[PMM]` 开头的统计行，并且内核继续正常初始化 Framebuffer、Console、Keyboard。如果一切正常，你的内核现在知道这台机器有多少物理内存，也知道了哪些页面可以被安全分配。

如果你想确认位图确实被正确放置了，可以在 init 完成后加一条 kprintf 打印位图的虚拟地址和物理地址——虚拟地址应该在 `__kernel_stack_top` 之后（对齐到页边界），物理地址应该在内核映像之后。不过这个调试打印在验证完成后记得删掉。

## 调试技巧

**PMM init 导致 triple fault**: 绝大多数情况是位图写入了一个不可访问的地址。用 GDB 在 `PMM::init` 入口设断点，检查 `bitmap_` 指针的值——它应该是一个 higher-half 虚拟地址（`0xFFFFFFFF8XXXXXXX` 范围）。如果这个地址看起来不对，检查 `__kernel_stack_top` 的取地址操作是否正确。你也可以在 GDB 中手动计算 `&__kernel_stack_top` 的值，看它是否落在链接脚本中定义的栈区域范围内。

**Free 值明显偏小（比如只有几 MB）**: 检查 `mark_region_used` 的参数。最可能的问题是第六步中"内核映像 + 栈"的结束地址算错了——如果你用了 `__kernel_end` 而不是 `bm_virt - KERNEL_VMA`，那么栈占用的页就没被算进内核区域，但这些页在第五步已经被标记为空闲了，又没在第六步被重新保护——所以它们"看起来"可用，但实际上栈正在用。这不会立刻 crash，但一旦分配器把这些页分配出去了，栈数据就会被覆盖，内核行为变得完全不可预测。

**Free 值等于 Total（内核区域没有被保护）**: 说明第六步的 `mark_region_used` 没有生效。检查 `info.kernel_phys_base` 的值——如果 BootInfo 结构体没有被正确填写（比如 bootloader 没有设置这个字段），它的值可能是 0，那么 `mark_region_used(0, ...)` 会把物理地址 0 开始的一大段标记为已用，但这不是我们想要的。用 kprintf 打印 `info.kernel_phys_base` 和 `bm_virt - KERNEL_VMA` 的值来确认。

**构建时 undefined reference to __kernel_stack_top**: 这说明链接脚本中没有定义 `__kernel_stack_top` 符号。检查你的 `kernel.ld`——它应该在大内核栈的定义之后导出这个符号。如果符号名有变化（比如改成了 `__big_kernel_stack_top`），`pmm.cpp` 中的 extern 声明也要同步修改。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 保守标记策略 | 先全填 0xFF（全用），再按 E820 解锁可用区域，最后保护内核区域 |
| 位图放置 | 紧跟内核栈顶（`__kernel_stack_top`），向上对齐到页边界 |
| Higher-Half 转换 | 物理地址 = 虚拟地址 - KERNEL_VMA (0xFFFFFFFF80000000) |
| 内核区域保护 | kernel_phys_base 到 (位图虚拟地址 - KERNEL_VMA) 标记为已用 |
| 位图自身保护 | 位图占用的页也必须标记为已用 |
| Linker Symbol | `extern "C" char sym; addr = &sym`，取地址而不是直接读值 |
| init 在 kernel_main 中的位置 | BootInfo breakpoint 之后、Framebuffer 之前 |
