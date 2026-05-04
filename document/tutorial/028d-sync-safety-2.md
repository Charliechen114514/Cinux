# 全面加锁实战——三层分级策略与 RAII 守卫设计

> 标签：自旋锁、线程安全、PMM、Heap、Scheduler、原子操作
> 前置：[028d-1 自旋锁原理](./028d-sync-safety-1.md)

## 前言

上一章我们搭好了 Spinlock 和 RAII 守卫的基建，现在该动手把锁加到内核的每个角落了。你可能会觉得这是个体力活——到处加 `auto g = lock_.guard(); (void)g;` 就完事了。但说实话，真正做起来远没那么简单。加少了会留下数据竞争的隐患，加多了会增加锁争用甚至引入死锁。更棘手的是，不同组件面临的风险等级完全不同——PMM 的位图操作如果被并发踩踏会立刻崩，而某个计数器多算一次少算一次系统还能接着跑。

我们需要一个分级策略，按风险等级决定保护强度。这就是本文的核心——把内核组件分成三个 Tier，每个 Tier 用最合适的同步手段保护。

## 环境说明

本文涉及的代码跨越 kernel/mm（PMM、Heap、VMM）、kernel/proc（Scheduler、Process）、kernel/fs（FDTable、VFS mount）和 kernel/drivers（Keyboard、PIT）。所有改动在 QEMU x86_64 上验证，工具链不变——GCC 14 + CMake。

## Tier 0：核心分配器——数据竞争必然崩溃

最危险的一批组件是 PMM、Heap 和 Scheduler 的运行队列。它们是内核最底层的共享资源，所有线程都在用，内部数据结构（位图、空闲链表、环形数组）在并发操作下会立刻损坏。

### PMM：锁的拆分模式

PMM 的核心是一个位图——每一位代表一个物理页，0 表示空闲，1 表示已用。`alloc_page` 需要扫描位图找到第一个 0，然后设为 1，同时减少空闲计数。如果两个线程同时执行这个操作，"扫描+设置"之间被打断，两个线程可能拿到同一个物理页——这就是典型的 TOCTOU (Time-of-Check-to-Time-of-Use) 竞争。

加锁方式是把原来的公共方法拆成"带锁的公共入口"和"不带锁的内部实现"：

```cpp
uint64_t PMM::alloc_page_locked() {
    int64_t idx = bm_find_first_free(bitmap_, highest_page_, bitmap_size_);
    if (idx < 0) return 0;
    bm_set(bitmap_, static_cast<uint64_t>(idx));
    free_pages_--;
    return static_cast<uint64_t>(idx) * PAGE_SIZE;
}

uint64_t PMM::alloc_page() {
    auto g = lock_.guard();
    (void)g;
    return alloc_page_locked();
}
```

为什么需要拆分而不是直接在 alloc_page 里加锁？因为 `alloc_pages`（分配连续多页）在持锁状态下需要调用单页分配的逻辑。如果 count==1 的分支调用了公共 `alloc_page()`，后者会再次尝试获取同一个 Spinlock——自旋锁不支持递归获取，同一个 CPU 两次 acquire 同一个锁会永远自旋，也就是死锁。`alloc_page_locked()` 的命名就是契约："调用者必须已持有锁"。

这一点真的坑了我半天——`alloc_pages` 里 count==1 的分支一开始写成了 `return alloc_page()`，编译没问题，运行直接卡死。后来才反应过来持锁状态下不能调用公共版本。改成了 `return alloc_page_locked()`，问题立刻消失。

### Heap：递归重试的拆分

Heap 的加锁模式和 PMM 类似，但有一个额外的复杂度。`alloc` 如果在空闲链表中找不到合适的 block，会调用 `expand` 扩展堆空间，然后递归调用自身重试。这个递归必须走 `alloc_locked` 而不是公共 `alloc`——原因和 PMM 一样，公共版本会再次获取锁导致死锁。

```cpp
void* Heap::alloc_locked(size_t size, size_t align) {
    // ... first-fit 搜索 ...
    // 没找到合适的 block
    expand(size + align + HEADER_SIZE);
    return alloc_locked(size, align);  // 递归走 _locked 版本
}

void* Heap::alloc(size_t size, size_t align) {
    auto g = lock_.guard();
    (void)g;
    return alloc_locked(size, align);
}
```

expand 内部会调用 PMM 的 alloc_page 和 VMM 的 map——这些调用各自有自己的锁（PMM 的 lock_ 和 VMM 的 lock_），和 Heap 的 lock_ 是不同的对象，不会产生死锁。这种"不同锁的嵌套"在内核中很常见，只要保证获取顺序一致（比如总是先 Heap 锁再 PMM 锁）就不会出问题。

### Scheduler：IrqGuard 的必要性

Scheduler 的运行队列加锁有一个特殊要求——必须用 IrqGuard 而不是普通 Guard。原因是 `tick()` 从时钟中断处理程序中被调用，中断处理程序可能在任何线程持锁时打断它。如果线程 A 正在执行 `enqueue` 持有普通 Guard（没关中断），tick 中断到来，中断处理程序调用 `pick_next` 试图获取同一个锁——死锁。

