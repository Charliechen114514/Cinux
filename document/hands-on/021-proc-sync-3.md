# 021-3 Semaphore 与 Producer-Consumer 实战

> 前置：[021-2 Mutex 阻塞式互斥锁](021-proc-sync-2.md)
> 标签：semaphore, Dijkstra, P/V, counting semaphore, producer-consumer, bounded buffer

## 导语

Mutex 解决了互斥访问的问题——同一时刻只有一个任务能进入临界区。但很多时候我们需要的不仅仅是互斥，还有同步：任务 A 要等任务 B 生产了数据才能继续。经典的 Producer-Consumer 问题就是这种场景的典型代表——生产者往缓冲区里写数据，消费者从里面读，缓冲区满了生产者要等，缓冲区空了消费者要等。Semaphore 正是为这类问题设计的同步原语。

完成本章后，你会掌握 Dijkstra 信号量的 P/V 操作语义，以及如何用 Semaphore + Mutex 组合解决 Producer-Consumer 问题。

## 概念精讲

### Dijkstra 信号量的直觉

信号量可以理解为一个带等待队列的计数器。计数器代表"可用资源的数量"——如果是 3，说明还有 3 个资源可以用；如果是 0，说明资源用完了；如果是负数（比如 -2），说明有 2 个任务在排队等资源。

两个核心操作是 P（wait，荷兰语 proberen = 尝试）和 V（post，荷兰语 verhogen = 增加）。P 操作把计数器减 1，如果减完还是非负就继续（资源够用），如果变成负数就把自己挂到等待队列上睡觉。V 操作把计数器加 1，如果有等待者就唤醒队列头的那个。

这个设计是 Edsger Dijkstra 在 1965 年提出的，至今仍然是操作系统教科书中最基础的同步原语之一。OSDev Wiki 的 Semaphore 页面对这个模型有完整的描述，并且特别强调了实现的两个关键点：一是两个操作必须是原子的，二是等待的任务应该从调度队列中移除以提高效率。

### 计数信号量 vs 二值信号量

计数信号量的初始值可以是任意非负整数。一个初始值为 N 的计数信号量可以用来追踪一个大小为 N 的资源池——比如一个有 4 个槽位的环形缓冲区，空闲槽位信号量初始值为 4，已用槽位信号量初始值为 0。

二值信号量（初始值为 1）在功能上等价于 Mutex，但语义上有微妙区别：Mutex 强调"谁锁谁解"（只有持有者能 unlock），而信号量不关心是哪个任务执行了 V 操作。Cinux 选择了让 Mutex 和 Semaphore 成为两个独立的原语，这和 Linux 的设计一致——Linux 也有独立的 `struct mutex` 和 `struct semaphore`。

### Producer-Consumer 模式

这是一个几乎和操作系统本身一样古老的同步问题。核心设置是：一个固定大小的缓冲区，一个生产者往里面放数据，一个消费者从里面取数据。约束条件有三个：缓冲区满时生产者必须等待、缓冲区空时消费者必须等待、缓冲区的读写必须互斥。

标准解法需要三个同步原语：一个计数信号量追踪空闲槽位（初始值 = 缓冲区大小）、一个计数信号量追踪已用槽位（初始值 = 0）、一个 Mutex 保护缓冲区的读写。

生产者的操作顺序是：先 wait(空闲槽位)——如果缓冲区满就阻塞；然后 lock(Mutex) 保护缓冲区；写入数据；unlock(Mutex)；最后 post(已用槽位)——通知消费者有新数据了。消费者的顺序是对称的：wait(已用槽位) → lock(Mutex) → 读取 → unlock(Mutex) → post(空闲槽位)。

### 关于 Semaphore 的 post() 实现细节

你可能注意到 Cinux 的 post() 实现有一个设计选择：先递增计数器，然后从等待队列取出一个任务，最后如果取到了任务就 unblock 它。这意味着计数器可能在 unblock 之前就已经是正值了。如果此时另一个任务正好调用 wait()，它会看到正的计数器直接成功，而原来在等待的任务也被 unblock 了——这不会导致逻辑错误，因为 unblock 只是让任务回到 Ready 状态，它恢复执行后不会再递减计数器（递减发生在 block 之前）。

## 动手实现

### Step 1: 定义 Semaphore 类

**目标**: 在 sync.hpp 中定义 Semaphore 类。

**设计思路**: Semaphore 的接口比 Mutex 更简洁——post()（V 操作）、wait()（P 操作）、try_wait()（非阻塞 P）、count()（诊断用）。内部持有 Spinlock、int64_t count_ 计数器、Task* wait_head_ 等待队列。构造函数接受一个 initial 参数指定初始计数值。

**实现约束**: count_ 使用 int64_t 而非 int，因为负值表示等待者数量。初始值可以是任意非负整数。

