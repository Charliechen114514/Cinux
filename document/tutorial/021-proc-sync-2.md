# 021-2 Mutex 与 Semaphore：从设计到实现

> 前置：[021-1 从自旋到阻塞](021-proc-sync-1.md)
> 标签：semaphore, Dijkstra, P/V, counting semaphore, producer-consumer, bounded buffer

## 前言

上一章我们看完了 Spinlock 和 Mutex 的设计哲学，以及侵入式等待队列如何避免堆分配。这一章我们聚焦 Semaphore——Dijkstra 1965 年提出的信号量概念，至今仍然是操作系统同步的基石。我们会拆解它的完整实现，然后用 Semaphore + Mutex 组合解决经典的 Producer-Consumer 问题。

说实话，信号量的设计直觉上很简单——不就是一个带等待队列的计数器吗？但当你真正开始实现的时候，lost wake-up 问题、P/V 操作的原子性、负数计数的语义……这些细节足以让新手抓狂。我们一步步来。

## 信号量的数学直觉

在深入代码之前，让我们先建立一个直觉模型。想象一个停车场：入口有一个电子显示屏显示"剩余车位：N"。每开进去一辆车，N 减 1（P 操作）。每开出来一辆车，N 加 1（V 操作）。如果 N 减到 0 还有人想进来，那就得在门口等着。如果 N 已经是 0 了还有人继续减（变成 -1、-2...），门口排队的人就越来越多。

这个模型完美对应了信号量的语义：N 就是 count_，门口排队的人就是等待队列。初始值 N 表示资源池大小。P 操作（wait）获取一个资源，V 操作（post）释放一个资源。count_ 为正表示有资源可用，为零表示资源耗尽但没人等，为负表示有 |count_| 个任务在排队。

Dijkstra 在 1965 年用数学语言精确定义了这个模型。P 和 V 分别来自荷兰语的 proberen（尝试）和 verhogen（增加）。这两个操作是原子的——在 Cinux 中通过内部 Spinlock 保证。OSDev Wiki 的 Semaphore 页面对这个模型有完整的描述，特别强调的两个关键点（原子性和等待者移除）在 Cinux 中都得到了满足。

## Semaphore 的接口设计

```cpp
class Semaphore {
public:
    explicit Semaphore(int64_t initial = 0);
    void post();     // V operation
    void wait();     // P operation
    bool try_wait();
    int64_t count() const;
private:
    Spinlock spin_;
    int64_t  count_;
    Task*    wait_head_ = nullptr;
    void enqueue_waiter(Task* task);
    Task* dequeue_waiter();
};
```

和 Mutex 相比，Semaphore 把 `owner_` 换成了 `count_`——一个 int64_t 计数器。正值表示可用资源数量，零表示资源耗尽但没人等，负值表示资源耗尽且有 |count_| 个任务在排队。`explicit` 防止隐式转换（`Semaphore s = 3` 会编译报错），避免意图不明确的代码。

构造函数接受初始计数值：`Semaphore(4)` 表示 4 个资源立即可用，`Semaphore(0)` 表示零资源（第一个 wait 就阻塞）。这个设计直接对应 producer-consumer 模式中的两个信号量——空闲槽位信号量初始为缓冲区大小，已用槽位信号量初始为零。

## post()——V 操作的实现

```cpp
void Semaphore::post() {
    spin_.acquire();
    count_++;
    Task* waiter = dequeue_waiter();
    spin_.release();
    if (waiter != nullptr) {
        Scheduler::unblock(waiter);
    }
}
```

post() 的实现非常直白：先递增计数器，然后检查有没有等待者，有就唤醒一个。你可能注意到 `dequeue_waiter()` 是无条件调用的——即使没有等待者，它也只是返回 nullptr，不会出错。

这里有个微妙之处：我们先递增 count_ 再取等待者。如果在递增和取等待者之间，恰好有另一个任务调用了 wait() 并且拿到了我们刚递增的资源呢？没关系——因为这一切都在 Spinlock 的保护下，不会被中断。而且即使真的有新来的任务抢到了资源，等待队列里的任务也会被 unblock（如果有的话），恢复执行后它会从 block 点继续，不会再次递减 count_（递减发生在 block 之前）。

## wait()——P 操作与 lost wake-up

```cpp
void Semaphore::wait() {
    spin_.acquire();
    count_--;
    if (count_ >= 0) {
        spin_.release();
        return;
    }
    Task* self = g_per_cpu.current;
    enqueue_waiter(self);
    spin_.release();
    Scheduler::block(self, "semaphore");
}
```

wait() 中最关键的设计选择是**先递减再判断**。count_ 无条件减 1，然后看结果：>= 0 说明递减前至少有一个资源，我们取走了它，直接返回；< 0 说明资源已耗尽，入队等待。

