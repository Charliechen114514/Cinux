---
title: 034-process-fork-exec-2 · Fork 与 Exec
---

# fork 的艺术——COW 页表与双返回

> 标签：fork, Copy-On-Write, page table, context switch, x86-64
> 前置：tag 034-1 ELF 格式解析与 PID 分配器

## 前言

上一篇我们准备好了 ELF 解析器和 PID 分配器，现在是时候实现 fork 了。

fork 是 UNIX 系统最独特的系统调用——一次调用，两次返回。父进程拿到子进程的 PID，子进程拿到 0。这个看似魔法的语义背后，是对进程状态（TCB、页表、栈帧）极其精确的复制和重定位。

说实话，fork 是这两个 tag 里最复杂的部分——页表递归复制、CoW 标记、子进程栈帧重定位、trampoline 机制……每一个都需要精确的实现。我们这一篇不只讲实现，还会把这些设计的来龙去脉讲清楚，让你知道为什么每一步都要那样写。

## 环境说明

x86_64 架构，四级页表（PML4 -> PDPT -> PD -> PT），4KB 页大小。

内核运行在 QEMU 中，cooperative scheduler（非抢占式），任务在显式 yield 或阻塞时切换。

这个 tag 新增了几个关键文件：
- `kernel/proc/process.cpp`——fork()、handle_cow_fault()、copy_page_table_level()
- `kernel/arch/x86_64/paging_config.hpp`——FLAG_COW 定义（bit 9）
- `kernel/syscall/sys_fork.hpp/cpp`——sys_fork 系统调用

## fork 的十步流程

fork 的实现可以拆成 10 个步骤，每一步都有明确的分配和初始化目标。

### 步骤 1-3：分配 PID 和 TCB

第一步，分配 PID。通过 `pid_alloc.alloc()` 从 PidAllocator 获取一个空闲的 PID（1 到 256 范围内）。如果返回 0（PID_NONE），说明池子耗尽了，直接返回 -1。

第二步，用 aligned new 在内核堆上分配一个新的 Task 结构体。如果分配失败，要记得回滚第一步（释放 PID）。这种"失败即回滚"的模式贯穿了整个 fork 实现。

第三步，memcpy 整个父 TCB 到子 TCB。这一步让子进程成为父进程的精确副本，包括 CPU 上下文（ctx 字段）、地址空间指针、栈指针等所有字段。到这里两个 Task 在内存中的内容完全一致。

### 步骤 4：修复子进程特有字段

第四步修复子进程特有的字段：
- 新 TID（通过全局原子计数器 next_tid 递增）
- 新 PID（从第一步获取的 child_pid）
- ppid 设为父 PID
- state 设为 Ready（等待调度）
- parent 指向父进程
- children 清空为 nullptr
- exit_status 清零

这些字段标识了子进程的身份，必须覆盖从父进程复制来的值。注意 tid 和 pid 是两个独立概念——tid 是全局唯一的任务标识，pid 是进程标识。

memcpy 带来了一个隐含问题：父进程的所有指针都被原样复制了，所以后续步骤中需要小心处理那些"必须独立"的资源（比如栈和地址空间）。

### 步骤 5：内核栈分配

第五步分配内核栈。每个任务需要独立的内核栈，大小是 4 页（16KB）。

通过 `g_pmm.alloc_pages(STACK_PAGES)` 分配连续物理页，然后逐页映射到内核虚拟地址空间。栈底写入 STACK_MAGIC（0xDEADC0DE）用于运行时检测栈溢出——如果这个值变了，说明栈被踩坏了，这是一个比静默踩坏内存好得多的调试体验。

## 栈帧复制与重定位

第六步和第七步是 fork 最精妙的部分。

fork 需要把父进程当前栈上的内容复制到子进程的新栈上，使得子进程被调度时栈帧看起来和父进程一样。

实现方式很直接：先计算父栈的已使用量 `parent_stack_used = parent->kernel_stack_top - parent->ctx.rsp`，然后将等量的内容从父栈的 ctx.rsp 位置复制到子栈的对应位置。

子栈的目标位置是从栈顶往下数 parent_stack_used 个字节的地方：`child_stack_virt + stack_size - parent_stack_used`。子进程的 ctx.rsp 也被设为这个位置，kernel_stack 和 kernel_stack_top 对应更新。

这种设计的好处是简单可靠——不依赖编译器生成的帧指针结构，不需要 inline assembly 读取 RBP/RSP，直接用 TCB 中已经保存的 ctx.rsp 来计算。配合 trampoline 机制，子进程首次调度时从 fork_child_trampoline 开始执行，rax 清零后 ret 到子栈上的对应位置。

## CoW 页表——共享但不牺牲隔离

