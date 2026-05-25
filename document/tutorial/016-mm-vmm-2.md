---
title: 016-mm-vmm-2 · 虚拟内存管理
---

# 016-2 四级页表遍历：VMM 的 map、unmap 与 translate

> 标签：VMM, 页表遍历, 虚拟内存, map/unmap/translate
> 前置：[016-1 从大页到 4KB 页：x86-64 页表项详解](016-mm-vmm-1.md)

## 前言

上一篇我们把页表项的每一根头发丝都理清楚了——`PageEntry` 联合体、分页常量、TLB 刷新，该有的零件全部就位。但你有没有想过，这些零件堆在桌上并不等于房子盖好了。试想一个场景：内核需要把虚拟地址 `0xFFFFFFFF80200000` 映射到物理地址 `0x200000`，你手上有 `PageEntry`、有 `PML4_INDEX` 宏、有 `flush_tlb`，然后呢？你得从 CR3 里读出 PML4 物理地址，转换成虚拟地址，用索引查到 PDPT 条目，再从条目里提取下一级物理地址，再转换成虚拟地址……重复四次才能走到 PT。这段逻辑你写一次就够痛苦了，结果 `map` 要写、`unmap` 也要写、`translate` 还要写——三次几乎相同但不完全相同的四级遍历。如果每个操作都从零展开，代码量爆炸不说，三份逻辑之间微妙的不一致几乎是迟早的事。

这就是 VMM 类存在的意义。它把"四级页表遍历"这个反复出现的模式封装成了一个 `walk_level` 辅助函数，然后在它上面构建 `map`、`unmap`、`translate` 三个语义截然不同但遍历逻辑共用的操作。今天这篇我们就来拆解这个封装：从 `phys_to_virt` 这个地址转换桥梁开始，到 `walk_level` 的两路分支设计，再到 VMM 初始化、map/unmap/translate 的完整实现。最后我们会把 Cinux 的方案和 xv6 的 `walk()`、Linux 的 `pgtable` 子系统做一个深度对比——你会发现不同内核在"走页表"这件事上的设计选择惊人地相似，但锁策略和页表分配时机又各有各的道理。

## 环境说明

和上一篇完全一致：x86-64 QEMU、GCC cross-compiler、higher-half 内核（`KERNEL_VMA = 0xFFFFFFFF80000000`）。PMM 已在 tag 015 就绪，可以随时分配 4KB 物理页。bootloader 建立的四级页表仍然生效——恒等映射 + higher-half 映射 + MMIO 映射都在，VMM 在此基础上扩展。

## phys_to_virt 与地址空间布局

我们直接进 `vmm.cpp` 的第一段代码，因为它是后面所有操作的前提。

```cpp
constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}
```

三行代码，但背后的东西值得好好说说。页表条目里存的是物理地址——这不是设计选择，而是硬件强制要求。Intel SDM Vol.3A Section 4.5.4（pp.4-22 到 4-25）写得明明白白：MMU 做地址翻译时，每一步从 PTE 中提取的 bit 51:12 都是下一级页表的物理基地址，CPU 根本不知道虚拟地址是什么。但我们的内核代码运行在虚拟地址空间里，想读写一个页表的内容就必须用虚拟地址去访问它。所以每次从 PTE 中拿到一个物理地址，都得先加上 `KERNEL_VMA` 转成虚拟地址，然后才能用指针去解引用。

`phys + KERNEL_VMA` 能直接用，靠的是 bootloader 在启动阶段做的一件事：用 2MB 大页把物理内存逐一映射到 `KERNEL_VMA` 以上的虚拟地址空间。物理地址 `0x3000` 对应虚拟地址 `0xFFFFFFFF80003000`，物理地址 `0x100000` 对应 `0xFFFFFFFF80100000`——一个线性偏移搞定，不需要查找表，不需要处理特殊情况。这种 higher-half 直接映射在小型内核里是最常见的做法，OSDev Wiki 的 [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) 页面对此有详细介绍。Linux 则用了更复杂的机制——它的 `phys_to_virt` 背后是 FLATMEM/SPARSEMEM 模型的 `mem_map` 数组，能处理物理内存有"洞"的情况，适合大内存服务器，但对 Cinux 来说完全杀鸡用牛刀。

