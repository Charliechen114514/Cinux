---
title: 020-proc-scheduler-1 · 进程调度
---

# 从协作到抢占：内核多任务的进化

> 标签：抢占式调度, Spinlock, PerCPU, 时间片轮转, idle 任务
> 前置：[019-3 踩坑实录：两个让内核崩掉的 Bug](019-proc-context-3.md)

## 前言

tag 019 结束时，Cinux 已经有了合作式多任务——两个线程通过 `yield()` 主动让出 CPU，交替打印 5 轮消息。看着串口输出里 `[A]` 和 `[B]` 交替出现，确实有那么一点"操作系统"的样子了。但说实话，这个 demo 离真正的操作系统还有一段距离。为什么？因为合作式调度有一个致命的前提：每个线程都必须自觉调用 `yield()`。如果某个线程写了一个死循环忘了 yield，整个系统就卡死了——其他线程永远得不到执行机会。

真实的操作系统显然不能依赖线程的"自觉性"。你的浏览器标签页不可能写一个 `while(true)` 就把整个桌面冻结了，你的音乐播放器也不可能因为一个 bug 就让鼠标不动了。这就是抢占式调度存在的意义——内核通过硬件定时器中断，每隔一段时间强制收回 CPU 的控制权，把执行权交给调度器，由调度器决定下一个该跑谁。线程不需要"配合"，甚至不需要知道抢占的存在。

tag 020 做的就是这件事：把 tag 019 的合作式调度器改造成抢占式调度器。改造涉及三块基础设施——Spinlock 保护共享队列的并发访问、PerCPU 为未来 SMP 做好数据布局、TSS.RSP0 更新确保任务切换后内核栈指针正确——以及调度器本身的核心逻辑：tick 驱动的时间片轮转、idle 任务的兜底机制、block/unblock 为同步原语铺路。这篇我们先讲基础设施的设计动机和实现，下一篇深入调度核心逻辑，第三篇再讲调试过程中踩到的两个经典暗坑。

## 经典设计对比：别人家怎么做抢占

在进入 Cinux 的实现之前，我们先看看成熟操作系统是怎么处理抢占式调度的。理解了别人的设计选择，回头看自己的会更有感觉。

xv6-riscv 的抢占调度路径非常直接。定时器中断触发后，`usertrap()` 或 `kerneltrap()` 检测到中断源是时钟（scause == 0x8000000000000005），直接调用 `yield()`。`yield()` 获取进程自旋锁 `p->lock`、把状态设为 RUNNABLE、调用 `sched()` -> `swtch()`，把当前进程的寄存器快照保存起来，切换到 per-CPU 的 scheduler 上下文。scheduler 在一个无限循环中扫描进程表，找到第一个 RUNNABLE 的进程，`swtch()` 过去执行。这个设计有一个显著的特点——每次上下文切换都经过中间的 scheduler 线程：进程 A 放弃 CPU 后先回到 scheduler，scheduler 再决定下一个运行 B。好处是调度逻辑集中在 scheduler 循环里，容易理解；代价是每次切换多了一趟"过路"。xv6 的 `clockintr()` 还有一个细节值得注意——它通过 `w_stimecmp(r_time() + 1000000)` 编程下一次时钟中断，相当于把定时器间隔硬编码成了 1 秒（以 CPU 时钟周期计）。这意味着 xv6 的时间片粒度取决于这个编程值和 RISC-V 的时钟频率。

Linux 的抢占模型则复杂得多。`scheduler_tick()` 在定时器中断中被调用，但它不直接做上下文切换——它只设置 `TIF_NEED_RESCHED` 标志，意思是"该考虑换人了"。实际的上下文切换被推迟到下一个安全的抢占点（比如从内核态返回用户态时、或者在中断处理返回时）。这种延迟抢占模型的好处是灵活性高——调度器可以在很多不同的点做出切换决策，而且可以通过 `preempt_count` 控制哪些区域不可抢占。Linux 的 `scheduler_tick()` 还会做大量额外工作：更新任务的运行时统计（`update_curr()`）、检查 CFS 的虚拟运行时间是否超标、处理实时任务的带宽限制、触发负载均衡等等。这些复杂性来自 Linux 需要在数万个并发进程、数百个 CPU 核心的生产环境中运行——和 Cinux 的教学目标完全不同。

