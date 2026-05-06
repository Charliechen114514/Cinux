# 018-1 通读：AddressSpace 类声明——进程地址空间的"壳子"

## 概览

本文是 tag `018_mm_address_space` 三篇通读教程的第一篇，精讲 `address_space.hpp` 的完整代码。在之前的 tag 中，Cinux 的虚拟内存子系统已经有了 PMM（物理页分配器）、VMM（四级页表遍历器）和 Heap（内核堆），但它们全部运行在同一套页表上——整个内核只有一个全局的 PML4，所有地址翻译共享同一个页表根。这种设计在单体内核阶段没有问题，可一旦我们想要"进程"这个概念，第一个要解决的问题就是：每个进程必须拥有独立的虚拟地址空间。`AddressSpace` 类正是为此而生——它封装了一个独立的 PML4 根，提供从构造到析构的完整生命周期管理，同时通过 PML4 条目的"上半复制、下半独立"策略实现内核映射共享与用户空间隔离。

我们从整个头文件的文件注释开始，然后逐段讲解类声明中的公开接口、私有成员、RAII 设计考量，最后分析 PML4 分割策略是如何体现在类设计中的。

## 架构图

```
    进程 A 的 AddressSpace          进程 B 的 AddressSpace
    ┌──────────────────┐            ┌──────────────────┐
    │    PML4 (独立)    │            │    PML4 (独立)    │
    │  ┌──────────┐    │            │  ┌──────────┐    │
    │  │ 0..255   │    │            │  │ 0..255   │    │
    │  │ 用户空间  │    │            │  │ 用户空间  │    │
    │  │ (私有)   │    │            │  │ (私有)   │    │
    │  ├──────────┤    │            │  ├──────────┤    │
    │  │ 256..511 │    │            │  │ 256..511 │    │
    │  │ 内核空间  │    │            │  │ 内核空间  │    │
    │  │ (共享)   │    │            │  │ (共享)   │    │
    │  └──────────┘    │            │  └──────────┘    │
    └──────────────────┘            └──────────────────┘
           │                               │
           │  两者 PML4[256..511] 完全一致   │
           └───────────┬───────────────────┘
                       ▼
              kernel_pml4_ (静态成员)
              init_kernel() 时从 CR3 读取保存

    AddressSpace 类结构:
    ┌──────────────────────────────────────────┐
    │  public:                                  │
    │    AddressSpace()          构造: 分配+清零+复制  │
    │    ~AddressSpace()         析构: 递归释放用户子树 │
    │    move 构造 / move 赋值   转移 PML4 所有权      │
    │    copy = delete           禁止共享物理页         │
    │    init_kernel()  static   保存内核 PML4 到全局    │
    │    map / unmap / translate  委托给 VMM             │
    │    activate()              写 CR3 切换地址空间      │
    ├──────────────────────────────────────────┤
    │  private:                                 │
    │    pml4_phys_              本空间 PML4 物理地址    │
    │    kernel_pml4_  static    全局内核 PML4 物理地址   │
    │    free_subtree()          递归释放页表子树         │
    └──────────────────────────────────────────┘
```

## 代码精讲

### 文件注释——设计意图的全景说明

```cpp
/**
 * @file kernel/mm/address_space.hpp
 * @brief Per-process virtual address space management
 *
 * Encapsulates an independent PML4 root and provides isolated user-space
 * page table management.  Each AddressSpace instance owns a freshly
 * allocated PML4 whose kernel half (entries 256-511) mirrors the global
 * kernel mapping, while the user half (entries 0-255) is private.
 *
 * Copy construction and copy assignment are deleted to prevent accidental
 * sharing of physical page table pages.
 *
 * Depends on: PMM (for page allocation), VMM (for map/unmap/translate
 * helpers), and x86_64 paging primitives.
 *
 * Namespace: cinux::mm
 */
```

文件注释已经把核心设计说得很清楚了。每个 `AddressSpace` 实例拥有一个"新鲜分配"的 PML4，其内核半部分（条目 256-511）镜像自全局内核映射，用户半部分（条目 0-255）则是私有的。这种设计用最小的代价实现了每个进程的地址空间隔离——内核映射不需要重新建立，只需从模板 PML4 复制过来；用户空间从零开始，互不干扰。注释还特别提到了拷贝构造和拷贝赋值被删除，因为两个 `AddressSpace` 不能共享同一组物理页表页——这会导致一个空间的 unmap 操作破坏另一个空间的映射，是经典的 use-after-free 场景。

### 构造与析构——RAII 生命周期管理

