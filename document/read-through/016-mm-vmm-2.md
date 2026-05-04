# 016-2 通读：VMM 核心——映射、解映射与翻译的工程实现

## 概览

本文是 tag `016_mm_vmm` 三篇通读教程的第二篇，聚焦于虚拟内存管理器的核心算法：VMM 类的 `map`/`unmap`/`translate` 三大操作及其底层支撑。如果说上一篇讲的 `PageEntry` 和分页常量是砖块和图纸，那这篇要讲的就是用这些砖块盖房子的施工过程——四级页表遍历、物理地址到虚拟地址的转换、中间页表的按需分配，以及内核启动时 VMM 的初始化流程。

我们按以下顺序展开：先看 `vmm.hpp` 中 VMM 类的接口设计，理解它暴露了哪些能力和为什么这么设计；然后深入 `vmm.cpp` 的实现，从 `phys_to_virt` 和 `walk_level` 这两个内部辅助函数开始——它们是整个 VMM 算法的灵魂；接着逐一讲解 `init`、`map`、`unmap`、`translate` 的完整代码；最后看 `main.cpp` 中 VMM 初始化在启动序列中的位置。

## 架构图

```
    VMM::map(virt, phys, flags, pml4=nullptr)
         │
         │  1. 解析 pml4 参数 → 选择页表根
         │  2. phys_to_virt(pml4_phys) → 拿到 PML4 虚拟地址
         │
         ▼
    ┌──────────┐    walk_level(should_alloc=true)
    │   PML4   │────────────────────────────────────────┐
    │[PML4_IDX]│  entry present? → 跟踪物理地址         │
    └──────────┘  entry absent?  → PMM 分配 + 清零     │
         │                                             │
         ▼                                             │
    ┌──────────┐    walk_level(should_alloc=true)      │
    │   PDPT   │────────────────────────────┐          │
    │[PDPT_IDX]│                            │          │
    └──────────┘                            │          │
         │                                  │          │
         ▼                                  │          │
    ┌──────────┐    walk_level(should_alloc=true)     │
    │    PD    │──────────────────────┐     │          │
    │[PD_IDX]  │                     │     │          │
    └──────────┘                     │     │          │
         │                           │     │          │
         ▼                           │     │          │
    ┌──────────┐                     │     │          │
    │    PT    │  pt[idx].raw =      │     │          │
    │[PT_IDX]  │  (phys & ADDR_MASK) │     │          │
    │          │  | (flags & ~ADDR)  │     │          │
    └──────────┘                     │     │          │
         │                           │     │          │
         ▼                           ▼     ▼          ▼
    flush_tlb(virt)            PMM::alloc_page()
         │                           │
         ▼                           ▼
    返回 true/false          phys_to_virt(phys)
                                    │
    ────────────────────────────────┤
    VMM::unmap(virt)                │
         │  walk_level(should_alloc=false) → 找不到就 return
         │  pt[idx].raw = 0
         │  flush_tlb(virt)
         ▼
    VMM::translate(virt)
         │  walk_level(should_alloc=false) → 找不到就 return 0
         │  entry.is_present()? → return entry.phys_addr() | offset
         ▼

    ┌──────────────────────────────────────────────┐
    │           Higher-Half 地址翻译               │
    │                                              │
    │  PTE 中存的是物理地址 (MMU 硬件需要)         │
    │  内核代码需要虚拟地址才能读写页表内容        │
    │                                              │
    │  phys_to_virt(phys) = phys + KERNEL_VMA      │
    │  KERNEL_VMA = 0xFFFFFFFF80000000             │
    │                                              │
    │  例: phys=0x3000 → virt=0xFFFFFFFF80003000   │
    └──────────────────────────────────────────────┘
```

## 代码精讲

### vmm.hpp —— VMM 类的公共接口

我们先看 VMM 类的头文件，理解它的 API 设计哲学：

