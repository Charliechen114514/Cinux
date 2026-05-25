---
title: 021-proc-sync-3 · 同步原语
---

# 021-3 测试与集成代码精讲

> 前置：[021-2 Semaphore 代码精讲](021-proc-sync-2.md)
> 聚焦：main.cpp producer-consumer demo、test_sync.cpp 测试策略、magic check 修复

## 概览

本文是 tag 021 Read-through 的第三篇，聚焦集成代码和测试。我们先看 main.cpp 中替换掉 6 线程抢占 demo 的 producer-consumer 演示，然后深入 test_sync.cpp 的测试策略，最后拆解 mini kernel 中 MOV 编码变体导致的 magic check 修复。

## main.cpp — 从 6 线程抢占到 Producer-Consumer

tag 020 的 main.cpp 创建了 6 个线程做抢占式调度演示。tag 021 把它们全部替换成两个协作任务——producer 和 consumer：

```cpp
static constexpr int PC_BUF_SIZE = 4;

static int g_pc_buf[PC_BUF_SIZE];
static cinux::proc::Semaphore g_sem_free(PC_BUF_SIZE);
static cinux::proc::Semaphore g_sem_used(0);
static cinux::proc::Mutex g_pc_mutex;
```

这是经典的有界缓冲区设置。`g_pc_buf[4]` 是环形缓冲区本身；`g_sem_free` 初始值 4 追踪空闲槽位；`g_sem_used` 初始值 0 追踪已用槽位；`g_pc_mutex` 保护缓冲区的读写。四个全局变量构成 producer-consumer 模式的完整同步框架。

### Producer

```cpp
static void producer() {
    for (int i = 0; i <= 4; i++) {
        g_sem_free.wait();
        {
            auto g = g_pc_mutex.guard();
            g_pc_buf[i % PC_BUF_SIZE] = i;
        }
        g_sem_used.post();
        cinux::lib::kprintf("sent: %d\n", i);
    }
}
```

Producer 循环 5 次发送数据 0 到 4。每次循环的流程是：wait(sem_free) 消耗一个空闲槽位 → 花括号内用 guard 自动管理 Mutex 保护写入 → post(sem_used) 增加一个已用槽位 → 串口打印。

这里的花括号块 `auto g = g_pc_mutex.guard()` 很巧妙：`g` 的生命周期限定在这个花括号内，出花括号时自动析构调用 unlock()。这样 Mutex 的保护范围精确覆盖了缓冲区写入，不会在 post() 时还持有锁。

### Consumer

```cpp
static void consumer() {
    for (int i = 0; i <= 4; i++) {
        g_sem_used.wait();
        int val;
        {
            auto g = g_pc_mutex.guard();
            val = g_pc_buf[i % PC_BUF_SIZE];
        }
        g_sem_free.post();
        cinux::lib::kprintf("got: %d\n", val);
    }
}
```

Consumer 的结构和 Producer 镜像：wait(sem_used) 等待数据 → guard 保护读取 → post(sem_free) 归还空闲槽位。`val` 在 guard 作用域外声明但在作用域内赋值，确保出了临界区后才打印。

### 任务创建

```cpp
auto* task_prod = cinux::proc::TaskBuilder()
    .set_entry(producer).set_name("producer").build();
auto* task_cons = cinux::proc::TaskBuilder()
    .set_entry(consumer).set_name("consumer").build();

cinux::proc::Scheduler::add_task(task_prod);
cinux::proc::Scheduler::add_task(task_cons);
```

和 tag 020 一样用 TaskBuilder 创建任务，只是从 6 个变成了 2 个。两个任务被加入调度队列后，PIT 定时器中断驱动的抢占式调度会让它们交替执行。

## test_sync.cpp (kernel) — QEMU 内核集成测试

kernel/test/test_sync.cpp 包含 584 行测试代码，组织成 9 个测试组。我们按组讲解测试策略。

### Spinlock 基础测试（Test 1）

```cpp
void test_acquire_release() {
    Spinlock s;
    s.acquire();
    s.release();
    TEST_ASSERT_TRUE(true);  // reached without deadlock
}
```

最简单的冒烟测试——能 acquire 也能 release，没有死锁就通过。RAII guard 测试类似：在作用域内持锁，出作用域后再次 acquire 确认锁确实被释放了。

### Mutex 基础测试（Test 2-4）

Mutex 测试需要模拟"当前任务"。做法是手动创建 Task 对象并设置 `g_per_cpu.current`：

```cpp
Task* task = TaskBuilder()
    .set_entry(dummy_entry)
    .set_name("mutex_owner")
    .build();
g_per_cpu.current = task;
```

这模拟了调度器把某个任务设为当前运行任务的场景。`dummy_entry` 是一个空函数——这些 Task 不会真正被调度执行，只是被用作 Mutex 的 owner 标识。

try_lock 测试通过切换 `g_per_cpu.current` 来模拟不同任务的视角：先让 task A lock，再让 task B try_lock，验证 B 拿不到锁。

### Mutex 竞争与 FIFO 测试（Test 3）

```cpp
void test_fifo_ordering() {
    // owner locks
    g_per_cpu.current = owner;
    Mutex m;
    m.lock();
    // Three waiters enqueue
    g_per_cpu.current = w1; m.lock();
    g_per_cpu.current = w2; m.lock();
    g_per_cpu.current = w3; m.lock();
    // Unlock three times
    m.unlock();  // w1 should be Ready
    m.unlock();  // w2 should be Ready
    m.unlock();  // w3 should be Ready
}
```

