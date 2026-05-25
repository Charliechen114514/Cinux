---
title: 028d-sync-safety-3 · 同步安全
---

# 028d-3 Read-through: 并发测试完整实现

## 概览

本文聚焦于 028d tag 中的三个测试文件——Host 端的 `test/unit/test_sync_concurrent.cpp`（真正的多线程压力测试）、QEMU 内核态的 `kernel/test/test_sync_concurrent.cpp`（RAII 守卫正确性测试）和生产内核的 `kernel/stress/stress_test.cpp`（抢占式多任务压力测试）。三层测试从不同角度验证同步机制的正确性，构成了完整的验证体系。

关键设计决策一览：Host 端使用 `std::thread` 做真正并发，内核态测试通过 RFLAGS 直接检查 IF 位，生产压力测试用原子计数器+yield 等待模式协调多线程。

## 架构图

```
测试层次
┌─────────────────────────────────────────────────────┐
│ Host 端 (std::thread)                               │
│  - 8 线程并发 Spinlock 压力测试                      │
│  - 4 线程并发 IrqGuard 模拟测试                      │
│  - 嵌套 InterruptGuard 模拟测试                      │
│  - 8 线程快速循环 acquire/release                    │
├─────────────────────────────────────────────────────┤
│ QEMU 内核态 (big_kernel_test)                        │
│  - InterruptGuard: IF 位保存/恢复/嵌套               │
│  - IrqGuard: 获取/释放/嵌套不同锁                    │
│  - 多 Task Spinlock 互斥验证                         │
│  - Scheduler 并发 add/remove/block/unblock           │
├─────────────────────────────────────────────────────┤
│ 生产内核 (stress_test)                               │
│  - 4 线程 x 200 PMM alloc/free                      │
│  - 4 线程 x 200 Heap alloc/free                     │
│  - 4 线程 x 1000 atomic counter increment            │
│  - boot continuation yield 等待 + 结果校验           │
└─────────────────────────────────────────────────────┘
```

## 代码精讲

### Host 端: 真正的多线程压力测试

Host 端测试的第一个挑战是 Spinlock 实现的替换——内核版本使用 x86 内联汇编（pushfq/cli），Host 端没有这些指令。解决方案是一个独立的 Host 端 Spinlock 类：

```cpp
class Spinlock {
public:
    void acquire() {
        while (locked_.exchange(true, std::memory_order_acquire)) {
        }
    }
    void release() { locked_.store(false, std::memory_order_release); }
    // ... guard() 和 irq_guard() 工厂方法 ...
private:
    std::atomic<bool> locked_{false};
};
```

`exchange(true, acquire)` 等价于内核版本的 `__atomic_test_and_set`——原子地设为 true 并返回旧值。`store(false, release)` 等价于 `__atomic_clear`。唯一缺少的是 PAUSE 指令——Host 端的多核 CPU 不需要 spin-wait hint（操作系统调度器会在时间片用完后切换线程），所以空循环就够了。

核心测试逻辑非常直接——创建多个线程，每个线程对共享计数器做 N 次递增（通过 Spinlock 保护），最后验证计数器值是否精确等于线程数乘以每线程操作数：

```cpp
TEST("concurrent: spinlock atomic counter") {
    constexpr int        NUM_THREADS = 8;
    constexpr int        ITERS       = 10000;
    Spinlock             lock;
    std::atomic<int64_t> counter{0};

    concurrent_stress(NUM_THREADS, ITERS, [&](int n) {
        for (int i = 0; i < n; i++) {
            auto g = lock.guard();
            (void)g;
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });

    ASSERT_EQ(counter.load(), static_cast<int64_t>(NUM_THREADS * ITERS));
}
```

`concurrent_stress` 是一个辅助函数，创建指定数量的线程，每个线程执行 lambda，然后 join 等待完成。8 个线程各做 10000 次递增，总计 80000——如果锁有 bug，这个数字一定不对。

值得注意的是 counter 本身也是 `std::atomic`，但在递增时用了 `fetch_add(relaxed)` 而不是 `++`。虽然 Spinlock Guard 已经保证了互斥（同一时刻只有一个线程在执行 `fetch_add`），但使用 atomic 可以防止编译器对 counter 的读写做激进优化——这是双重保险的做法。

快速循环测试验证的是 Spinlock 在高频 acquire/release 下的稳定性：

```cpp
TEST("concurrent: rapid spinlock cycling") {
    constexpr int NUM_THREADS = 8;
    constexpr int ITERS       = 50000;
    Spinlock      lock;

    concurrent_stress(NUM_THREADS, ITERS, [&](int n) {
        for (int i = 0; i < n; i++) {
            lock.acquire();
            lock.release();
        }
    });
    ASSERT_TRUE(true);
}
```

