---
title: 021-proc-sync-1 · 同步原语
---

# 021-1 Spinlock 重构与侵入式等待队列

> 前置：[020 抢占式调度](020-proc-scheduler-3.md)
> 标签：spinlock, atomic, wait queue, intrusive list, RAII

## 导语

在 tag 020 里我们搭好了抢占式调度的骨架——PIT 定时器中断驱动 `tick()`，时间片耗尽就 `schedule()` 切走当前任务，再加上 `block()`/`unblock()` 机制让任务可以主动让出 CPU。但说实话，block/unblock 一直是"有接口但没用起来"的状态。这一章我们要做的，就是给内核加上真正的同步原语——先从 Spinlock 的重构和一个侵入式等待队列开始。

完成本章后，你会理解原子操作在 x86-64 上的具体实现方式，以及为什么内核里等待队列通常用侵入式链表而不是 `new` 出来的节点。

## 环境说明

这一章的变更集中在 `kernel/proc/sync.hpp`（类定义）和 `kernel/proc/sync.cpp`（新增，211 行实现），加上 `kernel/proc/process.hpp` 中新增的 `Task::wait_next` 字段。构建系统新增了 `test/unit/test_sync.cpp`（775 行 host 端测试）和 `kernel/test/test_sync.cpp`（584 行内核集成测试）。和之前一样，我们用 QEMU 运行 big kernel 验证 producer-consumer demo。编译工具链仍然是 GCC 14 + CMake + Ninja，目标平台 x86-64。

## 概念精讲

### Spinlock 的本质——原子 test-and-set

Spinlock 的核心思想非常直白：用一个布尔值表示锁的状态，想拿锁就尝试把它从 false 改成 true，如果已经是 true 就一直转圈等。难点在于"尝试改"这个操作必须是原子的——不能有两个 CPU 同时读到 false 然后都以为自己拿到了锁。

x86-64 给了我们直接可用的工具：`__atomic_test_and_set` 这个 GCC builtin，底层编译成 `LOCK BTS` 或者等价的原子指令，配合 `__ATOMIC_ACQUIRE` 内存序保证读操作不会重排到获取锁之前。释放时用 `__atomic_clear` 配合 `__ATOMIC_RELEASE` 内存序，保证临界区内的写操作对其他 CPU 可见。Intel SDM Vol. 3A Section 8.1 "Locked Atomic Operations" 对 LOCK 前缀和原子操作语义有完整说明——简单来说，LOCK 前缀让指令对缓存行做排他性访问，保证多核环境下的原子性。

你可能还注意到自旋循环里有一条 `pause` 指令（`__asm__ volatile("pause")`）。这条指令是给 CPU 的一个提示："我现在在等一个锁，别太着急。"Intel SDM Vol. 2A 的 "PAUSE -- Spin Wait Hint" 章节中明确说明：Intel 的 P4 和 Xeon 处理器上，如果不加 PAUSE，spin-wait 循环结束时 CPU 会误判发生了内存顺序违规，触发流水线 flush——性能惩罚非常严重。即使在我们测试用的 QEMU 环境里 PAUSE 相当于 NOP，这也是一个好习惯。

现在我们要做的第一件事，是把 Spinlock 的 acquire/release 从头文件内联实现移到独立的 sync.cpp 文件中。Spinlock 的角色在这一章发生了变化——它从"唯一的同步原语"变成了 Mutex 和 Semaphore 的"内部保护锁"，只用来保护这些高级原语的元数据（owner 指针、等待队列、计数器），操作完立即释放，持有时间不超过十来条指令。

### 为什么需要等待队列

Spinlock 只能用来保护极短的临界区——几个寄存器操作，几条内存读写。一旦你需要在持锁状态下等待某个条件（比如"缓冲区非空"），自旋就变成了浪费 CPU 的无底洞。这时候我们需要的是：把当前任务挂到某个等待队列上，把 CPU 让给别人跑，等条件满足了再被唤醒。

