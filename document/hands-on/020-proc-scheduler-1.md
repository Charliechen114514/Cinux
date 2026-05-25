---
title: 020-proc-scheduler-1 · 进程调度
---

# 020-1 Spinlock 与 PerCPU 数据结构

## 导语

到 tag 019 为止，我们的调度器只支持协作式多任务——线程必须主动调用 `yield()` 才能让出 CPU。这在教学演示里勉强够用，但在真实的操作系统里，一个死循环的线程就能霸占整个 CPU，其他任务永远得不到执行机会。要解决这个问题，我们需要让定时器中断来"强制"收回 CPU 的控制权，这就从"协作式"跨入了"抢占式"调度。

但抢占式调度立刻带来了一个新问题：中断处理程序和普通线程代码可能在同一段数据上产生竞争。比如定时器中断把当前任务从 Running 改成 Ready，而此时另一个地方正在读取这个任务的状态——如果这两个操作不是原子的，状态就可能被撕成半新半旧的碎片。所以我们在动手改调度器之前，先把最基本的同步原语——Spinlock——准备好，再搭建一个 PerCPU 数据结构来管理"当前正在哪个 CPU 上跑什么任务"的信息。这篇文章的全部内容都是打地基，下一篇才会真正实现抢占式调度。

从协作式到抢占式的转变，本质上是调度器的调用上下文发生了变化。tag 019 的 `yield()` 是在普通函数调用中被触发的——完整的调用链是 `thread_a() -> yield() -> pick_next() -> context_switch()`，执行在同一个栈帧上，RFLAGS 中的 IF 位保持不变，不存在中断干扰。但 tag 020 的 `tick()` 是在定时器中断处理程序中被调用的——调用链是 `IRQ0 -> ISR stub -> irq0_handler() -> tick() -> schedule() -> context_switch()`，进入 ISR 时 CPU 自动清除 IF 位（中断门语义），整个调度过程在中断关闭的状态下执行。这个调用上下文的改变引入了新的正确性约束，而旧的代码（包括 `context_switch.S`）是在没有这些约束的情况下设计的。理解这个根本性的差异，是读懂后续三篇文章的关键。

## 概念精讲

### Spinlock——自旋锁为什么用 test_and_set + pause

自旋锁的核心思想非常简单：用一块共享内存（一个 bool 变量）来表示锁的状态——false 表示空闲，true 表示被占。线程获取锁时，尝试把 false 原子地换成 true；如果发现已经是 true 了，就原地转圈等待，直到另一个线程把锁释放。听起来像是在门口不停地敲门问"好了没"——确实很粗暴，但在内核里对短临界区来说它是最快的同步方案。

这里的关键词是"原子"。如果读和写不是原子的，两个线程可能同时读到 false、同时认为锁是空闲的、同时拿到了锁——锁就废了。GCC 提供的 `__atomic_test_and_set` 内建函数就是做这件事的：它把目标内存位置设置为 true，同时返回设置之前的旧值。如果旧值是 false，说明我们抢到了；如果旧值是 true，说明别人正拿着，我们继续循环。这个内建函数最终会被编译成 x86 的 `xchg` 或者 `lock bts` 指令，硬件层面保证原子性——x86 的 LOCK 前缀配合这些指令可以让缓存行的操作变成不可分割的原子事务。

自旋等待中我们插入了一条 `pause` 指令。这条指令在 Intel SDM Vol.2B 的指令参考中有说明——它是一个提示性质的指令，告诉 CPU"我现在在自旋等待"，CPU 可以据此降低流水线功耗、减少内存顺序违规检测的惩罚。在超线程场景下，`pause` 还能避免自旋线程占满执行单元导致同核心的另一个硬件线程饿死。总之，自旋循环里加 `pause` 是 x86 上的标准做法，不加虽然也能跑，但在密集竞争时性能会明显下降。说实话，在我们的教学内核里性能差异可以忽略不计，但好习惯要尽早养成。

