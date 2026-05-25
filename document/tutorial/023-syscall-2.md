---
title: 023-syscall-2 · 系统调用
---

# 分发、验证、终止 -- syscall handler 的工程实践

> 标签：syscall dispatch, sys_write, sys_exit, sys_yield, user address validation
> 前置：[023-1 从 Ring 3 打通内核](023-syscall-1.md)

## 前言

上一章我们搭好了 SYSCALL 的 MSR 配置和汇编入口，用户的 syscall 指令能正确跳到内核的 syscall_entry，保存好寄存器后调用 syscall_dispatch。现在 dispatch 函数收到一个系统调用号和六个参数——但它怎么知道该调用哪个函数？这就是分发表（dispatch table）的职责。本章我们要实现分发表机制和三个最基础的系统调用 handler：sys_write（让用户程序能输出字符）、sys_exit（让用户程序能正常终止）、sys_yield（让用户程序能主动让出 CPU）。

这三个 handler 虽然简单，但每一个都涉及操作系统设计中的经典问题。sys_write 要做用户地址验证——内核不能盲目信任用户传来的指针。sys_exit 要处理"调度器可能还没启动"的尴尬局面。sys_yield 则是进程协作调度的基石。说实话，别看代码量不大，这几个 handler 背后的设计考量比大多数教程愿意承认的要深得多。

## 环境说明

继续在 QEMU + GCC 14 + CMake 环境下开发。前置条件：023-1 完成的 syscall_init() 和 syscall_entry 汇编已就绪。

## 第一步 -- 分发表：一张函数指针数组

我们先定义分发表的数据结构。核心是一个 256 元素的函数指针数组，索引就是系统调用号。

```cpp
using SyscallFn = int64_t(*)(uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
SyscallFn syscall_table[256] = {};
```

SyscallFn 是统一的函数签名——六个 uint64_t 参数，返回 int64_t。你可能会问：sys_exit 只需要 1 个参数（退出码），为什么要声明 6 个？因为分发表要求所有 handler 类型一致，多余的参数直接忽略就好。这种设计在操作系统里非常普遍——Linux 的 sys_call_table 也是类似的统一签名（asmlinkage long sys_xxx(...)），xv6 的 syscalls[] 数组虽然用的是 int (*)(void) 签名但参数是从 trapframe 里取的，本质上也是统一入口。

```cpp
extern "C" int64_t syscall_dispatch(uint64_t nr,
                                    uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6) {
    if (nr >= 256) return -1;
    auto fn = syscall_table[nr];
    if (fn == nullptr) return -1;
    return fn(a1, a2, a3, a4, a5, a6);
}
```

dispatch 的逻辑直接得不能再直接——先检查调用号范围，然后查表，空指针返回 -1，否则调用 handler。这种"边界检查 + 查表 + 调用"的三段式结构和 Linux 的 do_syscall_64()、xv6 的 syscall() 本质上完全一样。差异只在于 Linux 额外做了 syscall 退出时的安全性检查（如 tainted syscall 缓解 Spectre），xv6 在 RISC-V 上从 trapframe 取参数而不是直接用寄存器——这是 ISA 差异导致的接口差异，不是设计哲学的不同。

系统调用号我们参考 Linux x86-64 的约定，用 enum class 定义：SYS_read=0, SYS_write=1, SYS_yield=24, SYS_exit=60 等，目前共有约 20 个系统调用号定义。之所以照搬 Linux 的编号，是为了将来移植用户程序时减少修改量。Linux 之所以选这些特定的数字（比如 exit 是 60 而不是 2），纯粹是历史原因——这些数字从 Linux 最早的 x86-64 移植版本就定下来了，为了保证二进制兼容性一直没变过。

## 第二步 -- sys_write：第一次用户地址验证

```cpp
int64_t sys_write(uint64_t fd, uint64_t buf_virt, uint64_t count,
                  uint64_t, uint64_t, uint64_t) {
    if (buf_virt == 0) return -1;
    uint64_t bit47 = (buf_virt >> 47) & 1;
    uint64_t upper = buf_virt >> 48;
    if (bit47 == 0 && upper != 0) return -1;
    if (bit47 == 1 && upper != 0xFFFF) return -1;

    // Check VFS fd table first
    cinux::fs::FDTable& tbl  = cinux::fs::current_fd_table();
    cinux::fs::File*    file = tbl.get(static_cast<int>(fd));
    if (file != nullptr && file->inode != nullptr && file->inode->ops != nullptr) {
        // VFS write path...
    }

    if (fd == 1) {
        const auto* buf = reinterpret_cast<const char*>(buf_virt);
        for (uint64_t i = 0; i < count; i++) {
            kprintf("%c", buf[i]);
        }
        return static_cast<int64_t>(count);
    }
    return -1;
}
```

