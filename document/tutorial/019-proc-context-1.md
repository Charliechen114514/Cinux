# 从单任务到多任务：内核线程的诞生

> 标签：上下文切换, TCB, CpuContext, TaskBuilder, callee-saved, x86-64 ABI
> 前置：[018-3 地址空间的最后一公里：从创建到销毁的全生命周期](018-mm-address-space-3.md)

## 前言

到 tag 018 为止，Cinux 已经搭好了完整的内存管理栈——PMM 管物理页、VMM 管虚拟映射、Heap 管动态分配、AddressSpace 管进程隔离。看起来很完备了对吧？但你会发现一个尴尬的事实：整个内核从头到尾只有一条执行流。`kernel_main` 从头跑到尾，中间没有任何"暂停当前任务、去干点别的、再回来"的机制。这对一个玩具内核来说没问题，可一旦你想让两个东西"同时"跑——比如一个处理键盘输入、一个刷新屏幕——单任务就完全不够用了。

这一章我们正式进入多任务的世界。tag 019 的目标很具体：让两个内核线程各自打印 5 轮消息，通过 `yield()` 主动让出 CPU，实现合作式交替运行。听起来简单？说实话，从"知道上下文切换的概念"到"真正写出一个能跑的调度器"之间的距离，远比想象中大。你需要回答一连串问题：切换时保存哪些寄存器？新线程的栈怎么初始化？线程退出了怎么办？这些问题的答案都藏在数据结构的设计里。

在进入代码之前，我们先看看其他操作系统是怎么做这件事的，这能帮我们理解 Cinux 的设计选择背后的"为什么"。

## 经典设计对比：别人家的上下文切换

上下文切换的核心思想其实全世界都一样——保存当前线程的寄存器快照，恢复下一个线程的寄存器快照，然后跳过去执行。区别在于细节。

xv6（MIT 的教学操作系统，32 位 x86）的 `swtch.S` 是最经典的参考。它保存 4 个 callee-saved 寄存器（ebp、ebx、esi、edi）加上隐式保存在栈上的 EIP，然后把 ESP 存到 `*old`、从 `new` 加载 ESP。整个切换过程非常简洁，但有一个特点：xv6 的线程切换必须经过中间的 scheduler 线程——从进程 A 切到进程 B 的路径是 A -> scheduler -> B，而不是 A 直接到 B。这是因为 xv6 给每个 CPU 维护了一个独立的 scheduler 上下文，进程释放 CPU 后先回到 scheduler，由 scheduler 决定下一个运行谁。简单归简单，但每次切换多了一趟"过路"。xv6 的 `struct context` 直接定义了 edi/esi/ebx/ebp/eip 五个字段，存在内核栈的底部——这和 Cinux 把 `CpuContext` 作为 `Task` 结构体的第一个字段是类似的思路，只不过 xv6 用指针间接访问（`proc->context`），而 Cinux 用内嵌（`task.ctx`）。

Linux 的 `__switch_to_asm`（arch/x86/entry/entry_64.S）做的是同一件事，只不过规模大得多。它保存 rbp、rbx、r12-r15 这 6 个 callee-saved 寄存器，把 RSP 存入 `task_struct->thread.sp`，加载下一个 task 的 RSP，弹出寄存器，然后跳到 `__switch_to`（一个 C 函数，负责 FPU、debug 寄存器等附加状态）。Linux 的调度策略体系（CFS、RT、Deadline、Idle）通过 `SchedulingClass` 层次结构组织，每个 CPU 有自己的运行队列 `struct rq`，里面有一个 `curr` 指针指向当前正在跑的 task。这个架构和 Cinux 后面要做的非常相似——Cinux 的 `SchedulingClass` 虚接口和 `Scheduler::current_` 的设计灵感就来源于此。

