---
title: 019-proc-context-3 · 进程上下文
---

# 019-3 通读：Bug 修复与测试验证——Higher-Half 修复、大页拆分、线程退出崩溃与集成测试

## 概览

本文是 tag `019_proc_context` 三篇通读教程的第三篇，聚焦于三个关键 bug 修复和两套测试代码。前两篇我们看到了调度器"应该怎样工作"，但实际把代码跑起来之后，踩了三个坑：第一，mini kernel 的 ELF 加载器错误地去掉了 higher-half 偏移，导致大内核运行在恒等映射地址而非 higher-half，破坏了地址空间隔离；第二，`walk_level` 遇到 2MB 大页时不知道怎么拆分，导致栈映射失败；第三，线程退出时 `ret` 弹出了垃圾地址加上 `exit_current` 的指针覆盖 bug，两个问题叠加导致 triple fault。我们会逐一拆解这些 bug 的根因和修复方式，然后看 QEMU 内核测试和 Host 单元测试如何覆盖调度器的各个功能点。

## 架构图

```
    Bug 修复关系图：
    ┌─────────────────────────────────────────────────────────────┐
    │  1. Higher-Half 修复                                        │
    │     elf_loader: return saved_entry (不去偏移)               │
    │     address_space: 只复制 PML4[256..511]                    │
    │     test_address_space: 移除 workaround                    │
    │                                                              │
    │  2. 大页拆分                                                 │
    │     walk_level: 检测 entry.huge → 分配 PT → 填充 512 条    │
    │     4KB PTE → 替换原来的大页条目                             │
    │                                                              │
    │  3. 线程退出崩溃                                             │
    │     TaskBuilder: 栈顶压入 exit_current 地址                 │
    │     exit_current: 先保存 prev = current_ 再切换             │
    └─────────────────────────────────────────────────────────────┘

    测试覆盖：
    ├── QEMU 内核测试 (kernel/test/test_scheduler.cpp)
    │     ├── TaskBuilder 构造验证
    │     ├── RoundRobin 入队/出队/轮转
    │     ├── CpuContext 布局断言
    │     ├── context_switch 协作切换
    │     └── 状态转换、block/unblock
    │
    └── Host 单元测试 (test/unit/test_scheduler.cpp)
          ├── CpuContext sizeof / offsetof
          ├── TaskState 枚举值
          ├── RoundRobin 空/单/多任务
          ├── RoundRobin dequeue 头/中/尾/不存在
          ├── Scheduler init/register/add_task
          └── Task 零初始化默认值
```

## 代码精讲

### Bug 1：ELF 加载器 Higher-Half 修复

```cpp
// elf_loader.cpp — 修改前（错误）
constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
uint64_t entry = saved_entry;
if (entry >= HIGHER_HALF_BASE) {
    entry = entry - HIGHER_HALF_BASE;  // 0xFFFFFFFF81000000 → 0x1000000
}
return entry;

// elf_loader.cpp — 修改后（正确）
return saved_entry;
```

这个 bug 的根因在于对 higher-half 内核地址的理解偏差。大内核的 ELF 入口点链接时就是 higher-half 虚拟地址（`0xFFFFFFFF81000000`），引导加载程序已经在 higher-half 映射中放置了所有物理内存。所以 `saved_entry` 本身就是可以直接跳转的虚拟地址，不需要减去偏移转成物理地址。修改前的代码把它转换成了 `0x1000000`（恒等映射地址），导致大内核运行在恒等映射区域，而引导时设置的两套映射指向不同的 PDPT 子树——PML4[0] 指向恒等映射的 PDPT，PML4[511] 指向 higher-half 的 PDPT。

这个问题的连锁反应很严重：`AddressSpace` 构造函数原本会复制 PML4[0]（恒等映射条目）到新创建的地址空间，导致所有进程共享同一套恒等映射的 PDPT/PD 子树。一个进程在这个共享子树中修改的页表项会"泄漏"到其他进程的地址空间，彻底破坏了进程隔离。

修复方案是双管齐下：ELF 加载器直接返回 `saved_entry`（higher-half 地址），同时 `AddressSpace` 构造函数不再复制 PML4[0]，只复制 PML4[256..511]（内核 higher-half 部分）：

```cpp
// address_space.cpp — 修改后
auto* kern_pml4 = phys_to_virt(kernel_pml4_);
for (uint32_t i = USER_PML4_END; i < PT_ENTRIES; i++) {
    pml4[i].raw = kern_pml4[i].raw;
}
```