Cinux 选择了最简单的模型——定时器中断直接调用 `schedule()`，立即执行上下文切换。没有延迟，没有中间上下文，没有抢占计数。和 xv6 类似的直截了当，但不经过 scheduler 中间层——直接从 prev 任务切到 next 任务。这种方案在单核、纯内核线程的场景下是最简洁的，但在引入用户态和 SMP 后需要重新审视。我们会在适当的时候引入延迟抢占。

三种设计放在一起对比就很有意思了：xv6 的"经过中间层"模型最清晰但最慢（每次切换多一趟 swtch），Linux 的"延迟抢占"模型最灵活但最复杂（维护 preempt_count 和 resched flag），Cinux 的"直接切换"模型最简单但在中断上下文中需要格外小心——因为 `context_switch` 执行时中断是关闭的，切换完成后必须保证新任务能正确接收后续中断。这个"保证"后来成了一个经典的坑，我们第三篇会详细讲。

这里还有一个 xv6 和 Cinux 的关键架构差异值得说明。xv6 在 `scheduler()` 函数中显式调用 `intr_on()` 开中断，然后在扫描进程表的过程中中断是开启的——如果此时定时器中断到来，CPU 会中断 scheduler 的循环，进入 `usertrap` / `kerneltrap`，但因为 scheduler 本身不是任何进程，不会被 yield。Cinux 不经过中间 scheduler 上下文，直接从 prev 任务切到 next 任务，所以不需要处理"scheduler 本身被中断"的问题——代价是 `context_switch` 必须在中断关闭的状态下完成。

## Spinlock：为什么 tag 019 不需要锁

tag 019 的调度器没有任何锁——因为根本不需要。合作式调度中，`yield()` 只在任务主动调用时触发，整个调度过程是同步的。`pick_next()` 从 RoundRobin 队列里取出下一个任务、`context_switch` 切换过去——这个过程中不可能有另一个执行流同时操作同一个队列。

但 tag 020 不一样了。调度由定时器中断驱动，IRQ0 随时可能在任何执行点打断当前任务并调用 `schedule()`。如果 RoundRobin 的 `pick_next()` 正在移动队列元素时被中断打断，而中断 handler 又调用了同一个队列的 `dequeue()`，数据结构就会损坏。这就是 Spinlock 存在的理由——保护临界区不被中断打断。

```cpp
class Spinlock {
public:
    Spinlock() = default;

    void acquire() {
        while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
    }

    void release() {
        __atomic_clear(&locked_, __ATOMIC_RELEASE);
    }

    [[nodiscard]] auto guard() {
        return Guard(this);
    }

private:
    volatile bool locked_ = false;

    class Guard {
    public:
        explicit Guard(Spinlock* lock) : lock_(lock) {
            lock_->acquire();
        }

        ~Guard() {
            lock_->release();
        }

        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        Spinlock* lock_;
    };
};
```

Spinlock 的核心思路很直接：用一个 `bool` 变量表示锁的状态，通过原子操作来保证只有一个线程能成功获取。`locked_` 初始化为 `false`，声明为 `volatile` 防止编译器把自旋循环优化成死循环。

`acquire()` 调用 GCC 内建函数 `__atomic_test_and_set`——这个函数把 `locked_` 原子地设为 `true`，同时返回旧值。如果旧值是 `true`（锁已被持有），就继续自旋。`__ATOMIC_ACQUIRE` 内存序保证了 acquire 之后的读操作不会被重排到 acquire 之前，这在保护队列数据时非常关键——你不想在获取锁之前就读到了队列的中间状态。自旋体内嵌了一条 `pause` 指令，Intel SDM Vol. 2B 把它描述为"improves the performance of spin-wait loops"，能降低 CPU 功耗并减少流水线惩罚。这条指令不是什么"神奇的优化开关"——它的作用是告诉 CPU 的流水线"我正在自旋等待，别做激进的推测执行"，从而避免昂贵的流水线清空（memory order violation）。在早期 x86 处理器上没有 `pause` 的自旋循环会导致严重的性能退化，现代处理器上差别没那么大，但加上始终是好的习惯。

`release()` 使用 `__atomic_clear` 把 `locked_` 原子地设为 `false`，`__ATOMIC_RELEASE` 内存序确保 release 之前的所有写操作对其他 CPU 可见。这一对 acquire/release 语义恰好构成了一个完整的锁序关系：获取者读到 release 写入的 `false` 之后，能看到锁保护区域内的所有修改。这个"happens-before"的关系是并发编程中所有正确性的基石。

