---
title: 023-syscall-2 · 系统调用
---

# 023-2 Hands-on: 系统调用分发与 Handler 实现

## 导语

上一章我们搭好了 SYSCALL 的 MSR 配置和汇编入口，用户态的 syscall 指令能正确跳到内核的 syscall_entry，保存完寄存器后调用 syscall_dispatch。现在的问题是——dispatch 怎么知道该调用哪个函数？本章我们实现分发表机制和三个具体的系统调用 handler：sys_write（输出到串口）、sys_exit（终止进程）、sys_yield（主动让出 CPU）。完成之后，从 syscall 指令到具体 handler 的整条链路就打通了。

前置知识：023-1 完成的 syscall_entry 汇编和 syscall_init()。

## 概念精讲

### 分发表 (Dispatch Table)

分发表的本质就是一个函数指针数组，索引就是系统调用号。这种设计在操作系统里非常常见——Linux 有 sys_call_table，xv6 有 syscalls[] 数组。好处是添加新系统调用只需要两步：在枚举里加一个号、在 init 时注册一个 handler，完全不需要改 dispatch 逻辑。我们的表有 256 个槽位，每个槽位存一个函数指针，未注册的槽位是 nullptr，dispatch 时遇到 nullptr 就返回 -1。

函数指针的统一签名是六个 uint64_t 参数返回 int64_t。这和 Linux 的设计一致：参数最多 6 个，通过寄存器传递（RDI/RSI/RDX/R10/R8/R9），返回值在 RAX。虽然不是所有 syscall 都需要 6 个参数（比如 sys_exit 只用第一个），但统一签名让分发表简洁。

### 用户地址验证

当用户程序调用 sys_write(fd, buf, count) 时，buf 是用户态的虚拟地址。内核必须验证这个地址确实指向用户空间而不是内核空间——否则恶意程序可以传入内核地址，让 sys_write 泄露或破坏内核数据。验证规则是 canonical address 检查：检查 bit 47 的值，如果 bit 47=0 则 bit 48-63 必须全为 0（用户空间地址），如果 bit 47=1 则 bit 48-63 必须全为 1（内核空间地址）。这种检查比简单的阈值比较更严格，确保地址在 x86-64 的 canonical 范围内。这个检查虽然简单但至关重要，是操作系统安全的基础。

### sys_exit 的两难

sys_exit 的语义是终止当前进程。但在 023 阶段，调度器可能还没初始化——如果直接调用 Scheduler::yield() 而 scheduler 不存在，系统直接崩溃。解决方案是检查调度器是否已初始化：如果已初始化就走正常流程（标记 Dead + yield），否则就打印提示信息然后 cli; hlt 死循环。这种防御性设计使得 syscall 模块可以在有/无调度器的环境下都能工作，是一个很好的解耦实践。

## 动手实现

### Step 1: 实现分发表和 dispatch 函数

**目标**：定义 syscall_table 数组和 syscall_dispatch 函数，完成系统调用号到 handler 的路由。

**设计思路**：dispatch 函数的逻辑非常直接——先检查系统调用号是否在表范围内（0 到 255），然后从表中取出函数指针，如果是 nullptr 就返回 -1，否则调用这个函数指针并把返回值传回。分发表用匿名命名空间包裹，对外只暴露注册函数和 dispatch 函数。

**实现约束**：分发表是 256 个元素的数组，类型是函数指针。syscall_dispatch 要声明为 extern "C"，因为它是从汇编调用的。dispatch 函数接受 7 个参数：系统调用号 + 6 个通用参数。

**验证**：注册一个测试用的 handler 到某个高号槽位，调用 dispatch 并确认返回值正确。测试未注册的槽位返回 -1，超范围号也返回 -1。

### Step 2: 实现 sys_write handler

**目标**：实现 write 系统调用，将用户缓冲区的内容输出到串口和 Console。

**设计思路**：sys_write 接受三个有意义的参数——文件描述符 fd、用户缓冲区虚拟地址 buf_virt、字节数 count。首先做地址验证（canonical address 检查），然后检查 fd 是否有 VFS 文件条目（如管道），如果有则走 VFS 写路径。如果没有 VFS 条目，且 fd=1（stdout），则走传统 kprintf 输出路径。其他情况返回 -1。