```cpp
void RoundRobin::enqueue(Task* task) {
    auto g = lock_.irq_guard();
    (void)g;
    // ... 入队逻辑 ...
}
```

`dequeue` 和 `pick_next` 完全相同的模式。IrqGuard 在获取锁之前先保存 RFLAGS 并执行 cli 禁用中断，确保在持锁期间不会有中断打断。

对比一下 Linux 的做法——Linux 的 `spinlock_irqsave` 做的事情和我们的 IrqGuard 完全一样：保存当前 CPU 的中断状态到局部变量，禁用中断，获取自旋锁。区别在于 Linux 把 flags 保存在函数的局部变量里（通过宏参数传递），而 Cinux 把 flags 保存在 RAII 对象的成员变量里。Linux 的方式更灵活（同一个 spinlock 可以在不同上下文中用不同方式保护），Cinux 的方式更安全（不可能忘记恢复 flags）。

xv6-riscv 的做法又有不同——它用 `push_off`/`pop_off` 维护一个 per-CPU 的中断禁用嵌套计数器 `noff`，以及一个 `intena` 标志记录 push_off 之前中断是否启用。这比 pushfq/popfq 更复杂，但好处是不需要为每个 Guard 对象分配 saved_flags_ 成员变量。对于 Cinux 的单核环境，pushfq/popfq 方案更简洁。

## Tier 1：原子计数器——只需原子性就够了

有些变量不需要锁的保护，只需要保证读写的原子性。PIT 的 tick 计数器、Scheduler 的 tick_count_ 和 current_slice_、TaskBuilder 的 next_tid 都属于这一类。它们被频繁读写，但单次不一致不会导致系统崩溃。

```cpp
// PIT
static std::atomic<uint64_t> tick_count_{0};
tick_count_.fetch_add(1, std::memory_order_relaxed);
return tick_count_.load(std::memory_order_relaxed);

// TaskBuilder
std::atomic<uint64_t> next_tid{1};
task->tid = next_tid.fetch_add(1, std::memory_order_relaxed);
```

全部使用 `memory_order_relaxed`——我们只需要原子性，不需要与其他操作建立顺序保证。在 x86 上，`fetch_add(relaxed)` 被编译为 `lock xadd` 指令，`load(relaxed)` 被编译为普通的 mov 指令（x86 的所有加载都有 acquire 语义），开销极小。

TaskBuilder 的 `next_stack_vaddr` 替换有一个精妙之处。原来的代码是"读取当前值 + 加上偏移量"两步操作，改成 `fetch_add` 后变成一步原子操作——返回加之前的旧值，同时把新值写回。两个线程同时调用 fetch_add 绝不会得到相同的返回值。

## Tier 2：中等风险——Spinlock Guard 足矣

文件系统组件的并发风险相对较低，但在多任务环境下仍需要保护。FDTable 加一个 Spinlock 防止两个线程同时 alloc 拿到同一个 slot（double-alloc），或者一个线程 close 了另一个线程正在用的 fd（use-after-free）。File 的 offset 加一个 Spinlock 保证 sys_read/sys_write 中"读取 offset + 操作 + 更新 offset"的原子性。VMM 的 map/unmap 加锁保护页表修改。全局 mount 表加一个 Spinlock 保护三个操作。

键盘驱动的 `poll()` 比较特殊——它用的是 InterruptGuard 而不是 Spinlock。原因是 IRQ1 handler 不获取任何锁（它在中断上下文中直接写入 ring buffer），poll 只需要确保读取时不会被 IRQ1 打断就够了。InterruptGuard 比 IrqGuard 更轻量——只禁中断不获取锁。

Linux 对 mount 表保护用的是 RCU（Read-Copy-Update）——读端完全无锁，写端通过指针替换（publish new version, then wait for old readers to leave）实现。Cinux 目前不需要这么复杂——mount 操作频率极低，Spinlock 足够了。但如果将来 mount 表变成性能热点，RCU 是升级方向。

## 收尾

验证方式是在下一章中用三层测试体系来验证——Host 端的多线程压力测试、QEMU 内核态的 RAII 守卫测试、以及生产内核的抢占式多任务压力测试。

到这里，内核的所有共享数据结构都加上了保护。PMM 和 Heap 用 _locked 拆分模式避免死锁，Scheduler 用 IrqGuard 中断安全，计数器用 std::atomic 轻量保护，文件系统用普通 Spinlock Guard。三层分级策略确保了每个组件用最合适的同步手段——不过保护，也不欠保护。下一章我们开始验证这些保护真的管用。

## 参考资料

- Intel SDM: Vol.3A Section 8.1 — Locked Atomic Operations，LOCK 前缀与原子指令语义
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives)
- xv6-riscv: [spinlock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c) — push_off/pop_off 嵌套中断禁用
- Linux: [mutex-design.rst](https://docs.kernel.org/locking/mutex-design.rst) — Linux mutex 快速路径/慢速路径/乐观自旋设计
- Linux: [spinlock.h](https://github.com/torvalds/linux/blob/master/include/linux/spinlock.h) — spinlock_irqsave 宏