这就是 Mutex 和 Semaphore 要做的事。而它们都需要一个等待队列来记录"谁在等"。等待队列的核心语义其实就两个：把任务挂进去（enqueue），把任务取出来（dequeue）。听起来简单，但在内核环境里实现起来有一堆约束——不能依赖堆分配、要保证 FIFO 顺序、要在 Spinlock 的保护下操作。

### 侵入式链表——不分配内存的队列

教科书上的链表节点通常长这样：`struct Node { T data; Node* next; }`，每个节点需要动态分配内存。但内核的同步原语不能依赖堆分配——万一堆本身就需要锁保护呢？这就形成了鸡生蛋的问题。你可能觉得"堆分配器加把锁不就行了？"——但加锁本身就需要同步原语，同步原语又需要等待队列，等待队列又需要分配内存……死循环了。

侵入式链表的解法很巧妙：我们不单独分配节点，而是把 `next` 指针直接嵌入被管理的对象里。对 Cinux 来说，就是在 Task 结构体里加一个 `wait_next` 字段。这样一来，一个任务同时只能在一个等待队列上等待（因为只有一个 next 指针），但这在教学 OS 的单核环境里完全够用。

Linux 用了不同的方式来绕开这个限制——每个等待操作在栈上分配一个独立的 `mutex_waiter` 结构，里面包含 `list_head` 和 task 指针。这样任务可以同时在多个等待队列上等待，但引入了更多复杂性。PintOS 则使用通用双向链表（`lib/kernel/list.c`），每次等待操作需要分配一个 `list_elem`。Cinux 用侵入式单向链表避免了所有额外分配，虽然功能上有限制（一个任务一个队列），但在教学 OS 的场景里这个取舍完全合理。

## 动手实现

### Step 1: 在 Task 结构体中添加 wait_next 字段

**目标**: 让 Task 支持被链入等待队列。

**设计思路**: 我们在 `process.hpp` 的 Task 结构体末尾（`sched_class` 之后）添加一个 `Task* wait_next` 指针。这个指针在任务创建时被零初始化（knew 会清零分配的内存），在任务被加入等待队列时由 enqueue 函数设置，离开队列时清零。

**实现约束**: 字段类型是 `Task*`，初始值应该是 nullptr。当任务不在任何等待队列中时，这个值保持 nullptr。添加字段时注意加好注释，说明这个字段的用途——它在 Mutex 和 Semaphore 的等待队列中充当侵入式链表的 next 指针。建议在注释中说明"一个任务同时只能在一个等待队列上等待"的限制。

**验证**: 创建一个 Task 后，检查其 wait_next 字段确实为 nullptr。这一步不需要运行，编译通过即可。Host 端单元测试中 `task: wait_next is null after zero init` 测试会验证这个行为。

### Step 2: 将 Spinlock 的 acquire/release 移到 .cpp 文件

**目标**: Spinlock 之前是头文件内联实现，现在需要拆分到 sync.cpp 中。

**设计思路**: 头文件只保留声明（函数签名），实现移到 sync.cpp。这是为了减少编译依赖，也和 Mutex/Semaphore 的组织方式保持一致。同步原语的实现不需要暴露给调用者。注意 sync.hpp 中还有 RAII Guard 类（构造时 acquire，析构时 release），它仍然留在头文件中因为是内联的。Guard 的拷贝构造和拷贝赋值需要 delete，防止误拷贝导致同一把锁被 unlock 两次。

**踩坑预警**: 移出实现后确保所有 include 路径正确。sync.cpp 需要包含 sync.hpp、process.hpp、scheduler.hpp 和 per_cpu.hpp。如果编译器报 `g_per_cpu` 未声明，说明遗漏了 `per_cpu.hpp` 的包含。

**验证**: 编译通过即可。这一步不涉及功能变化，只是代码组织方式的调整。

### Step 3: 实现侵入式等待队列的 enqueue 和 dequeue

**目标**: 为 Mutex 和 Semaphore 提供等待队列操作。

**设计思路**: 我们需要两个辅助操作：enqueue（尾插法，保证 FIFO 顺序）和 dequeue（头删法，先进先出）。这两个操作在 Spinlock 的保护下执行，所以它们本身不需要考虑并发安全。

