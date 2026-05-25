---
title: 035-multi-terminal-3 · 多终端
---

# 多终端桌面与 init 重构

> 标签：multi-terminal, GUI, fork+exec, pipe, zombie, stack guard
> 前置：tag 035-2 从 ext2 执行 Shell

## 前言

前面两篇我们把 fork + execve + COW 的底层机制全部打通了，Shell 能从 ext2 加载并在用户态运行。现在到了最激动人心的时刻——让桌面支持多终端。每次点击 Shell 图标就弹出一个独立的终端窗口，每个终端绑定自己的 shell 进程和 pipe 对，互不干扰。这才是真正意义上的多任务操作系统。

在这一篇中我们要做三件事：重构 create_shell_terminal 为每个终端创建独立资源，完善 gui_tick_callback 的多窗口 poll 逻辑，以及实现 Terminal 的生命周期管理（关闭时回收 shell 子进程防止 zombie）。另外还会涉及 init 线程的简化和 stack guard page 机制的完善。

## 环境说明

QEMU x86_64，GUI 模式（CINUX_GUI 宏开启），PS/2 鼠标 + 键盘，1024x768 分辨率 framebuffer。PIT tick 100Hz 驱动 gui_tick_callback。内核栈 16KB，每个终端拥有独立的 stdin/stdout pipe 对。WindowManager 维护一个窗口列表，支持 Z-order 排列和 focus 管理。Terminal 的 screen buffer 是 80x25 的 TerminalCell 数组（每个 cell 包含 char + fg color + bg color，约 24KB）。

## 多终端架构——每个终端都是独立世界

之前所有终端共享一对全局 pipe——gui_init.cpp 中有全局变量 g_stdin_pipe 和 g_stdout_pipe，所有终端的输入输出都经过同一对 pipe。这在单 shell 场景下勉强够用，但多 shell 场景下会串数据——两个 shell 同时写 stdout，poll_output 时分不清哪些数据来自哪个 shell。

新架构中 create_shell_terminal 每次调用都创建独立的资源。首先是 g_terminal_counter 自增生成唯一标题（"Shell #1"、"Shell #2"...），然后 new Terminal 创建终端窗口对象。接着创建两组 pipe：stdin pipe（Terminal.on_key() 写入，shell 从 fd 0 读取）和 stdout pipe（shell 向 fd 1 写入，Terminal.poll_output() 读取）。每个 pipe 需要包装成 Inode 才能放进 FDTable——stdin pipe 的 read 端用 PipeReadOps 包装成 stdin_read_inode，stdout pipe 的 write 端用 PipeWriteOps 包装成 stdout_write_inode。两个 inode 的 type 都设为 InodeType::Regular。

父进程把 pipe 对象直接绑到 Terminal 上：set_stdin_pipe(stdin_pipe) 让 on_key() 能把键盘字符写入 pipe，set_stdout_pipe(stdout_pipe) 让 poll_output() 能从 pipe 读取 shell 输出。Terminal 还通过 set_shell_pid 记录子进程 PID——析构时用来 waitpid 回收。

fork 后子进程创建私有 FDTable：fd 0 绑 stdin pipe 的 read inode（通过 cinux::fs::OpenFlags::RDONLY 打开），fd 1 绑 stdout pipe 的 write inode（通过 cinux::fs::OpenFlags::WRONLY 打开）。这样每个 shell 有自己独立的 I/O 通道——输入来自自己终端的键盘事件，输出到自己的终端窗口，不会和其他 shell 串数据。

和 Linux 对比：Linux 的多终端通过 init -> fork -> getty -> exec -> login -> fork -> exec shell 的链路实现，每个终端对应一个 tty 设备（/dev/tty1, tty2...）。getty 进程打开对应的 tty 设备作为 stdin/stdout/stderr。Cinux 的 pipe 扮演了类似 tty 的角色——但更简化，没有 termios（终端属性控制）和 job control。

SerenityOS 的终端（Terminal.app）是一个独立的用户进程，通过 PTY（pseudo-terminal）与 shell 进程通信。PTY 由内核管理，提供 bidirectional byte stream + 终端属性控制。Cinux 的 pipe 在功能上类似 PTY 但没有 slave/master 之分——pipe 是单向的，需要两个 pipe 组成双向通道。

