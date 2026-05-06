# 028d-3: 并发测试——三层验证策略

## 导语

锁加完了，但怎么确认它们真的管用？并发 bug 的可怕之处在于它们是概率性的——在测试环境里可能跑一万次都不出问题，一上线就在用户的机器上炸了。所以我们需要系统化的测试策略，从多个角度验证同步机制的正确性。

这一章我们会搭建三层测试体系：Host 端用 `std::thread` 做真正的多线程压力测试，QEMU 内核态测试验证 RAII 守卫的正确性，以及一个生产内核的压力测试——在真实的抢占式多任务和中断环境下锤炼 PMM、Heap 和原子计数器。完成这一章后，我们可以有信心地说：内核的同步保护在真实并发环境下是正确的。

前置要求：前两章的所有实现必须完成并编译通过。

## 概念精讲

### 为什么需要三层测试

你可能会问：既然有 Host 端的多线程测试，为什么还需要 QEMU 内核态测试？原因是 Host 端和内核态的环境差异非常大：

Host 端运行在 Linux（或 macOS/Windows）上，有完整的标准库和线程支持。我们可以用 `std::thread` 创建真正的并发线程，让多个线程同时操作同一个 Spinlock 保护的数据结构。这是验证锁正确性最直接的方式——如果锁有 bug，真正的并发会立刻暴露问题。但 Host 端无法模拟 x86 中断机制——没有 `pushfq`/`cli`/`popfq`，没有时钟中断打断执行流。所以 IrqGuard 和 InterruptGuard 在 Host 端只能用 stub 实现。

QEMU 内核态测试弥补了这个缺陷。它在真实的 x86 环境中运行，有真正的 IDT、时钟中断、cli/sti。我们可以直接测试 InterruptGuard 是否正确保存和恢复了 RFLAGS 的 IF 位，IrqGuard 是否在嵌套场景下正确工作。但它用的是 Cinux 自己的协作式调度器——虽然也有上下文切换，但不是真正的抢占式多线程。

生产内核压力测试则是最接近真实使用场景的——它启动 Cinux 的完整调度器，创建多个内核线程，在时钟中断驱动的抢占式多任务环境下并发操作 PMM 和 Heap。如果这个测试通过了，我们有理由相信内核在真实负载下是安全的。

### 压力测试的设计原则

一个好的并发压力测试需要满足几个条件：

第一，操作次数要足够多。单次操作暴露 bug 的概率很小，跑几千次才能累积到可观测的概率。我们的 PMM 压力测试每个线程做 200 次 alloc/free 循环，4 个线程共 800 次。

第二，共享状态要可验证。纯跑不崩溃不等于正确——我们需要检查操作计数是否精确匹配预期值。比如 4 个线程各做 200 次 PMM alloc+free，那总操作数必须精确等于 800，少一次就说明有一次操作被吞了（竞争导致丢失更新）。

第三，测试结束后要等待所有线程完成。由于是并发执行，主线程不能假设子线程立刻完成。我们用原子计数器 `threads_done` 让每个线程在退出前递增，主线程在 `while` 循环中 yield 等待，直到所有线程都完成。

## 动手实现

### Step 1: Host 端 Spinlock 压力测试

**目标**: 用 `std::thread` 验证 Spinlock 在真正的多核并发下的正确性。

**设计思路**: Host 端需要自己的 Spinlock 实现（用 `std::atomic<bool>` 的 `exchange` 和 `store` 替代 x86 内联汇编）。测试方案很简单：创建 8 个线程，每个线程对一个共享计数器做 10000 次 increment——通过 Spinlock Guard 保护。如果最终计数器值精确等于 80000，说明锁提供了正确的互斥。

**实现约束**:
- Host 端 Spinlock：`exchange(true, acquire)` 做 test-and-set，`store(false, release)` 做 clear
- Host 端 IrqGuard：简化版，只调用 acquire/release，不涉及 x86 指令
- Host 端 InterruptGuard：空操作（Host 没有中断概念）
- 辅助函数：创建 N 个线程各执行指定次数的 lambda，然后 join

**踩坑预警**: Host 端 Spinlock 的 `locked_` 必须是 `std::atomic<bool>` 而不是 `volatile bool`。`volatile` 不保证原子性，在高并发下计数器会出错。

**验证**:

```
cmake --build build --target test_sync_concurrent_host
./build/test/unit/test_sync_concurrent
```

预期输出所有测试 PASS，计数器值精确匹配。

### Step 2: QEMU 内核态 RAII 守卫测试

**目标**: 在真实 x86 环境中验证 InterruptGuard 和 IrqGuard 的正确性。

**设计思路**: 测试分为四组，从简单到复杂。

第一组：InterruptGuard 基本功能。读取 RFLAGS，创建 Guard，读取 RFLAGS 确认 IF=0，销毁 Guard，读取 RFLAGS 确认 IF 恢复。

第二组：InterruptGuard 嵌套。创建外层 Guard，确认 IF=0；创建内层 Guard，确认 IF=0；销毁内层，确认 IF 仍然是 0（不是 1！）；销毁外层，确认 IF 恢复。这一组测试至关重要——如果内层销毁后 IF 变成了 1，说明嵌套不安全，在生产中会导致中断在临界区内被意外打开。

第三组：IrqGuard 基本操作。创建 IrqGuard，确认 Spinlock 被持有且 IF=0；销毁后确认 Spinlock 可被再次获取。

第四组：多任务 Spinlock 互斥。模拟 3 个 Task（通过设置 `g_per_cpu.current`），每个 Task 对一个共享计数器做若干次 increment（通过 Spinlock guard 保护），最终验证计数器值。

