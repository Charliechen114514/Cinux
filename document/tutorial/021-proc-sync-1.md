---
title: 021-proc-sync-1 · 同步原语
---

# 021-1 从自旋到阻塞：内核同步原语设计哲学

> 前置：[020 抢占式调度——定时器中断驱动切换](020-proc-scheduler-3.md)
> 标签：spinlock, mutex, atomic operations, wait queue, intrusive list, xv6, Linux, PintOS

## 前言

tag 020 搭好了抢占式调度的一切基础设施——PIT 定时器中断驱动时间片轮转、block/unblock 让任务可以主动让出 CPU。但说实话，这些机制一直处于"有接口没场景"的状态。我们知道 block/unblock 是为同步原语准备的，但直到现在才真正用到它们。

今天我们就要把这一层捅破。Spinlock 的 acquire/release 从内联移到了独立的 .cpp 文件，作为更高级同步原语的内部保护机制。然后我们在它上面建起了 Mutex 和 Semaphore——前者解决互斥问题（同一时刻只有一个任务能访问），后者解决同步问题（任务之间协调时序）。再往后，我们用两者组合实现了经典的 Producer-Consumer 问题。

## 环境说明

这一章的变更集中在 `kernel/proc/sync.hpp`（类定义）和 `kernel/proc/sync.cpp`（新增，211 行实现），加上 `kernel/proc/process.hpp` 中新增的 `Task::wait_next` 字段。构建系统新增了 `test/unit/test_sync.cpp`（775 行 host 端测试）和 `kernel/test/test_sync.cpp`（584 行内核集成测试）。和之前一样，我们用 QEMU 运行 big kernel 验证 producer-consumer demo。编译工具链仍然是 GCC 14 + CMake + Ninja，目标平台 x86-64。

从代码量来看，sync.hpp 从原来的约 40 行 Spinlock 定义增长到约 210 行（包含 Spinlock、Mutex、Semaphore 三个类的完整定义）。sync.cpp 是全新的 211 行实现文件。加上 main.cpp 的修改（从 6 线程 demo 替换为 producer-consumer demo）、mini kernel 的 magic check 修复、以及构建系统变更，tag 021 的总 diff 约 1300 行。这是 Cinux 到目前为止最大的一个 tag。

## 从 tag 020 到 tag 021 的桥梁

在 tag 020 的结尾，我们的内核已经有了完整的抢占式调度：PIT 定时器中断驱动的 tick() 会在每个时间片耗尽时调用 schedule() 切换任务，block()/unblock() 机制让任务可以主动让出 CPU。main.cpp 中创建了 6 个线程做抢占式调度演示——每个线程做一个忙等循环并打印自己的标签。

但说实话，这 6 个线程之间没有任何共享数据，也不需要同步。它们各自跑各自的，定时器中断负责轮流给它们 CPU 时间。这就像 6 个人各自在各自的跑步机上跑步——虽然共用一个房间，但互相之间没有任何交互。

tag 021 的目标是让任务之间真正协作起来。Producer-Consumer 问题是一个经典的协作场景：一个任务往缓冲区里写数据，另一个任务从里面读。缓冲区满了生产者要等，缓冲区空了消费者要等，缓冲区的读写必须互斥。要实现这个场景，我们需要 Mutex（互斥）和 Semaphore（同步）。

这一章我们会覆盖三个主题：Spinlock 的角色转变、侵入式等待队列的设计、Mutex 的阻塞式互斥锁实现。每个主题都涉及一个核心设计问题，我们从 Spinlock 开始。

## 内存序深度解析

在深入代码之前，让我们先理解 Spinlock 背后的内存序概念。`__ATOMIC_ACQUIRE` 和 `__ATOMIC_RELEASE` 是 C11 原子操作标准定义的两种内存序。Acquire 语义保证"获取锁之后的读操作不会被重排到获取锁之前"，Release 语义保证"释放锁之前的写操作对其他 CPU 可见之后才释放锁"。这两个语义组合起来就形成了经典的 acquire-release 同步对：获取锁的一方看到释放锁一方的所有写操作。

