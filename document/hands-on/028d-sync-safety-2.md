# 028d-2: 全面加锁——三层分级策略

## 导语

上一章我们完成了自旋锁和 RAII 守卫的基建。现在到了真正动手的时候——把锁加到内核的每一个共享数据结构上。这听起来像是一个体力活，但实际上需要非常仔细的策略规划。加少了会留下数据竞争的隐患，加多了会影响性能甚至引入死锁。更关键的是，不同组件面临的风险等级不同，需要的保护手段也不同——有些需要完整的 IrqGuard（关中断+自旋锁），有些只需要普通 Guard，还有些用 `std::atomic` 就够了。

完成这一章后，内核的所有共享数据结构都将受到保护，支持在抢占式多任务和中断环境下安全运行。

前置要求：上一章的 Spinlock、IrqGuard、InterruptGuard 实现必须完成。

## 概念精讲

### 三层分级策略

在开始逐个组件加锁之前，我们先制定一个分级策略。不是所有共享数据都需要同等强度的保护，过度保护只会增加复杂度和死锁风险。

**TIER 0——核心分配器**（数据竞争必然崩溃）：PMM、Heap、Scheduler 运行队列。这些是内核最底层的共享资源，所有内核线程都在用，而且它们内部的数据结构（位图、空闲链表、环形数组）在并发操作下会立刻损坏。必须用最强的保护——Scheduler 用 IrqGuard（因为它的运行队列在中断上下文中被操作），PMM 和 Heap 用普通 Guard（它们的操作不会在中断处理程序中直接执行，但在递归调用时需要注意锁的拆分）。

**TIER 1——高频共享计数器**（竞争导致数据不一致）：PIT 的 tick 计数器、Scheduler 的时间片计数器、TaskBuilder 的 TID 和栈地址分配器。这些变量被频繁读写，但单次不一致不会导致系统崩溃。使用 `std::atomic` 的 `memory_order_relaxed` 就足够了——我们只需要保证读写的原子性，不需要强排序。

**TIER 2——中等风险组件**（并发访问频率较低）：File 的 offset、FDTable、VMM 的 map/unmap、全局 mount 表。这些在正常的内核操作中不太可能被并发访问，但在多线程环境下仍然需要保护。使用普通 Spinlock Guard 即可。

### 锁的拆分：为什么需要 _locked 版本

一个常见陷阱是"在持有锁的情况下调用也会获取同一个锁的函数"。PMM 就是一个典型例子：`alloc_pages` 需要分配多个连续页，内部逻辑和 `alloc_page` 几乎一样。如果 `alloc_pages` 持有锁然后调用 `alloc_page`（也会尝试获取同一个锁），立刻死锁——自旋锁不支持递归。

解决方案是把核心逻辑拆成一个不需要锁的内部版本（`alloc_page_locked`），公共版本只负责获取锁然后调用内部版本。这样 `alloc_pages` 持有锁后可以安全地调用 `_locked` 版本。

## 动手实现

### Step 1: PMM 加锁

**目标**: 保护物理内存管理器的位图操作。

**设计思路**: PMM 的核心是一个位图，`alloc_page` 需要"查找第一个空闲位 + 设为已用"这两步是原子的——如果两步之间被打断，另一个线程可能看到同一个空闲位并认为它可用。我们把原来的 `alloc_page` 拆分为两部分：一个不带锁的内部版本 `alloc_page_locked`（做实际工作），和一个公共版本 `alloc_page`（获取 Spinlock 后调用内部版本）。`free_page` 做同样的拆分。

`alloc_pages`（分配连续多页）需要特殊处理：它获取锁后，如果 count==1，直接调用 `_locked` 版本（不能调用公共版本，否则死锁）；如果 count>1，在持锁状态下直接操作位图。`free_pages` 同理——获取锁后循环调用 `_locked` 版本。

**实现约束**:
- PMM 类添加一个 Spinlock 成员
- 添加 `alloc_page_locked` 和 `free_page_locked` 私有方法
- 公共 `alloc_page`/`free_page`/`alloc_pages`/`free_pages` 都在入口处获取 Spinlock Guard
- `alloc_pages` 中 count==1 的分支必须调用 `_locked` 版本

**踩坑预警**: 最容易犯的错误是在 `alloc_pages` 的 count==1 分支中调用公共 `alloc_page` 而不是 `_locked` 版本。编译不会报错，但运行时必然死锁。

