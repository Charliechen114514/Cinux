# 021-2 Mutex：阻塞式互斥锁设计

> 前置：[021-1 Spinlock 与等待队列](021-proc-sync-1.md)
> 标签：mutex, blocking, FIFO, owner tracking, RAII guard

## 导语

上一章我们搭好了等待队列的基础设施——Spinlock 的实现从头文件移到了 sync.cpp，Task 结构体加上了 `wait_next` 字段，enqueue/dequeue 的尾插头删逻辑也写好了。现在问题来了：Spinlock 虽然简单，但如果你需要在锁住的时候做一件很耗时的事——比如等磁盘 I/O 完成、等另一个任务往缓冲区里写入数据——那自旋就变成了纯粹的 CPU 浪费。我们需要一种"拿不到锁就睡觉，别人释放了再叫醒我"的机制。这就是 Mutex 要解决的问题。

完成本章后，你会理解阻塞式互斥锁的完整生命周期，以及为什么 Mutex 内部需要用 Spinlock 来保护自己的元数据。

## 概念精讲

### Spinlock vs Mutex——什么时候用哪个

这是一个很多新手 OS 开发者会困惑的问题。核心区别在于：Spinlock 让等待者忙等（占用 CPU），Mutex 让等待者睡眠（释放 CPU）。忙等适合临界区极短（几条指令）的场景，比如修改一个链表头指针；睡眠适合临界区较长或者需要在锁内等待条件的场景。

在 Cinux 的设计里，Spinlock 被用作 Mutex 和 Semaphore 的"内部锁"——它只保护这些同步原语自身的元数据（owner 指针、等待队列、计数器），操作完立即释放，持有时间通常不超过十来条指令。而 Mutex 才是面向内核其他模块的"外层锁"。

这里有一条硬性规则：**永远不要在持有 Spinlock 的时候调用 block 或 schedule**。Spinlock 忙等期间，如果触发了上下文切换，下一个任务要拿同一把 Spinlock 就死锁了。Mutex 的设计正是为了避免这个问题——它的 Spinlock 在 block 之前就已经释放。

### Mutex 的三个核心状态

一个 Mutex 包含三样东西：一个 Spinlock（保护内部状态）、一个 owner 指针（谁持有这把锁）、一个 wait_head（等待队列）。这三个字段在 Mutex 对象创建时全部为零初始化——没有人持有锁，没有人在等。构造函数是 default 的，依赖 C++ 的零初始化规则把所有指针设为 nullptr。

这三个字段的分工很清晰：Spinlock 是"元锁"，保护 owner 和 wait_head 的修改不被并发打断；owner 回答"谁拿着锁"的问题；wait_head 指向等待队列的头部，回答"谁在排队"的问题。三者配合实现了 Mutex 的完整语义。

### lock() 的五步流程

获取 Mutex 的过程分为五个精确的步骤，每一步都有明确的目的。

第一步，获取内部 Spinlock——这保证了我们检查和修改 owner 的时候不会被其他 CPU（或中断）打断。第二步，检查 owner 是不是 nullptr。如果是空的，太好了，把当前任务的指针（`g_per_cpu.current`）写进 owner，释放 Spinlock，返回——这是无竞争的快速路径，整个操作只有几条指令。第三步，如果 owner 不为空，说明锁被别人拿着，我们需要把当前任务加入等待队列的尾部（enqueue）。第四步，关键一步——释放 Spinlock。为什么要在 block 之前释放？因为 block 会调用 schedule 切换上下文，如果这时候 Spinlock 还被拿着，下一个任务如果要操作同一个 Mutex 就会死锁。第五步，调用 `Scheduler::block()` 把当前任务挂起，CPU 开始跑别的任务。

你可能觉得第三步和第四步之间有竞态条件：我们先把任务加入等待队列，然后释放 Spinlock，再 block。如果在这个窗口期有人 unlock 了呢？没关系——unlock 会把等待队列里的任务取出来 unblock，所以即使 block 之前就被唤醒了，block 的实现也只需要把任务状态设为 Blocked 然后调用 schedule，而 schedule 检查到任务已经不是 Running 状态就会正确处理。这个设计在单核环境下是安全的，因为 Cinux 目前不支持 SMP，在 Spinlock 释放和 block 调用之间不会被另一个任务抢占。

