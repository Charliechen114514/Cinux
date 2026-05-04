# execve 与进程生命周期管理

> 标签：execve, ELF loading, waitpid, zombie, process lifecycle
> 前置：tag 034-2 fork 的艺术

## 前言

上一篇我们让 fork 跑起来了——子进程和父进程共享物理页，标记 CoW，通过 trampoline 从 fork 调用点返回 0。但子进程现在只能跑和父进程一样的代码，这不是我们想要的。真正的多进程世界里，fork 之后紧跟的是 execve——用一个全新的程序替换当前进程的内存映像。

execve 是进程生命周期中"替换"操作的核心，waitpid 是"回收"操作的核心。加上 fork 的"创建"，三者构成了完整的进程生命周期：创建 -> 运行（可能被 execve 替换）-> 退出 -> 回收。这一篇我们打通整条链路。

## 环境说明

execve 从 ext2 文件系统（AHCI port 1）读取 ELF 文件。构建脚本自动把 user/shell 复制到 ext2 镜像的 /bin/sh。QEMU 启动参数包含 `-drive file=ext2.img,format=raw` 挂载 ext2 镜像。

## execve 的完整链路

execve 做的事情用一句话概括：把当前进程的地址空间清空，从文件系统读取一个新的 ELF 可执行文件，把 PT_LOAD 段映射到地址空间，然后把入口点地址写入 task->ctx.rip。

先说路径解析。execve 接收一个路径字符串（比如 "/bin/sh"），通过 vfs_resolve 解析到具体的文件系统实例和相对路径（"bin/sh"），然后用 fs->lookup 找到 inode。接下来检查 inode 的类型——必须是 Regular 文件，不能是目录。任何一步失败都返回对应的 errno 风格错误码。

然后是 ELF 读取。先从 inode 读取 64 字节的 ELF header，validate_elf_header 验证后提取 program header table 的位置和数量。动态分配缓冲区一次性读取所有 program headers。

接下来是关键步骤——清除旧映射。clear_user_mappings 做四层嵌套遍历（PML4 -> PDPT -> PD -> PT），释放所有用户空间的物理页（包括数据页和页表页本身），将所有 entry 清零。这一步彻底摧毁了旧的地址空间，为新程序腾出位置。

最后是 PT_LOAD 段加载。对每个 PT_LOAD 段，计算它覆盖的虚拟地址范围（按页对齐），逐页处理：分配物理页、清零、从 ELF 文件填充数据、map 到地址空间。这里有个精妙的页内偏移计算——

## 页内偏移计算——那个差点让我放弃的 bug

如果一个段的 p_vaddr 是 0x400260（非页对齐），那么第一页的虚拟地址是 0x400000。段数据在第一页的 offset 0x260 处开始。从 ELF 文件的 p_offset 处读数据，写入物理页的 offset 0x260 处。第二页 vaddr = 0x401000，段数据从 offset 0 开始写入——in_page_off 是 0。

早期版本的 bug 是把 `page_base_offset = vaddr - p_vaddr` 当作页内偏移。第一页 offset=0 没问题，但第二页 offset=0xDA0，第三页 offset=0x1DA0...写入 dst+0x1DA0 直接越界到相邻内存。症状特别诡异：shell 的 .text 段在第一页所以代码能跑，但 .rodata 在后续页全是零——字符串常量变成 \x00。shell 能回显你按的键（因为回显走的是栈变量），但不显示 prompt 和欢迎信息（因为那些字符串在 .rodata 里）。

修复的关键是区分两个概念：in_page_off（数据在这一页内的起始偏移，永远 < PAGE_SIZE）和 seg_offset（段内偏移，用于计算文件读取位置）。data_vaddr = max(vaddr, p_vaddr) 确保第一页处理段非页对齐的情况，后续页 data_vaddr = vaddr 所以 in_page_off = 0。

Linux 的 load_elf_binary 处理了更复杂的情况——包括 PT_LOAD 的 p_align 要求（通常 0x1000 但可能是 0x200000 对齐大页）、ELF 随机化（ASLR）、以及 memsz > filesz 的 BSS 清零。Cinux 的简化版本先清零整页再填入文件数据，BSS 自然就是零。

## waitpid 与 Zombie 回收

进程调用 exit() 后进入 Zombie 状态——TCB 还在内存里（父进程可能要读取 exit_status），但不再参与调度。父进程调用 waitpid 收集子进程的退出状态、从 children 链表 unlink、释放 PID、把子进程标记为 Dead。

Cinux 的 waitpid 是非阻塞的——如果子进程还没退出就返回 NotExited。这在 cooperative scheduler 中是合理的：阻塞等待需要"唤醒"机制（子进程 exit 时通知父进程），增加了不少复杂度。调用者可以用循环重试来模拟阻塞效果（Terminal 析构函数就这么做）。

和 Linux 对比：Linux 的 waitpid（实际是 waitid 系统调用）默认阻塞，通过 wait_queue 实现——子进程 exit 时调用 wake_up_process 唤醒等待的父进程。WNOHANG 标志可以变成非阻塞模式。xv6 的 wait 也是阻塞的，在 sleep 系统调用上等待（xv6 用 sleep/wakeup 原语替代条件变量）。

SerenityOS 的 waitpid 是完整的 POSIX 实现——支持 WNOHANG、WUNTRACED、WCONTINUED 等标志，处理 stopped/continued 进程的状态变化。Cinux 只处理最核心的 Zombie 回收场景。

## 进程状态机

Cinux 的进程有五个状态。Ready：刚创建或刚被唤醒，等待调度。Running：正在 CPU 上跑。Blocked：等待 I/O 或锁（比如 pipe 空时 sys_read 阻塞）。Zombie：已退出但父进程还没 waitpid。Dead：已被回收，TCB 可释放。

状态转换路径：TaskBuilder.build() 创建时 Ready -> Scheduler 调度时 Running -> 等 I/O 时 Blocked -> I/O 完成时 Ready -> exit() 时 Zombie -> 父进程 waitpid 时 Dead。fork 出来的子进程直接是 Ready（不是 Running——它需要等调度器选中）。

这个状态机和 xv6 非常接近——xv6 也有 UNUSED/EMBRYO/RUNNABLE/RUNNING/ZOMBIE 五个状态（多了 UNUSED 和 EMBRYO 用于 TCB 分配过程）。Linux 更复杂——有 TASK_INTERRUPTIBLE/TASK_UNINTERRUPTIBLE/TASK_STOPPED/TASK_TRACED 等细分状态。

## 收尾

到这里 tag 034 的三个核心组件全部就位：ELF 解析、fork with CoW、execve + waitpid。内核现在具备了完整的多进程创建和执行能力。下一篇（035）我们会把这些能力用到实际场景中——让 Shell 从 ext2 独立加载，让桌面支持多终端并发。

## 参考资料
- Linux do_execve: https://github.com/torvalds/linux/blob/master/fs/exec.c
- Linux load_elf_binary: https://github.com/torvalds/linux/blob/master/fs/binfmt_elf.c
- xv6 exec.c: https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/exec.c
- POSIX waitpid: https://pubs.opengroup.org/onlinepubs/9699919799/functions/waitpid.html
- Intel SDM Vol.3A Section 6.15 "Page-Fault Exception (#PF)"