`USER_PML4_END = 256`，所以只复制 PML4 表的后半部分（索引 256 到 511），这些条目覆盖了内核的 higher-half 映射。PML4[0]（恒等映射）不再被复制到新地址空间，每个进程的 PML4[0] 都是空的，用户空间的映射需要由进程自己通过 `map()` 建立。

### Bug 2：walk_level 大页拆分

```cpp
// vmm.cpp — walk_level 中的大页检测与拆分
PageEntry* walk_level(PageEntry* table, uint64_t index, bool should_alloc) {
    PageEntry& entry = table[index];

    if (entry.is_present()) {
        if (entry.huge) {
            if (!should_alloc) {
                return nullptr;
            }

            uint64_t big_phys  = entry.phys_addr();
            uint64_t big_flags = entry.raw & ~ADDR_MASK;

            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            if (new_page == 0) {
                return nullptr;
            }

            auto* new_table = phys_to_virt(new_page);
            for (uint32_t i = 0; i < PT_ENTRIES; i++) {
                new_table[i].raw = (big_phys + static_cast<uint64_t>(i) * PAGE_SIZE)
                                 | (big_flags & ~FLAG_HUGE);
            }

            entry.raw = new_page | FLAG_PRESENT | FLAG_WRITABLE;
        }
        return phys_to_virt(entry.phys_addr());
    }
    // ... 分配新页表的逻辑 ...
}
```

`walk_level` 是 VMM 的核心辅助函数，它逐级遍历页表直到找到目标级别的页表项。在之前的实现中，如果某一层遇到一个 huge page 标志（`entry.huge`），函数会直接返回那个大页的物理地址——这对于只读遍历没问题，但如果需要在那个地址范围中映射一个 4KB 页（比如线程栈），就必须把 2MB 大页拆分成 512 个 4KB 页。

拆分逻辑是这样的：先保存大页的物理基地址和标志位，然后分配一个新的页表（PT），把 512 个 4KB PTE 填进去——每个 PTE 指向 `big_phys + i * PAGE_SIZE`，标志位沿用大页的标志但去掉 `FLAG_HUGE`。最后把原来的大页条目替换为指向新 PT 的普通条目。这个操作必须在 `should_alloc` 为 true 时才执行，否则说明调用者只是想查找而不是修改，遇到大页应该返回 nullptr 表示"无法在不修改页表的情况下获取子表"。

这个 bug 在 `TaskBuilder::build()` 中触发——`g_vmm.map()` 调用 `walk_level` 遍历页表，如果内核栈的目标虚拟地址落在一个 2MB 大页范围内，原来的代码会因为返回了错误的物理地址而导致映射失败。

### Bug 3：线程退出崩溃——两个 bug 的叠加

这是整个 tag 中最精彩的 debug 故事。现象是两个线程交替打印 5 轮后，`thread_a` 退出时 QEMU 报 `emulation failure, RIP=0xDEADC0DE`，`thread_b` 没有完成就 triple fault。

**Bug 3a：线程函数无返回地址**

```cpp
// TaskBuilder::build() — 修改前
task->ctx.rsp = stack_virt + stack_size;  // 裸栈顶，无返回地址
task->ctx.rip = entry_point;
```

`context_switch` 用 `jmp *56(%rsi)` 跳到入口函数，不是 `call`，所以不会在栈上压入返回地址。当线程函数执行到最后的 `ret` 时，CPU 从 RSP 指向的位置弹出一个 8 字节值作为返回地址——但栈顶是空的，一路弹下去最终弹到栈底的魔数 `0xDEADC0DE`，RIP 跳到了这个非法地址。

修复方法在第一篇已经讲过：在栈顶放置 `exit_current` 的地址。

**Bug 3b：exit_current 指针覆盖**

```cpp
// Scheduler::exit_current() — 修改前
current_ = next;                           // current_ 被覆盖
context_switch(&current_->ctx, &next->ctx); // from == to，空操作
```

即使修复了 Bug 3a，线程退出还是会崩。原因是 `exit_current` 先把 `current_` 设为 `next`，然后调用 `context_switch(&current_->ctx, &next->ctx)`——此时 `from` 和 `to` 都指向同一个 `next` 线程的 CpuContext，上下文切换变成了空操作（保存自己到自己的 ctx，然后从自己的 ctx 恢复），线程根本没有切换走，继续在已退出的线程栈上执行，最终崩溃。

