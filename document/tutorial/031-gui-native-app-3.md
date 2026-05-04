# 让 Shell 跑在窗口里 — Cinux GUI 进程架构全解析

> 标签：fork, execve, pipe, GUI, 进程架构, 系统调用路由
> 前置：[031-2 打造 GUI 文本终端](031-gui-native-app-2.md)

## 前言

管道搭好了，终端组件也写完了，现在到了整个 tag 最激动人心的时刻——把所有东西串起来。当用户点击 GUI 桌面上的 Shell 图标时，系统需要创建一个终端窗口、建立两对管道、fork 出一个子进程、把管道的读写端安装到子进程的 fd 0 和 fd 1、execve 加载 /bin/sh、然后跳转到用户态开始执行。shell 进程完全不知道自己的 stdin 和 stdout 连的是管道——它只管读写 fd 0 和 fd 1，内核的路由机制会自动把数据送到正确的地方。

这一章涉及的内容比较分散——FDTable 扩展、sys_read/sys_write 重构、GUI 启动流程、PIT tick 回调轮询——但它们共同解决了一个问题：让 shell 的标准 I/O 不再绑定串口，而是通过管道连接到 GUI 终端窗口。当这一切完成后，Cinux 就不再是一个"内核跑在 QEMU 串口里"的教学项目，而是一个"有图形界面、能在窗口里打字"的操作系统。

## 环境说明

本章基于 tag 031，修改涉及 `kernel/fs/`（FDTable 扩展）、`kernel/syscall/`（sys_read/sys_write 重构）、`kernel/gui/`（gui_init、window_manager）三个目录。Cinux 使用自写的 fork/execve 实现，子进程创建后需要手动设置 AddressSpace 和 FDTable。PIT 频率约 100Hz，每次 tick 执行事件处理和终端轮询。QEMU 作为运行和调试环境，串口输出用于观察 fork/execve 流程和管道数据传输。

## 第一步——sys_read/sys_write 路由重构

在引入管道之前，sys_read 对 fd=0 直接从键盘缓冲区读取，sys_write 对 fd=1 直接用 kprintf 输出到串口。这种硬编码的方式在只有串口终端的时候没问题——fd 0 就是键盘，fd 1 就是串口，一对一映射，简单粗暴。但现在 shell 进程的 fd 0 和 fd 1 指向管道 inode，我们需要让系统调用走到管道路径。

重构方案非常优雅——在处理具体 fd 编号之前，先查当前进程的 FDTable。如果 fd 对应的位置有一个 File 条目，而且这个 File 的 inode 有有效的 InodeOps，就直接调用 ops->read 或 ops->write。管道的 PipeReadOps/PipeWriteOps 会在这一步被命中。如果 FDTable 里没有对应条目，才回退到传统的硬编码路径。

这个设计的好处是：对用户态程序完全透明。Shell 调用 `sys_read(0, buf, 256)` 时，它不知道 fd 0 背后是管道还是键盘——路由由内核根据 FDTable 的内容自动决定。未来如果我们加入 socket、设备文件等新的文件类型，也不需要改 sys_read/sys_write 的代码，只需在 FDTable 中安装对应的 InodeOps 就行。这就是多态分发的威力——新增类型不需要修改已有代码。

### 和其他 OS 的对比

xv6 的 sys_read/sys_write 使用了不同的路由方式。它的 `struct file` 有一个 `type` 字段（FD_NONE、FD_PIPE、FD_INODE、FD_DEVICE），系统调用代码根据 type 分发到不同的处理函数。比如 sys_read 检查 `f->type == FD_PIPE` 就调用 piperead，`f->type == FD_INODE` 就调用 fileread。这种方式更简单直观，但扩展性差——每增加一种文件类型都要在 sys_read/sys_write 里加 case 分支。

Linux 内核的做法更接近 Cinux——通过 `struct file_operations` 函数指针表实现多态分发。系统调用代码只调用 `file->f_op->read_iter` 或 `file->f_op->write_iter`，不需要知道底层是什么类型的文件。如果 f_op 是 pipefifo_fops 就走管道路径，是 ext4_file_operations 就走 ext4 文件路径。Cinux 的 InodeOps 本质上就是 Linux 的 file_operations 的简化版——我们只有 read 和 write 两个方法，Linux 有几十个。

