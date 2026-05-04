# 020-1 Spinlock 与 PerCPU 数据结构

## 导语

到 tag 019 为止，我们的调度器只支持协作式多任务——线程必须主动调用 `yield()` 才能让出 CPU。这在教学演示里勉强够用，但在真实的操作系统里，一个死循环的线程就能霸占整个 CPU，其他任务永远得不到执行机会。要解决这个问题，我们需要让定时器中断来"强制"收回 CPU 的控制权，这就从"协作式"跨入了"抢占式"调度。

但抢占式调度立刻带来了一个新问题：中断处理程序和普通线程代码可能在同一段数据上产生竞争。比如定时器中断把当前任务从 Running 改成 Ready，而此时另一个地方正在读取这个任务的状态——如果这两个操作不是原子的，状态就可能被撕成半新半旧的碎片。所以我们在动手改调度器之前，先把最基本的同步原语——Spinlock——准备好，再搭建一个 PerCPU 数据结构来管理"当前正在哪个 CPU 上跑什么任务"的信息。这篇文章的全部内容都是打地基，下一篇才会真正实现抢占式调度。

## 概念精讲

### Spinlock——自旋锁为什么用 test_and_set + pause

自旋锁的核心思想非常简单：用一块共享内存（一个 bool 变量）来表示锁的状态——false 表示空闲，true 表示被占。线程获取锁时，尝试把 false 原子地换成 true；如果发现已经是 true 了，就原地转圈等待，直到另一个线程把锁释放。听起来像是在门口不停地敲门问"好了没"——确实很粗暴，但在内核里对短临界区来说它是最快的同步方案。

这里的关键词是"原子"。如果读和写不是原子的，两个线程可能同时读到 false、同时认为锁是空闲的、同时拿到了锁——锁就废了。GCC 提供的 `__atomic_test_and_set` 内建函数就是做这件事的：它把目标内存位置设置为 true，同时返回设置之前的旧值。如果旧值是 false，说明我们抢到了；如果旧值是 true，说明别人正拿着，我们继续循环。这个内建函数最终会被编译成 x86 的 `xchg` 或者 `lock bts` 指令，硬件层面保证原子性——x86 的 LOCK 前缀配合这些指令可以让缓存行的操作变成不可分割的原子事务。

自旋等待中我们插入了一条 `pause` 指令。这条指令在 Intel SDM Vol.2B 的指令参考中有说明——它是一个提示性质的指令，告诉 CPU"我现在在自旋等待"，CPU 可以据此降低流水线功耗、减少内存顺序违规检测的惩罚。在超线程场景下，`pause` 还能避免自旋线程占满执行单元导致同核心的另一个硬件线程饿死。总之，自旋循环里加 `pause` 是 x86 上的标准做法，不加虽然也能跑，但在密集竞争时性能会明显下降。说实话，在我们的教学内核里性能差异可以忽略不计，但好习惯要尽早养成。

释放锁时使用 `__atomic_clear`，配合 `__ATOMIC_RELEASE` 语义——这确保了锁持有期间的所有写操作对下一个获取锁的线程可见。acquire 使用 `__ATOMIC_ACQUIRE` 语义，和 release 配对形成完整的 happens-before 关系。如果你对 acquire/release 语义不太熟悉，可以简单理解为：acquire 保证"拿到锁之后能看到锁持有者做过的所有写操作"，release 保证"释放锁之前我的所有写操作对下一个获取者可见"。

这里有一个值得思考的问题：为什么不用 C++11 的 `std::atomic<bool>`？答案是内核环境——我们根本没有 C++ 标准库，更不可能依赖 libstdc++ 的原子实现。直接使用 GCC 内建函数是内核开发中最常见的做法，Linux 内核的 `atomic_t` 也是基于类似的编译器内建操作实现的。

> 参考：Intel SDM Vol.3A Section 8.1 — Locked Atomic Operations
> 参考：Intel SDM Vol.2B — PAUSE instruction reference

### RAII Guard——异常安全的锁管理模式

Spinlock 的 acquire 和 release 必须严格配对——每获取一次就一定要释放一次。听起来简单，但代码一旦有了分支、提前 return 或者异常（虽然我们没有 C++ 异常，但内核代码里的提前 return 可太常见了），漏掉 release 就会导致死锁。RAII（Resource Acquisition Is Initialization）模式完美解决了这个问题：在 Guard 对象的构造函数里 acquire，析构函数里 release。无论函数怎么退出——正常 return、提前 return 还是其他控制流——Guard 的析构函数都会被调用，锁一定会被释放。