```cpp
class AddressSpace {
public:
    // ============================================================
    // Construction / Destruction
    // ============================================================

    /**
     * @brief Construct an isolated address space
     *
     * Allocates a new PML4 page from the PMM, zeroes it, then copies
     * kernel-space entries (PML4[256..511]) from the saved kernel PML4
     * so that the kernel mapping is visible in every address space.
     *
     * @note Requires that init_kernel() has been called beforehand.
     */
    AddressSpace();

    /**
     * @brief Destroy the address space and free all user-space page tables
     *
     * Walks PML4[0..255] and, for each present entry, recursively frees
     * the entire subtree (PDPT -> PD -> PT) back to the PMM.  Finally
     * frees the PML4 page itself.
     */
    ~AddressSpace();
```

构造函数的职责很明确：从 PMM 分配一页作为新的 PML4，清零所有 512 个条目，然后把内核 PML4 的上半部分（条目 256-511）逐条复制过来。注释里的 `@note` 特别提醒——调用构造函数之前必须先调用 `init_kernel()`，否则 `kernel_pml4_` 还是 0，复制内核条目就变成了从地址 0 处读取，必然出错。这是典型的"两阶段初始化"模式：`init_kernel()` 在启动时调用一次，之后所有 `AddressSpace` 实例的构造都依赖于它保存的全局状态。

析构函数的描述揭示了一个精妙的设计决策：它只释放用户空间（PML4[0..255]）的页表子树，而不碰内核空间的条目。原因很简单——内核空间的页表页不属于任何单个 `AddressSpace`，它们是启动时建立的全局资源，只有用户空间页表才是这个实例"私有"的。递归释放走的是 PDPT -> PD -> PT 三级。到了 PT 层级，递归不再深入（`level > LEVEL_PT` 条件不满足），但仍然会对 PT 条目调用 `free_page`——这意味着 PT 条目指向的数据页也会被释放。在当前没有共享内存机制的设计中，这是合理的：每个 AddressSpace 独占其用户空间的物理页，析构时全量释放。

### 禁止拷贝、允许移动——独占所有权语义

```cpp
    // Disable copy -- each AddressSpace owns exclusive physical pages
    AddressSpace(const AddressSpace&)            = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

    // Allow move -- transfers ownership of pml4_phys_
    AddressSpace(AddressSpace&& other) noexcept;
    AddressSpace& operator=(AddressSpace&& other) noexcept;
```

这两行 `= delete` 是整个类的安全基石。试想一下：如果允许拷贝构造，两个 `AddressSpace` 对象的 `pml4_phys_` 指向同一个物理 PML4 页——当第一个对象析构时，它把 PML4 归还给 PMM，第二个对象手里就剩下一个悬空指针，后续任何 map/unmap/translate 操作都会往已经不属于自己的物理内存上写东西。禁止拷贝从根源上消除了这个问题。

移动语义则是为了支持把 `AddressSpace` 存入容器（比如未来的进程表）而保留的。移动构造和移动赋值做的事情很简单：接管 `other` 的 `pml4_phys_`，然后把 `other.pml4_phys_` 置为 0。被移除的对象在析构时检查到 `pml4_phys_ == 0` 就直接返回，不做任何释放。`noexcept` 标注也很重要——标准库容器（如 `std::vector`）在 realloc 时会优先使用移动操作，但前提是移动操作是 `noexcept` 的，否则它们宁可走拷贝（安全性更高的选择）。既然拷贝已经被删了，移动必须标 `noexcept`，否则容器根本无法编译。

### 静态初始化——保存内核页表根

```cpp
    // ============================================================
    // Static initialisation
    // ============================================================

    /**
     * @brief Read the current CR3 and save it as the kernel PML4
     *
     * Must be called once during boot, after the initial page tables
     * are set up but before any AddressSpace instance is created.
     */
    static void init_kernel();
```

`init_kernel()` 是一个静态方法，整个类只有这一个"启动钩子"。它做的事情极其简单——读 CR3，把值存到静态成员 `kernel_pml4_` 里。但它的调用时机非常关键：必须在 bootloader 设置好内核页表之后、在创建第一个 `AddressSpace` 之前调用。在我们的启动流程中，它被安排在 `kernel_main()` 的 Step 9，紧接着 VMM 初始化之后、Heap 初始化之前。时间窗口是确定的——VMM 初始化完成后内核页表已经就绪，此时读到的 CR3 就是"标准内核页表"的物理地址。

### 页表操作接口——委托给 VMM