为什么不先判断再递减？考虑 count_ = 1 时两个任务几乎同时调用 wait()。如果先判断 `count_ > 0`（都看到 1，都通过），再递减（都减 1，变成 -1），两个任务都以为自己拿到了资源——实际上只有一个。这就是 OSDev Wiki 上提到的 lost wake-up 问题：信号量递减的"判断"和"操作"之间如果存在窗口，并发任务可能同时通过判断，导致计数器状态不一致。

先递减再判断在 Spinlock 的保护下是原子的——不存在任何窗口。递减和判断在同一个 Spinlock 临界区内完成，其他任务无法干预。

## try_wait() 和 count()

```cpp
bool Semaphore::try_wait() {
    spin_.acquire();
    if (count_ <= 0) {
        spin_.release();
        return false;
    }
    count_--;
    spin_.release();
    return true;
}
```

try_wait 是非阻塞版本——count_ > 0 时递减返回 true，否则返回 false。注意判断条件是 `<= 0`（等于 0 时也没资源），和 wait() 中先递减再判断的策略不同。这是因为 try_wait 不需要处理阻塞路径，直接检查即可。

`count()` 返回当前计数值，标记为 const。这个值在并发环境下可能立即过时——它只适合调试输出，不能用于逻辑判断。

## 二值信号量 vs Mutex——看起来一样，实际不同

计数信号量的初始值可以是任意非负整数。一个初始值为 N 的计数信号量可以用来追踪一个大小为 N 的资源池——比如一个有 4 个槽位的环形缓冲区，空闲槽位信号量初始值为 4，已用槽位信号量初始值为 0。

二值信号量（初始值为 1）在功能上等价于 Mutex，但语义上有微妙区别：Mutex 强调"谁锁谁解"（只有持有者能 unlock），而信号量不关心是哪个任务执行了 V 操作。Cinux 选择了让 Mutex 和 Semaphore 成为两个独立的原语，这和 Linux 的设计一致——Linux 也有独立的 `struct mutex` 和 `struct semaphore`。PintOS 则走了另一条路——Lock 直接构建在 Semaphore（value=1）之上，形成了自底向上的层次结构。

## Producer-Consumer——信号量的经典舞台

有了 Mutex 和 Semaphore，我们终于可以解决那个几乎和操作系统本身一样古老的问题了。main.cpp 中创建了一个大小为 4 的环形缓冲区和三个同步原语：

```cpp
static constexpr int PC_BUF_SIZE = 4;
static int g_pc_buf[PC_BUF_SIZE];
static cinux::proc::Semaphore g_sem_free(PC_BUF_SIZE);
static cinux::proc::Semaphore g_sem_used(0);
static cinux::proc::Mutex g_pc_mutex;
```

Producer 的操作顺序是 `wait(sem_free) -> guard(mutex) -> 写缓冲 -> post(sem_used)`，Consumer 是 `wait(sem_used) -> guard(mutex) -> 读缓冲 -> post(sem_free)`。这个顺序保证了两件事：信号量保证时序正确（不会在缓冲区满时写入或空时读取），Mutex 保证缓冲区读写互斥（不会两个任务同时操作缓冲区导致数据混乱）。

这里有一个非常重要的顺序约束：**总是先 wait(信号量) 再 lock(Mutex)**。如果反过来，先拿 Mutex 再 wait 信号量，万一信号量不够用，任务就会抱着 Mutex 睡觉，另一个任务想拿 Mutex 也拿不到，双方都卡住——死锁。

Mutex 的 guard() 返回 RAII 对象，用花括号限制作用域。出花括号时 guard 析构自动 unlock——这比手动 lock/unlock 安全得多，即使写入过程中发生异常（虽然内核里没有 C++ 异常），析构函数也会被调用。

tag 021 的 main.cpp 把 tag 020 的 6 线程抢占 demo 全部替换成了 producer 和 consumer 两个协作任务。这个变化不仅简化了代码，更重要的是展示了内核从"独立运行的线程"到"协作的线程"的转变。6 个线程之间没有共享数据，不需要同步原语。而 producer-consumer 天然需要同步——缓冲区是共享的，两个任务必须协调对它的访问。

这四个全局变量构成了 producer-consumer 的完整同步框架。`g_pc_buf[4]` 是环形缓冲区本身。`g_sem_free` 初始值 4 追踪空闲槽位——producer 每次写入前要 wait 一个空闲槽位，consumer 读取后 post 归还一个空闲槽位。`g_sem_used` 初始值 0 追踪已用槽位——producer 写入后 post 增加一个已用槽位，consumer 每次读取前要 wait 一个已用槽位。`g_pc_mutex` 保护缓冲区的读写互斥。

