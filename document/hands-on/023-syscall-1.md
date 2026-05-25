---
title: 023-syscall-1 · 系统调用
---

# 023-1 Hands-on: SYSCALL/SYSRET 基础设施 -- MSR 配置与入口汇编

## 导语

上一章（022）我们打通了 Ring 3 切换，用户程序能跑起来了，但只能执行特权指令触发 #GP 证明隔离有效。真正的用户程序需要向内核请求服务——打印字符、退出进程、读写文件——这些都要走系统调用。本章我们要在内核里搭建 SYSCALL 指令的完整基础设施：配置三个关键 MSR、编写 Ring 3 到 Ring 0 的入口汇编、建立内核栈切换机制。完成之后，用户态的 syscall 指令将正确地跳入内核代码执行。

前置知识：022 已完成的 STAR MSR/EFER.SCE 配置、SYSRETQ 返回 Ring 3 的机制、GS 段基址与 SWAPGS。

## 概念精讲

### SYSCALL 指令做了什么

当用户程序执行 syscall 指令时，CPU 硬件自动完成以下操作：把当前 RIP 存入 RCX（这样返回时知道从哪继续），把当前 RFLAGS 存入 R11，然后从 LSTAR MSR 加载新的 RIP——这就是内核的入口地址。同时 CPU 根据 STAR MSR 设置内核态的 CS 和 SS 段选择子，并把 CPL 切换为 0（Ring 0）。还有一个关键细节：RFLAGS 会和 SFMASK MSR 取反后做 AND 操作，也就是说 SFMASK 中置 1 的位在进入内核时会被清除。Cinux 把 SFMASK 设为 0x200（bit 9，即 IF 中断允许标志），这意味着进入 syscall handler 时中断是关闭的，防止在内核栈还没切换好时被中断打断。

但是——这里是个大坑——SYSCALL **完全不碰 RSP**。它不帮你保存用户栈指针，也不帮你切换到内核栈。中断（INT 指令）会自动通过 TSS 切换栈，但 syscall 不会。这个设计是为了极致的速度，代价就是软件必须自己搞定栈切换。Linux 用 SWAPGS + per-CPU 数据解决这个问题，我们也采用同样的方案。

### 三大 MSR 各司其职

SYSCALL 的行为由三个 MSR 控制。LSTAR（地址 0xC0000082）存的是 syscall 入口函数的地址，CPU 执行 syscall 时直接跳到这里。STAR（地址 0xC0000081）的高 32 位编码了段选择子：[47:32] 是 SYSCALL 方向的 CS 基选择子（设为 0x10，即 GDT_KERNEL_CODE），[63:48] 是 SYSRET 方向的 CS 基选择子（设为 0x23，即 GDT_SYSRET_BASE）。CPU 会自动从 CS 基选择子派生出 SS（CS+8）。SYSCALL 方向：CS=0x10，SS=0x10+8=0x18。SYSRET 方向：CPU 计算 CS=0x23+16|3=0x33（用户代码段，RPL=3），SS=0x23+8|3=0x2B（用户数据段，RPL=3）。SFMASK（地址 0xC0000084）指定进入内核时要清除哪些 RFLAGS 位。

还有一个 MSR 很重要但不是给 SYSCALL 直接用的：IA32_KERNEL_GS_BASE（0xC0000102）。这是 SWAPGS 指令的交换目标——执行 swapgs 后，GS 段基址就指向了内核的 per-CPU 数据页，这个页里存着内核栈指针。

### SWAPGS 与 Per-CPU GS 数据页

我们的设计是分配一个物理页作为 GS 数据页，布局非常简单：偏移 0 存内核栈指针（syscall 入口时加载到 RSP），偏移 8 存用户 RSP 的暂存区，偏移 16 存返回值的暂存区。通过 wrmsr 把这个页的虚拟地址写入 IA32_KERNEL_GS_BASE MSR，之后 swapgs 就能让我们用 %gs:0、%gs:8 和 %gs:16 访问这些数据。

## 动手实现

### Step 1: 定义 syscall 号常量

**目标**：创建一个头文件，定义系统调用号枚举，内核和用户态共享。

**设计思路**：系统调用号就是一张查找表的索引。我们参考 Linux x86-64 的编号约定，这样以后移植用户程序时会轻松很多。定义一个枚举类型（SYS_read=0, SYS_write=1, SYS_yield=24, SYS_exit=60），加上一个分发表大小常量（256 个槽位）。

**实现约束**：头文件放在 kernel/syscall/ 目录下，使用 enum class 保证类型安全，值类型用 uint64_t。分发表大小用 constexpr 定义。

**验证**：写一个小测试，检查枚举值是否正确（SYS_read==0, SYS_write==1, SYS_exit==60, SYS_yield==24），分发表大小为 256。

### Step 2: 编写 syscall_init() -- MSR 配置

**目标**：初始化 SYSCALL 基础设施，配置 LSTAR、STAR、SFMASK 三个 MSR，并注册所有系统调用 handler。