**实现约束**：函数签名必须匹配 SyscallFn 类型（6 个 uint64_t 参数，返回 int64_t）。地址验证使用 canonical address 检查（检查 bit 47 和 bit 48-63 的一致性）。优先检查 VFS fd 表，如果 fd 有有效的 VFS 文件条目就走 VFS 写路径；否则 fd==1 走传统 kprintf 路径。

**踩坑预警**：buf_virt 是用户态虚拟地址，在内核态可以直接访问是因为我们的用户地址空间在低半区有映射。但在更完善的内核中，这里应该用 copy_from_user 之类的函数配合缺页处理。当前实现够用但不安全。

**验证**：直接调用 sys_write(1, valid_addr, count)，确认返回 count。调用 sys_write(1, kernel_addr, ...) 确认返回 -1（canonical address 检查拦截）。

### Step 3: 实现 sys_exit handler

**目标**：实现 exit 系统调用，标记当前进程为 Dead 并让出 CPU。

**设计思路**：sys_exit 第一个参数是退出码。首先尝试获取当前 task 指针，如果非空就把状态设为 Dead，并打印退出信息。然后检查调度器状态——已初始化就调用 yield() 让调度器切换到下一个任务，没初始化就打印提示后 cli; hlt 死循环。这里的设计意图是：在 023 阶段没有调度器，用户程序 exit 后系统停机是合理的；等到 024 阶段启用调度器后，exit 就能正确地让出 CPU 给其他进程。

**实现约束**：函数不返回（死循环分支），但签名必须包含 return 0 以消除编译警告。使用 Scheduler::is_initialized() 做条件判断。

**踩坑预警**：sys_exit 在有调度器的情况下**不会返回**——yield 会触发 context_switch 到另一个任务。所以 return 0 这行代码在有调度器时永远不会执行。但在没调度器的测试场景下会走 cli; hlt 死循环。

**验证**：创建一个测试 task，将其状态设为 Running，然后手动设置状态为 Dead（不直接调用 sys_exit 以免触发不可控的 yield），确认状态转换正确。

### Step 4: 实现 sys_yield handler

**目标**：实现 yield 系统调用，让当前进程主动放弃 CPU。

**设计思路**：最简单的 handler——直接调用 Scheduler::yield()。没什么额外逻辑。返回 0。和 sys_exit 一样，调用 yield 后可能不会立即返回（取决于调度器的选择）。

**实现约束**：直接委托给 Scheduler::yield()。

**验证**：确认 handler 返回 0，类型签名匹配 SyscallFn。

### Step 5: 注册所有 handler

**目标**：在 kernel_main 的初始化序列中，调用 syscall_init() 后注册所有 syscall handler。

**设计思路**：在 kernel_main 里，usermode_init() 之后调用 syscall_init()。syscall_init 内部会自己读取当前 RSP 作为内核栈指针，然后配置 LSTAR/STAR/SFMASK MSR，最后调用 register_builtin_handlers() 一次性注册所有内置的 syscall handler（包括 sys_write、sys_exit、sys_yield 等）。不需要在 kernel_main 里逐个调用 syscall_register。

**实现约束**：syscall_init 不接受参数，内部通过内联汇编读取 RSP。所有 handler 在 syscall_init 内部通过 register_builtin_handlers() 统一注册。

**验证**：编译运行，串口输出应包含 `[SYSCALL] LSTAR=... STAR configured SFMASK=0x200`。

## 构建与运行

将 sys_write.cpp、sys_exit.cpp、sys_yield.cpp 加入 CMakeLists.txt。编译后 QEMU 启动，观察 syscall 初始化日志。

## 调试技巧

- 如果 dispatch 返回 -1 但 handler 已注册：检查系统调用号是否匹配枚举值，确认 register 和 dispatch 用的是同一个号
- 如果 sys_write 没有输出：检查 fd 是否为 1、buf_virt 是否通过 canonical address 验证
- 如果 sys_exit 后系统卡死：检查是否走了"无调度器"分支——这是 023 阶段的预期行为

## 本章小结

| 组件 | 说明 |
|------|------|
| syscall_table[256] | 函数指针分发表，索引为系统调用号 |
| syscall_dispatch() | 从汇编调用，查表分发，未注册返回 -1 |
| sys_write | fd=1 输出到串口，验证 buf_virt 为 canonical address |
| sys_exit | 标记 Dead + yield（有调度器）或 cli; hlt（无调度器） |
| sys_yield | 直接调用 Scheduler::yield() |
| syscall_register() | 注册 handler 到分发表指定槽位 |
