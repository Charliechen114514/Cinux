---
title: 035-multi-terminal-1 · 多终端
---

# COW 页错误处理——fork 的物理引擎

> 标签：page fault, Copy-On-Write, swapgs, MSR, x86-64
> 前置：tag 034 fork 的艺术

## 前言

上一篇（034）我们实现了 fork 的 CoW 页表复制逻辑——子进程和父进程共享物理页，标记为只读 + FLAG_COW。但当时我们还没有实现真正的 page fault handler 来处理写时复制。也就是说，fork 出来的子进程一旦写入共享页，就会因为"写只读页"而 crash。这一篇我们要做的第一件事就是在 #PF handler 中识别并处理 COW fault。

但事情到这里还没完。在打通 fork + execve 的完整链路时，我们发现了一个更深层的 bug：syscall 使用的 GS 段寄存器（swapgs 指令依赖的 MSR_GS_BASE / MSR_KERNEL_GS_BASE）在 context switch 时没有被保存/恢复。这个 bug 只在多任务调度 + fork 场景下暴露——单任务时一切正常，加上 fork 后子进程进用户态执行 syscall 就崩溃。真正的坑在后面，而且隐蔽到令人发指。

## 环境说明

QEMU x86_64，cooperative scheduler，单核。内核中断在 Interrupt gate 下运行（IF=0），PIT tick 频率 100Hz。物理内存管理通过 PMM 的 alloc_page 分配，虚拟内存通过 VMM 的 map 映射。页表使用 4 级结构（PML4 -> PDPT -> PD -> PT），所有物理页通过 KERNEL_VMA（0xFFFFFFFF80000000）高半区映射访问。fork 在 tag 034 中实现了页表复制逻辑（copy_page_table_level），本篇补全的是 COW fault 的处理和 GS MSR 的保存/恢复。

## #PF Error Code——读懂 CPU 告诉你的信息

x86_64 的 #PF 异常会在栈上推入一个 error code，三个关键位决定了 fault 的性质。bit 0（P）为 0 表示页不存在（demand-paging 场景），为 1 表示权限违反。bit 1（W/R）为 0 表示读操作触发的 fault，为 1 表示写操作。bit 2（U/S）为 0 表示内核态触发，为 1 表示用户态触发。这三个位组合起来能区分出大部分 fault 类型：demand-paging（P=0）、CoW（P=1,W/R=1,U/S=1）、普通权限违反（P=1, 其他组合）。

COW fault 的特征非常明确：P=1（页存在）、W/R=1（写操作触发）、U/S=1（用户态）。这个组合和普通 demand-paging（P=0）完全不同，我们的 #PF handler 可以轻松区分两种情况。判断条件就是三个位同时置位：`(err & 0x01) && (err & 0x02) && (err & 0x04)`。

处理顺序设计为先 demand-paging（P=0），再 COW（P=1 && W/R=1 && U/S=1），最后才报告 fatal error。这个顺序很重要——demand-paging 比 COW 更常见（第一次访问任何页都会触发），先检查它可以减少 COW 检查的无效执行。在实际运行中，一个刚 fork 出来的子进程如果只是读取共享页，根本不会触发 COW——只有写入时才需要处理。

这里还有一个关于锁的关键设计。#PF handler 在 Interrupt gate 下运行（IF=0），意味着同 CPU 不可能有并发访问。所以 demand-paging 必须使用无锁路径：PMM 的 alloc_page_locked 和 VMM 的 map_nolock。如果在 IF=0 下拿自旋锁，一旦触发递归 #PF（比如分配页表时缺页），就会死锁。这个坑看似简单，但很容易忽略——因为 VMM 的自旋锁内部会 sti，在 IF=0 下调用会立刻出事。另外 demand-paging 的 map_flags 需要根据 fault 地址是否在用户空间来决定是否加 FLAG_USER，用 is_user_vaddr() 判断。

## handle_cow_fault——四步完成写时复制

