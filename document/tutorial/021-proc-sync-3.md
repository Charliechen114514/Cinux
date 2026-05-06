# 021-3 实战踩坑：Producer-Consumer 验证与 MOV 编码陷阱

> 前置：[021-2 Mutex 与 Semaphore](021-proc-sync-2.md)
> 标签：producer-consumer, testing, MOV encoding, x86 instruction variants, magic check

## 前言

前两章我们看完了 Spinlock、Mutex、Semaphore 的设计和实现，也搭好了 Producer-Consumer 的代码框架。这一章我们不写新代码——我们来聊两件实战中更重要的事：如何验证同步原语的正确性，以及一个差点让我血压拉满的 MOV 编码 bug。

说实话，同步原语的正确性验证比实现本身更难。你写了一个 Mutex，怎么证明它不会死锁？怎么证明 FIFO 顺序是对的？怎么证明 Semaphore 的计数器在各种边界条件下都正确？Cinux 用了两套测试来回答这些问题——host 端单元测试（775 行）和 QEMU 内核集成测试（584 行），覆盖了从 Spinlock 到 Producer-Consumer 的所有场景。

两套测试各有侧重，互为补充。Host 端测试验证"设计逻辑是否正确"——它用重新实现的同步原语逻辑和 mock scheduler 来测试，确保算法本身没有 bug。内核测试验证"实现是否正确"——它直接链接真实的 Scheduler 和同步原语，在 QEMU 中运行。Host 测试可以快速迭代验证设计逻辑，内核测试做最终集成验证。两套测试覆盖了 21 个 Spinlock/Mutex/Semaphore 相关的测试用例，从最基本的 acquire/release 到完整的 FIFO 顺序和 producer-consumer 场景。

这些测试加起来约 1360 行代码，超过了 sync.cpp 实现本身（211 行）的 6 倍。这不是过度测试——同步原语的 bug 通常只在特定时序下触发，没有全面的测试覆盖几乎不可能通过手工测试发现所有问题。

## 测试策略——两套测试的分工

Host 端测试（test/unit/test_sync.cpp）的策略是重新实现同步原语的逻辑，用 mock scheduler 替代真实的 block/unblock。这样做的好处是完全脱离内核环境，编译快、运行快、调试方便。坏处是重新实现的逻辑可能和内核代码有细微差异——所以它测的是"设计是否正确"而非"实现是否正确"。

内核集成测试（kernel/test/test_sync.cpp）直接链接真实的 Spinlock/Mutex/Semaphore 和 Scheduler，在 QEMU 中运行。它测的是"实现是否正确"，但编译慢、运行慢、调试需要看串口输出。

两套测试互补：host 测试快速迭代验证设计逻辑，内核测试做最终集成验证。这种策略在简单性和覆盖率之间取得了好的平衡。Host 测试可以快速验证 FIFO 顺序、边界条件、RAII guard 等行为；内核测试验证 Task 的真实内存布局、g_per_cpu.current 的正确设置、Scheduler::block 的真实行为。

## Host 端测试——Mock Scheduler 的设计

mock scheduler 的核心思路很简单：不调度，只记录。它不执行真正的上下文切换，只修改任务状态并记录 block/unblock 调用。这让测试可以验证"是否调用了 block"、"传入了哪个 Task"、"总共 block 了几次"等行为，而不需要真的在 QEMU 中运行。mock per_cpu 同样简单——它维护一个静态 Task 数组（8 个任务），通过 `set_current(idx)` 来模拟"哪个任务在运行"。

Host 测试使用 `TEST("name") { ... }` 宏格式，每个测试用例独立运行。测试开始前调用 `mock_per_cpu::init()` 和 `mock_scheduler::reset()` 清理状态，避免测试之间的干扰。

Semaphore 的边界条件测试特别有趣。初始值 0 的信号量调用 wait() 应该让 count 变成 -1 并阻塞调用者。然后 post() 应该让 count 变回 0 并唤醒被阻塞的任务。测试断言 `count_ == 0` 和 `task->state == Ready` 来验证这一行为。FIFO 测试验证多个等待者按入队顺序被唤醒——三个任务依次 wait（count 变 -1, -2, -3），三次 post 依次唤醒 task 0、task 1、task 2。

Mutex 的 FIFO 测试也有趣。三个任务按序 lock 被阻塞，owner 连续 unlock 三次。测试通过检查 `last_unblocked` 指针验证唤醒顺序是否正确。Host 测试还可以验证 `block_count` 和 `unblock_count` 是否匹配——如果 block 多于 unblock，说明有等待者没被唤醒。

