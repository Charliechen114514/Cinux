# 028d-1 Read-through: Spinlock 与 RAII 守卫完整实现

## 概览

本文聚焦于 `kernel/proc/sync.hpp` 和 `kernel/proc/sync.cpp` 中新增的同步基建——Spinlock 的 IrqGuard、独立的 InterruptGuard 类。这些是 028d tag 中所有其他加锁操作的基石：PMM、Heap、Scheduler 等组件的保护全部建立在这套 RAII 守卫之上。

关键设计决策一览：自旋锁使用 GCC `__atomic_test_and_set` + PAUSE 指令实现，提供两层 RAII 守卫（普通 Guard 和 IrqGuard），外加独立的 InterruptGuard。所有守卫都通过 pushfq/popfq 支持嵌套。

## 架构图

```
              Spinlock (volatile bool locked_)
             /         \
            /           \
     Guard (RAII)    IrqGuard (RAII)
     acquire/release  pushfq+cli+acquire
                      release+popfq

  InterruptGuard (独立)
  pushfq+cli / popfq
```

三个守卫的层次关系：Guard 是纯自旋锁保护，IrqGuard 是中断禁用+自旋锁的组合保护，InterruptGuard 是纯中断禁用保护。对于内核中的不同场景，选择合适的守卫——Scheduler 操作需要 IrqGuard（中断上下文也会访问运行队列），PMM/Heap 操作只需要 Guard（不会在中断中被调用），Keyboard 的 poll 只需要 InterruptGuard（IRQ handler 不获取锁）。

## 代码精讲

### Spinlock 核心实现

我们先来看 Spinlock 的 acquire 和 release，这是整个同步体系的最底层。

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

`acquire` 的实现是一个紧凑的自旋循环。`__atomic_test_and_set` 在一次不可分割的操作中完成"读取当前值 + 设为新值"——如果 `locked_` 原来是 false，它被设为 true 并返回 false（表示成功获取锁），循环结束；如果原来是 true，它保持 true 并返回 true（表示锁被持有），循环继续。`__ATOMIC_ACQUIRE` 内存序保证 acquire 之后的读操作不会被重排到 acquire 之前——这意味着获取锁之后，当前线程看到的临界区数据一定是最新的。

循环体中的 `__asm__ volatile("pause")` 对应 x86 的 PAUSE 指令。这个指令的自旋等待优化我们在 Hands-on 篇已经详细讨论过，这里只补充一点：`volatile` 关键字确保编译器不会把这条内联汇编优化掉——虽然编译器通常不会优化掉空的内联汇编，但加上 volatile 是零成本的保险措施。

`release` 就简单多了——`__atomic_clear` 原子地将 `locked_` 设为 false。`__ATOMIC_RELEASE` 语义保证 release 之前的所有写操作（即临界区内的所有修改）在 release 之后对其他线程可见。这样，当另一个线程的 acquire 成功返回时，它一定能看到前一个持锁者做的所有修改。

### Spinlock::Guard——纯锁 RAII 守卫

```cpp
class Guard {
public:
    explicit Guard(Spinlock* lock) : lock_(lock) { lock_->acquire(); }
    ~Guard() { lock_->release(); }
    Guard(const Guard&)            = delete;
    Guard& operator=(const Guard&) = delete;
private:
    Spinlock* lock_;
};
```

Guard 的设计非常直接——构造时获取锁，析构时释放锁。`explicit` 防止隐式转换，`delete` 拷贝操作防止 Guard 被意外复制（复制一个 Guard 意味着两个对象都认为自己持有锁，析构时锁会被释放两次）。

Guard 通过 `guard()` 工厂方法创建，返回值标记 `[[nodiscard]]`：

```cpp
[[nodiscard]] auto guard() { return Guard(this); }
```