FIFO 测试验证等待队列的顺序正确性。三个任务依次 lock 被阻塞，然后 owner 连续 unlock 三次，每次应该唤醒最早等待的那个。注意每次 `m.lock()` 之后，当前任务的 state 变成 Blocked，但测试代码不需要切换到其他任务——因为 block 只是修改了状态并调用 schedule，测试环境下 schedule 的行为由 Scheduler::init() 的状态决定。

### Semaphore 测试（Test 5-8）

Semaphore 测试覆盖了完整的生命周期：初始计数、post 递增、wait 递减（非阻塞和阻塞）、try_wait 边界条件、post 唤醒、FIFO 顺序、以及完整的 producer-consumer 无阻塞场景。

计数信号量模式测试验证了一个关键场景：初始值 3 的信号量连续 wait 3 次不阻塞（count 降到 0），然后连续 post 3 次（count 升到 3），再 wait 3 次也不阻塞——整个过程中没有任何任务被 block。

## test_sync.cpp (host) — Host 端单元测试

test/unit/test_sync.cpp 包含 775 行代码，在主机上编译运行。它的策略是重新实现（re-implement）Spinlock/Mutex/Semaphore 的逻辑，用 mock scheduler 代替真实的 block/unblock。

### Mock Scheduler

```cpp
namespace mock_scheduler {
static Task* last_blocked = nullptr;
static int block_count = 0;
void block(Task* task, const char* reason) {
    task->state = TaskState::Blocked;
    last_blocked = task;
    block_count++;
}
void unblock(Task* task) {
    task->state = TaskState::Ready;
    unblock_count++;
}
}
```

mock scheduler 不做真正的上下文切换，只是记录 block/unblock 调用并修改 Task 状态。这让测试可以验证"是否调用了 block"、"传入了哪个 Task"等行为。

Host 测试使用 `TEST("name") { ... }` 宏格式，每个测试用例独立运行。

## Magic Check 修复 — MOV 编码变体

这是 tag 021 中最隐蔽的 bug。添加 sync.cpp 后，BSS 段增长导致 `__kernel_stack_top` 的链接地址发生了变化。这个地址变化使得 GNU assembler 对 `_start` 中的 `movq $__kernel_stack_top, %rsp` 选择了不同的编码方式。

x86-64 对这条指令有两种合法编码。`REX.W MOV r64, imm64`（操作码 `48 BC`，8 字节立即数）是无条件的 64 位编码；`REX.W MOV r/m64, imm32`（操作码 `48 C7 C4`，4 字节符号扩展立即数）在立即数可以符号扩展到 64 位时使用。assembler 总是偏好更短的编码。

修复方案很简单——同时接受两种编码：

```cpp
bool is_real_kernel = (code[0] == 0xFA) && (code[1] == 0x48) &&
                      (code[2] == 0xC7 || code[2] == 0xBC);
```

这个 bug 的教训很深刻：x86-64 的指令编码不是唯一的，assembler 会根据操作数自动选择。任何基于机器码字节模式的检查都应覆盖所有合法编码变体。

## 设计决策

### 决策：Host 测试 re-implement vs 直接链接内核代码

**问题**: host 端测试如何测试内核同步原语？
**本项目的做法**: 在 host 端重新实现 Spinlock/Mutex/Semaphore 逻辑，用 mock scheduler 替代真实调度。
**备选方案**: 链接内核代码并 mock 掉 scheduler 依赖。
**为什么不选备选方案**: 内核代码有大量依赖（per_cpu、scheduler、process），mock 成本高。re-implement 保持了测试的独立性，且同步原语逻辑足够简单，重复实现的风险可控。
**如果要扩展**: 对于复杂的内核模块，可以考虑使用编译期 mock 或接口抽象来减少重复代码。

## 扩展方向

- **Stress test**: 创建多个 producer 和 consumer 并发操作同一缓冲区，验证无数据竞争（难度: 2 星）
- **Deadlock detection**: 在 Mutex 中添加持有图检测，发现循环等待时输出诊断信息（难度: 3 星）
- **Performance benchmark**: 测量 Mutex lock/unlock 的平均耗时，对比 Spinlock 的性能差异（难度: 1 星）

## 参考资料

- Intel SDM: Vol. 2A MOV instruction -- `REX.W MOV r64, imm64` vs `REX.W MOV r/m64, imm32` 编码变体
- OSDev Wiki: [Synchronization Primitives](https://wiki.osdev.org/Synchronization_Primitives) -- Producer-conber 模式示例
- Cinux Notes: `document/notes/021/001_big_kernel_magic_check.md` -- MOV 编码变体 bug 完整分析

## 测试策略的设计哲学

为什么 Cinux 要维护两套测试？这涉及测试哲学的问题。Host 端测试验证的是"设计逻辑是否正确"——它用重新实现的同步原语逻辑和 mock scheduler 来测试，确保算法本身没有 bug。比如 FIFO 测试验证的是 enqueue 尾插 + dequeue 头删是否真的保证了先进先出。这些测试在几毫秒内就能跑完，非常适合快速迭代。

内核测试验证的是"实现是否正确"——它直接链接真实的 Scheduler 和同步原语，在 QEMU 中运行。这些测试捕获的是 host 测试无法发现的问题：比如 Task 结构体的内存布局是否正确、g_per_cpu.current 的设置时机是否正确、Scheduler::block() 是否真的把任务从就绪队列中移除了。这些测试需要编译 big kernel 并在 QEMU 中运行，时间成本高但不可替代。

两套测试的测试用例名称和断言逻辑高度一致——host 测试用 `ASSERT_EQ`、`ASSERT_TRUE` 等宏，内核测试用 `TEST_ASSERT_EQ`、`TEST_ASSERT_TRUE` 等宏。这种一致性是有意的，它确保两套测试验证的是同一套行为规范。