这个测试没有断言——如果没死锁就算通过。8 个线程每个做 50000 次快速锁循环，总共 400000 次操作。如果 acquire/release 有任何微妙的 bug（比如 release 没有正确地清除标志位），这个测试有很大概率会挂住。

### QEMU 内核态: RAII 守卫正确性测试

内核态测试的核心是直接检查 RFLAGS 寄存器的 IF 位。我们通过内联汇编读取 RFLAGS，然后检查第 9 位（0x200 掩码）：

```cpp
void test_basic_save_restore() {
    uint64_t flags_before;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_before));
    bool if_before = (flags_before & 0x200) != 0;

    {
        InterruptGuard guard;
        uint64_t flags_during;
        __asm__ volatile("pushfq; popq %0" : "=rm"(flags_during));
        TEST_ASSERT_FALSE((flags_during & 0x200) != 0);
    }

    uint64_t flags_after;
    __asm__ volatile("pushfq; popq %0" : "=rm"(flags_after));
    bool if_after = (flags_after & 0x200) != 0;
    TEST_ASSERT_EQ(if_before, if_after);
}
```

三步验证：先记录初始 IF 状态，然后在 Guard 内确认 IF=0，Guard 析构后确认 IF 恢复到初始值。这个测试在 QEMU 中运行时中断默认是开启的（IF=1），所以 if_before 应该是 true，if_after 也应该是 true。

嵌套测试更加精妙——它验证的是"内层 Guard 析构后 IF 不会意外变成 1"：

```cpp
void test_nested_guard() {
    // ... 记录 flags_before (IF=1) ...
    {
        InterruptGuard outer;
        // ... 确认 IF=0 ...
        {
            InterruptGuard inner;
            // ... 确认 IF=0 ...
        }
        // 关键检查：inner 析构后 IF 仍然是 0
        uint64_t f3;
        __asm__ volatile("pushfq; popq %0" : "=rm"(f3));
        TEST_ASSERT_FALSE((f3 & 0x200) != 0);
    }
    // outer 析构后 IF 恢复到 1
```

如果 InterruptGuard 使用的是 `cli` + `sti`（而不是 pushfq + popfq），内层析构时执行 sti 会打开中断——这是一个经典的嵌套 bug。我们这个测试就是专门捕获这类问题的。

多 Task Spinlock 互斥测试模拟了多个 Task 上下文：

```cpp
void test_three_tasks_mutual_exclusion() {
    // ... 创建 3 个 Task (t1, t2, t3) ...
    // ... 静态 shared_counter = 0, test_lock ...

    g_per_cpu.current = t1;
    for (int i = 0; i < 10; i++) {
        auto g = test_lock.guard();
        (void)g;
        shared_counter++;
    }

    g_per_cpu.current = t2;
    for (int i = 0; i < 10; i++) {
        auto g = test_lock.guard();
        (void)g;
        shared_counter++;
    }

    // ... t3 同理 ...

    TEST_ASSERT_EQ(shared_counter, 30);
}
```

这里通过手动切换 `g_per_cpu.current` 来模拟不同 Task 的上下文。虽然是串行执行的（不是真正的并发），但它验证了 Spinlock 在不同 Task 上下文之间的获取和释放不会因为上下文切换而混淆。

### 生产内核: 抢占式多任务压力测试

这是最接近真实场景的测试——在完整的抢占式多任务环境下并发操作 PMM 和 Heap。

测试入口 `run_concurrent_stress()` 替代了原来 kernel_main 中的 `launch_first_user()`：

```cpp
extern "C" void run_concurrent_stress() noexcept {
    kprintf("[STRESS] ===== 028d Concurrent Stress Test =====\n");

    // 重置所有共享计数器
    threads_done.store(0, std::memory_order_release);
    pmm_ops_total.store(0, std::memory_order_relaxed);
    heap_ops_total.store(0, std::memory_order_relaxed);
    shared_counter.store(0, std::memory_order_relaxed);

    Scheduler::init();

    // 创建 boot continuation Task（等待所有 stress Task 完成）
    Task* boot = TaskBuilder()
        .set_entry(boot_continuation)
        .set_name("boot")
        .build();

    // 创建 4 个 stress Task
    for (int i = 0; i < NUM_THREADS; i++) {
        Task* t = TaskBuilder()
            .set_entry(stress_thread_entry)
            .set_name("stress")
            .build();
        Scheduler::add_task(t);
    }

    Scheduler::run_first(boot);
}
```