SerenityOS 也使用类似的多态分发模式——它的 `FileDescription` 持有一个 `Inode` 引用，`Inode` 上有 `read` 和 `write` 虚函数。SerenityOS 的终端组件（Terminal.app）通过 `pts`（伪终端）设备连接 shell，比 Cinux 的直接管道连接更接近 POSIX 标准的实现——pty 提供了行编辑缓冲区、信号传递（SIGINT/SIGPIPE）、窗口大小通知（SIGWINCH）等功能。

## 第二步——扩展 FDTable

原来的 FDTable 只有 alloc 和 close 方法——alloc 从 fd=3 开始搜索第一个空闲槽位（0/1/2 保留），close 释放指定 fd 的 File 对象。但 shell 终端初始化需要在 fd 0 和 fd 1 安装管道 inode（这是 POSIX 标准的 stdin/stdout 编号），所以我们需要一个 set 方法来强制在指定位置放入 File 对象。

set 的实现很简单——验证 fd 范围（0 到 FD_TABLE_SIZE-1）后在自旋锁保护下直接替换对应槽位的指针。需要注意的是 set 不负责释放之前槽位上的 File——调用方需要在调用前处理。在 shell 子进程的场景中这不是问题，因为子进程的 FDTable 是刚创建的（所有槽位都是 nullptr），set 只是往空槽位里放 File。

在未来的 dup2 系统调用中，set 方法也会派上用场——dup2(old_fd, new_fd) 的语义就是把 new_fd 重定向到 old_fd 指向的同一个 File。实现方式是先 close(new_fd) 再 set(new_fd, get(old_fd))。这是 Unix shell 实现管道操作符（`|`）和 I/O 重定向（`<`, `>`）的基础。

## 第三步——create_shell_terminal 启动流程

这是整个 tag 中最复杂的函数，也是最容易出 bug 的地方。它做了五件事：创建 Terminal 窗口、创建管道、fork 子进程、在子进程中设置 fd 和 execve shell、在父进程中注册窗口。整个函数约 150 行，涉及进程管理、内存管理、文件系统、GUI 四个子系统的协调。

管道的方向设计需要特别注意。我们有两个管道，每个管道有两个端。stdin 管道的方向是"终端 -> shell"：终端的 on_key 往里面写（try_write），shell 的 sys_read 从里面读。所以 stdin 管道的读端（PipeReadOps）要安装到 shell 的 fd 0，写端通过 pipe 指针直接给 Terminal。stdout 管道方向相反——shell 的 sys_write 往里面写（通过 PipeWriteOps），终端的 poll_output 从里面读（通过 pipe 指针 try_read）。

这个方向设计的关键在于：shell 的 fd 0 需要的是 PipeReadOps（只读），fd 1 需要的是 PipeWriteOps（只写）。如果你搞反了——把 PipeWriteOps 给 fd 0——shell 的 sys_read 就会调到 PipeWriteOps 的 read 方法，它直接返回 -1，shell 收到错误就退出了。这类方向错误在调试时很难定位，因为症状只是"shell 没有输出"而不是"系统崩溃"。

fork 之后的子进程路径有几个关键操作。首先是 cli 关中断——fork 后子进程的中断状态不确定，在设置新地址空间之前必须关闭中断防止 PIT 在不完整的 CR3 上触发。然后是创建新的 AddressSpace——父进程是内核线程（gui_worker），没有用户态地址空间，子进程需要从零开始创建才能让 execve 映射 ELF 段。接着创建独立的 FDTable 并用 set 把管道 inode 安装到 fd 0 和 fd 1。

execve 加载 /bin/sh 后，还需要手动映射用户栈（从 USER_STACK_TOP 向下分配 USER_STACK_PAGES 个物理页）、激活地址空间（切换 CR3 到子进程的页表）、更新 syscall 内核栈指针（确保后续 syscall 入口使用子进程的内核栈）、最后 jump_to_usermode 进入 Ring 3。这些步骤在正常的 fork+execve 流程中是由 execve 的用户态跳转逻辑处理的，但我们的子进程是从内核线程 fork 出来的，没有走正常的 execve 返回路径。

这里有一个大坑——在 jump_to_usermode 之前不要调用 sti。SYSRETQ 指令会从 R11 寄存器恢复 RFLAGS，而 R11 中已经设置了 IF=1（在 jump_to_usermode 的汇编代码中设置的），所以 SYSRETQ 执行后中断会自动启用。如果在 SYSRETQ 之前调用 sti，会打开一个危险的窗口期——此时 CR3 已经是子进程的页表，但 identity mapping 可能还不完整，PIT 在这个窗口期触发的话会直接 page fault。这个 bug 非常隐蔽，因为症状取决于 PIT 中断的时序，不一定每次都触发——有时候系统正常启动，有时候莫名 triple fault，让你以为是内存损坏或者随机硬件问题。

