# 从零实现内核管道 IPC — Cinux Pipe 机制全解析

> 标签：pipe, IPC, ring buffer, syscall, VFS, 内核开发
> 前置：[030 GUI 窗口管理器](030-gui-wm-basic-3.md)

## 前言

说实话，做到 tag 030 的时候，我们的 GUI 已经能画窗口了，但窗口里只能显示静态内容——彩色的矩形、图标、标题栏，没有任何交互能力。shell 还在串口终端里孤独地跑着，和 GUI 完全是两个世界。这种状态实在让人受不了——一个有窗口管理器的内核却不能在窗口里打字，就像买了一台显示器却忘了接键盘。

真正让桌面 OS 活起来的关键一步是：让 shell 进程的输入输出能到达 GUI 终端窗口。这就需要一个进程间通信（IPC）机制来传递字节数据，而 Unix 世界里最经典、最简洁的 IPC 方式就是管道（pipe）。接下来我们就要从零手搓一个内核管道——环形缓冲区、自旋锁保护、阻塞语义、VFS 集成、系统调用，一个不少。

## 环境说明

我们在 x86_64 架构上用 GCC 14 编译，C++ 内核禁用标准库和异常。构建系统是 CMake + NASM。QEMU 作为模拟器，串口输出用于调试。这个 tag 基于前一个 tag 030（GUI 窗口管理器），已有 VFS InodeOps 接口、文件描述符表（FDTable）、以及基本的 syscall 分发机制。管道模块位于 `kernel/ipc/` 目录下（新增），系统调用在 `kernel/syscall/sys_pipe.cpp`（新增）。

## 第一步——设计环形缓冲区

管道的核心是一个环形缓冲区（ring buffer），它本质上是一个固定大小的数组加上几个指针，让数据可以无限循环地写入和读出。我们选了 4096 字节（一页）作为缓冲区大小，这和 Linux 的默认管道容量一致，对于承载交互式 shell 的文本数据来说绰绰有余。

管理方式上，我们用了三个变量：`head`（下一个读取位置）、`tail`（下一个写入位置）、`count`（当前数据量）。判断空满状态很直观——`count == 0` 就是空，`count == 4096` 就是满。写入时先检查剩余空间，然后把数据放到 tail 位置并推进 tail。读取时从 head 位置取数据并推进 head。当指针到达数组末尾时自动回绕到开头——这就是"环形"的含义。

为什么要用三变量而不是像 xv6 那样用单调递增的 nread/nwrite 计数器？说实话两种方式都能用，但三变量方式在判断空满状态时更直观——不需要做减法运算，直接看 count 就行。对于一个教学 OS 来说，代码的可读性比那点微小的性能差异重要得多。而且 count 变量还能直接作为"管道里有多少数据"的查询接口，不需要额外计算。

环形缓冲区最需要小心的地方是"跨数组末尾"的写入。假设 tail 当前在 4090 的位置，你要写入 10 个字节，前 6 个字节写到 4090~4095，后 4 个字节要回绕到 0~3。读取也是一样。所以在实现中，每次写入（或读取）都分成两段操作——first 段从当前位置到数组末尾，second 段从数组开头继续。这个两段拷贝的长度计算是环形缓冲区中最容易出 off-by-one 的地方——如果 first 和 second 的加起来不等于 chunk，数据就会错位。

## 第二步——实现阻塞语义

缓冲区满了写不进去怎么办？缓冲区空了读不出来怎么办？在 Unix 系统里，进程会被挂起（sleep），等到另一端操作后唤醒。但我们现在的调度器还不支持在管道上做条件等待，所以我们用了一种折中方案——spin-wait。

spin-wait 的基本思路是：在一个循环里反复检查条件，同时调用 `hlt` 指令让 CPU 进入低功耗状态（比纯 `nop` 循环保电得多），最多等一百万次迭代就放弃。这不完美——CPU 时间被浪费了，但对于单核教学 OS 来说完全可以工作。真正的生产级 OS 会使用 wait queue 让进程在管道上睡眠，等到数据到达或空间可用时由另一端唤醒。