值得一提的是 Linux 的 `switch_to` 宏有一个著名的"prev 指针谜题"。`switch_to` 接受三个参数：`prev`、`next`、`last`。为什么需要第三个参数？因为 `switch_to` 是在 `context_switch()` 函数内部调用的，当 `next` 线程被切回来继续执行时，它"醒来"的位置还是 `context_switch()` 函数内部——但此时栈上所有局部变量的值都是 `next` 线程自己的，不是 `prev` 线程的。Linux 用 `last` 参数来保存"谁把我唤醒了"这个信息，以便 `context_switch()` 返回后调用者能知道上一个运行的是谁。Cinux 是单 CPU 内核，不存在这个问题——`current_` 始终指向当前线程，不存在"我在另一个 CPU 上醒来"的场景。

PintOS 的 `switch_threads()` 则更加直白：保存 ebx、ebp、esi、edi，把 ESP 存入当前线程的 `thread->stack` 字段（通过一个编译期计算出的固定偏移量 `thread_stack_ofs` 来定位），加载下一个线程的 ESP，恢复寄存器，返回。PintOS 的线程结构体用 C 实现（`struct thread`），但栈偏移量通过一个运行时计算的技巧获取——在 `thread.c` 中定义一个辅助函数，用 `offsetof` 在初始化阶段算出 `stack` 字段在结构体中的偏移，然后汇编代码通过这个偏移量直接访问。这种做法比 xv6 的指针传递更高效（少一次内存间接寻址），但需要在 C 和汇编之间维护一个"魔法偏移量"的约定。PintOS 有一个值得注意的设计——`kernel_thread()` 封装函数。每个新线程不是直接从用户提供的入口函数开始跑，而是从 `kernel_thread()` 开始，由它调用用户函数，函数返回后自动调用 `thread_exit()`。这个"自动退出"的模式后来也启发了 Cinux 的 `exit_current` 栈上地址技巧，我们在第三篇会详细讲。

Intel SDM Vol. 3A 第 8.7 节明确指出："In 64-bit mode, the task switching mechanism available in protected mode is not supported. Task management and switching must be performed by software." 意思是 64 位模式下硬件任务切换（通过 JMP/CALL 到 TSS 描述符）不再可用，处理器会对这种操作触发 #GP。所以我们没有选择，必须用软件实现上下文切换——而软件切换的核心就是保存和恢复 callee-saved 寄存器。

## 为什么只保存这些寄存器？

这是理解整个上下文切换设计的关键问题。x86-64 的 System V ABI 把通用寄存器分成两类：caller-saved（rax、rcx、rdx、rsi、rdi、r8-r11）和 callee-saved（rbx、rbp、r12-r15）。调用约定规定 callee-saved 寄存器在被调用函数返回时必须保持原值不变——如果被调用函数想用这些寄存器，它必须自己在栈上保存并在返回前恢复。caller-saved 寄存器则没有这个约束，调用者如果需要保留它们的值，必须在调用之前自己保存。

这个约定和上下文切换有什么关系？关系很大。`context_switch()` 本身就是一个函数调用。当某个线程调用 `Scheduler::yield()` -> `context_switch()` 时，编译器已经在调用 `context_switch()` 之前把所有 caller-saved 寄存器保存在当前线程的栈上了（如果它们还有用的话）。所以 `context_switch()` 只需要保存 callee-saved 的 6 个寄存器，再加上 RSP（栈指针，切换的核心）和 RIP（恢复执行的地址），就足以完整保存一个线程的执行状态。rax 之类的寄存器不是丢掉了——它们保存在当前线程的栈上，等这个线程被切回来的时候，栈恢复了，那些值自然也就恢复了。

这就是 Cinux 的 `CpuContext` 保存 8 个 64 位值的全部理由：r15、r14、r13、r12、rbp、rbx、rsp、rip，总共 64 字节。不是偷懒，而是 ABI 已经帮我们把工作分担好了。

## 环境说明

本章涉及的核心文件是 `kernel/proc/process.hpp`（约 200 行声明）和 `kernel/proc/process.cpp`（约 146 行实现）。运行环境和前面几个 tag 一致——GCC cross-compiler（`x86_64-elf`）、QEMU、higher-half 内核（`KERNEL_VMA = 0xFFFFFFFF80000000`）。前置条件是 PMM、VMM 和 Heap 已经初始化完毕，因为 `TaskBuilder::build()` 需要它们来分配和映射内核栈。这个 tag 新增了 2264 行代码，涉及 21 个文件的变更，核心是四个新文件：`process.hpp`、`process.cpp`、`scheduler.hpp`、`scheduler.cpp`，以及一个汇编文件 `context_switch.S`。另外还有约 970 行的测试代码（QEMU 集成测试 + Host 单元测试）。

