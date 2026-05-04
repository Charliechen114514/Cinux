# kernel_main 重构与调度器启动

> 标签：kernel_main, scheduler, TaskBuilder, boot task, init thread
> 前置：[028d 同步安全](028d-sync-safety-3.md)

## 概览

本文是 028e Read-through 系列的第一篇，聚焦于 `kernel/main.cpp` 的核心重构——从「一条路走到黑」的单线程初始化，变为「早期初始化 + 调度器启动 + init 线程」的三段式架构。我们将逐段拆解 diff 中的每一次改动，理解为什么 `kernel_main()` 不再负责文件系统挂载和 shell 启动，以及新的启动序列是如何让调度器、boot task、init 线程各司其职的。

关键设计决策一览：
- `Scheduler::init()` 从 `run_concurrent_stress()` 中提取到 `kernel_main()` 直接调用
- 新建 `kernel_init_thread()` 负责文件系统挂载和 shell 启动
- boot CPU 上下文变成 boot task，进入 yield+hlt 空闲循环
- 删除整个 `kernel/stress/` 目录

## 架构图

```
kernel_main()
  │
  ├── Step 1-21: 硬件初始化（串口、GDT、IDT、PIC、PIT、PMM、VMM...）
  ├── Step 22: AHCI init + set_instance()
  │
  ├── Scheduler::init()              ← 新增：直接调用
  │     └── 创建 idle task
  │
  ├── TaskBuilder → kernel_init      ← 新增：init 线程
  │     └── Scheduler::add_task()
  │
  ├── TaskBuilder → boot             ← 新增：boot task
  │     └── Scheduler::run_first()
  │           └── context_switch → kernel_init
  │
  └── yield + hlt 空闲循环            ← 新增：idle loop
```

## 代码精讲

### 删除旧的 stress test 和键盘轮询

首先看 `kernel_main()` 的尾部被移除了哪些内容。

原来的 `kernel_main()` 在 AHCI 初始化之后还有三个大块：ext2 挂载、VFS 注册、以及 `run_concurrent_stress()` 调用（内部包含压力测试和 shell 启动）。最后还有一个键盘轮询的死循环——不停地 `Keyboard::poll(ev)` 并把按键 echo 到控制台。

这些全部被移除了。ext2 挂载和 VFS 注册搬到了 `kernel_init_thread()` 中（下一篇文章会详细讲），`run_concurrent_stress()` 连同整个 `kernel/stress/` 目录被彻底删除，键盘轮询也不再需要——因为 shell 启动后会自己处理键盘输入。

同时被移除的还有头文件引用——`kernel/fs/ext2.hpp` 和 `kernel/fs/vfs_mount.hpp` 不再被 `main.cpp` 包含，取而代之的是 `kernel/proc/scheduler.hpp`、`kernel/proc/init.hpp` 和 `kernel/proc/process.hpp`：

```cpp
// kernel/main.cpp — 新增的 include
#include "kernel/arch/x86_64/memory_layout.hpp"
#include "kernel/proc/scheduler.hpp"
#include "kernel/proc/init.hpp"
#include "kernel/proc/process.hpp"
```

using 声明也相应更新，新增了 `Scheduler` 和 `TaskBuilder`，移除了 `KeyEvent`：

```cpp
using cinux::proc::Scheduler;
using cinux::proc::TaskBuilder;
```

### Scheduler::init() 直接调用

下面是 `kernel_main()` 尾部的新增代码，也是这次重构的核心。

```cpp
// Step 22: Initialise scheduler and spawn kernel init thread
cinux::lib::kprintf("[BIG] ===== Scheduler & Init Thread =====\n");
Scheduler::init();
```

`Scheduler::init()` 之前藏在 `run_concurrent_stress()` 里面，现在被提到 `kernel_main()` 中直接调用。这保证了调度器在创建任何 Task 之前就已经初始化完毕。`init()` 内部会注册默认的 RoundRobin 调度类，并创建 idle task。

### 创建 init 线程

```cpp
auto* init_task =
    TaskBuilder().set_entry(cinux::proc::kernel_init_thread).set_name("kernel_init").build();
if (init_task != nullptr) {
    Scheduler::add_task(init_task);
}
```

这段代码用 `TaskBuilder` 的 Builder 模式创建了一个新的 Task。`set_entry` 指定了线程入口函数——`kernel_init_thread()`，定义在 `kernel/proc/init.cpp` 中（下一篇文章的主角）。`set_name` 给它起了一个可读的名字 "kernel_init"，方便调试时在日志中识别。`build()` 完成实际的 Task 分配：从堆上分配 Task 结构体，分配内核栈的物理页并映射到虚拟地址空间，初始化 CPU 上下文（RIP 指向入口函数，RSP 指向栈顶-8 并压入 `exit_current` 的地址作为返回地址）。

创建成功后，`add_task()` 把它加入默认 RoundRobin 调度类的运行队列。此时 init 线程已经就绪，但还没有被调度到——要等 `run_first()` 启动调度之后才会执行。

### 创建 boot task 并启动调度