释放锁时使用 `__atomic_clear`，配合 `__ATOMIC_RELEASE` 语义——这确保了锁持有期间的所有写操作对下一个获取锁的线程可见。acquire 使用 `__ATOMIC_ACQUIRE` 语义，和 release 配对形成完整的 happens-before 关系。如果你对 acquire/release 语义不太熟悉，可以简单理解为：acquire 保证"拿到锁之后能看到锁持有者做过的所有写操作"，release 保证"释放锁之前我的所有写操作对下一个获取者可见"。

更精确地说，acquire/release 的配对在两个线程之间建立了一个 "synchronizes-with" 关系：线程 A 的 release 操作（`__atomic_clear`）和线程 B 的 acquire 操作（`__atomic_test_and_set` 成功）之间，所有在 A 的 release 之前发生的内存写操作，在 B 的 acquire 之后都保证可见。这不是说数据立刻从 A 的 cache 传输到 B 的 cache——x86 的 TSO（Total Store Order）内存模型已经保证了这一点。acquire/release 的真正价值在于提供了跨架构的可移植性保证，以及防止编译器做不利于正确性的指令重排。

这里有一个值得思考的问题：为什么不用 C++11 的 `std::atomic<bool>`？答案是内核环境——我们根本没有 C++ 标准库，更不可能依赖 libstdc++ 的原子实现。直接使用 GCC 内建函数是内核开发中最常见的做法，Linux 内核的 `atomic_t` 也是基于类似的编译器内建操作实现的。`__atomic_test_and_set` 最终会被编译成 x86 的 `xchg` 或者 `lock bts` 指令，而 `__atomic_clear` 会被编译成普通的 `mov` 指令（x86 的 TSO 内存模型保证了 store 的原子可见性）。这些底层细节我们在代码层面不需要关心，但理解它们有助于在排查内存可见性问题时做出正确的判断。

参考：

- Intel SDM Vol.3A Section 8.1 — Locked Atomic Operations
- Intel SDM Vol.2B — PAUSE instruction reference

### RAII Guard——异常安全的锁管理模式

Spinlock 的 acquire 和 release 必须严格配对——每获取一次就一定要释放一次。听起来简单，但代码一旦有了分支、提前 return 或者异常（虽然我们没有 C++ 异常，但内核代码里的提前 return 可太常见了），漏掉 release 就会导致死锁。RAII（Resource Acquisition Is Initialization）模式完美解决了这个问题：在 Guard 对象的构造函数里 acquire，析构函数里 release。无论函数怎么退出——正常 return、提前 return 还是其他控制流——Guard 的析构函数都会被调用，锁一定会被释放。

`[[nodiscard]]` 属性确保你不会忘记把 guard() 的返回值赋给一个变量——如果你写了 `spin.guard();` 而没有保存返回值，编译器会报警告。原因是临时对象在语句结束时立刻析构，锁又被释放了，等于没加锁。这种错误在代码审查中很不容易发现——锁看起来加了，实际上一微秒后就放开了。

Guard 的拷贝构造和赋值都被 delete 了。这是必须的——如果允许拷贝，两个 Guard 对象会指向同一把锁，第一个析构时释放，第二个析构时又释放，release 一个已经释放的锁，后续的 acquire/release 语义就全乱了。move 构造其实可以做，但目前的需求场景用不到，为了简单起见也一并 delete 了。

Guard 的典型使用方式是 `auto g = lock.guard();`——变量 `g` 在当前作用域结束时自动析构释放锁。如果你需要在某个特定的点提前释放锁，可以用额外的花括号块来控制生命周期：`{ auto g = lock.guard(); /* critical section */ }`，右花括号处 Guard 析构锁被释放。这种模式比手动调用 acquire/release 安全得多——即使 critical section 中间有 `return`、`break` 或 `continue`，锁都不会被遗忘。

### PerCPU 数据结构——为 SMP 预留的占位

在单核系统里，"当前正在运行的任务"就是一个简单的全局指针。但在多核（SMP）系统中，每个 CPU 核心都有自己的当前任务，所以 Linux 和大多数操作系统会用 per-CPU 数据区——一块每个 CPU 核心都有独立副本的内存区域，通过 GS（x86-64）或 TPIDR_EL1（ARM64）段寄存器来寻址。