`threads_done` 使用 `MemoryOrder::Release` 来 store——这是一个刻意的选择。release 语义保证：当 boot continuation Task 通过 `threads_done.load(MemoryOrder::Acquire)` 观察到新值时，stress Task 中在 release store 之前的所有写操作（包括 PMM/Heap 的 alloc/free 操作和 atomic counter 的递增）都对其可见。如果用 relaxed，boot continuation 可能看到 `threads_done == 4` 但 atomic counter 还是旧值——这会导致测试误报 FAIL。

每个 stress Task 的逻辑很简单——三轮操作然后退出：

```cpp
static void stress_thread_entry() {
    for (int i = 0; i < PMM_OPS; i++) {
        uint64_t p = g_pmm.alloc_page();
        if (p != 0) {
            g_pmm.free_page(p);
        }
        pmm_ops_total.fetch_add(1, std::memory_order_relaxed);
    }

    for (int i = 0; i < HEAP_OPS; i++) {
        void* b = g_heap.alloc(HEAP_BLOCK_SIZE);
        if (b != nullptr) {
            g_heap.free(b);
        }
        heap_ops_total.fetch_add(1, std::memory_order_relaxed);
    }

    for (int i = 0; i < 1000; i++) {
        shared_counter.fetch_add(1, std::memory_order_relaxed);
    }

    threads_done.fetch_add(1, std::memory_order_release);
    Scheduler::exit_current();
}
```

每轮 PMM 操作分配一个页然后立刻释放——如果 PMM 的 Spinlock 保护有 bug，两个线程可能同时认为同一个页是空闲的，导致 alloc 返回相同的物理地址，或者 free 后空闲计数不对。Heap 操作同理。atomic counter 的 1000 次递增验证的是无锁的原子操作本身——如果 fetch_add 不是真正原子的，最终值会小于 4000。

boot continuation 的等待逻辑使用了 yield + busy wait 模式：

```cpp
static void boot_continuation() {
    while (threads_done.load(std::memory_order_acquire) < NUM_THREADS) {
        Scheduler::yield();
    }
    // ... 校验结果并打印 ...
    cinux::arch::launch_first_user();
}
```

每次 yield 让出 CPU 给其他 Task 执行。当所有 stress Task 都完成后，boot continuation 校验结果，然后调用 `launch_first_user` 启动 shell——无缝衔接回正常的启动流程。

## 设计决策

### 决策：三层测试的定位划分

**问题**: 如何平衡测试的覆盖范围和可维护性？

**本项目的做法**: Host 端负责锁的正确性（真正的多核并发），内核态负责 x86 特定的行为（IF 位操作），生产压力测试负责端到端的正确性。

**备选方案**: 只做生产压力测试——在真实环境下跑一遍就够了。

**为什么不选备选方案**: 生产压力测试是"黑盒"的——它只能告诉你"通过"或"失败"，但不能精确定位 bug。如果生产测试失败了，你不知道是 Spinlock 有问题还是 Scheduler 有问题还是 atomic 有问题。分层测试让每一层只关注自己的职责，bug 定位更高效。

### 决策：yield + busy wait 通知机制

**问题**: boot continuation 如何知道所有 stress Task 都完成了？

**本项目的做法**: 原子计数器 + yield 循环。

**备选方案**: 用 Semaphore 或 Condition Variable 实现等待。

**为什么不选备选方案**: 测试代码应该尽量简单，不依赖被测试的对象。如果我们用 Semaphore 来等待 stress Task 完成，但 Semaphore 本身也使用了 Spinlock（被测试的对象），那测试就不是独立的了。原子计数器 + yield 是最简单的无依赖方案。

**如果要扩展**: 可以引入 `Completion` 或 `Latch` 抽象——本质上就是原子计数器 + yield 的封装，但语义更清晰。

## 扩展方向

- 增加 "chaos monkey" 模式：随机在 stress Task 中加入 yield，增加上下文切换频率
- 实现基于硬件性能计数器的测试——统计锁争用次数和持锁时间
- 添加 PMM 的连续分配压力测试——多线程同时 alloc_pages(4)
- 用 ThreadSanitizer (TSan) 编译 Host 端测试，自动检测数据竞争
- 研究如何用 model checker（如 TLA+ 或 CDSChecker）验证 Spinlock 的正确性

## 参考资料

- Intel SDM: Vol.2A Section 4.3 — PAUSE (Spin Wait Hint)
- Intel SDM: Vol.3A Section 6.3.2 — Maskable Hardware Interrupts (RFLAGS.IF)
- `std::atomic` memory ordering: cppreference `memory_order`
- Linux: `tools/testing/selftests/locking/` — 内核锁自测框架
- xv6-riscv: `spinlock.c` 中的 acquire/release 测试