```cpp
auto* boot_task = TaskBuilder()
                      .set_entry([]() {
                          cinux::lib::kprintf("[BOOT] boot_task_entry reached -- UNEXPECTED\n");
                          while (true)
                              __asm__ volatile("hlt");
                      })
                      .set_name("boot")
                      .build();
if (boot_task != nullptr) {
    Scheduler::run_first(boot_task);
}
```

boot task 的入口是一个 lambda——打印一条 "UNEXPECTED" 消息然后进入 hlt 死循环。这个 lambda 理论上永远不应该被执行到，因为 `run_first()` 会立即 context-switch 到运行队列中的第一个就绪任务（kernel_init）。boot task 的意义在于：它代表了 boot CPU 的原始执行上下文。`run_first()` 把当前 CPU 状态保存到 boot task 的上下文中，然后切换到 kernel_init。如果将来所有任务都退出并且调度器需要切回 boot task，它就会进入那个 hlt 循环——虽然这种情况不应该发生。

我们回头看 `Scheduler::run_first()` 的实现，它做了以下几件事：把 boot_task 设为 `current_`，从 RoundRobin 队列中 pick_next（拿到 kernel_init），然后执行 `context_switch(&boot_task->ctx, &kernel_init->ctx)`。这个 context_switch 是用汇编实现的——它保存当前寄存器到 boot_task 的 CpuContext 中，然后从 kernel_init 的 CpuContext 恢复寄存器，最后 ret 跳转到 kernel_init 的入口函数。

### kernel_main 的 idle 循环

```cpp
cinux::lib::kprintf("[BOOT] All tasks exited, entering idle loop.\n");
while (true) {
    Scheduler::yield();
    __asm__ volatile("hlt");
}
```

`run_first()` 不会返回——它通过 context_switch 永久地离开了 kernel_main 的执行流。但如果所有任务都退出了（或者某种路径导致 run_first 返回了），kernel_main 就会进入这个 yield + hlt 循环。`Scheduler::yield()` 会触发一次调度，试图找到其他就绪任务；如果没有，hlt 指令会让 CPU 进入低功耗状态，等待下一次中断唤醒。

### AHCI 实例注册

还有一处关键的修改在 AHCI 初始化部分：

```cpp
if (pci.find_ahci(ahci_dev)) {
    ahci.init(ahci_dev);
    cinux::drivers::ahci::AHCI::set_instance(&ahci);
    // ... MBR read ...
}
```

`AHCI::set_instance(&ahci)` 是新增的调用。它把 AHCI 实例的指针保存到类的静态成员中，这样 `init.cpp` 就可以通过 `AHCI::instance()` 获取这个实例，而不需要直接引用 `main.cpp` 中的局部变量。这是一个经典的 Singleton 模式变体，解决了跨翻译单元的对象访问问题。

### 魔法地址替换为布局常量

在 `kernel_main()` 中，原来的两处硬编码地址也被替换了：

```cpp
// 原来: constexpr uint64_t HEAP_VIRT_BASE = 0xFFFF800000000000ULL;
constexpr uint64_t HEAP_VIRT_BASE = cinux::arch::KMEM_HEAP_BASE;

// 原来: constexpr uint64_t buf_virt = 0xFFFF800000300000ULL;
constexpr uint64_t buf_virt = cinux::arch::KMEM_DMA_BASE;
```

这些替换本身不改变行为——地址值是相同的——但它们消除了魔法数字，让地址的来源可追溯。第三篇文章会详细讲解 `memory_layout.hpp` 的设计。

## 设计决策

### 决策：init 线程 vs 在 kernel_main 中直接启动

**问题**: 是否需要引入独立的 init 内核线程，还是继续在 `kernel_main()` 中完成所有工作？

**本项目的做法**: 引入 `kernel_init_thread()` 内核线程，把文件系统挂载和 shell 启动从 `kernel_main()` 中分离。

**备选方案**: 保持 `kernel_main()` 的线性流程，只在调度器启动后创建一个 shell Task。

**为什么不选备选方案**: 线性流程有几个问题——ext2 挂载和 shell 启动代码在 `kernel_main()` 中变得臃肿；伪造的 shell_task 没有正规的 Task 上下文，导致 `sys_exit` 路径不稳定；未来如果需要 init 线程做更多工作（比如启动更多守护进程），在线性流程中很难扩展。独立线程让 init 的职责清晰，也符合 Linux 的 PID 0/1 模型。

**如果要扩展**: init 线程可以进一步演进为支持 `fork()` + `execve("/sbin/init")` 的完整模式，或者添加 initcall 基础设施来自动注册初始化函数。

## 扩展方向

- 在 init 线程中添加简单的心跳日志，每隔几秒打印一次状态，验证调度器正常工作
- 实现 `kthreadd` 等价物——一个专门管理内核线程生命周期的系统线程
- 给 boot task 添加统计功能：记录空闲时间占比
- 支持 init 线程失败后的 fallback——比如尝试挂载不同的文件系统或启动 rescue shell

## 参考资料

- Linux init/main.c: `rest_init()` → `kernel_init` 线程 — [GitHub](https://github.com/torvalds/linux/blob/master/init/main.c)
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 内核虚拟地址空间布局
- Intel SDM: Vol.3A Section 4.3 — 4-Level Paging, canonical address space
