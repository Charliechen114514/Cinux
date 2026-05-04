# 021-2 Mutex 与 Semaphore：从设计到实现

> 前置：[021-1 从自旋到阻塞](021-proc-sync-1.md)
> 标签：semaphore, Dijkstra, P/V, counting semaphore, producer-consumer, bounded buffer

## 前言

上一章我们看完了 Spinlock 和 Mutex 的设计哲学，以及侵入式等待队列如何避免堆分配。这一章我们聚焦 Semaphore——Dijkstra 1965 年提出的信号量概念，至今仍然是操作系统同步的基石。我们会拆解它的完整实现，然后用 Semaphore + Mutex 组合解决经典的 Producer-Consumer 问题。

说实话，信号量的设计直觉上很简单——不就是一个带等待队列的计数器吗？但当你真正开始实现的时候，lost wake-up 问题、P/V 操作的原子性、负数计数的语义……这些细节足以让新手抓狂。我们一步步来。

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

## Producer-Consumer——信号量的经典舞台

有了 Mutex 和 Semaphore，我们终于可以解决那个几乎和操作系统本身一样古老的问题了。main.cpp 中创建了一个大小为 4 的环形缓冲区和三个同步原语：

```cpp
static constexpr int PC_BUF_SIZE = 4;
static int g_pc_buf[PC_BUF_SIZE];
static cinux::proc::Semaphore g_sem_free(PC_BUF_SIZE);
static cinux::proc::Semaphore g_sem_used(0);
static cinux::proc::Mutex g_pc_mutex;
```

Producer 的操作顺序是 `wait(sem_free) → guard(mutex) → 写缓冲 → post(sem_used)`，Consumer 是 `wait(sem_used) → guard(mutex) → 读缓冲 → post(sem_free)`。这个顺序保证了两件事：信号量保证时序正确（不会在缓冲区满时写入或空时读取），Mutex 保证缓冲区读写互斥（不会两个任务同时操作缓冲区导致数据混乱）。

Mutex 的 guard() 返回 RAII 对象，用花括号限制作用域。出花括号时 guard 析构自动 unlock——这比手动 lock/unlock 安全得多，即使写入过程中发生异常（虽然内核里没有 C++ 异常），析构函数也会被调用。

## 设计对比：信号量在不同 OS 中的角色

信号量在操作系统教学和实践中的地位差异很大。xv6 完全没有信号量——它只提供 spinlock 和 sleeplock。sleeplock 通过 sleep/wakeup 机制实现阻塞，不需要计数器概念。如果 xv6 要做 producer-consumer，只能用 sleep/wakeup 配合条件变量式的编程模式。这说明信号量虽然经典，但不是唯一的同步方案——sleep/wakeup 通道机制（xv6）或 wait queue + condition variable（Linux）是替代方案。

Linux 提供了独立的 `struct semaphore`（kernel/locking/semaphore.c），但它和 `struct mutex` 是两个完全独立的原语，有不同的 API 和语义。Linux 的 mutex 有乐观自旋、HANDOFF、wake_q 批量唤醒等高级特性；Linux 的 semaphore 更接近经典 Dijkstra 模型，提供 down/down_interruptible/down_trylock/up 操作。Cinux 的设计更接近 Linux——Mutex 和 Semaphore 各自独立实现，而非一个建立在另一个之上。

PintOS 的做法截然不同——它的 Lock 直接构建在 Semaphore 之上（value=1 的信号量加上持有者追踪），Condition Variable 也内部使用 Semaphore。PintOS 这种自底向上的设计适合教学——先学 Semaphore 再学 Lock 和 Condition Variable，层次清晰。但 Cinux 选择了 Linux 的方式，让 Mutex 和 Semaphore 独立实现，因为 Mutex 的所有权语义（只有持有者能 unlock、所有权转移）在 Semaphore 之上很难高效实现。

## 收尾

运行 big kernel 后串口输出应该显示 producer 依次发送 0 到 4、consumer 依次接收 0 到 4，顺序可能交错但数据必须一致。Host 端测试覆盖了 Spinlock、Mutex、Semaphore 的所有边界条件。

下一章我们会聊一个看似无关但极其隐蔽的 bug——添加 sync.cpp 后 BSS 段增长导致的 MOV 编码变体问题，以及 tag 021 的测试策略。

## 参考资料

- Intel SDM: Vol. 3A Section 8.1 "Locked Atomic Operations" -- 原子操作的 x86 实现
- OSDev Wiki: [Semaphore](http://wiki.osdev.org/Semaphore) -- Dijkstra P/V 操作和实现要点
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- 信号量与 producer-consumer 模式
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) -- xv6 不提供 Semaphore，用 sleep/wakeup 替代
- Linux: [kernel/locking/semaphore.c](https://github.com/torvalds/linux/blob/master/kernel/locking/semaphore.c) -- Linux 独立信号量实现
- PintOS: [Semaphore documentation](https://cs162.org/static/proj/pintos-docs/docs/synch/semaphores/) -- Lock 构建在 Semaphore 之上
