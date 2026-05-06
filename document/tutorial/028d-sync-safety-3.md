# 验证并发安全——三层测试体系的设计与实践

> 标签：并发测试、压力测试、std::thread、QEMU、原子操作
> 前置：[028d-2 全面加锁](./028d-sync-safety-2.md)

## 前言

锁加完了，接下来最重要的问题是——怎么确认它们真的管用？并发 bug 的可怕之处在于它们是概率性的。在测试环境跑一万次都不出问题，上线第一天就崩了——这种事情在内核开发中一点也不罕见。所以我们需要系统化的测试策略，从多个角度验证同步机制的正确性。

这一章我们会搭建三层测试体系。第一层在 Host 端用 `std::thread` 做真正的多核并发压力测试——如果 Spinlock 有 bug，8 个线程同时锤它一定会暴露。第二层在 QEMU 内核态验证 RAII 守卫的 x86 特定行为——InterruptGuard 是否正确保存和恢复了 RFLAGS 的 IF 位，嵌套场景是否安全。第三层在生产内核中做端到端压力测试——4 个内核线程在真实的抢占式多任务和时钟中断环境下并发操作 PMM 和 Heap。

说实话，写测试比写锁本身更考验功力。锁只有"对"和"错"两种状态，但测试需要回答的问题是"怎么证明它是对的"——而且是在无法穷举所有执行顺序的情况下。

## 环境说明

Host 端测试在 Linux 上用 `std::thread` 运行，编译器是 GCC 14。QEMU 内核态测试和压力测试在 x86_64 QEMU 中运行，使用 Cinux 自己的调度器和中断系统。Host 端的 Spinlock 用 `std::atomic<bool>` 替代 x86 内联汇编（因为 Host 没有 pushfq/cli），IrqGuard 和 InterruptGuard 在 Host 端退化为纯 Spinlock 操作和空操作。

## 第一层：Host 端多线程压力测试

Host 端测试的核心优势是真正的多核并发。`std::thread` 创建的线程由操作系统调度到不同的 CPU 核心上运行——这意味着两个线程真的可能同时执行 Spinlock 的 acquire，测试的是硬件级别的原子操作正确性。

核心测试逻辑很直接——创建多个线程，每个线程对一个共享计数器做 N 次递增（通过 Spinlock Guard 保护），最后验证计数器值是否精确等于线程数乘以每线程操作数。如果锁有 bug，两个线程同时进入了临界区，计数器的值一定不等于预期。

我们跑了四组测试。第一组用 8 个线程各做 10000 次 increment，验证纯 Spinlock Guard 的互斥正确性。第二组用 4 个线程测试 IrqGuard（Host 端模拟为纯 Spinlock）的互斥。第三组测试嵌套 InterruptGuard 的正确性——内层析构后中断不应该被意外恢复。第四组用 8 个线程做 50000 次快速 acquire/release 循环，验证 Spinlock 在高频操作下不会死锁。

第四组测试很有意思——它没有断言，如果没死锁就算通过。8 个线程每个做 50000 次操作，总共 400000 次 acquire/release。如果 release 没有正确地清除标志位，或者 acquire 的原子操作有微妙的问题，这个测试有很高概率会挂住。这就像把一辆车开到极限速度——平时开 60 码看不出问题，跑 200 码才知道引擎到底稳不稳。

## 第二层：QEMU 内核态 RAII 守卫测试

Host 端测试无法覆盖的一块是 x86 中断行为——pushfq/cli/popfq 的正确性。这部分在 QEMU 内核态测试中验证，因为 QEMU 模拟了完整的 x86 环境，包括 RFLAGS 寄存器和 IF 位。

测试方法很直接——用 `pushfq; popq` 内联汇编读取 RFLAGS，检查第 9 位（0x200 掩码）是否为 0。我们做了两组基本测试和一组嵌套测试。

基本测试验证 InterruptGuard 的生命周期：创建之前 IF=1（中断开启），创建之后 IF=0（中断关闭），销毁之后 IF=1（恢复原状）。

嵌套测试是重点中的重点——它验证的是"内层 Guard 析构后 IF 不会意外变成 1"。如果 InterruptGuard 实现有 bug（比如用 cli+sti 而不是 pushfq+popfq），内层析构时 sti 会打开中断，破坏外层的保护。这个测试确保了我们的 pushfq/popfq 方案在嵌套场景下是安全的。

另外还有 IrqGuard 的测试——验证获取时 Spinlock 被持有且 IF=0，销毁后锁被释放且 IF 恢复。以及多个不同 Spinlock 的 IrqGuard 嵌套——验证嵌套不会搞乱各自保存的 flags。

