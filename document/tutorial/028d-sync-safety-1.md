# 从竞态条件到自旋锁——给内核装上互斥的铠甲

> 标签：自旋锁、同步、RAII、原子操作、内存序
> 前置：[028c 文件系统 cwd/stat](./028c-fs-cwd-stat-1.md)

## 前言

说实话，我们之前写的内核一直有一个定时炸弹在滴答作响——多个线程同时操作同一个数据结构时会发生什么？在单核协作式调度的年代，这个问题并不紧迫，因为线程只有在主动 yield 的时候才会切换，只要你不在操作一半的时候 yield，数据就是安全的。但当我们引入了时钟中断驱动的抢占式调度之后，一切都变了：任何时刻一个 tick 中断都可能打断你正在修改链表的代码，切换到另一个也在修改同一条链表的线程。结局呢？链表指针被踩烂，内核直接三振出局。

笔者在这里血压拉满的时候，通常是在调试那些"跑十次崩一次"的 bug——数据竞争最恶心的地方在于它是概率性的，你加个 kprintf 它就不崩了（因为 timing 变了），去掉 kprintf 它又开始崩了。这种 Heisenbug 真能把人逼疯。

所以这一章，我们要彻底解决这个问题。核心工具是自旋锁（Spinlock）——最简单、最底层的互斥原语，配合 RAII 守卫让锁的生命周期自动管理，再加上中断安全守卫防止中断处理程序和普通代码之间的竞争。

## 环境说明

本次改动基于 tag 028d_sync_safety，在 x86_64 QEMU 环境下测试。工具链是 GCC 14 + CMake，内核语言是禁用了标准库和异常的 C++。我们的内核目前是单核设计，但单核上的中断驱动并发同样需要正确的同步——甚至比多核更棘手，因为中断可能在任何指令之间到来。

## 为什么 volatile 不够用

在动手写 Spinlock 之前，我们先搞清楚一个常见的误解。很多教程会告诉你用 `volatile` 来保护共享变量，这在单片机裸机编程里可能勉强管用，但在有编译器优化和多执行上下文的内核中完全不够。

`volatile` 做的唯一一件事是告诉编译器"每次读写这个变量都要真的访问内存，不要缓存在寄存器里"。它不保证原子性——`++counter` 在 C++ 里是 read-modify-write 三步操作，`volatile` 无法保证这三步不被打断。它更不建立 happens-before 关系——线程 A 写了一个 volatile 变量，编译器完全可能把之后的读操作重排到这个写之前。

我们需要的是真正的原子操作和正确的内存序语义。好消息是，GCC 的 `__atomic_*` 内建函数和 C++11 的 `std::atomic` 已经为我们准备好了这些工具。

## 从 test-and-set 到自旋锁

自旋锁的本质是一个布尔标志位加上一个原子操作。获取锁的时候，我们原子地"检查当前值并设为 true"——这就是所谓的 test-and-set。如果原来就是 true（锁被持有），我们就反复重试，也就是"自旋"；如果原来是 false，设置成功，我们获得了锁。

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

`__atomic_test_and_set` 是 GCC 提供的原子操作内建函数，在 x86 上会被编译为 `LOCK BTS`（Bit Test and Set）指令或者等价的 `XCHG` 指令——根据 Intel SDM Vol.3A Section 9.1 的说明，`XCHG` 指令隐含 LOCK 语义，不需要显式的 LOCK 前缀。这个函数原子地将 `locked_` 设为 true，并返回之前的旧值。如果旧值是 false，说明我们成功抢到了锁，循环结束；如果旧值是 true，说明别人还持有着锁，我们继续自旋。

`__ATOMIC_ACQUIRE` 是内存序参数，它的含义是"在这个操作之后的读操作不能被重排到这个操作之前"。为什么要这个保证？想象一下：获取锁之后我们要读临界区的数据，如果编译器把读操作重排到了获取锁之前，我们读到的可能是过时的数据——因为还没获得锁，其他线程可能正在修改数据。

release 用的 `__ATOMIC_RELEASE` 则反过来——"在这个操作之前的写操作不能被重排到这个操作之后"。这保证了我们在临界区内的所有修改，在锁释放之前都已经完成。当另一个线程获取到同一个锁时（acquire 语义），它一定能看到我们之前做的所有修改。acquire-release 配对，就这样建立了线程之间的 happens-before 关系。

循环体中的 `pause` 指令（Intel SDM Vol.2A Section 4.3 "PAUSE—Spin Wait Hint"）是给处理器的一个性能提示。Intel 的文档明确指出，在 P4 和 Xeon 处理器上，没有 PAUSE 的自旋循环会触发内存顺序违规检测，导致流水线被刷新，性能损失严重。在现代 Intel 处理器上 PAUSE 主要作为 NOP 操作，但加上它是零成本的正确做法。

## RAII 守卫：让锁自己管自己

有了 acquire/release 之后，下一步是让锁的获取和释放与 C++ 的作用域绑定。手动调用 acquire/release 是 bug 的温床——函数有多个返回路径时，很容易忘记某个分支没释放锁。C++ 的 RAII 机制完美解决这个问题：

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

