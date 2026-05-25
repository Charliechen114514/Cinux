---
title: 034-process-fork-exec-2 · Fork 与 Exec
---

# fork() 与 CoW 页表复制

## 概览

本篇深入讲解 fork() 的完整实现——从 TCB 复制到 CoW 页表处理，以及 fork_child_trampoline 汇编入口。fork 是整个 034 tag 最核心也最复杂的部分，它需要精确地复制父进程的状态、设置子进程的栈帧、处理页表共享，并确保调度器首次调度子进程时一切正常。

关键设计决策：fork 使用 ctx.rsp 计算栈使用量进行栈复制，CoW 使用 bit 9 (FLAG_COW) 作为可用位标记。

## 架构图

```
Parent Task
  │ fork()
  ├── alloc PID ──→ PidAllocator::alloc()
  ├── alloc TCB ──→ new Task (memcpy from parent)
  ├── alloc stack ──→ PMM + VMM (with STACK_MAGIC)
  ├── CoW page tables:
  │   PML4[0..255] ──→ copy_page_table_level()
  │     ├─ intermediate levels: alloc new PT pages, recurse
  │     └─ PT level: share phys pages, mark FLAG_COW
  └── link into parent->children

Child Task (when scheduled):
  context_switch ──→ fork_child_trampoline
    xor rax,rax; ret ──→ fork() returns 0 in child
```

## 代码精讲

### fork() 函数签名

```cpp
int fork(PidAllocator& pid_alloc) {
```

fork 接收一个 PidAllocator 的引用。这个函数的核心任务是创建一个和父进程几乎完全相同的子进程——包括 TCB 副本、独立的内核栈、CoW 共享的地址空间，以及正确的调度器注册。

### 步骤 1-4：PID 和 TCB 分配

```cpp
auto* parent = Scheduler::current();
int child_pid = pid_alloc.alloc();
auto* child = new (std::align_val_t{alignof(Task)}) Task;
std::memcpy(child, parent, sizeof(Task));

child->tid         = next_tid.fetch_add(1, std::memory_order_relaxed);
child->pid         = child_pid;
child->ppid        = parent->pid;
child->state       = TaskState::Ready;
child->parent      = parent;
child->children    = nullptr;
child->exit_status = 0;
```

memcpy 复制整个父 TCB 后，需要修复子进程特有的字段。注意 tid 使用全局原子计数器递增，而 pid 由 PidAllocator 分配——两个概念是独立的。children 清空为 nullptr，因为子进程还没有自己的子进程。memcpy 同时也复制了父进程的 ctx 字段（包括 ctx.rsp），所以后面需要修正这些值。

### 步骤 5：内核栈分配

```cpp
uint64_t child_stack_phys = cinux::mm::g_pmm.alloc_pages(TaskBuilder::STACK_PAGES);
uint64_t child_stack_virt = alloc_stack_vaddr(TaskBuilder::STACK_PAGES);
uint64_t stack_size = TaskBuilder::STACK_PAGES * cinux::arch::PAGE_SIZE;

for (uint64_t i = 0; i < TaskBuilder::STACK_PAGES; i++) {
    uint64_t phys = child_stack_phys + i * cinux::arch::PAGE_SIZE;
    uint64_t virt = child_stack_virt + i * cinux::arch::PAGE_SIZE;
    cinux::mm::g_vmm.map(virt, phys, 0x03);
}
*reinterpret_cast<uint64_t*>(child_stack_virt) = TaskBuilder::STACK_MAGIC;
```

分配 STACK_PAGES（4）个连续物理页作为子进程的内核栈，映射到虚拟地址空间。栈底写入 STACK_MAGIC（0xDEADC0DE），用于运行时检测栈被覆盖——如果这个位置的值变了，说明栈溢出踩到了栈底的 magic 值。

### 步骤 6-7：栈内容复制与 rsp 重定位

```cpp
uint64_t parent_stack_used = parent->kernel_stack_top - parent->ctx.rsp;
std::memcpy(
    reinterpret_cast<void*>(child_stack_virt + stack_size - parent_stack_used),
    reinterpret_cast<void*>(parent->ctx.rsp),
    parent_stack_used
);

child->kernel_stack     = child_stack_virt;
child->kernel_stack_top = child_stack_virt + stack_size;
child->ctx.rsp = child_stack_virt + stack_size - parent_stack_used;
```

这是 fork 最精妙的部分。parent_stack_used 计算的是父进程栈从当前位置到栈顶已经使用了多少字节。然后把等量的内容复制到子进程栈的对应位置——从子栈的栈顶往下数 parent_stack_used 个字节的地方开始写入。子进程的 ctx.rsp 也设为对应的位置，这样子进程被调度器恢复时，栈上的内容和父进程完全一致。

子进程的 ctx.rip 设置为 fork_child_trampoline 的地址，这样子进程首次被调度时不会继续执行 fork 的后续代码，而是走 trampoline 路径返回 0。

### 步骤 8：CoW 页表复制

```cpp
if (parent->addr_space != nullptr) {
    child->addr_space = new cinux::mm::AddressSpace();
    auto* parent_pml4_table = phys_to_virt(parent->addr_space->pml4_phys());
    auto* child_pml4_table  = phys_to_virt(child->addr_space->pml4_phys());

    for (uint32_t i = 0; i < 256; i++) {
        if (!parent_pml4_table[i].is_present()) continue;

        uint64_t new_page = cinux::mm::g_pmm.alloc_page();
        // ... zero and set child_pml4_table[i] ...
        copy_page_table_level(parent_pml4_table[i].phys_addr(), new_page, 3);
    }
}
```

只复制 PML4[0..255]（用户空间），跳过不 present 的条目。子进程通过共享的 PML4[256..511]（高半区）访问内核资源。每一级都分配新的页表页，但在最底层共享物理页并标记 CoW。