## CpuContext 结构体

有了上面的理论基础，我们来看 Cinux 的具体实现。

```cpp
struct alignas(16) CpuContext {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rsp;
    uint64_t rip;
};
```

结构体的字段顺序不是随便排的。8 个字段对应 6 个 callee-saved GPR 加上 RSP 和 RIP，偏移量从 0 到 56，每 8 字节一个，整整齐齐。整个结构体 `alignas(16)` 保证对齐，`sizeof` 为 64 字节。

你可能注意到了——字段顺序必须和汇编代码中的偏移量严格一致。为了防止"改了 C++ 头文件忘了改汇编"这种经典翻车，Cinux 用了一排 `static_assert` 来编译期锁定：

```cpp
static_assert(offsetof(CpuContext, r15) == 0, "r15 at offset 0");
static_assert(offsetof(CpuContext, rsp) == 48, "rsp at offset 48");
static_assert(offsetof(CpuContext, rip) == 56, "rip at offset 56");
static_assert(sizeof(CpuContext) == 64, "CpuContext must be 64 bytes");
```

这种"声明即文档、编译即校验"的做法和 Linux 的 `pt_regs` 设计理念一脉相承。如果有人往中间插了个字段或者改了顺序，编译直接报错，不可能漏到运行时才发现偏移量对不上。

## TaskState：线程的生命周期

在进入 `Task` 结构体之前，先看看线程的状态模型。`TaskState` 用 `scoped enum`（`enum class : uint8_t`）定义了四个状态：`Running`、`Ready`、`Blocked`、`Dead`。一个枚举值只占 1 字节，对齐紧凑。

这四个状态覆盖了合作式调度的核心生命周期。新创建的线程处于 `Ready` 状态，等待调度器把它加入运行队列。被调度器选中后变成 `Running`，独占 CPU 执行。线程调用 `yield()` 后回到 `Ready`，重新排队等待。如果线程因等待某个资源而主动挂起，进入 `Blocked` 状态，直到资源就绪被唤醒回 `Ready`。线程退出后变成 `Dead`，调度器把它从运行队列中移除。

对比 Linux 的状态机（`TASK_RUNNING`/`TASK_INTERRUPTIBLE`/`TASK_UNINTERRUPTIBLE`/`EXIT_ZOMBIE`/`EXIT_DEAD` 等，用位图组合），Cinux 的状态集是一个极简子集。Linux 多出来的 `Zombie` 状态是为了让父进程通过 `waitpid()` 获取子进程的退出状态——Cinux 目前还没有实现 `waitpid`，所以暂时不需要这个中间态。等后续实现进程回收的时候，在 `TaskState` 中加入 `Zombie` 即可。

## Task：线程控制块

有了寄存器快照的容器，我们还需要一个更大的结构来承载线程的全部状态。

```cpp
struct Task {
    CpuContext ctx;
    TaskState state;
    uint64_t tid;
    uint64_t priority;
    uint64_t kernel_stack;
    uint64_t kernel_stack_top;
    cinux::mm::AddressSpace* addr_space;
    const char* name;
    SchedulingClass* sched_class;
};
```

`Task` 是一个精简但完整的 TCB——9 个字段覆盖了调度所需的核心信息。`ctx` 字段放在最前面，它的地址和 `Task` 的地址完全相同，在某些场景下可以省去一个偏移计算。`state` 是线程生命周期状态机的当前节点。`tid` 是单调递增的任务 ID。`priority` 留给未来的优先级调度。`kernel_stack` 和 `kernel_stack_top` 记录栈的虚拟地址范围——前者定位栈底溢出魔数，后者设置 TSS 的 RSP0。`addr_space` 指向进程私有页表，纯内核线程为 nullptr。`sched_class` 指向该任务所属的调度策略对象。

