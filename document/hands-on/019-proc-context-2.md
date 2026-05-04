# 019-2 上下文切换与调度器

## 导语

上一篇我们把 Task 控制块、CpuContext 和 TaskBuilder 设计好了，地基已经打完。现在要盖楼了——我们要写一段不到 50 行的汇编来保存和恢复寄存器，实现 CPU 在两个任务之间的切换；然后搭一个最简单的 RoundRobin 调度器，让多个任务轮流使用 CPU。
这一篇是整个 tag 019 的核心——上下文切换是操作系统最本质的操作，理解了它你就理解了"多任务"到底是怎么做到的。说实话，第一次看到自己的两个线程在 QEMU 里交替打印的时候，那种感觉还是相当奇妙的——从零搓到能跑多线程，这是一个不小的里程碑。

## 概念精讲

### context_switch 的行为模型

我们可以把 context_switch 理解为一个拥有双重人格的函数——它被调用一次，却会"返回"两次。第一次"返回"发生在当前任务（被切出方）的上下文中，此时调用者看起来就像是 context_switch 刚刚执行完毕并正常返回了；第二次"返回"发生在另一个任务（被切入方）的上下文中——那个任务之前也是调用 context_switch 时被暂停的，现在它从暂停点恢复了。
这个"调用一次返回两次"的魔法完全靠保存和恢复 rsp + rip 来实现：切换 rsp 让 CPU 使用新任务的栈，跳转到保存的 rip 让 CPU 从新任务上次暂停的地方继续执行。
被切出的任务呢？它的 rsp 和 rip 已经被安全地保存在了自己的 CpuContext 中。下次调度器把它切换回来时，它会从 context_switch 内部的 `.restore` 标签处恢复执行，然后 ret 返回到调用 context_switch 的上层函数——就像什么都没发生过一样。
这个过程和 Linux 的 `__switch_to_asm` 在原理上完全一致：保存 callee-saved 寄存器 + rsp，加载新任务的寄存器 + rsp，然后跳转。Linux 的实现（arch/x86/entry/entry_64.S）也是保存 rbx、rbp、r12-r15 这六个 callee-saved 寄存器加上 rsp，和 Cinux 的选择一模一样。