**实现约束**:
- 使用 TEST_ASSERT_EQ / TEST_ASSERT_FALSE 等测试宏
- RFLAGS 的 IF 位在第 9 位（mask 0x200）
- 读取 RFLAGS 用 `pushfq; popq` 内联汇编
- 多任务测试中手动切换 `g_per_cpu.current` 来模拟不同 Task 上下文

**踩坑预警**: 第四组的"多任务"测试不是真正的并发——它只是模拟不同 Task 的上下文来测试 Spinlock 的互斥功能。真正的并发测试在生产内核压力测试中完成。

**验证**:

```
cmake --build build --target big_kernel_test
qemu-system-x86_64 -cdrom build/big_kernel_test.iso -serial stdio -display none
```

在串口输出中找到 "Sync Concurrent Tests (028d)" 部分，确认所有测试 PASS。

### Step 3: 生产内核压力测试

**目标**: 在真实的抢占式多任务环境下验证 PMM、Heap 和原子计数器的线程安全性。

**设计思路**: 这个测试替代了原来 kernel_main 最后调用的 `launch_first_user`——它先运行压力测试，测试通过后再启动 shell。

测试创建一个 "boot continuation" Task 和 N 个 "stress" Task。每个 stress Task 做三件事：对 PMM 做 alloc_page + free_page 循环（200 次），对 Heap 做 alloc + free 循环（200 次），对一个共享原子计数器做 1000 次 fetch_add。完成后递增 `threads_done` 原子计数器并调用 `Scheduler::exit_current()` 退出。

boot continuation Task 在循环中 yield，等待所有 stress Task 完成（`threads_done.load(acquire) >= NUM_THREADS`）。完成后比较实际操作数和预期值，打印 PASS/FAIL 结果。

**实现约束**:
- NUM_THREADS = 4，PMM_OPS = 200，HEAP_OPS = 200，原子计数器 OPS = 1000
- 共享计数器全部使用 `std::atomic`
- `threads_done` 使用 `MemoryOrder::Release`（store）和 `MemoryOrder::Acquire`（load）建立同步关系
- 其他计数器使用 `MemoryOrder::Relaxed`（只关心原子性）
- 测试入口是 `run_concurrent_stress()`，在 kernel_main 中替代 `launch_first_user`
- 测试完成后调用 `launch_first_user` 启动 shell

**踩坑预警**: 有两个容易忽略的细节。第一个：`threads_done` 必须用 release/acquire 语义而不是 relaxed——我们要保证 stress Task 中所有的写操作（包括 PMM/Heap 的状态修改）在 boot continuation Task 观察到 `threads_done` 增长之前对它可见。第二个：`run_concurrent_stress` 必须重新初始化 Scheduler（`Scheduler::init()`），因为在这个测试之前可能已有过调度器操作。

**验证**:

```
cmake --build build --target big_kernel
qemu-system-x86_64 -cdrom build/big_kernel.iso -serial stdio -display none
```

在串口输出中找到 "[STRESS]" 开头的行，确认：
- PMM alloc/free ops: expected=800 actual=800 PASS
- Heap alloc/free ops: expected=800 actual=800 PASS
- Atomic counter: expected=4000 actual=4000 PASS
- ALL PASSED -- launching shell

## 构建与运行

完整的构建和测试流程：

```
# Host 端并发测试
cmake --build build --target test_sync_concurrent_host
./build/test/unit/test_sync_concurrent

# QEMU 内核态测试（包含 028d 新增测试）
cmake --build build --target big_kernel_test
qemu-system-x86_64 -cdrom build/big_kernel_test.iso -serial stdio -display none

# 生产内核压力测试
cmake --build build --target big_kernel
qemu-system-x86_64 -cdrom build/big_kernel.iso -serial stdio -display none
```

## 调试技巧

**问题 1: Stress test 计数器不匹配**
如果 PMM 或 Heap 的操作计数不对，说明存在竞争——某个操作被两个线程同时执行导致状态损坏。排查方法：减少线程数到 1，确认单线程下计数正确；然后逐步增加线程数，找到出问题的临界点。同时检查对应的锁是否真的在每个操作路径上被获取。

**问题 2: Stress test 永远等不到 threads_done**
这说明某个 stress Task 没有正常退出。最可能的原因是 PMM 或 Heap 操作在持锁状态下崩溃（比如空指针解引用），导致 Task 被 kill 而没有执行到 `threads_done.fetch_add`。在 stress_thread_entry 的每个操作中加入 kprintf 来定位崩溃位置。

**问题 3: Host 端测试偶尔失败**
如果在 CI 中 Host 端测试偶发失败，可能是 `ITERS` 设得太小——并发 bug 的触发概率与操作次数相关。把 ITERS 增大一个数量级（比如从 10000 增到 100000），如果失败频率显著增加，说明确实存在竞争 bug。

## 本章小结

| 测试层 | 环境 | 验证内容 | 关键技术 |
|--------|------|----------|----------|
| Host 端 | Linux + std::thread | 真正多核并发下的 Spinlock 互斥 | std::atomic, std::thread |
| QEMU 内核态 | x86 + IDT | InterruptGuard/IrqGuard 的 RFLAGS 操作 | pushfq/popfq, IF 位检查 |
| 生产内核 | 抢占式多任务 + 中断 | PMM/Heap/atomic 在真实负载下的正确性 | Scheduler::yield, atomic release/acquire |

到这里，028d 的同步安全改造全部完成。我们有了自旋锁、RAII 守卫、三层分级加锁策略、以及从 Host 到 QEMU 到生产内核的完整测试体系。内核现在可以安全地在抢占式多任务环境下运行了。