enqueue 的流程是：先将新节点的 wait_next 置为 nullptr，然后遍历链表找到尾部，把尾部节点的 wait_next 指向新节点。如果链表为空（head 是 nullptr），直接把 head 指向新节点。遍历找尾部的时间复杂度是 O(n)，但单核教学 OS 中等待者数量极少（通常不超过三五个），线性遍历完全可接受。

dequeue 的流程是：如果 head 为 nullptr 就返回 nullptr；否则取 head 指向的节点，把 head 后移到下一个节点，把取出的节点的 wait_next 清零，返回该节点。

**实现约束**: Mutex 和 Semaphore 各自有独立的等待队列（各自有 wait_head_ 成员），所以各自需要独立的 enqueue/dequeue 方法。虽然逻辑完全相同，但由于它们操作的是不同的 wait_head_，所以需要分别实现。如果将来想要消除重复代码，可以考虑用一个模板基类或独立的辅助函数（接受 wait_head_ 引用参数），但目前保持各自独立实现更清晰。

**踩坑预警**: enqueue 时一定要把新节点的 wait_next 设为 nullptr，否则可能误把旧链表尾部之后的数据当成链表的一部分。这是一个非常容易犯的错误，尤其是 wait_next 字段可能残留之前的值。dequeue 时同样要清零取出的节点的 wait_next，否则被取出的任务还以为自己链在某个队列里。Host 端单元测试中 `wait_queue: enqueue clears wait_next` 测试专门验证了这个行为——它先把 wait_next 设为一个哨兵值（0xDEAD），然后 enqueue，验证 wait_next 被正确清零。

**验证**: enqueue 三个任务 A、B、C，然后连续 dequeue 三次，验证返回顺序是 A -> B -> C。Host 端单元测试中有专门的 `wait_queue` 测试组覆盖这个场景，包括 `wait_queue: enqueue one dequeue one`、`wait_queue: FIFO ordering`、`wait_queue: dequeue empty returns nullptr` 四个测试。

## 内存序补充说明

在深入实现之前，这里值得展开讲一下 `__ATOMIC_ACQUIRE` 和 `__ATOMIC_RELEASE` 的含义，因为这两个概念在后续 Mutex 和 Semaphore 的实现中会反复出现。理解了它们，你就能理解为什么 Spinlock 的 acquire/release 要用这两种特定的内存序。

Acquire 语义保证"获取锁之后的读操作不会被重排到获取锁之前"——想象你在餐厅拿了一个号牌，你必须先拿到号牌才能去看菜单（读临界区数据）。Release 语义保证"释放锁之前的写操作对其他 CPU 可见"——相当于你在离开餐厅之前必须先付完账（写操作），然后才能把号牌还回去（释放锁）。这两个语义组合起来就形成了经典的 acquire-release 同步对：获取锁的一方看到释放锁一方的所有写操作。

在 x86-64 上，由于 CPU 本身是强排序的（TSO 模型），acquire 和 release 屏障实际上大部分时候是 no-op。但显式标注内存序是好习惯——一是代码意图更清晰，二是移植到 ARM 等弱排序架构时不会出 bug。`__atomic_test_and_set` 和 `__atomic_clear` 是 GCC 的 C11 原子操作 builtin，不依赖 C++ `<atomic>` 头文件，适合在内核这种没有完整 C++ 标准库的环境下使用。

## 前置知识回顾：tag 020 的调度基础设施

在开始之前，我们快速回顾一下 tag 020 搭建的调度基础设施，因为这些是 tag 021 直接依赖的。

tag 020 实现了一个完整的抢占式调度器。`Scheduler::init()` 初始化调度器，创建 idle_task（只执行 hlt 的空闲任务）。`Scheduler::add_task(task)` 把任务加入就绪队列（环形数组 Round-Robin）。`Scheduler::block(task, reason)` 把任务标记为 Blocked 然后调用 `schedule()` 切换到下一个就绪任务。`Scheduler::unblock(task)` 把任务从 Blocked 改回 Ready 并重新加入就绪队列。`g_per_cpu.current` 指向当前正在运行的任务，在 `per_cpu.hpp` 中定义。`Scheduler::tick()` 每 N 次定时器中断调用一次 `schedule()`，实现时间片轮转。