**验证**: 定义编译通过即可。

### Step 2: 实现 post() 和 wait()

**目标**: 实现 Dijkstra P/V 操作。

**设计思路**: post() 先拿 Spinlock，递增 count_，从等待队列取头，释放 Spinlock，如果有等待者就 unblock。wait() 先拿 Spinlock，递减 count_，如果 count_ >= 0 说明资源够用直接返回，否则入队等待、释放 Spinlock、block。

**踩坑预警**: wait() 中"先递减再判断"的顺序很重要。如果你先判断 count_ > 0 再递减，在判断和递减之间可能有另一个任务也通过了判断——这就是经典的 lost wake-up 问题。先递减再判断保证了原子性：如果递减后还是非负，说明之前确实有资源，我们取走了；如果是负数，说明资源已被耗尽，必须等待。

**验证**: 测试以下场景——初始值 3 的信号量连续 wait 三次应该不阻塞（count 降到 0），第四次 wait 应该阻塞（count 变 -1）。连续 post 四次应该依次唤醒四个等待者。

### Step 3: 实现 try_wait() 和 count()

**目标**: 补全非阻塞操作和诊断接口。

**设计思路**: try_wait() 检查 count_，大于 0 则递减并返回 true，否则返回 false。注意判断条件是 count_ <= 0 时返回 false（等于 0 时也没有资源可用）。count() 直接返回 count_ 的当前值。

**验证**: try_wait 在 count > 0 时成功，count <= 0 时失败。

### Step 4: 编写 Producer-Consumer 演示

**目标**: 在 main.cpp 中用 Semaphore + Mutex 实现一个双任务 producer-consumer。

**设计思路**: 创建一个大小为 4 的共享环形缓冲区。声明两个全局 Semaphore：一个初始值为 4（追踪空闲槽位），一个初始值为 0（追踪已用槽位）。再加一个 Mutex 保护缓冲区读写。Producer 任务循环 5 次（发送 0 到 4），Consumer 也循环 5 次。每次循环遵循标准模式：wait(信号量) → lock(Mutex) → 操作缓冲区 → unlock(Mutex) → post(信号量)。

**踩坑预警**: Mutex 的 guard() 返回的 RAII 对象的作用域必须正确覆盖缓冲区操作。把 guard 对象放在一个花括号块里可以确保它在正确的时机析构。如果 guard 的作用域覆盖了整个循环体，那么 post() 要等 unlock 之后才能执行——这虽然不影响正确性但会降低并发度。

**验证**: 运行 big kernel，串口输出应该显示 producer 依次发送 `sent: 0, 1, 2, 3, 4`，consumer 依次接收 `got: 0, 1, 2, 3, 4`。顺序可能交错（因为两个任务并发执行），但接收顺序必须和发送顺序一致（FIFO 保证）。

## 构建与运行

完整构建和运行命令：

```bash
cd build && cmake .. && make big_kernel -j$(nproc)
make run-kernel
```

串口预期输出（交错顺序可能不同）：
```
[BIG] Starting producer-consumer demo (Mutex + Semaphore)...
sent: 0
sent: 1
got: 0
sent: 2
got: 1
sent: 3
got: 2
sent: 4
got: 3
got: 4
```

运行 host 端单元测试验证所有 Semaphore 操作：

```bash
cd build && make test_sync && ./test/unit/test_sync
```

## 调试技巧

1. **Consumer 永远阻塞**: 检查 producer 是否正确地在写入后调用了 post(sem_used)。如果忘记 post，consumer 的 wait 就永远不会返回。
2. **数据错乱**: 如果 consumer 读到了错误的数据，检查 Mutex 是否正确保护了缓冲区读写。一个常见错误是在 Mutex guard 的作用域之外访问缓冲区。
3. **死锁**: 如果两个任务都卡住不动，最可能的原因是锁的获取顺序不一致。在 producer-consumer 模式里，确保总是先 wait(信号量) 再 lock(Mutex)——不要反过来，否则可能拿到 Mutex 之后信号量不够用，抱着锁睡觉，另一个任务想拿 Mutex 也拿不到。
4. **环形缓冲区索引**: 确保使用 `i % BUF_SIZE` 来计算索引，不要用裸的 i。

## 本章小结

| 组件 | 角色 |
|------|------|
| Semaphore(N) | 计数信号量，初始值 N 追踪资源池大小 |
| post() (V) | 递增计数器，唤醒等待者 |
| wait() (P) | 递减计数器，负数时阻塞 |
| Producer 模式 | wait(sem_free) → mutex guard → 写入 → post(sem_used) |
| Consumer 模式 | wait(sem_used) → mutex guard → 读取 → post(sem_free) |
| 互斥 + 同步 | Mutex 保证缓冲区读写互斥，Semaphore 保证生产/消费的时序同步 |
