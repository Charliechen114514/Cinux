---
title: 018-mm-address-space-2 · 地址空间
---

# 018-2 通读：AddressSpace 实现——构造、析构、移动与 CR3 切换

## 概览

本文是 tag `018_mm_address_space` 三篇通读教程的第二篇，精讲 `address_space.cpp` 的全部 198 行实现代码。上一篇我们已经看过了类声明，了解了 `AddressSpace` 的公开接口和设计意图——每个实例封装一个独立的 PML4 根，用户空间私有，内核空间共享。现在我们深入实现细节：`init_kernel()` 如何在启动时"锚定"内核页表，构造函数如何"从零搭建"一个新地址空间，析构函数如何"递归拆毁"用户空间的页表子树，移动语义如何保证资源所有权的安全转移，以及 `activate()` 如何用一行内联汇编切换 CPU 的地址翻译上下文。

## 架构图

```
    address_space.cpp 实现结构:

    ┌─────────────────────────────────────────────┐
    │  静态成员初始化                               │
    │  kernel_pml4_ = 0                            │
    ├─────────────────────────────────────────────┤
    │  常量定义                                     │
    │  KERNEL_VMA, USER_PML4_START/END, LEVEL_*    │
    ├─────────────────────────────────────────────┤
    │  匿名命名空间: phys_to_virt()                 │
    │  物理地址 → higher-half 虚拟地址              │
    ├─────────────────────────────────────────────┤
    │  init_kernel()  → read_cr3() 保存到静态成员   │
    ├─────────────────────────────────────────────┤
    │  构造函数: alloc → zero → copy [256..511]     │
    ├─────────────────────────────────────────────┤
    │  析构函数: free_subtree [0..255] → free PML4  │
    ├─────────────────────────────────────────────┤
    │  移动构造: 接管 pml4_phys_, 源置 0             │
    │  移动赋值: 先释放自己, 再接管                  │
    ├─────────────────────────────────────────────┤
    │  map/unmap/translate → 委托 g_vmm             │
    │  activate() → write_cr3(pml4_phys_)           │
    ├─────────────────────────────────────────────┤
    │  free_subtree: 递归释放 PDPT→PD→PT            │
    └─────────────────────────────────────────────┘
```

## 代码精讲

### 常量定义与 phys_to_virt

```cpp
namespace cinux::mm {

// ============================================================
// Static member initialisation
// ============================================================

uint64_t AddressSpace::kernel_pml4_ = 0;

// ============================================================
// Constants
// ============================================================

using namespace cinux::arch;

constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;

// PML4 entry indices that belong to user space (lower half)
constexpr uint32_t USER_PML4_START = 0;
constexpr uint32_t USER_PML4_END   = 256;

// Recursion levels for subtree freeing:
//   PML4 -> PDPT (level 3) -> PD (level 2) -> PT (level 1)
constexpr int LEVEL_PDPT = 3;
constexpr int LEVEL_PD   = 2;
constexpr int LEVEL_PT   = 1;
```

`kernel_pml4_` 的类外初始化是 C++ 静态成员的标准写法。它在 `init_kernel()` 调用之前一直是 0——如果忘了调用 `init_kernel()` 就创建 AddressSpace，构造函数会从地址 0 处复制内核条目。

`KERNEL_VMA = 0xFFFFFFFF80000000ULL` 是内核虚拟基地址。物理地址到虚拟地址的转换就是 `phys + KERNEL_VMA`，整个内核通过这个偏移量访问物理内存。

`USER_PML4_START` 和 `USER_PML4_END` 定义用户空间的 PML4 条目范围：0 到 255，覆盖 x86-64 虚拟地址的下半部分（256 个 512GB 区域 = 128TB）。`LEVEL_*` 三个常量用于 `free_subtree` 的递归深度控制，分别对应 PDPT、PD、PT。

### phys_to_virt——物理地址到虚拟地址的桥梁

```cpp
namespace {

/** Convert a physical address to a virtual address via the higher-half mapping. */
PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}

}  // anonymous namespace
```

