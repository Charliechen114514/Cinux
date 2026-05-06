# 019-2 上下文切换与调度器

## 导语

上一篇我们把 Task 控制块、CpuContext 和 TaskBuilder 设计好了，地基已经打完。现在要盖楼了——我们要写一段不到 50 行的汇编来保存和恢复寄存器，实现 CPU 在两个任务之间的切换；然后搭一个最简单的 RoundRobin 调度器，让多个任务轮流使用 CPU。

这一篇是整个 tag 019 的核心——上下文切换是操作系统最本质的操作，理解了它你就理解了"多任务"到底是怎么做到的。说实话，第一次看到自己的两个线程在 QEMU 里交替打印的时候，那种感觉还是相当奇妙的——从零搓到能跑多线程，这是一个不小的里程碑。

本篇的核心问题是：如何让 CPU 从正在执行的线程 A"暂停"，转而去执行线程 B，稍后再切回来时 A 从暂停点继续？答案是保存 A 的 callee-saved 寄存器、切换栈指针、跳转到 B 的入口——这就是 context_switch 做的全部事情。

## 概念精讲

### context_switch 的行为模型

我们可以把 context_switch 理解为一个拥有双重人格的函数——它被调用一次，却会"返回"两次。第一次"返回"发生在当前任务（被切出方）的上下文中，此时调用者看起来就像是 context_switch 刚刚执行完毕并正常返回了；第二次"返回"发生在另一个任务（被切入方）的上下文中——那个任务之前也是调用 context_switch 时被暂停的，现在它从暂停点恢复了。

这个"调用一次返回两次"的魔法完全靠保存和恢复 rsp + rip 来实现：切换 rsp 让 CPU 使用新任务的栈，跳转到保存的 rip 让 CPU 从新任务上次暂停的地方继续执行。

被切出的任务呢？它的 rsp 和 rip 已经被安全地保存在了自己的 CpuContext 中。下次调度器把它切换回来时，它会从 context_switch 内部的 `.restore` 标签处恢复执行，然后 ret 返回到调用 context_switch 的上层函数——就像什么都没发生过一样。

这个过程和 Linux 的 `__switch_to_asm` 在原理上完全一致：保存 callee-saved 寄存器 + rsp，加载新任务的寄存器 + rsp，然后跳转。Linux 的实现（arch/x86/entry/entry_64.S）也是保存 rbx、rbp、r12-r15 这六个 callee-saved 寄存器加上 rsp，和 Cinux 的选择一模一样。

和 xv6 的对比也很有意思。xv6 的 `swtch.S` 把返回地址隐式地保存在栈上（call 压入的），而 Cinux 用 `lea .restore(%rip)` 显式计算并存入结构体字段。xv6 的方式更简洁，但 Cinux 的方式更利于调试——你可以直接打印 ctx.rip 看到一个有意义的恢复地址，而不是一个栈地址。