在 x86-64 上，由于 CPU 是强排序的（TSO 模式），这两种屏障大部分时候是 no-op。但显式标注是好习惯——代码意图更清晰，移植到 ARM 等弱排序架构时不会出 bug。除了 Acquire 和 Release，C11 还有 `__ATOMIC_RELAXED`（无排序保证）、`__ATOMIC_ACQ_REL`（同时具有 Acquire 和 Release 语义）和 `__ATOMIC_SEQ_CST`（最强的一致性保证）。Spinlock 使用 Acquire/Release 而非 SeqCst，因为在锁的实现中我们只需要 acquire-release 语义就够了。

`__atomic_test_and_set` 和 `__atomic_clear` 是 GCC 的 C11 原子操作 builtin，不依赖 C++ `<atomic>` 头文件，适合在内核这种没有完整 C++ 标准库的环境下使用。Intel SDM Vol. 3A Section 8.1 "Locked Atomic Operations" 对 LOCK 前缀和原子操作语义有完整的说明。

## Spinlock——从主角到配角

在 tag 020 里 Spinlock 是唯一的同步原语，现在它退居幕后，变成 Mutex 和 Semaphore 的"内部锁"。这个角色转换其实很自然。Spinlock 的核心语义是"短临界区的忙等互斥"——它只保护几个寄存器操作，持有时间不超过十几条指令，期间任务不睡眠、不阻塞、不放弃 CPU。

```cpp
void Spinlock::acquire() {
    while (__atomic_test_and_set(&locked_, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

void Spinlock::release() {
    __atomic_clear(&locked_, __ATOMIC_RELEASE);
}
```

`__atomic_test_and_set` 底层编译成 x86 的 `LOCK BTS` 或者等价的原子交换指令，把 `locked_` 设为 true 并返回旧值。`__ATOMIC_ACQUIRE` 内存序确保后续读操作不会重排到获取锁之前。循环体里的 `pause` 指令（Intel SDM Vol. 2A "PAUSE -- Spin Wait Hint"）告诉 CPU "我在等一个锁"，避免 P4/Xeon 上 spin-wait 结束时的流水线 flush 惩罚。释放时 `__atomic_clear` 配合 `__ATOMIC_RELEASE` 确保临界区内的写操作对其他 CPU 可见之后才释放。

你可能会问：既然 Spinlock 这么简单，为什么还需要 Mutex？核心区别在于 Spinlock 让等待者忙等（占用 CPU 转圈），Mutex 让等待者睡眠（释放 CPU 给别人跑）。如果你的临界区只有几条指令，Spinlock 足够；但如果需要在锁内等条件（比如"缓冲区非空"），自旋就是纯浪费。特别是在单核环境下，自旋的任务占着 CPU，持有锁的任务根本没机会运行来释放锁——这其实就是死锁。

这里有一条硬性规则：**永远不要在持有 Spinlock 的时候调用 block 或 schedule**。Spinlock 忙等期间如果触发了上下文切换，下一个任务要拿同一把 Spinlock 就死锁了。Mutex 的设计正是为了避免这个问题——它的 Spinlock 在 block 之前就已经释放。

## 侵入式等待队列——零分配的优雅

在 Mutex 和 Semaphore 能工作之前，我们需要一个等待队列来记录"谁在等"。教科书上的链表节点通常需要动态分配内存，但内核的同步原语不能依赖堆——万一堆分配器本身就需要锁保护呢？

侵入式链表是内核开发中的经典解决方案：不分配独立节点，而是把 `next` 指针直接嵌入被管理的对象。对 Cinux 来说，就是在 Task 结构体中加一个字段：

```cpp
/** Intrusive link for wait-queue linked lists (Mutex / Semaphore). */
Task* wait_next;
```

这一个指针就是整个等待队列的基础设施。enqueue（尾插）和 dequeue（头删）操作都是 O(n) 的，但在单核教学 OS 中等待者数量极少（通常不超过几个），线性遍历完全可接受。

这个设计限制了一个任务同时只能在一个等待队列上等待。Linux 用独立的 `mutex_waiter` 结构解决了这个问题——每个等待操作在栈上分配一个 waiter，包含 list_head 和 task 指针。但这引入了更多复杂性，对 Cinux 来说没有必要。

## Mutex 的生命周期——从创建到销毁

让我们跟踪一个 Mutex 从创建到使用的完整生命周期，来理解它的内部状态是如何变化的。