匿名命名空间中的辅助函数，具有内部链接属性。`phys + KERNEL_VMA` 把物理地址加上 higher-half 偏移得到虚拟地址，再 `reinterpret_cast` 成 `PageEntry*`。传入的物理地址都来自 PMM 分配或 CR3 读取，都在合法范围内。

### init_kernel()——锚定内核页表

```cpp
void AddressSpace::init_kernel() {
    kernel_pml4_ = cinux::arch::read_cr3();
    cinux::lib::kprintf("[AS] Kernel PML4 saved at phys %p\n",
                        reinterpret_cast<void*>(kernel_pml4_));
}
```

就两行——读 CR3，打印日志。`read_cr3()` 是 `paging.hpp` 中的内联汇编函数，执行 `mov %cr3, %0`。CR3 的 bit 51:12 存放着当前 PML4 表的物理页帧号。因为 Cinux 没有启用 PCID，`read_cr3()` 返回的值就是 PML4 的物理地址。

这个函数在 `kernel_main()` 中作为 Step 9 调用，紧跟在 VMM 初始化之后。此时内核页表已经完全建立好，CR3 指向的是最"标准"的内核 PML4。保存这个值后，后续所有 `AddressSpace` 构造函数都从这个模板复制内核条目。

### 构造函数——从零搭建一个新地址空间

```cpp
AddressSpace::AddressSpace() {
    // Step 1: Allocate a fresh PML4 page
    pml4_phys_ = g_pmm.alloc_page();
    if (pml4_phys_ == 0) {
        cinux::lib::kprintf("[AS] FATAL: failed to allocate PML4 page\n");
        return;
    }

    // Step 2: Zero the entire PML4
    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        pml4[i].raw = 0;
    }

    auto* kern_pml4 = phys_to_virt(kernel_pml4_);
    for (uint32_t i = USER_PML4_END; i < PT_ENTRIES; i++) {
        pml4[i].raw = kern_pml4[i].raw;
    }
}
```

构造函数分三步走。第一步从 PMM 分配一页 4KB 物理内存作为新的 PML4。如果分配失败（PMM 耗尽），`alloc_page()` 返回 0，构造函数打印错误日志并提前返回——此时 `pml4_phys_` 为 0，后续所有操作都会失效，析构时也跳过释放。这是一种"退化状态"，对象存在但不可用。

第二步把整个 PML4 清零，512 个条目全部设为 `raw = 0`。PTE 的 bit 0（Present）为 0，MMU 不会做地址翻译，访问对应虚拟地址会触发 page fault。这保证新地址空间的用户空间从"完全空白"开始。

第三步复制内核条目。从保存的内核 PML4 取出条目 256 到 511，逐条复制到新 PML4 的对应位置。这里用 `raw` 整体赋值——一条拷贝就完整保留了物理地址和所有标志位。复制完成后，新 PML4 的上半部分和内核 PML4 完全一致。值得注意的是，这是"浅拷贝"——只复制了 PML4 条目本身，没有递归复制 PDPT/PD/PT 页表页。所以所有地址空间共享同一套内核空间的页表结构，既节省内存又保证数据一致性。

### 析构函数——递归拆毁用户空间页表子树

```cpp
AddressSpace::~AddressSpace() {
    // Nothing to free if the PML4 was never allocated (or was moved out)
    if (pml4_phys_ == 0) {
        return;
    }

    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++) {
        if (pml4[i].is_present()) {
            free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);
        }
    }

    // Step 2: Free the PML4 page itself
    g_pmm.free_page(pml4_phys_);
    pml4_phys_ = 0;
}
```

析构函数的入口是防御性检查：`if (pml4_phys_ == 0) return;`。这覆盖构造函数分配失败和移动后源对象被"掏空"两种场景。

接下来遍历 PML4 的用户空间部分（条目 0 到 255），对每个 present 的条目调用 `free_subtree`。如果条目没有被 map 操作创建过（不 present），直接跳过。`free_subtree` 完成后，所有子页表页都已归还 PMM。最后归还 PML4 页本身，然后把 `pml4_phys_` 置 0——虽然对象马上销毁，但保持"自洽"状态是良好的编程习惯。