### unlock() 的所有权转移

unlock 的关键设计在于"所有权转移"：如果等待队列里有任务，unlock 不会把 owner 清零，而是直接把等待队列头部的任务设为新 owner。这样做的好处是保证了公平性——等待最久的任务优先获得锁。同时，在 Spinlock 释放之后、unblock 之前，新 owner 已经确立，其他尝试 lock 的任务会看到 owner 不为空，正确地排队等待。

这个设计和 xv6 的 sleeplock 形成了鲜明对比。xv6 的 `wakeup(chan)` 会唤醒所有在同一个通道上等待的进程——这就是 thundering herd 问题，所有被唤醒的进程抢一把锁，最终只有一个能拿到，其余的又回去睡觉。Cinux 的 FIFO 所有权转移避免了这个问题，每次只有一个等待者被唤醒。

### try_lock() 和 guard()

try_lock 是非阻塞版本的 lock：检查 owner，如果为空就占据，否则直接返回 false。它适用于那些"拿不到就算了"的场景，比如在中断处理程序中尝试获取锁，拿不到就不等了直接返回。

guard() 返回一个 RAII 对象，构造时调用 lock()，析构时调用 unlock()。标记为 `[[nodiscard]]` 是为了防止你不小心写了 `m.guard()` 但没有把返回值存起来——那样的话临时对象立即析构，锁拿到手又立刻释放了，跟没锁一样。这个属性会让编译器发出警告，帮你在编译期就抓到这个 bug。

## 动手实现

### Step 1: 定义 Mutex 类

**目标**: 在 sync.hpp 中定义 Mutex 类的接口。

**设计思路**: Mutex 需要公开 lock()、unlock()、try_lock() 和 guard() 四个方法。内部持有三个字段：Spinlock 保护元数据、Task* owner 追踪持有者、Task* wait_head_ 管理等待队列。还需要私有的 enqueue_waiter 和 dequeue_waiter 辅助方法，以及一个嵌套的 Guard RAII 类。在 sync.hpp 中需要前置声明 `struct Task;`，因为 Task 定义在 process.hpp 中而 sync.hpp 不想直接包含它。

**实现约束**: Guard 类需要禁用拷贝构造和拷贝赋值（`delete`），防止误拷贝导致同一把锁被 unlock 两次。Guard 的构造函数调用 lock()，析构函数调用 unlock()。构造函数标记为 `explicit` 防止隐式转换。

**验证**: 类定义编译通过即可。确保 sync.hpp 的 include 顺序正确——只依赖 `<stdint.h>`。

### Step 2: 实现 lock() 和 unlock()

**目标**: 按照前面描述的五步流程实现 Mutex 的核心方法。

**设计思路**: lock() 先拿 Spinlock，检查 owner。空则占据，否则入队等待，释放 Spinlock 后 block。unlock() 先拿 Spinlock，取等待队列头。空则清 owner，否则转移所有权给等待者，释放 Spinlock 后 unblock 等待者。

**踩坑预警**: unlock 中释放 Spinlock 和 unblock 的顺序不能颠倒——必须先释放 Spinlock 再 unblock，否则 unblock 可能让另一个 CPU 上的任务立刻被调度到，尝试 lock 同一个 Mutex 时发现 Spinlock 还被占着，造成不必要的自旋甚至死锁。这是实现阻塞式同步原语时最容易犯的错误之一。

另一个需要注意的点是：我们的 Mutex 不支持递归锁。如果同一个任务连续两次调用 lock()，第二次会看到 owner 不为空（因为就是自己），然后把自己加入等待队列并 block——但没有人会来 unlock，于是死锁。如果需要递归语义，需要额外增加一个递归计数器。

**验证**: 以下场景应该全部通过——单任务 lock 后立刻 unlock（无竞争）、两个任务竞争同一 Mutex 验证 FIFO 顺序（第一个 lock 成功，第二个 lock 被阻塞，unlock 后第二个被唤醒并成为新 owner）。内核集成测试中 `test_fifo_ordering` 测试用三个等待者验证了这个场景。

### Step 3: 实现 try_lock() 和 guard()

**目标**: 补全非阻塞获取和 RAII 包装。