```cpp
    // ============================================================
    // Page table operations
    // ============================================================

    /**
     * @brief Map a single 4 KB virtual page within this address space
     *
     * Delegates to VMM using this space's PML4 as the root.
     *
     * @param virt   Virtual address to map (page-aligned recommended)
     * @param phys   Physical address to map to (must be page-aligned)
     * @param flags  Combination of FLAG_PRESENT, FLAG_WRITABLE, etc.
     * @return true on success, false on allocation failure
     */
    bool map(uint64_t virt, uint64_t phys, uint64_t flags);

    /**
     * @brief Unmap a single 4 KB virtual page within this address space
     *
     * @param virt  Virtual address to unmap
     */
    void unmap(uint64_t virt);

    /**
     * @brief Translate a virtual address to physical within this space
     *
     * @param virt  Virtual address to look up
     * @return Physical address, or 0 if not mapped
     */
    uint64_t translate(uint64_t virt);

    /**
     * @brief Activate this address space (load CR3)
     *
     * Writes this space's PML4 physical address into CR3, making it
     * the active page table root.  Flushes the TLB implicitly.
     */
    void activate();
```

`map`、`unmap`、`translate` 三个方法是对 VMM 的直接委托，把 `&pml4_phys_` 作为可选参数传给 `g_vmm` 的对应方法。这种设计让 `AddressSpace` 成了一个非常轻量的"所有权包装器"——页表遍历和修改的复杂逻辑全部复用 VMM 已有的实现，`AddressSpace` 只负责"用哪个 PML4 做根"这个上下文管理。注释里标注了 `@return` 值的含义，`map` 在分配中间页表页失败时返回 `false`（比如 PMM 耗尽），`translate` 在地址未映射时返回 0。

`activate()` 是唯一一个不委托给 VMM 的操作——它直接调用 `write_cr3(pml4_phys_)`，用内联汇编把 PML4 物理地址写入 CR3 寄存器。根据 Intel SDM Vol.3A Section 4.10.2 的规定，写入 CR3 会隐式刷新所有非全局的 TLB 条目，所以切换地址空间后 CPU 的地址翻译缓存自动失效，后续所有内存访问都使用新的页表。这个方法在进程切换时会被调用。

### 访问器——暴露内部状态

```cpp
    // ============================================================
    // Accessors
    // ============================================================

    /** Get the physical address of this space's PML4 root. */
    uint64_t pml4_phys() const;

    /** Get the saved kernel PML4 physical address. */
    static uint64_t kernel_pml4();
```

两个访问器都很直白。`pml4_phys()` 返回实例的 PML4 物理地址——测试代码用它来验证构造是否成功分配了非零的 PML4。`kernel_pml4()` 是静态方法，返回全局保存的内核 PML4 地址——测试代码用它来验证新构造的 `AddressSpace` 确实拥有与内核 PML4 不同的根。

### 私有成员——数据与辅助

```cpp
private:
    // ============================================================
    // Internal helpers
    // ============================================================

    /**
     * @brief Recursively free all page table pages under a given entry
     *
     * @param table_virt  Virtual address of the current-level table
     * @param level       Current level: 3 = PDPT, 2 = PD, 1 = PT
     */
    void free_subtree(uint64_t table_phys, int level);

    // ============================================================
    // Data members
    // ============================================================

    /** Physical address of this address space's PML4 root. */
    uint64_t pml4_phys_{};

    /** Saved kernel PML4 physical address (populated by init_kernel). */
    static uint64_t kernel_pml4_;
};
```

`free_subtree` 是析构函数的核心辅助，负责递归释放一棵页表子树。参数 `level` 从 3（PDPT）开始递减到 1（PT），在 PT 层停止递归但依然释放当前层级的页表页本身。注意注释说的是 `table_virt`，但实际参数名叫 `table_phys`——这是因为函数接收的是物理地址，内部再通过 higher-half 直接映射转换成虚拟地址来访问。这个命名在下一篇文章的 cpp 精讲中会更加清晰。

`pml4_phys_{}` 用了 C++11 的成员初始化器语法，初始化为 0。这个值在构造时被赋为 PMM 分配的物理地址，在移动后被置为 0（表示"不再拥有资源"），在析构时也被置为 0（防御性编程）。可以说 `pml4_phys_` 是否为 0 是整个对象状态的"哨兵"——所有生命周期操作（析构、移动）都先检查它。

`kernel_pml4_` 是静态成员，所有 `AddressSpace` 实例共享同一个值——启动时保存的内核 PML4 物理地址。它在 `init_kernel()` 中被赋值一次，之后只读。

## 设计决策

### Decision: 禁止拷贝、只允许移动

**问题**：`AddressSpace` 持有一个物理页表根（PML4）的所有权。如果两个对象共享同一个物理 PML4，一个析构后另一个就悬空了。

**本项目的做法**：`= delete` 拷贝构造和拷贝赋值，只提供移动语义。移动后源对象的 `pml4_phys_` 置 0，析构变成 no-op。