fork 的页表复制是内存开销最大的操作。如果把父进程的所有物理页都复制一份，fork 一个进程的内存使用量就翻倍。

Copy-On-Write（COW）策略解决这个问题：fork 时只复制页表结构，物理页共享，但标记为只读。任何一方写入时触发 page fault，handler 分配新页并复制内容。

### 递归页表复制

具体实现是一个递归函数 `copy_page_table_level`，从 PML4 的前 256 项（用户空间）开始，逐级向下遍历。

中间级别（PDPT、PD）分配新的页表页并递归。叶子级别（PT）共享物理页——直接把父进程的 PTE raw 值复制给子进程。

对可写的 PT entry，父子两个 PTE 都被修改：清除 FLAG_WRITABLE、设置 FLAG_COW（bit 9，Intel SDM 明确标注为 "Available for software use"）。

### 为什么要标记父进程的 PTE

这里有个关键点：为什么要把父进程的 PTE 也改为只读？

因为两个 PTE 指向同一个物理页，如果只改子进程的，父进程还能写这个页。当父进程写入后，子进程的 CoW handler 不知道父进程已经改了内容——它复制的是"被修改过的页"，但不知道还有另一个进程在共享。

把父子都标记为只读，确保任何一方的第一次写入都会触发 CoW 复制，各自持有独立的物理页副本。

### 与 Linux 的对比

这和 Linux 的 fork 方案类似，但 Linux 更精细——它使用 page reference counting（get_page/put_page），每个物理页有一个引用计数。fork 时 increment refcount 而不是标记 PTE。

写时检查 refcount > 1 才复制，refcount == 1 直接恢复可写（不需要复制）。Cinux 用"双写触发"策略（父子都标记 CoW，谁先写谁复制），更简单但没有"提前解 CoW"的优化。

xv6 的 COW 实现更原始——xv6-riscv 在 uvmcopy 中直接复制所有物理页（不共享），因为 RISC-V 的 PTE 可用位只有 2 个（RSW），而且 xv6 的内存通常很小（128MB），全复制的开销可接受。

## fork_child_trampoline——两条指令的魔法

子进程被调度器首次调度时，context_switch 恢复它的 ctx。ctx.rip 指向 fork_child_trampoline 的地址。

这个 trampoline 只有两条汇编指令：`xorq %rax, %rax; ret`。xor 把 rax 清零（fork 在子进程中返回 0），ret 弹出 ctx.rsp 指向的地址到 RIP——那个地址就是子栈上复制的返回地址位置。

这个设计的精妙之处在于：子进程从不"调用" fork——它是被调度器从某个点"恢复"的。trampoline 只是设置正确的返回值然后跳转回去。两条指令完成了"子进程从 fork 返回 0"这个看似不可能的任务。

## handle_cow_fault——写时复制的核心

当任何一方写入 CoW 页时，CPU 触发 #PF（因为页是只读的）。

page fault handler 调用 handle_cow_fault(fault_vaddr)，它通过四级页表遍历（get_pte 函数）找到对应的 PTE，检查 FLAG_COW 是否设置。

如果设置了，执行四个步骤：
1. 分配新的物理页
2. 从共享页逐字节复制内容（通过 KERNEL_VMA 高半区映射访问）
3. 更新 PTE 指向新页并恢复 FLAG_WRITABLE，清除 FLAG_COW
4. flush TLB

整条链路从 page fault 到恢复执行，不需要任何进程参与——完全由内核透明处理。

## 子进程链表与调度器注册

第九步，子进程通过 wait_next 指针链入父进程的 children 单链表（头插法：`child->wait_next = parent->children; parent->children = child;`）。这个链表在 waitpid 时被遍历，用于查找特定 PID 的子进程或者找第一个 Zombie 状态的子进程。

第十步，通过 `Scheduler::add_task(child)` 加入调度器的运行队列。fork 返回 child_pid 给父进程。子进程不会从这个函数返回——它被调度时从 trampoline 开始执行，fork 在子进程中返回 0。

## 收尾

到这里 fork 的完整实现已经讲清楚了。下一篇我们看 execve 和 waitpid——前者用新的 ELF 程序替换进程的内存映像，后者回收退出的子进程防止 zombie 堆积。三件套（fork + execve + waitpid）构成了完整的进程生命周期。

## 参考资料
- Intel SDM Vol.3A Section 4.3 "64-Bit Mode" — 四级页表结构
- Intel SDM Vol.3A Section 4.7 "Page-Table Entries" — bit 9 Available for OS
- OSDev Wiki Copy-on-write: https://wiki.osdev.org/Paging
- Linux fork (copy_process): https://github.com/torvalds/linux/blob/master/kernel/fork.c
- xv6 vm.c (uvmcopy): https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c
