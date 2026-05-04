# 016-3 通读：缺页异常处理、按需分页与测试验证

## 概览

本文是 tag `016_mm_vmm` 三篇通读教程的最后一篇。前两篇我们已经把砖块（`PageEntry` 联合体、分页常量）和施工过程（VMM 的 `map`/`unmap`/`translate` 核心算法）都讲清楚了，现在要做的是把整个虚拟内存子系统串起来跑通——在缺页异常处理函数中接入 demand paging，在启动序列中插入 VMM 初始化，然后通过一套双轨测试策略验证一切工作正常。QEMU 环境里的集成测试验证真实页表、真实 TLB、真实 #PF 流程；Host 端的 mock 测试则用纯软件模拟跑遍各种边界情况，两条线互补，确保算法正确性和硬件交互正确性都能被覆盖到。

## 架构图

```
    CPU 触发 #PF (vector 14)
    ├── CR2 ← fault_addr (硬件自动写入)
    ├── error_code 推入栈 (P/W/R/U/S/RSVD/I/D)
    ▼
    handle_pf(InterruptFrame* frame)
    ├── err & 0x01 == 0 (P=0, 页不存在)
    │   ├── 页对齐 CR2
    │   ├── PMM::alloc_page_locked() 分配物理页
    │   ├── VMM::map_nolock() 建立映射 (FLAG_PRESENT | FLAG_WRITABLE)
    │   ├── return → IRETQ 恢复执行
    │   └── 分配/映射失败 → 落入 fatal 路径
    ├── err & 0x01 == 1 (P=1, 权限违反 / CoW / 保留位)
    │   ├── CoW fault → handle_cow_fault()
    │   └── 否则 → panic
    ▼
    启动序列: g_pmm.init() → g_vmm.init() → run_vmm_tests()

    双轨测试:
    ┌─────────────────────────┐  ┌──────────────────────────┐
    │ QEMU 集成测试            │  │ Host 单元测试              │
    │ kernel/test/test_vmm.cpp │  │ test/unit/test_vmm.cpp    │
    │ 真实页表/TLB/#PF/PMM     │  │ MockPMM + sim_memory      │
    │ 10 个测试用例             │  │ TestVMM 算法镜像, 15 个   │
    └─────────────────────────┘  └──────────────────────────┘
```

## 代码精讲

### 缺页异常处理与按需分页

现在我们来看 `handle_pf` 中新增的 demand-paging 逻辑。这个函数在 RT-1 和 RT-2 中都没有涉及——它属于 `exception_handlers.cpp`，而不是 VMM 核心代码，但它是让"按需分页"真正运转起来的关键环节。

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;
```

函数开头先从 CR2 寄存器读取触发缺页的线性地址。Intel SDM Vol.3A Section 4.7 明确规定，#PF 发生时 CPU 会把引起异常的线性地址写入 CR2——这个写入是硬件自动完成的，不需要软件干预。`err` 是 CPU 推入栈的错误码，它的 bit 0（P 位）是 demand paging 的核心判断依据：P=0 表示"页不存在"，P=1 表示"权限违反"。

接下来是一段栈溢出检测代码（guard page 检测），这部分和 VMM 无关，属于进程管理的范畴。跳过这段，我们直接看 demand-paging 的核心逻辑：

```cpp
    // Demand-paging: try to allocate a page for not-present faults
    // Use lock-free allocation paths — the PF handler runs under an
    // Interrupt gate (IF=0) so no concurrent VMM/PMM access is possible
    // on this CPU.  Taking locks here would deadlock on recursive faults.
    if ((err & 0x01) == 0) {
        uint64_t virt_page = fault_addr & ~0xFFFULL;
        uint64_t map_flags = cinux::arch::FLAG_PRESENT | cinux::arch::FLAG_WRITABLE;
        if (cinux::arch::is_user_vaddr(fault_addr)) {
            map_flags |= cinux::arch::FLAG_USER;
        }
        uint64_t phys = cinux::mm::g_pmm.alloc_page_locked();
        if (phys != 0) {
            uint64_t cur_cr3 = cinux::arch::read_cr3();
            bool     ok      = g_vmm.map_nolock(virt_page, phys, map_flags, &cur_cr3);
            if (ok) {
                kprintf("[VMM] Demand-paged %p -> phys %p\n",
                        reinterpret_cast<void*>(virt_page),
                        reinterpret_cast<void*>(phys));
                return;
            }
            cinux::mm::g_pmm.free_page_locked(phys);
        }
    }
