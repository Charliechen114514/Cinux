# 021-2 Mutex：阻塞式互斥锁设计

> 前置：[021-1 Spinlock 与等待队列](021-proc-sync-1.md)
> 标签：mutex, blocking, FIFO, owner tracking, RAII guard

## 导语

上一章我们搭好了等待队列的基础设施。现在问题来了：Spinlock 虽然简单，但如果你需要在锁住的时候做一件很耗时的事——比如等磁盘 I/O 完成、等另一个任务往缓冲区里写入数据——那自旋就变成了纯粹的 CPU 浪费。我们需要一种"拿不到锁就睡觉，别人释放了再叫醒我"的机制。这就是 Mutex 要解决的问题。

完成本章后，你会理解阻塞式互斥锁的完整生命周期，以及为什么 Mutex 内部需要用 Spinlock 来保护自己的元数据。

## 概念精讲

### Spinlock vs Mutex——什么时候用哪个

这是一个很多新手 OS 开发者会困惑的问题。核心区别在于：Spinlock 让等待者忙等（占用 CPU），Mutex 让等待者睡眠（释放 CPU）。忙等适合临界区极短（几条指令）的场景，比如修改一个链表头指针；睡眠适合临界区较长或者需要在锁内等待条件的场景。

在 Cinux 的设计里，Spinlock 被用作 Mutex 和 Semaphore 的"内部锁"——它只保护这些同步原语自身的元数据（owner 指针、等待队列、计数器），操作完立即释放，持有时间通常不超过十来条指令。而 Mutex 才是面向内核其他模块的"外层锁"。

### Mutex 的三个核心状态

一个 Mutex 包含三样东西：一个 Spinlock（保护内部状态）、一个 owner 指针（谁持有这把锁）、一个 wait_head（等待队列）。这三个字段在 Mutex 对象创建时全部为零初始化——没有人持有锁，没有人在等。

### lock() 的五步流程

获取 Mutex 的过程分为五个精确的步骤。第一步，获取内部 Spinlock——这保证了我们检查和修改 owner 的时候不会被其他 CPU（或中断）打断。第二步，检查 owner 是不是 nullptr。如果是空的，太好了，把当前任务的指针写进 owner，释放 Spinlock，返回。第三步，如果 owner 不为空，说明锁被别人拿着，我们需要把当前任务加入等待队列的尾部。第四步，关键一步——释放 Spinlock。为什么要在 block 之前释放？因为 block 会调用 schedule 切换上下文，如果这时候 Spinlock 还被拿着，下一个任务如果要操作同一个 Mutex 就会死锁。第五步，调用 `Scheduler::block()` 把当前任务挂起，CPU 开始跑别的任务。

你可能觉得第三步和第四步之间有竞态条件：我们先把任务加入等待队列，然后释放 Spinlock，再 block。如果在这个窗口期有人 unlock 了呢？没关系——unlock 会把等待队列里的任务取出来 unblock，所以即使 block 之前就被唤醒了，block 的实现也只需要把任务状态设为 Blocked 然后调用 schedule，而 schedule 检查到任务已经不是 Running 状态就会正确处理。

### unlock() 的所有权转移

unlock 的关键设计在于"所有权转移"：如果等待队列里有任务，unlock 不会把 owner 清零，而是直接把等待队列头部的任务设为新 owner。这样做的好处是保证了公平性——等待最久的任务优先获得锁。同时，在 Spinlock 释放之后、unblock 之前，新 owner 已经确立，其他尝试 lock 的任务会看到 owner 不为空，正确地排队等待。

### try_lock() 和 guard()

try_lock 是非阻塞版本的 lock：检查 owner，如果为空就占据，否则直接返回 false。它适用于那些"拿不到就算了"的场景。

guard() 返回一个 RAII 对象，构造时调用 lock()，析构时调用 unlock()。标记为 `[[nodiscard]]` 是为了防止你不小心写了 `m.guard()` 但没有把返回值存起来——那样的话临时对象立即析构，锁拿到手又立刻释放了，跟没锁一样。

## 动手实现

### Step 1: 定义 Mutex 类

**目标**: 在 sync.hpp 中定义 Mutex 类的接口。