我们目前的 PerCPU 结构体只有两个字段：`current` 指向当前任务，`kernel_stack` 保存当前任务的内核栈顶地址。单核场景下用一个简单的全局变量 `g_per_cpu` 就够了，等以后支持 SMP 时再替换成真正的 per-CPU 段。这个占位设计的妙处在于：调度器代码中对 `g_per_cpu.current` 的读写在未来不需要改——只要把 g_per_cpu 从全局变量变成通过 GS 寄存器寻址的 per-CPU 变量，所有使用点自动变成"访问本 CPU 的数据"。

一个可能让你困惑的问题：既然我们已经有 `Scheduler::current_` 静态成员来追踪当前任务，为什么还需要 `g_per_cpu.current`？答案是分层。`Scheduler::current_` 是调度器的内部状态——只有调度器自己会读写。而 `g_per_cpu.current` 是一个全局可见的、per-CPU 的"当前任务"指针——中断处理程序、系统调用入口、未来 SMP 初始化代码都可能需要快速知道"当前 CPU 上跑的是谁"。把这两个概念分开，调度器内部用 `current_` 做调度决策，外部通过 `g_per_cpu.current` 做快速查询，职责更清晰。

Linux 的做法是 `DEFINE_PER_CPU(type, name)` 宏来声明 per-CPU 变量，运行时通过 `this_cpu_read/write` 系列宏来访问，底层用 GS 段寄存器做偏移。我们现在只取其意而不取其形——用最简单的全局变量来模拟，等真正需要 SMP 的时候再引入宏和段寄存器寻址。

### TSS.RSP0——为什么每次切换都要更新

x86-64 的 Task-State Segment（TSS）里有一个 RSP0 字段——当 CPU 从 ring 3（用户态）切换到 ring 0（内核态）时，处理器会自动把 RSP 加载为 TSS.RSP0 的值。Intel SDM Vol.3A Section 8.7 明确说明，64 位模式下 TSS 的主要用途就是这个——不再是硬件任务切换，而是提供特权级切换时的栈指针。

虽然我们目前还没有用户态任务，但提前在每次上下文切换时更新 RSP0 是一个好习惯：一旦未来添加了用户态支持，你不需要回头改调度器——RSP0 已经在每个上下文切换中被正确维护了。如果你忘记更新，用户态任务触发系统调用时 CPU 加载的就是上一个任务的内核栈——栈指针错误，写入数据覆盖了别人的栈帧，内核的崩溃几乎不可避免。更糟糕的是这种 bug 只在用户态任务出现后才会暴露，到时候你可能完全想不到是调度器的问题。

TSS 中除了 RSP0-RSP2，还有 IST1-IST7（Interrupt Stack Table）字段。IST 提供了一种独立于 RSP0 的中断栈机制——当 IDT entry 中配置了 IST 索引时，CPU 在处理中断时会切换到 IST 指定的栈，而不是 RSP0。Cinux 用 IST1 作为 Double Fault 的专用栈（在 GDT 初始化时通过 `tss_.ist[0] = df_stack_top` 设置），因为 Double Fault 发生时内核栈可能已经损坏，不能信任 RSP0 指向的栈。普通中断（包括 IRQ0）不使用 IST，走的是 RSP0 路径。

我们在 GDT 类中新增一个 `tss_set_rsp0()` 静态方法，直接写入全局 GDT 结构体中的 TSS RSP0 字段。这个方法在 schedule()、exit_current() 和 run_first() 三个地方被调用——正好是所有会执行 context_switch 的路径。

参考：

- Intel SDM Vol.3A Section 2.1.3 — Task-State Segments
- Intel SDM Vol.3A Section 8.7 — Task Management in 64-Bit Mode

## 动手实现

### 文件变更概览

在开始逐步实现之前，先列出这篇文章涉及的文件变更：