```cpp
// Scheduler::exit_current() — 修复后
Task* prev = current_;                     // 先保存
if (prev != nullptr) {
    prev->state = TaskState::Dead;
    prev->sched_class->dequeue(prev);
}
Task* next = default_rr_.pick_next();
// ...
current_ = next;
context_switch(&prev->ctx, &next->ctx);    // from ≠ to，正确切换
```

修复很简单：用局部变量 `prev` 保存 `current_` 的旧值，然后在 `context_switch` 中使用 `prev` 作为 `from` 参数。这保证了 `from`（已退出线程的 ctx）和 `to`（下一个线程的 ctx）是不同的指针。

### QEMU 内核测试——test_scheduler.cpp

内核测试运行在 QEMU 的大内核测试框架中，使用真实的 PMM/VMM/Heap 后端。测试入口是 `run_scheduler_tests()`，由 `main_test.cpp` 在所有子系统初始化完成后调用。

**CpuContext 布局验证**：

```cpp
void test_layout() {
    TEST_ASSERT_EQ(sizeof(CpuContext), 64u);
    TEST_ASSERT_EQ(offsetof(CpuContext, r15), 0u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rsp), 48u);
    TEST_ASSERT_EQ(offsetof(CpuContext, rip), 56u);
    TEST_ASSERT_EQ(alignof(CpuContext), 16u);
}
```

这些断言确保 C++ 结构体的布局与 `context_switch.S` 中使用的偏移量完全匹配。如果有人修改了 `CpuContext` 的字段顺序或新增了字段而忘了更新汇编，这个测试会在 QEMU 中立刻失败。

**协作式上下文切换实测**：

```cpp
static CpuContext boot_ctx, task_a_ctx, task_b_ctx;

static void task_a_func() {
    task_a_count++;
    context_switch(&task_a_ctx, &task_b_ctx);  // 切到 B
    task_a_count++;
    done = true;
    context_switch(&task_a_ctx, &boot_ctx);    // 切回 boot
}

static void task_b_func() {
    task_b_count++;
    context_switch(&task_b_ctx, &task_a_ctx);  // 切回 A
}

void test_cooperative_switch() {
    // ... 初始化上下文和静态栈 ...
    task_a_ctx.rip = reinterpret_cast<uint64_t>(task_a_func);
    task_a_ctx.rsp = reinterpret_cast<uint64_t>(&stack_a[4096]);
    task_b_ctx.rip = reinterpret_cast<uint64_t>(task_b_func);
    task_b_ctx.rsp = reinterpret_cast<uint64_t>(&stack_b[4096]);

    context_switch(&boot_ctx, &task_a_ctx);    // 启动 A

    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQ(task_a_count, 2);
    TEST_ASSERT_EQ(task_b_count, 1);
}
```

这个测试用裸 `CpuContext` 和静态栈缓冲区直接调用 `context_switch`，绕过整个 Scheduler，验证汇编原语本身的正确性。执行流是 boot → A → B → A → boot，最终检查 `task_a_count == 2`（A 执行了两段代码）和 `task_b_count == 1`（B 执行了一次）。

**RoundRobin 轮转验证**：

```cpp
void test_enqueue_dequeue() {
    RoundRobin rr;
    Task *t1 = ..., *t2 = ..., *t3 = ...;
    rr.enqueue(t1); rr.enqueue(t2); rr.enqueue(t3);

    TEST_ASSERT_EQ(rr.pick_next(), t1);  // A
    TEST_ASSERT_EQ(rr.pick_next(), t2);  // B
    TEST_ASSERT_EQ(rr.pick_next(), t3);  // C
    TEST_ASSERT_EQ(rr.pick_next(), t1);  // 绕回 A
}
```

三个任务入队后，`pick_next` 按入队顺序依次返回，绕一圈后回到第一个——验证了真正的轮转语义。`test_dequeue_middle` 测试从中间移除任务后队列的正确性，`test_empty_pick_next` 验证空队列返回 nullptr。

### Host 单元测试——test/unit/test_scheduler.cpp

Host 测试在开发机上编译运行，不依赖 QEMU 和内核基础设施。它重新实现了 `RoundRobin` 和 `Scheduler` 的纯逻辑部分，用栈上的 `Task` 对象进行白盒测试。

```cpp
// Host 测试中重新定义的 Task（精简版）
struct Task {
    CpuContext       ctx;
    TaskState        state;
    uint64_t         tid;
    uint64_t         priority;
    uint64_t         kernel_stack;
    uint64_t         kernel_stack_top;
    void*            addr_space;
    const char*      name;
    SchedulingClass* sched_class;
};
```