```cpp
/**
 * @file kernel/mm/vmm.hpp
 * @brief Virtual Memory Manager
 *
 * Provides map / unmap / translate operations over the 4-level page table
 * hierarchy (PML4 -> PDPT -> PD -> PT -> Page).  When intermediate tables
 * are missing during a map, new pages are allocated from the PMM and
 * zeroed automatically.
 *
 * The VMM class is an instance object (not a static singleton) so that
 * future milestones can manage multiple address spaces.
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stdint.h>
#include "kernel/proc/sync.hpp"

namespace cinux::mm {

class VMM {
public:
    void init();

    bool map(uint64_t virt, uint64_t phys, uint64_t flags,
             uint64_t* pml4 = nullptr);

    bool map_2mb(uint64_t virt, uint64_t phys, uint64_t flags,
                 uint64_t* pml4 = nullptr);

    void unmap(uint64_t virt, uint64_t* pml4 = nullptr);

    bool split_2mb_page(uint64_t virt);

    uint64_t translate(uint64_t virt, uint64_t* pml4 = nullptr);

    bool map_nolock(uint64_t virt, uint64_t phys, uint64_t flags,
                    uint64_t* pml4 = nullptr);

    uint64_t kernel_pml4() const;

private:
    uint64_t              kernel_pml4_{};
    cinux::proc::Spinlock lock_;
};

/// Global VMM instance.
extern VMM g_vmm;

}  // namespace cinux::mm
```

关于这个接口设计，有几个关键的决策值得说道。

VMM 被设计成实例对象而不是纯静态类。当前内核只有一个全局实例 `g_vmm`，但实例化设计意味着未来每个进程可以拥有自己的 VMM 或至少自己的 `pml4` 参数。这正是 `map`/`unmap`/`translate` 的第四个参数 `pml4` 存在的原因——传 `nullptr` 表示使用内核的默认 PML4，传一个地址指针则使用指定的页表根。这个设计在 Linux 中对应的是 `mm_struct->pgd`——每个进程有自己的 PGD（相当于 x86-64 的 PML4），进程切换时重载 CR3 指向新进程的 PGD。

`map_nolock` 是 `map` 的无锁版本，专门为缺页处理函数设计。缺页处理函数运行在中断门下（IF=0），此时不可能有并发的 VMM 访问，所以不需要加锁——如果强行加锁，反而可能在递归缺页时死锁。`split_2mb_page` 和 `map_2mb` 则是处理大页和普通页之间转换的方法，用于 boot 阶段大页映射到运行时 4KB 页的拆分。

私有成员只有两个：`kernel_pml4_` 存储初始化时从 CR3 读到的内核页表根物理地址，`lock_` 是一个自旋锁保护并发的 map/unmap 操作。用类内初始化（`kernel_pml4_{}`）确保全局对象构造时处于确定状态，不需要自定义构造函数。

### phys_to_virt —— 物理地址到虚拟地址的桥梁

现在我们进入 `vmm.cpp` 的实现。第一个要理解的概念是 `phys_to_virt`——它是整个 VMM 能工作的前提条件。

```cpp
namespace cinux::mm {

// ============================================================
// Constants
// ============================================================

constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

// ============================================================
// Global instance
// ============================================================

VMM g_vmm;

// ============================================================
// Internal helpers
// ============================================================
using namespace cinux::arch;

namespace {

/** Convert a physical address to a virtual address via the higher-half mapping. */
PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}
```

这短短四行代码背后藏着一个关键问题：页表条目里存的是物理地址——因为硬件 MMU 做地址翻译时走的是物理内存，它不关心虚拟地址。但我们的内核代码运行在虚拟地址空间里，要用虚拟地址才能读写页表的内容。所以每次拿到一个 PTE 中的物理地址（指向下一级页表），都必须先把它转换成虚拟地址，然后才能通过指针访问那个页表。

`phys + KERNEL_VMA` 利用了 bootloader 建立的 higher-half 直接映射。在 Cinux 的内存布局中，物理地址 `X` 被映射到虚拟地址 `X + 0xFFFFFFFF80000000`。这个映射覆盖了整个物理内存（bootloader 在启动时用 2MB 大页做的恒等映射 + higher-half 映射），所以只要物理地址有效，`phys_to_virt` 就一定能访问到。举个例子：CR3 指向的 PML4 物理地址是 `0x3000`，那么 `phys_to_virt(0x3000)` 返回的就是 `0xFFFFFFFF80003000`，内核通过这个虚拟地址就能读写 PML4 的内容。