## Semaphore 的 count_ 为什么用 int64_t

这个设计选择值得展开讨论。count_ 用 int64_t 而非 int 或 uint32_t，原因是负数的语义。当 count_ 变成负数时，它的绝对值表示正在等待的任务数量。比如 count_ = -3 表示有 3 个任务在等待队列上。如果用无符号类型，减到 0 之后再减会下溢变成一个巨大的正数（约 40 亿），逻辑就完全错了。

使用 64 位而不是 32 位有符号整数，是为了和 x86-64 的自然字长匹配，避免不必要的符号扩展指令。在 x86-64 上 64 位操作和 32 位一样快，寄存器都是 64 位的，使用 int64_t 没有额外开销。注意 count_ 不是 atomic 变量——它的原子性通过 Spinlock 保护。任何对 count_ 的读写都在 Spinlock 的临界区内完成，不需要额外的原子操作。

## 设计对比：信号量在不同 OS 中的角色

信号量在操作系统教学和实践中的地位差异很大。xv6 完全没有信号量——它只提供 spinlock 和 sleeplock。sleeplock 通过 sleep/wakeup 机制实现阻塞，不需要计数器概念。如果 xv6 要做 producer-consumer，只能用 sleep/wakeup 配合条件变量式的编程模式。这说明信号量虽然经典，但不是唯一的同步方案——sleep/wakeup 通道机制（xv6）或 wait queue + condition variable（Linux）是替代方案。

Linux 提供了独立的 `struct semaphore`（kernel/locking/semaphore.c），但它和 `struct mutex` 是两个完全独立的原语，有不同的 API 和语义。Linux 的 mutex 有乐观自旋、HANDOFF、wake_q 批量唤醒等高级特性；Linux 的 semaphore 更接近经典 Dijkstra 模型，提供 down/down_interruptible/down_trylock/up 操作。Cinux 的设计更接近 Linux——Mutex 和 Semaphore 各自独立实现，而非一个建立在另一个之上。

PintOS 的做法截然不同——它的 Lock 直接构建在 Semaphore 之上（value=1 的信号量加上持有者追踪），Condition Variable 也内部使用 Semaphore。PintOS 这种自底向上的设计适合教学——先学 Semaphore 再学 Lock 和 Condition Variable，层次清晰。但 Cinux 选择了 Linux 的方式，让 Mutex 和 Semaphore 独立实现，因为 Mutex 的所有权语义（只有持有者能 unlock、所有权转移）在 Semaphore 之上很难高效实现。

## 收尾

运行 big kernel 后串口输出应该显示 producer 依次发送 0 到 4、consumer 依次接收 0 到 4，顺序可能交错但数据必须一致。Host 端测试覆盖了 Spinlock、Mutex、Semaphore 的所有边界条件。内核集成测试在 QEMU 中验证了真实调度环境下的行为。

信号量的强大之处在于它的通用性。一个初始值为 N 的计数信号量可以管理 N 个同类资源。一个初始值为 0 的信号量可以用作同步屏障——一个任务 wait 等待另一个任务 post。两个信号量配合可以实现有界缓冲区。三个信号量可以实现 Reader-Writer Lock。这种极简但通用的设计是 Dijkstra 信号量历经 60 年仍然被广泛使用的原因。

Semaphore 和 Mutex 的组合是操作系统同步的基石。虽然现代内核还提供 Condition Variable、ReadWrite Lock、Barrier 等更高级的同步原语，但它们大多可以建立在 Semaphore 和 Mutex 之上。理解了 Semaphore 的 P/V 操作和 Mutex 的互斥保护，你就掌握了理解任何同步机制的基础。

下一章我们会聊一个看似无关但极其隐蔽的 bug——添加 sync.cpp 后 BSS 段增长导致的 MOV 编码变体问题，以及 tag 021 的测试策略。

## 参考资料

- Intel SDM: Vol. 3A Section 8.1 "Locked Atomic Operations" -- 原子操作的 x86 实现
- OSDev Wiki: [Semaphore](http://wiki.osdev.org/Semaphore) -- Dijkstra P/V 操作和实现要点
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- 信号量与 producer-consumer 模式
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) -- xv6 不提供 Semaphore，用 sleep/wakeup 替代
- Linux: [kernel/locking/semaphore.c](https://github.com/torvalds/linux/blob/master/kernel/locking/semaphore.c) -- Linux 独立信号量实现
- PintOS: [Semaphore documentation](https://cs162.org/static/proj/pintos-docs/docs/synch/semaphores/) -- Lock 构建在 Semaphore 之上