### free_subtree——递归释放的"拆楼"算法

```cpp
void AddressSpace::free_subtree(uint64_t table_phys, int level) {
    auto* table = phys_to_virt(table_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!table[i].is_present()) {
            continue;
        }

        // Stop recursion at PT level -- PT entries point to data pages
        // which are NOT owned by the address space infrastructure
        if (level > LEVEL_PT) {
            free_subtree(table[i].phys_addr(), level - 1);
        }

        // Free the page table page at this level
        g_pmm.free_page(table[i].phys_addr());
    }
}
```

这个函数是析构逻辑的核心。调用链是：析构函数遍历 PML4[0..255]，对每个 present 条目调用 `free_subtree(entry.phys_addr(), LEVEL_PDPT)`。函数收到页表页的物理地址和当前层级，遍历所有条目。

关键在于递归终止条件：`if (level > LEVEL_PT)` 才继续递归。当 `level == LEVEL_PT` 时不再递归——递归到此为止。但注意 `free_page` 在循环中是无条件调用的，所以在 PT 层（level=1），PT 条目指向的数据页仍然会被 `free_page` 释放。这意味着 AddressSpace 析构时会释放用户空间中的所有物理页，包括数据页。在当前没有共享内存机制的设计中，这是合理的行为。递归顺序是"先深入，后释放"——保证释放父级页表页之前子级已处理完毕。

### 移动构造与移动赋值——所有权安全转移

```cpp
AddressSpace::AddressSpace(AddressSpace&& other) noexcept
    : pml4_phys_(other.pml4_phys_) {
    other.pml4_phys_ = 0;
}

AddressSpace& AddressSpace::operator=(AddressSpace&& other) noexcept {
    if (this != &other) {
        // Free our current resources
        if (pml4_phys_ != 0) {
            auto* pml4 = phys_to_virt(pml4_phys_);
            for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++) {
                if (pml4[i].is_present()) {
                    free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);
                }
            }
            g_pmm.free_page(pml4_phys_);
        }

        // Take ownership of the other's PML4
        pml4_phys_       = other.pml4_phys_;
        other.pml4_phys_ = 0;
    }
    return *this;
}
```

移动构造很简洁——用 `other.pml4_phys_` 初始化自己的，然后把 `other.pml4_phys_` 置 0。纯粹的"指针偷窃"，被移除的对象进入退化状态。

移动赋值多了一步：先释放自己当前的资源，再接管对方的。释放逻辑和析构函数完全一样。`if (this != &other)` 守卫防止自赋值导致 use-after-free——实际中常通过 `container[i] = std::move(container[j])`（i == j）间接触发。

### 页表操作与 CR3 切换——委托与激活

```cpp
bool AddressSpace::map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return g_vmm.map(virt, phys, flags, &pml4_phys_);
}

void AddressSpace::unmap(uint64_t virt) {
    g_vmm.unmap(virt, &pml4_phys_);
}

uint64_t AddressSpace::translate(uint64_t virt) {
    return g_vmm.translate(virt, &pml4_phys_);
}

void AddressSpace::activate() {
    cinux::arch::write_cr3(pml4_phys_);
}
```

`map`/`unmap`/`translate` 是纯粹的委托——调用 `g_vmm` 的对应接口，传入 `&pml4_phys_` 作为自定义页表根。VMM 接受可选的 `uint64_t* pml4_out` 参数：传入非空指针就用它指向的 PML4 做遍历起点，传入 `nullptr` 就用默认内核 PML4。一份代码两种用途。

`activate()` 是整个类中唯一接触硬件寄存器的方法。`write_cr3(pml4_phys_)` 把 PML4 物理地址写入 CR3。根据 Intel SDM Vol.3A Section 4.10.2，写入 CR3 会自动使所有非 Global 的 TLB 条目失效。这也是为什么进程切换是 TLB miss 的主要来源——每次切换都丢掉了上一个进程积累的 TLB 缓存。