对比 Linux 的 `task_struct`（超过 9 KB，包含文件描述符、信号处理、命名空间等），Cinux 的 `Task` 选择了"够用就好"的策略：先实现多线程调度所需的最小字段集合，后续功能在对应的 tag 中逐步添加。`name` 是一个 `const char*`，指向静态字符串字面量，不持有所有权——如果传入栈上的局部 char 数组，函数返回后就会变成悬垂指针，后续的调度器日志打印会读到垃圾数据。

## TaskBuilder：把构造的复杂性藏起来

直接裸分配 `Task` + 手动初始化每个字段非常容易出错——忘了写溢出魔数、栈映射失败但 TCB 已经分配了、返回地址忘了压到栈上……Cinux 用 Builder 模式把这些细节封装到 `TaskBuilder` 里。

```cpp
auto* task_a = TaskBuilder()
    .set_entry(thread_a)
    .set_name("thread_a")
    .build();
```

`build()` 是真正干活的地方。我们来一步步看。

首先，入口检查和 TCB 分配。函数一开头检查 `entry_` 是否为空，如果调用者忘了调 `set_entry()`，直接返回 nullptr 而不是在后面某个步骤崩溃——防御性编程的基本操作。然后用 placement new 在内核堆上分配 `Task` 结构体，对齐到 `alignof(Task)`。分配完后手动逐字节清零，因为内核堆的 `new` 不保证零初始化，不清零的话 `state`、`tid` 等字段可能是垃圾值。

接下来是物理栈的分配和映射。PMM 分配 4 个连续物理页（16 KB），然后通过 `alloc_stack_vaddr` 从内核虚拟地址空间（`0xFFFF800000100000` 起）分配连续的虚拟页。物理页逐页映射到虚拟地址空间（`0x03` 标志是 `FLAG_PRESENT | FLAG_WRITABLE`），如果任何一页映射失败，函数会释放已分配的 TCB 并返回空指针。这种"分配失败就回滚"的模式在内核代码中非常重要，因为内核没有异常处理机制——你只能自己检查每一步的返回值。

映射完成后，在栈底写入溢出检测魔数 `0xDEADC0DE`。如果线程的栈增长到覆盖了栈底的魔数，调试时通过检查魔数是否被破坏就能判断是否发生了栈溢出。

最后也是最精巧的部分——CpuContext 的初始化：

```cpp
task->ctx.rsp = stack_virt + stack_size - 8;
*reinterpret_cast<uint64_t*>(task->ctx.rsp) =
    reinterpret_cast<uint64_t>(&Scheduler::exit_current);
task->ctx.rip = reinterpret_cast<uint64_t>(entry_);
```

`ctx.rip` 指向线程入口函数，但 `ctx.rsp` 不是简单的栈顶。它指向 `stack_virt + stack_size - 8`，并且在那个位置预先写入了 `Scheduler::exit_current` 的地址。这是因为 `context_switch` 用 `jmp`（不是 `call`）跳到入口函数，不会在栈上压入返回地址。当线程函数执行到最后的 `ret` 时，CPU 从 RSP 指向的位置弹出一个 8 字节值作为返回地址——我们预先放的 `exit_current` 地址正好被弹出，线程进入干净的退出流程。这个设计很像 PintOS 的 `kernel_thread()` 封装，只不过 Cinux 把退出地址直接压在了栈上而不是用一个 wrapper 函数。至于这个设计在开发过程中曾经翻过什么车……第三篇踩坑实录会详细讲。

把上面的文字梳理一下，一个新线程的栈布局长这样：

```
高地址
┌──────────────┐  ← kernel_stack_top
│ exit_current │  ← ctx.rsp 指向这里（线程返回地址）
│  (free)      │
│  (free)      │  ← 16 KB 的空栈空间，线程运行时使用
│  (free)      │
│ 0xDEADC0DE   │  ← 栈底魔数，溢出检测
└──────────────┘  ← kernel_stack
低地址
```

`ctx.rsp` 指向 `exit_current` 的位置，`ctx.rip` 指向线程入口函数。当 `context_switch` 通过 `jmp` 跳到入口函数时，线程从入口函数开始执行，栈顶只有一个 `exit_current` 地址——这是线程函数 return 后的唯一去处。栈中间的空白区域就是线程运行时使用的空间——局部变量、函数调用链、中断帧都会压在这里。

