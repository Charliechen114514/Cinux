---
title: 021-proc-sync-3 · 同步原语
---

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

这个设计是 Edsger Dijkstra 在 1965 年提出的，至今仍然是操作系统教科书中最基础的同步原语之一。OSDev Wiki 的 Semaphore 页面对这个模型有完整的描述，并且特别强调了实现的两个关键点：一是两个操作必须是原子的（在 Cinux 中通过内部 Spinlock 保证），二是等待的任务应该从调度队列中移除以提高效率（通过 block/unblock 实现）。

### 计数信号量 vs 二值信号量

计数信号量的初始值可以是任意非负整数。一个初始值为 N 的计数信号量可以用来追踪一个大小为 N 的资源池——比如一个有 4 个槽位的环形缓冲区，空闲槽位信号量初始值为 4，已用槽位信号量初始值为 0。

二值信号量（初始值为 1）在功能上等价于 Mutex，但语义上有微妙区别：Mutex 强调"谁锁谁解"（只有持有者能 unlock），而信号量不关心是哪个任务执行了 V 操作。Cinux 选择了让 Mutex 和 Semaphore 成为两个独立的原语，这和 Linux 的设计一致——Linux 也有独立的 `struct mutex` 和 `struct semaphore`。PintOS 则走了另一条路——Lock 直接构建在 Semaphore（value=1）之上，形成了自底向上的层次结构。

### Producer-Consumer 模式

这是一个几乎和操作系统本身一样古老的同步问题。核心设置是：一个固定大小的缓冲区，一个生产者往里面放数据，一个消费者从里面取数据。约束条件有三个：缓冲区满时生产者必须等待、缓冲区空时消费者必须等待、缓冲区的读写必须互斥。

标准解法需要三个同步原语：一个计数信号量追踪空闲槽位（初始值 = 缓冲区大小）、一个计数信号量追踪已用槽位（初始值 = 0）、一个 Mutex 保护缓冲区的读写。

生产者的操作顺序是：先 wait(空闲槽位)——如果缓冲区满就阻塞；然后 lock(Mutex) 保护缓冲区；写入数据；unlock(Mutex)；最后 post(已用槽位)——通知消费者有新数据了。消费者的顺序是对称的：wait(已用槽位) -> lock(Mutex) -> 读取 -> unlock(Mutex) -> post(空闲槽位)。

这里有一个非常重要的顺序约束：**总是先 wait(信号量) 再 lock(Mutex)**。如果反过来，先拿 Mutex 再 wait 信号量，万一信号量不够用，任务就会抱着 Mutex 睡觉，另一个任务想拿 Mutex 也拿不到，双方都卡住——死锁。

### 关于 Semaphore 的 post() 实现细节

你可能注意到 Cinux 的 post() 实现有一个设计选择：先递增计数器，然后从等待队列取出一个任务，最后如果取到了任务就 unblock 它。这意味着计数器可能在 unblock 之前就已经是正值了。如果此时另一个任务正好调用 wait()，它会看到正的计数器直接成功，而原来在等待的任务也被 unblock 了——这不会导致逻辑错误，因为 unblock 只是让任务回到 Ready 状态，它恢复执行后不会再递减计数器（递减发生在 block 之前）。

## 动手实现

### Step 1: 定义 Semaphore 类

**目标**: 在 sync.hpp 中定义 Semaphore 类。

**设计思路**: Semaphore 的接口比 Mutex 更简洁——post()（V 操作）、wait()（P 操作）、try_wait()（非阻塞 P）、count()（诊断用）。内部持有 Spinlock、int64_t count_ 计数器、Task* wait_head_ 等待队列。构造函数接受一个 initial 参数指定初始计数值，默认为 0。构造函数标记为 `explicit` 防止隐式转换（`Semaphore s = 3` 会编译报错）。

**实现约束**: count_ 使用 int64_t 而非 int，因为负值表示等待者数量。初始值可以是任意非负整数。和 Mutex 一样，Semaphore 有自己独立的 enqueue_waiter 和 dequeue_waiter 方法，因为操作的是不同的 wait_head_ 成员。

**验证**: 定义编译通过即可。

### Step 2: 实现 post() 和 wait()

**目标**: 实现 Dijkstra P/V 操作。

**设计思路**: post() 先拿 Spinlock，递增 count_，从等待队列取头，释放 Spinlock，如果有等待者就 unblock。wait() 先拿 Spinlock，递减 count_，如果 count_ >= 0 说明资源够用直接返回，否则入队等待、释放 Spinlock、block。