构造时获取锁，析构时释放锁——无论控制流怎么离开作用域（正常返回、提前 return、break），锁都一定会被释放。`delete` 拷贝操作防止 Guard 被意外复制（复制的后果是两个对象都认为自己持有锁，析构时锁被释放两次）。

工厂方法标记了 `[[nodiscard]]`：

```cpp
[[nodiscard]] auto guard() { return Guard(this); }
```

这个属性看似不起眼，实际上救了我好几次。如果你写了 `lock.guard()` 但没把返回值赋给变量，Guard 是个临时对象，在这一行结束时就被析构——锁立刻释放，临界区完全没被保护。加上 `[[nodiscard]]` 后编译器会对这种写法发出警告，防止你犯这个低级错误。

## 中断安全：IrqGuard 的设计

单纯的自旋锁在一个场景下会死锁——中断处理程序和普通代码竞争同一个锁。考虑这个场景：线程 A 正在操作调度器的运行队列，持有 Spinlock，此时一个时钟中断到来。中断处理程序调用 `Scheduler::tick()`，tick 又调用 `pick_next()` 试图获取同一个 Spinlock——但锁已经被当前线程持有了。而当前线程被中断打断了，无法继续执行 release。死锁，完美死锁。

解决方案是在获取自旋锁之前先禁用中断：

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

构造函数做三件事：保存 RFLAGS（包含 IF 位），执行 `cli` 禁用中断，然后获取自旋锁。析构函数反过来：先释放锁，再恢复 RFLAGS。先释放锁再恢复 flags 的顺序很重要——如果先恢复 flags（可能打开中断）再释放锁，中断处理程序会在锁还被持有时尝试获取它，又是死锁。

嵌套安全性由 pushfq/popfq 天然保证。如果外层 IrqGuard 已经禁用了中断（IF=0），内层 pushfq 保存的 RFLAGS 中 IF=0。内层析构时 popfq 恢复 IF=0——中断不会因为内层析构而被意外打开。只有最外层析构时才会恢复到最初的 IF=1。

## xv6 和 Linux 怎么做的

对比一下其他操作系统的实现有助于理解设计选择。

xv6-riscv 的 spinlock.c 中有完全相同的模式——`push_off` 在获取锁之前禁用中断，`pop_off` 在释放锁之后恢复中断状态。xv6 用 `noff` 计数器处理嵌套（每 push_off 递增，每 pop_off 递减，只在 0 变 1 和 1 变 0 时操作中断），而 Cinux 用 pushfq/popfq 保存完整 RFLAGS。两种方案等价，但 pushfq/popfq 不需要额外的计数器成员变量。xv6 额外追踪了锁的持有者（`cpu` 字段），在 release 时检查是否是获取锁的那个 CPU 在释放——这是一个调试辅助功能，Cinux 目前没有实现。

Linux 内核在 x86_64 上使用 ticket lock 而非简单 test-and-set。ticket lock 的原理像面包店的排队机——每个获取锁的线程拿一个递增的号码，锁释放时叫下一个号码。这保证了 FIFO 公平性，避免了某些线程饿死。Linux 的 `spinlock_irqsave` 宏和我们 IrqGuard 的功能一样——保存中断状态、禁用中断、获取自旋锁。Linux 用 `local_irq_save(flags)` + `spin_lock(lock)` 两步实现，Cinux 把它们合并到一个 RAII 对象中。Linux 还区分了 `spinlock_irqsave`（保存并禁中断）和 `spinlock_irq`（只禁中断不保存），Cinux 目前只提供了一种 IrqGuard。

我们选择 test-and-set 而非 ticket lock 的原因是简单——单核环境下公平性不是问题（同一时刻只有一个线程在自旋），test-and-set 的代码量只有 ticket lock 的一半。如果将来支持 SMP 多核，ticket lock 是必须要上的——多核环境下 test-and-set 自旋锁的公平性问题会导致严重的性能退化（cache line bouncing）。

## 收尾

验证方式很直接——在后续章节中我们会用 Host 端的 `std::thread` 压力测试和 QEMU 内核态测试来验证 Spinlock 的正确性。这里我们先把基建搭好，下一章开始给具体的组件加锁。

到这里，我们完成了同步基建的第一块拼图——自旋锁和 RAII 守卫。下一章我们会把锁加到 PMM、Heap、Scheduler 等所有共享数据结构上，真正让内核在抢占式多任务下安全运行。

## 参考资料

- Intel SDM: Vol.2A Section 4.3 — PAUSE (Spin Wait Hint)，PAUSE 指令提示处理器处于自旋等待循环
- Intel SDM: Vol.3A Section 9.1 — Locked Atomic Operations，LOCK 前缀与原子操作语义
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) — 自旋锁、Mutex、Semaphore 分类
- xv6-riscv: [spinlock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c) — push_off/pop_off 中断嵌套机制
- Linux: [spinlock.h](https://github.com/torvalds/linux/blob/master/include/linux/spinlock.h) — spinlock_irqsave 宏实现