不过 spin-wait 里有一个非常关键的 IRQ 安全设计问题。在进入阻塞循环时，我们先 `irq_save()` 保存当前中断标志并关中断，然后获取自旋锁。如果缓冲区满需要等待，必须先释放锁再启用中断。为什么呢？因为 PIT 中断处理程序会调用 `poll_output -> try_read`，而 try_read 也需要获取同一把锁。如果你持有锁且关中断去做 hlt，PIT 中断进不来，try_read 永远执行不了，缓冲区永远不被消费，write 就永远在等——完美的死锁。

正确的做法是在 hlt 之前 unlock + irq_enable，检查条件之前 irq_disable + lock。这样每次 hlt 期间中断都能触发，try_read 有机会执行并消费数据，缓冲区就不满了，write 就能继续。这个模式在内核编程中非常常见——任何涉及"持有锁 + 等待事件"的场景都要考虑中断安全性。

除了阻塞版本的 read/write，我们还提供了非阻塞的 try_read/try_write。它们在缓冲区满/空时立即返回 0，不做任何等待。这个版本专门给 PIT 中断回调用——中断上下文中不能做任何可能阻塞的操作，否则整个系统的时钟都停了。

## 第三步——桥接到 VFS

光有 Pipe 数据结构还不够。在 Cinux 的架构里，所有文件操作都通过 VFS（虚拟文件系统）层——sys_read/sys_write 查找 FDTable 中的 File，File 指向 Inode，Inode 上挂着一组 InodeOps 虚函数。管道要让这个系统调用路径能走到它，就必须实现自己的 InodeOps。

我们用了两个薄适配器：PipeReadOps 和 PipeWriteOps。PipeReadOps 的 read 方法直接调用 Pipe::read，write 方法返回 -1（读端不可写）。PipeWriteOps 对称——write 走 Pipe::write，read 返回 -1。InodeOps 的 read/write 签名里有一个 offset 参数，但管道是不可寻址的字节流——你不可能 fseek 一个管道——所以这个参数直接被忽略了。

这个设计让管道在 VFS 层面和其他文件类型完全同构。对于上层代码来说，管道 fd 和普通文件 fd 的使用方式一模一样，只是底层的 InodeOps 实现不同。这也是为什么后面重构 sys_read/sys_write 时只需要加一个"先查 FDTable"的逻辑就能同时支持管道和传统设备——多态分发已经替我们做好了路由。

### 和 xv6 的对比

xv6 的管道实现走了另一条路。它没有通用的 InodeOps 接口——相反，在 `struct file` 里加了一个 `type` 字段（FD_PIPE、FD_INODE、FD_DEVICE），系统调用代码里根据 type 分发到不同的处理函数。这种方式更简单直接，但扩展性差——每增加一种文件类型都要在 sys_read/sys_write 里加 case 分支。Cinux 的 InodeOps 方案是通过多态分发，新增文件类型不需要改系统调用代码。

xv6 的阻塞方式也比我们优雅。它用 sleep/wakeup 机制——pipewrite 在缓冲区满时 sleep 在 `&pi->nwrite` 上，piperead 消费数据后 wakeup 同一个地址。piperead 在缓冲区空时 sleep 在 `&pi->nread` 上，pipewrite 写入数据后 wakeup。这是 Unix 内核的经典同步模式（条件变量的前身），效率远高于我们的 spin-wait。但代价是需要一个功能完整的调度器和条件等待机制，Cinux 暂时还没有。

xv6 的管道缓冲区只有 512 字节（PIPESIZE），而我们选了 4096 字节。512 字节对于早期的 PDP-11 来说足够了，但在现代系统中一个 shell 命令的输出就可能超过 512 字节（比如 `ls /` 列出几十个文件名）。4KB 给了更多的缓冲空间，减少了阻塞等待的频率。