```

这几行代码做了三件事。第一，`fault_addr & ~0xFFFULL` 把故障地址向下对齐到页边界——CR2 里的地址可能是页内的任意偏移，但我们的映射操作是以整页为单位的，所以必须先对齐。`~0xFFFULL` 这个掩码清掉了低 12 位，等价于 `fault_addr & ~(PAGE_SIZE - 1)`。

第二，根据故障地址是属于用户空间还是内核空间，决定映射时是否带 `FLAG_USER` 标志。`is_user_vaddr` 在 RT-1 中出现过——它检查虚拟地址的 bit 47 是否为 0。用户态触发的缺页必须带 User 标志，否则 Ring 3 的代码后续访问这个页时又会触发权限违反的 #PF，陷入死循环。

第三，调用 PMM 的 `alloc_page_locked()` 和 VMM 的 `map_nolock()`。注释里特别强调了为什么要用无锁版本——#PF handler 运行在中断门下（IF=0），此时不可能有并发的 VMM/PMM 访问，如果强行拿锁反而可能在递归缺页时死锁。`map_nolock` 正是 RT-2 中为这个场景设计的无锁版本。映射成功后直接 `return`，CPU 执行 IRETQ 回到触发异常的那条指令重新执行，此时页表已经建好了，访问正常通过。

如果 PMM 分配失败（内存耗尽）或映射失败，会释放刚分配的物理页（`free_page_locked`），然后落入下方的 fatal 路径。后面的 CoW 处理和 panic 报错我们在本篇不展开——它们属于更高级的内存管理功能。

### 启动集成

VMM 在内核启动序列中的位置非常精确。在 `main.cpp` 中：

```cpp
// Step 9: Initialise Physical Memory Manager
auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
cinux::mm::g_pmm.init(*boot_info);

// Step 10: Initialise Virtual Memory Manager
cinux::mm::g_vmm.init();
```

先 PMM 后 VMM 的顺序是硬性要求——VMM 的 `map` 在分配中间页表时需要调用 `PMM::alloc_page`，如果 PMM 还没初始化，分配就会失败或分配出垃圾。`g_vmm.init()` 做的事情很简单：从 CR3 读出内核 PML4 的物理地址并保存，这在 RT-2 中已经详细讲过了。

在测试环境中，`main_test.cpp` 重复了同样的顺序：

```cpp
run_pmm_tests();