你可能会问：返回类型为什么是 `PageEntry*` 而不是 `void*` 或 `uint64_t*`？因为 `phys_to_virt` 的调用场景几乎全是"拿到下一级页表的虚拟地址，然后按 `PageEntry` 数组去索引"，所以直接返回 `PageEntry*` 省去了调用处的强制转换。这种"返回类型跟随使用场景"的设计在内核代码里很常见，属于一种小但实用的工程决策。

## 四级页表遍历——walk_level

`walk_level` 是整个 VMM 的灵魂，我们先看完整代码再拆解。

```cpp
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc) {
    PageEntry& entry = table[index];

    if (entry.is_present()) {
        return phys_to_virt(entry.phys_addr());
    }

    if (!should_alloc) {
        return nullptr;
    }

    uint64_t new_page = cinux::mm::g_pmm.alloc_page();
    if (new_page == 0) {
        return nullptr;
    }

    // Zero the new page table
    auto* new_table = phys_to_virt(new_page);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        new_table[i].raw = 0;
    }

    entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE;
    return new_table;
}
```

这个函数封装了"走一级页表"的完整语义，被 `map`、`unmap`、`translate` 三个操作复用。参数设计很精练：`table` 是当前级页表的虚拟地址（已经通过 `phys_to_virt` 转换好了），`index` 是由 `PML4_INDEX`/`PDPT_INDEX`/`PD_INDEX` 之中的一个算出来的索引值，`should_alloc` 控制遇到空条目时是否分配新页表。

第一个分支——`entry.is_present()`——处理条目已存在的情况。直接取物理地址、转换成虚拟地址返回，整条路径零分配、零副作用。

第二个分支——`!should_alloc`——是"只读模式"的快速退出。`unmap` 和 `translate` 调用 `walk_level` 时传 `false`，意思是"中间任何一级断了我就放弃，不分配新东西"。这让 `walk_level` 在只读场景下完全无副作用。

第三个分支是真正的重头戏：分配新页表。从 PMM 拿一个 4KB 物理页，然后**必须清零**。这一点怎么强调都不过分——新分配的物理页可能包含上一次使用留下的任意数据，那些垃圾会被硬件当作 PTE 解释。如果某个垃圾字段的 bit 0 恰好是 1，硬件就认为那是一个 Present 的映射，指向一个随机的物理地址，后果不可预测。xv6 的 `walk()` 在分配新页表后做了 `memset(0)`，Linux 的页表分配路径同样会清零——这不是优化，是正确性的硬性要求，Intel SDM Vol.3A Section 4.5.4（p.4-24）明确指出，PTE 中 bit 51:12 以外的保留位如果被设为 1 会触发保留位 #PF。

清零之后，把新页的物理地址加上 Present + Writable 标志写入当前条目。xv6 的 `walk` 里用 `alloc` 布尔参数控制分配行为，和 Cinux 的 `should_alloc` 完全同一思路——看来在这个问题上，MIT 的教学内核和我们的玩具内核独立收敛到了同一个设计。

## VMM 初始化

```cpp
void VMM::init() {
    kernel_pml4_ = cinux::arch::read_cr3();
    cinux::lib::kprintf("[VMM] Initialised, kernel PML4 at phys %p\n",
                        reinterpret_cast<void*>(kernel_pml4_));
}
```

`init` 做的事情简单到让你可能觉得不值一提：读 CR3，存起来，打印一行日志。但这一步的时机有讲究。在 Cinux 的 `kernel_main` 里，调用序列是 PMM 初始化在前、VMM 初始化在后——因为 VMM 的 `map` 在分配中间页表时要调 `PMM::alloc_page`，PMM 不就绪 `map` 就会失败。CR3 里存的是什么？是 bootloader 设置好的内核 PML4 的物理基地址，Intel SDM Vol.3A Section 2.5（pp.2-14 到 2-16）定义了 CR3 的格式：bit 51:12 是 PML4 的物理地址，低 12 位是 PCID 或保留。`read_cr3()` 返回的值直接就是页表根，不需要做任何移位或掩码处理。

这种"继承 bootloader 页表"的做法并不独特。Linux 的 `startup_64` 入口同样使用 bootloader 提供的初始页表，后续才通过 `init_memory_mapping()` 逐步重建。区别在于 Linux 最终会完全替换掉 bootloader 的页表，而 Cinux 在 tag 016 阶段只是在原来的基础上做增量——PML4 里 bootloader 建好的那些条目（恒等映射、higher-half 映射）原封不动保留，VMM 的 `map` 只是往空 slot 里填新条目。

