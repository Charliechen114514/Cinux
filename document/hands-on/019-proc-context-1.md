---
title: 019-proc-context-1 · 进程上下文
---

# 019-1 进程数据结构与任务构建

## 导语

到 tag 018 为止，我们的内核已经有了独立的地址空间抽象——每个进程可以拥有自己的页表，互不干扰。但"拥有页表"和"能被调度执行"之间还有一道巨大的鸿沟：我们缺一个用来描述"正在运行的任务"的数据结构，缺一套把 CPU 寄存器状态保存和恢复的机制，更缺一个决定"接下来该轮到谁"的调度器。

这一章我们从最基础的地方开始——设计进程控制块（Task Control Block，TCB），定义 CPU 上下文（CpuContext）只保存哪些寄存器、为什么要只保存这些，然后用 Builder 模式把新任务的构造过程封装起来。和之前几篇一样，这篇文章只涉及设计和数据结构定义，不涉及上下文切换的具体实现——那是下一篇的内容。

本篇完成后你将拥有：一个描述线程生命周期状态的枚举、一个 64 字节对齐的寄存器快照结构体、一个包含调度元数据的 TCB、以及一个安全的流式构造器。这些都是后续 context_switch 和调度器的基石。

## 概念精讲

### 为什么 x86-64 不支持硬件任务切换

如果你翻过 Intel SDM Vol.3A Chapter 8，你会看到 Intel 在 32 位保护模式下提供了一套完整的硬件任务切换机制——通过 TSS 描述符和 JMP/CALL 指令让 CPU 自动保存旧任务的寄存器、加载新任务的寄存器，一条指令搞定上下文切换。

听起来很美好，但在 64 位模式下这一切都不存在了。Intel SDM Vol.3A Section 8.7（PDF page 281）明确写道：64 位模式下的任务切换必须由软件完成，如果尝试用 JMP/CALL 到 TSS 描述符来触发硬件切换，处理器会直接给你一个 #GP。

所以我们的路只有一条：自己写汇编保存寄存器、切换栈指针、跳转到新任务——这就是所谓的软件上下文切换。好消息是，x86-64 的 System V ABI 帮我们把该保存哪些寄存器这件事规定得清清楚楚。

值得一提的是，虽然 64 位模式下 TSS 硬件任务切换不可用了，TSS 本身依然有用途——它用于定义 IST（Interrupt Stack Table）条目和 RSP0（内核栈指针），在后续支持用户态进程时会用到。但在 tag 019 中我们暂时不需要 TSS 的这些功能。

### Callee-saved 寄存器——为什么只保存这六个

System V AMD64 ABI 把 x86-64 的通用寄存器分为两组：caller-saved（rax, rcx, rdx, rsi, rdi, r8-r11）和 callee-saved（rbx, rbp, r12-r15）。

caller-saved 寄存器的含义是：函数调用结束后这些寄存器的值可能被破坏，调用者如果需要保留它们的值，必须在调用前自己保存。callee-saved 寄存器正好相反：被调用函数如果使用了这些寄存器，必须在返回前恢复原值，调用者可以安全地假设这些寄存器在调用前后不变。

这个约定和上下文切换有什么关系？关键在于我们的上下文切换总是在一个已知的调用边界上发生——调度器的 yield() 函数被调用，yield() 调用 context_switch()。在 yield() 被 call 指令进入的那一刻，caller-saved 寄存器已经被 yield() 的调用者视为"可能被破坏"的值了——调用者要么已经把需要保留的值保存到了栈上，要么根本不在乎这些值。所以我们只需要保存 callee-saved 的那六个寄存器（rbx, rbp, r12-r15），加上栈指针 rsp 和指令指针 rip，就足以完整地描述一个被暂停的任务。rip 虽然不是 callee-saved（它通过 call/ret 隐式管理），但我们需要显式保存它，因为 context_switch 不是用 ret 返回的——它用 jmp 跳转到新任务。

那 caller-saved 寄存器去哪了？答案是：它们保存在当前线程的栈上。编译器在调用 yield() 之前，如果某个 caller-saved 寄存器的值还有用，编译器已经生成了 push 指令把它保存在栈上了。等这个线程被切回来的时候，栈恢复了，那些值自然也就恢复了。所以 caller-saved 寄存器不是"丢了"，而是由编译器和调用约定自动管理了，不需要我们手动保存。