// VMM tests: initialise VMM after PMM, then run tests
cinux::mm::g_vmm.init();
run_vmm_tests();
```

PMM 测试先跑完（确保物理内存管理器工作正常），然后初始化 VMM，最后跑 VMM 测试。这种"先依赖、后测试"的顺序是集成测试的基本原则——每一个测试阶段都假设它之前的所有基础设施已经就绪。

### QEMU 集成测试

`kernel/test/test_vmm.cpp` 包含 10 个测试用例，全部在 QEMU 中运行，操作的是真实的页表、真实的 TLB 和真实的 PMM。我们把它们按功能分组讲解。

**第一组：初始化验证（test 1）**

```cpp
void test_init_pml4() {
    TEST_ASSERT_NE(g_vmm.kernel_pml4(), 0u);
}
```

最简单但也最基础的一个测试——验证 VMM 初始化后 `kernel_pml4` 非零。如果为 0 说明 `init()` 没有被调用，或者 CR3 读到了异常值。后续所有测试都建立在这个断言之上。

**第二组：map + translate 基本流程（test 2-3）**

```cpp
void test_map_translate() {
    uint64_t virt = 0x20000000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = g_vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);

    uint64_t result = g_vmm.translate(virt);
    TEST_ASSERT_EQ(result, phys);

    g_vmm.unmap(virt);
    g_pmm.free_page(phys);
}
```

test 2 是 map + translate 的标准流程：从 PMM 分配一个物理页，映射到虚拟地址 `0x20000000`，然后 translate 验证返回的物理地址一致。test 3 在此基础上验证页内偏移保留——映射 `0x20010000`，translate `0x20010123`，期望得到 `phys + 0x123`。这两个测试验证了 VMM 最核心的"映射建立"和"地址翻译"两条路径。

**第三组：unmap 与未映射地址（test 4-5）**

test 4 验证 unmap 的效果：先 map 再 translate 确认映射存在，然后 unmap，再次 translate 期望返回 0。test 5 更直接——对一个从未映射过的地址调用 translate，期望返回 0。它们一起验证了"映射可以清除"和"未映射地址查询为空"两个基本语义。

**第四组：多页映射与覆盖（test 6-8）**

test 6 映射两个不同的虚拟地址到两个不同的物理页，验证它们互不干扰。test 7 映射同一个虚拟地址到两个不同的物理页（remap），验证后者覆盖前者——`translate` 返回的是第二次映射的物理地址。test 8 对一个从未映射过的地址调用 unmap，验证这是安全的空操作。这三个测试覆盖了"多映射共存"、"映射覆盖"和"空操作安全性"三种交互模式。

**第五组：高位地址与 demand paging（test 9-10）**

```cpp
void test_high_address() {
    uint64_t virt = 0xFFFFFFFF80000000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = g_vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(g_vmm.translate(virt), phys);

    g_vmm.unmap(virt);
    g_pmm.free_page(phys);
}
```

test 9 验证内核空间高位 canonical 地址的映射——`0xFFFFFFFF80000000` 是 Cinux 的 `KERNEL_VMA`，对应 PML4 的最高几个条目。这说明 VMM 不只对用户空间的低地址有效，对内核空间的地址也能正确处理四级索引。

test 10 是 demand-paging 的端到端验证。由于当前 bootloader 已经用大页映射了全部物理内存，这个测试实际上验证的是直接映射区域的写入可读性——向 `0x40000000`（1GB 处）写入一个魔数然后读回，确认值一致。在更早的版本中，这个测试曾经真正触发 #PF，由 handler 分配物理页后再恢复执行。

### Host 单元测试框架

`test/unit/test_vmm.cpp` 是一个完全独立于内核的 host 端测试文件，它在一个模拟环境中重新实现了 VMM 的核心算法，用 420 行代码覆盖了比 QEMU 集成测试更多的边界情况。

**MockPMM —— bitmap 分配的物理内存模拟器**

```cpp
struct MockPMM {
    uint8_t  bitmap[MOCK_POOL_PAGES / 8];
    uint32_t next_page;

    MockPMM() : next_page(0) { memset(bitmap, 0, sizeof(bitmap)); }