| 文件 | 操作 | 说明 |
|------|------|------|
| `kernel/proc/sync.hpp` | 新增 | Spinlock 类 + RAII Guard，header-only |
| `kernel/proc/per_cpu.hpp` | 新增 | PerCPU 结构体 + extern 声明，16 行 |
| `kernel/arch/x86_64/gdt.hpp` | 修改 | 新增 `static void tss_set_rsp0(uint64_t)` |
| `kernel/arch/x86_64/gdt.cpp` | 修改 | 新增 tss_set_rsp0 实现，1 行赋值 |
| `kernel/proc/scheduler.hpp` | 修改 | 新增 5 个静态成员声明 + 4 个新方法 |
| `kernel/proc/scheduler.cpp` | 修改 | 新增 g_per_cpu 定义 + 4 个静态成员定义 |

新增的两个头文件（sync.hpp 和 per_cpu.hpp）都是 header-only 的，不引入新的编译单元。修改的三个文件（gdt.hpp/cpp、scheduler.hpp/cpp）都是在现有文件中追加代码。这个文件布局反映了 tag 020 的设计哲学——基础设施和核心逻辑分开放：sync.hpp 和 per_cpu.hpp 是独立的工具组件，gdt 的修改是架构相关的适配层，scheduler 的修改才是真正的调度逻辑。

### Step 1: 创建 Spinlock 类

**目标**: 在 `kernel/proc/sync.hpp` 中实现自旋锁，包含 acquire、release 和 RAII Guard。

**设计思路**: 我们需要三个核心操作。acquire 使用 `__atomic_test_and_set` 配合 `__ATOMIC_ACQUIRE` 语义进行原子测试并设置，失败时执行 `pause` 指令降低功耗后重试。release 使用 `__atomic_clear` 配合 `__ATOMIC_RELEASE` 语义清零锁变量。Guard 是一个嵌套类，构造时 acquire、析构时 release，拷贝构造和赋值都被 delete，防止同一个 Guard 被多次释放。

Spinlock 的底层存储是一个 `volatile bool locked_`，初始值为 false。volatile 告诉编译器不要对这个变量做优化（比如缓存到寄存器里），每次读写都必须访问内存——这在自旋锁中是必要的，因为锁的状态可能被另一个 CPU 核心随时改变。不过说实话，`__atomic_test_and_set` 本身已经隐含了 volatile 语义，这里加 volatile 更多是代码意图的表达——明确告诉读者"这个变量会被外部修改"。

Spinlock 的设计有一个隐含的前提：临界区必须足够短。自旋锁是一种"忙等"（busy-wait）锁——拿不到锁的线程不会睡眠，而是不停地循环检查锁的状态，浪费 CPU 周期。如果临界区很长（比如包含 I/O 操作或长时间的计算），自旋等待的线程就会白白消耗大量 CPU 时间。在单核环境中这个问题更严重——持有锁的线程需要 CPU 时间来执行临界区代码并释放锁，但自旋等待的线程霸占了 CPU，持有者永远得不到执行机会，系统直接死锁。所以 Spinlock 只适合保护"几条指令到几十条指令"的短临界区。对于长临界区，需要使用基于 block/unblock 的 Mutex（tag 021 的内容）。

**实现约束**: 整个类是 header-only 的，所有方法都在类定义中内联。不使用任何外部库——纯靠 GCC 内建原子操作和内联汇编。命名空间 `cinux::proc`。类不需要构造函数参数——默认构造即可，locked_ 的就地初始化器会处理初始状态。

**踩坑预警**: 千万不要在持有 Spinlock 的情况下调用 block() 或 yield()——自旋锁的设计前提是临界区极短（几条指令到几十条指令），持有者绝不能让出 CPU。如果你在锁持有期间触发了上下文切换，其他线程尝试获取同一把锁时会永远自旋下去——因为持有锁的线程没在跑了，锁永远不会被释放。这就是经典的自旋锁死锁场景，一旦发生系统直接卡死，连日志都打不出来。这个约束在后续 021 的 Mutex 实现中会更加重要——Mutex 内部会先释放 Spinlock 再调用 block()，顺序绝对不能颠倒。