### copy_page_table_level 递归实现

```cpp
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    auto* src_table = phys_to_virt(src_phys);
    auto* dst_table = phys_to_virt(dst_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!src_table[i].is_present()) continue;

        if (level > 1) {
            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            // ... zero new_table, set dst entry, recurse ...
            copy_page_table_level(src_table[i].phys_addr(), new_page, level - 1);
        } else {
            // PT level: share phys page, mark CoW
            uint64_t entry_flags = src_table[i].raw & FLAG_MASK;
            dst_table[i].raw = src_table[i].raw;
            if (entry_flags & FLAG_WRITABLE) {
                dst_table[i].raw &= ~FLAG_WRITABLE;
                dst_table[i].raw |= FLAG_COW;
                src_table[i].raw &= ~FLAG_WRITABLE;
                src_table[i].raw |= FLAG_COW;
            }
        }
    }
}
```

中间级别（level 3=PDPT, 2=PD）分配新页表页并递归。叶子级别（level 1=PT）共享物理页并标记 CoW。关键点：父子两个 PTE 都要被修改——都变成只读 + FLAG_COW。这样无论哪方先写入，都会触发 #PF 进入 handle_cow_fault。注意 FLAG_COW 定义在 paging_config.hpp 中，是 bit 9（Intel SDM 标注为 "Available for software use"）。

### handle_cow_fault 实现

```cpp
bool handle_cow_fault(uint64_t fault_vaddr) {
    // ... get PTE via get_pte() ...
    if (!pte->is_present()) return false;
    if (pte->raw & FLAG_WRITABLE) return false;
    if (!(pte->raw & FLAG_COW)) return false;

    uint64_t old_phys = pte->phys_addr();
    uint64_t new_phys = cinux::mm::g_pmm.alloc_page();
    // Copy page contents
    auto* src = reinterpret_cast<uint8_t*>(old_phys + KERNEL_VMA);
    auto* dst = reinterpret_cast<uint8_t*>(new_phys + KERNEL_VMA);
    for (uint64_t i = 0; i < PAGE_SIZE; i++) dst[i] = src[i];

    // Update PTE: point to new page, restore write, clear CoW
    pte->set_phys_addr(new_phys);
    pte->raw |= FLAG_WRITABLE;
    pte->raw &= ~FLAG_COW;

    cinux::arch::flush_tlb(fault_vaddr & ~(PAGE_SIZE - 1));
    return true;
}
```

CoW page fault 的处理流程很清晰：分配新物理页、从共享页复制内容、更新 PTE 指向新页并恢复可写、清除 FLAG_COW、刷新 TLB。注意复制时通过 KERNEL_VMA 高半区映射访问物理内存。

### fork_child_trampoline

```asm
fork_child_trampoline:
    xorq %rax, %rax
    ret
```

两条指令，干净利落。调度器恢复子进程的 ctx 后，RIP 指向这里，RSP 指向子栈上的某个位置。xor rax,rax 使 fork 返回 0，ret 弹出栈顶地址到 RIP——子进程从 fork 调用点"返回"了。子进程从不真正"调用" fork——它是被调度器从某个点"恢复"的，trampoline 只是设置正确的返回值然后跳转回去。

### 步骤 9-10：链入子进程列表并加入调度器

```cpp
// Step 9: Link child into parent's children list
child->wait_next = parent->children;
parent->children = child;

// Step 10: Add child to the scheduler
Scheduler::add_task(child);
```

子进程通过 wait_next 指针链入父进程的 children 单链表（头插法）。然后加入调度器，等待被调度执行。fork 返回 child_pid 给父进程。子进程不会从这里返回——它被调度时从 trampoline 开始执行。

## 设计决策

### 决策：CoW 标记位

**问题**: 用哪个 PTE 位标记 CoW？
**做法**: 使用 bit 9（Available bit），定义为 FLAG_COW。
**备选方案**: 用 bit 8（Global）或 bit 7（PS/Huge）或自定义引用计数表。
**原因**: Intel SDM 明确指出 bit 9 是 "Available for software use"。bit 8 是 Global TLB 标志，bit 7 是 Page Size，都有硬件语义。独立的引用计数表增加复杂度——Cinux 用"双写触发"策略（父子都标记 CoW，谁先写谁复制），不需要引用计数。

### 决策：栈复制策略

**问题**: fork 如何复制父进程的栈到子进程？
**做法**: 计算 parent_stack_used = kernel_stack_top - ctx.rsp，然后 memcpy 等量内容到子栈对应位置。
**备选方案**: 通过帧指针（RBP）精确定位 fork 的返回地址，只复制必要的栈帧。
**原因**: 使用 ctx.rsp 直接计算已用栈量更简单可靠，不依赖编译器生成的帧指针结构。整个父栈从 ctx.rsp 到 kernel_stack_top 的内容都被复制，子进程的 ctx.rsp 重定位到子栈的对应位置。配合 trampoline 机制，子进程首次调度时直接从 fork 返回点开始执行。

## 扩展方向

- 帧指针优化——如果需要精确控制只复制 fork 相关的栈帧（而非整个已用栈），可以加入帧指针定位和 no-omit-frame-pointer 属性
- 实现 swap-in/swap-out（CoW 页可换出到磁盘）
- 引用计数优化（记录每个物理页被多少 PTE 共享，减到 1 就清除 CoW 标记）
- Copy-on-write for pipe buffer（fork 后共享 pipe buffer 而非复制）

## 参考资料
- Intel SDM Vol.3A Section 4.7 "Page-Table Entries" — Available bits
- OSDev Wiki Copy-on-write: https://wiki.osdev.org/Paging
- xv6 vm.c (uvmcopy): https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c