### walk_level —— 四级遍历的通用"一脚"

`walk_level` 是 VMM 最核心的内部辅助函数，它封装了"走一级页表"的逻辑，被 `map`/`unmap`/`translate` 复用。

```cpp
/**
 * @brief Walk one level of the page table, allocating if needed
 *
 * @param table   Pointer to the current-level table (virtual address)
 * @param index   Index into this table
 * @param should_alloc  Whether to allocate a new table if the entry is absent
 * @return Pointer to the next-level table, or nullptr on allocation failure
 */
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc,
                      uint64_t user_flag = 0) {
    PageEntry& entry = table[index];
```

参数设计很有讲究。`table` 是当前级页表的虚拟地址（已经通过 `phys_to_virt` 转换过的），`index` 是这一级要查找的索引（由 `PML4_INDEX`/`PDPT_INDEX`/`PD_INDEX` 计算得到）。`should_alloc` 控制行为模式：`map` 操作在遇到缺失的中间表时需要分配新页，而 `unmap` 和 `translate` 只是查找，不分配——找不到就直接返回空。`user_flag` 是一个用于传递 `FLAG_USER` 标志的参数，在分配新中间表时传播用户空间权限。

```cpp
    if (entry.is_present()) {
        if (entry.huge) {
            if (!should_alloc) {
                return nullptr;
            }

            uint64_t big_phys  = entry.phys_addr();
            uint64_t big_flags = entry.raw & ~ADDR_MASK;

            uint64_t new_page = cinux::mm::g_pmm.alloc_page_locked();
            if (new_page == 0) {
                return nullptr;
            }

            auto* new_table = phys_to_virt(new_page);
            for (uint32_t i = 0; i < PT_ENTRIES; i++) {
                new_table[i].raw =
                    (big_phys + static_cast<uint64_t>(i) * PAGE_SIZE) | (big_flags & ~FLAG_HUGE);
            }

            entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
        }
        return phys_to_virt(entry.phys_addr());
    }
```

第一个分支处理"条目已存在"的情况。如果 `is_present()` 为真，说明这一级已经有指向下一级的指针了，直接取物理地址、转成虚拟地址返回即可。

但这里有一个特殊的子情况：`entry.huge` 为真时，表示当前条目是一个大页（2MB 或 1GB），而不是指向下一级页表的指针。这种情况下，如果我们正在做 `map`（`should_alloc = true`），就需要把大页"拆分"成一个新页表——分配一个 4KB 页作为新的下一级页表，把原来大页覆盖的地址范围展开成 512 个 4KB 条目。这就是所谓的"大页拆分"（huge page splitting），bootloader 用 2MB 大页映射的前 1GB 物理内存，在 VMM 需要精确控制某个 4KB 页的映射时就必须先拆分。拆分时保留了原来的标志位（`big_flags & ~FLAG_HUGE` 清除 Huge 位），确保拆分后的 4KB 页和原来的 2MB 大页有相同的访问权限。

```cpp
    if (!should_alloc) {
        return nullptr;
    }

    uint64_t new_page = cinux::mm::g_pmm.alloc_page_locked();
    if (new_page == 0) {
        return nullptr;
    }

    auto* new_table = phys_to_virt(new_page);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        new_table[i].raw = 0;
    }

    entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE | user_flag;
    return new_table;
}

}  // anonymous namespace
```

第二个分支处理"条目不存在且需要分配"的情况。从 PMM 分配一个 4KB 页作为新的下一级页表，然后**必须清零**——这一点怎么强调都不过分。新分配的物理页可能包含上一次使用留下的任意数据，如果不清零，那些垃圾数据会被硬件当作 PTE 来解释，可能导致幽灵映射（指向随机物理地址的条目恰好 Present 位为 1）或者保留位错误（触发 #PF）。xv6 的 `walk()` 函数在分配新页表后也做了 `memset(0)`，Linux 的 `pgtable_ctor` 同样如此——这不是可有可无的优化，是正确性的硬性要求。