这些机制就是 Mutex 和 Semaphore 的基础——lock() 调用 block() 让拿不到锁的任务睡眠，unlock() 调用 unblock() 唤醒等待者。block/unblock 在 tag 020 已经实现好了，tag 021 直接使用它们。

另外，tag 020 的 Spinlock 是头文件内联实现的，类定义在 sync.hpp 中。tag 021 保持 sync.hpp 的 Spinlock 类定义不变，只是把实现移到 sync.cpp。Task 结构体的 `sched_class` 字段是 tag 020 添加的，tag 021 的 `wait_next` 字段会加在它之后。

## 代码组织与文件结构

在开始动手之前，我们先理清 tag 021 涉及的文件变更和它们之间的关系。这有助于你在动手实现时知道改哪里、为什么改。

新增文件只有一个：`kernel/proc/sync.cpp`，包含 Spinlock 的 acquire/release 实现、Mutex 的全部方法、Semaphore 的全部方法，总共约 211 行。这是 tag 021 的核心实现文件。

修改的文件有三个：`kernel/proc/sync.hpp` 在原有 Spinlock 类声明的基础上新增了 Mutex 和 Semaphore 的完整类定义，包括前置声明、嵌套 Guard 类、文档注释等，从头文件的角度提供了所有同步原语的公开接口。`kernel/proc/process.hpp` 在 Task 结构体末尾新增了 `Task* wait_next` 字段，这个字段的注释说明了它是侵入式等待队列的链表节点。`kernel/CMakeLists.txt` 把 sync.cpp 加入 big_kernel_common 的源文件列表，把 test_sync.cpp 加入 big_kernel_test 的源文件列表。

测试文件新增两个：`test/unit/test_sync.cpp` 是 host 端单元测试（775 行），在主机上编译运行，用重新实现的同步原语逻辑和 mock scheduler 测试。`kernel/test/test_sync.cpp` 是内核集成测试（584 行），在 QEMU 中运行，直接链接真实的 Scheduler 和同步原语。`test/CMakeLists.txt` 和 `kernel/test/main_test.cpp` 分别注册了新的测试目标。

理解了这些文件之间的关系，我们就可以开始动手了。

值得注意的一个细节是：sync.hpp 中需要前置声明 `struct Task;`。这是因为 Mutex 和 Semaphore 的成员变量（`Task* owner_`、`Task* wait_head_`）以及 enqueue/dequeue 方法的参数都涉及 Task 类型，但 sync.hpp 不想直接包含 process.hpp（那会引入大量不必要的编译依赖）。前置声明告诉编译器"Task 是一个 struct，你只需要知道它的名字就够了，具体定义在别的地方"。只有在 sync.cpp 中我们才真正需要 Task 的完整定义，所以 sync.cpp 包含了 process.hpp。

## 构建与运行

编译并运行 host 端单元测试，验证等待队列操作的正确性：

```bash
cd build && cmake .. && make test_sync -j$(nproc)
./test/unit/test_sync
```

预期输出应该显示所有 wait_queue 相关测试通过，包括 `wait_queue: enqueue one dequeue one`、`wait_queue: FIFO ordering`、`wait_queue: dequeue empty returns nullptr` 和 `wait_queue: enqueue clears wait_next`。同时 Spinlock 的测试也应该全部通过——`spinlock: initial state is unlocked`、`spinlock: acquire and release cycle`、`spinlock: guard acquires and releases` 和 `spinlock: double release is benign`。

也可以运行内核集成测试来验证等待队列在真实调度环境中的行为：

```bash
cd build && cmake .. && make run-kernel-test -j$(nproc)
```

串口输出中 "Sync Tests (021)" 段的 `test_wait_next_null_after_build` 测试验证了 Task 创建后 wait_next 为 nullptr。`test_acquire_release` 和 `test_guard_raii` 验证 Spinlock 在真实内核环境下的正确行为。