Host 测试的重点是 RoundRobin 的边界条件：空队列 `pick_next` 返回 nullptr、单任务入队后出队队列为空、中间删除后顺序保持正确、头部和尾部删除的正确性、删除不存在的任务是空操作。`Scheduler` 的测试覆盖了 `init` 注册默认类、`register_class` 超过上限的静默忽略、`add_task` 在 `sched_class` 为空时自动使用默认 RoundRobin、显式指定 `sched_class` 时使用指定的策略。

这种"在 Host 上用简化数据结构测试核心逻辑"的模式非常有用——它可以在秒级完成编译和运行，不需要等 QEMU 启动，适合 TDD 风格的开发。代价是必须手动维护两份 `RoundRobin`/`Scheduler` 实现（内核版和测试版）的同步，如果改了内核版的逻辑而忘了更新测试版，测试通过并不意味着内核代码正确。

### exception_handlers.cpp——页错误处理 CR3 感知

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));
    uint64_t err = frame->error_code;

    if ((err & 0x01) == 0) {
        uint64_t virt_page = fault_addr & ~0xFFFULL;
        uint64_t phys = cinux::mm::g_pmm.alloc_page();
        if (phys != 0) {
            uint64_t cur_cr3 = cinux::arch::read_cr3();
            bool ok = g_vmm.map(virt_page, phys,
                                FLAG_PRESENT | FLAG_WRITABLE, &cur_cr3);
            if (ok) { return; }
            cinux::mm::g_pmm.free_page(phys);
        }
    }
    // ... fatal output ...
}
```

页错误处理程序的一个重要修改：demand-page 时先读取当前 CR3 值，传给 `g_vmm.map()` 作为目标页表。在引入进程后，不同进程有不同的 CR3（不同的 PML4），demand-page 必须在当前进程的页表中进行映射，而不是默认使用全局内核页表。原来的代码直接调用 `g_vmm.map(virt_page, phys, flags)` 不传 CR3，在进程隔离场景下会在错误的页表中创建映射。

## 设计决策

**为什么保留两套测试（QEMU + Host）？** QEMU 测试验证完整的内核环境（真实 PMM/VMM/Heap + 汇编上下文切换），Host 测试验证纯 C++ 逻辑的边界条件。两者互补：QEMU 测试能发现汇编偏移量错误和内存分配问题，Host 测试能快速迭代 RoundRobin 的边界条件。缺点是维护成本翻倍，每改一处内核逻辑需要同步更新 Host 测试的简化实现。

**为什么 demand-page 要传 CR3？** 在单地址空间时代（所有线程共享同一个 PML4），`g_vmm.map()` 可以使用默认页表。引入 `AddressSpace` 后，每个进程有独立的 PML4，`handle_pf` 必须使用当前 CR3 指向的页表来处理缺页。如果继续使用全局页表，demand-page 创建的映射会出现在错误进程的地址空间中，导致更难排查的隔离性 bug。

**大页拆分为什么在 walk_level 中做？** 拆分逻辑放在 `walk_level` 而不是独立的 `split_huge_page` 函数中，是因为 `walk_level` 是所有页表遍历的必经之路——不管是 `map()`、`unmap()` 还是 `translate()`，都通过 `walk_level` 逐级下降。在这里做拆分保证了对调用者透明：`g_vmm.map()` 不需要知道自己操作的目标地址原本是不是大页。

## 扩展方向

- **大页拆分的引用计数**：当前拆分后直接替换大页条目，如果有多个地址空间共享这个大页，拆分只会影响当前页表。未来需要引用计数来决定何时真正拆分。
- **demand-page 的地址空间感知**：当前 `handle_pf` 通过读 CR3 获取当前页表，更完整的方案是直接从 `Scheduler::current()->addr_space` 获取。
- **TLB shootdown**：大页拆分后需要刷新 TLB 中对应 2MB 范围的所有条目，当前依赖单次 `INVLPG` 只刷新了 4KB 范围。

## 参考资料

- Intel SDM Vol. 3A, Section 4.5.2: CR3 与地址空间切换
- Intel SDM Vol. 3A, Section 4.3.2: 大页 (2MB/1GB) 的页表格式
- document/notes/019_proc_context/001_higher_half_fix.md: Higher-Half 修复调试笔记
- document/notes/019_proc_context/002_thread_exit_crash.md: 线程退出崩溃排查笔记