分配并清零后，把新页的物理地址加上 Present + Writable 标志写入当前条目，建立父子页表之间的链接。`user_flag` 的传播确保了用户空间的页表中间层也有 User 标志——如果没有这个标志，Ring 3 的代码访问这些页时硬件会在遍历中间级时就触发权限错误，即使最终的 PTE 有 User 标志也没用。

### VMM::init —— 记住页表根

```cpp
void VMM::init() {
    kernel_pml4_ = cinux::arch::read_cr3();
    cinux::lib::kprintf("[VMM] Initialised, kernel PML4 at phys %p\n",
                        reinterpret_cast<void*>(kernel_pml4_));
}
```

`init` 非常简洁：读 CR3，保存到 `kernel_pml4_`。在 Cinux 的启动流程中，CR3 指向的是 bootloader 设置好的内核 PML4——bootloader 已经建立了恒等映射（低地址）和 higher-half 映射（高地址），以及 MMIO 区域的映射。VMM 不需要从零建页表，而是在 bootloader 的基础上扩展。这种"继承 bootloader 页表"的做法在 Linux 中也很常见——`startup_64` 使用 bootloader 的页表，后续通过 `init_memory_mapping()` 逐步替换。

### VMM::map_nolock —— 四级遍历的完整展开

```cpp
bool VMM::map_nolock(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t* pml4) {
    uint64_t pml4_phys  = pml4 ? *pml4 : kernel_pml4_;
    auto*    pml4_table = phys_to_virt(pml4_phys);
    uint64_t user_flag  = flags & FLAG_USER;

    auto* pdpt = walk_level(pml4_table, PML4_INDEX(virt), true, user_flag);
    if (!pdpt)
        return false;

    auto* pd = walk_level(pdpt, PDPT_INDEX(virt), true, user_flag);
    if (!pd)
        return false;

    auto* pt = walk_level(pd, PD_INDEX(virt), true, user_flag);
    if (!pt)
        return false;

    uint64_t pt_idx = PT_INDEX(virt);
    pt[pt_idx].raw  = (phys & ADDR_MASK) | (flags & ~ADDR_MASK);

    cinux::arch::flush_tlb(virt);
    return true;
}
```

这段代码是四级页表遍历的直接翻译。从 PML4 出发，依次走 PDPT、PD、PT，每走一步调用一次 `walk_level`，传入 `should_alloc=true` 表示"缺失就分配"。任何一步分配失败（PMM 内存不足），立即返回 false。走完四级之后到达 PT，在对应的 PT slot 里写入 `(phys & ADDR_MASK) | (flags & ~ADDR_MASK)`——物理地址占 bit 51:12，标志位占其余位置，两者通过掩码分离后 OR 在一起。最后调用 `flush_tlb(virt)` 使这一页的 TLB 条目失效，确保后续访问使用新的映射。

关于 `pml4 ? *pml4 : kernel_pml4_` 这个三元表达式：如果调用者传入了 `pml4` 指针（非空），就使用指针指向的值作为页表根——这为未来进程切换后的地址空间操作留了接口。如果传空，使用保存的内核默认 PML4。

关于 `user_flag = flags & FLAG_USER`：这行代码从用户传入的 flags 中提取 User 标志，然后传递给每一级 `walk_level`。这样做是因为 x86-64 的权限检查是"最弱权限胜出"——如果中间任何一级没有 User 标志，即使用户态代码最终访问的 PTE 有 User 标志，硬件也会拒绝访问。所以分配中间页表时必须把 User 标志传播下去。

```cpp
bool VMM::map(uint64_t virt, uint64_t phys, uint64_t flags, uint64_t* pml4) {
    auto g = lock_.guard();
    (void)g;
    return map_nolock(virt, phys, flags, pml4);
}
```

`map` 是 `map_nolock` 的加锁版本。RAII guard 确保函数返回时自动释放锁。这种"公共接口加锁、内部实现无锁"的分层设计让缺页处理函数可以直接调用 `map_nolock` 而不会死锁。

### VMM::unmap —— 反向操作，精简但不可少

