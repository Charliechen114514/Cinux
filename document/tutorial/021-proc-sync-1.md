# 021-1 从自旋到阻塞：内核同步原语设计哲学

> 前置：[020 抢占式调度——定时器中断驱动切换](020-proc-scheduler-3.md)
> 标签：spinlock, mutex, atomic operations, wait queue, intrusive list, xv6, Linux, PintOS

## 前言

tag 020 搭好了抢占式调度的一切基础设施——PIT 定时器中断驱动时间片轮转、block/unblock 让任务可以主动让出 CPU。但说实话，这些机制一直处于"有接口没场景"的状态。我们知道 block/unblock 是为同步原语准备的，但直到现在才真正用到它们。

今天我们就要把这一层捅破。Spinlock 的 acquire/release 从内联移到了独立的 .cpp 文件，作为更高级同步原语的内部保护机制。然后我们在它上面建起了 Mutex 和 Semaphore——前者解决互斥问题（同一时刻只有一个任务能访问），后者解决同步问题（任务之间协调时序）。再往后，我们用两者组合实现了经典的 Producer-Consumer 问题。

## 环境说明

这一章的变更集中在 `kernel/proc/sync.hpp`（类定义）和 `kernel/proc/sync.cpp`（新增，211 行实现），加上 `kernel/proc/process.hpp` 中新增的 `Task::wait_next` 字段。构建系统新增了 `test/unit/test_sync.cpp`（775 行 host 端测试）和 `kernel/test/test_sync.cpp`（584 行内核集成测试）。和之前一样，我们用 QEMU 运行 big kernel 验证 producer-consumer demo。

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

你可能会问：既然 Spinlock 这么简单，为什么还需要 Mutex？核心区别在于 Spinlock 让等待者忙等（占用 CPU 转圈），Mutex 让等待者睡眠（释放 CPU 给别人跑）。如果你的临界区只有几条指令，Spinlock 足够；但如果需要在锁内等条件（比如"缓冲区非空"），自旋就是纯浪费。

## 侵入式等待队列——零分配的优雅

在 Mutex 和 Semaphore 能工作之前，我们需要一个等待队列来记录"谁在等"。教科书上的链表节点通常需要动态分配内存，但内核的同步原语不能依赖堆——万一堆分配器本身就需要锁保护呢？

侵入式链表是内核开发中的经典解决方案：不分配独立节点，而是把 `next` 指针直接嵌入被管理的对象。对 Cinux 来说，就是在 Task 结构体中加一个字段：

```cpp
/** Intrusive link for wait-queue linked lists (Mutex / Semaphore). */
Task* wait_next;
```

这一个指针就是整个等待队列的基础设施。enqueue（尾插）和 dequeue（头删）操作都是 O(n) 的，但在单核教学 OS 中等待者数量极少（通常不超过几个），线性遍历完全可接受。

这个设计限制了一个任务同时只能在一个等待队列上等待。Linux 用独立的 `mutex_waiter` 结构解决了这个问题——每个等待操作在栈上分配一个 waiter，包含 list_head 和 task 指针。但这引入了更多复杂性，对 Cinux 来说没有必要。

## Mutex——阻塞式互斥锁

Mutex 的核心思路是：拿不到锁就睡觉，别人释放了再叫醒我。它的三个内部状态精确分工——Spinlock 保护元数据修改、owner 指针追踪持有者、wait_head 指向等待队列。

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

## 参考资料

- Intel SDM: Vol. 2A "PAUSE -- Spin Wait Hint" -- PAUSE 避免流水线 flush
- Intel SDM: Vol. 3A Section 8.1 "Locked Atomic Operations" -- 原子操作语义
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives)
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) / [spinlock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c)
- Linux: [kernel/locking/mutex.c](https://github.com/torvalds/linux/blob/master/kernel/locking/mutex.c)
- PintOS: [Semaphore documentation](https://cs162.org/static/proj/pintos-docs/docs/synch/semaphores/)