    uint64_t alloc_page() {
        for (uint32_t i = 0; i < MOCK_POOL_PAGES; i++) {
            uint32_t byte = i / 8;
            uint32_t bit  = i % 8;
            if (!(bitmap[byte] & (1U << bit))) {
                bitmap[byte] |= static_cast<uint8_t>(1U << bit);
                return MOCK_POOL_BASE + static_cast<uint64_t>(i) * PAGE_SIZE;
            }
        }
        return 0;
    }
```

MockPMM 用一个 256 位的 bitmap 管理一个 256 页的模拟物理内存池（1MB），基地址 `0x2000000`。分配时线性扫描 bitmap 找到第一个空闲位，置位后返回对应的模拟物理地址。这个设计比真实的 PMM 简单得多——没有栈、没有锁、没有内存区域检测——但对于验证 VMM 的页表遍历算法来说，我们只需要"能分配出不同的物理页"这一条就够了。

**sim_memory —— 页表内容的真实存储**

```cpp
constexpr uint32_t SIM_PAGES = 128;
alignas(4096) uint8_t sim_memory[SIM_PAGES][PAGE_SIZE];

uint8_t* sim_virt_of(uint64_t phys) {
    return &sim_memory[(phys - MOCK_POOL_BASE) / PAGE_SIZE][0];
}
```

这是 host 测试的核心数据结构——128 个 4KB 对齐的字节数组，充当模拟物理内存。MockPMM 分配的物理地址就是 `MOCK_POOL_BASE + n * PAGE_SIZE`，`sim_virt_of` 把这个"物理地址"转换成 host 进程可以实际读写的指针。这和内核中 `phys_to_virt` 的角色完全一样，只是转换方式不同——内核用 `phys + KERNEL_VMA`，host 测试用数组索引。`alignas(4096)` 确保每个模拟页的起始地址是 4KB 对齐的，这很重要——如果不对齐，把 `sim_memory[n]` 当作 `PageEntry[512]` 来访问时，末尾可能跨页边界。

**TestVMM —— 算法的忠实镜像**

```cpp
class TestVMM {
public:
    void init() {
        MockPMM  pmm;
        uint64_t pml4_phys = pmm.alloc_page();
        pml4_              = reinterpret_cast<PageEntry*>(sim_virt_of(pml4_phys));
        memset(pml4_, 0, PAGE_SIZE);
        pml4_phys_ = pml4_phys;
        pmm_       = pmm;
    }
```

TestVMM 在 `init` 时分配一个空页作为 PML4 并清零，然后保存其物理地址。和真实 VMM 的 `init()` 相比，唯一区别是真实 VMM 从 CR3 读取 bootloader 已设置好的 PML4，而 TestVMM 从零开始创建。

它实现了 `walk_or_alloc` 和 `walk_only` 两个方法，分别对应真实 VMM 中 `walk_level(should_alloc=true)` 和 `walk_level(should_alloc=false)`。这个拆分比真实 VMM 更激进——完全去掉了 `should_alloc` 参数的运行时判断，因为 host 测试不需要考虑代码体积的共用性。`map`/`unmap`/`translate` 的逻辑和真实 VMM 一一对应，是算法正确性的独立验证。

### Host 单元测试用例

420 行代码中的 15 个测试按以下分组归类。

**正常路径测试（6 个）**：map+translate 基本流程、页内偏移保留、unmap 后 translate 返回 0、双页映射互不干扰、未映射地址 translate 返回 0、remap 覆盖旧映射。这组和 QEMU 集成测试高度重叠，但验证的是纯算法正确性——不涉及 TLB、不涉及 CR3、不涉及真正的物理内存。

**标志位测试（1 个）**：验证 map 时传入 `FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER` 后 translate 仍然返回正确的物理地址。这确保了标志位不会污染物理地址字段——`ADDR_MASK` 的分离是正确的。

**同 PT 多页测试（2 个）**：连续映射 16 个页（同一 PDPT/PD 条目，不同 PT slot），验证全部 translate 正确；然后 unmap 其中一个，验证相邻页不受影响。这测试了 PT 级别的隔离性——一个 PTE 的清零不会影响同一 PT 中的其他 PTE。

**边界情况测试（3 个）**：unmap 从未映射的地址（安全空操作）、高位 canonical 地址映射（`0xFFFFFFFF80000000`）、完整的 map→unmap→remap 循环。高位地址测试特别重要——它验证了 PML4 最高条目的索引计算是否正确，而真实内核的 higher-half 映射正是依赖这些高位条目。

**PageEntry 联合体测试（2 个）**：独立验证 `phys_addr()` 提取地址字段和 `is_present()` 检查 bit 0 的行为。这是对 RT-1 中 `PageEntry` 设计的单元级验证，不依赖任何 VMM 上下文。

## 设计决策

### 决策：demand-paging 的简化策略

**问题**：#PF handler 需要处理"页不存在"的情况。对于这个 tag 来说，demand paging 应该做到什么程度？

**本项目的做法**：对所有 P=0 的缺页，无条件分配一个新的物理页并映射到故障地址。不检查地址合法性、不维护页表使用上限、不做交换。

**备选方案**：Linux 的做法——维护完整的 `vm_area_struct` 链表，只有当故障地址落在某个合法的 VMA 区间内时才分配物理页；否则向进程发送 SIGSEGV。同时集成 page cache 和 swap，物理页可以从磁盘换入。

**为什么不选备选方案**：Cinux 目前没有进程地址空间描述（没有 VMA），也没有磁盘驱动和文件系统。Linux 那套完整的 demand paging 需要"进程管理 + VMA 描述 + page cache + 块设备驱动"四个子系统协同工作，这在内核开发的早期阶段显然不现实。我们的目标更朴素——验证 #PF handler 能正确地捕获缺页、分配物理页、建立映射并恢复执行，形成完整的"异常 → 处理 → 恢复"闭环。

**如果要扩展/改进，应该怎么做**：首先添加 VMA（Virtual Memory Area）数据结构，记录每个进程的合法地址范围。然后在 #PF handler 中用 VMA 校验故障地址——落在合法范围内才分配，否则向当前进程发送 SIGSEGV 信号。接下来引入 page cache，让 demand paging 不只分配零页，还能从文件系统或 swap 区域加载内容。

## 扩展方向

1. **写时复制（Copy-on-Write）**：在 #PF handler 中已经预留了 CoW 路径（检查 P=1、W/R=1、U/S=1 的组合）。研究 `fork()` 时如何将子进程的页表指向父进程的物理页并标记为只读，写时再分配新页。

2. **页替换算法**：当物理内存不足时，需要选择牺牲页换出到磁盘。研究 Linux 的 LRU（Least Recently Used）变种——利用 PTE 的 Accessed 位作为硬件提供的热度信息。

3. **内存映射文件（mmap）**：扩展 demand paging 使其能从文件系统加载数据——#PF handler 检查故障地址对应的 VMA 是否是文件映射，如果是则从磁盘读入相应页面。

4. **Guard page 与栈自动扩展**：当前 #PF handler 中已有 guard page 检测，但只是 panic。研究如何将 guard page 改为"栈扩展"——访问到 guard page 时自动分配新页并移动 guard page，实现栈的动态增长。

5. **Per-process 地址空间与 #PF 隔离**：研究在进程切换后，#PF handler 如何根据当前进程的 VMA 和 PML4 做出正确的分配决策——不同进程的相同虚拟地址应该映射到不同的物理页。

## 参考资料

- Intel SDM Vol.3A Section 4.7, pp.4-37 to 4-38：Page-Fault Exception 的完整描述，错误码格式（P/W/R/U/S/RSVD/I/D），CR2 寄存器的含义。Cinux 的 `handle_pf` 通过检查 bit 0（P 位）区分"页不存在"和"权限违反"，正是依据此处规定。
- Intel SDM Vol.3A Section 4.12, p.4-61：Virtual Memory 与 Demand Paging 的概念描述。Intel 明确指出"线性地址空间的某些部分不需要映射到物理地址空间，未映射地址的数据可以存储在外部"，这正是 demand paging 的理论基础。
- OSDev Wiki: [Page Fault](https://wiki.osdev.org/Page_Fault) — #PF 异常的错误码位域详解，P=0（not-present）和 P=1（protection violation）的区别。
- xv6 RISC-V `trap.c`：xv6 的 `usertrap()` 中处理缺页的方式——检查 `scause` 是否为 13（page fault），然后在进程的地址空间中分配新页。可以对比 RISC-V 和 x86-64 缺页处理机制的异同。
- Linux `arch/x86/mm/fault.c`：Linux 的 `do_page_fault()` 是一个远比 Cinux 复杂的缺页处理函数——包含 VMA 查找、COW 处理、swap 换入、信号发送等完整逻辑，可以看作 Cinux demand paging 的最终演进方向。
