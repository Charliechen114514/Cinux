---
title: 020-proc-scheduler-1 · 进程调度
---

# 020-1 通读：Spinlock 原子锁、PerCPU 数据与 GDT TSS 更新——抢占调度的基础设施


## 概览

本文是 tag `020_proc_scheduler` 三篇通读教程的第一篇，聚焦抢占式调度所需的三块基础设施：`sync.hpp` 中的 Spinlock 原子自旋锁与 RAII Guard、`per_cpu.hpp` 中的 PerCPU 结构体、以及 `gdt.hpp` / `gdt.cpp` 中新增的 `tss_set_rsp0()` 方法。这些组件本身并不执行调度，但没有它们，抢占式调度就无法安全地运行——Spinlock 保护共享队列的并发访问，PerCPU 为未来 SMP 做好数据布局准备，TSS.RSP0 更新确保任务切换后内核栈指针正确。我们从 Spinlock 开始，然后看 PerCPU 的设计意图，最后分析 GDT 的修改。

如果你是从 tag 019 直接跳到这里的读者，可能会问：tag 019 不是已经有了 RoundRobin 队列和 Scheduler 吗，为什么还需要 Spinlock？答案是 tag 019 的调度是协作式的——`yield()` 只在任务主动调用时触发，整个调度过程是同步的，不存在并发问题。但 tag 020 的调度由定时器中断驱动，IRQ0 随时可能在任何执行点打断当前任务并调用 `schedule()`。如果 RoundRobin 的 `pick_next()` 正在移动队列元素时被中断打断，而中断 handler 又调用了同一个队列的 `dequeue()`，数据结构就会损坏。Spinlock 就是为了保护这些临界区而引入的。

如果你是从 tag 019 直接跳到这里的读者，可能会问：tag 019 不是已经有了 RoundRobin 队列和 Scheduler 吗，为什么还需要 Spinlock？答案是 tag 019 的调度是协作式的——`yield()` 只在任务主动调用时触发，整个调度过程是同步的，不存在并发问题。但 tag 020 的调度由定时器中断驱动，IRQ0 随时可能在任何执行点打断当前任务并调用 `schedule()`。如果 RoundRobin 的 `pick_next()` 正在移动队列元素时被中断打断，而中断 handler 又调用了同一个队列的 `dequeue()`，数据结构就会损坏。Spinlock 就是为了保护这些临界区而引入的。

## 架构图

```
    Spinlock + PerCPU + GDT 在调度中的角色：
    ┌──────────────────────────────────────────────────────────┐
    │  IRQ0 (定时器中断)                                       │
    │    └── Scheduler::tick()                                 │
    │          └── schedule()                                  │
    │                ├── RoundRobin::pick_next()               │
    │                │     └── lock_.guard() (Spinlock)        │
    │                ├── g_per_cpu.current = next (PerCPU)     │
    │                ├── GDT::tss_set_rsp0(stack) (GDT)        │
    │                └── context_switch(&prev->ctx, &next->ctx)│
    └──────────────────────────────────────────────────────────┘
```

这三块组件的协作关系可以从架构图中看到：IRQ0 触发后，`tick()` 调用 `schedule()`，schedule 内部通过 RoundRobin 的 `pick_next()` 选下一个任务（由 Spinlock 的 Guard 保护），然后更新 PerCPU 的 `current` 指针和 GDT 的 TSS.RSP0，最后执行 `context_switch` 完成物理切换。接下来我们逐个深入这些组件。

注意这个架构图展示的是完整运行时的调用链，但本文只覆盖 Spinlock、PerCPU 和 GDT 三块基础设施。`tick()`、`schedule()` 和 `context_switch` 的具体实现会在第二篇和第三篇中分析。第一篇的目标是理解"工具"本身，第二篇理解"调度逻辑"，第三篇理解"集成和调试"。

## 代码精讲

### Spinlock——原子 test-and-set 自旋锁