sys_write 是三个 handler 中安全设计最值得说的。第一道防线是 canonical address 验证：检查 buf_virt 的 bit 47，如果 bit 47=0 则 bit 48-63 必须全为 0（用户空间），如果 bit 47=1 则 bit 48-63 必须全为 1（内核空间）。如果用户程序传入一个非 canonical 地址，这个检查直接拦截。第二道防线是 VFS 文件表检查——如果 fd 有有效的 VFS 条目（比如管道），就走 VFS 写路径。只有当 fd=1 且没有 VFS 条目时，才走传统的 kprintf 串口输出。

说句实话，这个验证是最基本的。它只检查地址是否为 canonical，不检查该地址是否真的在当前进程的页表中有映射。Linux 用 copy_from_user() 配合 SMAP/SMEP 硬件机制做更完善的检查——如果用户传入一个未映射的地址，copy_from_user 会触发缺页异常并通过 fixup 表优雅地返回错误。Cinux 目前的实现是够用的，但离生产级安全还有距离。

## 第三步 -- sys_exit：防御性进程终止

```cpp
int64_t sys_exit(uint64_t code, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t) {
    auto* task = cinux::proc::Scheduler::current();
    if (task != nullptr) {
        task->state = cinux::proc::TaskState::Dead;
        cinux::lib::kprintf("[SYSCALL] sys_exit(%u) from tid=%u '%s'\n",
                            static_cast<unsigned>(code), static_cast<unsigned>(task->tid),
                            task->name);
    }
    if (cinux::proc::Scheduler::is_initialized()) {
        cinux::proc::Scheduler::yield();
    } else {
        cinux::lib::kprintf("[SYSCALL] sys_exit: no scheduler, halting.\n");
        while (1) { __asm__ volatile("cli; hlt"); }
    }
    return 0;
}
```

sys_exit 看起来简单，但有一个很容易被忽略的问题：在 023 阶段，调度器根本没启动。如果你直接调用 Scheduler::yield()，程序要么断言失败，要么访问空指针。所以我们做了一个运行时检查——Scheduler::is_initialized() 返回 false 时走 cli; hlt 死循环。这个分支在 023 阶段会走到，串口会打印 "no scheduler, halting" 后停机。到了 024 阶段调度器启动后，sys_exit 就会走 yield 路径，正确地切换到下一个就绪任务。

这种"根据运行环境做条件分支"的设计在操作系统中非常常见。Linux 的 early boot 阶段也有类似的防御性检查——在调度器完全初始化之前，很多内核服务是不能用的。xv6 的第一个用户进程 init 也是在调度器完全就绪后才创建的。区别在于 xv6 是通过严格的初始化顺序来保证的，而 Cinux 选择了运行时检查——后者更灵活但多了一个分支。

## 第四步 -- sys_yield：最简单的协作调度

```cpp
int64_t sys_yield(uint64_t, uint64_t, uint64_t,
                  uint64_t, uint64_t, uint64_t) {
    Scheduler::yield();
    return 0;
}
```

sys_yield 就是 yield 的 syscall 封装，一行委托。在 023 阶段它的用途有限（只有单任务），但从设计上说它是协作式多任务的基础——用户程序在长时间计算中主动让出 CPU，让其他任务有机会运行。Linux 的 sched_yield() 做的事情本质一样，但内部实现更复杂（涉及 CFS 调度器的树操作）。xv6 的 yield 也是类似的一行委托。

## 验证

把 sys_write、sys_exit、sys_yield 注册到分发表的工作由 syscall_init() 内部的 register_builtin_handlers() 统一完成。在 kernel_main 里调用 syscall_init() 后，所有内置 handler 就已注册。编译运行后串口应显示 syscall MSR 配置成功的日志。此时还没有用户程序调用 syscall——端到端的验证要等到下一章。

## 收尾

到这里，从分发表注册到具体 handler 执行的软件链路已经完成了。但如果你现在编译一个用户态 C++ 程序跑起来，大概率会收获一个 #GP——因为 GCC 默认用 SSE 指令优化，而内核还没启用 FPU。更坑的是，就算启用了 FPU，栈对齐也可能不对。这些"连环坑"就是我们下一章要解决的问题。

## 参考资料

- Intel SDM Vol.2D SYSCALL: 参数传递约定（RDI/RSI/RDX/R10/R8/R9）
- Linux sys_call_table: arch/x86/entry/syscalls/syscall_64.tbl -- 系统调用号定义
  - https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl
- Linux copy_from_user: include/linux/uaccess.h -- 用户地址安全访问机制
- xv6 syscall.c: 函数指针分发表和参数获取
  - https://pdos.csail.mit.edu/6.S081/2024/labs/syscall.html
- OSDev Wiki System Calls: https://wiki.osdev.org/System_Calls