`[[nodiscard]]` 是一个很容易被忽略但极其重要的属性。如果你写了 `lock.guard()` 但没把返回值赋给变量，Guard 作为临时对象在这一行结束时就被析构了——锁立刻释放，临界区完全没被保护。加上 `[[nodiscard]]` 后编译器会对这种写法发出警告，帮你避免这个 bug。

### Spinlock::IrqGuard——中断安全守卫

IrqGuard 是整个同步体系中最关键的守卫，它解决了"中断处理程序与普通代码竞争同一个锁"的死锁问题。

```cpp
Spinlock::IrqGuard::IrqGuard(Spinlock* lock) : lock_(lock) {
    __asm__ volatile("pushfq; popq %0; cli" : "=rm"(saved_flags_));
    lock_->acquire();
}

Spinlock::IrqGuard::~IrqGuard() {
    lock_->release();
    __asm__ volatile("pushq %0; popfq" : : "rm"(saved_flags_));
}
```

构造函数做了三件事，顺序很重要：先把当前的 RFLAGS 寄存器压栈弹到 `saved_flags_` 中（保存了 IF 位在内的所有标志），然后执行 `cli` 清除 IF 位禁用中断，最后才获取自旋锁。如果先获取锁再禁中断，那么在锁被获取之后、cli 之前的那个窗口期，中断仍然可以到来——而中断处理程序可能尝试获取同一个锁，导致死锁。

析构函数的顺序也很讲究：先释放锁，再恢复 RFLAGS。如果先恢复 RFLAGS（可能打开中断）再释放锁，那么中断处理程序会在锁还被持有时尝试获取它——又是死锁。虽然 "释放锁→恢复 flags" 之间也有一个极小的窗口中断可以到来，但由于锁已经释放了，中断处理程序可以成功获取锁，不会死锁。

`pushfq; popq %0` 这条内联汇编把 64 位的 RFLAGS 存入 `saved_flags_`（uint64_t），而 `pushq %0; popfq` 反过来恢复。这里用的是 `"=rm"` 和 `"rm"` 约束——允许编译器选择内存或寄存器作为操作数。在优化后的代码中，编译器通常会选择寄存器，效率更高。

我们特别要注意嵌套场景的正确性。假设外层 IrqGuard 已经禁用了中断（IF=0），内层 IrqGuard 构造时 pushfq 保存的 RFLAGS 中 IF=0。内层析构时 popfq 恢复 IF=0——中断仍然被禁用，不会因为内层析构而被意外打开。只有最外层的 IrqGuard 析构时 popfq 才会恢复到最初的 IF=1 状态。这种天然的嵌套安全性是 pushfq/popfq 方案的最大优点。

### InterruptGuard——独立的中断禁用守卫

```cpp
class InterruptGuard {
public:
    InterruptGuard();
    ~InterruptGuard();
    InterruptGuard(const InterruptGuard&)            = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;
private:
    uint64_t saved_flags_;
};
```

```cpp
InterruptGuard::InterruptGuard() {
    __asm__ volatile("pushfq; popq %0; cli" : "=rm"(saved_flags_));
}

InterruptGuard::~InterruptGuard() {
    __asm__ volatile("pushq %0; popfq" : : "rm"(saved_flags_));
}
```

InterruptGuard 是 IrqGuard 的"去掉 Spinlock"版本。它的实现和 IrqGuard 的中断控制部分完全一样，只是不涉及锁的获取和释放。

什么时候用 InterruptGuard 而不是 IrqGuard？答案在键盘驱动的 `poll()` 方法中。键盘的 IRQ1 handler 直接写入 ring buffer，不获取任何锁——它在中断上下文中运行，不会被另一个中断打断（8259 PIC 不可重入），所以 IRQ handler 内部不需要锁保护。而用户线程的 `poll()` 读取 ring buffer 时，只需要确保不被 IRQ1 打断就够了——这正是 InterruptGuard 提供的保护。

### Spinlock 的数据成员

```cpp
private:
    volatile bool locked_ = false;
```

