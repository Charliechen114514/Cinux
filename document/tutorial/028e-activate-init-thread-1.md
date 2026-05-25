---
title: 028e-activate-init-thread-1 · Init 线程
---

# 为什么要引入 init 线程？——从 kernel_main 的膨胀说起

> 标签：init thread, kernel_main, scheduler bootstrap, PID 0/1, Linux comparison
> 前置：[028d 同步安全——全局状态审计与 Spinlock 保护](028d-sync-safety-3.md)

## 前言

说实话，当我们的 `kernel_main()` 从 Step 1 一路涨到 Step 22+ 的时候，我就知道这一天迟早要来。这个函数从串口初始化开始，经过 GDT、IDT、PIC、PMM、VMM、Heap、Framebuffer、Console、Keyboard、PCI、AHCI、ext2、VFS，最后直接跳进用户态——一条近 300 行的线性函数，承载了内核从上电到 shell 启动的全部工作。这在一个教学 OS 里或许还能忍受，但问题是，当调度器加入之后，事情开始变得不对劲了。

在 028d 中，我们添加了 Spinlock 和 InterruptGuard 来保护全局状态，并且写了一个 `run_concurrent_stress()` 来验证并发安全性。但这个函数实际上是在 `kernel_main()` 中被调用的——它启动调度器、跑压力测试、然后调用 `launch_first_user()`。问题在于，`launch_first_user()` 里面伪造了一个 `static Task shell_task{}`，一个零初始化的空壳 Task，用来骗过调度器的 `current()` 查询。这个 hack 在早期 shell 只读不退出的场景下能凑合，但当 shell 开始使用 `sys_exit` 系统调用时，这个伪造的 Task 就成了一个定时炸弹——没有内核栈、不在运行队列、CPU 上下文全是零。

我们现在要做的事情，在 OS 开发中有一个非常经典的名称——「引入 init 线程」。这不是什么新发明，Linux 在二十多年前就这么做了。核心思想是：把 `kernel_main()` 分为「早期初始化」（硬件、内存、中断——必须在调度器启动前完成）和「后期初始化」（文件系统、shell——可以在调度器管理下运行）两个阶段，后期初始化交给一个独立的内核线程来完成。

## 环境说明

我们的工作环境一如既往：x86_64 QEMU 虚拟机，工具链是 GCC 14 + CMake，内核运行在 higher-half（`0xFFFFFFFF80000000`），虚拟地址空间的高半部分（`0xFFFF8000xxxxxxxx`）用于内核堆、MMIO、栈等区域。本 tag 的改动涉及 `kernel/main.cpp`、新建 `kernel/proc/init.cpp` 和 `kernel/arch/x86_64/memory_layout.hpp`，以及删除整个 `kernel/stress/` 目录。

## 第一步——拆解 kernel_main 的尾部

我们回头看 `kernel_main()` 的原始结构。在 AHCI 初始化（Step 21）之后，原来的代码做了三件事：

```cpp
// 原来的 kernel_main 尾部
static cinux::fs::Ext2 ext2(ahci, 1);
if (!ext2.mount()) { ... }
cinux::fs::vfs_mount_init();
cinux::fs::vfs_mount_add("/", &ext2);
run_concurrent_stress();  // 内部启动调度器，跑压力测试，然后 launch_first_user
```

这段代码的问题不在于它做了什么，而在于它在什么上下文中做。`run_concurrent_stress()` 内部调用 `Scheduler::init()`，启动了抢占式调度——但从 `kernel_main()` 的视角看，它只是一个普通函数调用，返回后继续执行下面的键盘轮询循环。调度器认为当前任务已经在运行了，但实际上没有任何有效的 Task 结构代表 `kernel_main()` 的执行上下文。

现在我们把 `kernel_main()` 的尾部改成这样：

```cpp
// 新的 kernel_main 尾部
cinux::lib::kprintf("[BIG] ===== Scheduler & Init Thread =====\n");
Scheduler::init();

auto* init_task =
    TaskBuilder().set_entry(cinux::proc::kernel_init_thread).set_name("kernel_init").build();
if (init_task != nullptr) {
    Scheduler::add_task(init_task);
}

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

// 如果所有任务都退出了，进入空闲循环
while (true) {
    Scheduler::yield();
    __asm__ volatile("hlt");
}
```

这三段代码的含义非常清晰。`Scheduler::init()` 创建 idle task 并注册 RoundRobin 调度类。`TaskBuilder` 创建 init 线程（入口是 `kernel_init_thread`）和 boot task（入口是一个永远不会被执行到的 hlt 循环）。`run_first(boot_task)` 是整个启动流程的关键——它把 boot CPU 的当前上下文保存到 boot task 中，然后 context-switch 到运行队列中的第一个就绪任务（kernel_init）。从此以后，init 线程在调度器的管理下运行，而 boot CPU 的原始执行流变成了一个「休眠」的 Task。

