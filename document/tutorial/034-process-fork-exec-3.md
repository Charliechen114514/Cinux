# execve 与进程生命周期管理

> 标签：execve, ELF loading, waitpid, zombie, process lifecycle
> 前置：tag 034-2 fork 的艺术

## 前言

上一篇我们让 fork 跑起来了——子进程和父进程共享物理页，标记 CoW，通过 trampoline 从 fork 调用点返回 0。但子进程现在只能跑和父进程一样的代码，这不是我们想要的。

真正的多进程世界里，fork 之后紧跟的是 execve——用一个全新的程序替换当前进程的内存映像。execve 是进程生命周期中"替换"操作的核心，waitpid 是"回收"操作的核心。加上 fork 的"创建"，三者构成了完整的进程生命周期：创建 -> 运行（可能被 execve 替换）-> 退出 -> 回收。

这一篇我们打通整条链路。

## 环境说明

execve 从 ext2 文件系统（AHCI port 1）读取 ELF 文件。构建脚本自动把 user/shell 复制到 ext2 镜像的 /bin/sh。QEMU 启动参数包含 `-drive file=ext2.img,format=raw` 挂载 ext2 镜像。

这个 tag 的大部分代码在 `kernel/proc/process.cpp` 中，包括 execve()、waitpid()、clear_user_mappings()、以及 elf_error_to_execve() 辅助函数。系统调用注册在 `kernel/syscall/sys_execve.cpp` 和 `kernel/syscall/sys_waitpid.cpp` 中。

## execve 的完整链路

execve 做的事情用一句话概括：把当前进程的地址空间清空，从文件系统读取一个新的 ELF 可执行文件，把 PT_LOAD 段映射到地址空间，然后把入口点地址写入 task->ctx.rip。

### 路径解析

先说路径解析。execve 接收一个路径字符串（比如 "/bin/sh"），首先做参数检查——路径不能是空指针也不能是空字符串。

然后通过 vfs_resolve 解析到具体的文件系统实例和相对路径（"bin/sh"），再用 fs->lookup 找到 inode。接下来检查 inode 的类型——必须是 Regular 文件，不能是目录。

任何一步失败都返回对应的 errno 风格错误码（ENOENT=-2, EISDIR=-21 等）。ExecveResult 枚举把每个错误都映射到了 Linux 标准的 errno 值，这样 syscall 层可以直接返回给用户态。

### ELF 读取与防御性检查

然后是 ELF 读取。先检查 inode->size >= sizeof(Elf64_Ehdr)（64 字节），太小就返回 ReadFailed。

然后检查 inode->ops 是否为 nullptr——如果文件系统驱动没有实现 read 操作，直接返回错误。这些防御性检查看起来很琐碎，但在内核开发中，任何一步的疏忽都可能导致 null pointer dereference 然后收获一个漂亮的 kernel panic。

用栈上的 64 字节缓冲区读取 ELF header，validate_elf_header 验证后提取 program header table 的位置和数量。用 aligned new 动态分配缓冲区一次性读取所有 program headers。验证阶段已经确认了 phoff、phentsize、phnum 都是合法的，所以读取可以放心进行。用完记得 delete[] phdrs——忘记释放会导致内核堆内存泄漏。

### 清除旧映射

接下来是关键步骤——清除旧映射。

clear_user_mappings 做四层嵌套遍历（PML4 -> PDPT -> PD -> PT），释放所有用户空间的物理页——不只是数据页，页表页本身（PT、PD、PDPT 页）也都逐一释放，所有 entry 清零。

这一步彻底摧毁了旧的地址空间，为新程序腾出位置。内存释放的顺序是从叶子到根：先释放 PT 级别的数据页，然后释放 PT 页表页，再释放 PD 页表页，最后释放 PDPT 页表页。这个顺序确保了在释放子节点之前不会丢失对它的引用。

## PT_LOAD 段加载

最后是 PT_LOAD 段加载。对每个 PT_LOAD 段，计算它覆盖的虚拟地址范围（seg_start = p_vaddr 向下对齐到页，seg_end = p_vaddr + p_memsz 向上对齐到页），逐页处理：分配物理页、清零、从 ELF 文件填充数据、map 到地址空间。

### 页内偏移计算

实际代码使用 `page_base_offset = vaddr - p_vaddr` 来判断当前页是否有文件数据需要读取。

如果 page_base_offset < p_filesz，说明这一页还有文件数据没读完，计算 copy_len（从当前偏移到 filesz 结束，不超过一页大小），从文件的 p_offset + copy_start 位置读取数据写入物理页的 copy_start 位置。

这个算法的前提是 ELF 的 PT_LOAD 段的 p_vaddr 是页对齐的（p_align 通常为 0x1000），所以 seg_start 等于 p_vaddr，page_base_offset 从 0 开始递增。如果 p_vaddr 不是页对齐的，需要额外的页内偏移处理——需要区分 in_page_off（数据在页内的起始位置）和 seg_offset（段内偏移，用于文件读取位置）。不过 Cinux 的 shell 和标准工具都是页对齐的 ELF，所以当前实现足够使用。