**踩坑预警**: wait() 中"先递减再判断"的顺序很重要。如果你先判断 count_ > 0 再递减，在判断和递减之间可能有另一个任务也通过了判断——这就是经典的 lost wake-up 问题。先递减再判断保证了原子性：如果递减后还是非负，说明之前确实有资源，我们取走了；如果是负数，说明资源已被耗尽，必须等待。这一切在 Spinlock 的保护下完成，不存在并发窗口。

**验证**: 测试以下场景——初始值 3 的信号量连续 wait 三次应该不阻塞（count 降到 0），第四次 wait 应该阻塞（count 变 -1）。连续 post 四次应该依次唤醒四个等待者。内核测试中 `test_wait_decrements_when_positive` 和 `test_wait_blocks_when_zero` 分别覆盖了这两个场景。

### Step 3: 实现 try_wait() 和 count()

**目标**: 补全非阻塞操作和诊断接口。

**设计思路**: try_wait() 检查 count_，大于 0 则递减并返回 true，否则返回 false。注意判断条件是 count_ <= 0 时返回 false（等于 0 时也没有资源可用）。count() 直接返回 count_ 的当前值。注意 count() 返回的值在并发环境下可能立即过时——它只适合调试输出，不能用于逻辑判断（比如 `if (s.count() > 0) s.wait()` 在并发环境下是错误的）。

**验证**: try_wait 在 count > 0 时成功，count <= 0 时失败。内核测试中 `test_try_wait_success`、`test_try_wait_fail_zero` 和 `test_try_wait_all` 三个测试覆盖了这个场景。

### Step 4: 编写 Producer-Consumer 演示

**目标**: 在 main.cpp 中用 Semaphore + Mutex 实现一个双任务 producer-consumer，替换掉 tag 020 的 6 线程抢占 demo。

**设计思路**: 创建一个大小为 4 的共享环形缓冲区。声明两个全局 Semaphore：一个初始值为 4（追踪空闲槽位），一个初始值为 0（追踪已用槽位）。再加一个 Mutex 保护缓冲区读写。Producer 任务循环 5 次（发送 0 到 4），Consumer 也循环 5 次。每次循环遵循标准模式：wait(信号量) -> lock(Mutex) -> 操作缓冲区 -> unlock(Mutex) -> post(信号量)。

**踩坑预警**: Mutex 的 guard() 返回的 RAII 对象的作用域必须正确覆盖缓冲区操作。把 guard 对象放在一个花括号块里可以确保它在正确的时机析构。如果 guard 的作用域覆盖了整个循环体，那么 post() 要等 unlock 之后才能执行——这虽然不影响正确性但会降低并发度。在 Cinux 的实现中，我们用独立的花括号块精确限制 guard 的生命周期。

环形缓冲区的索引必须用 `i % PC_BUF_SIZE` 计算，不能直接用 i。虽然我们只发 5 个数据（0-4）、缓冲区大小为 4，但取模保证了即使数据量超过缓冲区大小也能正确回绕。

**验证**: 运行 big kernel，串口输出应该显示 producer 依次发送 `sent: 0, 1, 2, 3, 4`，consumer 依次接收 `got: 0, 1, 2, 3, 4`。顺序可能交错（因为两个任务并发执行），但接收顺序必须和发送顺序一致（FIFO 保证）。

## 信号量的 count_ 为什么用 int64_t

在实现 Semaphore 之前，有一个设计选择值得讨论：count_ 为什么用 `int64_t` 而不是 `int` 或者 `uint32_t`？这个选择直接影响了信号量能表达的状态范围。

原因是负数的语义。Dijkstra 信号量的一个关键特性是：当 count 变成负数时，它的绝对值表示正在等待的任务数量。比如 count_ = -3 表示有 3 个任务在等待队列上。如果用无符号类型（uint32_t），减到 0 之后再减会下溢变成一个巨大的正数（约 40 亿），逻辑就完全错了——信号量会以为有海量资源可用。如果用 32 位有符号整数（int32_t），理论上最多追踪 2^31 个等待者，这当然足够了——但使用 64 位是为了和 x86-64 的自然字长匹配，避免不必要的符号扩展指令。

另一个考虑是：将来的 64 位系统可能需要追踪大量资源（比如一个大的磁盘块缓存），64 位计数器可以避免溢出问题。虽然教学 OS 里不太可能遇到这种场景，但使用 int64_t 没有额外开销（x86-64 上 64 位操作和 32 位一样快，寄存器都是 64 位的），而且更安全。

