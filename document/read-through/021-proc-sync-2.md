---
title: 021-proc-sync-2 · 同步原语
---

# 021-2 Semaphore 完整代码精讲

> 前置：[021-1 Spinlock + Mutex 代码精讲](021-proc-sync-1.md)
> 聚焦：sync.hpp Semaphore 类定义 + sync.cpp Semaphore 实现 + Dijkstra P/V 语义

## 概览

本文是 tag 021 Read-through 的第二篇，聚焦 Semaphore（信号量）的完整实现。我们逐段拆解 Semaphore 的类定义和每个方法的代码，深入理解 Dijkstra 经典 P/V 操作的语义和实现细节。

## Semaphore 类定义

Semaphore 的接口比 Mutex 简洁，但语义更加通用：

```cpp
class Semaphore {
public:
    explicit Semaphore(int64_t initial = 0);
    void post();
    void wait();
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

和 Mutex 类似，Semaphore 内部也用一个 Spinlock 保护自己的元数据，但把 `owner_` 换成了 `count_`——一个可以取任意 int64_t 值的计数器。`wait_head_` 同样指向侵入式等待队列的头部。

构造函数接受一个 `initial` 参数，默认值为 0。这意味着默认构造的信号量表示"零个可用资源"——第一个 wait() 调用就会阻塞。传入正值表示初始可用资源数量，比如 `Semaphore(4)` 表示 4 个资源立即可用。

`explicit` 关键字防止隐式转换——你不能写 `Semaphore s = 3;`，必须写 `Semaphore s(3);`。这是一个好习惯，避免意图不明确的隐式构造。

## 构造函数

```cpp
Semaphore::Semaphore(int64_t initial)
    : count_(initial), wait_head_(nullptr) {}