### BSS 清零

每页在填充文件数据之前先清零，这样 memsz > filesz 的部分（.bss 段）自然就是零，不需要额外的清零步骤。这是一个巧妙的设计决策——先清零整页，再填入文件数据，BSS 自然就是零。

### 段标志位转换

段标志位也需要转换：根据 PF_W 设置 FLAG_WRITABLE，根据 PF_X 清除 FLAG_NX（No-Execute）。代码段通常是 PF_R | PF_X（只读可执行），数据段是 PF_R | PF_W（可读写）。

### 入口点设置

加载完成后检查 has_load_segment 标志——如果没有找到任何 PT_LOAD 段就返回 NoLoadSegments。

最后设置 task->ctx.rip = ehdr->e_entry，返回 Ok。注意 PID、ppid、parent、children 等进程身份信息保持不变——execve 只替换内存映像，不改变进程身份。这是 POSIX 标准的核心语义。

### 与 Linux 的对比

Linux 的 load_elf_binary 处理了更复杂的情况——包括 PT_LOAD 的 p_align 要求（可能 0x200000 对齐大页）、ELF 随机化（ASLR）、以及 memsz > filesz 的 BSS 清零。Cinux 的简化版本先清零整页再填入文件数据，BSS 自然就是零。

## waitpid 与 Zombie 回收

进程调用 exit() 后进入 Zombie 状态——TCB 还在内存里（父进程可能要读取 exit_status），但不再参与调度。

父进程调用 waitpid 收集子进程的退出状态、从 children 链表 unlink、释放 PID、把子进程标记为 Dead。

### 非阻塞设计

Cinux 的 waitpid 是非阻塞的——如果子进程还没退出就返回 NotExited。

这在 cooperative scheduler 中是合理的：阻塞等待需要"唤醒"机制（子进程 exit 时通知父进程），增加了不少复杂度。调用者可以用循环重试来模拟阻塞效果（Terminal 析构函数就这么做——在循环中反复调用 waitpid，直到子进程变成 Zombie 被回收）。

### 链表遍历与 unlink

waitpid 的实现扫描父进程的 children 单链表，支持两种模式：pid > 0 时等待指定子进程，pid == -1 时等待任意一个退出的子进程。

遍历时维护 prev 指针用于 unlink 操作——如果 prev 是 nullptr 说明目标是头节点，要把 parent->children 改为 target->wait_next。

找到 Zombie 状态的子进程后，收集 exit_status、从链表 unlink、释放 PID（通过 pid_alloc.free）、标记 Dead。每一步都有清晰的日志输出，方便调试。

### 与 Linux 的对比

Linux 的 waitpid（实际是 waitid 系统调用）默认阻塞，通过 wait_queue 实现——子进程 exit 时调用 wake_up_process 唤醒等待的父进程。WNOHANG 标志可以变成非阻塞模式。

xv6 的 wait 也是阻塞的，在 sleep 系统调用上等待（xv6 用 sleep/wakeup 原语替代条件变量）。

## 进程状态机

Cinux 的进程有五个状态：
- **Ready**：刚创建或刚被唤醒，等待调度
- **Running**：正在 CPU 上跑
- **Blocked**：等待 I/O 或锁（比如 pipe 空时 sys_read 阻塞）
- **Zombie**：已退出但父进程还没 waitpid
- **Dead**：已被回收，TCB 可释放

状态转换路径：TaskBuilder.build() 创建时 Ready -> Scheduler 调度时 Running -> 等 I/O 时 Blocked -> I/O 完成时 Ready -> exit() 时 Zombie -> 父进程 waitpid 时 Dead。

fork 出来的子进程直接是 Ready（不是 Running——它需要等调度器选中）。这个状态机看起来简单，但它覆盖了 Cinux 目前所有进程的完整生命周期。

### 与其他内核的对比

这个状态机和 xv6 非常接近——xv6 也有 UNUSED/EMBRYO/RUNNABLE/RUNNING/ZOMBIE 五个状态（多了 UNUSED 和 EMBRYO 用于 TCB 分配过程）。

Linux 更复杂——有 TASK_INTERRUPTIBLE/TASK_UNINTERRUPTIBLE/TASK_STOPPED/TASK_TRACED 等细分状态。

## 收尾

到这里 tag 034 的三个核心组件全部就位：ELF 解析、fork with CoW、execve + waitpid。内核现在具备了完整的多进程创建和执行能力。

后续我们会把这些能力用到实际场景中——让 Shell 从 ext2 独立加载，让桌面支持多终端并发。

## 参考资料
- Linux do_execve: https://github.com/torvalds/linux/blob/master/fs/exec.c
- Linux load_elf_binary: https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c
- xv6 exec.c: https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/exec.c
- POSIX waitpid: https://pubs.opengroup.org/onlinepubs/9699919799/functions/waitpid.html
- Intel SDM Vol.3A Section 6.15 "Page-Fault Exception (#PF)"
