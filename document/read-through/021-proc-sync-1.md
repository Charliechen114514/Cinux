# 021-1 Spinlock + Mutex 完整代码精讲

> 前置：[020 抢占式调度](020-proc-scheduler-3.md)
> 聚焦：sync.hpp Spinlock/Mutex 类定义 + sync.cpp Spinlock/Mutex 实现 + process.hpp wait_next 字段

## 概览

本文是 tag 021 三篇 Read-through 的第一篇，聚焦 Spinlock 和 Mutex 的完整实现。我们先看 `process.hpp` 中新增的 `wait_next` 字段，然后逐段拆解 `sync.hpp` 中 Spinlock 和 Mutex 的类定义，最后深入 `sync.cpp` 中每个方法的实现细节。

## Task::wait_next — 侵入式等待队列的锚点

整个 tag 021 的等待队列机制建立在 Task 结构体的一个新字段之上：

```cpp
/** Intrusive link for wait-queue linked lists (Mutex / Semaphore). */
Task* wait_next;
```

这一个指针就是我们侵入式单向链表的全部基础设施。它的含义很直白：当任务在某个 Mutex 或 Semaphore 上等待时，`wait_next` 指向同一等待队列中的下一个任务；不在任何等待队列中时保持 nullptr。

你可能注意到这个设计限制了一个任务同时只能在一个等待队列上——因为只有一个 next 指针。这在教学 OS 的场景里完全够用，生产级内核（比如 Linux）会用独立的 `wait_queue_entry` 结构来避免这个限制，但代价是需要额外的内存分配。

`wait_next` 在 Task 创建时通过 `knew` 的零初始化自动被设为 nullptr，不需要在 TaskBuilder 中显式处理。

## Spinlock — 从内联到 .cpp 的重构

Spinlock 在 tag 020 中是纯头文件内联实现的，现在我们把它移到 sync.cpp。类定义本身没变：

```cpp
class Spinlock {
public:
    Spinlock() = default;
    void acquire();
    void release();
    [[nodiscard]] auto guard() { return Guard(this); }
private:
    volatile bool locked_ = false;
    // Guard 嵌套类...
};
```

`volatile bool locked_` 是 Spinlock 的唯一状态——false 表示未锁，true 表示已锁。`volatile` 告诉编译器不要缓存这个变量的值（每次访问都要从内存读），虽然在原子操作语境下 `__atomic_test_and_set` 本身已经保证了这一点，加上 volatile 是双重保险。

实现部分和之前完全一致：

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

`__atomic_test_and_set` 把 `locked_` 设为 true 并返回之前的值。如果之前是 false（未锁），我们就成功获取了锁，循环退出；如果之前是 true（已锁），返回值非零，继续自旋。`__ATOMIC_ACQUIRE` 内存序确保后续的读操作不会被重排到获取锁之前。

循环体里的 `pause` 指令是 Intel 给 spin-wait 循环的专用提示，SDM Vol. 2A 中明确说明它避免了 P4/Xeon 处理器上 spin-wait 结束时的流水线 flush 惩罚。在 QEMU 中它相当于 NOP，但养成使用习惯很重要。

释放时 `__atomic_clear` 把 `locked_` 设为 false，配合 `__ATOMIC_RELEASE` 内存序确保临界区内的所有写操作对其他 CPU 可见之后才释放锁。

## Mutex 类定义

Mutex 的接口比 Spinlock 丰富得多：

```cpp
class Mutex {
public:
    Mutex() = default;
    void lock();
    void unlock();
    bool try_lock();
    [[nodiscard]] auto guard() { return Guard(this); }
private:
    Spinlock spin_;
    Task*    owner_     = nullptr;
    Task*    wait_head_ = nullptr;
    void enqueue_waiter(Task* task);
    Task* dequeue_waiter();
    // Guard 嵌套类...
};
```

三个成员变量的分工很清晰：`spin_` 是内部自旋锁，保护 `owner_` 和 `wait_head_` 的修改；`owner_` 指向当前持有者任务（nullptr 表示无人持有）；`wait_head_` 指向等待队列的头部。

## 等待队列操作

Mutex 的 enqueue 和 dequeue 是侵入式链表的标准操作，尾插头删保证 FIFO 顺序：

