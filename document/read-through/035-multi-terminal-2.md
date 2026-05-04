# execve 从 ext2 加载 Shell

## 概览

本篇讲解 tag 035 中最关键的架构变更：Shell 从嵌入内核的二进制 blob 变成从 ext2 文件系统独立加载的 ELF 可执行文件。涉及 usermode.cpp 的大幅重构（launch_first_user 被移除）、per-CPU 数据页的引入、以及 create_shell_terminal 中完整的 fork -> execve -> jump_to_usermode 链路。

关键设计决策：usermode_init 只负责 MSR 和 GS 页配置，shell 加载逻辑由调用者驱动；per-CPU 数据页通过 gs:0 提供 syscall 入口所需的内核栈指针。

## 架构图

```
usermode_init():
  STAR/EFER MSR ──→ per-CPU GS page (gs:0=kernel_stack)
  g_per_cpu.gs_page_vaddr = gs_page_vaddr

create_shell_terminal():
  fork() ──→ parent: add_window ──→ child:
    cli → new AddressSpace → new FDTable (fd0=stdin, fd1=stdout)
    execve("/bin/sh") → alloc user stack → activate → update_syscall_stack
    jump_to_usermode(entry, user_stack_top)

kernel_init_thread() [non-GUI]:
  fork() ──→ parent: wait ──→ child:
    same as above but direct (no pipe, uses legacy sys_read/write)
```

## 代码精讲

### usermode_init() 简化

```cpp
void usermode_init() {
    usermode_init_asm();  // STAR + EFER MSR 配置
    kprintf("[USER] STAR/EFER MSRs configured for SYSRET.\n");

    constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
    uint64_t gs_phys = g_pmm.alloc_page();
    auto* gs_virt = reinterpret_cast<uint64_t*>(gs_phys + KERNEL_VMA);
    gs_virt[0] = 0;  // kernel stack — filled by scheduler
    gs_virt[1] = 0;

    write_msr(MSR_KERNEL_GS_BASE, gs_phys + KERNEL_VMA);
    cinux::proc::g_per_cpu.gs_page_vaddr = gs_phys + KERNEL_VMA;
}
```

之前 launch_first_user() 做了太多事（映射 shell 代码页、映射用户栈、激活地址空间、跳转 Ring 3），现在全部移除。usermode_init 只做两件事：调用汇编函数配置 STAR/EFER MSR，以及分配 per-CPU GS 数据页。gs_virt[0] 初始化为 0——实际的内核栈指针由调度器在每次 context switch 时通过 update_syscall_stack 写入。

### PerCPU 结构与 update_syscall_stack

```cpp
struct PerCPU {
    Task* current;
    uint64_t kernel_stack;
    uint64_t gs_page_vaddr;

    void update_syscall_stack(uint64_t stack_top) {
        kernel_stack = stack_top;
        if (gs_page_vaddr != 0) {
            *reinterpret_cast<volatile uint64_t*>(gs_page_vaddr) = stack_top;
        }
    }
};
```

gs_page_vaddr 是 per-CPU 数据页的虚拟地址。update_syscall_stack 在每次 context switch 后被调用，把新任务的 kernel_stack_top 写入 gs:0。这样 syscall_entry 中 `movq %gs:0, %rsp` 就能加载正确的内核栈指针。

### create_shell_terminal() — 子进程路径

```cpp
int child_pid = cinux::proc::fork(cinux::proc::g_pid_alloc);
if (child_pid > 0) {
    // Parent: record shell PID and add window
    term->set_shell_pid(child_pid);
} else if (child_pid == 0) {
    __asm__ volatile("cli");  // 防止 PIT 在不完整的 CR3 上触发

    auto* task = cinux::proc::Scheduler::current();
    task->addr_space = new cinux::mm::AddressSpace();

    task->fd_table = new cinux::fs::FDTable();
    task->fd_table->set(0, new cinux::fs::File(stdin_read_inode, 0, RDONLY));
    task->fd_table->set(1, new cinux::fs::File(stdout_write_inode, 0, WRONLY));

    auto result = cinux::proc::execve("/bin/sh", argv, envp);
    // ... set up user stack ...
    task->addr_space->activate();
    cinux::proc::g_per_cpu.update_syscall_stack(task->kernel_stack_top);
    jump_to_usermode(entry, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
}
```