### 和 Linux 的 shell 启动流程对比

Linux 的 shell 启动流程更复杂但原理相似。init 进程（systemd 或 sysvinit）通过 fork + exec 启动 getty，getty 打开 /dev/ttyN 设备然后 exec login，login 验证身份后 exec shell。shell 的 stdin/stdout/stderr 都连接到 tty 设备，tty 驱动内部维护了行编辑缓冲区（用于退格、历史等）、信号处理（Ctrl+C 发 SIGINT）、和前台进程组管理（job control）。

Cinux 简化了这一切——没有 tty 驱动层，没有 login 认证，shell 直接通过管道连接终端窗口。这种简化的代价是缺少了很多 POSIX 特性：没有 job control（Ctrl+Z 挂起进程）、没有信号传递（shell 关闭时子进程不会收到 SIGHUP）、没有行编辑缓冲区（退格和光标移动由 shell 自己处理）。但对于教学 OS 来说，这些缺失保持了代码的可理解性——每一步都是显式的、可追踪的。

## 第四步——PIT tick 轮询

终端窗口创建好了，shell 也在跑了，但 shell 的输出怎么到达终端？答案是在 PIT tick 回调中轮询。每次 PIT 中断（约 100Hz），我们遍历窗口管理器中的所有窗口，对 Terminal 类型的窗口调用 poll_output（从 stdout 管道非阻塞读取 shell 输出）和 render_to_canvas（渲染到 Canvas），然后 composite 合成到屏幕。

这里有一个重要的设计决策——我们轮询所有终端窗口，不仅仅是获得焦点的那个。这保证了多个 shell 会话可以并发运行和更新。每个 Terminal 有自己独立的管道对，互不干扰。实际上这就是一个简化的多任务终端——你可以同时开好几个 Shell 窗口，每个都在独立运行，不需要任何额外的调度或同步机制。

点击 Shell 图标时的终端创建是延迟执行的——ISR 中只设置一个 Atomic flag（IconAction::OpenShell），gui_worker 线程在主循环中检查这个 flag 并执行 create_shell_terminal。这样做是因为 fork + execve 涉及进程表操作和内存分配，不适合在中断上下文中执行——中断处理程序应该尽可能短小快速，把耗时操作推迟到线程上下文。

## 收尾

到这里整个 tag 031 的功能就完整了。在 QEMU 中启动 Cinux，GUI 桌面显示 Shell 和 Calculator 图标，点击 Shell 图标弹出一个终端窗口，shell 提示符出现在窗口内，你可以用键盘输入命令，shell 的执行结果实时显示在终端上。从串口终端到 GUI 终端，这一步标志着 Cinux 正式从一个"能跑 shell 的内核"进化成了一个"有图形界面的操作系统"。

回顾一下我们走过的路：从环形缓冲区的 head/tail/count 管理，到自旋锁保护的阻塞读写，到 VFS InodeOps 适配器，到 sys_pipe 系统调用，到 80x25 字符缓冲区的终端组件，到 ANSI 转义序列解析，到 PSF 字体像素渲染，到 fork+execve+pipe 三件套，到 sys_read/sys_write 的 VFS 路由重构。每一步都是在前一步的基础上叠加，最终形成了从键盘到屏幕的完整数据通路。这就是操作系统的魅力——每一层都很简单，但叠加在一起就是一个能工作的系统。

当然还有很多可以改进的地方——wait_queue 替代 spin-wait、完整 VT100 兼容、伪终端 (pty) 设备、信号传递（SIGPIPE/SIGHUP）、dup2 和 shell 管道操作符——但这些都是未来的事了。完结撒花。

## 参考资料

- OSDev Wiki: [Unix Pipes](https://wiki.osdev.org/Unix_Pipes) — fork/execve/pipe 三件套和文件描述符继承机制
- xv6 RISC-V: [kernel/pipe.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c) — 经典教学 OS 的 fork+execve+pipe 集成，使用 FD_PIPE 类型分发
- Intel SDM: Vol.2A SYSRETQ — SYSRETQ 从 R11 恢复 RFLAGS 的行为，IF 位自动设置，不需要手动 sti
- Oracle Linux Blog: [Pipe and Splice](https://blogs.oracle.com/linux/pipe-and-splice) — Linux 内核的 file_operations 多态分发模式和 pipe_buffer 结构