这里用了 `volatile bool` 而不是 `std::atomic<bool>`。看起来有点奇怪——我们不是刚说了 volatile 不保证原子性吗？关键在于，`locked_` 的所有访问都通过 `__atomic_test_and_set` 和 `__atomic_clear` 这两个编译器内建函数进行的，它们本身会生成正确的原子指令。`volatile` 在这里的作用是防止编译器对 `locked_` 做激进的优化（比如把它缓存在寄存器中），确保每次 `__atomic_*` 操作都真正访问内存。

不过说实话，`__atomic_*` 内建函数本身就会忽略 volatile 限定符去做正确的原子操作，所以这里的 volatile 更多是一种防御性编程的习惯。如果以后迁移到 `std::atomic<bool>`，逻辑会更清晰。

## 设计决策

### 决策：自旋锁 vs 禁中断 vs 两者组合

**问题**: 保护临界区的方式有三种——纯自旋锁、纯禁中断、自旋锁+禁中断。如何选择？

**本项目的做法**: 三种都提供，通过不同的 RAII 守卫区分使用场景。Scheduler 用 IrqGuard（自旋锁+禁中断），PMM/Heap 用 Guard（纯自旋锁），Keyboard 用 InterruptGuard（纯禁中断）。

**备选方案**: 全部统一用 IrqGuard——不管什么场景都禁中断+自旋锁。

**为什么不选备选方案**: 禁中断有代价——它延迟了所有中断的处理，包括时钟中断和键盘中断。如果 PMM 的 alloc_page 操作耗时较长（位图扫描可能需要几百个周期），禁中断会导致系统响应性下降。实际上 PMM 的 alloc_page 不会被中断处理程序直接调用（中断处理程序通常不做内存分配），所以纯自旋锁就够了。

**如果要扩展/改进，应该怎么做**: 如果将来支持 SMP（多核），纯自旋锁就不够了——单核上自旋锁保护的临界区不会被另一个核的线程打扰，但多核上需要 IrqGuard 或者更强的原语（比如 Linux 的 `spinlock_irqsave`）。另外可以考虑将 `volatile bool` 替换为 `std::atomic<bool>`，语义更清晰。

### 决策：pushfq/popfq vs 手动维护 IF 状态

**问题**: IrqGuard 和 InterruptGuard 的嵌套支持有两种实现方式——用 pushfq/popfq 保存完整 RFLAGS，或用一个计数器记录嵌套层数。

**本项目的做法**: 用 pushfq/popfq 保存完整 RFLAGS。

**备选方案**: 用一个 `int nest_count_` 记录嵌套深度，只在最外层（nest_count_ 从 1 变 0 时）恢复 IF。

**为什么不选备选方案**: pushfq/popfq 方案更简单且更正确——它保存的是精确的中断状态，而不是一个推算的布尔值。而且它没有额外的成员变量开销，saved_flags_ 就是一个 uint64_t。Linux 内核的 `spinlock_irqsave` 也使用类似的 flags 保存方案。

## 扩展方向

- 尝试实现一个 Ticket Lock（FIFO 公平自旋锁），对比简单 test-and-set 自旋锁在公平性上的差异
- 为 Spinlock 添加持有者检测（记录哪个 Task 持有锁），在 release 时检查调用者是否是持有者
- 实现 `preempt_disable`/`preempt_enable` 计数器，结合 InterruptGuard 实现内核抢占控制
- 考虑用 `std::atomic<bool>` 替换 `volatile bool`，统一原子操作的写法
- 研究 MCS Lock 的实现原理，理解为什么 Linux 在高争用场景下使用排队锁

## 参考资料

- Intel SDM: Vol.2A Section 4.3 — PAUSE 指令 (Spin Wait Hint)
- Intel SDM: Vol.3A Section 8.1 — LOCK 前缀与原子操作
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives)
- Linux: `spinlock_irqsave` / `spinlock_irqrestore` 实现
- xv6-riscv: `spinlock.c` 中的 `push_off`/`pop_off` 中断嵌套机制
