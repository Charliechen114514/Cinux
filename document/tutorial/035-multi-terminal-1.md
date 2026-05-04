# COW 页错误处理——fork 的物理引擎

> 标签：page fault, Copy-On-Write, swapgs, MSR, x86-64
> 前置：tag 034 fork 的艺术

## 前言

上一篇（034）我们实现了 fork 的 CoW 页表复制逻辑——子进程和父进程共享物理页，标记为只读 + FLAG_COW。但当时我们还没有实现真正的 page fault handler 来处理写时复制。也就是说，fork 出来的子进程一旦写入共享页，就会因为"写只读页"而 crash。这一篇我们要做的第一件事就是在 #PF handler 中识别并处理 COW fault。

但事情到这里还没完。在打通 fork + execve 的完整链路时，我们发现了一个更深层的 bug：syscall 使用的 GS 段寄存器（swapgs 指令依赖的 MSR_GS_BASE / MSR_KERNEL_GS_BASE）在 context switch 时没有被保存/恢复。这个 bug 只在多任务调度 + fork 场景下暴露——单任务时一切正常，加上 fork 后子进程进用户态执行 syscall 就崩溃。真正的坑在后面，而且隐蔽到令人发指。

## 环境说明

QEMU x86_64，cooperative scheduler，单核。内核中断在 Interrupt gate 下运行（IF=0），PIT tick 频率 100Hz。

## #PF Error Code——读懂 CPU 告诉你的信息

x86_64 的 #PF 异常会在栈上推入一个 error code，三个关键位决定了 fault 的性质。bit 0（P）为 0 表示页不存在（demand-paging 场景），为 1 表示权限违反。bit 1（W/R）为 0 表示读操作触发的 fault，为 1 表示写操作。bit 2（U/S）为 0 表示内核态触发，为 1 表示用户态触发。

COW fault 的特征非常明确：P=1（页存在）、W/R=1（写操作触发）、U/S=1（用户态）。这个组合和普通 demand-paging（P=0）完全不同，我们的 #PF handler 可以轻松区分两种情况。

处理顺序设计为先 demand-paging（P=0），再 COW（P=1 && W/R=1 && U/S=1），最后才报告 fatal error。这个顺序很重要——demand-paging 比 COW 更常见（第一次访问任何页都会触发），先检查它可以减少 COW 检查的无效执行。

## handle_cow_fault——四步完成写时复制

handle_cow_fault 接收触发 fault 的虚拟地址。首先通过 get_pte 做四级页表 walk 找到对应的 PT entry，然后做四重检查：PTE 存在、不可写、有 FLAG_COW 标记。全部通过后执行四步操作。

第一步，分配新物理页。第二步，从旧页逐字节复制内容到新页——通过 KERNEL_VMA（0xFFFFFFFF80000000）高半区映射访问物理页，因为不能直接用物理地址。第三步，更新 PTE：指向新物理页、恢复 FLAG_WRITABLE、清除 FLAG_COW。第四步，invlpg 刷新该页的 TLB 条目——确保 CPU 不再缓存旧的只读映射。

这里有个设计取舍值得讨论。Cinux 用"逐字节复制"（for 循环 PAGE_SIZE 次），而不是 memcpy 或 SIMD。原因是内核没有链接标准库，std::memcpy 的实现可能不够安全（特别是在 IF=0 的中断上下文中）。逐字节复制最保守最可靠，性能差异在 4KB 页上可以忽略。

Linux 的 CoW 实现用 copy_user_page（最终调用 memcpy），并且做了引用计数优化——每个物理页有一个 refcount，fork 时 increment。写时检查 refcount > 1 才复制，refcount == 1 直接恢复可写（CoW 提前解除）。Cinux 的"双写触发"策略（父子都标记 CoW，谁先写谁复制）更简单但不做提前优化。

xv6 不做 COW——它直接复制所有物理页（uvmcopy），每个页都分配新的物理内存。这对 xv6 的小内存模型来说足够了，但不符合 Linux/UNIX 的 fork 语义（fork 应该是 O(1) 的地址空间操作，而不是 O(n) 的全量复制）。

## GS MSR 保存——那个差点让我血压拉满的 bug

现在说那个隐蔽的 bug。shell 子进程 execve 成功进入用户态，但执行第一条 syscall 时崩溃：demand-page at 0x0，Double Fault，RSP=0。

根因分析：syscall 使用 swapgs 交换 MSR_GS_BASE 和 MSR_KERNEL_GS_BASE。正常流程是：用户态 MSR_GS_BASE=0, MSR_KERNEL_GS_BASE=per-CPU 数据页 -> syscall -> swapgs -> MSR_GS_BASE=per-CPU 数据页, MSR_KERNEL_GS_BASE=0 -> gs:0 读到内核栈指针。但 cooperative scheduler 可能在 swapgs 对之间切换任务（比如父进程 syscall 中阻塞等待 pipe），此时 GS MSR 处于"已交换"状态。

崩溃链条：父进程 syscall 阻塞时 GS 已 swapgs -> 调度器切换到 gui_worker（不保存 GS）-> gui_worker 调 fork -> 子进程 memcpy 继承父进程的已交换 GS 状态 -> 子进程进用户态 -> syscall -> swapgs 后 MSR_GS_BASE=0 -> gs:0 读地址 0 -> RSP=0 -> 下一次 push 崩溃。

修复方案：CpuContext 扩展 gs_base 和 kgs_base 字段（偏移 64 和 72，总大小 80 字节）。context_switch.S 中通过 rdmsr/wrmsr 保存和恢复。fork() 和 TaskBuilder::build() 中初始化为"未交换"状态。

Linux 不需要在 context switch 中保存 GS MSR——因为它使用 per-CPU 变量（通过 GS 段在编译时确定偏移），每个 CPU 的 per-CPU 基地址在启动时设置，之后不变。swapgs 只在 syscall entry/exit 时使用，内核代码从不直接 swapgs。Cinux 的问题在于 cooperative scheduler 允许在 syscall 路径中间切换任务（阻塞时），这打破了 swapgs 的配对语义。

## 收尾

到这里 COW fault handler 和 GS MSR 保存/恢复都搞定了。fork 的基础设施已经完全就绪——父子进程可以各自独立地修改内存而互不影响，syscall 的 GS 状态也能正确地在任务间切换。下一篇我们把这些能力用到 execve 的实际场景中——从 ext2 加载 Shell 并跳转到用户态。

## 参考资料
- Intel SDM Vol.3A Section 6.15 "Page-Fault Exception (#PF)" — error code 格式
- Intel SDM Vol.3A Section 5.8.4 "SWAPGS Instruction" — swapgs 配对语义
- Intel SDM Vol.3A Section 6.14 "GS Base Registers" — MSR_GS_BASE / MSR_KERNEL_GS_BASE
- OSDev Wiki #PF: https://wiki.osdev.org/Exceptions#Page_Fault
- Linux CoW (do_wp_page): https://github.com/torvalds/linux/blob/master/mm/memory.c