你可能会想：为什么不直接用更强的 `__ATOMIC_SEQ_CST`（顺序一致性）？原因是性能。`__ATOMIC_ACQUIRE` 和 `__ATOMIC_RELEASE` 是最轻量级的锁序对——acquire 保证之后的读写不会被重排到之前，release 保证之前的读写不会被重排到之后。更强的 `__ATOMIC_SEQ_CST` 会插入完整的内存屏障（`mfence`），在 x86 上代价更高。对于锁操作来说，acquire/release 已经足够——锁保护区域内的所有操作自然地被包含在 acquire-release 对之间。

这里有一个细节值得展开：为什么是 `volatile bool` 而不是 `std::atomic<bool>`？原因是 Cinux 的内核运行在 freestanding 环境中，`<atomic>` 头文件不一定可用，而 `__atomic_test_and_set` / `__atomic_clear` 是 GCC 的编译器内建函数，不需要任何头文件支持。`volatile` 的作用是防止编译器把 `locked_` 缓存到寄存器中——每次循环迭代都必须真正从内存读取最新值。一个反面例子是早期的 Linux 内核大量使用内联汇编实现原子操作，后来逐步迁移到 `atomic_t` 封装层，正是因为可移植性的考量。

Guard 的 RAII 模式也很标准——构造时获取锁，析构时释放锁，禁用拷贝。`[[nodiscard]]` 属性防止调用者忽略返回值。如果写了 `lock.guard()` 却不保存返回值，Guard 的析构函数会立即执行，锁就等于没加。这种模式在 Linux 内核的 `guard(spinlock_irqsave)` 中也经常出现。虽然 Cinux 当前的内核线程不会抛出 C++ 异常，但 RAII 的优势不仅在于异常安全——它还防止了程序员忘记在某个 early-return 路径上调用 `release()`。这类 bug 在内核代码中非常隐蔽，因为忘记释放锁不会立即崩溃，而是以难以复现的死锁形式出现。

不过这里有一个更深层的自旋锁安全陷阱需要提一下。Spinlock 本身只能保证同一时刻只有一个执行流进入临界区，但它不能防止中断。如果在持有 Spinlock 的同时收到定时器中断，而中断 handler 又试图获取同一把锁，就会产生自死锁——自己等自己释放锁。解决思路是"获取锁之前先关中断，释放锁之后恢复中断状态"——Linux 内核的 `spin_lock_irqsave()` / `spin_unlock_irqrestore()` 做的也是完全一样的事情。在 tag 020 中我们暂时没有实现这个"中断安全的 Guard"——RoundRobin 的队列操作使用的是普通 Guard。这是因为 tag 020 的 `tick()` 调用链（IRQ0 -> ISR -> tick() -> schedule()）在中断门语义下 IF 已经被自动清零，不会触发另一个 IRQ0，所以普通 Guard 在当前阶段是安全的。但如果未来 tick() 的调用上下文发生变化（比如从 trap gate 进入，IF 不被清零），就需要升级为中断安全的版本。

## PerCPU：为 SMP 做好占位

```cpp
struct PerCPU {
    Task*    current;
    uint64_t kernel_stack;
};

extern PerCPU g_per_cpu;
```

`PerCPU` 的设计意图是"每个 CPU 一份"的数据结构。在单核阶段它只是一个普通的静态全局变量 `g_per_cpu`，但字段布局已经为 SMP 做好了准备。`current` 指向当前正在运行的任务——这个指针在每次 `schedule()` 时被更新，是调度器最频繁访问的字段之一。`kernel_stack` 记录当前任务的内核栈顶地址，用于用户态系统调用快速定位内核栈。

在 SMP 场景下，每个 CPU 通过 GS 段基址加上固定偏移就能定位到自己对应的 PerCPU 结构，不需要全局锁。Linux 的 `this_cpu_read` / `this_cpu_write` 系列宏做的也是这件事——通过 GS 或 FS 段寄存器绕过锁直接访问 per-CPU 数据。Cinux 选择从最简形式开始——两个字段，一个静态全局——当未来引入 SMP 时，每个 CPU 的 GS 基址会指向各自的 PerCPU 实例，通过 `swapgs` 指令在内核态和用户态之间切换。这种"先占位后扩展"的做法好处是每次只引入最必要的字段，避免一次性设计过度复杂的结构体。