**备选方案**：引用计数（共享所有权），类似 `std::shared_ptr`。多个 `AddressSpace` 共享同一个 PML4，最后一个析构时释放。

**为什么不选备选方案**：地址空间在语义上就是"独占"的——一个进程的页表不应该被另一个进程的地址空间对象共享。引用计数引入了不必要的原子操作开销，而且语义上也不对：两个进程的地址空间如果共享 PML4，那它们实际上就是同一个地址空间，何必用两个对象表示？禁止拷贝在编译期就阻止了误用，比运行时的引用计数更安全、更高效。

### Decision: 静态 init_kernel() 而非构造时自动检测

**问题**：新创建的 `AddressSpace` 需要知道"模板 PML4"在哪里才能复制内核条目。这个信息应该在什么时候获取？

**本项目的做法**：用一个静态方法 `init_kernel()` 在启动时显式保存 CR3 到 `kernel_pml4_`。构造函数从这个静态变量读取模板。

**备选方案**：构造函数内部直接读 CR3 作为模板。

**为什么不选备选方案**：构造函数读到的 CR3 是"当前正在使用的"页表根，不一定是"标准内核页表"。如果进程 A 创建 `AddressSpace` 时 CR3 指向进程 B 的 PML4，复制出来的内核条目可能不是期望的。显式保存确保了模板来源是确定的、一致的——永远是我们启动时建立的那套页表。

### Decision: 委托 VMM 而非自行实现页表遍历

**问题**：`map`/`unmap`/`translate` 需要四级页表遍历。这个逻辑已经在 VMM 中实现了。`AddressSpace` 应该复用还是重新实现？

**本项目的做法**：直接委托 `g_vmm` 的对应方法，传入 `&pml4_phys_` 作为自定义页表根。

**备选方案**：`AddressSpace` 自己维护一套独立的页表遍历逻辑。

**为什么不选备选方案**：代码复用是显而易见的理由，但更深层的原因是职责单一。VMM 的角色是"页表操作的算法实现"，`AddressSpace` 的角色是"页表根的生命周期管理"。两者混在一起会让类变得臃肿，也让测试变得困难——`AddressSpace` 的测试不应该关心四级页表遍历的正确性，那是 VMM 测试的职责。

## 扩展方向

1. **fork() 支持** (⭐⭐⭐)：当前的 `AddressSpace` 没有复制语义——无法从一个地址空间 fork 出另一个。实现 Copy-On-Write fork 需要：复制 PML4 用户空间条目、对所有映射的数据页标记 `FLAG_COW` 并清除 `FLAG_WRITABLE`、在写时缺页处理函数中分配新物理页并复制内容。这是 tag 019 的核心工作。

2. **VMA (Virtual Memory Area) 管理** (⭐⭐)：Linux 用 `vm_area_struct` 描述一段连续的虚拟地址区域的属性（可读/可写/可执行/私有/共享）。在 `AddressSpace` 中引入 VMA 链表，可以为 `mmap`/`munmap`/`mprotect` 提供基础。思考：红黑树 vs 链表 vs 区间树，哪种数据结构最适合 VMA 查找？

3. **PCID 优化** (⭐⭐)：每次 `activate()` 都会全量刷 TLB，这对性能是灾难性的。阅读 Intel SDM Vol.3A Section 4.10.1，研究 CR4.PCIDE=1 后如何在 CR3 低 12 位编码 PCID，实现跨地址空间的 TLB 条目共存。

4. **延迟分配 PML4** (⭐)：当前构造函数立即分配 PML4。如果创建了大量 `AddressSpace` 但只用了一小部分（比如预分配进程槽位），就浪费了物理内存。可以改为"首次 map 或 activate 时再分配"的懒初始化策略。

5. **内核地址空间偷渡检测** (⭐⭐)：当前架构信任内核代码不会修改 PML4[256..511]。可以添加一个 `validate()` 方法，比较当前 PML4 的内核半部分与模板是否一致，用于检测内核页表被意外篡改的情况。

## 参考资料

- Intel SDM Vol.3A Section 4.5.2, pp.4-20 to 4-21：CR3 与 4 级分页的关系，PML4 物理基地址在 CR3 中的位置。
- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：4 级分页的完整地址翻译流程，每个 PML4 条目控制 512GB 区域。
- Intel SDM Vol.3A Section 4.10.2：CR3 重载对 TLB 的影响，`activate()` 的隐式 TLB 刷新机制来源。
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 内核映射到每个地址空间的上半部分，用户空间在下半部分独立。
- Linux `mm_struct`：`pgd_alloc()` 复制内核条目的设计与 Cinux 的构造函数逻辑完全对应，参考 kernel-internals.org 的 [process address space 文档](https://www.kernel.org/doc/html/next/mm/process_addrs.html)。
