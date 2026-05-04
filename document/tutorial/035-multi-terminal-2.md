# 从 ext2 执行 Shell——execve 完整链路

> 标签：execve, ELF, ext2, usermode, syscall, swapgs
> 前置：tag 035-1 COW 页错误处理

## 前言

到这一篇为止，Shell 一直是嵌入内核二进制的 blob（通过 objcopy 的 _binary_shell_bin_start/end 符号引用）。说实话这套方案在真正的操作系统面前实在站不住脚——你不能指望把每个用户程序都编译进内核。这一篇我们要实现一个质的飞跃：Shell 变成从 ext2 文件系统独立加载的 ELF 可执行文件。

这涉及 usermode.cpp 的大幅重构（launch_first_user 被移除，usermode_init 简化为只配置 MSR 和 GS 页）、per-CPU 数据页的引入、以及 create_shell_terminal 中完整的 fork -> execve -> jump_to_usermode 链路。过程中还踩了一个差点让我放弃的页内偏移 bug，以及一个"cli 后不要 sti"的时序陷阱。

## 环境说明

ext2 文件系统挂载在 AHCI port 1。构建脚本 create_ext2_disk.sh 自动把 user/shell ELF 复制到 ext2 镜像的 /bin/sh。usermode_init 在内核启动时调用一次，分配 per-CPU GS 数据页。

## usermode_init 简化——职责分离

之前的 launch_first_user() 做了太多事：分配用户地址空间、映射 shell 代码页、映射用户栈、激活地址空间、设置 GS base、跳转 Ring 3。这个函数承担了太多职责，违反了单一职责原则。

重构后 usermode_init 只做两件事。第一，调用 usermode_init_asm() 配置 STAR 和 EFER MSR（SYSCALL/SYSRET 的硬件基础）。第二，分配 per-CPU GS 数据页——一个 4KB 的物理页，gs:0 存储内核栈指针（调度器每次 context switch 时更新），gs:8 存储 user RSP scratch。这个数据页的虚拟地址保存到 g_per_cpu.gs_page_vaddr，写入 MSR_KERNEL_GS_BASE。

shell 的加载和用户态跳转完全由调用者负责——GUI 模式下是 create_shell_terminal()，非 GUI 模式下是 kernel_init_thread()。这种职责分离使得同一个 execve + jump_to_usermode 逻辑可以在不同场景复用。

## per-CPU 数据页——syscall 的栈指针来源

syscall_entry 的入口汇编使用 `swapgs; movq %gs:0, %rsp` 来获取内核栈指针。gs:0 指向的就是 per-CPU 数据页的第一个 8 字节。这个值必须和当前任务的内核栈顶一致——否则 syscall 会用错误的栈。

g_per_cpu.update_syscall_stack(stack_top) 方法做两件事：更新 kernel_stack 字段，以及把 stack_top 写入 gs:0（通过 gs_page_vaddr 计算出的虚拟地址）。调度器每次 context switch 后调用这个方法，确保 gs:0 始终指向当前任务的栈顶。

Linux 的 per-CPU 机制更复杂——它使用 DEFINE_PER_CPU 宏在编译时分配 per-CPU 变量，运行时通过 __per_cpu_offset 数组索引。但 Cinux 是单核的，只有一个 per-CPU 数据页，所以直接用全局变量 + GS 段前缀就够了。

## create_shell_terminal——从图标到进程

点击 Shell 图标后的完整链路：PIT tick 中断 -> gui_tick_callback 检测到 IconAction::OpenShell -> 设置 deferred action（Atomic store）-> gui_worker 线程 -> gui_process_pending -> create_shell_terminal。

为什么不在 PIT tick 中直接 fork？因为 PIT tick 在 Interrupt gate 下运行（IF=0），而 fork 需要分配物理页和映射页表——这些操作可能需要锁，IF=0 下拿锁会死锁。延迟工作队列（Atomic + worker 线程）是一个经典的 ISR + deferred work 模式。

create_shell_terminal 每次调用都创建独立的资源：stdin/stdout pipe 对、pipe inode 和 ops、Terminal 对象。fork 后子进程创建新的 AddressSpace（父进程 gui_worker 是内核线程没有地址空间）、新的 FDTable（fd 0 绑 stdin read inode，fd 1 绑 stdout write inode），然后 execve("/bin/sh")。

## cli 后不要 sti——一个时序陷阱

子进程路径中有一行 `__asm__ volatile("cli")`，cli 后一直到 jump_to_usermode 之间不能 sti。原因很精妙：SYSRETQ 指令会从 R11 恢复 RFLAGS（包括 IF 位）。jump_to_usermode 在进入前把 R11 设为包含 IF=1 的值，所以 SYSRETQ 执行后中断自动开启——这是原子操作，没有窗口。

如果在 jump_to_usermode 前显式 sti，PIT tick 可能在子进程的 CR3 下触发。此时用户地址空间刚设置好但内核的恒等映射可能不完整（特别是 framebuffer 区域），gui_tick_callback 的 composite 操作可能把 demand-page 零页映射到 framebuffer MMIO 区域上，直接黑屏。这个 bug 非常难定位——因为黑屏时内核实际上还在跑，只是 framebuffer 内容被覆盖了。

## 页内偏移 bug——shell 只回显不执行命令

这个 bug 的表现特别诡异：shell 能实时回显按键（按 p 显示 p），但不显示欢迎信息和提示符。原因在于 execve 加载 ELF 时，页内偏移计算错误导致 .rodata 段全是零。

旧代码把 `page_base_offset = vaddr - p_vaddr` 直接当作页内偏移。第一页 offset=0 没问题，第二页 offset=0xDA0，第三页 offset=0x1DA0...写入 dst+0x1DA0 越界到相邻内存，dst 本身保持全零。.text 在第一页所以代码能执行，.rodata 在后续页所以字符串常量变成 \x00。

调试线索非常关键：栈变量（char c; write_buf(&c, 1)）正常，.rodata 字符串（write_str("cinux> ")）丢失。这个差异直接指向了 ELF 加载器 bug——栈由内核显式映射并初始化，.rodata 由 execve 从 ELF 文件填充，加载器 bug 只影响后者。

修复方案是正确区分 in_page_off（数据在当前页内的偏移，永远 < PAGE_SIZE）和 seg_offset（段内偏移，用于文件读取位置）。data_vaddr = max(vaddr, p_vaddr) 处理第一页非页对齐的情况。

## 收尾

到这里 Shell 从 ext2 独立加载的完整链路已经打通。下一篇我们把这个能力用到多终端桌面上——每次点击 Shell 图标创建一个独立的终端窗口，每个终端绑定自己的 shell 进程和 pipe 对。

## 参考资料
- Intel SDM Vol.3A Section 5.8.7 "Performing Fast Calls to System Procedures" — SYSCALL/SYSRET
- OSDev Wiki SYSCALL: https://wiki.osdev.org/SYSCALL
- Linux execve (do_execve): https://github.com/torvalds/linux/blob/master/fs/exec.c
- ELF-64 Object File Format: https://uclibc.org/docs/elf-64-gen.pdf