Tag 020 新增的第一个文件是 `kernel/proc/sync.hpp`，它定义了内核中最基础的同步原语。我们从 diff 中看到的版本开始，逐段分析每个方法的实现。

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
```

Spinlock 的核心思路很直接：用一个 `bool` 变量表示锁的状态，通过原子操作来保证只有一个线程能成功获取。`locked_` 初始化为 `false`，声明为 `volatile` 防止编译器把自旋循环优化成死循环。

`acquire()` 调用 GCC 内建函数 `__atomic_test_and_set`——这个函数把 `locked_` 原子地设为 `true`，同时返回旧值。如果旧值是 `true`（锁已被持有），就继续自旋。`__ATOMIC_ACQUIRE` 内存序保证了 `acquire` 之后的读操作不会被重排到 `acquire` 之前，这在保护队列数据时非常关键。自旋体内嵌了一条 `pause` 指令，这是 x86 专门为 spin-wait 循环优化的提示指令，能降低 CPU 功耗并减少流水线惩罚。Intel SDM Vol. 2B 对 `PAUSE` 的描述是"improves the performance of spin-wait loops"。

`release()` 使用 `__atomic_clear` 把 `locked_` 原子地设为 `false`，`__ATOMIC_RELEASE` 内存序确保 `release` 之前的所有写操作对其他 CPU 可见。这一对 acquire/release 语义恰好构成了一个完整的锁序关系：获取者读到 release 写入的 `false` 之后，能看到锁保护区域内的所有修改。

`guard()` 返回一个 RAII 包装对象，`[[nodiscard]]` 属性防止调用者忽略返回值——如果写了 `lock.guard()` 却不保存返回值，Guard 的析构函数会立即执行，锁就等于没加。

这里还有一个细节值得展开：为什么锁变量是 `volatile bool` 而不是 `std::atomic<bool>`？原因是 Cinux 的内核运行在 freestanding 环境中，`<atomic>` 头文件不一定可用，而 `__atomic_test_and_set` / `__atomic_clear` 是 GCC 的编译器内建函数，不需要任何头文件支持。`volatile` 在这里的作用是防止编译器把 `locked_` 缓存到寄存器中——每次循环迭代都必须真正从内存读取最新值。虽然在严格意义上 `volatile` 不能替代原子操作（它不保证多核可见性），但配合 `__atomic_*` 内建函数使用时，原子语义由后者提供，`volatile` 只是额外的编译器提示。

### RAII Guard——异常安全的锁持有

```cpp
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
```

Guard 的设计非常标准——构造时获取锁，析构时释放锁，禁用拷贝构造和拷贝赋值以防止意外的双重释放。使用方式也很简洁：`auto g = lock.guard();`，`g` 在作用域结束时自动析构。对比 Linux 内核的 `spin_lock()` / `spin_unlock()` 裸调用，RAII 模式在 C++ 中更加安全，因为即使在提前 return 或异常路径下也能保证锁被释放。

虽然 Cinux 当前的内核线程不会抛出 C++ 异常，但 RAII 的优势不仅在于异常安全——它还防止了程序员忘记在某个 early-return 路径上调用 `release()`。这类 bug 在内核代码中非常隐蔽，因为忘记释放锁不会立即崩溃，而是以难以复现的死锁形式出现。C++ 的局部对象按声明的逆序析构——如果你在一个函数里获取了多把锁，它们的 Guard 对象会按"最后声明的先释放"的顺序析构，即 LIFO 锁释放语义。大多数情况下这正是你想要的，但如果你需要特定的释放顺序，可以用额外的花括号块来控制 Guard 的生命周期范围。

值得注意的是，RoundRobin 的 `pick_next()` 中使用 `lock_.guard()` 来保护队列操作——这是 Spinlock 的 RAII Guard 的标准使用方式。Guard 对象在作用域结束时自动析构释放锁，`(void)g` 可以抑制编译器"未使用变量"的警告。

### PerCPU 结构体——SMP 布局的占位符

```cpp
struct PerCPU {
    Task*    current;
    uint64_t kernel_stack;
};