`[[nodiscard]]` 属性确保你不会忘记把 guard() 的返回值赋给一个变量——如果你写了 `spin.guard();` 而没有保存返回值，编译器会报警告。原因是临时对象在语句结束时立刻析构，锁又被释放了，等于没加锁。这种错误在代码审查中很不容易发现——锁看起来加了，实际上一微秒后就放开了。

Guard 的拷贝构造和赋值都被 delete 了。这是必须的——如果允许拷贝，两个 Guard 对象会指向同一把锁，第一个析构时释放，第二个析构时又释放，release 一个已经释放的锁，后续的 acquire/release 语义就全乱了。move 构造其实可以做，但目前的需求场景用不到，为了简单起见也一并 delete 了。

### PerCPU 数据结构——为 SMP 预留的占位

在单核系统里，"当前正在运行的任务"就是一个简单的全局指针。但在多核（SMP）系统中，每个 CPU 核心都有自己的当前任务，所以 Linux 和大多数操作系统会用 per-CPU 数据区——一块每个 CPU 核心都有独立副本的内存区域，通过 GS（x86-64）或 TPIDR_EL1（ARM64）段寄存器来寻址。

我们目前的 PerCPU 结构体只有两个字段：`current` 指向当前任务，`kernel_stack` 保存当前任务的内核栈顶地址。单核场景下用一个简单的全局变量 `g_per_cpu` 就够了，等以后支持 SMP 时再替换成真正的 per-CPU 段。这个占位设计的妙处在于：调度器代码中对 `g_per_cpu.current` 的读写在未来不需要改——只要把 g_per_cpu 从全局变量变成通过 GS 寄存器寻址的 per-CPU 变量，所有使用点自动变成"访问本 CPU 的数据"。

Linux 的做法是 `DEFINE_PER_CPU(type, name)` 宏来声明 per-CPU 变量，运行时通过 `this_cpu_read/write` 系列宏来访问，底层用 GS 段寄存器做偏移。我们现在只取其意而不取其形——用最简单的全局变量来模拟，等真正需要 SMP 的时候再引入宏和段寄存器寻址。

### TSS.RSP0——为什么每次切换都要更新

x86-64 的 Task-State Segment（TSS）里有一个 RSP0 字段——当 CPU 从 ring 3（用户态）切换到 ring 0（内核态）时，处理器会自动把 RSP 加载为 TSS.RSP0 的值。Intel SDM Vol.3A Section 8.7 明确说明，64 位模式下 TSS 的主要用途就是这个——不再是硬件任务切换，而是提供特权级切换时的栈指针。

虽然我们目前还没有用户态任务，但提前在每次上下文切换时更新 RSP0 是一个好习惯：一旦未来添加了用户态支持，你不需要回头改调度器——RSP0 已经在每个上下文切换中被正确维护了。如果你忘记更新，用户态任务触发系统调用时 CPU 加载的就是上一个任务的内核栈——栈指针错误，写入数据覆盖了别人的栈帧，内核的崩溃几乎不可避免。更糟糕的是这种 bug 只在用户态任务出现后才会暴露，到时候你可能完全想不到是调度器的问题。

我们在 GDT 类中新增一个 `tss_set_rsp0()` 静态方法，直接写入全局 GDT 结构体中的 TSS RSP0 字段。这个方法在 schedule()、exit_current() 和 run_first() 三个地方被调用——正好是所有会执行 context_switch 的路径。

> 参考：Intel SDM Vol.3A Section 2.1.3 — Task-State Segments
> 参考：Intel SDM Vol.3A Section 8.7 — Task Management in 64-Bit Mode

## 动手实现

### Step 1: 创建 Spinlock 类

**目标**: 在 `kernel/proc/sync.hpp` 中实现自旋锁，包含 acquire、release 和 RAII Guard。

**设计思路**: 我们需要三个核心操作。acquire 使用 `__atomic_test_and_set` 配合 `__ATOMIC_ACQUIRE` 语义进行原子测试并设置，失败时执行 `pause` 指令降低功耗后重试。release 使用 `__atomic_clear` 配合 `__ATOMIC_RELEASE` 语义清零锁变量。Guard 是一个嵌套类，构造时 acquire、析构时 release，拷贝构造和赋值都被 delete，防止同一个 Guard 被多次释放。

Spinlock 的底层存储是一个 `volatile bool locked_`，初始值为 false。volatile 告诉编译器不要对这个变量做优化（比如缓存到寄存器里），每次读写都必须访问内存——这在自旋锁中是必要的，因为锁的状态可能被另一个 CPU 核心随时改变。不过说实话，`__atomic_test_and_set` 本身已经隐含了 volatile 语义，这里加 volatile 更多是代码意图的表达——明确告诉读者"这个变量会被外部修改"。

