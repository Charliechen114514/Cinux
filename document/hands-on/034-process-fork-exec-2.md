# fork 系统调用——COW 页表与进程复制

## 导语

上一篇我们把 ELF 解析和 PID 分配的地基打好了，现在是时候啃最硬的一块骨头——fork()。fork 是 UNIX 系统最独特的系统调用：一次调用，两次返回。父进程得到子进程的 PID，子进程得到 0。两个进程从此分道扬镳，各自运行。

实现 fork 的核心挑战是页表复制——如果把父进程的所有物理页都复制一份，fork 一个进程的内存开销就翻倍了，而且大部分页面子进程根本不会修改。所以我们要用 Copy-On-Write（COW）策略：fork 时只复制页表结构，共享物理页，标记为只读。等某个进程真的要写入时，再触发 page fault 复制那一页。

完成本篇后，内核将具备创建子进程的能力——虽然子进程还只能跑和父进程一样的代码（execve 留到下一篇），但 fork 的基础已经完全就位。

## 概念精讲

### Copy-On-Write 页表

x86_64 的四级页表（PML4 -> PDPT -> PD -> PT）中，每个 PT entry 的最低 12 位是标志位。其中 bit 0 是 Present，bit 1 是 Read/Write，bit 2 是 User/Supervisor，bit 9 是 Available（供 OS 自由使用）。Cinux 把 bit 9 定义为 FLAG_COW——当这个位被设置且 FLAG_WRITABLE 被清除时，表示这是一个 CoW 共享页。

fork 的页表复制策略是：遍历 PML4 的前 256 项（用户空间部分），为每一级分配新的页表页，但在最底层的 PT 级别，不分配新的数据页——直接让子进程的 PTE 指向父进程的同一个物理页。然后，如果这个页原来是可写的，就把父子两个 PTE 都改为只读 + FLAG_COW。等任何一方写入时，CPU 触发 #PF（因为页是只读的），handler 检查 FLAG_COW，分配新页，复制内容，恢复可写，清除 FLAG_COW。

### 栈帧重定位

Cinux 的 fork() 使用 ctx.rsp 来计算父栈的已使用量，而不是通过帧指针（RBP）来定位栈帧。这种方式更简单直接：parent_stack_used = kernel_stack_top - ctx.rsp 就是父栈从当前位置到栈顶的总使用量。子栈的 ctx.rsp 重定位到 child_stack_virt + stack_size - parent_stack_used。配合 fork_child_trampoline，子进程首次调度时从 trampoline 开始执行，rax 清零后 ret 到子栈上的返回地址。

如果你后续需要更精确的栈帧复制（只复制 fork 相关的栈帧而非整个已用栈），就需要通过帧指针定位返回地址——这时要注意 Release 模式下 -fomit-frame-pointer 会破坏帧指针语义。

## 动手实现

### Step 1: TCB 扩展

**目标**: 在 Task 结构体中添加进程管理所需的字段。

**设计思路**: UNIX 进程模型中每个进程需要知道自己的 PID、父进程的 PID、退出状态码、以及自己的子进程列表。我们用单链表管理子进程——每个 Task 有一个 children 指针指向第一个子进程，子进程之间通过 wait_next 指针串联。另外需要在 TaskState 中新增 Zombie 状态——进程已退出但父进程还没调用 waitpid 回收。

**实现约束**: Task 新增 int pid（进程 ID，由 PidAllocator 分配）、int ppid（父进程 ID）、int exit_status（退出状态码，Zombie 时有效）、Task* children（子进程链表头指针）、Task* parent（父任务指针）。TaskState 枚举新增 Zombie 值（在 Blocked 和 Dead 之间）。所有新字段默认初始化为 0/nullptr。

### Step 2: fork() 主体流程

**目标**: 实现 fork() 函数，创建当前进程的完整副本。

**设计思路**: fork 的流程可以拆解为 10 步——每一步都有明确的分配和初始化目标。先分配 PID 和 TCB，再复制父进程状态，然后分配新栈，接着处理 CoW 页表，最后链入子进程列表和调度器。任何一步失败都要回滚之前已分配的资源（PID、TCB、栈页、地址空间）。

**实现约束**: 第一步通过 pid_alloc.alloc() 分配 PID。第二步用 aligned new 分配 Task 结构体。第三步 memcpy 整个父 TCB 到子 TCB。第四步修复子进程特有字段：新 TID、新 PID、ppid=父PID、state=Ready、parent=父、children=nullptr、exit_status=0。第五步分配 STACK_PAGES(4) 个物理页作为内核栈，映射到内核虚拟地址空间（在栈底写入 STACK_MAGIC 用于溢出检测）。第六步复制父进程栈内容到子进程栈——计算父栈的已使用量（kernel_stack_top - ctx.rsp），然后将等量的内容从父栈复制到子栈的对应位置。第七步设置子进程的 ctx.rsp 和 kernel_stack_top，使子进程的栈指针正确指向复制后的栈帧位置，ctx.rip 指向 fork_child_trampoline。第八步如果有地址空间就做 CoW 页表复制。第九步链入父进程的 children 列表。第十步加入调度器。