**设计思路**: try_lock 就是一个简化版的 lock——只走快速路径：获取 Spinlock，检查 owner，为空则占据并返回 true，不为空返回 false。不需要操作等待队列。guard() 返回 Guard 对象，Guard 的生命周期自动管理锁的获取和释放。

**验证**: 验证 try_lock 在 Mutex 空闲时成功、被持有时失败。验证 guard 在作用域结束时自动 unlock——可以用 try_lock 来检测：guard 析构后 try_lock 应该能成功。内核测试中 `test_try_lock_free` 和 `test_try_lock_held` 分别验证了这两种场景。

## 从 Spinlock 到 Mutex 的演进路径

理解了 Spinlock 和 Mutex 的区别之后，我们来看一下从 Spinlock 到 Mutex 的演进路径，这有助于理解为什么 Mutex 的内部设计是这样的。这个演进路径本质上也是操作系统同步原语的发展历史——从最简单的忙等到高效的阻塞等待。

第一步：Spinlock 能做互斥，但它让等待者忙等。如果临界区只有几条指令没问题，但如果临界区长（比如需要等磁盘 I/O），CPU 时间就浪费在空转上了。在单核环境下这个问题尤其严重——自旋的任务占着 CPU，持有锁的任务根本没机会运行来释放锁。这其实就是死锁。

第二步：在 Spinlock 之上加一个"owner"指针。lock 时先拿 Spinlock，检查 owner，如果为空就占据。如果不为空，说明别人拿着锁。关键问题来了：拿不到锁怎么办？Spinlock 是继续转圈等，但我们要做的是让当前任务睡眠。

第三步：引入等待队列。拿不到锁的任务不是转圈等，而是把自己加入一个等待队列，然后调用 block 让出 CPU。等待队列需要用 Spinlock 保护（因为 enqueue 和 dequeue 可能在不同任务中并发执行），但 Spinlock 只在操作队列时短暂持有，block 之后立刻释放。这就是为什么 Mutex 内部需要一个 Spinlock——它保护的不是用户数据，而是 Mutex 自身的元数据。

第四步：unlock 时做所有权转移。不是简单地把 owner 清零让所有等待者重新竞争，而是把等待队列头的任务直接设为新 owner。这避免了 thundering herd 问题，保证了 FIFO 公平性。

这就是 Mutex 从 Spinlock 演进出来的完整路径。每一步都解决了一个具体问题：忙等 -> 睡眠、竞争 -> 队列、惊群 -> 所有权转移。理解了这个路径，你就能更好地理解 Mutex 的每个设计决策。

## Mutex 的状态机视角

把 Mutex 的行为理解为一个状态机会非常有帮助。Mutex 有三种状态：Free（无人持有）、Held（有人持有，无等待者）、Contended（有人持有，有等待者）。

Free 状态下，任何任务调用 lock() 都会直接走到快速路径：获取 Spinlock，发现 owner 为空，设置 owner 为自己，释放 Spinlock，返回。状态变为 Held。这个路径只需要几条指令，性能开销极小。

Held 状态下，如果有任务调用 lock()，它会走到慢速路径：获取 Spinlock，发现 owner 不为空，把自己加入等待队列，释放 Spinlock，调用 block。状态变为 Contended。

Contended 状态下，如果 owner 调用 unlock()，它不会把 owner 清零，而是直接把等待队列头部的任务设为新 owner，然后 unblock 那个任务。如果等待队列变空了，状态回到 Held；如果还有等待者，状态保持 Contended。只有在 Contended -> unlock 最后一个等待者时，状态才回到 Held。

Contended 状态下，如果 owner 再次调用 unlock()，每次都唤醒队列头的下一个等待者。如果三个任务 A、B、C 按顺序等待，第一次 unlock 唤醒 A（owner 变为 A），第二次 unlock 唤醒 B（owner 变为 B），第三次 unlock 唤醒 C（owner 变为 C）。这就是 FIFO 公平性的保证。

try_lock() 在任何状态下都只尝试快速路径——如果 owner 为空就占据并返回 true，否则返回 false。它不会改变 Mutex 的状态。

## 构建与运行

内核测试会创建虚拟任务模拟竞争场景：

```bash
cd build && cmake .. && make run-kernel-test -j$(nproc)
```