**实现约束**: 整个类是 header-only 的，所有方法都在类定义中内联。不使用任何外部库——纯靠 GCC 内建原子操作和内联汇编。命名空间 `cinux::proc`。类不需要构造函数参数——默认构造即可，locked_ 的就地初始化器会处理初始状态。

**踩坑预警**: 千万不要在持有 Spinlock 的情况下调用 block() 或 yield()——自旋锁的设计前提是临界区极短（几条指令到几十条指令），持有者绝不能让出 CPU。如果你在锁持有期间触发了上下文切换，其他线程尝试获取同一把锁时会永远自旋下去——因为持有锁的线程没在跑了，锁永远不会被释放。这就是经典的自旋锁死锁场景，一旦发生系统直接卡死，连日志都打不出来。这个约束在后续 021 的 Mutex 实现中会更加重要——Mutex 内部会先释放 Spinlock 再调用 block()，顺序绝对不能颠倒。

**验证**: 编译通过，确认没有链接错误。

### Step 2: 定义 PerCPU 结构体

**目标**: 在 `kernel/proc/per_cpu.hpp` 中定义 per-CPU 数据占位结构。

**设计思路**: PerCPU 结构体包含两个字段：`Task* current` 指向当前正在该 CPU 上运行的任务，`uint64_t kernel_stack` 保存当前任务的内核栈顶地址。声明一个全局实例 `extern PerCPU g_per_cpu`，定义在 scheduler.cpp 中，初始化为 `{nullptr, 0}`。

结构体用 `struct` 而不是 `class`——所有成员默认 public，因为我们希望调度器能直接读写这些字段。PerCPU 的设计理念是"数据不属于任何类"，它是一块被多个子系统共享的状态——调度器写 current，中断处理程序读 current，未来 SMP 初始化代码设置 GS 基址指向对应的 per-CPU 区域。这种全局共享数据的设计在操作系统内核中非常常见，虽然面向对象的纯粹主义者可能会皱眉，但在内核里"简单直接"永远比"封装优雅"更值钱。

**实现约束**: 前向声明 `struct Task` 即可，不需要包含完整的 Task 定义——这减少了头文件依赖，避免了 include 循环。命名空间 `cinux::proc`。整个头文件不超过 16 行。

**验证**: 编译通过。

### Step 3: 在 GDT 中添加 tss_set_rsp0 方法

**目标**: 在 `kernel/arch/x86_64/gdt.hpp` 和 `gdt.cpp` 中添加静态方法来更新 TSS 的 RSP0 字段。

**设计思路**: TSS 在 GDT 中被定义为一个包含 RSP0-RSP2 和 IST1-IST7 的结构。我们的 GDT 类已经有一个 `g_gdt` 静态成员来持有整个 GDT 布局，其中 `tss_` 就是那个 TSS 结构。`tss_set_rsp0()` 直接把传入的值写入 `g_gdt.tss_.rsp[0]`——注意 RSP0 是 `rsp` 数组的第一个元素（索引 0）。

方法声明为 static，因为它操作的是全局 GDT，不需要实例。调用者传入的值是新任务的 `kernel_stack_top`，即任务内核栈的最高地址（栈是从高地址向低地址增长的，所以栈顶是高地址那一端）。

**实现约束**: 在 gdt.hpp 的 public 区域声明 `static void tss_set_rsp0(uint64_t rsp0)`，放在 init() 方法之后。在 gdt.cpp 中实现，函数体只有一行赋值语句。不需要重载 GDT 或执行 `lgdt`——TSS 的 RSP0 字段在下次 ring 3 到 ring 0 切换时会被 CPU 自动读取，不需要 CPU 侧的任何刷新操作。

**验证**: 编译通过。

### Step 4: 在 scheduler.cpp 中定义 g_per_cpu 并更新静态成员

**目标**: 补充 scheduler.cpp 中新增的静态成员定义和 PerCPU 全局实例。

**设计思路**: Scheduler 类在 tag 020 中新增了几个静态成员：`idle_task_`（空闲任务指针）、`initialized_`（是否已完成初始化）、`tick_count_`（全局 tick 计数器）、`current_slice_`（当前任务已使用的 tick 数）。这些都需要在 scheduler.cpp 的命名空间作用域中定义并初始化——C++ 的静态成员变量必须在类外有一份定义，否则链接器报 "undefined reference"。

同时需要定义 `PerCPU g_per_cpu{nullptr, 0}` 并 include 新增的 `per_cpu.hpp` 和 `gdt.hpp` 头文件。include 的顺序是先 arch 头文件再 proc 头文件——这和 kernel_main.cpp 中的 include 顺序保持一致，虽然 C++ 的 include 顺序不影响语义，但在大项目中保持统一的顺序能减少 merge conflict。

