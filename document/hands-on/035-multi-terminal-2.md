---
title: 035-multi-terminal-2 · 多终端
---

# 从 ext2 执行 Shell——execve 完整链路

## 导语

上一篇我们把 COW fault handler 和 GS MSR 保存/恢复搞定了，fork 的基础设施已经就绪。现在要打通从"点击 Shell 图标"到"Shell 在用户态运行"的完整链路。这一篇聚焦在 execve 的实际使用场景：从 ext2 文件系统读取 /bin/sh 这个 ELF 可执行文件，加载到子进程的地址空间，设置用户栈，然后跳转到 Ring 3 开始执行。

在此之前，Shell 一直是嵌入内核二进制的 blob（_binary_shell_bin_start/end）。完成本篇后，Shell 变成了一个从 ext2 独立加载的用户程序——这才是真正的操作系统该有的样子。

## 概念精讲

### usermode.cpp 重构

之前 usermode.cpp 有一个 launch_first_user() 函数，它做了太多事：分配用户地址空间、映射 shell 代码页、映射用户栈、激活地址空间、设置 GS base、跳转到 Ring 3。在 035 中这个函数被完全移除——所有这些职责分散到了正确的位置。usermode_init() 简化为只做两件事：配置 STAR/EFER MSR 和分配 per-CPU GS 数据页。shell 的加载和跳转由 create_shell_terminal()（GUI 模式）或 kernel_init_thread（非 GUI 模式）负责。

### Per-CPU 数据页

syscall_entry 使用 GS 段前缀访问 per-CPU 数据：gs:0 存储内核栈指针（scheduler 每次切换时更新），gs:8 存储 user RSP scratch，gs:16 存储 return value scratch。这个数据页的虚拟地址保存在 g_per_cpu.gs_page_vaddr 中。调度器每次 context switch 时调用 g_per_cpu.update_syscall_stack()，把新任务的 kernel_stack_top 写入 gs:0。

### execve 完整链路

从点击 Shell 图标到 Shell 执行，完整链路是：PIT tick -> gui_tick_callback -> 检测到 IconAction::OpenShell -> 设置 deferred action -> gui_worker 线程 -> gui_process_pending() -> create_shell_terminal() -> fork() -> 子进程：创建 AddressSpace、设置 FDTable、execve("/bin/sh")、设置用户栈、activate 地址空间、update_syscall_stack、jump_to_usermode。

## 动手实现

### Step 1: usermode_init() 简化

**目标**: 移除 launch_first_user()，简化 usermode_init() 为只配置 MSR 和 GS 页。

**设计思路**: usermode_init() 仍然调用 usermode_init_asm() 来设置 STAR 和 EFER MSR。然后分配一个物理页作为 per-CPU 数据页，通过 KERNEL_VMA 映射后清零，把虚拟地址写入 MSR_KERNEL_GS_BASE（这样 swapgs 后就能通过 GS 访问）。同时把虚拟地址保存到 g_per_cpu.gs_page_vaddr 供后续使用。

**实现约束**: 分配一个物理页（g_pmm.alloc_page()），通过 phys + KERNEL_VMA 得到虚拟地址。清零 gs_virt[0] 和 gs_virt[1]。write_msr(MSR_KERNEL_GS_BASE, gs_phys + KERNEL_VMA)。设置 g_per_cpu.gs_page_vaddr = gs_phys + KERNEL_VMA。删除 launch_first_user() 的声明和实现。

### Step 2: PerCPU 结构扩展

**目标**: 在 PerCPU 中添加 gs_page_vaddr 和 update_syscall_stack 方法。

**设计思路**: PerCPU 是单核内核的全局变量（g_per_cpu），保存当前 CPU 的状态。新增 gs_page_vaddr 保存 per-CPU 数据页的虚拟地址。update_syscall_stack() 方法在每次 context switch 时被调用，把新任务的内核栈顶地址写入 gs:0，这样下次 syscall_entry 时 gs:0 就能读到正确的栈指针。

**实现约束**: PerCPU 新增 uint64_t gs_page_vaddr（初始化为 0）和 void update_syscall_stack(uint64_t stack_top) 方法。方法实现：更新 kernel_stack 字段，然后如果 gs_page_vaddr != 0，通过 reinterpret_cast 写入 *gs_page_vaddr = stack_top。

### Step 3: 子进程的 execve 链路

**目标**: 在 create_shell_terminal() 中实现完整的 fork -> execve -> jump_to_usermode 链路。