串口输出中应该能看到 "Sync Tests (021)" 段的所有测试通过，包括 mutex 相关的 `test_lock_unlock`、`test_try_lock_free`、`test_try_lock_held`、`test_lock_blocks_and_enqueues`、`test_unlock_transfers_to_waiter`、`test_fifo_ordering` 和 `test_guard_scope` 测试。

Host 端单元测试也有对应的 Mutex 测试：

```bash
cd build && make test_sync -j$(nproc) && ./test/unit/test_sync
```

预期输出中 Mutex 相关测试应该全部 PASS，包括 `mutex: lock sets owner to current`、`mutex: unlock clears owner when no waiters`、`mutex: try_lock succeeds on free mutex`、`mutex: try_lock fails on held mutex`、`mutex: lock on held mutex blocks and enqueues`、`mutex: unlock transfers ownership to waiter`、`mutex: FIFO ordering of waiters` 和 `mutex: guard locks and unlocks`。

## 扩展方向：Mutex 的进阶话题

虽然 Cinux 的 Mutex 实现已经覆盖了基本功能，但有几个重要的进阶话题值得了解，特别是当你将来要支持 SMP 或者实现更复杂的内核模块时。

**Priority Inheritance（优先级继承）**: 当高优先级任务在低优先级任务持有的 Mutex 上阻塞时，临时提升持有者优先级，防止优先级反转。这在实时系统中是必须的功能，Linux 的 `rt_mutex` 就实现了这个机制。实现方式是在 Mutex 中额外追踪等待者中最高优先级的那一个，把它临时赋给 owner。

**Recursive Mutex（递归锁）**: 允许同一个任务对同一个 Mutex 多次 lock，内部用一个递归计数器追踪。每次 lock 递增计数器，每次 unlock 递减，减到 0 才真正释放。PintOS 的 Lock 不支持递归，Linux 有单独的 `mutex_trylock` 路径处理递归尝试。

**Spinlock 中断安全**: 当前实现没有在 acquire 时禁用中断。如果在持锁期间发生了定时器中断，中断处理程序可能尝试操作同一个 Mutex，导致死锁。xv6 通过 `push_off`/`pop_off` 机制在 Spinlock 层面管理中断状态。这是 Cinux 将来需要补上的一个重要安全特性。

## 调试技巧

1. **死锁**: 如果系统挂死在 Mutex 操作上，最可能的原因是在 Spinlock 持有期间调用了 block 或 schedule。用串口输出在 lock/unlock 的每个步骤打印 trace 来定位。具体来说，在 spin_.acquire() 和 spin_.release() 之间不应该有任何可能导致上下文切换的调用。
2. **Owner 不正确**: 如果 unlock 后 owner 指向了一个已经被释放的 Task，说明生命周期管理有问题。确保等待队列中的 Task 在被 unblock 之前不会被销毁。
3. **Guard 被丢弃**: 如果你写了 `m.guard()` 而不是 `auto g = m.guard()`，编译器应该发出 `[[nodiscard]]` 警告。开启 `-Werror=nodeprecated-declarations` 来捕获这个问题。
4. **递归死锁**: 如果同一个任务对同一个 Mutex 调用了两次 lock()，第二次会永远阻塞。如果你需要递归锁语义，需要增加递归计数器支持。
5. **等待队列泄漏**: 如果 unlock 后等待队列中还有任务但任务状态不对（不是 Blocked），说明 block/unblock 的时序有问题。可以在 unlock 中加一个断言检查 `waiter->state == Blocked`。

## 设计对比：每个 OS 的 Mutex 实现

xv6 的 sleeplock 用 spinlock 保护一个 `locked` 标志，阻塞时调用 `sleep(chan, &lk)` 在通道上等待，唤醒时 `wakeup(chan)` 扫描整个进程表找匹配的 SLEEPING 进程。这种设计简洁但效率是 O(n)——每次 wakeup 都要遍历所有进程，而且有 thundering herd 问题。xv6 根本不提供 Semaphore，只有 spinlock 和 sleeplock 两种选择。