```cpp
void VMM::unmap(uint64_t virt, uint64_t* pml4) {
    auto g = lock_.guard();
    (void)g;

    uint64_t pml4_phys  = pml4 ? *pml4 : kernel_pml4_;
    auto*    pml4_table = phys_to_virt(pml4_phys);
    auto*    pdpt       = walk_level(pml4_table, PML4_INDEX(virt), false);
    if (!pdpt)
        return;

    auto* pd = walk_level(pdpt, PDPT_INDEX(virt), false);
    if (!pd)
        return;

    auto* pt = walk_level(pd, PD_INDEX(virt), false);
    if (!pt)
        return;

    uint64_t pt_idx = PT_INDEX(virt);
    pt[pt_idx].raw  = 0;

    cinux::arch::flush_tlb(virt);
}
```

`unmap` 的结构和 `map` 几乎完全对称，但有两个关键区别：所有 `walk_level` 调用都传 `should_alloc=false`，所以中间任何一级找不到就直接 return——对不存在的映射执行 unmap 是安全的空操作。找到 PT 后把对应条目清零（`raw = 0`），然后刷新 TLB。

你会发现 `unmap` 并不释放被映射的物理页。这是有意为之的设计——`unmap` 只负责清除虚拟到物理的映射关系，物理页的释放由调用者负责。这种"映射管理与物理内存管理分离"的设计让调用者有机会在 unmap 之后仍然使用那个物理页（比如把它映射到另一个虚拟地址），或者在确认不再需要时再调用 `PMM::free_page`。Linux 的 `pte_clear` 和 xv6 的 `uvmunmap` 也是同样的设计——页表操作和物理页生命周期管理是两个独立的关注点。

另一个值得注意的设计选择是：`unmap` 不回收中间页表。即使一个 PDPT 或 PD 下面只剩一个条目被使用，`unmap` 也不会在清空最后一个子条目后回收父页表。这是一个简化的设计——回收中间页表需要引用计数（跟踪每个中间页表有多少个子条目在使用），增加了复杂性。Linux 的做法是在释放整个地址空间时递归释放所有页表（`free_pgtables`），而不是在每次 unmap 时尝试回收。Cinux 可以在未来的进程退出逻辑中实现类似的批量回收。

### VMM::translate —— 查询，最纯粹的四步走

```cpp
uint64_t VMM::translate(uint64_t virt, uint64_t* pml4) {
    uint64_t pml4_phys  = pml4 ? *pml4 : kernel_pml4_;
    auto*    pml4_table = phys_to_virt(pml4_phys);
    auto*    pdpt       = walk_level(pml4_table, PML4_INDEX(virt), false);
    if (!pdpt)
        return 0;

    auto* pd = walk_level(pdpt, PDPT_INDEX(virt), false);
    if (!pd)
        return 0;

    auto* pt = walk_level(pd, PD_INDEX(virt), false);
    if (!pt)
        return 0;

    PageEntry& entry = pt[PT_INDEX(virt)];
    if (!entry.is_present())
        return 0;

    uint64_t offset = virt & (PAGE_SIZE - 1);
    return entry.phys_addr() | offset;
}
```

`translate` 是四个公共方法中最纯粹的——它只读不写，不做任何分配，不修改任何状态。四级遍历的逻辑和 `unmap` 一样（`should_alloc=false`），但最后不是清零而是读取：从最终的 PTE 中提取物理地址，然后拼上虚拟地址的页内偏移（bit 11:0）。页内偏移的保留是因为 `translate` 的语义是"这个虚拟地址对应哪个物理地址"，而不仅仅是"这个虚拟页对应哪个物理页帧"——如果虚拟地址是 `0x20000123`，物理页帧是 `0x100000`，那 `translate` 应该返回 `0x100123`，而不是 `0x100000`。

返回值 0 表示"未映射"。这隐含了一个假设：物理地址 0 永远不会被映射到——因为物理地址 0 是实模式中断向量表区域，在 PMM 初始化时就被排除了。如果未来需要映射物理地址 0（某些嵌入式场景），就需要用其他方式表示"未映射"（比如 optional 或 error code）。