**验证**: 编译通过，确认没有链接错误。

### Step 2: 定义 PerCPU 结构体

**目标**: 在 `kernel/proc/per_cpu.hpp` 中定义 per-CPU 数据占位结构。

**设计思路**: PerCPU 结构体包含两个字段：`Task* current` 指向当前正在该 CPU 上运行的任务，`uint64_t kernel_stack` 保存当前任务的内核栈顶地址。声明一个全局实例 `extern PerCPU g_per_cpu`，定义在 scheduler.cpp 中，初始化为 `{nullptr, 0}`。

结构体用 `struct` 而不是 `class`——所有成员默认 public，因为我们希望调度器能直接读写这些字段。PerCPU 的设计理念是"数据不属于任何类"，它是一块被多个子系统共享的状态——调度器写 current，中断处理程序读 current，未来 SMP 初始化代码设置 GS 基址指向对应的 per-CPU 区域。这种全局共享数据的设计在操作系统内核中非常常见，虽然面向对象的纯粹主义者可能会皱眉，但在内核里"简单直接"永远比"封装优雅"更值钱。

**实现约束**: 前向声明 `struct Task` 即可，不需要包含完整的 Task 定义——这减少了头文件依赖，避免了 include 循环。命名空间 `cinux::proc`。整个头文件不超过 16 行。

**踩坑预警**: 如果你把 PerCPU 的 `extern` 声明放在了 `.cpp` 文件中而不是头文件中，其他翻译单元就看不到 `g_per_cpu`——链接器会报 "undefined reference"。`extern` 声明必须放在头文件中，定义（不带 extern）放在恰好一个 `.cpp` 文件中。这是 C++ 编译模型的基本规则，但在内核项目中头文件依赖关系比较复杂，容易搞混。

**验证**: 编译通过。可以加一个简单的测试：在某个初始化函数中创建 Spinlock，调用 acquire() 后检查 `locked_` 为 true，调用 release() 后检查 `locked_` 为 false，再验证 guard() 的 RAII 语义——在作用域内 locked_ 为 true，退出作用域后为 false。

### Step 3: 在 GDT 中添加 tss_set_rsp0 方法

**目标**: 在 `kernel/arch/x86_64/gdt.hpp` 和 `gdt.cpp` 中添加静态方法来更新 TSS 的 RSP0 字段。

**设计思路**: TSS 在 GDT 中被定义为一个包含 RSP0-RSP2 和 IST1-IST7 的结构。我们的 GDT 类已经有一个 `g_gdt` 静态成员来持有整个 GDT 布局，其中 `tss_` 就是那个 TSS 结构。`tss_set_rsp0()` 直接把传入的值写入 `g_gdt.tss_.rsp[0]`——注意 RSP0 是 `rsp` 数组的第一个元素（索引 0）。

方法声明为 static，因为它操作的是全局 GDT，不需要实例。调用者传入的值是新任务的 `kernel_stack_top`，即任务内核栈的最高地址（栈是从高地址向低地址增长的，所以栈顶是高地址那一端）。

**实现约束**: 在 gdt.hpp 的 public 区域声明 `static void tss_set_rsp0(uint64_t rsp0)`，放在 init() 方法之后。在 gdt.cpp 中实现，函数体只有一行赋值语句。不需要重载 GDT 或执行 `lgdt`——TSS 的 RSP0 字段在下次 ring 3 到 ring 0 切换时会被 CPU 自动读取，不需要 CPU 侧的任何刷新操作。

**踩坑预警**: TSS 中 `rsp` 是一个包含 3 个 `uint64_t` 的数组，对应 RSP0、RSP1、RSP2。我们只使用 RSP0（索引 0），对应 ring 3 到 ring 0 的特权级切换。RSP1 和 RSP2 在 64 位模式下不使用——x86_64 实际上只用了 ring 0 和 ring 3 两个特权级。如果你不小心写成了 `rsp[1]` 或 `rsp[2]`，编译不会报错但功能完全不对，而且这种 bug 只在用户态任务出现后才会暴露。

**验证**: 编译通过。