## gui_tick_callback——不只 poll focused 窗口

之前的 tick callback 只 poll focused 窗口的输出。具体来说，旧代码是 `auto* focused = wm.focused(); if (focused && focused->is_terminal()) { focused->poll_output(); }`。多终端场景下每个 shell 都在后台往自己的 stdout pipe 写数据，如果只 poll 一个窗口，其他 shell 的输出就堆积在 pipe buffer 里不显示。

新逻辑：遍历 WindowManager 的所有窗口（通过 wm.window_count() 和 wm.window_at(i)），对每个 is_terminal() 的窗口调用 poll_output() + render_to_canvas()。这放在 composite() 之前，确保 compositing 时数据是最新的。poll_output 从 stdout pipe 非阻塞读取可用数据（try_read），写入 Terminal 的 screen buffer；render_to_canvas 把 screen buffer 渲染到后备缓冲区。

这里有个设计取舍值得讨论。poll 所有窗口的开销是 O(n)（n = 窗口数量），每个窗口的 poll_output 调用 try_read（非阻塞），如果 pipe 为空立即返回。所以即使有很多窗口，开销也很小——只在有数据时才做实际工作。Linux 的 tty poll 机制类似——poll 系统调用检查 fd 的可读/可写状态，有数据才唤醒进程。

tick callback 还处理 deferred action。旧的实现是检测到 IconAction::OpenShell 后直接调用 create_shell_terminal()。但在 PIT tick 的 Interrupt gate 上下文中直接 fork 会死锁（因为 fork 需要拿 PMM/VMM 的锁，而 IF=0 下拿锁不安全）。所以改用 std::atomic<IconAction> g_pending_action 存储 tick 中检测到的图标点击事件，gui_worker 线程通过 gui_process_pending() 取出并执行。gui_process_pending 用 exchange(IconAction::None, memory_order_acq_rel) 原子地取出并清空 pending action。

## Terminal 析构——关闭 pipe + 回收 zombie

Terminal 被销毁时（通常是窗口管理器 remove_window 后 delete），需要做两件事：关闭 pipe 端点让 shell 知道终端没了，以及 waitpid 回收 shell 子进程防止 zombie。

关闭 pipe 的方向必须正确。Terminal 是 stdin pipe 的 writer（on_key 把键盘字符写入 pipe -> shell 从 fd 0 读取），所以关闭 writer 端——stdin_pipe_->close_writer() 让 shell 的 sys_read 返回 0（EOF），shell 通常会因此退出。Terminal 是 stdout pipe 的 reader（shell 向 fd 1 写入 -> poll_output 从 pipe 读取），所以关闭 reader 端——stdout_pipe_->close_reader() 让 shell 的 sys_write 失败，shell 也会因此退出。关闭后两个指针置 nullptr，防止重复关闭。

waitpid 用循环重试（最多 1000 次）——因为 waitpid 是非阻塞的，子进程没退出就返回 WaitpidResult::NotExited。pipe 关闭后 shell 通常很快退出变成 Zombie，循环很快就能回收。如果 waitpid 返回 NoChildren 或 NotFound（比如子进程已经被别的路径回收了），直接 break。但 1000 次上限防止 shell 卡住时无限循环。shell_pid_ 最后置 0。

这里有个实际踩坑经验值得分享。Terminal::screen_[80][25] 的 TerminalCell 数组约 24KB，加上 Pipe buffer 约 32KB，远超 16KB 内核栈。如果在栈上分配 Terminal 对象，stack guard page 检测立刻捕获溢出——之前没有 guard page 时，栈溢出静默踩坏相邻内存，debug 起来非常痛苦。所以 Terminal 必须用 new 在堆上分配。同样，create_shell_terminal 中有多个 pipe 和 inode 对象也是用 new 分配的——总内存量超过栈容量。

## init 线程简化——pipe 职责下沉

之前的 init 线程在 gui_start() 前创建全局 pipe 对（stdin_pipe、stdout_pipe、对应的 PipeReadOps/PipeWriteOps/Inode），设置 global fd table 的 fd 0 和 fd 1，然后通过 set_shell_pipes() 接口传给 GUI 子系统。这个设计在多终端场景下不再适用——每个终端需要独立的 pipe，全局 pipe 只能服务一个 shell。