对比 xv6 的 `struct cpu`（持有 `proc*`、`int noff`、`int intena` 等字段），Cinux 的 PerCPU 目前只保留了最核心的两个指针。对比 Linux 的 per-CPU 数据（运行队列 `rq`、中断计数器、RCU 状态等），差距更大。但 Cinux 选择从最简形式开始逐步扩展，这种做法让每个 tag 的 diff 更聚焦。实际上 per-CPU 数据结构在 Linux 中有更广泛的用途——不仅仅是 `current` 指针，还包括 per-CPU 运行队列、中断计数器、RCU 状态等。Cinux 选择从最简形式开始，逐步扩展。

## TSS.RSP0：任务切换时更新内核栈

上面 Spinlock 保护共享数据结构、PerCPU 为每个 CPU 保存运行时状态，但还有一个问题没解决：当 CPU 从用户态（ring 3）通过中断或系统调用进入内核态（ring 0）时，它怎么知道该用哪个栈？

答案是 TSS 中的 RSP0 字段。Intel SDM Vol. 3A Section 8.7 说明，在 64 位模式下 TSS 的 RSP0 字段在 CPU 从 ring 3 切换到 ring 0 时被自动加载到 RSP。也就是说，如果未来某个任务在用户态执行时触发了中断或系统调用，CPU 会用 TSS.RSP0 作为内核栈。每次任务切换时，我们必须把这个字段更新为新任务的内核栈顶。

```cpp
void GDT::tss_set_rsp0(uint64_t rsp0) {
    g_gdt.tss_.rsp[0] = rsp0;
}
```

一行代码看起来平淡无奇，但它解决了一个非常重要的硬件约束。如果不更新这个字段，所有用户态任务都会共享同一个内核栈——轻则数据损坏，重则直接 triple fault。有趣的是，在纯内核线程阶段（ring 0 到 ring 0 切换），CPU 根本不看 TSS.RSP0——因为特权级没变，不需要从 TSS 加载栈指针。所以当前阶段这个调用其实是"预防性的"，在用户态支持上线之前不会有实际效果，但养成每次切换都更新的习惯可以避免将来忘记。Linux 在 `context_switch()` -> `switch_to()` 中也有类似的操作——通过 `load_sp0` 更新 `tss_struct` 的 `x86_tss.sp0`。

回过头来看 TSS 结构体中的 `rsp` 数组——它包含 3 个元素，分别对应 RSP0、RSP1、RSP2，即从 ring 3/2/1 切换到 ring 0 时加载的栈指针。Cinux 只使用 RSP0，因为 x86_64 的特权级实际上只用到了 ring 0（内核）和 ring 3（用户），RSP1/RSP2 永远不会被触发。TSS 中还有 IST1-IST7 字段用于中断栈表——Cinux 用 IST1 作为 Double Fault 的专用栈，在 GDT 初始化时通过 `tss_.ist[0] = df_stack_top` 设置。普通中断不使用 IST 机制，走的是 RSP0 路径。

理解了 RSP0 的硬件语义之后，`tss_set_rsp0()` 为什么必须在每次上下文切换时调用就很好理解了：每个任务有自己独立的内核栈（由 `TaskBuilder::build()` 分配的 4 页连续虚拟内存），如果切到任务 B 但 TSS.RSP0 还指向任务 A 的栈，那么任务 B 在用户态触发中断时会用到 A 的栈——轻则数据损坏，重则直接 triple fault。

把它声明为 `static` 方法是因为 GDT 实例 `g_gdt` 是全局唯一的，调用者不需要持有 GDT 引用，直接通过 `GDT::tss_set_rsp0(addr)` 即可。这个方法在每次 `schedule()`、`exit_current()` 和 `run_first()` 中都会被调用，确保 TSS.RSP0 始终指向当前任务的内核栈顶。这是一个很小但很典型的"硬件-软件同步"模式——软件负责在切换时告诉硬件"新任务用哪个栈"，硬件在中断到达时自动使用这个栈。两者的契约通过 TSS 这个数据结构来维护。

## scheduler.hpp 的新增声明