## 第三层：生产内核压力测试

这是最接近真实场景的测试——在 Cinux 自己的抢占式多任务环境下运行。测试创建 4 个内核线程，每个线程做 200 次 PMM alloc+free、200 次 Heap alloc+free、1000 次 atomic counter increment。还有一个 "boot continuation" 线程负责等待所有 stress 线程完成，校验结果，然后启动 shell。

这里有一个值得说一说的设计选择——如何等待所有 stress 线程完成。最直觉的做法是用 Semaphore 或 Condition Variable，但我们的 Semaphore 内部也用了 Spinlock——用被测对象来做测试基础设施，这不是好的测试设计。所以我们用了最简单的方案：原子计数器 + yield 循环。

每个 stress 线程完成后递增 `threads_done` 原子计数器（使用 `MemoryOrder::Release`），boot continuation 在循环中 yield 并检查计数器（使用 `MemoryOrder::Acquire`）。release-acquire 配对建立了 happens-before 关系——当 boot continuation 观察到 threads_done == 4 时，所有 stress 线程的操作（包括 PMM/Heap 的状态修改和 pmm_ops_total 等计数器的递增）对其完全可见。

为什么用 release/acquire 而不是 relaxed？如果 threads_done 用 relaxed，boot continuation 可能观察到 threads_done == 4 但 pmm_ops_total 还是旧值——因为 relaxed 不保证其他写操作可见。这会导致测试误报 FAIL。release-acquire 的开销几乎为零（x86 上 acquire 是免费的，因为所有 load 都有 acquire 语义），但正确性保证强得多。需要注意的是，Cinux 内核直接使用 GCC 的 `__atomic_*` 内建函数。

结果校验也很简单——检查 pmm_ops_total 是否等于 4*200=800，heap_ops_total 是否等于 800，shared_counter 是否等于 4*1000=4000。如果任何一个不匹配，说明存在竞争——某个操作被并发踩踏导致状态损坏。

## 其他操作系统怎么测试同步机制

Linux 内核有专门的锁测试框架，在 `tools/testing/selftests/locking/` 下。它用内核模块创建多个内核线程并发操作各种锁，然后检查状态一致性。Linux 还支持 ThreadSanitizer（TSan）——一种编译时插桩工具，可以在运行时检测数据竞争。当 TSan 检测到两个线程在没有同步的情况下访问同一个内存位置且至少一个是写操作时，它会报告一个 data race。Cinux 的 Host 端测试也可以用 TSan 编译（`-fsanitize=thread`），自动检测 Spinlock 保护遗漏的地方。

xv6 的测试方式更简洁——它的 `spinlock.c` 中有一个 `holding()` 函数检查锁是否被当前 CPU 持有。在 acquire 开头调用 `holding()` 并断言失败——如果当前 CPU 已经持有了这个锁，说明有递归获取的 bug，直接 panic。这是一种运行时断言而非独立的测试，但非常有效。

PintOS 的同步测试集中在 `threads/` 目录下，用 `timer_sleep` 和信号量构建各种场景。PintOS 的测试框架有一个很好的设计——每个测试用例在独立的线程中运行，超时后自动 kill 并报告 FAIL。这解决了"测试死锁导致整个测试套件卡住"的问题。

## 收尾

三层测试体系从不同角度验证了同步机制的正确性：Host 端验证锁本身的互斥性，内核态验证 x86 中断操作的正确性，生产压力测试验证端到端的并发安全。如果这三层都通过了，我们可以有信心地说内核的同步保护在真实负载下是可靠的。

完结撒花。到这里，028d 的同步安全改造全部完成。我们有了自旋锁、RAII 守卫、三层分级加锁策略、以及完整的三层测试体系。内核现在可以在抢占式多任务和中断环境下安全运行了——这是通往更复杂功能（多核支持、设备驱动并发、多进程文件系统操作）的基础。下一章见。

## 参考资料

- Intel SDM: Vol.3A Section 6.3.2 — Maskable Hardware Interrupts (RFLAGS.IF 的行为)
- `std::atomic` memory ordering: [cppreference memory_order](https://en.cppreference.com/w/cpp/atomic/memory_order)
- Linux: [lock selftests](https://github.com/torvalds/linux/tree/master/tools/testing/selftests/locking) — 内核锁自测框架
- ThreadSanitizer: [TSan documentation](https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual) — 数据竞争检测工具
- xv6-riscv: [spinlock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/spinlock.c) — holding() 运行时断言
- PintOS: [Thread tests](https://web.stanford.edu/~ouster/cgi-bin/cs140-spring19/pintos/pintos_6.html) — 同步测试框架