子进程路径做了 6 件事：cli（防止中断在不完整状态下触发）、创建新 AddressSpace（父进程是内核线程没有地址空间）、创建私有 FDTable（绑定 stdin/stdout pipe）、execve 加载 shell ELF、设置用户栈、激活地址空间并跳转到 Ring 3。

update_syscall_stack 调用很关键——它把子进程的 kernel_stack_top 写入 gs:0。如果不做这一步，子进程在用户态执行 syscall 时，syscall_entry 从 gs:0 加载的 RSP 还是父进程的栈指针，会导致栈混乱。

### 页内偏移修复（详细版）

旧版代码的 bug：

```cpp
// BUG: page_base_offset 被错误地当作页内偏移
uint64_t page_base_offset = vaddr - phdr.p_vaddr;
copy_start = page_base_offset;
inode->ops->read(inode, phdr.p_offset + copy_start, dst + copy_start, copy_len);
```

当 vaddr = 0x401000（第二页），phdr.p_vaddr = 0x400260 时，page_base_offset = 0xDA0。copy_start = 0xDA0。dst 是一个新分配的 4KB 物理页（通过 KERNEL_VMA 映射），写入 dst + 0xDA0 还是安全的。但当 vaddr = 0x402000 时，page_base_offset = 0x1DA0，copy_start = 0x1DA0，写入 dst + 0x1DA0 就越界了——dst 只有 4096 字节。

修复版：

```cpp
uint64_t data_vaddr  = (vaddr < phdr.p_vaddr) ? phdr.p_vaddr : vaddr;
uint64_t in_page_off = data_vaddr - vaddr;    // 第一页可能 > 0，后续页 = 0
uint64_t seg_offset  = data_vaddr - phdr.p_vaddr;

if (seg_offset < phdr.p_filesz) {
    uint64_t copy_len = phdr.p_filesz - seg_offset;
    uint64_t avail    = PAGE_SIZE - in_page_off;
    if (copy_len > avail) copy_len = avail;

    inode->ops->read(inode, phdr.p_offset + seg_offset,
                      dst + in_page_off, copy_len);
}
```

in_page_off 永远 < PAGE_SIZE（最多 PAGE_SIZE-1），所以 dst + in_page_off 不会越界。seg_offset 是段内偏移，用于计算文件读取位置——和页内写入位置完全解耦。

## 设计决策

### 决策：cli 后不 sti

**问题**: 子进程在 jump_to_usermode 前要不要 sti？
**做法**: 不 sti。SYSRETQ 自动恢复 IF。
**原因**: SYSRETQ 从 R11 恢复 RFLAGS。jump_to_usermode 在进入前把 R11 设为包含 IF=1 的值。如果在 jump_to_usermode 前 sti，PIT tick 可能在子进程的 CR3 下触发——此时用户地址空间刚设置好但内核的恒等映射可能不完整（特别是 framebuffer），gui_tick_callback 的 composite 操作可能把 demand-page 零页映射到 framebuffer MMIO 区域上，直接黑屏。这是一个真正的坑——cli 到 SYSRETQ 之间的窗口必须是中断关闭的。

## 扩展方向

- 支持通过命令行参数传递 execve 的 argv（比如 /bin/sh -c "echo hello"）
- 实现 init 进程的标准 PID 1 语义（孤儿进程被 init 收养）
- 用户栈的 guard page（类似内核栈的 guard page 机制）

## 参考资料
- Intel SDM Vol.3A Section 5.8.7 "Performing Fast Calls to System Procedures"
- OSDev Wiki SYSCALL: https://wiki.osdev.org/SYSCALL
- Linux execve (do_execve): https://github.com/torvalds/linux/blob/master/fs/exec.c
