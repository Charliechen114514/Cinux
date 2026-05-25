---
title: 035-multi-terminal-1 · 多终端
---

# COW 页错误处理与 fork 返回两次

## 导语

上一篇（034）我们实现了 fork 的页表复制逻辑——子进程和父进程共享物理页，标记 FLAG_COW。但那时我们还没有实现真正的 page fault handler 来处理 COW 写时复制。所以 fork 出来的子进程一旦写入共享页就会直接 crash。这一篇我们要做两件事：在 #PF handler 中识别并处理 COW fault，以及确保 fork 的 "返回两次" 语义正确工作——子进程被调度器首次调度时，需要通过 trampoline 正确地从 fork 调用点返回 0。

完成本篇后，fork 将真正可用——父子进程可以各自独立地修改内存而互不影响。

## 概念精讲

### Page Fault Error Code 解读

x86_64 的 #PF 异常会在栈上推入一个 error code，三个关键位决定了 fault 的性质。bit 0（P）= 0 表示页不存在，= 1 表示权限违反。bit 1（W/R）= 0 表示读访问，= 1 表示写访问。bit 2（U/S）= 0 表示内核态访问，= 1 表示用户态访问。

COW fault 的特征是：P=1（页存在）、W/R=1（写操作）、U/S=1（用户态）。这和普通 demand-paging（P=0）完全不同。我们的 #PF handler 需要先尝试 demand-paging（P=0），如果不行再尝试 COW（P=1 && W/R=1 && U/S=1），最后才报告 fatal error。

### COW Page Fault 处理流程

当 #PF handler 确认这是一个 COW fault 后，执行四步操作：分配一个新的物理页，从旧页（通过 KERNEL_VMA 高半区映射访问）逐字节复制内容到新页，更新 PTE 指向新页并恢复 FLAG_WRITABLE、清除 FLAG_COW，最后 invlpg 刷新该页的 TLB 条目。这个流程保证了只有实际被写入的页才会被复制——其他共享页继续共享。

### fork 的返回语义

fork 最神奇的地方在于"一次调用两次返回"。父进程调用 fork 后正常返回子进程 PID（>0）。子进程呢？它不调用 fork——它是被调度器从 fork 的调用点"恢复"的。子进程的 ctx.rip 被设为 fork_child_trampoline 的地址，ctx.rsp 指向 fork 返回地址在子栈上的位置。调度器首次调度子进程时，context_switch 恢复这些寄存器，trampoline 执行 xor rax,rax; ret，子进程就从 fork 调用点"返回"了 0。

## 动手实现

### Step 1: handle_cow_fault() 实现

**目标**: 在 process.cpp 中实现 COW 页错误处理函数。

**设计思路**: 函数接收触发 fault 的虚拟地址。首先获取当前任务和它的地址空间，通过 get_pte() 找到对应的 PT entry。然后做四重检查：PTE 是否 present、是否不可写、是否有 FLAG_COW。全部通过后才执行 COW 处理。

**实现约束**: get_pte() 是一个内部辅助函数，从 PML4 开始逐级 walk 页表，返回指定虚拟地址的 PT entry 指针。handle_cow_fault() 的流程：alloc_page() 分配新页，通过 old_phys + KERNEL_VMA 和 new_phys + KERNEL_VMA 访问源和目标页，逐字节复制（PAGE_SIZE 次），然后 pte->set_phys_addr(new_phys) 更新物理地址，pte->raw |= FLAG_WRITABLE 恢复可写，pte->raw &= ~FLAG_COW 清除 CoW 标记，最后 flush_tlb 刷新该页。

**踩坑预警**: 复制页内容时必须通过高半区映射访问物理页——不能直接用物理地址，因为除了内核恒等映射之外没有其他方式访问物理内存。而且在高半区映射中，物理地址 + KERNEL_VMA 得到的虚拟地址才是可用的。flush_tlb 的参数必须是页对齐的虚拟地址，所以要把 fault_vaddr 的低位清零。

### Step 2: #PF handler 扩展

**目标**: 在 exception_handlers.cpp 的 handle_pf 中集成 COW fault 检测。

**设计思路**: 原来的 handle_pf 只处理 demand-paging（P=0 的情况）。现在在 demand-paging 之后、fatal error 之前，加一个 COW 检测分支。COW 的 error code 特征是 P=1 && W/R=1 && U/S=1，即 (err & 0x01) && (err & 0x02) && (err & 0x04)。如果三个位都置位，调用 handle_cow_fault()，成功则 return。