重构后 init 线程在 GUI 模式下只做三件事。第一，挂载 ext2 + VFS。第二，启动 GUI（gui_start 注册图标和 tick callback）。第三，创建 gui_worker 线程（TaskBuilder 设置 entry 为 gui_worker_thread，name 为 "gui_worker"），然后 Scheduler::add_task 添加到调度队列。gui_worker_thread 是一个无限循环，不断调用 gui_process_pending() 然后 yield。

pipe 创建和 fork+execve 的职责完全下沉到 create_shell_terminal()，init 不再关心。set_shell_pipes() 函数被删除，g_stdin_pipe 和 g_stdout_pipe 全局变量被删除，相关的头文件引用也被清理。gui_init.hpp 中不再需要前置声明 cinux::ipc::Pipe。

非 GUI 模式保留了直接 fork+execve 的路径，用 legacy sys_read（keyboard polling）和 sys_write（kprintf serial+console）代替 pipe。非 GUI 模式下没有 FDTable 和 pipe 的概念——shell 直接用内核的 global fd table。这个路径在 init.cpp 中用 #else 分支实现，和 GUI 模式共享 cli -> AddressSpace -> execve -> user stack -> activate -> jump_to_usermode 的核心流程。

## Stack Guard Page——栈溢出的"保险丝"

每个内核栈下面有一个 guard page（4KB，故意不映射）。栈溢出时写入 guard page 触发 #PF，handler 检查 fault 地址是否在 guard 范围内，如果是就报告 "STACK OVERFLOW" 然后 panic。

#PF handler 有两层检测。第一层是 scheduler task 的 guard page：通过 cur->kernel_stack_guard_page 字段获取 guard 基地址，检查 fault_addr 是否在 [guard_base, guard_base + PAGE_SIZE) 范围内。如果是，打印详细诊断信息（task 名称、tid、pid、fault 地址、guard 范围、栈范围、当前 RSP、RIP）然后 kpanic。

第二层是 boot stack 的 guard 区域：当 cur == nullptr（没有 scheduler task，说明运行在 boot stack 上）时，检查 fault_addr 是否落在 __boot_guard_start 和 __boot_guard_end 之间（由 linker script 定义）。boot stack 在运行测试时使用，guard 区域在 linker script 中定义为 64KB（16 页），但需要在测试启动时显式 unmap——因为 boot stack 用 2MB huge page 映射，整个 2MB 都可访问，guard 区域只是 linker script 里留了空间但并没有自动 unmap。

TaskBuilder::build() 和 fork() 都为子进程分配 guard page：调用 alloc_stack_vaddr(STACK_PAGES + 1) 多分配一个虚拟页作为 guard，但不映射。这个 guard 页的虚拟地址保存在 task->kernel_stack_guard_page 中。linker.ld 也被修改：在 BSS 和 boot stack 之间插入了 .boot_guard section（64KB，NOLOAD），导出 __boot_guard_start 和 __boot_guard_end 符号。

Linux 的 kernel stack guard page 也是类似机制——每个内核栈底部有一个未映射的 guard page。Linux 还使用 STACKLEAK 插件在函数返回时擦除栈内容，防止信息泄露。

## 收尾

到这里 tag 035 全部完成。Cinux 桌面现在拥有真正的多任务能力——点击 Shell 图标，弹出终端，每个终端运行独立的 shell 进程。fork + execve + COW + pipe + GUI 这几个子系统协同工作，构成了一个虽然简单但功能完整的多任务操作系统。

两个 tag（034 + 035）一共新增了近一万行代码，引入了 ELF 解析、PID 分配、fork with CoW、execve from ext2、waitpid zombie 回收、GS MSR 保存/恢复、多终端桌面等核心功能。这是 Cinux 项目的一个重要里程碑——从一个"能跑 GUI 的内核"进化为一个"能管理多进程的操作系统"。

## 参考资料

- Linux PTY: https://man7.org/linux/man-pages/man7/pty.7.html
- SerenityOS Terminal: https://github.com/SerenityOS/serenity/tree/master/Userland/Applications/Terminal
- Linux kernel stack guard: https://lwn.net/Articles/692386/
- OSDev Wiki Interrupt Stack Table: https://wiki.osdev.org/Task_State_Segment
- xv6 init.c: https://github.com/mit-pdos/xv6-riscv/blob/riscv/user/init.c