> 参考：Intel SDM Vol.3A Section 8.7 — 64-bit mode 不支持硬件任务切换，必须用软件实现
> 参考：OSDev Wiki — Context Switching (https://wiki.osdev.org/Context_Switching)
> 参考：Linux kernel context switch 演化史 — https://www.maizure.org/projects/evolution_x86_context_switch_linux/

### RoundRobin 的循环队列设计

RoundRobin 是最朴素的调度算法——所有任务地位平等，排成一队轮流执行。我们的实现用一个固定大小的循环缓冲区（64 个槽位）来存储就绪队列，head 和 tail 两个索引指示队列的头尾。enqueue 在 tail 处追加，pick_next 从 head 处取出，取出后把任务重新追加到 tail——这就是真正的 round-robin 语义：每个任务执行一轮后回到队列末尾等待下一轮。dequeue 是一个线性查找 + 压缩操作：找到目标任务后，把后面的元素依次前移填补空位。这在任务数量少的时候（我们目前只有两三个）性能完全够用。

这个设计和 xv6 的 `struct proc` 数组 + 线性扫描调度有异曲同工之妙——简单、直接、可预测。Linux 的 O(1) 调度器用两个优先级数组（active 和 expired）来实现更高效的时间片轮转，但那种复杂度对我们的教学内核来说完全没必要。

## 动手实现

### Step 1: 实现 context_switch.S

**目标**: 在 `kernel/arch/x86_64/context_switch.S` 中实现上下文切换的汇编原语。

**设计思路**: 函数签名为 `void context_switch(CpuContext* from, CpuContext* to)`，按 System V ABI 参数通过 rdi 和 rsi 传入。整个函数分三个阶段。第一阶段是"保存"：把 callee-saved 寄存器（r15, r14, r13, r12, rbp, rbx）逐个写入 from 指向的 CpuContext 对应偏移处，然后把当前 rsp 保存到 from+48。接下来是最巧妙的一步——用 `lea .restore(%rip), %rax` 计算出 `.restore` 标签的绝对地址，保存到 from+56 作为 rip。为什么要用 lea 而不是直接保存当前 rip？因为 CPU 没有直接读取 rip 的指令——lea 通过 RIP-relative 寻址间接拿到了下一条指令的地址。这样一来，当这个任务被切换回来时，context_switch 会跳转到 `.restore`，然后 ret 返回到调用 yield() 的上层函数。

第二阶段是"恢复"：从 to 指向的 CpuContext 逐个加载 callee-saved 寄存器，然后把 to+48 的值加载到 rsp——这一步切换了栈。注意恢复的顺序：先恢复通用寄存器（r15-r12, rbp, rbx），再切换 rsp，最后用 `jmp *56(%rsi)` 跳转到 to+56 保存的 rip。

第三阶段就是 `.restore` 标签：`ret`。当被切出的任务再次被切换回来时，执行流会到达这里，然后 ret 到调用者的代码。注意整个函数里没有 push/pop——我们直接用 mov 指令读写 CpuContext 结构体的固定偏移，这样避免了栈操作带来的 rsp 副作用。整个文件只有 75 行，纯粹的寄存器保存/恢复和栈切换——没有 MSR 读写、没有中断控制、没有 FPU 状态操作。后续如果需要 GS MSR 保存（per-CPU 数据区）或 FPU 状态保存（用户态浮点），可以在此基础上逐步添加。

**实现约束**: 文件使用 AT&T 汇编语法，`.section .text` 段。用 `.global context_switch` 和 `.type context_switch, @function` 导出符号。所有注释用 `/* */` 风格。

**踩坑预警**: 第一大坑——context_switch 末尾用 `jmp` 跳转到新任务的 rip，不是 `call`，也不是 `ret`。这意味着新任务的栈上不需要有 context_switch 的返回地址，但线程入口函数的栈上必须有返回地址——否则入口函数 return 时 ret 弹出的就是垃圾。这个坑在第三篇文章中有完整的排查过程，先留个印象。

**验证**: 编译通过即可。真正的运行验证需要等调度器完成后才能做。

### Step 2: 定义 SchedulingClass 接口和 RoundRobin 实现

**目标**: 在 `kernel/proc/scheduler.hpp` 中定义调度策略的虚基类，以及 RoundRobin 的声明。

**设计思路**: SchedulingClass 是一个纯虚接口，定义了调度策略必须实现的三个操作：enqueue（将任务加入就绪队列）、dequeue（将任务从队列中移除）、pick_next（取出下一个应该执行的任务）。还有一个 name() 方法返回策略名称，用于日志输出。
RoundRobin 继承 SchedulingClass，内部维护一个固定大小的 Task 指针数组（64 个元素）作为循环队列，加上 head、tail、count 三个索引变量。构造函数将所有槽位初始化为 nullptr。enqueue 把任务放到 tail 位置并递增 tail，同时设置任务状态为 Ready。dequeue 线性搜索目标任务，找到后把后续元素前移压缩。pick_next 从 head 取出第一个任务，状态设为 Running，然后立刻把它重新追加到 tail——这就是 round-robin 的语义，每个任务执行完一轮后回到队尾排队。如果队列为空，pick_next 返回 nullptr。

我们为什么不用链表？因为我们的内核不支持动态内存分配的链表节点（malloc 在中断上下文中不安全），固定数组更简单、更可预测。64 个槽位对我们来说绰绰有余——真正的限制是 PMM 的物理页数量而不是调度队列大小。

**实现约束**: 命名空间 `cinux::proc`。RoundRobin::MAX_TASKS = 64。三个核心操作没有加锁——因为当前是单核协作式调度，yield() 和 exit_current() 调用时不会发生中断嵌套。

**验证**: 编译通过。

### Step 3: 实现 Scheduler 静态门面类

**目标**: 在 scheduler.hpp 和 scheduler.cpp 中实现调度器的顶层接口。

**设计思路**: Scheduler 是一个全静态方法的类——它不持有实例状态，而是通过静态成员变量管理全局调度状态。这些静态成员包括 classes_（已注册的调度策略数组，最多 4 个）、current_（当前正在运行的任务指针）和 default_rr_（默认的 RoundRobin 实例）。
核心方法有五个。init() 初始化调度器：清空状态、注册默认的 RoundRobin 策略。add_task() 给任务分配默认调度策略（如果没指定的话）然后入队。yield() 是主动让出 CPU：获取当前任务的调度策略，pick_next 取出下一个任务，调用 context_switch 切换到新任务。exit_current() 是线程退出：把当前任务标记为 Dead、从队列中移除、pick_next 找到下一个任务、context_switch 切换过去——如果队列空了就执行 `cli; hlt` 永久停机。
run_first() 是一个特殊的方法，用于从"启动上下文"切换到第一个真正的任务。它把 boot_task 设为 current_，从 RoundRobin 取出第一个任务，然后 context_switch 切换过去——boot_task 的 CpuContext 保存的是启动时的寄存器状态，这个状态以后不会再被恢复。

**实现约束**: 所有方法都是 static 的。Scheduler 的静态成员变量在 scheduler.cpp 顶部定义并初始化。current_ 在 exit_current 和 schedule 中更新时要特别注意顺序——先保存旧值再更新，否则 from 和 to 可能指向同一个对象。

**踩坑预警**: 这里有 tag 019 中最凶险的 bug 之一——exit_current 的指针覆盖。如果你写 `current_ = next; context_switch(&current_->ctx, &next->ctx);`，那么 from 和 to 就指向同一个 Task，context_switch 变成一个 no-op（保存完寄存器又立刻恢复了同样的值），执行继续在已经标记为 Dead 的任务栈上运行——迟早会崩。正确写法是先用局部变量 `prev` 保存旧值：`Task* prev = current_; current_ = next; context_switch(&prev->ctx, &next->ctx);`。这个 bug 的排查过程在第三篇文章中详细记录，先记住结论：在指针操作中，凡是"先赋值再用旧值"的场景都要多看一眼。

**验证**: 编译通过。

### Step 4: 更新 CMakeLists 并添加 kernel_main 调度器初始化

**目标**: 把 scheduler.cpp 和 context_switch.S 加入构建，在 kernel_main 中初始化调度器并创建演示线程。

**设计思路**: CMakeLists.txt 需要添加 `kernel/proc/scheduler.cpp`、`kernel/arch/x86_64/context_switch.S` 到源文件列表。注意 .S 文件（大写 S）会被 GCC 当作需要预处理的汇编源文件处理——所以 .S 文件里可以用 `#include` 和 `#define`，而 .s 文件（小写 s）不会经过预处理器。
在 kernel_main 中，我们需要在所有子系统初始化完成之后启动调度器。调用顺序是：Scheduler::init()、用 TaskBuilder 创建两个演示线程、把线程 add_task 到调度器、构造一个 boot_task 作为启动上下文、调用 run_first(boot_task) 切换到第一个线程。
两个演示线程的设计很简单：各循环 5 次，每次打印一行带标识的日志，然后调用 yield() 让出 CPU。交替执行后应该能看到类似 `[A] thread_a iteration 0` / `[B] thread_b iteration 0` 的交替输出。

**实现约束**: context_switch 声明为 `extern "C"` 以避免 C++ 的 name mangling——汇编函数必须在 C linkage 下声明，否则链接器找不到符号。

**验证**: 编译并运行内核，检查串口输出。

```bash
cmake --build build --target big_kernel && \
  qemu-system-x86_64 -kernel build/big_kernel.bin -serial stdio -display none 2>&1 | grep -E '\[A\]|\[B\]|\[SCHED\]'
```

## 构建与运行

如果一切正确，内核启动后你应该能看到类似这样的输出：

```
[SCHED] Scheduler initialised with RoundRobin class
[SCHED] Task tid=1 'thread_a' added to RoundRobin
[SCHED] Task tid=2 'thread_b' added to RoundRobin
[A] thread_a iteration 0
[B] thread_b iteration 0
[A] thread_a iteration 1
[B] thread_b iteration 1
...
[A] thread_a done
[SCHED] Task tid=1 'thread_a' exited
[B] thread_b done
[SCHED] Task tid=2 'thread_b' exited
[SCHED] No more tasks, halting.
```

如果你看到的输出在 thread_a done 之后就 crash 了（比如 `RIP=00000000deadc0de`），恭喜你踩到了线程退出崩溃的 bug——这正是下一篇要解决的问题。原因就是上一篇提到的"栈上没有返回地址"和"exit_current 指针覆盖"两个 bug 叠加。

## 调试技巧

**QEMU 显示 "emulation failure" + RIP=0xDEADC0DE**: 这就是线程函数 ret 弹出了栈底的 magic 值。说明 TaskBuilder 没有在栈顶压入 exit_current 的地址，或者压入的位置不对。检查 build() 中 rsp 的计算：应该是 `stack_virt + stack_size - 8`（栈顶减一个指针宽度），而不是裸的 `stack_virt + stack_size`。

**context_switch 后没有输出**: 可能是 from 和 to 指向了同一个 CpuContext。在 exit_current 和 schedule 中加一个 `kprintf("prev=%p next=%p\n", prev, next)` 的日志来确认两个指针不同。

**编译错误 "undefined reference to context_switch"**: 确认 process.hpp 中 context_switch 的声明使用了 `extern "C"`。如果用了 C++ linkage，函数名会被 mangle 成类似 `_ZN6cinux4proc14context_switchEP10CpuContextS2_` 的东西，汇编中定义的 `context_switch` 符号自然找不到。

**调度器 halt 后继续收到中断**: 这是正常的——idle 任务或者 halt 循环中 CPU 处于 hlt 状态，中断到来时唤醒执行一轮，然后又 hlt。不影响功能。

## 本章小结

这一篇我们把上下文切换和调度器从零实现了出来：context_switch.S 用 callee-saved 寄存器保存/恢复 + 栈切换 + rip 跳转三步完成了任务切换的原子操作，RoundRobin 用循环队列实现了最朴素的公平调度，Scheduler 用静态门面把 init/add_task/yield/exit_current/run_first 这些操作串起来。
到这里我们的内核已经可以运行多个内核线程了——只不过如果线程退出就会 crash。下一篇我们修复两个关键 bug（Higher-Half 内核的 ELF 加载错误和线程退出崩溃），然后做一个完整的两线程交替打印集成测试来验证一切工作正常。