**验证**: 分配 8 个页，检查 `free_page_count` 减少了 8；然后释放这 8 个页，检查计数恢复原值。

### Step 2: Heap 加锁

**目标**: 保护堆分配器的空闲链表。

**设计思路**: Heap 的 `alloc` 方法遍历空闲链表做 first-fit 搜索，找到合适的 block 后把它从链表中移除或分割。如果两个线程同时做 alloc，链表指针会被破坏。和 PMM 一样，我们把核心分配逻辑拆成 `alloc_locked`（内部版本），公共 `alloc` 只负责获取锁。

这里有一个额外的复杂性：`alloc_locked` 如果在空闲链表中找不到合适的 block，会调用 `expand` 扩展堆，然后递归调用自己重试。这个递归不能走公共 `alloc`（会死锁），必须走 `alloc_locked`。

**实现约束**:
- Heap 类添加一个 Spinlock 成员
- 将 `alloc` 的核心逻辑移到 `alloc_locked` 私有方法
- 公共 `alloc` 获取锁后调用 `alloc_locked`
- `free` 直接在入口处获取锁
- `alloc_locked` 中的递归重试调用自身

**踩坑预警**: Heap 的 expand 操作内部会调用 PMM 的 alloc_page 和 VMM 的 map——这些操作各自有自己的锁，不会与 Heap 的锁冲突（锁的对象不同），所以不存在嵌套死锁问题。但要注意确保 expand 调用的是 PMM 和 VMM 的公共版本（带锁的版本），因为在 Heap 锁的保护下调用这些是安全的。

**验证**: 循环分配 30 个不同大小的内存块，写入特征字节；然后全部释放；重复 5 轮。如果没有崩溃且特征字节正确，说明加锁没有破坏分配逻辑。

### Step 3: Scheduler 运行队列加锁

**目标**: 保护 RoundRobin 调度器的运行队列。

**设计思路**: 调度器的运行队列是一个环形数组，enqueue/dequeue/pick_next 都在修改它。但调度器有一个特殊要求：`tick()` 是从时钟中断处理程序中被调用的。中断处理程序的执行是不可预测的——它可能在任何线程执行到任何位置时打断。所以如果线程 A 正在执行 `enqueue` 持有普通 Guard（没关中断），此时 tick 到来，中断处理程序调用 `pick_next` 试图获取同一个锁——死锁。

解决方案：RoundRobin 的三个操作都用 `irq_guard()` 而不是普通 `guard()`。这样在操作运行队列之前先禁用中断，中断处理程序不可能打断持锁的代码段。

**实现约束**:
- RoundRobin 类添加一个 Spinlock 成员
- `enqueue`、`dequeue`、`pick_next` 入口处获取 `irq_guard()`
- `tick()` 中修改 `tick_count_` 和 `current_slice_` 时不需要额外的锁——这些是 atomic 变量

**验证**: 创建 3 个 Task，依次 enqueue，然后 pick_next 验证 FIFO 顺序；dequeue 中间的 task 后再次验证顺序。

### Step 4: 原子计数器替换

**目标**: 将高频共享计数器从普通变量替换为 `std::atomic`。

**设计思路**: 有些变量不需要锁的保护，只需要保证读写的原子性。比如 PIT 的 tick 计数器——每次中断加 1，各种地方读取它来做超时判断。用自旋锁保护它代价太大（每个 tick 中断都要获取锁），而 `std::atomic` 的 `fetch_add` 在 x86 上会被编译成一条 `lock xadd` 指令，开销极小。

同理，`Scheduler` 的 `tick_count_` 和 `current_slice_`，以及 `TaskBuilder` 的 `next_tid` 和 `next_stack_vaddr`——它们只需要原子递增，不需要与其他操作组成原子事务。对于这些变量，`memory_order_relaxed` 就够了——我们不在乎操作之间的顺序，只在乎操作本身的原子性。

**实现约束**:
- PIT 的 `tick_count_` 改为 `std::atomic<uint64_t>`，递增用 `fetch_add(1, relaxed)`，读取用 `load(relaxed)`
- Scheduler 的 `tick_count_`/`current_slice_` 改为 `std::atomic<int>`，操作同理
- TaskBuilder 的 `next_tid` 用 `fetch_add(1, relaxed)` 替代 `++`
- TaskBuilder 的 `next_stack_vaddr` 用 `fetch_add` 替代读-加-写序列

