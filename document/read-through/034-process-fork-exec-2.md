# fork() 与 CoW 页表复制

## 概览

本篇深入讲解 fork() 的完整实现——从 TCB 复制到 CoW 页表处理，以及 fork_child_trampoline 汇编入口。fork 是整个 034 tag 最核心也最复杂的部分，它需要精确地复制父进程的状态、设置子进程的栈帧、处理页表共享，并确保调度器首次调度子进程时一切正常。

关键设计决策：fork 使用帧指针定位返回地址（需 `no-omit-frame-pointer` 属性），CoW 使用 bit 9 (FLAG_COW) 作为可用位标记。

## 架构图

```
Parent Task
  │ fork()
  ├── alloc PID ──→ PidAllocator::alloc()
  ├── alloc TCB ──→ new Task (memcpy from parent)
  ├── alloc stack ──→ PMM + VMM (with guard page)
  ├── CoW page tables:
  │   PML4[0..255] ──→ copy_page_table_level()
  │     ├─ intermediate levels: alloc new PT pages, recurse
  │     └─ PT level: share phys pages, mark FLAG_COW
  ├── copy FDTable ──→ new FDTable, clone File objects
  └── init GS MSR ──→ gs_base=0, kgs_base=per_cpu

Child Task (when scheduled):
  context_switch ──→ fork_child_trampoline
    xor rax,rax; ret ──→ fork() returns 0 in child
```

## 代码精讲

### fork() 函数签名与帧指针保护

```cpp
__attribute__((optimize("no-omit-frame-pointer"), noinline))
int fork(PidAllocator& pid_alloc) {
```

这两个属性缺一不可。`optimize("no-omit-frame-pointer")` 强制 GCC 为这个函数生成标准帧指针（push rbp; mov rbp, rsp），这样 [rbp] 是调用者的 RBP、[rbp+8] 是返回地址。`noinline` 防止编译器内联——内联后函数边界消失，帧指针语义也跟着变了。

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
child->fd_table    = nullptr;
```

memcpy 复制整个父 TCB 后，需要修复子进程特有的字段。注意 tid 使用全局原子计数器递增，而 pid 由 PidAllocator 分配——两个概念是独立的。fd_table 先置 nullptr，后面单独处理。

### 步骤 5：内核栈分配与 guard page

```cpp
uint64_t child_stack_phys = cinux::mm::g_pmm.alloc_pages(TaskBuilder::STACK_PAGES);
uint64_t child_guard_virt = alloc_stack_vaddr(TaskBuilder::STACK_PAGES + 1);
uint64_t child_stack_virt = child_guard_virt + cinux::arch::PAGE_SIZE;
// DO NOT map child_guard_virt
for (uint64_t i = 0; i < TaskBuilder::STACK_PAGES; i++) {
    uint64_t phys = child_stack_phys + i * cinux::arch::PAGE_SIZE;
    uint64_t virt = child_stack_virt + i * cinux::arch::PAGE_SIZE;
    cinux::mm::g_vmm.map(virt, phys, 0x03);
}
*reinterpret_cast<uint64_t*>(child_stack_virt) = TaskBuilder::STACK_MAGIC;
```

保留一个虚拟页作为 guard page（故意不映射）。栈溢出时写入 guard page 触发 #PF，handler 检测到 fault 地址在 guard 范围内就报告 overflow。STACK_MAGIC (0xDEADC0DE) 写在栈底，用于运行时检测栈被覆盖。

### 步骤 6-7：栈帧复制与重定位

```cpp
uint64_t current_rsp, current_rbp;
__asm__ volatile("movq %%rsp, %0" : "=r"(current_rsp));
__asm__ volatile("movq %%rbp, %0" : "=r"(current_rbp));

uint64_t full_stack_used   = parent->kernel_stack_top - current_rsp;
uint64_t child_stack_start = child_stack_virt + stack_size - full_stack_used;
std::memcpy(reinterpret_cast<void*>(child_stack_start),
            reinterpret_cast<void*>(current_rsp), full_stack_used);