**踩坑预警**: 栈复制要用父栈的"已使用量"（kernel_stack_top - ctx.rsp）来决定复制多少字节，而不是复制整个栈。ctx.rsp 在子进程中必须指向子栈上对应的位置——计算方式是 child_stack_virt + stack_size - parent_stack_used。

**验证**: 在 fork 返回后打印日志："[PROC] fork: created child pid=X tid=... parent_pid=Z"。在 QEMU 中应该看到这条日志，然后子进程被调度器调度后开始执行。

### Step 3: CoW 页表复制

**目标**: 实现递归页表复制，将可写页标记为 CoW。

**设计思路**: 从 PML4 开始，逐级向下遍历页表。遇到非 present 的条目跳过。中间级别（PDPT、PD）分配新的页表页并递归。叶子级别（PT）共享物理页，如果原来是可写的就把父子两个 PTE 都标记为只读 + FLAG_COW。这里有一个重要的过滤：只复制有 FLAG_USER 标志的条目——没有 FLAG_USER 的是内核恒等映射（RAM、MMIO），子进程通过共享的高半区 PML4[256..511] 访问这些资源。

**实现约束**: 写一个递归函数，接收源页表物理地址、目标页表物理地址、当前级别（3=PDPT, 2=PD, 1=PT）。中间级别：alloc_page 分配新页表页，清零，设置目标 entry 的物理地址和标志位，递归处理下一级。叶子级别：直接复制源 entry 的 raw 值（共享物理页），如果 FLAG_WRITABLE 被设置，则清除父子双方的 FLAG_WRITABLE，设置 FLAG_COW。注意：需要通过 KERNEL_VMA（0xFFFFFFFF80000000）将物理地址转换为可访问的虚拟地址。

**踩坑预警**: 在修改父进程的 PTE 时（标记 CoW），要同时修改子进程的——因为两个 PTE 原来指向同一个物理页，如果只改子进程的，父进程还能写这个页，但子进程的 CoW 处理会认为只有子进程在共享——这会导致页被错误释放。另外 huge page（FLAG_HUGE）的条目不应该走 CoW 逻辑，应该直接共享。

**验证**: fork 后，在串口输出中不应该看到立即的 page fault。只有当子进程（或父进程）第一次写入共享页时，才会触发 CoW fault 并在串口打印 "[COW] resolved fault at vaddr=..."。

### Step 4: fork_child_trampoline

**目标**: 编写一小段汇编，使子进程从 fork() 返回 0。

**设计思路**: 子进程被调度器首次调度时，context_switch 会恢复子进程的 ctx.rip——这个值被设置为 trampoline 的地址。trampoline 做的事很简单：把 rax 清零（fork 在子进程中返回 0），然后 ret。ret 会弹出栈顶的值到 RIP——栈顶存的是 fork 的返回地址（在子进程栈上重定位后的位置）。这样子进程就从 fork 调用点"返回"了，返回值是 0。

**实现约束**: 纯汇编函数，两条指令：xorq %rax, %rax; ret。声明为 global function，用 .size 指令标注大小。在 process.hpp 中声明 extern "C" void fork_child_trampoline()。

**验证**: 子进程被调度后，如果一切正确，fork 返回 0，子进程进入 child path 的代码分支（比如 create_shell_terminal 中 child_pid==0 的分支）。

## 构建与运行

```bash
cmake --build build
make run
```

如果跑内核测试：
```bash
make run-kernel-test
```

## 调试技巧

1. **子进程 Double Fault + RSP 在用户空间**: 检查 ctx.rsp 的计算是否正确。如果 RSP 指向一个异常地址（比如用户空间地址），说明 child->ctx.rsp 的重定位计算有误——确认 parent_stack_used 和 child_stack_start 的计算逻辑。

2. **CoW 不触发**: 如果 fork 后修改全局变量没有触发 page fault，检查 PTE 是否正确设置了 FLAG_COW 和清除了 FLAG_WRITABLE。可以用 QEMU monitor 的 `info mem` 命令查看页表。

3. **Huge page 复制导致崩溃**: 如果 fork 后子进程访问某些地址时 #PF 显示 huge page 标志，说明你的 CoW 逻辑没有跳过 huge page。在 copy_page_table_level 中检查 FLAG_HUGE 的条目是否直接共享而不走 CoW。

## 本章小结

| 组件 | 关键设计 | 要点 |
|------|----------|------|
| TCB 扩展 | pid/ppid/children/parent | 单链表管理子进程 |
| fork() | memcpy TCB + 新栈 + CoW 页表 | ctx.rsp 重定位栈帧 |
| CoW 页表 | 共享物理页 + FLAG_COW | 父子双方都标记只读 |
| fork_child_trampoline | xor rax,rax; ret | 子进程返回 0 |