**踩坑预警**: `next_stack_vaddr` 的原始代码是"读取当前值然后加上偏移量"的两步操作。改成 `fetch_add` 后，返回值就是加之前的旧值——这正好是我们需要的栈起始地址。如果你手动实现"load + add + store"三步，那不是原子的，两个线程可能读到同一个地址。

**验证**: 创建多个 Task，检查它们的 TID 互不相同且递增；检查栈地址互不重叠。

### Step 5: 文件系统与驱动加锁

**目标**: 保护 FDTable、File offset、VMM、mount 表、键盘缓冲区。

**设计思路**: 这些组件的并发风险相对较低，但在多任务环境下仍然需要保护。

FDTable 的 `alloc`/`close`/`get` 需要一个 Spinlock：两个线程同时 alloc 可能拿到同一个 slot（double-alloc），或者一个线程 close 了另一个线程正在用的 fd（use-after-free）。加一个 Spinlock 到 FDTable 类中，三个方法入口获取 Guard。

File 结构体的 `offset` 字段需要单独保护——多个线程可能通过不同的 fd 指向同一个 File（虽然 Cinux 目前不支持，但 offset_lock_ 为未来做准备），或者在 sys_read/sys_write 中需要保证"读取 offset + 操作 + 更新 offset"的原子性。File 结构体添加一个 `mutable Spinlock offset_lock_`，sys_read/sys_write/sys_getdents 中用 guard 保护 offset 的读取和更新。

VMM 的 `map`/`unmap` 添加 Spinlock——两个线程同时修改页表结构会导致页表损坏。全局 mount 表 `g_mount_table` 添加全局 Spinlock `g_mount_lock`——`vfs_mount_add`、`vfs_mount_remove`、`vfs_resolve` 入口处获取 guard。

键盘驱动的 `poll()` 方法使用 InterruptGuard 而非 Spinlock——IRQ1 handler 不获取任何锁（它在中断上下文中直接写入 ring buffer），poll 读取端只需要确保读取时不会被 IRQ1 打断。

**验证**: 打开文件、读取内容、关闭文件——这些操作在之前的 tag 中已经有测试覆盖。加锁后重新运行这些测试，确保功能不受影响。

## 构建与运行

完整构建所有测试：

```
cmake --build build --target big_kernel_test
```

运行 QEMU：

```
qemu-system-x86_64 -cdrom build/big_kernel_test.iso -serial stdio -display none
```

预期在串口输出中看到所有已有的测试仍然通过，新增的 PMM 并发测试和 Heap 锁压力测试也通过。

## 调试技巧

**问题 1: PMM/Heap 死锁**
如果系统在启动时挂住，最可能的原因是 `_locked` 版本的拆分不完整——某个地方在持锁状态下调用了会再次获取同一个锁的公共方法。用 GDB attach 到 QEMU，查看调用栈：

```
gdb build/big_kernel_test
(gdb) target remote :1234
(gdb) bt
```

**问题 2: 中断被永久禁用**
如果系统突然停止响应中断（没有时钟、没有键盘），检查所有 IrqGuard/InterruptGuard 的配对。一个常见的坑是在持锁状态下调用了一个也会获取 IrqGuard 的函数——由于 cli 是幂等的（多次 cli 没关系），问题通常出在析构顺序上。

**问题 3: 串口输出混乱**
如果 kprintf 的输出出现乱序或截断，说明 kprintf 本身可能也需要锁保护。不过 Cinux 的 kprintf 是直接写串口端口，单字节写入是原子的，所以通常不会出问题。如果确实需要保护，可以给 kprintf 加一个 Spinlock。

## 本章小结

| 组件 | 保护方式 | 原因 |
|------|----------|------|
| PMM 位图 | Spinlock Guard + _locked 拆分 | 位图查找+设置必须原子 |
| Heap 空闲链表 | Spinlock Guard + alloc_locked 拆分 | 链表遍历+修改必须原子 |
| Scheduler 运行队列 | Spinlock IrqGuard | 中断上下文也会操作 |
| PIT/Scheduler/TaskBuilder 计数器 | std::atomic relaxed | 只需原子递增 |
| FDTable | Spinlock Guard | 防止 double-alloc/use-after-free |
| File offset | Spinlock Guard | 读写 offset+更新必须原子 |
| VMM map/unmap | Spinlock Guard | 页表修改需要互斥 |
| Mount 表 | Spinlock Guard | 全局表修改需要互斥 |
| Keyboard ring buffer | InterruptGuard | 简单的中断安全即可 |
