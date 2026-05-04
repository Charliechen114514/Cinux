# 从 kernel_main 到 init 线程——内核启动架构重构

> 标签：init thread, scheduler bootstrap, kernel thread, PID 0/1
> 前置：[028d 同步安全](028d-sync-safety-3.md)

## 导语

到目前为止，我们的 `kernel_main()` 一直是「一条路走到黑」的单线程初始化流程——从串口、GDT、IDT 一直初始化到 AHCI、ext2、VFS，最后直接跳进用户态。说实话，这个方案在前面的 tag 里跑得还行，但随着调度器和进程系统越来越完善，我们逐渐发现一个严重的架构问题：所有 post-scheduler 的工作（文件系统挂载、shell 启动）都塞在 `kernel_main()` 或者 `run_concurrent_stress()` 里面，而此时调度器已经启动了——这意味着这些操作并不是在一个正规的 Task 上下文中执行的。

本章我们要做一件在 OS 开发中非常经典的事情：把 `kernel_main()` 的后期工作拆出来，放进一个独立的内核线程——我们叫它 `kernel_init_thread`，也就是 Linux 里的 `kernel_init` 的 Cinux 版本。完成这一步之后，我们的启动流程会变成：boot CPU 上下文变成 boot task（类似 Linux PID 0），init 线程负责文件系统和 shell（类似 PID 1），然后 kernel_main 进入 yield+hlt 空闲循环。这种分离是任何一个正经操作系统都会做的——让内核初始化和进程管理各司其职。

## 概念精讲

### 为什么要引入 init 线程

在之前的 tag 中，我们的启动流程大概是这样的：`kernel_main()` 把所有硬件和子系统初始化完，然后调用 `run_concurrent_stress()`——这个函数会启动调度器、创建一堆压力测试线程、等它们跑完，最后调用 `launch_first_user()` 跳进用户态。问题在于，`launch_first_user()` 会创建一个假的 `static Task shell_task{}`，一个零初始化的 Task 结构体——没有内核栈、tid 是 0、不在任何运行队列里。这就像你伪造了一张身份证去银行办事，一开始看着没问题，但当系统尝试用这张身份证做一些需要真实身份信息的操作时，就会全线崩溃。

更深层的问题在于：我们的调度器设计假设所有正在运行的代码都有一个有效的 Task 上下文。当 `sys_exit` 被调用时，它会通过 `Scheduler::current()` 获取当前 Task 并将其标记为 Dead——但那个伪造的 shell_task 不在运行队列中，调度器根本不知道它的存在。如果调度器试图切回这个 Task，它会发现没有有效的 CPU 上下文，然后你就收获一个漂亮的 kernel panic。

### Linux 的 init 模型

我们来看一下 Linux 是怎么做的。Linux 的 `start_kernel()` 相当于我们的 `kernel_main()`，它在单线程环境下完成所有早期硬件初始化。在 `start_kernel()` 的末尾，`rest_init()` 被调用，它做了三件事：首先，spawn `kernel_init` 内核线程（这将成为 PID 1）；其次，spawn `kthreadd` 内核线程（PID 2，负责管理后续所有内核线程）；最后，boot CPU 进入 idle 循环（PID 0）。`kernel_init` 线程负责完成剩余的驱动初始化、挂载根文件系统，然后尝试 exec `/sbin/init`（或 systemd）进入用户态。

这个模型的核心思想是：内核初始化分为「单线程早期初始化」和「可调度的后期初始化」两个阶段。早期初始化负责那些必须在 scheduler 运行前完成的工作（内存管理、中断控制器），后期初始化可以在 scheduler 管理下进行，享受抢占、同步等好处。我们现在要做的，就是把 Cinux 的后期初始化搬进一个 init 内核线程。

### Boot Task 的角色

在我们的设计中，`kernel_main()` 在调度器启动后创建一个 boot task，然后调用 `Scheduler::run_first(boot_task)` 开始调度。这个 boot task 的入口函数是一个简单的 halt 循环——理论上它永远不应该被执行到，因为 `run_first` 会立即 context-switch 到 kernel_init。boot task 的角色类似 Linux 的 PID 0 idle task：它是一个「占位符」，代表 boot CPU 的原始执行上下文被正式纳入调度器的管理之下。

## 动手实现

### Step 1: 创建 init 线程声明

**目标**: 新建一个头文件，声明 init 线程的入口函数。

**设计思路**: init 线程的入口函数需要是一个无参数、无返回值的函数，和 `TaskBuilder::set_entry()` 接受的函数签名一致。我们把它放在 `kernel/proc/` 目录下，和调度器、进程管理代码放在一起——因为 init 线程本质上就是进程系统的一部分。

**实现约束**: 头文件只需要一个简单的命名空间声明，不需要包含其他头文件。函数签名为 void(void)。

**验证**: 头文件创建后，暂时无法单独验证。等后续步骤完成后一起编译。

### Step 2: 重构 kernel_main——调度器启动与线程创建

**目标**: 修改 `kernel_main()` 的尾部，使其启动调度器、创建 init 线程和 boot task，然后进入 idle 循环。

**设计思路**: 原来的 `kernel_main()` 在 AHCI 初始化之后直接做 ext2 挂载、VFS 注册、shell 启动。现在这些工作全部交给 init 线程。`kernel_main()` 的新职责变成了：初始化调度器，用 `TaskBuilder` 创建 kernel_init 线程并添加到运行队列，创建 boot task 并调用 `Scheduler::run_first()` 启动调度。

`Scheduler::run_first()` 的语义很重要——它会把当前 CPU 上下文保存到 boot task 中，然后 context-switch 到运行队列中的第一个就绪任务（也就是 kernel_init）。从此以后，kernel_init 就在调度器的管理下运行了，而 boot CPU 的原始上下文变成了一个「休眠」的 Task。如果所有任务都退出了，`kernel_main()` 的最后会进入一个 yield + hlt 的空闲循环。