注意 count_ 不是 atomic 变量——它的原子性通过 Spinlock 保护。任何对 count_ 的读写都在 Spinlock 的临界区内完成，不需要额外的原子操作。

## 从 main.cpp 的变化看整体设计

tag 021 的 main.cpp 发生了很大的变化。tag 020 的 main.cpp 创建了 6 个线程（A 到 F），每个线程做一个忙等循环（`for (volatile int j = 0; j < 20000000; j++) {}`）并打印自己的标签和迭代次数。这个 demo 展示了抢占式调度能工作，但完全没有展示同步。

tag 021 把 6 个线程全部替换成 2 个协作任务：producer 和 consumer。这个变化的意义不仅在于代码更简洁——它展示了内核从"独立运行的线程"到"协作的线程"的转变。6 个线程之间没有共享数据，不需要同步原语。而 producer-consumer 天然需要同步——缓冲区是共享的，两个任务必须协调对它的访问。

四个全局变量构成了 producer-consumer 的完整同步框架：`g_pc_buf[4]` 是环形缓冲区本身，`g_sem_free` 初始值 4 追踪空闲槽位，`g_sem_used` 初始值 0 追踪已用槽位，`g_pc_mutex` 保护缓冲区的读写。这四个变量在 kernel_main() 执行之前就已经被初始化好了（全局变量的静态初始化）。

Producer 发送 0 到 4 共 5 个数据。每次循环的流程严格遵循：wait(sem_free) 消耗空闲槽位 -> 用 Mutex guard 保护缓冲区写入 -> post(sem_used) 增加已用槽位。Consumer 的流程镜像对称：wait(sem_used) 等待数据 -> 用 Mutex guard 保护缓冲区读取 -> post(sem_free) 归还空闲槽位。两个任务由 PIT 定时器中断驱动的抢占式调度交替执行。

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

预期输出应该显示 Semaphore 相关测试全部 PASS，包括 `semaphore: initial count`、`semaphore: post increments count`、`semaphore: wait decrements count when positive`、`semaphore: wait blocks when count is zero`、`semaphore: FIFO ordering of waiters` 和 `semaphore: counting semaphore pattern`。

## 关于 magic check 的一个隐蔽 bug

在 tag 021 的开发过程中，我们遇到了一个非常隐蔽的 bug，记录在 `document/notes/021/001_big_kernel_magic_check.md` 中。这个 bug 和同步原语本身无关，但和添加 sync.cpp 后的副作用有关，值得在这里提一下。

现象很诡异：mini kernel 测试全部通过，但 big kernel 测试从未执行，直接退出。原因是 mini kernel 在跳转 big kernel 之前会检查入口字节，验证 ELF 入口点确实是 `_start` 函数。`_start` 的前两条指令是 `cli`（opcode 0xFA）和 `movq $__kernel_stack_top, %rsp`。旧检查只认一种编码：`0xFA 0x48 0xBC`。

问题出在 x86-64 对 `movq $imm, %rsp` 有两种合法编码。`48 BC <8字节立即数>` 是无条件的 64 位编码，`48 C7 C4 <4字节符号扩展立即数>` 是更短的编码，在立即数可以符号扩展到 64 位时使用。添加 sync.cpp（211 行新代码）后 BSS 段增长，导致 `__kernel_stack_top` 的链接地址从需要 8 字节编码变成了可以用 4 字节符号扩展表示。assembler 自动选择了更短的 `48 C7 C4` 编码，而旧检查只认 `48 BC`，直接判定"不是真内核"。

修复方案同时接受两种编码。这个 bug 的教训是：x86-64 的指令编码不是唯一的，assembler 会根据操作数自动选择最优编码。任何基于机器码字节模式的检查都应覆盖所有合法编码变体。

## 调试技巧

1. **Consumer 永远阻塞**: 检查 producer 是否正确地在写入后调用了 post(sem_used)。如果忘记 post，consumer 的 wait 就永远不会返回。这是最常见的 bug——信号量的 V 操作漏写。
2. **数据错乱**: 如果 consumer 读到了错误的数据，检查 Mutex 是否正确保护了缓冲区读写。一个常见错误是在 Mutex guard 的作用域之外访问缓冲区。
3. **死锁**: 如果两个任务都卡住不动，最可能的原因是锁的获取顺序不一致。在 producer-consumer 模式里，确保总是先 wait(信号量) 再 lock(Mutex)——不要反过来，否则可能拿到 Mutex 之后信号量不够用，抱着锁睡觉，另一个任务想拿 Mutex 也拿不到。
4. **环形缓冲区索引**: 确保使用 `i % BUF_SIZE` 来计算索引，不要用裸的 i。
5. **信号量初始值错误**: `sem_free` 的初始值应该是缓冲区大小（4），`sem_used` 的初始值应该是 0。如果搞反了，producer 会直接阻塞或者 consumer 会读垃圾数据。

