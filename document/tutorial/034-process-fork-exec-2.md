# fork 的艺术——COW 页表与双返回

> 标签：fork, Copy-On-Write, page table, context switch, x86-64
> 前置：tag 034-1 ELF 格式解析与 PID 分配器

## 前言

上一篇我们准备好了 ELF 解析器和 PID 分配器，现在是时候实现 fork 了。fork 是 UNIX 系统最独特的系统调用——一次调用，两次返回。父进程拿到子进程的 PID，子进程拿到 0。这个看似魔法的语义背后，是对进程状态（TCB、页表、栈帧）极其精确的复制和重定位。

说实话，fork 是这两个 tag 里踩坑最多的部分——帧指针陷阱、GS MSR 状态丢失、huge page 过滤……每一个都是"不修就一定炸"的级别。我们这一篇不只讲实现，还会把这些坑的来龙去脉讲清楚，让你知道为什么每一步都要那样写。

## 环境说明

x86_64 架构，四级页表（PML4 -> PDPT -> PD -> PT），4KB 页大小。内核运行在 QEMU 中， cooperative scheduler（非抢占式），任务在显式 yield 或阻塞时切换。

## fork 的十步流程

fork 的实现可以拆成 10 个步骤，每一步都有明确的分配和初始化目标。

第一步，分配 PID。第二步，分配新 TCB。第三步，memcpy 整个父 TCB 到子 TCB。到这里子进程是父进程的精确副本——包括 CPU 上下文（ctx 字段）、地址空间指针、栈指针等等。

第四步修复子进程特有的字段：新 TID、新 PID（从 PidAllocator 分配的）、ppid 设为父 PID、state 设为 Ready（等待调度）、parent 指向父进程、children 清空、exit_status 清零。这些字段标识了子进程的身份，必须覆盖从父进程复制来的值。

第五步分配内核栈。每个任务需要独立的内核栈，大小是 4 页（16KB）。关键点是保留一个 guard page——栈下面的一页故意不映射，栈溢出时写入 guard page 触发 #PF。这是比静默踩坏内存好得多的调试体验。

第六步和第七步是最精妙的部分——栈帧复制和重定位。

## 帧指针陷阱——Release 模式下的暗坑

fork 需要知道"当前函数的返回地址在哪里"——因为子进程被调度器首次调度时，它要从 fork 的调用点"返回"。在 Debug 模式下，每个函数都有标准的帧指针（push rbp; mov rbp, rsp），此时 [rbp+8] 就是返回地址。但 Release 模式（-O2）默认开启 -fomit-frame-pointer，RBP 变成通用寄存器，[rbp+8] 不再是返回地址——可能是内存中某个随机位置。

这个坑在 035 的实际调试中暴露出来了。子进程被调度后 Double Fault，异常帧中 RSP 是一个用户空间地址（0x7FFFF947F008）。原因就是 RBP 不是帧指针，ctx.rsp 的计算公式 (current_rbp + 8) - current_rsp + child_stack_start 产出了一个垃圾值。

修复方案是在 fork 函数上加两个编译器属性：`__attribute__((optimize("no-omit-frame-pointer"), noinline))`。前者强制这个函数保留帧指针，后者防止内联（内联后函数边界消失，帧指针语义也变了）。

和 xv6 对比：xv6 运行在 RISC-V 上，swtch.S 直接保存 callee-saved 寄存器（包括 s0/fp），不存在帧指针省略问题。但 x86_64 的 -fomit-frame-pointer 是 GCC 的默认行为（在 -O2 下），任何需要通过帧指针定位栈帧的代码都必须显式保护。

## CoW 页表——共享但不牺牲隔离

fork 的页表复制是内存开销最大的操作。如果把父进程的所有物理页都复制一份，fork 一个进程的内存使用量就翻倍。Copy-On-Write（COW）策略解决这个问题：fork 时只复制页表结构，物理页共享，但标记为只读。任何一方写入时触发 page fault，handler 分配新页并复制内容。

具体实现是一个递归函数 `copy_page_table_level`，从 PML4 的前 256 项（用户空间）开始，逐级向下遍历。中间级别（PDPT、PD）分配新的页表页并递归。叶子级别（PT）共享物理页——但有两个关键过滤：没有 FLAG_USER 的条目（内核恒等映射）被跳过，子进程通过共享的 PML4[256..511]（高半区）访问内核资源；huge page 条目直接共享，不走 CoW。