### kernel_main 中的 VMM 初始化——启动序列的精确位置

```cpp
// Step 9: Initialise Physical Memory Manager
auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
cinux::mm::g_pmm.init(*boot_info);

// Step 10: Initialise Virtual Memory Manager
cinux::mm::g_vmm.init();
```

VMM 的初始化在 PMM 之后、帧缓冲区之前。这个顺序是严格要求的：VMM 的 `map` 在分配中间页表时需要调用 `PMM::alloc_page`，所以 PMM 必须先就绪。而帧缓冲区驱动的 `map_mmio` 虽然目前是独立函数（不经过 VMM），但未来的重构会把 MMIO 映射也统一到 VMM 中。VMM 初始化之后，整个内核就拥有了完整的虚拟内存管理能力——动态映射、解映射、地址翻译，全部就位。

## 设计决策

### Decision: walk_level 的 should_alloc 参数模式

**问题**：map 需要在遇到缺失的中间页表时分配新页，而 unmap 和 translate 只查找不分配。如何避免代码重复？

**本项目的做法**：提取 `walk_level(table, index, should_alloc, user_flag)` 函数，用布尔参数控制是否分配。map 传 `true`，unmap/translate 传 `false`。

**备选方案**：分成两个函数 `walk_or_alloc` 和 `walk_only`（host 端测试实际就是这么做的），避免运行时分支判断。

**为什么不选备选方案**：内核代码中 `walk_level` 的两个分支（present vs absent）已经很清晰，`should_alloc` 只在 absent 分支内部多了一层判断，对性能影响可以忽略。而且合并成一个函数确保了"查找逻辑"只有一份实现，减少了两个函数之间微妙不一致的风险。xv6 的 `walk(pagetable, va, alloc)` 用的是完全相同的模式——一个 `alloc` 布尔参数控制行为。

**如果要扩展/改进，应该怎么做**：如果性能分析显示 `walk_level` 成为热路径（比如频繁的 translate 操作），可以考虑在编译期通过模板参数（`bool ShouldAlloc`）消除运行时分支。或者在 translate 的热路径中直接内联四级遍历，不经过 `walk_level`。

### Decision: phys_to_virt 使用 higher-half 线性偏移

**问题**：PTE 中的物理地址需要转换成虚拟地址才能被内核代码访问。如何建立物理地址到虚拟地址的映射？

**本项目的做法**：`phys_to_virt(phys) = phys + KERNEL_VMA`，利用 bootloader 建立的 higher-half 直接映射。简单、高效、无查找表。

**备选方案**：Linux 的 `phys_to_virt` 使用 SPARSEMEM 或 FLATMEM 的 `mem_map` 数组做 `pfn_to_page` 转换，然后通过 `page_address()` 获取虚拟地址。这种间接映射允许物理内存有"洞"（不连续的物理地址范围），适用于大内存机器。

**为什么不选备选方案**：Cinux 运行在 QEMU 中，物理内存是连续的，没有内存洞的问题。线性偏移（`phys + KERNEL_VMA`）是最简单的实现——不需要查找表，不需要处理特殊情况，一条加法指令搞定。而且 higher-half 直接映射在内核启动的最早阶段就已经建立好了（bootloader 用 2MB 大页映射），所以 `phys_to_virt` 在任何时刻都能正常工作。

**如果要扩展/改进，应该怎么做**：如果未来需要支持超过 1TB 的物理内存（5 级分页场景），线性偏移可能不够——KERNEL_VMA 窗口是有限的。可以参考 Linux 的做法，为内核建立独立的"物理内存直接映射区"（linux 的 `PAGE_OFFSET` 区域），或者使用 `vmalloc` 风格的非线性映射来访问稀疏物理内存。

### Decision: unmap 不释放物理页，也不回收中间页表

**问题**：`unmap` 清除 PTE 后，被映射的物理页怎么办？中间页表如果空了怎么办？

**本项目的做法**：`unmap` 只清除 PTE + 刷新 TLB，不释放物理页也不回收中间页表。物理页由调用者释放（`PMM::free_page`），中间页表在进程退出时统一回收。