## 调试技巧

1. **wait_next 没有被清零**: 如果 dequeue 后任务的 wait_next 指向了无效地址，下次 enqueue 时可能触发 page fault。在调试器中检查 wait_next 的值。更具体地说，可以在 enqueue 函数入口加一个断言检查 `task->wait_next == nullptr`，如果断言失败说明上一个 dequeue 没有正确清零。
2. **enqueue 后链表成环**: 这通常是因为忘记设置 `task->wait_next = nullptr`，导致尾部节点的 next 指向了之前的节点。遍历链表时如果无限循环就是这个问题。一个简单的诊断方法是在 enqueue 后从头遍历链表，如果遍历步数超过等待者总数就说明有环。
3. **FIFO 顺序不对**: 检查 enqueue 是不是用的尾插法（遍历到尾部再插入），而不是头插法。如果用头插法，出队顺序会变成 LIFO。这个 bug 在测试中很容易暴露——FIFO 测试用例会检查出队顺序是否和入队顺序一致。
4. **头文件依赖缺失**: 如果 sync.cpp 编译失败提示 `g_per_cpu` 未声明，检查是否包含了 `per_cpu.hpp`。如果提示 `Scheduler` 未声明，检查是否包含了 `scheduler.hpp`。
5. **Spinlock 实现被内联**: 如果你发现 Spinlock 的 acquire/release 仍然是内联的，检查 sync.cpp 是否被正确编译和链接。可以在 .cpp 文件中加一个 `static_assert` 或者打印语句确认代码被执行。

## 为什么 Spinlock 需要重构到 .cpp

你可能会问：既然 Spinlock 的实现没有变化，为什么还要费劲把它从头文件移到 .cpp 文件？原因有三点。

第一，编译依赖管理。如果 Spinlock 的实现留在头文件里，任何包含 sync.hpp 的文件都会在编译时看到 `__atomic_test_and_set` 和 `__asm__ volatile("pause")` 的实现细节。这些是 x86-64 特有的，如果将来 Cinux 要移植到 ARM 就得改头文件，引发大规模重编译。

第二，和 Mutex/Semaphore 的组织方式保持一致。Mutex 和 Semaphore 的实现都在 sync.cpp 中，只有 Spinlock 留在头文件里会显得不一致。统一放在 .cpp 中让代码组织更清晰。

第三，为 sync.cpp 的其他实现提供基础设施。Mutex 的 lock/unlock 和 Semaphore 的 wait/post 都需要 Spinlock 来保护内部状态。把 Spinlock 的实现也放在同一个文件里，所有同步原语的实现集中在 sync.cpp 中，方便维护和理解。

## 设计对比：侵入式链表在不同 OS 中的实现

每个教学 OS 处理等待队列的方式都不一样。xv6 用通道（channel）机制实现等待——`sleep(chan, &lk)` 让进程在某个通道上等待，`wakeup(chan)` 扫描整个进程表找匹配的 SLEEPING 进程。这种设计不需要侵入式链表，但每次唤醒都是 O(n) 的全表扫描，而且有 thundering herd 问题（所有在同一个通道上等待的进程都被唤醒，然后只有一个能真正拿到资源）。

PintOS 使用通用双向链表（`lib/kernel/list.c`），每个等待操作需要分配一个 `list_elem` 作为链表节点。这个方案功能强大但引入了额外的内存分配。

Linux 的方案最复杂——每个等待操作在栈上分配一个 `wait_queue_entry` 结构（不需要堆分配），通过 `list_head` 双向链表挂入等待队列。Cinux 的侵入式单向链表在功能和复杂度之间取了一个教学环境下的最佳平衡。

## 参考资料