Linux 的 mutex 就复杂得多了——三级路径设计：快速路径用原子 `cmpxchg` 尝试获取（无竞争时 O(1)），乐观自旋路径在锁持有者正在另一个 CPU 上运行时自旋而非立即睡眠（减少上下文切换），慢速路径才真正进入等待队列睡眠。owner 字段复用指针低位存储 WAITERS/HANDOFF/PICKUP 三个标志位。HANDOFF 机制允许等待者请求直接所有权转移以防止饥饿。整个实现约 700 行代码。

PintOS 走了第三条路——Lock 直接建立在 Semaphore 之上（value=1 的信号量加上持有者追踪）。这意味着 PintOS 的 Lock 和 Semaphore 是有层次关系的，而 Cinux 和 Linux 让它们成为两个独立的原语。

## Host 端测试与内核测试如何验证 Mutex

理解了两套测试的区别，我们来看它们各自如何验证 Mutex 的行为。Host 端的 Mutex 测试用 mock scheduler 和 mock per_cpu 来模拟任务切换。它创建一个静态 Task 数组（8 个），通过 `set_current(idx)` 来模拟"哪个任务正在运行"。当 lock() 调用 block() 时，block 只是修改任务状态并记录参数——不做真正的上下文切换。这让测试可以精确验证"是否调用了 block"、"传入了哪个 Task"等行为。

内核测试的策略不同。它使用真实的 TaskBuilder 创建 Task 对象（会在堆上分配），手动设置 `g_per_cpu.current` 来模拟任务执行。当 lock() 调用 Scheduler::block() 时，block 会真正修改任务状态并调用 schedule()。但在测试环境下，因为只有 idle_task 在就绪队列中，schedule 会切到 idle_task 执行 hlt。测试代码在 lock() 返回后（如果当前任务被阻塞，测试实际上无法到达这里——所以 Mutex 测试中的"等待"任务在 lock() 调用后不会真正返回，测试只是检查它的状态是否变为 Blocked）。

FIFO 测试是最核心的。三个任务按序 lock 被阻塞，owner 连续 unlock 三次。每次 unlock 后，测试检查下一个等待者的状态是否变为 Ready。这个测试在 host 和内核两套环境中都运行，确保等待队列的实现和调度器的行为都正确。

## 参考资料

- Intel SDM: Vol. 3A Section 8.1 "Locked Atomic Operations" -- 原子操作语义
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- Mutex 概述
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) -- xv6 的 sleep/wakeup 机制
- Linux: [kernel/locking/mutex.c](https://github.com/torvalds/linux/blob/master/kernel/locking/mutex.c) -- 生产级 mutex 实现
- PintOS: [Semaphore documentation](https://cs162.org/static/proj/pintos-docs/docs/synch/semaphores/) -- Lock 构建在 Semaphore 之上

## 本章小结

| 操作 | 行为 |
|------|------|
| lock() | 拿 Spinlock -> 检查 owner -> 空: 占据返回 / 满: 入队、释放 Spinlock、block |
| unlock() | 拿 Spinlock -> 取等待者 -> 无: 清 owner / 有: 转移所有权、释放 Spinlock、unblock |
| try_lock() | 拿 Spinlock -> owner 空: 占据返回 true / 满: 返回 false |
| guard() | RAII 包装，[[nodiscard]] 防丢弃，构造 lock、析构 unlock |
| FIFO | 等待队列尾插头删，所有权转移保证先到先得 |
| 硬性规则 | Spinlock 持有期间绝不调用 block/schedule |

## 下一步预告

这一章我们完成了 Mutex 的完整实现——lock 的五步阻塞获取、unlock 的所有权转移、try_lock 的非阻塞尝试、guard 的 RAII 管理。Mutex 解决了互斥问题，但互斥不是同步的全部。

很多时候我们需要的是"等到某个条件满足"而不是"等到轮到我"——这就是 Semaphore 要做的事。下一章我们来实现 Dijkstra 的经典信号量，然后用 Mutex + Semaphore 组合解决 Producer-Consumer 问题，这是操作系统同步中最经典的模式之一。如果你对 Mutex 的 FIFO 公平性还有疑问，建议在继续之前重新阅读 unlock() 的所有权转移部分，确保理解了为什么直接转移比清零重竞争更好。
理解了 Mutex 的"互斥"语义之后，Semaphore 的"同步"语义就会水到渠成。