### Step 4: 在 scheduler.cpp 中定义 g_per_cpu 并更新静态成员

**目标**: 补充 scheduler.cpp 中新增的静态成员定义和 PerCPU 全局实例。

**设计思路**: Scheduler 类在 tag 020 中新增了几个静态成员：`idle_task_`（空闲任务指针）、`initialized_`（是否已完成初始化）、`tick_count_`（全局 tick 计数器）、`current_slice_`（当前任务已使用的 tick 数）。这些都需要在 scheduler.cpp 的命名空间作用域中定义并初始化——C++ 的静态成员变量必须在类外有一份定义，否则链接器报 "undefined reference"。

同时需要定义 `PerCPU g_per_cpu{nullptr, 0}` 并 include 新增的 `per_cpu.hpp` 和 `gdt.hpp` 头文件。include 的顺序是先 arch 头文件再 proc 头文件——这和 kernel_main.cpp 中的 include 顺序保持一致，虽然 C++ 的 include 顺序不影响语义，但在大项目中保持统一的顺序能减少 merge conflict。

**实现约束**: 静态成员的初始值分别是：idle_task_ = nullptr, initialized_ = false, tick_count_ = 0, current_slice_ = 0。g_per_cpu 的初始化放在 PerCPU 定义之后、Scheduler 静态成员定义之前。头文件 include 顺序为：`kernel/arch/x86_64/gdt.hpp`、`kernel/lib/kprintf.hpp`、`kernel/proc/per_cpu.hpp`。

这里补充一点关于 C++ 静态成员链接规则的知识。在 C++ 中，类的静态成员变量只是声明（在头文件中），必须在恰好一个翻译单元（.cpp 文件）中提供定义。如果忘记定义，链接器会报 "undefined reference" 错误——编译能过，但链接会失败。如果在不只一个 .cpp 文件中定义，链接器会报 "multiple definition" 错误。Scheduler 的所有静态成员都集中在 scheduler.cpp 中定义，这是最干净的做法。

**验证**: 编译通过。可以用 `nm build/big_kernel | grep Scheduler` 检查静态成员符号是否在 BSS 段或 DATA 段中正确出现。

一个常见的错误是把静态成员的定义放在了头文件中——这会导致 "multiple definition" 链接错误，因为每个 include 该头文件的编译单元都会生成一份定义。确保所有 `Scheduler::xxx = ...` 的定义只出现在 scheduler.cpp 中。

### Step 5: 编译验证

**目标**: 确认所有新增文件和修改能正确编译。

```bash
cmake --build build --target big_kernel 2>&1 | tail -20
```

预期输出中不应有编译错误或链接错误。新增的 sync.hpp 和 per_cpu.hpp 是 header-only 的，不会增加编译单元。gdt.cpp 新增了一行函数实现，scheduler.cpp 新增了静态成员定义。

如果编译报错说找不到 sync.hpp 或 per_cpu.hpp，检查文件是否放在了 `kernel/proc/` 目录下，以及 CMakeLists.txt 的 include path 中是否包含 `kernel/` 目录——通常我们用 `#include "kernel/proc/sync.hpp"` 这种从项目根目录开始的路径来引用，而不是相对路径。

如果链接阶段报 "multiple definition of cinux::proc::g_per_cpu"，说明你不小心在多个 .cpp 文件中定义了 g_per_cpu。确保只有 scheduler.cpp 中有一行 `PerCPU g_per_cpu{nullptr, 0}`，其他文件通过 include `per_cpu.hpp` 中的 `extern PerCPU g_per_cpu` 来使用。

如果编译通过但运行时行为异常（比如 Spinlock 的 acquire 立刻返回，Guard 析构后 locked_ 仍为 true），检查 Spinlock 对象的生命周期。如果 Spinlock 是局部变量且在函数返回后被销毁，那么其他持有该锁指针的 Guard 对象就会指向已释放的内存——这是经典的 use-after-free。在内核中，Spinlock 通常应该声明为全局变量、静态成员或嵌入到长生命周期的结构体中（比如 RoundRobin 类中的 `lock_` 成员）。