在深入每块基础设施之前，我们先看一眼 scheduler.hpp 中新增的声明，这能帮助我们建立全局视图：

```cpp
class Scheduler {
public:
    static constexpr int DEFAULT_TIME_SLICE = 2;

    static void remove_task(Task* task);
    static bool is_initialized();

    static void tick();
    static void schedule();
    static void block(Task* task, const char* reason);
    static void unblock(Task* task);

private:
    static void idle_entry();
    static Task* idle_task_;
    static bool initialized_;
    static int tick_count_;
    static int current_slice_;
};
```

`DEFAULT_TIME_SLICE = 2` 意味着每个线程在耗尽 2 个 tick（即 20ms）后被强制切换——这个值不是拍脑袋定的，而是踩了第一个暗坑（下一篇会讲）之后得出的经验值。`tick()` 和 `schedule()` 是抢占式调度的双引擎：前者在每次定时器中断中被调用，负责判断时间片是否耗尽；后者负责实际的上下文切换。`block()` 和 `unblock()` 为未来的 Mutex/Semaphore 预留了接口——线程在等待锁时 block 自己，锁释放时被 unblock 唤醒。`idle_task_` 指向一个永不退出、只执行 `hlt` 的特殊任务，在所有普通任务都阻塞或退出时兜底。`initialized_` 是标志位防止启动阶段误触发调度——`tick()` 在调度器初始化完成之前会直接返回。

## 环境说明

本章涉及的核心文件是三个新增文件和两个修改文件。`kernel/proc/sync.hpp`（45 行）定义了 Spinlock 和 Guard；`kernel/proc/per_cpu.hpp`（16 行）定义了 PerCPU 结构体和全局变量；`kernel/arch/x86_64/gdt.hpp` / `gdt.cpp` 新增了 `tss_set_rsp0()` 的声明和实现。运行环境和前面几个 tag 一致——GCC cross-compiler（`x86_64-elf`）、QEMU、higher-half 内核（`KERNEL_VMA = 0xFFFFFFFF80000000`）。前置条件是 tag 019 的调度器和 PIT 驱动已经就绪。整个 tag 新增 521 行代码，涉及 19 个文件的变更，核心变更集中在 proc 子系统和 arch/x86_64。

调度器的所有静态成员变量在 `scheduler.cpp` 中定义：

```cpp
Task* Scheduler::idle_task_ = nullptr;
bool Scheduler::initialized_ = false;
int Scheduler::tick_count_ = 0;
int Scheduler::current_slice_ = 0;
```

四个变量全部零初始化。`initialized_` 在 `init()` 结束时被设为 `true`，是调度器从"不可用"到"可用"的分界线。`tick_count_` 是全局 tick 计数器，单调递增，用于系统级的时间统计。`current_slice_` 是当前任务已使用的 tick 数，达到 `DEFAULT_TIME_SLICE` 后被重置。这些变量看起来简单，但它们的正确初始化和重置对抢占式调度的可靠性至关重要——第三篇的调试故事就是围绕这些变量展开的。

## 设计决策回顾

到这里我们已经把抢占式调度的三块基础设施全部过了一遍。回顾一下几个关键的设计选择，以及为什么这样选而不是那样选。

Spinlock 为什么用 GCC 内建原子操作而非内联汇编？`__atomic_test_and_set` 和 `__atomic_clear` 是 GCC 提供的编译器内建函数，比手写内联汇编更安全——编译器理解它们的语义，能够正确地与周围的代码进行指令重排优化。而内联汇编是编译器的黑盒，往往需要额外的 `volatile` 和 `"memory"` clobber 来保证正确性。如果有一天 Cinux 要移植到 RISC-V 或 ARM，这些内建函数会自动映射到对应架构的原子指令（如 RISC-V 的 `lr.d` / `sc.d`），而内联汇编就必须全部重写。

PerCPU 为什么不直接用 `thread_local`？C++ 的 `thread_local` 在裸机内核中不可用——它依赖操作系统的 TLS 支持，而 Cinux 本身就是那个操作系统。用 GS 段基址加固定偏移量是 x86_64 内核实现 per-CPU 数据的标准做法。

