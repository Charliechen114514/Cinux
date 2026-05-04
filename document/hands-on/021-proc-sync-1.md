# 021-1 Spinlock 重构与侵入式等待队列

> 前置：[020 抢占式调度](020-proc-scheduler-3.md)
> 标签：spinlock, atomic, wait queue, intrusive list, RAII

## 导语

在 tag 020 里我们搭好了抢占式调度的骨架——PIT 定时器中断驱动 `tick()`，时间片耗尽就 `schedule()` 切走当前任务，再加上 `block()`/`unblock()` 机制让任务可以主动让出 CPU。但说实话，block/unblock 一直是"有接口但没用起来"的状态。这一章我们要做的，就是给内核加上真正的同步原语——先从 Spinlock 的重构和一个侵入式等待队列开始。

完成本章后，你会理解原子操作在 x86-64 上的具体实现方式，以及为什么内核里等待队列通常用侵入式链表而不是 `new` 出来的节点。

## 概念精讲

### Spinlock 的本质——原子 test-and-set

Spinlock 的核心思想非常直白：用一个布尔值表示锁的状态，想拿锁就尝试把它从 false 改成 true，如果已经是 true 就一直转圈等。难点在于"尝试改"这个操作必须是原子的——不能有两个 CPU 同时读到 false 然后都以为自己拿到了锁。

x86-64 给了我们直接可用的工具：`__atomic_test_and_set` 这个 GCC builtin，底层编译成 `LOCK BTS` 或者等价的原子指令，配合 `__ATOMIC_ACQUIRE` 内存序保证读操作不会重排到获取锁之前。释放时用 `__atomic_clear` 配合 `__ATOMIC_RELEASE` 内存序，保证临界区内的写操作对其他 CPU 可见。

你可能还注意到自旋循环里有一条 `pause` 指令（`__asm__ volatile("pause")`）。这条指令是给 CPU 的一个提示："我现在在等一个锁，别太着急。"Intel 的 P4 和 Xeon 处理器上，如果不加 PAUSE，spin-wait 循环结束时 CPU 会误判发生了内存顺序违规，触发流水线 flush——性能惩罚非常严重。即使在我们测试用的 QEMU 环境里 PAUSE 相当于 NOP，这也是一个好习惯。

### 为什么需要等待队列

Spinlock 只能用来保护极短的临界区——几个寄存器操作，几条内存读写。一旦你需要在持锁状态下等待某个条件（比如"缓冲区非空"），自旋就变成了浪费 CPU 的无底洞。这时候我们需要的是：把当前任务挂到某个等待队列上，把 CPU 让给别人跑，等条件满足了再被唤醒。

这就是 Mutex 和 Semaphore 要做的事。而它们都需要一个等待队列来记录"谁在等"。

### 侵入式链表——不分配内存的队列

教科书上的链表节点通常长这样：`struct Node { T data; Node* next; }`，每个节点需要动态分配内存。但内核的同步原语不能依赖堆分配——万一堆本身就需要锁保护呢？这就形成了鸡生蛋的问题。

侵入式链表的解法很巧妙：我们不单独分配节点，而是把 `next` 指针直接嵌入被管理的对象里。对 Cinux 来说，就是在 Task 结构体里加一个 `wait_next` 字段。这样一来，一个任务同时只能在一个等待队列上等待（因为只有一个 next 指针），但这在教学 OS 的单核环境里完全够用。

## 动手实现

### Step 1: 在 Task 结构体中添加 wait_next 字段

**目标**: 让 Task 支持被链入等待队列。

**设计思路**: 我们在 `process.hpp` 的 Task 结构体末尾（`sched_class` 之后）添加一个 `Task* wait_next` 指针。这个指针在任务创建时被零初始化（knew 会清零分配的内存），在任务被加入等待队列时由 enqueue 函数设置，离开队列时清零。

**实现约束**: 字段类型是 `Task*`，初始值应该是 nullptr。当任务不在任何等待队列中时，这个值保持 nullptr。

**验证**: 创建一个 Task 后，检查其 wait_next 字段确实为 nullptr。这一步不需要运行，编译通过即可。

### Step 2: 将 Spinlock 的 acquire/release 移到 .cpp 文件

**目标**: Spinlock 之前是头文件内联实现，现在需要拆分到 sync.cpp 中。

**设计思路**: 头文件只保留声明（函数签名），实现移到 sync.cpp。这是为了减少编译依赖，也和 Mutex/Semaphore 的组织方式保持一致。同步原语的实现不需要暴露给调用者。注意 sync.hpp 中还有 RAII Guard 类（构造时 acquire，析构时 release），它仍然留在头文件中因为是内联的。

**踩坑预警**: 移出实现后确保所有 include 路径正确。sync.cpp 需要包含 sync.hpp 和相关头文件。

**验证**: 编译通过即可。

### Step 3: 实现侵入式等待队列的 enqueue 和 dequeue

**目标**: 为 Mutex 和 Semaphore 提供等待队列操作。

**设计思路**: 我们需要两个辅助操作：enqueue（尾插法，保证 FIFO 顺序）和 dequeue（头删法，先进先出）。这两个操作在 Spinlock 的保护下执行，所以它们本身不需要考虑并发安全。

enqueue 的流程是：先将新节点的 wait_next 置为 nullptr，然后遍历链表找到尾部，把尾部节点的 wait_next 指向新节点。如果链表为空（head 是 nullptr），直接把 head 指向新节点。

dequeue 的流程是：如果 head 为 nullptr 就返回 nullptr；否则取 head 指向的节点，把 head 后移到下一个节点，把取出的节点的 wait_next 清零，返回该节点。

**实现约束**: Mutex 和 Semaphore 各自有独立的等待队列（各自有 wait_head_ 成员），所以各自需要独立的 enqueue/dequeue 方法。虽然逻辑完全相同，但由于它们操作的是不同的 wait_head_，所以需要分别实现。

**踩坑预警**: enqueue 时一定要把新节点的 wait_next 设为 nullptr，否则可能误把旧链表尾部之后的数据当成链表的一部分。dequeue 时同样要清零取出的节点的 wait_next。

**验证**: enqueue 三个任务 A、B、C，然后连续 dequeue 三次，验证返回顺序是 A → B → C。

## 构建与运行

编译并运行 host 端单元测试，验证等待队列操作的正确性：

```bash
cd build && cmake .. && make test_sync -j$(nproc)
./test/unit/test_sync
```

预期输出应该显示所有 wait_queue 相关测试通过。

## 调试技巧

1. **wait_next 没有被清零**: 如果 dequeue 后任务的 wait_next 指向了无效地址，下次 enqueue 时可能触发 page fault。在调试器中检查 wait_next 的值。
2. **enqueue 后链表成环**: 这通常是因为忘记设置 `task->wait_next = nullptr`，导致尾部节点的 next 指向了之前的节点。遍历链表时如果无限循环就是这个问题。
3. **FIFO 顺序不对**: 检查 enqueue 是不是用的尾插法（遍历到尾部再插入），而不是头插法。

## 本章小结

| 概念 | 要点 |
|------|------|
| Spinlock | `__atomic_test_and_set` + `pause` + `__atomic_clear`，仅用于极短临界区 |
| 侵入式链表 | `Task::wait_next` 嵌入 Task 结构体，零额外内存分配 |
| enqueue | 尾插法，保证 FIFO 顺序 |
| dequeue | 头删法，先进先出 |
| RAII Guard | 构造获取锁，析构释放锁，防止忘记 unlock |