```cpp
namespace mock_scheduler {
static Task* last_blocked = nullptr;
static int block_count = 0;
void block(Task* task, const char* reason) {
    task->state = TaskState::Blocked;
    last_blocked = task;
    block_count++;
}
}
```

`block()` 把任务状态改为 Blocked 并记录，`unblock()` 改为 Ready 并记录。测试代码通过检查 `block_count`、`last_blocked` 和任务状态来验证同步原语的行为。

比如 Mutex 的 FIFO 测试是这样的：三个任务依次 lock 被阻塞，然后连续 unlock 三次，验证每次唤醒的都是最早等待的那个。测试断言检查 `last_unblocked` 是否按序指向 task1、task2、task3。

Semaphore 的边界条件测试更有意思。初始值 0 的信号量调用 wait() 应该让 count 变成 -1 并阻塞调用者。然后 post() 应该让 count 变回 0 并唤醒被阻塞的任务。测试断言 `count_ == 0` 和 `task->state == Ready` 来验证这一行为。

## 内核集成测试——在 QEMU 中验证真实行为

内核测试的组织方式和 host 测试不同——它使用 `TaskBuilder` 创建真实的 Task 对象（在堆上分配），手动设置 `g_per_cpu.current` 来模拟"哪个任务在运行"。当 lock() 调用 Scheduler::block() 时，block 会真正修改任务状态并调用 schedule()。但在测试环境下，因为只有 idle_task 在就绪队列中，schedule 会切到 idle_task 执行 hlt。

内核测试的 FIFO 测试是最核心的集成测试。它验证的不只是等待队列的顺序——还验证了 Mutex::unlock() 的所有权转移是否正确工作。三个任务 w1、w2、w3 依次在同一个 Mutex 上阻塞，owner 连续 unlock 三次。每次 unlock 后，测试检查 w1/w2/w3 的状态是否按序变为 Ready。如果等待队列或所有权转移有任何 bug，这个测试就能抓到。

Semaphore 的集成测试覆盖了完整的生命周期：初始计数、post 递增、wait 递减（非阻塞和阻塞）、try_wait 边界条件、post 唤醒、FIFO 顺序、以及完整的 producer-consumer 无阻塞场景。计数信号量模式测试验证了一个关键场景：初始值 3 的信号量连续 wait 3 次不阻塞（count 降到 0），然后连续 post 3 次（count 升到 3），再 wait 3 次也不阻塞——整个过程中没有任何任务被 block。

## Producer-Consumer 验证——串口输出是最好的朋友

main.cpp 中的 producer-consumer demo 是最终的集成验证。Producer 发送 0 到 4，Consumer 接收它们。如果同步原语有 bug，你会看到以下症状之一：

1. Consumer 收到重复数据——Mutex 没有正确保护缓冲区
2. Consumer 永远阻塞——Semaphore 的 post() 没有正确唤醒
3. Producer 发送了 5 个数据但 Consumer 只收到 3 个——信号量计数不对
4. 死锁——锁的获取顺序不一致或 Spinlock 在阻塞前没释放

正确的串口输出应该是 `sent: 0` 到 `sent: 4` 和 `got: 0` 到 `got: 4` 交错出现，数据值严格递增。由于 PIT 定时器中断驱动的抢占式调度，producer 和 consumer 会交替执行，但每次打印的值必须正确。这个测试虽然简单，但覆盖了同步原语的完整功能——Semaphore 的 P/V 操作、Mutex 的互斥保护、Scheduler 的 block/unblock 都在一条执行路径上被验证。

## MOV 编码陷阱——差点让我血压拉满

这是 tag 021 中最隐蔽的 bug，记录在 `document/notes/021/001_big_kernel_magic_check.md` 中。现象很诡异：mini kernel 测试全部通过，但 big kernel 测试从未执行，直接退出。串口输出显示 `=== Loaded ELF is not a real kernel, exiting ===`，但我们明明编译了一个正确的 big kernel。

Mini kernel 在跳转 big kernel 之前会检查入口字节——验证 ELF 的入口点确实是 `_start` 函数。`_start` 的前两条指令是 `cli`（opcode 0xFA）和 `movq $__kernel_stack_top, %rsp`。旧检查只认一种编码：`0xFA 0x48 0xBC`，即 `REX.W MOV r64, imm64`。这个检查在 tag 020 及之前都工作正常，因为 assembler 总是生成 `48 BC` 编码。