tss_set_rsp0 为什么在每次 schedule 都要调用？因为每次上下文切换都可能切到一个不同的任务，而每个任务有独立的内核栈。如果遗漏了更新，用户态中断就会用错误的栈。有趣的是，在纯内核线程阶段这个调用没有实际效果——CPU 在 ring 0 到 ring 0 切换时不看 TSS.RSP0。但养成每次切换都更新的习惯可以在用户态支持上线时避免忘记。Linux 在 `switch_to` 中也做了类似的事情。

## 收尾

Spinlock 提供了最底层的互斥原语，确保中断驱动的调度不会踩坏共享队列；PerCPU 为 per-CPU 数据访问铺好了路，是 SMP 扩展的第一步；TSS.RSP0 更新把硬件级别的栈切换机制和软件调度器连接了起来，为未来的用户态任务做好准备。这三块基础设施本身不执行调度——真正让线程被抢占、被切换、被重新调度的核心逻辑，是下一篇要讲的 `tick()` / `schedule()` / `block()` / `unblock()` 的完整实现。

从代码量来看，tag 020 的三块基础设施涉及两个新增文件（`sync.hpp` 45 行、`per_cpu.hpp` 16 行）和两个修改文件（`gdt.hpp/cpp` 各增几行）。和第二篇的 scheduler.cpp（新增约 100 行）相比，这一篇的代码量并不大，但概念密度很高——Spinlock 的原子语义、PerCPU 的 SMP 设计意图、TSS.RSP0 的硬件约束——每一个都值得深入理解。

## 扩展方向

当前的 Spinlock 是最简单的 test-and-set 自旋锁。在 SMP 环境下，多个 CPU 同时自旋时会导致总线争用和 cache line bouncing——所有 CPU 都在疯狂写同一个 `locked_` 变量，每次写入都使其他 CPU 的 cache line 失效。Linux 使用 ticket spinlock（或 MCS lock）来保证 FIFO 公平性和更低的 cache 压力——每个 CPU 取一个"号码"，按号码顺序获取锁，避免了多 CPU 同时竞争同一个变量的问题。当 Cinux 引入 SMP 支持时，这是一个值得优先升级的组件。

PerCPU 也有明确的扩展路径。当前 RoundRobin 是全局唯一的队列，所有调度操作都通过同一把锁保护。在多核系统中，全局队列的 Spinlock 竞争会成为瓶颈。Linux 的 CFS 使用 per-CPU 运行队列（`struct rq`），每个 CPU 在本地队列上做调度决策，只在负载均衡时才访问其他 CPU 的队列。Cinux 可以把 `RoundRobin` 实例移到 `PerCPU` 结构体中，作为 SMP 扩展的第一步。这个改动在架构上非常自然——`g_per_cpu.current` 已经是 per-CPU 的了，把运行队列也放进去只是顺水推舟。

还有一个值得一提的扩展方向是中断安全的通用化。当前 Spinlock 只提供了普通的 Guard，不涉及中断控制。未来可以考虑引入一个"中断安全的 Guard"——在获取锁之前先保存 RFLAGS 并关中断，释放锁之后用保存的 RFLAGS 恢复中断状态。这能防止持有 Spinlock 时被中断打断导致的自死锁。Linux 内核的 `spin_lock_irqsave()` / `spin_unlock_irqrestore()` 就是这个模式。

## 参考资料

- Intel SDM Vol. 2B, "PAUSE -- Spin Loop Hint"：`pause` 指令降低 spin-wait 功耗
- Intel SDM Vol. 3A, Section 8.7 "Task Management in 64-Bit Mode"：TSS.RSP0 在权限级切换时的作用
- Intel SDM Vol. 3A, Section 2.1.3 "Task-State Segments and Task Gates"：TSS 结构体中 RSP0/RSP1/RSP2 和 IST 字段
- GCC Manual, "Built-in Functions for Memory Model Aware Atomic Operations"：`__atomic_test_and_set` / `__atomic_clear` 语义
- xv6-riscv `trap.c` / `proc.c`：[https://github.com/mit-pdos/xv6-riscv](https://github.com/mit-pdos/xv6-riscv) — timer interrupt -> yield -> sched -> swtch 路径
- Linux `kernel/sched/core.c`, `scheduler_tick()`：[https://stackoverflow.com/questions/72227175](https://stackoverflow.com/questions/72227175) — Linux 的延迟抢占模型
- Linux `arch/x86/include/asm/percpu.h`：per-CPU 变量的 GS 段基址访问方式