**设计思路**: Mutex 需要公开 lock()、unlock()、try_lock() 和 guard() 四个方法。内部持有三个字段：Spinlock 保护元数据、Task* owner 追踪持有者、Task* wait_head_ 管理等待队列。还需要私有的 enqueue_waiter 和 dequeue_waiter 辅助方法，以及一个嵌套的 Guard RAII 类。

**实现约束**: Guard 类需要禁用拷贝构造和拷贝赋值（`delete`），防止误拷贝导致同一把锁被 unlock 两次。Guard 的构造函数调用 lock()，析构函数调用 unlock()。

**验证**: 类定义编译通过即可。

### Step 2: 实现 lock() 和 unlock()

**目标**: 按照前面描述的五步流程实现 Mutex 的核心方法。

**设计思路**: lock() 先拿 Spinlock，检查 owner。空则占据，否则入队等待，释放 Spinlock 后 block。unlock() 先拿 Spinlock，取等待队列头。空则清 owner，否则转移所有权给等待者，释放 Spinlock 后 unblock 等待者。

**踩坑预警**: unlock 中释放 Spinlock 和 unblock 的顺序不能颠倒——必须先释放 Spinlock 再 unblock，否则 unblock 可能让另一个 CPU 上的任务立刻被调度到，尝试 lock 同一个 Mutex 时发现 Spinlock 还被占着，造成不必要的自旋甚至死锁（如果 spinlock 持有者在 unblock 路径中被抢占的话）。

**验证**: 以下场景应该全部通过——单任务 lock 后立刻 unlock（无竞争）、同一任务连续 lock/unlock 多次（重入性测试，注意我们的 Mutex 不支持递归，同一任务连续两次 lock 会死锁）、两个任务竞争同一 Mutex 验证 FIFO 顺序。

### Step 3: 实现 try_lock() 和 guard()

**目标**: 补全非阻塞获取和 RAII 包装。

**设计思路**: try_lock 就是一个简化版的 lock——检查 owner，为空则占据，不为空返回 false。不需要操作等待队列。guard() 返回 Guard 对象，Guard 的生命周期自动管理锁的获取和释放。

**验证**: 验证 try_lock 在 Mutex 空闲时成功、被持有时失败。验证 guard 在作用域结束时自动 unlock——可以用 try_lock 来检测：guard 析构后 try_lock 应该能成功。

## 构建与运行

内核测试会创建虚拟任务模拟竞争场景：

```bash
cd build && cmake .. && make run-kernel-test -j$(nproc)
```

串口输出中应该能看到 "Sync Tests (021)" 段的所有测试通过，包括 mutex 相关的 lock_unlock、try_lock、contention 和 FIFO ordering 测试。

Host 端单元测试也有对应的 Mutex 测试：

```bash
cd build && make test_sync -j$(nproc) && ./test/unit/test_sync
```

## 调试技巧

1. **死锁**: 如果系统挂死在 Mutex 操作上，最可能的原因是在 Spinlock 持有期间调用了 block 或 schedule。用串口输出在 lock/unlock 的每个步骤打印 trace 来定位。
2. **Owner 不正确**: 如果 unlock 后 owner 指向了一个已经被释放的 Task，说明生命周期管理有问题。确保等待队列中的 Task 在被 unblock 之前不会被销毁。
3. **Guard 被丢弃**: 如果你写了 `m.guard()` 而不是 `auto g = m.guard()`，编译器应该发出 `[[nodiscard]]` 警告。开启 `-Werror=nodeprecated-declarations` 来捕获这个问题。

## 本章小结

| 操作 | 行为 |
|------|------|
| lock() | 拿 Spinlock → 检查 owner → 空: 占据返回 / 满: 入队、释放 Spinlock、block |
| unlock() | 拿 Spinlock → 取等待者 → 无: 清 owner / 有: 转移所有权、释放 Spinlock、unblock |
| try_lock() | 拿 Spinlock → owner 空: 占据返回 true / 满: 返回 false |
| guard() | RAII 包装，构造 lock、析构 unlock |
| FIFO | 等待队列尾插头删，保证先到先得 |