handle_cow_fault 接收触发 fault 的虚拟地址。首先通过 get_pte 做四级页表 walk 找到对应的 PT entry，然后做四重检查：PTE 存在（walk 成功）、页 present（不是 demand-paging）、不可写（已经被 fork 标记为只读）、有 FLAG_COW 标记（区分 CoW 和普通的权限违反）。任何一关不通过就返回 false，让调用者继续尝试其他处理路径。

get_pte 是一个内部辅助函数，从 PML4 开始逐级 walk 页表。从 PML4 到 PT 逐级查找，任何一级不存在就返回 nullptr。所有物理地址都通过 phys_to_virt（phys + KERNEL_VMA）转换为可访问的虚拟地址。索引提取使用 paging_config.hpp 中的移位和掩码宏（PML4_INDEX、PDPT_INDEX、PD_INDEX、PT_INDEX）。

全部通过后执行四步操作。第一步，调用 g_pmm.alloc_page() 分配新物理页。第二步，从旧页逐字节复制内容到新页——通过 KERNEL_VMA 高半区映射访问物理页（old_phys + KERNEL_VMA 和 new_phys + KERNEL_VMA），因为不能直接用物理地址。第三步，更新 PTE：调用 set_phys_addr(new_phys) 指向新物理页、`pte->raw |= FLAG_WRITABLE` 恢复可写、`pte->raw &= ~FLAG_COW` 清除 CoW 标记。第四步，调用 flush_tlb（执行 invlpg 指令）刷新该页的 TLB 条目——确保 CPU 不再缓存旧的只读映射。

这里有个设计取舍值得讨论。Cinux 用"逐字节复制"（for 循环 PAGE_SIZE 次），而不是 memcpy 或 SIMD。原因是内核没有链接标准库，std::memcpy 的实现可能不够安全（特别是在 IF=0 的中断上下文中）。逐字节复制最保守最可靠，性能差异在 4KB 页上可以忽略——毕竟 COW fault 每次只处理一页，不是热路径上的批量操作。

flush_tlb 的参数必须是页对齐的虚拟地址，所以要把 fault_vaddr 的低位清零：`fault_vaddr & ~(PAGE_SIZE - 1)`。如果 TLB 中还缓存着旧的只读映射，下次写入还是触发 #PF——这个 bug 看起来像是"COW 处理后仍然 #PF"，实际上就是 TLB 没刷新。

和 Linux 对比：Linux 的 CoW 实现用 copy_user_page（最终调用 memcpy），并且做了引用计数优化——每个物理页有一个 refcount，fork 时 increment。写时检查 refcount > 1 才复制，refcount == 1 直接恢复可写（CoW 提前解除）。Cinux 的"双写触发"策略（父子都标记 CoW，谁先写谁复制）更简单但不做提前优化。xv6 不做 COW——它直接复制所有物理页（uvmcopy），这对 xv6 的小内存模型来说足够了，但不符合 Linux/UNIX 的 fork 语义（fork 应该是 O(1) 的地址空间操作，而不是 O(n) 的全量复制）。

## GS MSR 保存——那个差点让我血压拉满的 bug

现在说那个隐蔽的 bug。shell 子进程 execve 成功进入用户态，但执行第一条 syscall 时崩溃：demand-page at 0x0，Double Fault，RSP=0。看到 RSP=0 的那一刻，笔者的血压直接拉满。

根因分析：syscall 使用 swapgs 交换 MSR_GS_BASE 和 MSR_KERNEL_GS_BASE。正常流程是：用户态 MSR_GS_BASE=0, MSR_KERNEL_GS_BASE=per-CPU 数据页 -> syscall -> swapgs -> MSR_GS_BASE=per-CPU 数据页, MSR_KERNEL_GS_BASE=0 -> movq gs:0 加载到 RSP。但 cooperative scheduler 可能在 swapgs 对之间切换任务（比如父进程 syscall 中阻塞等待 pipe），此时 GS MSR 处于"已交换"状态。