**设计思路**：init 函数要完成三件事。第一步，保存调用者传入的 kernel_rsp 到全局变量。第二步，清空分发表（把 256 个函数指针全设为 nullptr）并配置 MSR：LSTAR 写入 syscall_entry 汇编入口的地址；STAR 的 [47:32] 和 [63:48] 都写入 0x08（GDT_KERNEL_CODE）；SFMASK 写 0x200 清除 IF 位。第三步，注册内置的 syscall handler（sys_write、sys_exit、sys_yield 等）。

**实现约束**：syscall_init 接受一个 uint64_t kernel_rsp 参数——调用者负责传入正确的内核栈指针。wrmsr 内联汇编接受三个参数——ECX=MSR 地址，EDX:EAX=64 位值（高 32 位在 EDX，低 32 位在 EAX）。syscall_entry 需要声明为 extern "C" 的汇编函数。

**踩坑预警**：STAR MSR 的 [47:32] 是 SYSCALL 方向，[63:48] 是 SYSRET 方向，别搞反了。当前代码两个方向都使用 0x08（GDT_KERNEL_CODE）作为基——SYSCALL 时 CPU 派生 CS=0x08、SS=0x10，SYSRET 时派生 CS=0x1B（0x08+16|3）、SS=0x13（0x08+8|3）。SFMASK 只清除对应位为 1 的 RFLAGS 标志，设置 0x200 就是只清 IF。

**验证**：用 rdmsr 读回 LSTAR 确认非零、读回 STAR 确认 [47:32]==0x08 和 [63:48]==0x08。

### Step 3: 编写 syscall_entry 汇编入口

**目标**：实现 Ring 3 到 Ring 0 的入口汇编，完成 SWAPGS、栈切换、寄存器保存、分发调用、状态恢复、SYSRETQ 返回。

**设计思路**：入口函数分为十个步骤，严格按顺序执行。第一步 swapgs 激活内核 GS 基址。第二步把用户 RSP 存到 GS:8（暂存），从 GS:0 加载内核栈指针到 RSP。第三步在内核栈上构建 trap frame——依次 push 用户 RBP、RBX（callee-saved）、R9、R8、R10、RDX、RSI、RDI（六个参数寄存器）、RAX（syscall 号）、R11（用户 RFLAGS）、RCX（用户 RIP），最后把用户 RSP（从 GS:8 读回）也 push 上去，共 12 个 slot。

第四步调用 C 函数 syscall_dispatch，把 trap frame 里的参数按 System V ABI 顺序加载到正确的寄存器。注意 syscall 的第四个参数在 R10（CPU 不破坏 R10），但 C 调用约定第四个参数在 RCX，需要手动调整。第六个参数（R9）要压到栈上传递（C ABI 只有 6 个寄存器参数，我们传 7 个：syscall 号 + 6 个参数）。压栈前需要先 subq $8 对齐栈到 16 字节边界，push 后总共增加了 16 字节偏移。

调用完成后保存返回值到 GS 数据页的 gs:16 槽位，然后反向恢复 trap frame：取出用户 RSP 存到 GS:8，取出用户 RIP 到 RCX（SYSRETQ 从 RCX 恢复 RIP），取出 RFLAGS 到 R11（SYSRETQ 从 R11 恢复 RFLAGS），同时从 trap frame 恢复用户态 RBX。释放 trap frame（addq $96, %rsp），从 GS:8 恢复用户 RSP，从 gs:16 恢复返回值到 RAX，swapgs 回用户 GS 基址，最后 sysretq 返回 Ring 3。

**踩坑预警**：SYSRETQ 从 RCX 恢复 RIP、从 R11 恢复 RFLAGS，所以恢复这两个寄存器的值必须在 sysretq 之前完成。返回值暂存在 gs:16 槽位，同时用户态 RBX 会从 trap frame 正确恢复——这意味着 syscall 不会破坏用户程序的 RBX。

**验证**：编译通过后，在 QEMU 中运行，串口应该能看到 SYSCALL 初始化成功的日志。目前还没有用户程序调用 syscall，所以不会有实际的 syscall 入口触发。

## 构建与运行

将 syscall.S 和 syscall.cpp 加入 CMakeLists.txt 的 big_kernel_common 源文件列表。编译后用 QEMU 启动，观察串口输出是否有 `[SYSCALL] LSTAR=... STAR configured SFMASK=0x200` 字样。

## 调试技巧

- 如果 syscall_init 触发 #GP：检查 MSR 地址是否正确（0xC0000081/82/84），检查 EFER.SCE 是否已启用
- 如果 syscall 入口触发 triple fault：检查 GS base 是否已设置（KERNEL_GS_BASE MSR），检查 gs:0 是否有有效的内核栈指针
- 用 QEMU `-d int` 可以捕获异常详细信息

## 本章小结

| 概念 | 说明 |
|------|------|
| SYSCALL | 用户态→内核态快速切换，硬件自动保存 RCX/R11，不保存 RSP |
| LSTAR | SYSCALL 入口地址 MSR (0xC0000082) |
| STAR | 段选择子 MSR (0xC0000081)，编码 SYSCALL/SYSRET 两个方向 |
| SFMASK | RFLAGS 屏蔽 MSR (0xC0000084)，置 1 的位进入内核时被清除 |
| SWAPGS | 交换 GS 基址与 KERNEL_GS_BASE MSR，用于访问 per-CPU 数据 |
| Trap Frame | 12 slot x 8 字节 = 96 字节的寄存器保存区 |