> 参考：Intel SDM Vol.3A Section 8.7 — 64-bit mode 不支持硬件任务切换，必须用软件实现
> 参考：OSDev Wiki — Context Switching (https://wiki.osdev.org/Context_Switching)
> 参考：Linux kernel context switch 演化史 — https://www.maizure.org/projects/evolution_x86_context_switch_linux/

### RoundRobin 的循环队列设计

RoundRobin 是最朴素的调度算法——所有任务地位平等，排成一队轮流执行。我们的实现用一个固定大小的循环缓冲区（64 个槽位）来存储就绪队列，head 和 tail 两个索引指示队列的头尾。enqueue 在 tail 处追加，pick_next 从 head 处取出，取出后把任务重新追加到 tail——这就是真正的 round-robin 语义：每个任务执行一轮后回到队列末尾等待下一轮。dequeue 是一个线性查找 + 压缩操作：找到目标任务后，把后面的元素依次前移填补空位。这在任务数量少的时候（我们目前只有两三个）性能完全够用。

这个设计和 xv6 的 `struct proc` 数组 + 线性扫描调度有异曲同工之妙——简单、直接、可预测。Linux 的 O(1) 调度器用两个优先级数组（active 和 expired）来实现更高效的时间片轮转，但那种复杂度对我们的教学内核来说完全没必要。

### SchedulingClass 虚接口的意义

你可能会问：既然只有 RoundRobin 一种策略，为什么要抽象出 SchedulingClass 接口？直接写 RoundRobin 不就好了？

答案是为扩展做准备。Linux 的调度策略体系（CFS / RT / Deadline / Idle）通过类似的 `sched_class` 层次结构组织，非常值得借鉴。先抽象接口，以后加 CFS 或者实时调度器不需要改 Scheduler 的代码。目前 SchedulingClass 只定义了四个虚方法：enqueue、dequeue、pick_next 和 name——这个接口已经足够覆盖大多数调度策略的核心操作了。

## 动手实现

### Step 1: 实现 context_switch.S

**目标**: 在 `kernel/arch/x86_64/context_switch.S` 中实现上下文切换的汇编原语。

**设计思路**: 函数签名为 `void context_switch(CpuContext* from, CpuContext* to)`，按 System V ABI 参数通过 rdi 和 rsi 传入。整个函数分三个阶段。第一阶段是"保存"：把 callee-saved 寄存器（r15, r14, r13, r12, rbp, rbx）逐个写入 from 指向的 CpuContext 对应偏移处，然后把当前 rsp 保存到 from+48。接下来是最巧妙的一步——用 `lea .restore(%rip), %rax` 计算出 `.restore` 标签的绝对地址，保存到 from+56 作为 rip。为什么要用 lea 而不是直接保存当前 rip？因为 CPU 没有直接读取 rip 的指令——lea 通过 RIP-relative 寻址间接拿到了下一条指令的地址。这样一来，当这个任务被切换回来时，context_switch 会跳转到 `.restore`，然后 ret 返回到调用 yield() 的上层函数。

第二阶段是"恢复"：从 to 指向的 CpuContext 逐个加载 callee-saved 寄存器，然后把 to+48 的值加载到 rsp——这一步切换了栈。注意恢复的顺序：先恢复通用寄存器（r15-r12, rbp, rbx），再切换 rsp，最后用 `jmp *56(%rsi)` 跳转到 to+56 保存的 rip。

第三阶段就是 `.restore` 标签：`ret`。当被切出的任务再次被切换回来时，执行流会到达这里，然后 ret 到调用者的代码。注意整个函数里没有 push/pop——我们直接用 mov 指令读写 CpuContext 结构体的固定偏移，这样避免了栈操作带来的 rsp 副作用。整个文件只有 75 行，纯粹的寄存器保存/恢复和栈切换——没有 MSR 读写、没有中断控制、没有 FPU 状态操作。后续如果需要 GS MSR 保存（per-CPU 数据区）或 FPU 状态保存（用户态浮点），可以在此基础上逐步添加。

**实现约束**: 文件使用 AT&T 汇编语法，`.section .text` 段。用 `.global context_switch` 和 `.type context_switch, @function` 导出符号。所有注释用 `/* */` 风格。文件名必须用大写 `.S` 后缀（不是小写 `.s`），因为大写 S 会经过 C 预处理器，可以用 `#include` 和 `#define`。

**踩坑预警**: 第一大坑——context_switch 末尾用 `jmp` 跳转到新任务的 rip，不是 `call`，也不是 `ret`。这意味着新任务的栈上不需要有 context_switch 的返回地址，但线程入口函数的栈上必须有返回地址——否则入口函数 return 时 ret 弹出的就是垃圾。这个坑在第三篇文章中有完整的排查过程，先留个印象。

**验证**: 编译通过即可。真正的运行验证需要等调度器完成后才能做。

如果你想在此时做单元测试，可以在 test_scheduler.cpp 中写一个 cooperative switch 测试——用三个静态 CpuContext（boot, task_a, task_b）和静态栈缓冲区，手动设置 rip/rsp 然后调用 context_switch，验证执行流的跳转是否正确。

### Step 2: 定义 SchedulingClass 接口和 RoundRobin 实现

**目标**: 在 `kernel/proc/scheduler.hpp` 中定义调度策略的虚基类，以及 RoundRobin 的声明。

**设计思路**: SchedulingClass 是一个纯虚接口，定义了调度策略必须实现的三个操作：enqueue（将任务加入就绪队列）、dequeue（将任务从队列中移除）、pick_next（取出下一个应该执行的任务）。还有一个 name() 方法返回策略名称，用于日志输出。
RoundRobin 继承 SchedulingClass，内部维护一个固定大小的 Task 指针数组（64 个元素）作为循环队列，加上 head、tail、count 三个索引变量。构造函数将所有槽位初始化为 nullptr。enqueue 把任务放到 tail 位置并递增 tail，同时设置任务状态为 Ready。dequeue 线性搜索目标任务，找到后把后续元素前移压缩。pick_next 从 head 取出第一个任务，状态设为 Running，然后立刻把它重新追加到 tail——这就是 round-robin 的语义，每个任务执行完一轮后回到队尾排队。如果队列为空，pick_next 返回 nullptr。

我们为什么不用链表？因为我们的内核不支持动态内存分配的链表节点（malloc 在中断上下文中不安全），固定数组更简单、更可预测。64 个槽位对我们来说绰绰有余——真正的限制是 PMM 的物理页数量而不是调度队列大小。

**实现约束**: 命名空间 `cinux::proc`。RoundRobin::MAX_TASKS = 64。三个核心操作没有加锁——因为当前是单核协作式调度，yield() 和 exit_current() 调用时不会发生中断嵌套。

为什么不用链表？因为我们的内核不支持动态内存分配的链表节点（malloc 在中断上下文中不安全），固定数组更简单、更可预测。64 个槽位绰绰有余——真正的限制是 PMM 的物理页数量而不是调度队列大小。如果未来需要支持更多任务，可以把 run_queue_ 改成动态数组或哈希表。

**验证**: 编译通过。

### Step 3: 实现 Scheduler 静态门面类

**目标**: 在 scheduler.hpp 和 scheduler.cpp 中实现调度器的顶层接口。

**设计思路**: Scheduler 是一个全静态方法的类——它不持有实例状态，而是通过静态成员变量管理全局调度状态。这些静态成员包括 classes_（已注册的调度策略数组，最多 4 个）、current_（当前正在运行的任务指针）和 default_rr_（默认的 RoundRobin 实例）。
核心方法有五个。init() 初始化调度器：清空状态、注册默认的 RoundRobin 策略。add_task() 给任务分配默认调度策略（如果没指定的话）然后入队。yield() 是主动让出 CPU：获取当前任务的调度策略，pick_next 取出下一个任务，调用 context_switch 切换到新任务。exit_current() 是线程退出：把当前任务标记为 Dead、从队列中移除、pick_next 找到下一个任务、context_switch 切换过去——如果队列空了就执行 `cli; hlt` 永久停机。
run_first() 是一个特殊的方法，用于从"启动上下文"切换到第一个真正的任务。它把 boot_task 设为 current_，从 RoundRobin 取出第一个任务，然后 context_switch 切换过去——boot_task 的 CpuContext 保存的是启动时的寄存器状态，这个状态以后不会再被恢复。

**实现约束**: 所有方法都是 static 的。Scheduler 的静态成员变量在 scheduler.cpp 顶部定义并初始化。current_ 在 exit_current 和 schedule 中更新时要特别注意顺序——先保存旧值再更新，否则 from 和 to 可能指向同一个对象。

**踩坑预警**: 这里有 tag 019 中最凶险的 bug 之一——exit_current 的指针覆盖。如果你写 `current_ = next; context_switch(&current_->ctx, &next->ctx);`，那么 from 和 to 就指向同一个 Task，context_switch 变成一个 no-op（保存完寄存器又立刻恢复了同样的值），执行继续在已经标记为 Dead 的任务栈上运行——迟早会崩。正确写法是先用局部变量 `prev` 保存旧值：`Task* prev = current_; current_ = next; context_switch(&prev->ctx, &next->ctx);`。这个 bug 的排查过程在第三篇文章中详细记录，先记住结论：在指针操作中，凡是"先赋值再用旧值"的场景都要多看一眼。

**验证**: 编译通过。

如果你想在两篇之间做部分测试，可以只实现 Scheduler::init() 和 add_task()，在 kernel_main 中创建两个 Task、加入队列、打印队列内容来验证 RoundRobin 的 enqueue 和 pick_next 逻辑。不需要 context_switch 就能验证调度策略的正确性。

### Step 4: 更新 CMakeLists 并添加 kernel_main 调度器初始化

**目标**: 把 scheduler.cpp 和 context_switch.S 加入构建，在 kernel_main 中初始化调度器并创建演示线程。

**设计思路**: CMakeLists.txt 需要添加 `kernel/proc/scheduler.cpp`、`kernel/arch/x86_64/context_switch.S` 到源文件列表。注意 .S 文件（大写 S）会被 GCC 当作需要预处理的汇编源文件处理——所以 .S 文件里可以用 `#include` 和 `#define`，而 .s 文件（小写 s）不会经过预处理器。
在 kernel_main 中，我们需要在所有子系统初始化完成之后启动调度器。调用顺序是：Scheduler::init()、用 TaskBuilder 创建两个演示线程、把线程 add_task 到调度器、构造一个 boot_task 作为启动上下文、调用 run_first(boot_task) 切换到第一个线程。
两个演示线程的设计很简单：各循环 5 次，每次打印一行带标识的日志，然后调用 yield() 让出 CPU。交替执行后应该能看到类似 `[A] thread_a iteration 0` / `[B] thread_b iteration 0` 的交替输出。

这也是 tag 019 的设计意图——第二篇结束时你的代码"几乎"能工作，但还有关键 bug 需要修复。这种"先实现核心功能、再修 bug"的开发节奏在内核开发中非常常见。实际产出中，先看到"交替打印能跑"给了我们信心，然后才去定位退出崩溃的问题。

boot_task 的构造需要特别注意：它不是通过 TaskBuilder 创建的（因为 boot_task 不需要栈分配），而是手动清零一个栈上的 Task 结构体，然后设置 state=Running、tid=0、name="boot"。它的 CpuContext 在 context_switch 时会被写入（保存启动时的寄存器状态），但这个状态以后不会再被恢复。

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

## 构建与验证策略

这一篇涉及的代码量比较大（context_switch.S 约 75 行、scheduler.cpp 约 160 行、main.cpp 修改约 40 行），建议分步验证。不要试图一次性写完所有代码然后编译——那种方式在内核开发中几乎注定会失败。

1. 先只编译 context_switch.S，确认汇编语法正确（`as` 不报错）
2. 再编译 scheduler.cpp，确认 C++ 代码和 process.hpp 的接口一致
3. 最后修改 kernel_main，完整编译并运行

这种增量式验证策略在内核开发中非常重要——一次性改了太多文件，出错时很难定位是哪个文件的问题。每一步都确认编译通过后再进行下一步，可以大大减少调试时间。

另一个有用的技巧：在 kernel_main 的调度启动代码前后各加一个 kprintf，比如 `[BIG] Starting cooperative multitasking demo...` 和 `[BIG] Switching to first task...`。这样如果内核在调度启动之前就 crash 了，你可以通过最后的日志判断崩溃发生在哪一步。run_first 之后的日志只有在所有线程都退出后才会执行——因为 run_first 切换到第一个线程后，kernel_main 的后续代码要等到 run_first"返回"才会执行。而 run_first 的"返回"发生在所有线程退出、调度器 halt 的时候。

## 调试技巧

**QEMU 显示 "emulation failure" + RIP=0xDEADC0DE**: 这就是线程函数 ret 弹出了栈底的 magic 值。说明 TaskBuilder 没有在栈顶压入 exit_current 的地址，或者压入的位置不对。检查 build() 中 rsp 的计算：应该是 `stack_virt + stack_size - 8`（栈顶减一个指针宽度），而不是裸的 `stack_virt + stack_size`。

**context_switch 后没有输出**: 可能是 from 和 to 指向了同一个 CpuContext。在 exit_current 和 schedule 中加一个 `kprintf("prev=%p next=%p\n", prev, next)` 的日志来确认两个指针不同。

**编译错误 "undefined reference to context_switch"**: 确认 process.hpp 中 context_switch 的声明使用了 `extern "C"`。如果用了 C++ linkage，函数名会被 mangle 成类似 `_ZN6cinux4proc14context_switchEP10CpuContextS2_` 的东西，汇编中定义的 `context_switch` 符号自然找不到。

**QEMU 启动后没有任何输出**: 可能是 run_first 在 boot_task 的 CpuContext 中保存了启动时的寄存器，但 boot_task 没有正确初始化（比如忘记清零）。确保 boot_task 被逐字节清零，并且 state 和 tid 字段被正确设置。

**交替输出变成了单线程执行**: yield() 中有一个 `next == current_` 的检查——如果 pick_next 返回了当前线程自身，yield() 直接返回不做切换。这通常发生在只有一个线程在运行队列中的情况。如果你创建了两个线程但只有一个在队列里，检查另一个是否 add_task 成功。

**调度器 halt 后继续收到中断**: 这是正常的——idle 任务或者 halt 循环中 CPU 处于 hlt 状态，中断到来时唤醒执行一轮，然后又 hlt。不影响功能。

**线程 A 的 yield 没有切到线程 B**: 可能是 RoundRobin 的 pick_next 返回了 A 自己。在只有两个线程的情况下，pick_next 永远应该在 A 和 B 之间交替。如果 yield() 中的 `next == current_` 检查触发了，说明 pick_next 有 bug——检查 head/tail/count 的更新逻辑。

## 设计分析

### 为什么 context_switch 不保存 FPU/SIMD 状态

当前的内核线程不使用浮点运算，省去 512 字节的 fxsave/fxrstor 可以让每次切换更快。后续如果需要支持用户态进程（可能使用 SSE/AVX），可以在切换前后加上 FPU 状态保存。Linux 用延迟 FPU 保存来优化这一点——只有当另一个线程实际使用了 FPU 时才保存，通过 CR0.TS 位触发 #NM 异常来按需保存。

### 为什么 Scheduler 是全静态的

Scheduler 不需要被实例化——它本质上是一组全局状态（current_ 指针、调度策略列表）加上操作这些状态的方法。用静态类比全局变量加自由函数更清晰：命名空间明确（`Scheduler::yield()` vs `yield()`），静态成员变量集中管理，而且不允许意外创建实例。Linux 的调度器也是类似的设计——全局的 `struct rq` 数组（per-CPU 运行队列）加上全局函数。

### extern "C" 的必要性

context_switch 的声明在 process.hpp 中使用了 `extern "C"`，这是必须的——C++ 编译器会对函数名进行 name mangling（比如把 `context_switch` 变成 `_ZN6cinux4proc14context_switchEP10CpuContextS2_`），而汇编中定义的符号名是原始的 `context_switch`。不用 `extern "C"` 链接器会找不到符号。

### run_first 的特殊角色

run_first 和 yield/exit_current 不同——它是从"启动上下文"到第一个线程的一次性切换。boot_task 不在调度队列中，它的 CpuContext 只作为 context_switch 的 from 参数。这个上下文以后不会被恢复——因为内核永远不会"切回"启动上下文。boot_task 的唯一作用是提供一个合法的 CpuContext 地址，让 context_switch 的保存阶段有地方写入。这意味着 boot_task 可以是栈上的局部变量——它不需要比 run_first 的调用帧活得更久。这也是为什么 kernel_main 中直接在栈上声明 `Task boot_task;` 而不是用 new 分配。

## 本章小结

这一篇我们把上下文切换和调度器从零实现了出来：context_switch.S 用 callee-saved 寄存器保存/恢复 + 栈切换 + rip 跳转三步完成了任务切换的原子操作，RoundRobin 用循环队列实现了最朴素的公平调度，Scheduler 用静态门面把 init/add_task/yield/exit_current/run_first 这些操作串起来。

回顾一下从调用者的视角看整个切换过程。线程 A 调用 `Scheduler::yield()`，yield 内部调用 `context_switch(&A->ctx, &B->ctx)`。context_switch 保存 A 的寄存器到 A->ctx，切换到 B 的栈，跳转到 B 的入口。B 执行一段时间后也调用 yield()，context_switch 保存 B 的寄存器到 B->ctx，切换回 A 的栈，跳转到 A 的 .restore 标签——A 从 yield() 返回，就像刚才什么都没发生过一样。这就是"调用一次返回两次"的本质。

整个切换过程的关键时间点只有三个：保存寄存器（约 8 条 mov 指令）、切换 rsp（1 条 mov）、跳转到新 rip（1 条 jmp）。从硬件角度看，这三个操作加起来的延迟在纳秒级别——上下文切换本身不是性能瓶颈，真正的开销在 cache miss 和 TLB flush（切换地址空间时）。

到这里我们的内核已经可以运行多个内核线程了——只不过如果线程退出就会 crash。下一篇我们修复两个关键 bug（Higher-Half 内核的 ELF 加载错误和线程退出崩溃），然后做一个完整的两线程交替打印集成测试来验证一切工作正常。

## 延伸阅读

- **Intel SDM Vol.3A Section 8.7**: 64 位模式任务管理的完整描述
- **System V AMD64 ABI Section 3.2**: 调用约定的寄存器分类定义
- **Linux `__switch_to_asm`** (arch/x86/entry/entry_64.S): 工业级上下文切换实现参考
- **xv6 `swtch.S`** (https://github.com/mit-pdos/xv6-public): 32 位上下文切换对比
- **OSDev Wiki "Context Switching"** (https://wiki.osdev.org/Context_Switching): 软件上下文切换的通用方法
- **OSDev Wiki "Scheduling Algorithms"** (https://wiki.osdev.org/Scheduling_Algorithms): Round Robin 及其变体
- **Linux context switch 演化史** (https://www.maizure.org/projects/evolution_x86_context_switch_linux/): 从 2.6 到现在的切换实现变化
- **Linux kernel `__switch_to`** (https://blog.codingconfessions.com/p/linux-context-switching-internals): 64 位上下文切换的工业级实现