**备选方案**：在 `unmap` 中自动释放物理页（unmap = "释放映射 + 释放物理页"的一站式操作），并在中间页表清空时递归回收。

**为什么不选备选方案**：自动释放物理页的问题在于，调用者可能需要"解映射但保留物理页"的场景——比如把一个物理页从虚拟地址 A 移到虚拟地址 B，先 unmap(A) 再 map(B, 同一物理页)。如果 unmap 自动释放了物理页，map(B) 就找不到那个页了。回收中间页表的问题在于需要引用计数——每个中间页表需要记录"我下面有多少个有效的子条目"，每次 unmap 都要递减计数并在归零时触发回收，这大大增加了复杂性，而且容易出错（忘记递增/递减计数）。Linux 的选择是"进程退出时递归释放整个页表树"，Cinux 也计划采用同样的策略。

**如果要扩展/改进，应该怎么做**：在实现进程管理时，添加 `free_page_tables(pml4_phys)` 函数，递归遍历四级页表树，释放所有中间页表占用的物理页。这是 Linux `free_pgtables()` 的简化版本。对于单页的 unmap，可以添加一个 `unmap_and_free(virt)` 便利方法，内部调用 `unmap` + `free_page`，提供给那些确实想"解映射并释放"的调用者。

## 扩展方向

1. **页表引用计数与中间页表回收** (⭐⭐⭐)：在 `walk_level` 分配新中间页表时初始化引用计数为 0，在 map/unmap 时增减计数，计数归零时释放中间页表。研究 Linux 的 `pgtable_page_ctor/dtor` 机制。

2. **批量映射（map_range）** (⭐⭐)：实现 `VMM::map_range(virt_start, phys_start, count, flags)` 接口，一次映射连续的多页。思考如何优化：同一 PT 内的连续映射可以跳过前三级的重复遍历。

3. **页表自映射（recursive mapping）** (⭐⭐⭐)：研究 x86-64 的自映射技巧——把 PML4 的某个条目指向 PML4 自身，这样可以通过一个固定的虚拟地址范围访问整个页表结构。对比 Cinux 当前 `phys_to_virt` 方式的优缺点。

4. **Per-process 地址空间** (⭐⭐⭐)：基于 `pml4` 参数和 `VMM::map_nolock`，实现进程地址空间的创建（`create_address_space`：分配新 PML4，复制内核映射）和销毁（递归释放页表树）。这是多进程支持的基础。

5. **地址翻译性能优化** (⭐)：在 `translate` 的热路径中，研究是否可以利用 x86-64 的 `CLFLUSH`/`CLDEMOTE` 指令预取页表缓存行，或者使用软件 TLB（缓存最近的 translate 结果）减少四级遍历次数。

## 参考资料

- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：4 级页表遍历的完整描述，PML4->PDPT->PD->PT 的索引方式和物理地址提取。Cinux 的 `walk_level` 链式调用直接实现了这个硬件流程。
- Intel SDM Vol.3A Section 4.7, pp.4-37 to 4-38：Page-Fault Exception 的错误码格式。P 位（bit 0）区分"页不存在"和"权限违反"，这是 demand paging 的判断依据。
- Intel SDM Vol.3A Section 2.5, pp.2-14 to 2-16：CR3 寄存器格式和 PML4 物理基地址的位置。
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — Higher-half 映射的概念和 `phys_to_virt` 的原理。
- OSDev Wiki: [Paging (64-bit)](https://wiki.osdev.org/Paging) — 4 级分页的虚拟地址分解和索引提取。
- xv6 RISC-V `vm.c`：xv6 的 `walk(pagetable, va, alloc)` 函数与 Cinux 的 `walk_level` 结构几乎相同——循环遍历 3 级页表，`alloc` 参数控制是否分配缺失的中间页表。可以对比 RISC-V Sv39 和 x86-64 四级分页的实现差异。
- Linux `arch/x86/mm/pgtable.c`：Linux 的 PTE/PMD/PUD/PGD 分配和管理。注意 Linux 使用 5 级分页（PGD->P4D->PUD->PMD->PTE），比 Cinux 多一级，但核心的遍历和分配逻辑是相通的。