```cpp
void Mutex::enqueue_waiter(Task* task) {
    task->wait_next = nullptr;
    if (wait_head_ == nullptr) {
        wait_head_ = task;
        return;
    }
    Task* tail = wait_head_;
    while (tail->wait_next != nullptr) {
        tail = tail->wait_next;
    }
    tail->wait_next = task;
}
```

入队时先把新节点的 `wait_next` 清零（重要！这个字段可能残留之前的值），然后遍历到链表尾部挂上去。如果链表为空就直接让 head 指向新节点。

```cpp
Task* Mutex::dequeue_waiter() {
    if (wait_head_ == nullptr) {
        return nullptr;
    }
    Task* task      = wait_head_;
    wait_head_      = task->wait_next;
    task->wait_next = nullptr;
    return task;
}
```

出队时取 head 指向的节点，head 后移，清零被取出节点的 `wait_next`。

## lock() — 五步阻塞获取

这是 Mutex 最核心的方法：

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

第一步拿 Spinlock，保护 owner 检查和等待队列操作。第二步如果 owner 为空直接占据并返回——这是无竞争的快速路径。第三步进入慢速路径：入队。第四步释放 Spinlock——这在 block 之前做，因为 block 会触发 schedule() 切换上下文，如果此时 Spinlock 还被持着就死锁了。第五步调用 block 让出 CPU。

`g_per_cpu.current` 是当前正在执行的任务指针，在 `cinux::proc` 命名空间下的全局 PerCPU 变量。

## unlock() — 所有权转移

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

unlock 的关键在于"所有权转移"：如果有等待者，owner 直接转给队列头的任务，而不是先清零再让等待者重新竞争。这保证了 FIFO 公平性——等待最久的任务一定先获得锁。

## try_lock() 和 Guard

```cpp
bool Mutex::try_lock() {
    spin_.acquire();
    if (owner_ != nullptr) {
        spin_.release();
        return false;
    }
    owner_ = g_per_cpu.current;
    spin_.release();
    return true;
}
```

try_lock 是 lock 的无阻塞版本——只走快速路径，慢速路径直接返回 false。

```cpp
class Guard {
public:
    explicit Guard(Mutex* mtx) : mtx_(mtx) { mtx_->lock(); }
    ~Guard() { mtx_->unlock(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
private:
    Mutex* mtx_;
};
```

Guard 通过 RAII 管理锁的生命周期。禁用拷贝防止误用——拷贝一个 Guard 意味着两个对象析构时都调用 unlock，这会导致未定义行为。

## 设计决策

### 决策：侵入式链表 vs 独立分配的等待队列节点

**问题**: 等待队列如何管理等待者？ **本项目的做法**: 在 Task 结构体中嵌入 `wait_next` 指针，零额外分配。 **备选方案**: 分配独立的 `WaitQueueNode` 结构（Linux 的 `mutex_waiter`，PintOS 的 `list_elem`）。 **为什么不选备选方案**: 单核教学环境不需要任务同时在多个队列上等待；独立分配增加内存管理复杂度和失败路径。 **如果要扩展**: 为 SMP 支持时，可以引入 `wait_queue_entry` 结构允许任务同时在多个队列上等待，如 Linux 的 `DEFINE_WAIT` 模式。

## 扩展方向

- **Priority inheritance**: 当高优先级任务在低优先级任务持有的 Mutex 上阻塞时，临时提升持有者优先级防止优先级反转（难度: 2 星）
- **Recursive mutex**: 在 Mutex 中增加递归计数，允许同一任务多次 lock（难度: 1 星）
- **Spinlock 中断安全**: 在 acquire 时禁用中断（类似 xv6 的 push_off/pop_off），防止中断处理程序和线程竞争同一把锁（难度: 2 星）

## 参考资料

- Intel SDM: Vol. 2A "PAUSE -- Spin Wait Hint" -- PAUSE 指令避免 P4/Xeon 流水线 flush
- Intel SDM: Vol. 3A Section 8.1 "Locked Atomic Operations" -- LOCK 前缀和原子操作语义
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- Spinlock/Mutex/Semaphore 总览
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) -- xv6 的阻塞式锁实现（sleep/wakeup 机制）
- Linux: [kernel/locking/mutex.c](https://github.com/torvalds/linux/blob/master/kernel/locking/mutex.c) -- 生产级 mutex 实现（乐观自旋 + 等待队列）