创建阶段：`Mutex m;` 调用默认构造函数，三个字段（spin_.locked_ = false, owner_ = nullptr, wait_head_ = nullptr）全部零初始化。此时 Mutex 处于 Free 状态——任何人都可以 lock。

快速路径：任务 A 调用 `m.lock()`。lock() 先拿 Spinlock，检查 owner_ 为 nullptr，于是设置 owner_ = A，释放 Spinlock，返回。此时 Mutex 处于 Held 状态——只有 A 可以 unlock。

慢速路径：任务 B 也调用 `m.lock()`。lock() 先拿 Spinlock，检查 owner_ 发现是 A（不为 nullptr），于是把 B 加入等待队列（wait_head_ -> B），释放 Spinlock，调用 block(B)。此时 Mutex 处于 Contended 状态——有一个等待者。

所有权转移：任务 A 调用 `m.unlock()`。unlock() 先拿 Spinlock，从等待队列取出 B，设置 owner_ = B（所有权转移），释放 Spinlock，调用 unblock(B)。此时 Mutex 仍然处于 Held 状态——owner 是 B，没有等待者。B 恢复执行后从 lock() 返回，拥有了 Mutex。

这个生命周期中有一个关键不变量：在 Spinlock 持有期间，owner_ 和 wait_head_ 的修改不会被其他任务打断。这保证了状态转换的原子性。另一个不变量是 Spinlock 在 block 之前必须释放——否则 block 触发的 schedule 会让下一个任务死锁在 Spinlock 上。

## Mutex——阻塞式互斥锁

Mutex 的核心思路是：拿不到锁就睡觉，别人释放了再叫醒我。它的三个内部状态精确分工——Spinlock 保护元数据修改、owner 指针追踪持有者、wait_head 指向等待队列。

你可能觉得 Mutex 就是"带睡眠功能的 Spinlock"，但实际上 Mutex 的设计远不止于此。Spinlock 只有一个状态位（locked/unlocked），Mutex 有三个独立的字段（spin_, owner_, wait_head_），每个字段都有明确的职责。Spinlock 保护的是 Mutex 自身的元数据，不是用户数据。owner_ 记录的是"谁持有这把锁"，这使得所有权转移成为可能。wait_head_ 管理的是等待队列，保证了 FIFO 公平性。

lock() 的流程体现了精心设计的五步协议：

```cpp
void Mutex::lock() {
    spin_.acquire();
    if (owner_ == nullptr) {
        owner_ = g_per_cpu.current;
        spin_.release();
        return;
    }
    Task* self = g_per_cpu.current;
    enqueue_waiter(self);
    spin_.release();
    Scheduler::block(self, "mutex");
}
```

先拿 Spinlock 保护检查和修改操作。如果 owner 为空直接占据——这是无竞争的快速路径。否则进入慢速路径：入队等待，释放 Spinlock（这一步必须在 block 之前，否则 block 触发 schedule 时 Spinlock 还被持着就死锁了），然后调用 block 让出 CPU。

你可能觉得入队和释放 Spinlock 之间有竞态窗口。但在单核环境下，spin_.release() 之后、block() 之前不会被另一个任务抢占（中断可能发生但中断处理程序不会操作同一个 Mutex）。即使在 SMP 环境下，如果有其他 CPU 上的任务在此窗口期 unlock，unlock 会把等待队列里的任务取出来 unblock——即使 block 之前就被唤醒了也不会出错。

unlock() 的关键设计是"所有权转移"：

```cpp
void Mutex::unlock() {
    spin_.acquire();
    Task* waiter = dequeue_waiter();
    if (waiter == nullptr) {
        owner_ = nullptr;
        spin_.release();
        return;
    }
    owner_ = waiter;
    spin_.release();
    Scheduler::unblock(waiter);
}
```

如果有等待者，owner 直接转给队列头的任务而不是先清零。这保证了 FIFO 公平性——等待最久的任务一定先获得锁。相比之下，xv6 的 sleeplock 通过全局 wakeup 扫描进程表唤醒所有在同一个通道上睡眠的任务（thundering herd 问题），效率低且不够公平。

## 设计对比：每个 OS 都有自己的 Mutex