**实现约束**: 静态成员的初始值分别是：idle_task_ = nullptr, initialized_ = false, tick_count_ = 0, current_slice_ = 0。g_per_cpu 的初始化放在 PerCPU 定义之后、Scheduler 静态成员定义之前。头文件 include 顺序为：`kernel/arch/x86_64/gdt.hpp`、`kernel/lib/kprintf.hpp`、`kernel/proc/per_cpu.hpp`。

**验证**: 编译通过。

### Step 5: 编译验证

**目标**: 确认所有新增文件和修改能正确编译。

```bash
cmake --build build --target big_kernel 2>&1 | tail -20
```

预期输出中不应有编译错误或链接错误。新增的 sync.hpp 和 per_cpu.hpp 是 header-only 的，不会增加编译单元。gdt.cpp 新增了一行函数实现，scheduler.cpp 新增了静态成员定义。

如果编译报错说找不到 sync.hpp 或 per_cpu.hpp，检查文件是否放在了 `kernel/proc/` 目录下，以及 CMakeLists.txt 的 include path 中是否包含 `kernel/` 目录——通常我们用 `#include "kernel/proc/sync.hpp"` 这种从项目根目录开始的路径来引用，而不是相对路径。

## 构建与运行

到这一步我们还没有实现抢占式调度的任何核心逻辑——Spinlock 和 PerCPU 只是工具和基础设施。内核的行为和 tag 019 完全一样：协作式多任务、手动 yield、两线程交替打印。所以运行结果应该和 tag 019 一模一样，如果你看到什么不同，说明在迁移过程中不小心改动了不该改的东西。

如果你迫不及待想验证 Spinlock 是否能正常工作，可以在任意内核函数中创建一个 Spinlock，获取它、释放它，打印前后的 locked_ 值——不过说实话这个验证意义不大，因为单核环境下没有竞争条件，Spinlock 的 acquire 第一次就会成功。真正的考验要等后面有了中断驱动的抢占式调度，再加上 021 的 Mutex 和 Semaphore 才有意义。

## 调试技巧

**编译报错 "volatile bool is not a pointer type"**: 可能是你在 `__atomic_test_and_set` 中传了错误的参数类型。这个内建函数的第一个参数应该是指向 volatile bool 的指针——直接传 `&locked_` 即可。如果你不小心写了 `__atomic_test_and_set(locked_, ...)` 少了取地址符，编译器就会报这个错。

**Guard 对象的析构顺序错误**: C++ 的局部对象按声明的逆序析构。如果你在一个函数里获取了多把锁，它们的 Guard 对象会按"最后声明的先释放"的顺序析构——这就是 LIFO 锁释放语义。大多数情况下这正是你想要的，但如果你需要特定的释放顺序，可以用额外的花括号块来控制 Guard 的生命周期范围。

**链接错误 "undefined reference to cinux::proc::g_per_cpu"**: 说明 g_per_cpu 只有 extern 声明（在 per_cpu.hpp 中）但没有定义（在某个 .cpp 文件中）。确认 scheduler.cpp 中有 `PerCPU g_per_cpu{nullptr, 0};`。

**tss_set_rsp0 写入了错误的位置**: 如果你发现用户态系统调用时内核栈指针不对，检查 TSS 的 rsp 数组索引是否为 0。RSP0 对应索引 0，RSP1 对应索引 1，RSP2 对应索引 2。在 64 位模式下只有 RSP0 有实际意义（ring 3 到 ring 0 切换），RSP1 和 RSP2 在 64 位下不使用。

**Spinlock 在 release 后其他 CPU 仍然自旋**: 这是内存可见性问题。确认 release 使用了 `__ATOMIC_RELEASE` 语义—— bare write 虽然在 x86 的 TSO 内存模型下通常也能工作，但标准要求使用 release 语义才能保证跨架构的可移植性。

## 本章小结

这一篇我们把抢占式调度的前置基础设施搭好了：Spinlock 用 GCC 原子内建操作 + pause 指令实现了轻量级的自旋互斥，Guard 用 RAII 保证了锁的获取和释放严格配对，PerCPU 结构体为"当前任务"和"内核栈顶"这两个关键信息提供了一个将来可以无缝扩展到 SMP 的数据抽象，tss_set_rsp0 让我们在每次上下文切换时都能正确维护 TSS 中用于特权级切换的栈指针。这些组件本身不做什么惊天动地的事，但没有它们，后面的抢占式调度就是空中楼阁。下一篇才是重头戏——我们将在调度器中实现 tick()、schedule()、block/unblock 和 idle 任务，把整个抢占式调度的核心逻辑搭建起来。