- Intel SDM: Vol. 2A "PAUSE -- Spin Wait Hint" -- PAUSE 指令避免 P4/Xeon 流水线 flush
- Intel SDM: Vol. 3A Section 8.1 "Locked Atomic Operations" -- LOCK 前缀和原子操作语义
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- Spinlock/Mutex/Semaphore 总览
- xv6: [sleeplock.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/sleeplock.c) -- xv6 的通道等待机制
- Linux: [include/linux/wait.h](https://github.com/torvalds/linux/blob/master/include/linux/wait.h) -- Linux 等待队列实现
- GCC Documentation: [Atomic Builtins](https://gcc.gnu.org/onlinedocs/gcc/_005f_005fatomic-Builtins.html) -- `__atomic_test_and_set` 和 `__atomic_clear` 的完整说明

## 测试策略概述

tag 021 使用两套互补的测试策略来验证同步原语的正确性。Host 端单元测试（`test/unit/test_sync.cpp`，775 行）在主机上编译运行，用 mock scheduler 替代真实的 block/unblock，编译快、运行快、调试方便。内核集成测试（`kernel/test/test_sync.cpp`，584 行）在 QEMU 中运行，直接链接真实的 Scheduler 和同步原语，验证实际硬件行为。

两套测试使用了不同的 Task 创建方式——host 测试在栈上创建 Task 结构体，内核测试用 TaskBuilder 在堆上创建。这种差异是有意的，它验证了等待队列在不同内存分配策略下的正确性。

## Host 端测试与内核测试的区别

你可能注意到 Cinux 有两套测试：host 端单元测试和内核集成测试。在 tag 021 中，它们分别对应 `test/unit/test_sync.cpp`（775 行）和 `kernel/test/test_sync.cpp`（584 行）。它们的策略截然不同，理解这个区别有助于你写出更好的测试。

Host 端测试的策略是"重新实现"——在测试文件中重新写一份 Spinlock/Mutex/Semaphore 的逻辑，用 mock scheduler（只记录 block/unblock 调用，不做真正的上下文切换）替代真实的 Scheduler。好处是编译快、运行快、调试方便，在主机终端就能看到结果。坏处是重新实现的逻辑可能和内核代码有细微差异——所以它测的是"设计是否正确"。

内核集成测试直接链接真实的 Spinlock/Mutex/Semaphore 和 Scheduler，在 QEMU 中运行。它测的是"实现是否正确"，但编译慢、运行慢、调试需要看串口输出。两套测试互补：host 测试快速迭代验证设计逻辑，内核测试做最终集成验证。

对于等待队列的测试，host 端测试验证 enqueue/dequeue 的逻辑正确性（FIFO 顺序、空队列处理、wait_next 清零），内核测试验证在真实调度环境中的行为（Task 对象的真实内存布局、g_per_cpu.current 的正确设置）。两者缺一不可。

## 本章小结

| 概念 | 要点 |
|------|------|
| Spinlock | `__atomic_test_and_set` + `pause` + `__atomic_clear`，仅用于极短临界区 |
| 侵入式链表 | `Task::wait_next` 嵌入 Task 结构体，零额外内存分配 |
| enqueue | 尾插法，保证 FIFO 顺序，O(n) 但等待者极少 |
| dequeue | 头删法，先进先出，必须清零取出节点的 wait_next |
| RAII Guard | 构造获取锁，析构释放锁，防止忘记 unlock |
| 设计对比 | xv6 通道扫描 / PintOS 双向链表 / Linux 栈上 waiter / Cinux 侵入式单向链表 |

## 下一步预告

这一章我们完成了 Spinlock 的重构和侵入式等待队列的实现，为 Mutex 和 Semaphore 打好了基础。Spinlock 从主角变成了配角——它现在是更高级同步原语的内部保护机制。但 Spinlock 本身只能做忙等互斥，我们还没有让它能"睡觉"。

下一章我们就在这个基础上构建 Mutex——阻塞式互斥锁，让拿不到锁的任务可以优雅地让出 CPU 而不是傻转圈。Mutex 是 tag 021 最核心的原语，它直接使用了我们在这一章实现的等待队列和重构后的 Spinlock。如果你对侵入式链表的实现还有疑问，建议在继续之前重新阅读 Step 3 的 enqueue/dequeue 流程，确保理解了尾插头删的 FIFO 保证。