**实现约束**: COW 检测放在 demand-paging 之后。如果 handle_cow_fault 返回 true，直接 return，不走到 fatal error。另外 demand-paging 的 map_flags 需要根据 fault 地址是否在用户空间来决定是否加 FLAG_USER——用 is_user_vaddr() 判断。

**踩坑预警**: demand-paging 必须使用无锁路径（alloc_page_locked / map_nolock），因为 #PF handler 在 Interrupt gate 下运行（IF=0），如果在这里拿 VMM 或 PMM 的自旋锁，一旦触发递归 #PF 就会死锁。

### Step 3: context_switch 保存/恢复 GS MSR

**目标**: 在 context_switch.S 中保存和恢复 MSR_GS_BASE 与 MSR_KERNEL_GS_BASE。

**设计思路**: syscall 使用 swapgs 指令交换 MSR_GS_BASE 和 MSR_KERNEL_GS_BASE。正常情况下，内核态 MSR_GS_BASE=per-CPU 数据页地址，用户态 MSR_GS_BASE=0。但调度器可能在 swapgs 对之间切换任务（比如 syscall 中阻塞），此时 GS MSR 处于"已交换"状态。如果不保存/恢复，下一个被调度进来的任务会继承错误的 GS 状态。

**实现约束**: CpuContext 扩展两个 uint64_t 字段：gs_base（偏移 64）和 kgs_base（偏移 72），总大小从 64 变成 80 字节。context_switch.S 在保存 callee-saved 寄存器后，用 rdmsr 读取 MSR_GS_BASE (0xC0000101) 和 MSR_KERNEL_GS_BASE (0xC0000102) 到 from 的对应字段。恢复时从 to 的字段读出并用 wrmsr 写回。注意 rdmsr 返回 64 位值在 EDX:EAX 中（各 32 位），wrmsr 也需要把 64 位值拆到 EAX 和 EDX。

**踩坑预警**: 这是 035 中最隐蔽的 bug。症状是 shell 子进程 execve 成功进入用户态，但执行第一条 syscall 时崩溃——demand-page at 0x0, Double Fault, RSP=0。原因是 syscall_entry 中 swapgs 后 MSR_GS_BASE 变成了 0，movq gs:0 加载的 RSP 就是 0。如果你在 Double Fault 帧中看到 RSP=0，首先检查 context_switch 是否正确保存/恢复 GS MSR。

### Step 4: fork() 初始化子进程 GS 状态

**目标**: 在 fork() 和 TaskBuilder::build() 中正确初始化子进程/新任务的 GS MSR。

**设计思路**: 子进程首次被调度时，context_switch 会恢复它的 gs_base 和 kgs_base 到 MSR。子进程应该从"未交换"状态开始——gs_base=0（用户态默认值），kgs_base=per-CPU 数据页地址。这样子进程第一次进用户态后执行 syscall 时，swapgs 会正确地把 MSR_GS_BASE 设为 per-CPU 数据页地址。

**实现约束**: fork() 中在加入调度器之前设置 child->ctx.gs_base = 0 和 child->ctx.kgs_base = g_per_cpu.gs_page_vaddr。TaskBuilder::build() 中同样设置 task->ctx.gs_base = 0 和 task->ctx.kgs_base = g_per_cpu.gs_page_vaddr。

## 构建与运行

```bash
cmake --build build
make run
```

## 调试技巧

1. **COW fault 不触发**: fork 后写入共享页没有触发 #PF。检查 PTE 是否真的被标记为 FLAG_COW + 只读。在 copy_page_table_level 中加日志打印每个被标记 CoW 的 PTE。

2. **子进程 syscall 崩溃 + demand-page at 0x0**: 典型的 GS MSR 状态丢失。检查 context_switch.S 是否有 rdmsr/wrmsr，检查 fork() 和 build() 是否初始化了 gs_base/kgs_base。

3. **COW 处理后仍然 #PF**: 可能是 TLB 没刷新。invlpg 的参数必须是页对齐地址（低位清零）。如果 TLB 中还缓存着旧的只读映射，下次写入还是触发 #PF。

## 本章小结

| 组件 | 关键设计 | 要点 |
|------|----------|------|
| handle_cow_fault | 分配新页 -> 复制 -> 更新 PTE -> flush TLB | 通过 KERNEL_VMA 访问物理页 |
| #PF handler | demand-paging -> COW -> fatal | COW: P=1,W=1,U=1 |
| GS MSR 保存/恢复 | rdmsr/wrmsr in context_switch | CpuContext 扩展到 80 字节 |
| fork GS 初始化 | gs_base=0, kgs_base=per-CPU | "未交换"状态 |