```

构造函数做的就是初始化计数器和等待队列头。Spinlock 的 `locked_` 通过类内默认值 `= false` 初始化，不需要在初始化列表中显式写出。

## post() — V 操作

post() 是信号量的 V 操作（verhogen，增加），负责释放资源并唤醒等待者：

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

这五步非常精确：获取 Spinlock → 递增计数器 → 从等待队列取出头部任务 → 释放 Spinlock → 如果有等待者就 unblock。

你可能会想：为什么不是"如果有等待者就不递增计数器"？确实有这种实现变体——直接把"资源"交给唤醒的等待者而不是递增计数器。Cinux 的实现选择了更简单的做法：总是递增计数器，然后检查是否有等待者需要唤醒。这意味着在 unblock 之后，count_ 的值可能暂时比预期多 1（等待者还没来得及恢复执行并"消费"这个资源），但这不会导致逻辑错误。

`Scheduler::unblock()` 把任务状态从 Blocked 改为 Ready 并重新加入调度队列。注意 unblock 是在 Spinlock 释放之后调用的——和 Mutex::unlock() 一样，避免在持锁状态下做调度操作。

## wait() — P 操作

wait() 是信号量的 P 操作（proberen，尝试），负责获取资源或阻塞等待：

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

这里有一个非常关键的设计选择：**先递减再判断**。count_ 先无条件减 1，然后检查结果。如果 >= 0，说明递减之前至少有 1 个资源可用，我们取走了它，直接返回。如果 < 0，说明资源已经被耗尽（负数的绝对值就是等待者数量），需要入队等待。

为什么不能先判断再递减？考虑这个场景：count_ = 1，两个任务几乎同时调用 wait()。如果先判断 count_ > 0（都通过），再递减（都递减到 -1），两个任务都以为拿到了资源——但实际上只有一个资源。这就是经典的 lost wake-up 问题。先递减再判断在 Spinlock 的保护下是原子的，避免了这个问题。

阻塞路径和 Mutex 完全一致：入队 → 释放 Spinlock → block。`Scheduler::block()` 把任务标记为 Blocked 并调用 `schedule()` 切换到其他任务。

## try_wait() — 非阻塞 P

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

try_wait 是 wait 的非阻塞版本。注意判断条件是 `count_ <= 0`——等于 0 时也没有资源可用（wait 会把它减到 -1 然后阻塞）。只有在 count_ 严格大于 0 时才能成功递减。

和 Mutex 的 try_lock 不同的是，Semaphore 的 try_wait 检查的是一个范围（> 0）而非一个二值状态（== nullptr）。

## count() — 诊断接口

```cpp
int64_t Semaphore::count() const {
    return count_;
}
```

`count()` 返回当前计数器的值。这个方法标记为 `const` 因为它不修改对象状态。注意这个值在并发环境下可能立即过时——它只适合用于调试和断言，不能用来做逻辑判断（比如 `if (s.count() > 0) s.wait();` 这种写法在并发环境下是错误的）。

## 设计决策

### 决策：explicit 构造函数

**问题**: Semaphore 的构造函数为什么标记为 `explicit`？
**本项目的做法**: `explicit Semaphore(int64_t initial = 0);`
**备选方案**: 不加 explicit，允许隐式转换。
**为什么不选备选方案**: 如果不加 explicit，你可以写 `Semaphore s = 3;`，这看起来像在给信号量赋值 3，但实际上是在构造一个初始值为 3 的信号量。这种隐式转换容易让读者误解代码意图。加上 explicit 后，必须写 `Semaphore s(3);`，意图明确。这是 C++ 最佳实践——单参数构造函数默认应该加 explicit，除非你有意支持隐式转换（比如 `std::string` 从 `const char*` 的隐式转换）。

### 决策：先递减再判断 vs 先判断再递减

**问题**: wait() 中应该先递减 count_ 还是先判断？
**本项目的做法**: 先递减再判断。count_ 无条件减 1，然后检查结果。
**备选方案**: 先判断 count_ > 0 再递减（需要 else 分支处理阻塞）。
**为什么不选备选方案**: 先判断再递减在判断和递减之间存在窗口，需要更复杂的同步来保证原子性。先递减再判断在 Spinlock 的保护下天然原子。
**如果要扩展**: 对于需要更细粒度控制的场景（如 priority semaphore），可以在入队时按优先级排序而非简单尾插。

## 代码行数与复杂度分析

Semaphore 的 sync.cpp 实现总共约 60 行代码（不含 Spinlock 和 Mutex 的实现），相比之下 Mutex 约为 50 行。额外的复杂度来自 count_ 的负值语义——Mutex 只有一个二元状态（locked/unlocked），而 Semaphore 的状态空间是一整个整数范围。

构造函数只有 1 行，post() 约 10 行，wait() 约 12 行，try_wait() 约 8 行，count() 1 行，enqueue_waiter 和 dequeue_waiter 各约 10 行。整个 Semaphore 实现没有使用任何条件变量的概念——只有 Spinlock + count_ + 等待队列三个组件。这种极简设计正是 Dijkstra 信号量的魅力所在：用最少的原语表达最丰富的同步语义。

### 决策：Mutex 和 Semaphore 作为独立原语

**问题**: Mutex 应该建立在 Semaphore 之上吗？
**本项目的做法**: Mutex 和 Semaphore 完全独立实现。
**备选方案**: 用 Semaphore(1) 实现 Mutex（PintOS 的做法）。
**为什么不选备选方案**: Mutex 有额外的 owner 追踪语义（只有持有者能 unlock），Semaphore 不具备。独立实现让 Mutex 的 unlock 可以做所有权转移而非简单的 V 操作，效率更高。
**如果要扩展**: 可以增加 Condition Variable 原语，内部使用 Semaphore 实现（类似 PintOS 的设计）。

## 等待队列操作（Semaphore 版本）

Semaphore 的 enqueue_waiter 和 dequeue_waiter 实现和 Mutex 版本完全相同——都是侵入式链表的尾插头删操作。之所以重复实现而不是共享代码，是因为它们操作的是不同的 `wait_head_` 成员。Mutex 的 `wait_head_` 和 Semaphore 的 `wait_head_` 是完全独立的链表头指针，各自追踪各自的等待者。

如果将来想要消除重复代码，有几种方案。一种是提取一个独立的辅助函数，接受 `Task*& head` 和 `Task* task` 作为参数。另一种是用模板或者基类把等待队列逻辑抽象出来。但在教学 OS 的场景下，保持各自独立实现更清晰——读者不需要跳转多个文件就能理解一个原语的完整逻辑。Cinux 选择了代码清晰度优于 DRY 原则。

## 扩展方向

- **Barrier 同步**: 用 Semaphore 实现 N 个任务的屏障同步——所有任务都到达屏障点后才一起继续（难度: 2 星）
- **Reader-Writer Lock**: 允许多个读者并行但写者独占的锁，可以用 Semaphore 和 Mutex 组合实现（难度: 2 星）
- **Condition Variable**: 实现 `wait(signal, mutex)` 和 `signal(signal)` 语义，允许任务在等待条件成立时释放 Mutex（难度: 3 星）

## 参考资料

- OSDev Wiki: [Semaphore](http://wiki.osdev.org/Semaphore) -- Dijkstra 信号量定义和 producer-consumer 示例
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- P/V 操作的原始描述
- PintOS: [Semaphore documentation](https://cs162.org/static/proj/pintos-docs/docs/synch/semaphores/) -- PintOS 的 sema_down/sema_up 实现对比
- xv6: 不提供 Semaphore，只有 sleeplock -- 这和 Cinux 的设计选择形成对比

## 与 Mutex 的实现差异对比

虽然 Semaphore 和 Mutex 都使用相同的等待队列机制（侵入式单向链表、尾插头删），但它们的实现有几个关键差异值得总结。

第一个差异是状态表示。Mutex 用一个指针（`owner_`）表示状态：nullptr 表示空闲，非空表示被持有。Semaphore 用一个整数（`count_`）表示状态：正值表示可用资源数，零表示耗尽，负值的绝对值表示等待者数。这意味着 Semaphore 的状态空间比 Mutex 大得多——Mutex 只有两种状态，Semaphore 有无限种。

第二个差异是阻塞条件。Mutex 的阻塞条件是"owner 不为空"（有人持有锁）。Semaphore 的阻塞条件是"count_ 递减后变成负数"（资源耗尽）。Mutex 的判断是二元的，Semaphore 的判断涉及算术运算。

第三个差异是所有权语义。Mutex 的 unlock 只能由 owner 调用（虽然 Cinux 没有强制检查这个约束），Semaphore 的 post 可以由任何任务调用。这意味着 Mutex 有"谁锁谁解"的语义，Semaphore 没有。这也是为什么 Mutex 和 Semaphore 应该是两个独立的原语——把 Mutex 建立在 Semaphore 之上会导致所有权语义的丢失。

## 下一步预告

本文完整拆解了 Semaphore 的实现细节——从类定义到构造函数，从 post() 的 V 操作到 wait() 的 P 操作，从 try_wait() 的非阻塞尝试到 count() 的诊断接口。我们还对比了 Mutex 和 Semaphore 的实现差异，理解了为什么它们应该是两个独立的原语。

下一篇我们将聚焦集成代码和测试：main.cpp 中的 producer-consumer demo 如何使用 Semaphore + Mutex、test_sync.cpp 的测试策略、以及那个差点让我们血压拉满的 MOV 编码 bug。
这三个主题分别回答了"怎么用"、"怎么测"和"怎么踩坑"三个问题。

## 设计决策补充

### 决策：总是递增 count_ 再取等待者

**问题**: post() 中应该先递增 count_ 还是先检查是否有等待者？
**本项目的做法**: 总是先递增 count_，然后无条件从等待队列取头部任务，最后根据取到的是否为 nullptr 决定是否 unblock。
**备选方案**: 如果等待队列非空，直接把等待者作为新 owner 唤醒而不递增 count_（计数器"转移"给唤醒者）。
**为什么不选备选方案**: 虽然计数器转移在语义上更精确（count_ 的值总是精确反映可用资源数），但实现更复杂——需要区分"有等待者"和"无等待者"两条路径，分别处理 count_ 的变化。Cinux 的实现更简洁：无条件递增 + 无条件取等待者，只有 unblock 需要判空。这个简化在功能上是等价的，因为被唤醒的任务已经完成了 count_ 递减（在 block 之前），恢复执行后直接从 block 点继续。