崩溃链条是这样的：父进程 syscall 阻塞时 GS 已 swapgs -> 调度器切换到 gui_worker（不保存 GS）-> gui_worker 调 fork -> 子进程 memcpy 继承父进程的已交换 GS 状态 -> 子进程进用户态 -> syscall -> swapgs 后 MSR_GS_BASE=0 -> gs:0 读地址 0 -> RSP=0 -> 下一次 push 崩溃。整个链条环环相扣，任何一步断了 bug 就不出现。

如果你在 Double Fault 帧中看到 RSP=0，首先检查 context_switch 是否正确保存/恢复 GS MSR。

修复方案：CpuContext 扩展 gs_base 和 kgs_base 字段（偏移 64 和 72，总大小从 64 变成 80 字节）。context_switch.S 中在保存 callee-saved 寄存器后，通过 rdmsr 读取 MSR_GS_BASE（0xC0000101）和 MSR_KERNEL_GS_BASE（0xC0000102），存入 from 的对应字段。恢复时从 to 的字段读出并用 wrmsr 写回。MSR_GS_BASE 和 MSR_KERNEL_GS_BASE 是 x86_64 的 64 位 MSR，rdmsr 把 64 位值返回在 EDX:EAX（各 32 位），所以用 movl 分别存到 CpuContext 的 gs_base 和 kgs_base 字段的低 32 位和高 32 位。

fork() 和 TaskBuilder::build() 中初始化子进程/新任务的 GS MSR 为"未交换"状态——gs_base=0（用户态默认值），kgs_base=g_per_cpu.gs_page_vaddr（per-CPU 数据页地址）。这样子进程第一次进用户态后执行 syscall 时，swapgs 会正确地把 MSR_GS_BASE 设为 per-CPU 数据页地址。

Linux 不需要在 context switch 中保存 GS MSR——因为它使用 per-CPU 变量（通过 GS 段在编译时确定偏移），每个 CPU 的 per-CPU 基地址在启动时设置，之后不变。swapgs 只在 syscall entry/exit 时使用，内核代码从不直接 swapgs。Cinux 的问题在于 cooperative scheduler 允许在 syscall 路径中间切换任务（阻塞时），这打破了 swapgs 的配对语义。

## PerCPU 结构与 update_syscall_stack

和 GS MSR 保存配套的改动是 PerCPU 结构体的扩展。PerCPU 是单核内核的全局变量（g_per_cpu），保存当前 CPU 的状态。新增 gs_page_vaddr 保存 per-CPU 数据页的虚拟地址。update_syscall_stack() 方法在每次 context switch 后被调用，把新任务的 kernel_stack_top 写入 gs:0。方法实现很简单：更新 kernel_stack 字段，然后如果 gs_page_vaddr != 0，通过 reinterpret_cast 写入 `*gs_page_vaddr = stack_top`。

g_per_cpu 初始化为 `{nullptr, 0, 0}`，usermode_init 时填充 gs_page_vaddr。调度器的 schedule()、exit_current()、run_first() 三个函数中都添加了 g_per_cpu.update_syscall_stack 调用，确保每次任务切换后 gs:0 指向正确的栈顶。

## 收尾

到这里 COW fault handler 和 GS MSR 保存/恢复都搞定了。fork 的基础设施已经完全就绪——父子进程可以各自独立地修改内存而互不影响，syscall 的 GS 状态也能正确地在任务间切换。下一篇我们把这些能力用到 execve 的实际场景中——从 ext2 加载 Shell 并跳转到用户态。

## 参考资料

- Intel SDM Vol.3A Section 6.15 "Page-Fault Exception (#PF)" — error code 格式
- Intel SDM Vol.3A Section 5.8.4 "SWAPGS Instruction" — swapgs 配对语义
- Intel SDM Vol.3A Section 6.14 "GS Base Registers" — MSR_GS_BASE / MSR_KERNEL_GS_BASE
- OSDev Wiki #PF: https://wiki.osdev.org/Exceptions#Page_Fault
- Linux CoW (do_wp_page): https://github.com/torvalds/linux/blob/master/mm/memory.c