## 扩展方向：Semaphore 的进阶应用

虽然 Producer-Consumer 是 Semaphore 最经典的用例，但信号量还有许多其他应用场景值得了解。

**Barrier 同步**: 用一个初始值为 0 的 Semaphore 和一个计数器可以实现 N 个任务的屏障同步——所有任务都到达屏障点后才一起继续。每个到达的任务递减计数器，最后一个到达的任务 post N-1 次唤醒所有等待者。

**Reader-Writer Lock**: 允许多个读者并行但写者独占的锁，可以用 Semaphore 和 Mutex 组合实现。用 Semaphore 追踪当前读者数量，Mutex 保护这个计数器的修改。写者需要等待 Semaphore 降到 0（没有读者）才能获取独占访问。

**Condition Variable**: 实现 `wait(condition, mutex)` 和 `signal(condition)` 语义，允许任务在等待条件成立时释放 Mutex。PintOS 的 Condition Variable 就是内部使用 Semaphore 实现的。Linux 使用 `wait_queue_head_t` 和 `prepare_to_wait`/`finish_wait` 实现了更高效的条件等待。

## 设计对比：信号量在不同 OS 中的角色

信号量在操作系统教学和实践中的地位差异很大。xv6 完全没有信号量——它只提供 spinlock 和 sleeplock。sleeplock 通过 sleep/wakeup 机制实现阻塞，不需要计数器概念。如果 xv6 要做 producer-consumer，只能用 sleep/wakeup 配合条件变量式的编程模式。这说明信号量虽然经典，但不是唯一的同步方案——sleep/wakeup 通道机制（xv6）或 wait queue + condition variable（Linux）是替代方案。

Linux 提供了独立的 `struct semaphore`（kernel/locking/semaphore.c），但它和 `struct mutex` 是两个完全独立的原语。Linux 的 semaphore 提供了 down/down_interruptible/down_trylock/up 操作，更接近经典 Dijkstra 模型。Cinux 的设计更接近 Linux——Mutex 和 Semaphore 各自独立实现，而非一个建立在另一个之上。

## 参考资料

- OSDev Wiki: [Semaphore](http://wiki.osdev.org/Semaphore) -- Dijkstra 信号量定义和 producer-consumer 示例
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- P/V 操作的原始描述
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) -- xv6 不提供 Semaphore
- Linux: [kernel/locking/semaphore.c](https://github.com/torvalds/linux/blob/master/kernel/locking/semaphore.c) -- Linux 独立信号量实现
- PintOS: [Semaphore documentation](https://cs162.org/static/proj/pintos-docs/docs/synch/semaphores/) -- Lock 构建在 Semaphore 之上

## 本章小结

| 组件 | 角色 |
|------|------|
| Semaphore(N) | 计数信号量，初始值 N 追踪资源池大小 |
| post() (V) | 递增计数器，唤醒等待者 |
| wait() (P) | 先递减再判断，负数时阻塞（避免 lost wake-up） |
| try_wait() | 非阻塞 P，count > 0 时递减成功 |
| Producer 模式 | wait(sem_free) -> mutex guard -> 写入 -> post(sem_used) |
| Consumer 模式 | wait(sem_used) -> mutex guard -> 读取 -> post(sem_free) |
| 互斥 + 同步 | Mutex 保证缓冲区读写互斥，Semaphore 保证生产/消费的时序同步 |
| 顺序约束 | 总是先 wait(信号量) 再 lock(Mutex)，避免死锁 |

## 下一步预告

到这里 tag 021 就大功告成了。我们实现了 Spinlock 的重构、Mutex 的阻塞式互斥锁、Semaphore 的 Dijkstra P/V 操作，用三者的组合解决了 Producer-Consumer 问题，还踩了一个极其隐蔽的 x86 编码陷阱。

这些同步原语是后续所有内核功能的基础——文件系统需要 Mutex 保护 inode 缓存，网络栈需要 Semaphore 管理缓冲区，用户态进程需要同步来安全地共享内存。确保你的 producer-consumer demo 在 QEMU 中正确运行，串口输出显示 `sent: 0` 到 `sent: 4` 和 `got: 0` 到 `got: 4`，这是后续所有工作的基础。

下一章我们将进入 tag 022——Ring 3 用户态，Cinux 的内核线程终于要走到用户空间了。