child->ctx.rsp = (current_rbp + 8) - current_rsp + child_stack_start;
child->ctx.rbp = *reinterpret_cast<uint64_t*>(current_rbp);
child->ctx.rip = reinterpret_cast<uint64_t>(fork_child_trampoline);
```

这是整个 fork 最精妙的部分。current_rsp 是 fork() 函数内部的 RSP（包含了从 fork 调用点到当前的所有栈帧），current_rbp 是 fork() 的帧指针。[current_rbp + 8] 是 fork 的返回地址（被调用者 push 的），[current_rbp] 是调用者的 RBP。

子进程的 ctx.rsp 计算公式把 fork 返回地址在父栈上的位置重定位到子栈：fork 返回地址在 (current_rbp + 8) 处，而 (current_rbp + 8) - current_rsp 是它相对于 RSP 的偏移，加上 child_stack_start 就是子栈上的对应位置。

ctx.rbp 设为调用者的帧指针（从 [current_rbp] 读出），ctx.rip 指向 trampoline。调度器首次调度子进程时，context_switch 恢复这些值，trampoline ret 到 ctx.rsp 指向的地址（fork 返回地址），rax 被清零。

### 步骤 8：CoW 页表复制

```cpp
if (parent->addr_space != nullptr) {
    child->addr_space = new cinux::mm::AddressSpace();
    auto* parent_pml4_table = phys_to_virt(parent->addr_space->pml4_phys());
    auto* child_pml4_table  = phys_to_virt(child->addr_space->pml4_phys());

    for (uint32_t i = 0; i < 256; i++) {
        if (!parent_pml4_table[i].is_present()) continue;
        if (!(parent_pml4_table[i].raw & FLAG_USER)) continue;

        uint64_t new_page = cinux::mm::g_pmm.alloc_page();
        // ... zero and set child_pml4_table[i] ...
        copy_page_table_level(parent_pml4_table[i].phys_addr(), new_page, 3);
    }
}
```

只复制 PML4[0..255]（用户空间），跳过没有 FLAG_USER 的条目（内核恒等映射）。子进程通过共享的 PML4[256..511]（高半区）访问内核资源。

### copy_page_table_level 递归实现

```cpp
void copy_page_table_level(uint64_t src_phys, uint64_t dst_phys, int level) {
    auto* src_table = phys_to_virt(src_phys);
    auto* dst_table = phys_to_virt(dst_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!src_table[i].is_present()) continue;
        if (!(src_table[i].raw & FLAG_USER)) continue;  // 跳过内核映射
        if (src_table[i].huge) {
            dst_table[i].raw = src_table[i].raw;  // huge page 直接共享
            continue;
        }

        if (level > 1) {
            uint64_t new_page = cinux::mm::g_pmm.alloc_page();
            // ... zero new_table, set dst entry, recurse ...
            copy_page_table_level(src_table[i].phys_addr(), new_page, level - 1);
        } else {
            // PT level: share phys page, mark CoW
            dst_table[i].raw = src_table[i].raw;
            if (src_table[i].raw & FLAG_WRITABLE) {
                dst_table[i].raw &= ~FLAG_WRITABLE;
                dst_table[i].raw |= FLAG_COW;
                src_table[i].raw &= ~FLAG_WRITABLE;
                src_table[i].raw |= FLAG_COW;
            }
        }
    }
}
```

中间级别（level 3=PDPT, 2=PD）分配新页表页并递归。叶子级别（level 1=PT）共享物理页并标记 CoW。关键点：父子两个 PTE 都要被修改——都变成只读 + FLAG_COW。这样无论哪方先写入，都会触发 #PF 进入 handle_cow_fault。

### fork_child_trampoline

```asm
fork_child_trampoline:
    xorq %rax, %rax
    ret
```

两条指令，干净利落。调度器恢复子进程的 ctx 后，RIP 指向这里，RSP 指向子栈上的 fork 返回地址。xor rax,rax 使 fork 返回 0，ret 弹出返回地址到 RIP——子进程从 fork 调用点"返回"了。

### context_switch.S 的 GS MSR 保存/恢复

```asm
/* Save GS MSRs */
movq $0xC0000101, %rcx
rdmsr
movl %eax, 64(%rdi)
movl %edx, 68(%rdi)

movq $0xC0000102, %rcx
rdmsr
movl %eax, 72(%rdi)
movl %edx, 76(%rdi)

/* Restore GS MSRs */
movl 64(%rsi), %eax
movl 68(%rsi), %edx
movq $0xC0000101, %rcx
wrmsr

movl 72(%rsi), %eax
movl 76(%rsi), %edx
movq $0xC0000102, %rcx
wrmsr
```

MSR_GS_BASE (0xC0000101) 和 MSR_KERNEL_GS_BASE (0xC0000102) 是 64 位寄存器，rdmsr/wrmsr 通过 EDX:EAX 传递高 32 位和低 32 位。保存到 CpuContext 的 gs_base（偏移 64）和 kgs_base（偏移 72）。这保证了即使任务在 swapgs 对之间被切出，调度器也能正确恢复 GS 状态。

## 设计决策

### 决策：CoW 标记位

**问题**: 用哪个 PTE 位标记 CoW？
**做法**: 使用 bit 9（Available bit），定义为 FLAG_COW。
**备选方案**: 用 bit 8（Global）或 bit 7（PS/Huge）或自定义引用计数表。
**原因**: Intel SDM 明确指出 bit 9 是 "Available for software use"。bit 8 是 Global TLB 标志，bit 7 是 Page Size，都有硬件语义。独立的引用计数表增加复杂度——Cinux 用"双写触发"策略（父子都标记 CoW，谁先写谁复制），不需要引用计数。

## 扩展方向

- 实现 swap-in/swap-out（CoW 页可换出到磁盘）
- 引用计数优化（记录每个物理页被多少 PTE 共享，减到 1 就清除 CoW 标记）
- Copy-on-write for pipe buffer（fork 后共享 pipe buffer 而非复制）

## 参考资料
- Intel SDM Vol.3A Section 4.7 "Page-Table Entries" — Available bits
- OSDev Wiki Copy-on-write: https://wiki.osdev.org/Copy-on-write
- xv6 vm.c (uvmcopy): https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c