> 参考：System V AMD64 ABI, Section 3.2.1 — Registers and the Stack Frame
> (https://gitlab.com/x86-psABIs/x86-64-ABI)
> 参考：Intel SDM Vol.3A Section 8.7 — Task Management in 64-Bit Mode

### TaskBuilder——为什么用 Builder 模式

创建一个新任务涉及很多步骤：分配 TCB 内存、分配物理页作为栈、把栈映射到虚拟地址空间、初始化 CPU 上下文、填写任务元数据。这些步骤有严格的先后依赖关系——比如你必须先分配栈页，才能把栈顶地址写入 CpuContext；必须先初始化 CpuContext，才能把任务加入调度队列。

如果把这些全部塞进一个巨大的 Task 构造函数里，参数列表会爆炸（入口地址、名字、优先级、地址空间、调度类……），而且构造函数没法返回错误——它只能抛异常或者在对象里设一个错误标志，两种方式在我们的内核里都不太好用（我们没有 C++ 异常运行时）。

Builder 模式完美解决了这个问题：用一组 setter 方法逐步积累配置参数，最终在 build() 里一次性执行所有分配和初始化步骤，失败时返回 nullptr。调用者拿到的是一个完全就绪的 Task 指针，要么是有效的，要么是空。

### 和 xv6 / Linux 的设计对比

在设计自己的 TCB 之前，我们不妨看看成熟的教学和生产操作系统是怎么做的，这有助于理解 Cinux 设计选择背后的"为什么"。

xv6（MIT 的教学操作系统，32 位 x86）的 `struct proc` 是最经典的参考。它把上下文信息存在 `proc->context` 指针中，指向内核栈底部的一个 `struct context`（包含 edi/esi/ebx/ebp/eip 五个字段）。xv6 的 `swtch.S` 保存 callee-saved 寄存器到 `*old`，从 `new` 加载。Cinux 的做法更直接：`CpuContext` 直接嵌入在 `Task` 结构体的第一个字段，不需要额外的指针间接寻址，而且用 `static_assert` 编译期锁定偏移量——xv6 没有这一层保护，偏移量错了只能运行时才发现。

Linux 的 `task_struct` 是一个庞大的结构体（超过 9 KB），包含文件描述符、信号处理、命名空间等几十个字段。它的上下文信息存在 `task_struct->thread` 中（包含 sp、ip、bp 等字段），调度策略通过 `sched_class` 虚函数指针实现。Cinux 的 `Task` 是 Linux `task_struct` 的极简子集——先实现调度所需的最小字段集，后续功能在对应的 tag 中逐步添加。这个"够用就好"的策略让每个 tag 的 diff 更聚焦、更易理解。

PintOS（Stanford 的教学操作系统）的线程结构体也有类似的精简设计。它有一个值得注意的特点：每个新线程不是直接从用户提供的入口函数开始跑，而是从一个 `kernel_thread()` wrapper 函数开始，由 wrapper 调用用户函数，函数返回后自动调用 `thread_exit()`。Cinux 采用了一种更轻量的等价方案——把退出地址直接压在栈上，而不是用 wrapper 函数。这个设计在第三篇踩坑实录中会详细讨论。

## 动手实现

### Step 1: 定义 TaskState 枚举

**目标**: 在 `kernel/proc/process.hpp` 中定义任务生命周期的状态枚举。

**设计思路**: 任务状态应该覆盖一个任务从创建到消亡的核心阶段——刚创建等待调度的是 Ready，正在 CPU 上跑的是 Running，因等待 I/O 或锁而暂停的是 Blocked，退出后被回收的是 Dead。我们用 scoped enum（`enum class TaskState : uint8_t`）而不是老式的 C enum，好处是类型安全——编译器不会让你把 int 隐式转成 TaskState。底层类型选 uint8_t 而不是默认的 int，因为状态值只有四个，没必要浪费四个字节。

**实现约束**: 枚举值按生命周期阶段排列：Running、Ready、Blocked、Dead。放在命名空间 `cinux::proc` 中。

**扩展思考**: 你可能会问为什么不加一个 Zombie 状态（像 Linux 那样）？Zombie 的作用是让父进程通过 waitpid 获取子进程的退出状态。Cinux 目前还没有实现 waitpid，所以暂时不需要。等后续实现进程回收的时候，在 TaskState 中加入 Zombie 即可——这正是"够用就好"策略的体现。

**验证**: 编译通过即可。

### Step 2: 定义 CpuContext 结构

**目标**: 在同一个头文件中定义 CpuContext 结构体，用于保存上下文切换时的寄存器快照。

**设计思路**: 字段排列顺序必须和 context_switch.S 中使用的偏移量完全一致。我们把 r15 放在偏移 0，r14 在偏移 8，依此类推到 rip 在偏移 56——每字段 8 字节，一共 8 个 callee-saved 相关字段（6 个 GPR 加上 rsp 和 rip）。整个结构体用 `alignas(16)` 对齐，总大小 64 字节。每一个字段我们都用 `static_assert` 验证它在结构体中的偏移量，这样一旦有人改了字段顺序或类型，编译就会直接报错——这种防御性编程在内核里价值极高，因为汇编和 C++ 之间的接口没有任何运行时检查，偏移量错了只会表现为运行时的随机崩溃，调试起来痛苦万分。

**实现约束**: 使用 `struct alignas(16) CpuContext`，所有字段都是 `uint64_t`。必须包含一组 static_assert 检查每个字段的 offset 和总 sizeof。放在命名空间 `cinux::proc` 中。一共 8 个 static_assert：7 个检查偏移量（0, 8, 16, 24, 32, 40, 48, 56），1 个检查总大小等于 64。

**踩坑预警**: 如果你改了字段顺序但忘了同步修改汇编中的偏移量常量，最可能的症状是上下文切换后新任务的 rsp 指向了错误的地址（比如拿到了 rip 的值），CPU 立刻 triple fault。这种 bug 非常隐蔽，因为每个值单独看都是合法的 64 位数字。static_assert 就是为了在编译时捕获这类问题而存在的——千万别省掉它。

**扩展思考**: 为什么不把 rax 等 caller-saved 寄存器也保存了？虽然多保存几个寄存器不会有功能问题（只是浪费一点内存和几条 mov 指令），但这样做没有任何好处——编译器已经把 caller-saved 寄存器保存在栈上了，重复保存只是增加上下文切换的延迟。在上下文切换这种热路径上，每一条指令的延迟都很重要。

**验证**: 编译通过且 static_assert 全部成立。

### Step 3: 定义 Task 控制块

**目标**: 定义完整的 TCB 数据结构，包含调度所需的所有信息。

**设计思路**: Task 结构体是调度器操作的核心数据单元。它包含 CpuContext（上一步定义的寄存器快照）、TaskState（生命周期状态）、tid（单调递增的任务 ID）、priority（优先级，目前未使用，预留给未来的优先级调度）、kernel_stack 和 kernel_stack_top（栈的虚拟地址范围，用于栈溢出检测和 TSS RSP0 设置）、addr_space（进程的独立页表指针，内核线程为 nullptr）、name（人类可读的名字，指向静态字符串，不持有所有权）、sched_class（指向该任务所属的调度策略对象）。当前的字段集是多线程调度的最小集合——后续的文件描述符、进程树、FPU 状态等功能会在对应的 tag 中逐步添加到 Task 中。

注意 CpuContext 被放在 Task 的第一个字段——这意味着 `&task->ctx` 的地址和 `task` 完全相同，在某些场景下可以省去一个偏移计算。xv6 用指针间接访问（`proc->context`），Cinux 用内嵌（`task.ctx`），少一次内存间接寻址。

**实现约束**: 使用 `struct Task`（不用 class），所有成员都是 public 的。Task 不是一个 RAII 类——它的生命周期由 TaskBuilder 创建、调度器管理、Scheduler::exit_current 销毁。放在命名空间 `cinux::proc` 中。

**踩坑预警**: name 字段是一个 `const char*`，它指向的字符串必须有比 Task 对象更长的生命周期。在实践中这意味着你应该用字符串字面量（比如 `"thread_a"`）来调用 set_name——如果你传了一个栈上的局部 char 数组，Task 的 name 指针就会在函数返回后变成悬垂指针，后续的调度器日志打印就会读到垃圾数据，轻则乱码，重则 page fault。

**验证**: 编译通过。

### Step 4: 定义 TaskBuilder 类

**目标**: 定义流式 Builder 类，用于逐步配置并最终构建 Task 对象。

**设计思路**: TaskBuilder 的接口是一组返回 `*this` 引用的 setter 方法，调用者可以链式配置。set_entry 设置线程入口函数指针（必须调用，否则 build() 返回 nullptr），set_name 设置名字（默认 "unnamed"），set_priority 设置优先级（默认 0），set_addr_space 设置地址空间（默认 nullptr，表示纯内核线程），set_sched_class 设置调度策略（默认 nullptr，build 后由 Scheduler 自动填充默认的 RoundRobin）。

核心方法是 build()，它的执行步骤是这样的：第一步从内核堆分配 Task 结构体（使用对齐的 placement new），第二步清零整个结构体，第三步从 PMM 分配连续的物理页作为内核栈（默认 4 页 = 16KB），第四步在虚拟地址空间中映射栈页，第五步在栈底写入一个 magic 值（0xDEADC0DE），第六步初始化 CpuContext——这里有一个非常关键的设计：rsp 指向栈顶减 8 的位置，这个 8 字节里存的是 exit_current 函数的地址。为什么？因为线程入口函数总有一天会 return，而 context_switch 是用 jmp（不是 call）跳到入口的，所以栈上没有返回地址。ret 指令弹出栈顶内容当作返回地址——如果我们不在栈顶放一个合法的地址，ret 就会弹出 0xDEADC0DE 这个 magic 值然后跳过去，CPU 瞬间 triple fault。放 exit_current 的地址就意味着线程函数 return 后自动进入清理流程，完美。

**实现约束**: TaskBuilder 是一个值类型——它持有配置参数但不持有任何资源。setter 方法返回 `TaskBuilder&`，build() 返回 `Task*`（失败返回 nullptr）。STACK_MAGIC 定义为 0xDEADC0DE，STACK_PAGES 定义为 4。build() 中使用 `reinterpret_cast` 将函数指针转换为 uint64_t 存入 CpuContext 的 rip 字段。栈的虚拟地址从一个单调递增的静态变量分配（`0xFFFF800000100000` 起始），避免多个任务的栈在虚拟地址空间中重叠。

**踩坑预警**: TaskBuilder 的 build() 在任何一步失败时都必须回退之前已经分配的资源——TCB 分配成功但栈分配失败时，要 delete 掉 TCB 再返回 nullptr，否则就是内存泄漏。

**验证**: 单独编译 process.hpp 和 process.cpp，确认没有编译错误。

### Step 5: 更新 CMakeLists 并验证编译

**目标**: 把新文件加入构建系统，确认编译通过。

**设计思路**: kernel/CMakeLists.txt 需要添加 `kernel/proc/process.cpp` 到 big_kernel_common 的源文件列表。proc 目录是新创建的，后续的 scheduler.cpp 和 context_switch.S 也会放在这个目录下。注意 scheduler.cpp 和 context_switch.S 的编译依赖 process.hpp，所以 process.hpp 需要先完成。

**验证**: 编译内核，确认新增的文件被正确编译和链接。

### Step 5: 更新 CMakeLists 并验证编译

**目标**: 把新文件加入构建系统，确认编译通过。

**设计思路**: kernel/CMakeLists.txt 需要添加 `kernel/proc/process.cpp` 到 big_kernel_common 的源文件列表。proc 目录是新创建的，后续的 scheduler.cpp 和 context_switch.S 也会放在这个目录下。

**验证**: 编译内核，确认新增的文件被正确编译和链接。

如果这一步出现编译错误，最常见的三个原因依次是：(1) 头文件路径不对——确认 process.hpp 中的 include 路径和实际文件位置匹配；(2) CpuContext 的 static_assert 失败——回到 Step 2 检查字段排列；(3) 命名空间 cinux::proc 没有正确闭合——检查花括号匹配。

```bash
cmake --build build --target big_kernel 2>&1 | tail -20
```

## 构建与运行

到这一步，我们还没有调度器来运行任何任务——TaskBuilder 创建的 Task 对象只是安静地躺在堆上，等待被注册到调度队列。

所以验证方式就是编译通过、没有链接错误。如果你在 main.cpp 中随手创建一个 TaskBuilder 但不调用 build()，它就是一个普通的栈对象，离开作用域时自动析构，不会产生任何副作用。

你也可以尝试构建一个简单的验证：在 kernel_main 中创建一个 TaskBuilder 并调用 build()，检查返回值是否非空，然后打印 task 的 tid 和 name。注意不要真的去运行这个 task（调度器还没实现），只是验证 TCB 的分配和初始化是否正确。验证完毕后 delete 掉这个 Task 即可。

如果你想验证 CpuContext 的布局正确性，可以在 process.hpp 的 static_assert 之后加一个简单的编译检查——比如声明一个 `static_assert(sizeof(Task) >= sizeof(CpuContext))` 来确保 Task 至少包含完整的 CpuContext。不过这个检查不是强制的，因为 static_assert 已经覆盖了每个字段的偏移量。

在下一篇中我们会实现 context_switch.S 汇编原语和 RoundRobin 调度器，让这些 Task 对象真正地被调度执行。

## 设计分析

### CpuContext 为什么用 static_assert 而不直接用数组

用命名字段 + static_assert 是一种"声明即文档、编译即校验"的做法。如果用 `uint64_t regs[8]` 加宏定义偏移量（`#define RSP_OFFSET 48`），可读性会大打折扣——看到 `ctx.regs[6]` 你还得去查宏定义才知道这是 rsp。而且很容易在新增字段时忘记更新宏。命名字段本身就是文档，static_assert 保证了 C++ 和汇编之间的偏移量永远一致。

### 栈为什么用魔数而不是 guard page

STACK_MAGIC 是一个轻量级的溢出检测手段——写在栈底，检查是否被覆盖就能判断栈溢出。guard page 更强（硬件辅助，访问就触发 #PF），但需要额外的虚拟地址空间和页表操作。当前的魔数方案已经能覆盖最常见的栈溢出场景。后续如果需要更严格的保护，可以在栈底下方预留一页未映射的 guard page。

值得注意的是，0xDEADC0DE 这个魔数的选择不是随意的——它是一个明显无效的地址（在低位地址空间中，通常不映射），如果 RIP 跳到这里很容易在调试器中识别出来。这也是一种"fail loudly"的设计思想。

### 为什么 Task 不用 RAII

Task 的生命周期比一个作用域更复杂——它被创建后加入调度队列，可能被多个 CPU（未来 SMP）引用，最终在线程退出时被销毁。这种生命周期用 RAII 的构造/析构很难表达（unique_ptr 不行因为所有权是共享/转移的，shared_ptr 需要引用计数开销）。所以 Cinux 选择了手动管理——TaskBuilder 创建、调度器持有、exit_current 销毁。Linux 的 task_struct 也是同样的手动管理策略。

### 栈的虚拟地址分配策略

`alloc_stack_vaddr` 从 `0xFFFF800000100000` 起始单调递增地分配栈的虚拟地址。为什么选这个地址？因为 `0xFFFF800000000000` 是 canonical 地址空间的高半部分起始点（基于 Intel SDM 中 48 位虚拟地址的约定），我们跳过前 1MB 避免和可能的 MMIO 区域冲突。每个线程栈占 4 页（16KB），所以连续分配多个线程的栈不会重叠。

当然这个简化实现在虚拟地址空间耗尽前不会出问题，但如果未来需要回收线程栈的虚拟地址空间（线程销毁后释放），需要改成更完善的分配器。对于当前只有两三个线程的教学内核来说，单调递增完全够用。

## 调试技巧

**build() 返回 nullptr**: 最可能的原因是 entry 没有设置——TaskBuilder 要求必须调用 set_entry。第二个可能是 PMM 物理页耗尽，检查 g_pmm 的可用页数。第三个可能是 VMM map 失败（虚拟地址空间耗尽），这种情况在正常使用中不太可能发生。

**static_assert 编译失败**: 说明 CpuContext 的字段排列或大小和预期不一致。最常见的错误是漏了一个字段或者字段类型不是 uint64_t——每一项都必须是 8 字节，偏移量才能正确递增。如果 static_assert 报告 sizeof(CpuContext) 不是 64，说明你可能多加了字段或者用了错误的对齐。

**链接错误 "undefined reference to context_switch"**: 这是正常的——context_switch 的声明在 process.hpp 中，但实现在下一篇才添加的 context_switch.S 中。如果你想在两篇之间测试编译，可以暂时写一个空的 stub 函数。

**编译错误 "offsetof requires standard layout"**: 如果 CpuContext 不是标准布局类型（比如加了 virtual 方法），offsetof 会编译失败。确保 CpuContext 是一个纯 POD 结构体——只有数据成员，没有虚函数，没有继承。

**栈映射失败**: 如果 build() 在第四步（映射栈页）失败，最可能的原因是 walk_level 遇到了一个 2MB 大页但不知道怎么拆分——这个 bug 会在第三篇中修复。如果在你编译的时候大页拆分逻辑已经存在，这个错误不应该发生。

**placement new 返回的指针需要对齐检查**: 如果内核堆的分配器没有正确处理 `alignof(Task)` 对齐，placement new 返回的指针可能不是对齐的。这在 higher-half 内核中一般不是问题，因为内核堆从虚拟地址空间分配的内存通常已经是页对齐的。

**name 指针在 GDB 中显示乱码**: 如果你用 GDB 调试时发现 task->name 指向的内容是乱码，检查 name 是否指向了一个栈上的局部变量。正确的做法是用字符串字面量——字符串字面量存储在二进制的 .rodata 段中，生命周期和内核一样长。

**tid 从 1 而不是 0 开始**: 这是设计上的选择——tid=0 被预留给 boot_task（启动上下文），所以用户创建的第一个任务 tid=1。在 kernel_main 中手动构造的 boot_task 会设 `tid = 0`。

## 本章小结

这一篇我们把进程管理的地基打好了：TaskState 枚举描述了任务的生命周期，CpuContext 精确地保存了上下文切换所需的 6 个 callee-saved 寄存器（r15-r12, rbp, rbx）加上 rsp 和 rip，Task 结构体把所有调度相关的信息打包成一个完整的 TCB，TaskBuilder 用流式接口封装了 TCB 分配、栈映射、上下文初始化的全部流程。

回顾一下整体设计的层次关系：最底层是 CpuContext——64 字节的寄存器快照，是上下文切换的原子操作对象；中间层是 Task——包含 CpuContext 加上调度状态、内存信息的 TCB；最上层是 TaskBuilder——封装了从 TCB 分配到栈映射到上下文初始化的全部细节。三层各司其职，互不耦合：修改 CpuContext 只影响上下文切换的汇编代码；修改 Task 只影响调度器和进程管理；修改 TaskBuilder 只影响线程创建流程。

但这些东西本身不会让线程跑起来——真正让两个线程交替执行的，是下一篇要讲的 context_switch.S 汇编原语和 RoundRobin 调度器。而真正让开发过程跌宕起伏的，是第三篇要讲的那些让内核崩掉的 bug。准备好了吗？

下一篇我们要干一件更刺激的事——用汇编实现真正的上下文切换，让两个 Task 对象在 CPU 上轮流执行。

## 延伸阅读

- **Intel SDM Vol.3A Chapter 8**: 完整的任务管理机制描述，包括为什么 64 位模式废弃了硬件切换
- **System V AMD64 ABI Section 3.2**: callee-saved 和 caller-saved 寄存器的完整定义
- **xv6 `proc.h` / `swtch.S`** (https://github.com/mit-pdos/xv6-public): 对比 32 位教学操作系统的 TCB 和上下文切换实现
- **Linux `task_struct`** (include/linux/sched.h): 工业级 TCB 的完整定义，理解"够用就好"的反面是什么样的
- **PintOS `switch.S`** (https://uchicago-cs.github.io/mpcs52030/switch.html): 线程退出封装的替代方案参考