## 构建与运行

到这一步我们还没有实现抢占式调度的任何核心逻辑——Spinlock 和 PerCPU 只是工具和基础设施。内核的行为和 tag 019 完全一样：协作式多任务、手动 yield、两线程交替打印。所以运行结果应该和 tag 019 一模一样，如果你看到什么不同，说明在迁移过程中不小心改动了不该改的东西。

如果你迫不及待想验证 Spinlock 是否能正常工作，可以在任意内核函数中创建一个 Spinlock，获取它、释放它，打印前后的 locked_ 值——不过说实话这个验证意义不大，因为单核环境下没有竞争条件，Spinlock 的 acquire 第一次就会成功。真正的考验要等后面有了中断驱动的抢占式调度，再加上 021 的 Mutex 和 Semaphore 才有意义。

一个更有效的验证方式是检查编译产物中的符号。用 `nm build/big_kernel | grep -E "g_per_cpu|tss_set_rsp0"` 确认 PerCPU 全局变量和 tss_set_rsp0 函数都正确地出现在符号表中。如果你看不到这些符号，说明对应的编译单元没有被链接进来——检查 CMakeLists.txt 中是否包含了新增的源文件。

另外，确认 scheduler.cpp 中新增的静态成员（idle_task_、initialized_、tick_count_、current_slice_）也都出现在 BSS 段中——它们应该被零初始化。如果出现在 DATA 段也没问题（取决于编译器的选择），但如果完全找不到符号，那就是忘记定义了。

## 调试技巧

**编译报错 "volatile bool is not a pointer type"**: 可能是你在 `__atomic_test_and_set` 中传了错误的参数类型。这个内建函数的第一个参数应该是指向 volatile bool 的指针——直接传 `&locked_` 即可。如果你不小心写了 `__atomic_test_and_set(locked_, ...)` 少了取地址符，编译器就会报这个错。

**Guard 对象的析构顺序错误**: C++ 的局部对象按声明的逆序析构。如果你在一个函数里获取了多把锁，它们的 Guard 对象会按"最后声明的先释放"的顺序析构——这就是 LIFO 锁释放语义。大多数情况下这正是你想要的，但如果你需要特定的释放顺序，可以用额外的花括号块来控制 Guard 的生命周期范围。

**链接错误 "undefined reference to cinux::proc::g_per_cpu"**: 说明 g_per_cpu 只有 extern 声明（在 per_cpu.hpp 中）但没有定义（在某个 .cpp 文件中）。确认 scheduler.cpp 中有 `PerCPU g_per_cpu{nullptr, 0};`。

**tss_set_rsp0 写入了错误的位置**: 如果你发现用户态系统调用时内核栈指针不对，检查 TSS 的 rsp 数组索引是否为 0。RSP0 对应索引 0，RSP1 对应索引 1，RSP2 对应索引 2。在 64 位模式下只有 RSP0 有实际意义（ring 3 到 ring 0 切换），RSP1 和 RSP2 在 64 位下不使用。

**Spinlock 在 release 后其他 CPU 仍然自旋**: 这是内存可见性问题。确认 release 使用了 `__ATOMIC_RELEASE` 语义—— bare write 虽然在 x86 的 TSO 内存模型下通常也能工作，但标准要求使用 release 语义才能保证跨架构的可移植性。

**编译警告 "missing field initializer"**: 如果编译器对 `PerCPU g_per_cpu{nullptr, 0}` 报警告，可能是因为你用了 `-Wmissing-field-initializers`。可以用 `PerCPU g_per_cpu = {}` 来做全零初始化，效果一样但不会触发这个警告。不过在我们的项目里这个警告级别通常是被关闭的。

**spinlock 的 pause 指令被优化掉**: 如果你发现自旋循环里没有 `pause`，检查 `__asm__ volatile("pause")` 中是否遗漏了 `volatile`。没有 `volatile` 的内联汇编可能被编译器优化掉或重排，这对自旋循环的正确性影响不大（因为 `__atomic_test_and_set` 本身已经提供了内存屏障），但会影响 CPU 功耗和超线程公平性。