这种 boot task + init thread 的架构直接对应了 Linux 的 PID 0 和 PID 1 模型。在 Linux 中，`start_kernel()` 完成单线程初始化后，`rest_init()` 创建 `kernel_init` 线程（PID 1）和 `kthreadd`（PID 2），boot CPU 进入 `cpu_startup_entry()` idle 循环（PID 0）。我们的 Cinux 版本简化了——没有 kthreadd，init 线程直接做所有后期工作——但核心思想是一致的。

## 第二步——理解 run_first 的调度启动机制

`Scheduler::run_first()` 是一个特殊的调度操作——它不是 yield（从一个 Task 切到另一个 Task），而是从「没有调度器」到「有调度器」的过渡。它的实现值得仔细看看：

```cpp
void Scheduler::run_first(Task* boot_task) {
    current_          = boot_task;
    g_per_cpu.current = boot_task;
    cinux::arch::GDT::tss_set_rsp0(boot_task->kernel_stack_top);
    current_slice_.store(0, std::memory_order_relaxed);

    Task* next = default_rr_.pick_next();  // 从队列中取出 kernel_init
    // ... 设置 current_ = next ...
    // ... 保存 boot_task FPU，切换地址空间 ...
    context_switch(&boot_task->ctx, &next->ctx);
    // 不会返回——执行流已经跳转到 kernel_init_thread
}
```

`context_switch` 是汇编实现的——它把当前所有 callee-saved 寄存器（rbx、rbp、r12-r15、rsp、rip）保存到 `boot_task->ctx` 中，然后从 `next->ctx`（kernel_init）恢复寄存器。当它 `ret` 的时候，RIP 已经指向了 `kernel_init_thread()` 的入口地址，RSP 指向了 init 线程的内核栈顶。从此，init 线程就开始执行了。

这里有一个微妙但重要的点：`context_switch` 之后的代码（`fxrstor` 那行）对于 boot_task 来说不会执行——因为执行流已经跳走了。但这行代码在 `run_first` 返回路径上是有意义的：当将来某个时刻调度器切回 boot_task 时，`context_switch` 会返回到 `fxrstor` 这行，恢复 boot_task 的 FPU 状态，然后 `run_first` 返回到 `kernel_main()` 的 while 循环中。

## 与其他 OS 的对比

### Linux 的 init 模型

Linux 的启动流程是 `start_kernel()` → `rest_init()` → `kernel_init` 线程。`kernel_init` 线程做了很多 Cinux 不需要做的事情——initcall 遍历（调用所有 `__init` 函数）、设备模型初始化、workqueue 创建、sysfs 挂载等。然后在最后阶段，`kernel_init` 尝试 exec 一系列 init 程序（`/sbin/init`、`/etc/init`、`/bin/init`、`/bin/sh`），如果全部失败就 kernel panic。

Cinux 的 init 线程远比 Linux 简单——没有 initcall 基础设施，没有设备模型，只有一个 ext2 挂载、VFS 注册、shell 启动的三步流程。但设计思想是相同的：早期初始化在单线程环境下完成（不需要锁），后期初始化在调度器管理下进行（享受抢占和同步的好处）。

### xv6 的第一进程创建

xv6 采取了完全不同的路线。它没有「内核 init 线程」的概念——`main()` 初始化完所有子系统后，直接调用 `userinit()` 手动创建第一个用户进程。`userinit()` 分配一个 `struct proc`，为其创建页表，把 `/init` 二进制的 ELF 内容复制进去，然后设置 `p->context->eip = forkret`。调度器第一次运行这个进程时，`forkret` 被调用，它初始化文件系统然后返回——通过 `trapret` 跳转到用户空间。

xv6 这种设计很巧妙（复用了 fork 返回路径），但也有局限：文件系统初始化发生在第一个进程的上下文中，而不是在独立的内核线程中。这意味着如果文件系统初始化需要阻塞（比如等待磁盘 I/O），整个系统就卡住了——因为此时只有一个进程在运行，没有其他进程可以继续执行。Cinux 使用独立的 init 内核线程避免了这个问题——init 线程在调度器管理下运行，即使它阻塞了，其他 Task（idle task、boot task）仍然可以被调度。

## 收尾

到这里，我们已经完成了 `kernel_main()` 的核心重构——调度器启动、init 线程创建、boot task 占位。init 线程的入口 `kernel_init_thread()` 目前还是空的，它的具体实现（ext2 挂载、VFS 注册、shell 启动）以及 `launch_first_user()` 中的 shell_task 修复，我们留到下一篇来完成。

**验证方式**：构建并运行内核，检查串口输出中是否出现了 `[BIG] ===== Scheduler & Init Thread =====` 以及 `[SCHED]` 和 `[PROC]` 系列日志。如果看到了 `[INIT] kernel_init started`，说明 init 线程已经被正确调度。

## 参考资料

- Linux init/main.c: `rest_init()` 和 `kernel_init()` — [GitHub](https://github.com/torvalds/linux/blob/master/init/main.c)
- xv6 proc.c: `userinit()` — [GitHub](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c)
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 内核启动后的虚拟地址空间设计
- Intel SDM: Vol.3A Section 4.5 — 4-Level Paging and canonical address space