### 和 Linux 的对比

Linux 内核的管道实现比我们复杂了不止一个数量级。它不是用一个连续的 4KB 缓冲区，而是用一个由 16 个 `pipe_buffer` 组成的数组，每个 `pipe_buffer` 指向一个独立的物理页（4KB），总容量 64KB，还可以通过 `fcntl(F_SETPIPE_SZ)` 动态调整。每个 pipe_buffer 有自己的 offset 和 len，支持 splice 操作（零拷贝页面迁移——数据在管道和文件之间移动时不需要经过用户态缓冲区）。

阻塞使用 wait_queue，允许进程在管道上睡眠等待。每个 pipe 有一个等待队列，写端 sleep 时读端 wakeup，反之亦然。这比 spin-wait 高效得多——sleep 的进程不占用 CPU，调度器可以运行其他进程。

我们选择 4KB 单缓冲区是经过权衡的。Cinux 的管道主要承载 shell 和终端之间的文本通信，数据量很小（交互式命令的输入输出通常不超过几百字节），64KB 的多页缓冲区完全没必要。简单的实现更适合教学目的——学生能在一次阅读中理解管道的全部核心逻辑，而不是被 Linux 的 splice、page cache、pipe_buf_operations 绕晕。

## 第四步——sys_pipe 系统调用

最后一步是把一切暴露给用户态。`pipe()` 系统调用做的事情很直观：在内核堆上 new 一个 Pipe，创建两套 InodeOps + Inode，在当前进程的 FDTable 中分配两个 fd，然后把 fd 编号写入用户态的 int 数组。

这里有一个容易忽略的细节——失败清理路径。如果第二个 fd 分配失败，必须先 close 已经分配的第一个 fd，然后按逆序 delete 所有 new 出来的对象（write_ops、write_inode、read_ops、read_inode、pipe）。如果你在代码评审时看到哪个 OS 项目在 sys_pipe 里忘了一两行 delete，那大概率有内存泄漏。在 Cinux 中，每个失败路径都有对应的清理代码，保证在任何一步失败时都不会泄漏资源。

地址验证也很重要。x86_64 的 canonical address 规则要求 bit 47 必须等于 bits 48-63，而且 bit 47 为 1 表示内核空间。sys_pipe 接收的用户态指针必须通过这个检查，否则恶意的用户程序可以传入一个内核地址让内核往自己的数据结构里写 fd 编号——这是一个典型的权限提升漏洞。

## 收尾

到这里，管道 IPC 机制就完整了。我们有了一个 4KB 环形缓冲区的 Pipe 类，阻塞和非阻塞两种读写模式，VFS InodeOps 适配器，以及一个 POSIX 语义的 sys_pipe 系统调用。整个管道模块大约 600 行代码（pipe.hpp/cpp + pipe_ops.hpp/cpp + sys_pipe.cpp），对于一个功能完整的 IPC 机制来说相当精简。

下一章我们会在这个管道上建一个终端组件——一个能接收字节流、渲染字符、处理 ANSI 转义序列的 GUI 窗口。到了那里，管道就不再是一个孤立的内核数据结构，而是连接 shell 和 GUI 的桥梁。

## 参考资料

- OSDev Wiki: [Unix Pipes](https://wiki.osdev.org/Unix_Pipes) — 管道设计模式和 fork/execve 集成指导，涵盖环形缓冲区、阻塞语义、fd 继承
- xv6 RISC-V: [kernel/pipe.c](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/pipe.c) — 经典教学 OS 的管道实现，使用 sleep/wakeup 阻塞和单调计数器管理
- Oracle Linux Blog: [Pipe and Splice](https://blogs.oracle.com/linux/pipe-and-splice) — Linux 内核多页管道缓冲区和 splice 零拷贝机制详解
- GitHub: [linux-kernel-notes/pipe](https://github.com/sheharyaar/linux-kernel-notes/blob/main/implementations/pipes.md) — pipe_buffer 结构体和内核管道实现细节