说到 Mutex 实现，每个操作系统都走了不同的路。xv6-riscv 的 sleeplock 用 spinlock 保护一个 `locked` 标志，阻塞时调用 `sleep(chan, &lk)` 在通道上等待，唤醒时 `wakeup(chan)` 扫描整个进程表找匹配的 SLEEPING 进程。这种设计简洁但效率是 O(n)——每次 wakeup 都要遍历所有进程。而且 xv6 根本不提供 Semaphore，只有 spinlock 和 sleeplock 两种选择。

Linux 的 mutex 就复杂得多了——三级路径设计：快速路径用原子 `cmpxchg` 尝试获取（无竞争时 O(1)），乐观自旋路径在锁持有者正在另一个 CPU 上运行时自旋而非立即睡眠（减少上下文切换），慢速路径才真正进入等待队列睡眠。owner 字段复用指针低位存储 WAITERS/HANDOFF/PICKUP 三个标志位。HANDOFF 机制允许等待者请求直接所有权转移以防止饥饿。整个实现约 700 行代码，加上 optimistic spinning 的 MCS 锁和 wake_q 批量唤醒。Cinux 的实现只有约 210 行，但保留了核心设计原则：内部 spinlock 保护元数据、阻塞前释放 spinlock、FIFO 公平性。

PintOS 走了第三条路——Lock 直接建立在 Semaphore 之上（value=1 的信号量加上持有者追踪）。这意味着 PintOS 的 Lock 和 Semaphore 是有层次关系的，而 Cinux 和 Linux 让它们成为两个独立的原语。PintOS 的等待列表使用通用双向链表（`lib/kernel/list.c`），每个等待操作需要分配一个 `list_elem`，而 Cinux 用侵入式链表避免了所有额外分配。

## 为什么 Mutex 和 Semaphore 应该独立实现

这是一个值得深入讨论的设计决策。PintOS 把 Lock 构建在 Semaphore 之上（Semaphore value=1 加持有者追踪），看起来很优雅——代码复用，层次清晰。但这种方式有一个致命缺陷：Semaphore 不维护"谁 post 的"语义，而 Mutex 需要"只有 owner 才能 unlock"的语义。在 PintOS 中，任何任务都可以调用 sema_up，理论上任何任务都能"解锁"一个 Lock——这不符合 Mutex 的互斥语义。

Cinux 选择了 Linux 的方式——Mutex 和 Semaphore 完全独立实现。Mutex 有 owner 追踪和所有权转移，这些语义在 Semaphore 之上很难高效实现。Semaphore 有负数计数的语义（|count_| = 等待者数量），这在 Mutex 的二元状态上也不自然。两个独立的原语各司其职：Mutex 做互斥，Semaphore 做同步，互不干扰。

## 参考资料

- Intel SDM: Vol. 2A "PAUSE -- Spin Wait Hint" -- PAUSE 避免流水线 flush
- Intel SDM: Vol. 3A Section 8.1 "Locked Atomic Operations" -- 原子操作语义
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives)
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) / [spinlock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c)
- Linux: [kernel/locking/mutex.c](https://github.com/torvalds/linux/blob/master/kernel/locking/mutex.c)
- PintOS: [Semaphore documentation](https://cs162.org/static/proj/pintos-docs/docs/synch/semaphores/)
- GCC Documentation: [Atomic Builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html) -- `__atomic_test_and_set` 和 `__atomic_clear` 的完整说明

## 本章小结

tag 021 的第一部分建立了同步原语的设计基础。Spinlock 从主角变成了配角，退居幕后为 Mutex 和 Semaphore 提供内部保护。侵入式等待队列用 Task::wait_next 一个字段解决了等待队列的零分配问题。Mutex 的五步 lock() 协议和所有权转移的 unlock() 保证了 FIFO 公平性和不死锁。

我们看到了三个核心设计对比：xv6 的 O(n) 通道扫描 vs Cinux 的 O(1) FIFO 等待队列，Linux 的三级路径设计 vs Cinux 的简单直接实现，PintOS 的 Semaphore-built Lock vs Cinux 的独立原语。这些对比帮助我们理解了设计空间和 trade-off。

下一章我们将在这个基础上构建 Semaphore，并用两者的组合实现 Producer-Consumer——这是操作系统同步中最经典的模式之一。