extern PerCPU g_per_cpu;
```

`PerCPU` 的设计意图是"每个 CPU 一份"的数据结构。在单核阶段，它只是一个普通的静态全局变量 `g_per_cpu`，但字段布局已经为 SMP 做好了准备。`current` 指向当前正在运行的任务——这个指针在每次 `schedule()` 时被更新，是调度器最频繁访问的字段之一。调度器的 `current()` getter 实际上就是返回 `current_` 静态变量，但 `g_per_cpu.current` 为未来的 SMP 场景提供了一个 per-CPU 快速访问路径：每个 CPU 通过 GS 段基址加上固定偏移就能定位到自己对应的 PerCPU 结构，不需要全局锁。

`kernel_stack` 记录当前任务的内核栈顶地址，用于用户态系统调用快速定位内核栈。在 tag 020 的 diff 中，PerCPU 只有这两个字段——这是刻意的最简设计，等需求明确后再扩展。

一个可能让你困惑的问题：既然有 `Scheduler::current_` 为什么还需要 `g_per_cpu.current`？答案是分层。`current_` 是调度器的内部状态，只有调度器自己会读写。`g_per_cpu.current` 是全局可见的 per-CPU 指针——中断处理程序、系统调用入口、未来 SMP 初始化代码都可能需要快速知道"当前 CPU 上跑的是谁"。把这两个概念分开，调度器内部用 `current_` 做调度决策，外部通过 `g_per_cpu.current` 做快速查询，职责更清晰。

在 scheduler.cpp 中，`g_per_cpu` 的初始化非常简单：

```cpp
PerCPU g_per_cpu{nullptr, 0};
```

两个字段全部清零。每次 `schedule()` 选出新任务后，都会更新 `g_per_cpu.current`。这种"初始化为空、使用时更新"的模式在内核代码中非常常见——全局状态在启动时全部清零，在运行过程中被各个子系统的初始化函数逐步填充。这样做的好处是保证了启动前所有全局状态都是确定的（全零），避免了未初始化变量导致的未定义行为。

对比 xv6 的 `struct cpu`（持有 `proc*`、`int noff`、`int intena` 等字段），Cinux 的 PerCPU 目前只保留了最核心的两个指针。当未来引入 SMP 时，每个 CPU 的 GS 基址会指向各自的 PerCPU 实例，通过 `swapgs` 指令在内核态和用户态之间切换。

实际上，per-CPU 数据结构在 Linux 中有更广泛的用途——不仅仅是 `current` 指针，还包括 per-CPU 运行队列（`rq`）、中断计数器、RCU 状态等。Cinux 选择从最简形式开始，逐步扩展。这种做法的好处是每次只引入最必要的字段，避免一次性设计过度复杂的结构体。

对比 xv6 的 `struct cpu`（持有 `proc*`、`int noff`、`int intena` 等字段），Cinux 的 PerCPU 目前只保留了最核心的两个指针。xv6 的 `noff` 和 `intena` 用于管理嵌套的中断关闭状态——类似于 Linux 的 `spin_lock_irqsave` 的嵌套计数。Cinux 在 tag 020 中不需要这些，因为当前的 Spinlock 不涉及中断控制。

### GDT tss_set_rsp0()——任务切换时更新内核栈

上面我们看了 Spinlock 如何保护共享数据结构、PerCPU 如何为每个 CPU 保存运行时状态。接下来要解决的问题是：当 CPU 从用户态（ring 3）通过中断或系统调用进入内核态（ring 0）时，它怎么知道该用哪个栈？答案是 TSS 中的 RSP0 字段。每次任务切换时，我们必须把这个字段更新为新任务的内核栈顶。

在 `gdt.hpp` 中新增了一行声明：

```cpp
static void tss_set_rsp0(uint64_t rsp0);
```

对应实现在 `gdt.cpp` 中：

```cpp
void GDT::tss_set_rsp0(uint64_t rsp0) {
    g_gdt.tss_.rsp[0] = rsp0;
}
```

这一行代码看起来平淡无奇，但它解决了一个非常重要的硬件约束。Intel SDM Vol. 3A 第 8.7 节说明，在 64 位模式下 TSS 的 `RSP0` 字段在 CPU 从 ring 3 切换到 ring 0 时被自动加载到 RSP。也就是说，如果未来某个任务在用户态执行时触发了中断或系统调用，CPU 会用 TSS.RSP0 作为内核栈。如果不更新这个字段，所有用户态任务都会共享同一个内核栈——这是灾难性的。

把它声明为 `static` 方法是因为 GDT 实例 `g_gdt` 是全局唯一的。调用者不需要持有 GDT 引用，直接通过 `GDT::tss_set_rsp0(addr)` 即可。这个方法在每次 `schedule()`、`exit_current()` 和 `run_first()` 中都会被调用，确保 TSS.RSP0 始终指向当前任务的内核栈顶。

回过头来看 TSS 结构体的定义，`rsp` 是一个包含 3 个元素的数组，分别对应 RSP0、RSP1、RSP2——即从 ring 3/2/1 切换到 ring 0 时加载的栈指针。Cinux 只使用 RSP0，因为 x86_64 的特权级实际上只用到了 ring 0（内核）和 ring 3（用户），RSP1/RSP2 永远不会被触发。TSS 中还有 IST1-IST7 字段用于中断栈表——Cinux 用 IST1 作为 Double Fault 的专用栈，在 GDT 初始化时通过 `tss_.ist[0] = df_stack_top` 设置。普通中断不使用 IST 机制，走的是 RSP0 路径。

理解了 RSP0 的硬件语义之后，`tss_set_rsp0()` 为什么必须在每次上下文切换时调用就很好理解了：每个任务有自己独立的内核栈（由 `TaskBuilder::build()` 分配的 4 页连续虚拟内存），如果切到任务 B 但 TSS.RSP0 还指向任务 A 的栈，那么任务 B 在用户态触发中断时会用到 A 的栈——轻则数据损坏，重则直接 triple fault。

有趣的是，在纯内核线程阶段（ring 0 到 ring 0 切换），CPU 根本不看 TSS.RSP0——因为特权级没变，不需要从 TSS 加载栈指针。所以当前阶段这个调用其实是"预防性的"，在用户态支持上线之前不会有实际效果，但养成每次切换都更新的习惯可以避免将来忘记。Linux 在 `context_switch()` -> `switch_to()` 中也有类似的操作——通过 `load_sp0` 更新 `tss_struct` 的 `x86_tss.sp0`。这种"先占位后生效"的模式在内核开发中非常常见。

TSS 中还有 IST1-IST7 字段用于中断栈表——Cinux 用 IST1 作为 Double Fault 的专用栈，在 GDT 初始化时通过 `tss_.ist[0] = df_stack_top` 设置。普通中断不使用 IST 机制，走的是 RSP0 路径。IST 机制的设计初衷是处理"栈已经损坏"的严重异常——Double Fault 发生时内核栈可能已经不可用，CPU 切换到 IST 指定的独立栈来保证异常处理程序有可用的栈空间。

## 设计决策

**Spinlock 为什么是 header-only 的？** 因为 `sync.hpp` 中所有方法都在类定义中内联实现。这种设计在内核中很常见——锁操作通常极短（几条指令），内联后避免了函数调用开销。如果 Spinlock 的实现放在单独的 .cpp 文件中，编译器在编译调用者时看不到实现，无法内联，每次 acquire/release 都是一次函数调用——虽然开销很小（几纳秒），但在高频锁操作中会累积。

**Spinlock 为什么用 GCC 内建原子操作而非内联汇编？** `__atomic_test_and_set` 和 `__atomic_clear` 是 GCC 提供的编译器内建函数，比手写内联汇编更安全——编译器理解它们的语义，能够正确地与周围的代码进行指令重排优化。而内联汇编是编译器的黑盒，往往需要额外的 `volatile` 和 `"memory"` clobber 来保证正确性。此外，`__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE` 语义精确匹配了 C11 memory model 的要求，在所有架构上都有对应的硬件指令映射。如果有一天 Cinux 要移植到 RISC-V 或 ARM，这些内建函数会自动映射到对应架构的原子指令（如 RISC-V 的 `lr.d` / `sc.d`），而内联汇编就必须全部重写。一个反面例子是，早期的 Linux 内核大量使用内联汇编实现原子操作，后来逐步迁移到 `atomic_t` 封装层，正是因为可移植性的考量。

**为什么 acquire/release 而不是 seq_cst？** `__ATOMIC_ACQUIRE` 和 `__ATOMIC_RELEASE` 是最轻量级的锁序对。`acquire` 保证之后的读写不会被重排到之前，`release` 保证之前的读写不会被重排到之后。更强的 `__ATOMIC_SEQ_CST`（顺序一致性）会插入完整的内存屏障（`mfence`），在 x86 上代价更高。对于锁操作来说，acquire/release 已经足够——锁保护区域内的所有操作自然地被包含在 acquire-release 对之间。

**为什么 `volatile bool` 而不是 `std::atomic<bool>`？** Cinux 的内核运行在 freestanding 环境中，`<atomic>` 头文件不一定可用，而 `__atomic_test_and_set` / `__atomic_clear` 是 GCC 的编译器内建函数，不需要任何头文件支持。`volatile` 的作用是防止编译器把 `locked_` 缓存到寄存器中——每次循环迭代都必须真正从内存读取最新值。一个反面例子是早期的 Linux 内核大量使用内联汇编实现原子操作，后来逐步迁移到 `atomic_t` 封装层，正是因为可移植性的考量。

**PerCPU 为什么不直接用 `ThreadLocal`？** C++ 的 `thread_local` 在裸机内核中不可用——它依赖操作系统的 TLS 支持，而 Cinux 本身就是那个操作系统。用 GS 段基址 + 固定偏移量是 x86_64 内核实现 per-CPU 数据的标准做法，Linux 也是这么做的（`this_cpu_read` / `this_cpu_write` 系列宏）。

**tss_set_rsp0 为什么在每次 schedule 都要调用？** 因为每次上下文切换都可能切到一个不同的任务，而每个任务有独立的内核栈。如果遗漏了更新，用户态中断就会用错误的栈。Linux 在 `context_switch()` -> `switch_to()` 中也有类似的操作（通过 `load_sp0` 更新 `tss_struct` 的 `x86_tss.sp0`）。有趣的是，在纯内核线程阶段（ring 0 -> ring 0 切换），CPU 根本不看 TSS.RSP0——因为特权级没变，不需要从 TSS 加载栈指针。所以当前阶段这个调用其实是"预防性的"，在用户态支持上线之前不会有实际效果，但养成每次切换都更新的习惯可以避免将来忘记。

## 扩展方向

**Ticket Spinlock：** 当前的 Spinlock 是最简单的 test-and-set 自旋锁。在 SMP 环境下，多个 CPU 同时自旋时会导致总线争用和 cache line bouncing。Linux 使用 ticket spinlock（或 MCS lock）来保证 FIFO 公平性和更低的 cache 压力。当 Cinux 引入 SMP 支持时，这是一个值得优先升级的组件。

**PerCPU 运行队列：** 当前 RoundRobin 是全局唯一的队列。在多核系统中，全局队列的 Spinlock 竞争会成为瓶颈。Linux 的 CFS 使用 per-CPU 运行队列（`struct rq`），每个 CPU 在本地队列上做调度决策，只在负载均衡时才访问其他 CPU 的队列。Cinux 可以把 `RoundRobin` 实例移到 `PerCPU` 结构体中，作为 SMP 扩展的第一步。

**中断安全的通用化：** 当前 Spinlock 只提供了普通的 `Guard`。如果未来需要在持有 Spinlock 时防止中断干扰（比如 RoundRobin 的队列操作可能被定时器中断打断），可以考虑引入一个"中断安全的 Guard"——获取锁之前先关中断，释放锁之后恢复中断状态。Linux 内核的 `spin_lock_irqsave()` / `spin_unlock_irqrestore()` 做的就是这件事。在 tag 020 中我们使用普通 Guard 是因为 `tick()` 调用链在中断门语义下 IF 已经被清零，不会触发另一个 IRQ0，所以普通 Guard 在当前阶段是安全的。

**PerCPU 运行队列：** 当前 RoundRobin 是全局唯一的队列。在多核系统中，全局队列的 Spinlock 竞争会成为瓶颈。Linux 的 CFS 使用 per-CPU 运行队列（`struct rq`），每个 CPU 在本地队列上做调度决策，只在负载均衡时才访问其他 CPU 的队列。Cinux 可以把 `RoundRobin` 实例移到 `PerCPU` 结构体中，作为 SMP 扩展的第一步。

到这里，我们已经完成了 tag 020 三块基础设施的代码精讲。Spinlock 提供了最底层的互斥原语，PerCPU 为 per-CPU 数据访问铺好了路，TSS.RSP0 更新把硬件级别的栈切换机制和软件调度器连接了起来。接下来第二篇我们将深入 scheduler.cpp 的核心逻辑——tick/schedule/block/unblock 的完整实现。

### 与 tag 019 的代码对比

从 tag 019 到 tag 020，`sync.hpp` 和 `per_cpu.hpp` 是全新文件，`gdt.hpp/cpp` 的改动很小（新增一个静态方法）。但 scheduler.hpp/cpp 的变化很大——新增了 6 个函数声明和 4 个静态成员变量。如果你用 `git diff 019..020 -- kernel/proc/scheduler.hpp` 查看差异，会看到类声明从约 15 行增长到了约 35 行。这个增长反映了从"协作式调度"到"抢占式调度"的复杂度跃升——时间片管理、idle 任务、block/unblock 都是抢占式调度独有的概念，协作式调度不需要这些。

对比其他操作系统的类似演进路径，可以看到一个有趣的共性。xv6-riscv 从第一个版本就同时支持了 `usertrap()` 中的时钟中断处理（`which_dev == 2` 时调用 `yield()`），没有经历从协作式到抢占式的迁移。Linux 在 2.4 时代完成了从大内核锁到细粒度锁的重构，和 Cinux 从无锁到 Spinlock 的演进方向一致，但规模大得多。Cinux 的演进路径是教学内核中比较典型的——先做最简单的（协作式），再逐步增加复杂度（抢占式），每一步引入的概念都是必要的。

## 参考资料

- Intel SDM Vol. 2B, "PAUSE -- Spin Loop Hint": `pause` 指令降低 spin-wait 功耗
- Intel SDM Vol. 3A, Section 2.1.3 "Task-State Segments and Task Gates" + Section 8.7 "Task Management in 64-Bit Mode": TSS.RSP0 在权限级切换时的作用
- Intel SDM Vol. 3A, Section 8.1 "Locked Atomic Operations": x86 LOCK 前缀和原子操作语义
- GCC Manual, "Built-in Functions for Memory Model Aware Atomic Operations": `__atomic_test_and_set` / `__atomic_clear` 语义
- Linux `arch/x86/include/asm/percpu.h`: per-CPU 变量的 GS 段基址访问方式
- Linux `arch/x86/kernel/process.c`, `switch_to()` -> `__switch_to()`: Linux 通过 `load_sp0` 更新 TSS.RSP0 的实现
- xv6-riscv `proc.c`: xv6 的 `struct cpu` 作为 per-CPU 数据的对比参考
- OSDev Wiki, "Scheduling Algorithms": Round Robin 时间片建议 20ms-50ms
- document/notes/020/001_time_slice_too_long.md: 时间片过长问题的调试记录