两个访问器 `pml4_phys()` 和 `kernel_pml4()` 在测试中频繁使用——验证构造成功、PML4 地址唯一性、activate 后 CR3 更新等。

## 设计决策

### Decision: 浅拷贝内核条目而非深拷贝整个内核页表树

**问题**：新 `AddressSpace` 需要内核映射可见。是复制整个内核页表树（PML4/PDPT/PD/PT 全部独立），还是只复制 PML4 条目？

**本项目的做法**：只复制 PML4[256..511] 的 raw 值。所有地址空间共享同一套内核 PDPT/PD/PT 页表页。

**备选方案**：深拷贝——为新地址空间创建一份完整的内核页表副本。

**为什么不选备选方案**：内核映射在所有地址空间中必须完全一致。独立副本意味着运行时动态添加的映射（如 map_mmio）需要同步到所有副本，复杂度不可接受。共享页表树修改一处全局生效，代价是这个不变量需要内核代码自行维护。

### Decision: free_subtree 在 PT 层释放数据页

**问题**：地址空间销毁时，用户空间的映射指向的数据页应该如何处理？

**本项目的做法**：free_subtree 递归到 PT 层后不再深入，但对 PT 条目仍然调用 free_page——这意味着数据页也会被释放。这是"全量释放"策略。

**备选方案**：在 PT 层不调用 free_page，只释放页表结构页（PDPT/PD/PT），数据页的生命周期由上层管理。

**为什么当前选择全量释放**：当前每个 AddressSpace 独占其用户空间的物理页，没有共享内存机制。析构时全量释放是最简单的策略，不需要引用计数。未来实现共享内存或 COW fork 时，需要改为"只释放页表结构"策略，配合引用计数管理数据页。

### Decision: activate() 不自动恢复内核 PML4

**问题**：`activate()` 切换了 CR3。调用者是否需要在后续手动恢复？

**本项目的做法**：`activate()` 是纯粹的一行 `write_cr3`，不保存、不恢复。调用者负责在适当时机恢复到内核 PML4。

**备选方案**：`activate()` 接受一个回调或 scope guard，在作用域结束时自动恢复。

**为什么不选备选方案**：进程切换发生在调度器里，每次都是显式的 `activate(A)` -> `activate(B)` 操作，没有"自动恢复"的需求。自动恢复增加复杂度，在错误路径中容易产生混淆。

## 扩展方向

1. **引用计数数据页** (⭐⭐⭐)：设计 `PageRef` 结构，每个数据页带引用计数。`map` 增计数，`unmap` 减计数，归零归还 PMM。这是共享内存和 COW fork 的基础。

2. **TLB 刷新优化** (⭐⭐)：当前 unmap 后没有刷新 TLB，如果地址刚被访问过，TLB 中可能还有过时条目。阅读 Intel SDM Vol.3A Section 4.10.4，在 `unmap` 中加入 `flush_tlb(virt)`。

3. **批量映射** (⭐⭐)：添加 `map_range(virt, phys, num_pages, flags)` 接口，减少重复页表遍历开销。连续映射时如何复用中间页表页？

4. **debug dump** (⭐)：添加 `dump()` 方法遍历 PML4 用户空间条目，通过 kprintf 打印所有 present 条目的地址映射。

## 参考资料

- Intel SDM Vol.3A Section 4.5.2, pp.4-20 to 4-21：CR3 寄存器格式。`init_kernel()` 和 `activate()` 的硬件基础。
- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：四级页表遍历流程。`free_subtree` 的递归结构与此对应。
- Intel SDM Vol.3A Section 4.10.2：CR3 写入导致非全局 TLB 条目失效。
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging) — 四级分页结构详解。
- OSDev Wiki: [TLB](https://wiki.osdev.org/TLB) — TLB 刷新方法与 CR3 重载的影响。
- Linux `pgd_alloc()` / `pgd_free()`：PGD 分配和释放逻辑，与 Cinux 的构造/析构模式高度相似。