**PerCPU 的 g_per_cpu.current 和 Scheduler::current() 不一致**: 这两个值在每次 `schedule()` 中都会被同步更新（`current_ = next; g_per_cpu.current = next;`）。如果你发现它们不一致，说明某处代码绕过了 `schedule()` 直接修改了其中一个——检查是否有代码直接赋值 `current_` 而没有同时更新 `g_per_cpu.current`。在当前的单核设计中，所有修改 `current_` 的地方都应该同时修改 `g_per_cpu.current`。

**Spinlock 反复 acquire 成功但没有 release**: 如果你在调试输出中看到 Spinlock 被 acquire 了多次但 release 次数不匹配，最可能的原因是 Guard 对象的生命周期管理出了问题——可能是 Guard 被移动到了更外层的作用域（比如存到了一个成员变量中），导致析构时机晚于预期。

## 设计决策回顾

**Spinlock 为什么是 header-only 的？** 因为 `sync.hpp` 中所有方法都在类定义中内联实现。这种设计在内核中很常见——锁操作通常极短（几条指令），内联后避免了函数调用开销。如果 Spinlock 的实现放在单独的 .cpp 文件中，编译器在编译调用者时看不到实现，无法内联，每次 acquire/release 都是一次函数调用——虽然开销很小（几纳秒），但在高频锁操作中会累积。header-only 的代价是增加了 include sync.hpp 的编译单元的编译时间，但在我们这个规模的项目中完全可以忽略。

**PerCPU 为什么不做成模板或宏？** Linux 用 `DEFINE_PER_CPU(type, name)` 宏来定义 per-CPU 变量，好处是统一接口、方便替换底层实现。但 Cinux 目前只有一个 per-CPU 变量（`g_per_cpu`），引入宏会过度设计。等将来有了多个 per-CPU 变量（比如 per-CPU 运行队列、中断计数器等），再抽象成宏也不迟。过早抽象是内核代码中常见的问题——Linux 的许多早期接口在后续版本中都被重写过，因为需求变化了。我们选择先写最直接的代码，等需求明确后再重构。

**tss_set_rsp0 为什么声明为 static？** 因为 GDT 实例 `g_gdt` 是全局唯一的——系统只有一个 GDT（单核场景下）。调用者不需要持有 GDT 引用或指针，直接通过 `GDT::tss_set_rsp0(addr)` 即可。如果声明为非 static 成员方法，调用者需要先获取 GDT 实例，增加了不必要的复杂度。Linux 也采用了类似的设计——`load_sp0` 是 `tss_struct` 的直接操作，不需要通过对象实例。

**为什么 PerCPU 不用模板？** Linux 用 `DEFINE_PER_CPU(type, name)` 宏来定义 per-CPU 变量，可以支持不同类型的 per-CPU 数据。Cinux 目前只有 `PerCPU` 一个结构体，用模板或宏来定义它会过度设计。等将来需要多个不同类型的 per-CPU 变量时（比如 per-CPU 运行队列、中断计数器），再引入抽象也不迟。"先写具体代码，等需求明确后再抽象"是内核开发中避免过度设计的有效策略。

## 本章小结

这一篇我们把抢占式调度的前置基础设施搭好了：Spinlock 用 GCC 原子内建操作 + pause 指令实现了轻量级的自旋互斥，Guard 用 RAII 保证了锁的获取和释放严格配对，PerCPU 结构体为"当前任务"和"内核栈顶"这两个关键信息提供了一个将来可以无缝扩展到 SMP 的数据抽象，tss_set_rsp0 让我们在每次上下文切换时都能正确维护 TSS 中用于特权级切换的栈指针。这些组件本身不做什么惊天动地的事，但没有它们，后面的抢占式调度就是空中楼阁。

下一篇才是重头戏——我们将在调度器中实现 tick()、schedule()、block/unblock 和 idle 任务，把整个抢占式调度的核心逻辑搭建起来。准备好了吗？让我们进入 tag 020 的核心战场。