## map：建立虚拟到物理的映射

```cpp
bool VMM::map(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t* pml4) {
    uint64_t pml4_phys  = pml4 ? *pml4 : kernel_pml4_;
    auto*    pml4_table = phys_to_virt(pml4_phys);

    auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true);
    if (!pdpt) return false;

    auto* pd = walk_level(pdpt, PDPT_INDEX(virt), true);
    if (!pd) return false;

    auto* pt = walk_level(pd, PD_INDEX(virt), true);
    if (!pt) return false;

    uint64_t pt_idx = PT_INDEX(virt);
    pt[pt_idx].raw  = (phys & ADDR_MASK) | (flags & ~ADDR_MASK);

    cinux::arch::flush_tlb(virt);
    return true;
}
```

这段代码是 Intel SDM Vol.3A Section 4.5.4 描述的四级遍历的直接翻译。从 PML4 出发，经过 PDPT、PD、PT，每一步都调 `walk_level` 并传 `should_alloc=true`——任何一级的中间页表不存在就从 PMM 分配一个。四次 `walk_level` 的调用依次用 `PML4_INDEX`、`PDPT_INDEX`、`PD_INDEX`、`PT_INDEX` 从虚拟地址中提取对应层级的索引，每个宏做的事情就是把虚拟地址的对应 9-bit 字段移位到低位。OSDev Wiki 的 [Paging (64-bit)](https://wiki.osdev.org/Paging) 页面有一张经典的地址分解图，bit 47:39 给 PML4、38:30 给 PDPT、29:21 给 PD、20:12 给 PT、11:0 是页内偏移。

走到 PT 之后，`pt[pt_idx].raw = (phys & ADDR_MASK) | (flags & ~ADDR_MASK)` 把物理地址和标志位拼在一起写入 PTE。`ADDR_MASK = 0x000FFFFFFFFFF000` 覆盖 bit 51:12（物理页帧地址），`~ADDR_MASK` 覆盖其余位置（标志位），两者不重叠，OR 在一起就是完整的 PTE 值。最后 `flush_tlb(virt)` 用 `invlpg` 指令让这一页的 TLB 条目失效——Intel SDM Vol.3A Section 4.10（pp.4-43 到 4-52）解释了为什么这步不能省：TLB 是 CPU 内部的缓存，页表改了之后 TLB 里的旧条目不会自动失效，如果不刷，CPU 会继续用旧的翻译结果，你的 map 就形同虚设。

关于 `pml4` 参数：传 `nullptr` 使用内核默认 PML4，传一个非空指针则解引用它拿到自定义的页表根物理地址。这个参数是为了未来多进程支持预留的接口——每个进程有自己的 PML4（对应 Linux 的 `mm_struct->pgd`），进程切换时把新进程的 PML4 物理地址写入 CR3 就行了。

## unmap 与 translate

`unmap` 和 `map` 结构几乎完全对称，但语义截然相反。

```cpp
void VMM::unmap(uint64_t virt, uint64_t* pml4) {
    uint64_t pml4_phys  = pml4 ? *pml4 : kernel_pml4_;
    auto*    pml4_table = phys_to_virt(pml4_phys);
    auto*    pdpt       = walk_level(pml4_table, PML4_INDEX(virt), false);
    if (!pdpt) return;
    auto* pd = walk_level(pdpt, PDPT_INDEX(virt), false);
    if (!pd) return;
    auto* pt = walk_level(pd, PD_INDEX(virt), false);
    if (!pt) return;

    uint64_t pt_idx = PT_INDEX(virt);
    pt[pt_idx].raw  = 0;

    cinux::arch::flush_tlb(virt);
}
```

所有 `walk_level` 调用都传 `should_alloc=false`——中间任何一级找不到就直接 return，对不存在的映射执行 unmap 是安全的空操作。找到 PT 后把对应条目清零，再刷 TLB。你会发现 `unmap` 不释放被映射的物理页，也不回收中间页表。这是有意为之的设计：映射管理和物理内存生命周期管理是两个独立的关注点。调用者可能需要"解映射但保留物理页"——比如把一个物理页从虚拟地址 A 移到虚拟地址 B，先 `unmap(A)` 再 `map(B, 同一物理页)`。Linux 的 `pte_clear` 和 xv6 的 `uvmunmap` 也遵循同样的分离原则。中间页表的回收则推迟到进程退出时统一处理，避免在每次 unmap 里维护引用计数的复杂性。

`translate` 是最纯粹的四步走——只读不写，零副作用。

```cpp
uint64_t VMM::translate(uint64_t virt, uint64_t* pml4) {
    uint64_t pml4_phys  = pml4 ? *pml4 : kernel_pml4_;
    auto*    pml4_table = phys_to_virt(pml4_phys);
    auto*    pdpt       = walk_level(pml4_table, PML4_INDEX(virt), false);
    if (!pdpt) return 0;
    auto* pd = walk_level(pdpt, PDPT_INDEX(virt), false);
    if (!pd) return 0;
    auto* pt = walk_level(pd, PD_INDEX(virt), false);
    if (!pt) return 0;

    PageEntry& entry = pt[PT_INDEX(virt)];
    if (!entry.is_present()) return 0;

    uint64_t offset = virt & (PAGE_SIZE - 1);
    return entry.phys_addr() | offset;
}
```

前面四级遍历和 `unmap` 完全一致，区别在最后一步：从 PTE 提取物理地址，然后拼上虚拟地址的页内偏移。`translate` 的语义是"这个虚拟地址对应哪个物理地址"，而不是"这个虚拟页对应哪个物理页帧"——如果你问 `0x20000123` 映射到哪，而页帧是 `0x100000`，返回值应该是 `0x100123`，`0x123` 的偏移不能丢。返回 0 表示"未映射"，这隐含了一个假设：物理地址 0 不会被映射，因为它是实模式中断向量表区域，PMM 初始化时就排除了。

## 并发考量

在 tag 016 阶段，VMM 不包含任何锁机制。这在单核 QEMU 环境下是完全安全的——不存在两个 CPU 同时操作页表的可能性。但这个设计在多核环境下会出问题：想象一下 CPU 0 正在 `map` 的中途——刚分配了一个新的 PDPT 页表但还没来得及在 PML4 里链接它——此时 CPU 1 触发了一次缺页异常，缺页处理函数也要调 `map`。两个 CPU 同时往同一张 PML4 写条目，结果就是数据损坏。

未来的多核支持需要引入自旋锁保护 map/unmap 操作。但有一个微妙的地方：缺页处理函数运行在中断门下（IF=0），此时不可能有并发的 VMM 访问，如果强行拿锁反而可能在递归缺页时死锁。所以未来的设计会采用"公共接口加锁、内部实现无锁"的分层——类似 Linux 在 `mm_struct` 里放一把 `mmap_lock`（读写信号量），`mmap`/`munmap` 走写路径、缺页处理走读路径。xv6 更粗暴——直接在 `proc` 结构体里放 `int lock`，所有页表操作都在锁保护下进行。

Cinux 的 `walk_level` 使用 `g_pmm.alloc_page()` 而非某个带锁的变体，这也是因为 tag 016 的单核环境不需要考虑并发。当引入锁之后，`walk_level` 会调用 `alloc_page_locked()`（PMM 的无锁版本），因为调用者已经持有了 VMM 的锁——如果在 `walk_level` 内部再次获取 PMM 的锁，就可能导致死锁（如果 PMM 的锁和 VMM 的锁不是同一把）。

## 设计对比：Cinux vs xv6 vs Linux

我们先看 xv6。xv6 的核心遍历函数 `walk(pagetable_t, uint64_t va, int alloc)` 和 Cinux 的 `walk_level` 在结构上几乎一模一样——循环遍历页表层级（xv6 是 RISC-V Sv39 的三级，Cinux 是 x86-64 的四级），遇到缺失条目时根据 `alloc` 参数决定是否分配新页表，分配后清零再链接。区别在于 xv6 把四级展开写成了一个三迭代的 `for` 循环，而 Cinux 用四次独立的 `walk_level` 调用。两种写法各有利弊：循环更紧凑，但 Cinux 的展开写法让每一级的语义更清晰——你可以直接看到"PML4 走到 PDPT、PDPT 走到 PD、PD 走到 PT"这个链式过程，调试时也更容易在某一层打断点。xv6 的 `mappages()` 支持范围映射（一次映射连续多页），而 Cinux 的 `map()` 只处理单页——更简单但也意味着映射大块内存需要调用者自己写循环。xv6 在 `mappages` 里对 remap 情况直接 `panic`，态度比 Cinux 坚决得多（Cinux 会静默覆盖旧映射）。源码见 [xv6-riscv kernel/vm.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c)。

再看 Linux。Linux 的 x86-64 分页最多五级（PGD->P4D->PUD->PMD->PTE），比 Cinux 多一层 P4D，但通过 P4D folding 在硬件只支持四级时退化成四级。Linux 的页表分配分散在 `arch/x86/mm/pgtable.c` 的各个函数里——`pte_alloc`、`pmd_alloc`、`pud_alloc`、`p4d_alloc`、`pgd_alloc`——每一级有自己独立的分配函数，不像 Cinux 用一个 `walk_level` 打天下。Linux 的好处是每一级可以有定制的分配逻辑（比如 PMD 可以选择分配大页），但代价是代码量和复杂度成倍增长。在锁策略上，Linux 的 `pgtable` 操作通常受 `mm->mmap_lock` 或 `pte_lockptr(ptepage)` 保护，粒度细到单个 PT 页——Cinux 和 xv6 的粗粒度锁在对比之下显得很"学生项目"，但对教学目的完全足够。Linux 的地址翻译不走软件遍历——它依赖硬件 MMU 的页表行走单元（Page Walk Unit），内核只在特殊场景（如 `follow_page`、`get_user_page`）才做软件遍历，而 Cinux 的 `translate` 每次都走四级遍历，性能差距显而易见。源码见 [Linux arch/x86/mm/pgtable.c](https://github.com/torvalds/linux/blob/master/arch/x86/mm/pgtable.c)，Mel Gorman 的 [Understanding the Linux Virtual Memory Manager](https://www.kernel.org/doc/gorman/html/understand/understand006.html) 是这个话题的经典参考。

SerenityOS 则走了另一条路。它的 `MemoryManager` 是一个全局单例，内部维护了一个 region-based 的虚拟内存分配器——不只是 map/unmap 这种底层操作，还管理虚拟地址范围的分配和回收。Cinux 的 VMM 只管"这一个 4KB 页怎么映射"，不管"哪一段虚拟地址空间是空闲的"。SerenityOS 把这两层——底层的页表操作和上层的地址空间管理——合并在 `MemoryManager` 里，好处是接口统一，代价是耦合度高。Cinux 的分层策略（PMM 管物理页、VMM 管页表映射、未来的 VM 管虚拟地址范围）更接近 Linux 的 `pmm`/`pgtable`/`vm_area_struct` 三层架构，扩展性更好。

## 收尾

到这里，Cinux 的虚拟内存管理器已经具备了三大核心能力：`map` 能建立映射并在途中自动分配缺失的中间页表，`unmap` 能安全地清除映射而不管物理页的生命周期，`translate` 能查询任意虚拟地址的物理映射。`walk_level` 把四级遍历的公共逻辑提取成一个干净的辅助函数，让三个操作的代码量都保持在十几行的范围内。`phys_to_virt` 桥接了物理地址和虚拟地址两个世界，是整个系统能运转的暗桩。

但这个 VMM 还不完整。你一定注意到了——`map` 能分配新页表，`unmap` 能清除映射，但如果内核访问了一个还没映射的地址会怎样？CPU 会抛出缺页异常（#PF），而我们现在的缺页处理函数只会 panic。下一篇就会讲到：如何利用这个异常实现按需分页——在访问的那一刻才分配物理页、建立映射，让内存的分配时机从"启动时全量映射"推迟到"真正需要时才映射"。

## 参考资料

- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：4 级页表遍历的完整描述，PML4->PDPT->PD->PT 索引方式和物理地址提取
- Intel SDM Vol.3A Section 4.10, pp.4-43 to 4-52：TLB 缓存机制和 INVLPG 指令语义
- Intel SDM Vol.3A Section 2.5, pp.2-14 到 2-16：CR3 寄存器格式
- OSDev Wiki: [Paging (64-bit)](https://wiki.osdev.org/Paging) — 4 级分页虚拟地址分解
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — Higher-half 映射与 phys_to_virt 原理
- xv6 RISC-V: [kernel/vm.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c) — `walk()` 和 `mappages()` 实现
- Linux: [arch/x86/mm/pgtable.c](https://github.com/torvalds/linux/blob/master/arch/x86/mm/pgtable.c) — x86-64 页表分配与管理
- Mel Gorman: [Understanding the Linux Virtual Memory Manager](https://www.kernel.org/doc/gorman/html/understand/understand006.html)
- SerenityOS: [Kernel/Memory/](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/) — Region-based 虚拟内存管理