问题出在第二条指令上。x86-64 的 `movq $imm, %rsp` 有两种合法编码。`48 BC <8字节立即数>` 是无条件的 64 位编码，总是可用；`48 C7 C4 <4字节立即数>` 是符号扩展编码，只在立即数可以符号扩展到 64 位时使用。GNU assembler 总是偏好更短的编码。Intel SDM Vol. 2A 的 MOV 指令条目中明确列出了这两种编码变体。

添加 sync.cpp（211 行新代码）后，BSS 段增长导致 `__kernel_stack_top` 的链接地址发生了变化——从一个需要完整 8 字节编码的地址，变成了可以用 4 字节符号扩展表示的地址。assembler 自动选择了更短的 `48 C7 C4` 编码，而旧检查只认 `48 BC`，直接判定"不是真内核"。

修复方案同时接受两种编码：

```cpp
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) &&
                      (code[2] == 0xC7 || code[2] == 0xBC);
```

这个 bug 的教训非常深刻：x86-64 的指令编码不是唯一的，assembler 会根据操作数自动选择最优编码。任何基于机器码字节模式的检查都应覆盖所有合法编码变体。而 BSS 段大小的变化就能改变链接地址，进而改变 assembler 的编码选择——这个连锁反应特别难预测。这个 bug 也说明了为什么测试策略要覆盖端到端场景——如果只测 Spinlock/Mutex/Semaphore 的单元测试，这个 bug 永远不会被发现。只有运行完整的 big kernel 才能触发这个 magic check。

## 设计对比：测试策略在不同 OS 中的差异

xv6 没有独立的同步原语测试——它的测试主要通过用户态程序（比如 `usertests`）来间接验证。PintOS 的测试框架更加结构化——每个 Project 有明确的测试用例清单，Semaphore 的正确性通过 Project 1（Threads）的 alarm 和 priority scheduling 测试来验证。Linux 的同步原语测试更加底层——`tools/testing/selftests/locking/` 下有专门的 futex 和 mutex 测试，使用 `pthread_mutex` 和原子操作来验证锁的正确性。

Cinux 的双层测试策略（host 单元测试 + QEMU 集成测试）在简单性和覆盖率之间取得了好的平衡。Host 测试可以快速验证设计逻辑，内核测试验证实际硬件行为。这种策略的一个缺点是 host 测试的重新实现可能和内核代码有细微差异，但通过保持两套实现的测试逻辑一致（相同的测试场景、相同的断言条件），可以把这种风险降到最低。

## 收尾

到这里 tag 021 就大功告成了。我们实现了 Spinlock 的重构、Mutex 的阻塞式互斥锁、Semaphore 的 Dijkstra P/V 操作，用三者的组合解决了 Producer-Consumer 问题，还踩了一个极其隐蔽的 x86 编码陷阱。

tag 021 交付物清单：`kernel/proc/sync.hpp` 新增 Mutex 和 Semaphore 的完整类定义，`kernel/proc/sync.cpp` 新增 211 行实现代码，`kernel/proc/process.hpp` 新增 Task::wait_next 字段，`test/unit/test_sync.cpp` 新增 775 行 host 端测试，`kernel/test/test_sync.cpp` 新增 584 行内核集成测试。main.cpp 从 6 线程抢占 demo 替换为 producer-consumer demo。mini kernel 的 magic check 修复同时接受 MOV 的两种编码变体。

这些同步原语是后续所有内核功能的基础——文件系统需要 Mutex 保护 inode 缓存，网络栈需要 Semaphore 管理缓冲区，用户态进程需要同步来安全地共享内存。确保你的 producer-consumer demo 在 QEMU 中正确运行，串口输出显示 `sent: 0` 到 `sent: 4` 和 `got: 0` 到 `got: 4`，这是后续所有工作的基础。

下一章我们将进入 tag 022——Ring 3 用户态，Cinux 的内核线程终于要走到用户空间了。

## 参考资料

- Intel SDM: Vol. 2A MOV instruction -- `REX.W MOV r64, imm64` vs `REX.W MOV r/m64, imm32` 编码变体
- Cinux Notes: [document/notes/021/001_big_kernel_magic_check.md](https://github.com/Charliechen114514/cinux/blob/main/document/notes/021/001_big_kernel_magic_check.md) -- MOV 编码变体 bug 完整分析
- xv6: [usertests.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/user/usertests.c) -- xv6 通过用户态程序间接测试同步原语
- Linux: [tools/testing/selftests/locking/](https://github.com/torvalds/linux/tree/master/tools/testing/selftests/locking) -- Linux 锁测试套件