**设计思路**: fork 后子进程进入 child path（child_pid == 0）。子进程首先关闭中断（cli），因为此时它还跑在父进程的内核栈上，而且可能被 PIT tick 中断。然后创建新的 AddressSpace（因为父进程 gui_worker 是内核线程没有地址空间），设置私有的 FDTable（绑定 stdin/stdout pipe），调用 execve 加载 shell ELF，成功后分配用户栈页，activate 地址空间，更新 syscall 栈，最后 jump_to_usermode 进入 Ring 3。

**实现约束**: 子进程代码路径：cli、Scheduler::current() 获取 task、new AddressSpace()、new FDTable() 并 set(0, stdin_read_file) set(1, stdout_write_file)、execve("/bin/sh", argv, envp)（失败则 exit_current）、从 task->ctx.rip 获取 entry point、分配 USER_STACK_PAGES 个物理页并 map 到 USER_STACK_TOP 下方、task->addr_space->activate()、g_per_cpu.update_syscall_stack(task->kernel_stack_top)、jump_to_usermode(entry, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0)。

**踩坑预警**: 在 cli 之后、jump_to_usermode 之前，不要调用 sti。SYSRETQ 指令会自动从 R11 恢复 RFLAGS（包括 IF 位），R11 在 jump_to_usermode 中被设为包含 IF=1 的值。如果在 jump_to_usermode 前显式 sti，PIT tick 可能在子进程的 CR3 下触发，导致 gui_tick_callback 用不完整的页表去 composite——把 demand-page 的零页映射到 framebuffer MMIO 区域上，直接黑屏。

### Step 4: 页内偏移修复

**目标**: 修复 execve 加载 PT_LOAD 段时的页内偏移计算 bug。

**设计思路**: 正确的页内偏移计算需要区分三个概念：vaddr 是当前正在处理的页的起始虚拟地址（页对齐的），p_vaddr 是段的起始虚拟地址（可能非页对齐），in_page_off 是段数据在这一页内的起始偏移。对于第一页，如果 p_vaddr > vaddr，那么 in_page_off = p_vaddr - vaddr（段数据不在页的开头）。对于后续页，in_page_off = 0。seg_offset = data_vaddr - p_vaddr 是段内偏移，用于计算从 ELF 文件的什么位置读取数据。

**实现约束**: 在加载循环中，对每个 vaddr：data_vaddr = max(vaddr, p_vaddr)、in_page_off = data_vaddr - vaddr、seg_offset = data_vaddr - p_vaddr。如果 seg_offset < p_filesz：copy_len = min(p_filesz - seg_offset, PAGE_SIZE - in_page_off)，从 inode 的 (p_offset + seg_offset) 处读取 copy_len 字节到 dst + in_page_off。

**验证**: execve 后 shell 应该显示欢迎信息 "Cinux shell - type 'help' for commands" 和提示符 "cinux> "。如果只看到键盘回显但看不到 prompt，说明 .rodata 加载有问题。

## 构建与运行

Shell ELF 现在从 ext2 镜像加载。构建脚本 create_ext2_disk.sh 会自动把 user/shell 复制到 ext2 镜像的 /bin/sh。

```bash
cmake --build build
make run
```

## 调试技巧

1. **Shell 不显示 prompt 但能回显按键**: 典型的 .rodata 全零问题。在 execve 加载每页后打印 vaddr、in_page_off、copy_len，检查第二页起 in_page_off 是否为 0。如果 in_page_off 在第二页还是非零，说明页内偏移计算有 bug。

2. **黑屏 + demand-page 0xFD000000 洪水**: 这是 fork 复制了内核恒等映射的大页条目导致的问题。检查 copy_page_table_level 是否正确跳过了没有 FLAG_USER 的条目。

3. **execve 返回 FileNotFound**: 检查 ext2 镜像里是否真的有 /bin/sh。用 `debugfs ext2.img` 然后 `ls /bin` 查看。如果没有，检查 create_ext2_disk.sh 是否正确复制了 shell 二进制。

## 本章小结

| 组件 | 关键设计 | 要点 |
|------|----------|------|
| usermode_init 简化 | 只配置 MSR + GS 页 | launch_first_user 移除 |
| PerCPU 扩展 | gs_page_vaddr + update_syscall_stack | scheduler 每次切换更新 gs:0 |
| execve 完整链路 | fork -> AS -> FDTable -> execve -> stack -> activate -> usermode | cli 后不要 sti |
| 页内偏移修复 | in_page_off vs seg_offset | data_vaddr = max(vaddr, p_vaddr) |