**实现约束**: 需要注意以下几点——`Scheduler::init()` 必须在创建任何 Task 之前调用；`TaskBuilder` 的 `set_entry` 接受一个函数指针；`Scheduler::add_task` 把任务加入默认调度类的运行队列；`run_first` 启动第一次调度；boot task 的入口应该是一个死循环 hlt。

**踩坑预警**: 如果你忘了 `Scheduler::init()` 直接创建 Task，会在 TaskBuilder::build 里因为空指针崩溃。另外，boot task 的 lambda 捕获要小心——不要捕获任何栈变量，因为 boot task 的 lambda 可能在 `kernel_main` 返回很久之后才被执行到（虽然理论上不应该被执行到）。

**验证**: 构建并运行内核，检查串口输出。你应该看到类似这样的序列：
```
[BIG] ===== Scheduler & Init Thread =====
[SCHED] Idle task created tid=1
[SCHED] Scheduler initialised with RoundRobin class
[PROC] Created task tid=2 name='kernel_init' stack=0x...
[SCHED] Task tid=2 'kernel_init' added to RoundRobin
[PROC] Created task tid=3 name='boot' stack=0x...
```

### Step 3: 删除旧的 stress test

**目标**: 移除 `kernel/stress/stress_test.cpp` 和整个 `kernel/stress/` 目录，同时更新 CMakeLists.txt。

**设计思路**: stress test 是 028d 为了验证同步安全而写的临时测试代码。现在 init 线程模型已经就位，压力测试的职责完全可以由独立的测试程序来承担，不需要在内核启动流程中硬编码。删除 stress test 也简化了启动流程——从「先跑压力测试再启动 shell」变成了「直接启动 shell」。

**实现约束**: CMakeLists.txt 中需要移除 stress_test.cpp 的编译项。同时删除 `kernel_main()` 中对 `run_concurrent_stress()` 的外部声明和调用。确认没有任何其他文件引用 stress test 的函数或变量。

**验证**: 编译通过，链接没有报未定义引用。运行时不再看到 `[STRESS]` 相关的串口输出。

### Step 4: 实现 init 线程的框架

**目标**: 在 `kernel/proc/init.cpp` 中实现 `kernel_init_thread()` 的基本框架。

**设计思路**: init 线程的核心工作有三步：挂载 ext2 文件系统、注册 VFS、启动第一个用户态程序。这三个步骤之前在 `kernel_main()` 中是直接调用的，现在只需要把它们搬到 init 线程里就行——但有一个关键区别：AHCI 实例不再是 `kernel_main()` 里的局部变量，而是通过 `AHCI::instance()` 单例获取。

**实现约束**: init 线程需要包含 AHCI、ext2、VFS、usermode 等头文件。ext2 实例声明为 static 局部变量（和原来在 `kernel_main` 中一样）。挂载失败时打印错误日志但不崩溃——后续的 shell 启动可能会失败，但至少不会 kernel panic。init 线程的末尾调用 `Scheduler::exit_current()` 正常退出。

**踩坑预警**: ext2 构造函数接受一个 AHCI 引用和端口号。使用 `AHCI::instance()` 返回引用时，确保 `set_instance()` 已经在 `kernel_main()` 中被调用过了。如果 init 线程跑得太快（在 `set_instance` 之前就被调度到了），你会得到一个空指针解引用。不过在我们的实现中，`set_instance()` 在 `Scheduler::init()` 之前就完成了，所以不会有这个问题。

**验证**: 运行内核，检查串口输出中 `[INIT]` 前缀的日志。你应该看到 init 线程启动、ext2 挂载、VFS 初始化的完整序列。

## 构建与运行

构建命令和之前一样：
```
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

运行 QEMU：
```
./run.sh
```

或者直接：
```
qemu-system-x86_64 -cdrom cinux.iso -serial stdio -no-reboot
```

## 调试技巧

**最常见的问题：调度器启动后无输出**。如果 `Scheduler::run_first()` 之后什么日志都看不到，最可能的原因是 init 线程的入口函数指针为空或者 TaskBuilder::build 返回了 nullptr。检查 `TaskBuilder::set_entry()` 是否正确传入了函数指针。

**第二个常见问题：init 线程中 AHCI 操作超时**。如果看到 `[AHCI] Port X: command timeout`，那很可能是 MMIO 地址被覆盖了——这是本 tag 最关键的 bug，我们会在第三篇文章中详细讨论。暂时可以确认 `memory_layout.hpp` 中的各区域地址没有重叠。

**用串口辅助调试**：所有 `[INIT]`、`[SCHED]`、`[PROC]` 前缀的日志都会输出到串口。如果 QEMU 窗口卡住了但串口还在输出，说明调度器在正常工作但显示可能有问题。如果串口也停了，大概率是 triple fault——用 `-d int` 参数运行 QEMU 查看异常信息。

## 本章小结

| 概念 | 说明 |
|------|------|
| init 线程 | 类似 Linux PID 1，负责 post-scheduler 初始化 |
| boot task | 类似 Linux PID 0 idle，boot CPU 上下文的占位符 |
| `Scheduler::run_first()` | 从 boot task context-switch 到第一个就绪任务 |
| `Scheduler::exit_current()` | init 线程完成后的正常退出 |
| `AHCI::instance()` | 单例模式，解决跨翻译单元访问问题 |

到这里，我们完成了内核启动架构的核心重构。但 init 线程目前还只是个框架——它里面 ext2 挂载和 shell 启动的具体细节，以及 `launch_first_user()` 的 shell_task 修复，我们留到下一章来完成。