对可写的 PT entry，父子两个 PTE 都被修改：清除 FLAG_WRITABLE、设置 FLAG_COW（bit 9，Intel SDM 明确标注为 "Available for software use"）。这样无论哪方先写入，都会触发 #PF。handler 检查 FLAG_COW，分配新物理页，逐字节复制内容（通过 KERNEL_VMA 高半区映射访问），更新 PTE 指向新页并恢复可写。

这和 Linux 的 fork 方案类似，但 Linux 更精细——它使用 page reference counting（get_page/put_page），每个物理页有一个引用计数。fork 时 increment refcount 而不是标记 PTE。写时检查 refcount > 1 才复制，refcount == 1 直接恢复可写（不需要复制）。Cinux 用"双写触发"策略（父子都标记 CoW，谁先写谁复制），更简单但没有"提前解 CoW"的优化——即使只剩一个进程引用某页，它也保持只读直到第一次写入。

xv6 的 COW 实现更原始——xv6-riscv 在 uvmcopy 中直接复制所有物理页（不共享），因为 RISC-V 的 PTE 可用位只有 2 个（RSW），而且 xv6 的内存通常很小（128MB），全复制的开销可接受。

## fork_child_trampoline——两条指令的魔法

子进程被调度器首次调度时，context_switch 恢复它的 ctx。ctx.rip 指向 fork_child_trampoline 的地址。这个 trampoline 只有两条汇编指令：`xorq %rax, %rax; ret`。xor 把 rax 清零（fork 在子进程中返回 0），ret 弹出 ctx.rsp 指向的地址到 RIP——那个地址就是 fork 的返回地址在子栈上的位置。

这个设计的精妙之处在于：子进程从不"调用" fork——它是被调度器从 fork 的调用点"恢复"的。trampoline 只是设置正确的返回值然后返回到调用点。

## context_switch 的 GS MSR 扩展

在 035 中我们发现了一个隐蔽的 bug：shell 子进程 execve 成功进入用户态，但执行第一条 syscall 时崩溃——demand-page at 0x0，Double Fault，RSP=0。根因是调度器不保存/恢复 GS MSR。

syscall 使用 swapgs 交换 MSR_GS_BASE 和 MSR_KERNEL_GS_BASE。正常流程是：用户态 syscall -> swapgs -> 内核态通过 gs:0 读取内核栈指针。但如果任务在 swapgs 对之间被切出（比如 syscall 阻塞），调度器必须保存当前的 GS 状态。否则下一个被调度进来的任务会继承错误的 GS——swapgs 后 MSR_GS_BASE 可能变成 0，gs:0 读到的就是地址 0。

修复方案：CpuContext 扩展 gs_base 和 kgs_base 字段（偏移 64 和 72，总大小从 64 变成 80 字节），context_switch.S 中通过 rdmsr/wrmsr 保存和恢复。fork() 和 TaskBuilder::build() 中初始化为"未交换"状态（gs_base=0, kgs_base=per-CPU 数据页地址）。

Linux 在 __switch_to 中不保存 GS MSR——因为 Linux 使用 per-CPU 变量（DEFINE_PER_CPU），内核编译时就知道每个 CPU 的 per-CPU 数据地址，不需要在 context switch 时动态切换。Cinux 的 cooperative scheduler 是单核的，per-CPU 数据页只有一个，但 swapgs 的状态是全局的，必须在 context switch 时保存。

## 收尾

到这里 fork 的完整实现已经讲清楚了。下一篇我们看 execve 和 waitpid——前者用新的 ELF 程序替换进程的内存映像，后者回收退出的子进程防止 zombie 堆积。

## 参考资料
- Intel SDM Vol.3A Section 4.3 "64-Bit Mode" — 四级页表结构
- Intel SDM Vol.3A Section 4.7 "Page-Table Entries" — bit 9 Available for OS
- Intel SDM Vol.3A Section 5.8.4 "SWAPGS Instruction"
- OSDev Wiki Copy-on-write: https://wiki.osdev.org/Copy-on-write
- Linux fork (copy_process): https://github.com/torvalds/linux/blob/master/kernel/fork.c
- xv6 vm.c (uvmcopy): https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c