最后是 TCB 元数据的填充。`tid` 通过 `next_tid++` 分配，初始状态设为 `Ready`，等待调度器把它加入运行队列。`addr_space` 为 nullptr 表示这是一个纯内核线程，没有独立的用户地址空间。`sched_class` 为 nullptr 时由 `Scheduler::add_task()` 自动分配默认的 RoundRobin 策略。所有这些字段加在一起，就是一个完整的、随时可以运行的线程。

## 设计决策回顾

到这里我们已经把多任务的基础数据结构全部过了一遍。回顾一下几个关键的设计选择，以及为什么这样选而不是那样选。

`CpuContext` 为什么用命名字段 + `static_assert` 而不是 `uint64_t regs[8]` 加宏定义偏移量？因为命名字段本身就是文档——看到 `ctx.rsp` 你就知道这是栈指针，看到 `regs[6]` 你还得去查宏定义才知道这是第几个寄存器。`static_assert` 在编译期锁定偏移量，保证 C++ 声明和汇编实现永远一致。如果用数组加宏，新增字段时很容易忘了更新宏定义，而且编译器不会帮你检查。

`Task` 为什么保持精简而不是一次性把所有字段都加上？因为每个 tag 的变更范围应该可控。Linux 的 `task_struct` 经过了二十多年的演化才变成今天的样子。Cinux 选择"够用就好"——先实现调度所需的最小字段集合，文件描述符、进程树、工作目录等功能在后续的 tag 中逐步添加。这样读者更容易跟踪 `Task` 结构体的演化过程，每个 tag 的 diff 也更聚焦。

栈为什么用魔数而不是 guard page？`STACK_MAGIC` 是一个轻量级的溢出检测手段——写在栈底，检查是否被覆盖就能判断栈溢出。guard page 更强（硬件辅助，访问就触发 #PF），但需要额外的虚拟地址空间和页表操作。当前的魔数方案已经能覆盖最常见的栈溢出场景，后续可以按需加入 guard page。

## 收尾

这一章我们搭好了多任务的基础积木：`CpuContext` 定义了"一个线程的执行状态长什么样"，`Task` 把线程的全部元数据打包在一起，`TaskBuilder` 封装了构造过程中所有容易出错的细节。

在结束之前，我们站远一点看看这个设计的全貌。Cinux 的线程管理分为三层：最底层是 `CpuContext`——64 字节的寄存器快照，是上下文切换的原子操作对象；中间层是 `Task`——包含 `CpuContext` 加上调度状态、内存信息的 TCB；最上层是 `TaskBuilder`——封装了从 TCB 分配到栈映射到上下文初始化的全部细节。三层各司其职，互不耦合：修改 `CpuContext` 只影响上下文切换的汇编代码；修改 `Task` 只影响调度器和进程管理；修改 `TaskBuilder` 只影响线程创建流程。

但这些东西本身不会让线程跑起来——真正让两个线程交替执行的，是下一章要讲的 `context_switch.S` 汇编原语和 `RoundRobin` 调度器。而真正让开发过程跌宕起伏的，是第三篇要讲的那些让内核崩掉的 bug。

## 参考资料

- Intel SDM Vol. 3A, Chapter 8 "Task Management", Section 8.7 "Task Management in 64-Bit Mode" (PDF pages 281-282)：64 位模式不支持硬件任务切换，必须由软件完成
- System V AMD64 ABI: [https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf](https://refspecs.linuxbase.org/elf/x86_64-abi-0.99.pdf) — callee-saved 寄存器定义
- xv6 `swtch.S` / `proc.h`: [https://github.com/mit-pdos/xv6-public](https://github.com/mit-pdos/xv6-public) — 32 位上下文切换和 TCB 设计对比
- Linux `__switch_to_asm`: [https://kernel-internals.org/sched/context-switch/](https://kernel-internals.org/sched/context-switch/) — 64 位上下文切换的工业级实现
- PintOS `switch.S`: [https://uchicago-cs.github.io/mpcs52030/switch.html](https://uchicago-cs.github.io/mpcs52030/switch.html) — 线程退出封装模式参考
