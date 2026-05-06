# 从 Ring 3 打通内核 -- SYSCALL 指令与 MSR 配置

> 标签：x86-64, syscall, MSR, SWAPGS, Ring 0/Ring 3
> 前置：[022 Ring 3 User Mode](022-ring3-usermode-1.md)

## 前言

上一章我们搞定了从 Ring 0 跳到 Ring 3 执行用户代码，验证了特权指令隔离——用户态一执行 cli 就乖乖触发 #GP。但真正的用户程序不可能只跑空循环然后触发异常，它得向内核请求服务：我要打印字符、我要退出进程、我要读写文件。这些东西统称为"系统调用"（system call），是用户态和内核态之间唯一正式的沟通渠道。

x86-64 上做系统调用有几种方案——软中断（int 0x80）、sysenter/sysexit（Intel 的 32 位方案）、以及 syscall/sysret（AMD 设计，x86-64 标准方案）。Linux 从 2.6 开始在 x86-64 上全面采用 syscall/sysret，因为它比中断方式快得多——不需要查 IDT、不需要完整的栈帧保存、不检查权限的冗余步骤。我们也跟 Linux 走同一条路，这样以后理解 Linux 的系统调用路径会轻松很多。

说实话，从 022 的"能跳到 Ring 3"到 023 的"能从 Ring 3 调用内核"，看起来只是一步之遥，但这个 gap 涉及的硬件机制比上一章还多。SWAPGS、per-CPU 数据页、SYSCALL MSR 三件套、手动栈切换——每一个环节都至关重要，而且出错的后果是 triple fault，连错误信息都看不到。

## 环境说明

我们在 QEMU 上跑 Cinux 内核，工具链是 GCC 14 + CMake，内核用 C++23 编写（-ffreestanding -fno-exceptions）。当前处于 milestone 023，前置条件是 022 已完成——GDT/TSS/IDT 就绪、STAR MSR 已配置、EFER.SCE 已启用、SYSRETQ 跳转 Ring 3 已验证。

## 第一步 -- 理解 SYSCALL 指令的硬件行为

在写任何代码之前，我们必须搞清楚 CPU 在执行 syscall 指令时自动做了什么。Intel SDM Vol.2D 的 SYSCALL 条目给出了完整的伪代码，核心操作如下：RCX 被赋值为当前 RIP（也就是 syscall 的下一条指令地址，返回时要用），R11 被赋值为当前 RFLAGS，然后 RIP 从 IA32_LSTAR MSR（地址 0xC0000082）加载——这就是内核的入口地址。同时 CS 和 SS 从 IA32_STAR MSR（0xC0000081）的 [47:32] 位域派生，CPL 切换为 0。RFLAGS 则与 IA32_FMASK MSR（0xC0000084）取反后做 AND——SFMASK 中为 1 的位在进入内核时会被清除。

这里有一个非常关键的细节，也是很多人第一次实现 syscall 时踩的坑：**SYSCALL 完全不碰 RSP**。它不保存用户栈指针，不切换到内核栈。中断指令（int）会通过 TSS.RSP0 自动切换栈，但 syscall 为了极致速度省掉了这一步。这意味着软件必须在入口的第一时间自己搞定栈切换——否则后续的 push 操作会写到用户栈上，整个内核状态就乱了。

Linux 的解决方案是 SWAPGS + per-CPU 数据。SWAPGS 这条指令交换 GS 段基址和 IA32_KERNEL_GS_BASE MSR 的值——执行前 GS 指向用户态 TLS，执行后 GS 指向内核的 per-CPU 数据结构，里面有当前任务的内核栈指针。Cinux 采用同样的设计，入口第一条指令就是 swapgs，然后通过 `movq %gs:0, %rsp` 加载内核栈。

## 第二步 -- 配置 SYSCALL MSR 三件套

现在我们来写 syscall_init()。这个函数负责配置三个 MSR 并初始化分发表。

```cpp
void syscall_init(uint64_t kernel_rsp) {
    g_syscall_kernel_rsp = kernel_rsp;
    for (uint64_t i = 0; i < SYSCALL_TABLE_SIZE; i++) {
        syscall_table[i] = nullptr;
    }
```

调用者负责传入正确的内核栈指针（kernel_rsp）。这个值会在调度器做上下文切换时写入 GS 数据页的 gs:0 偏移。然后清空分发表，256 个槽位全部初始化为 nullptr。

接下来是 MSR 配置。STAR MSR 的高 32 位编码了段选择子：[47:32] 给 SYSCALL 方向（进入内核），[63:48] 给 SYSRET 方向（返回用户态）。当前代码两个方向都设为 0x08（GDT_KERNEL_CODE）——SYSCALL 时 CPU 派生 CS=0x08（内核代码段）和 SS=0x08+8=0x10（内核数据段）。SYSRET 时 CPU 自动派生 CS=0x08+16|3=0x1B（用户 64 位代码段，RPL=3）和 SS=0x08+8|3=0x13（用户数据段，RPL=3）。

LSTAR 写入 syscall_entry 的地址——这是我们接下来要写的汇编入口函数。SFMASK 设为 0x200（bit 9 = IF 中断允许标志），这意味着每次 syscall 进入时中断自动关闭。这是一个重要的安全措施——在栈切换完成之前如果被中断打断，内核栈可能还没准备好，后果不堪设想。所有内置的 syscall handler 通过 register_builtin_handlers() 在 syscall_init 内部统一注册，不需要在 kernel_main 中逐个注册。

## 第三步 -- 编写 syscall_entry 汇编入口

这是整个 023 最核心的代码。入口函数必须完成：SWAPGS、栈切换、寄存器保存、调用分发器、恢复状态、SYSRETQ 返回。任何一步出错都会 triple fault。

入口的第一件事是 swapgs 激活内核 GS 基址，然后把用户 RSP 存到 gs:8 暂存，从 gs:0 加载内核栈指针。这三步必须在 swapgs 之后、任何 push 之前完成。接下来在内核栈上构建 trap frame——保存用户 RSP、RCX（RIP）、R11（RFLAGS）、RAX（syscall 号）、六个参数寄存器（RDI/RSI/RDX/R10/R8/R9）以及两个 callee-saved 寄存器（RBX/RBP），共 12 个 slot。

调用 syscall_dispatch 之前需要做参数重排。syscall 的第四个参数在 R10（CPU 设计如此），但 C 调用约定的第四个参数在 RCX——需要从 trap frame 里取出 R10 的值放入 RCX。第六个参数（R9）需要压栈作为第七个 C 参数（syscall 号 + 6 个参数 = 7 个参数，第 7 个压栈）。

调用完成后，返回值暂存到 gs:16（GS 数据页的返回值暂存槽位）。然后依次恢复用户 RSP（存回 gs:8）、用户 RIP（到 RCX，SYSRETQ 从这里加载）、用户 RFLAGS（到 R11），以及用户态 RBX（从 trap frame 中恢复）。释放 trap frame 后从 gs:8 恢复用户 RSP，从 gs:16 恢复返回值到 RAX，swapgs 回用户 GS，最后 sysretq 返回 Ring 3。使用 gs:16 暂存返回值的好处是可以正确地从 trap frame 恢复用户态 RBX——如果直接用 RBX 暂存返回值，用户态的 RBX 就会丢失，可能导致用户程序出现难以追踪的 bug。

你会发现这个过程和 Linux 的 entry_SYSCALL_64 高度相似——swapgs、per-CPU 栈切换、寄存器保存到栈上、调用分发表、恢复并 sysretq。主要差异在于 Linux 保存更完整的 pt_regs（全部 GPR），而 Cinux 只保存 syscall 必需的寄存器。xv6（RISC-V 版）的做法完全不同——用 ecall 触发异常进入 trap handler，通过内存中的 trapframe 结构传参，而不是像 x86-64 这样通过寄存器直接传参。这些差异反映了不同 ISA 的设计哲学：x86-64 追求 syscall 路径的极致性能，RISC-V 则保持了统一的异常处理机制。

## 验证

编译运行后，串口应显示 `[SYSCALL] LSTAR=0x... STAR configured SFMASK=0x200 (clear IF)`。这表明三个 MSR 已正确写入。可以尝试用 rdmsr 读回验证：LSTAR 应该是 syscall_entry 的地址，STAR 的 [47:32] 应该是 0x08（GDT_KERNEL_CODE），[63:48] 也应该是 0x08。

## 收尾

到这里，从 Ring 3 通过 SYSCALL 进入 Ring 0 的"通道"已经打通了。但 syscall_dispatch 收到调用号后怎么知道该调用谁？这个分发表机制和具体的 handler 实现，就是我们下一章要解决的问题。

## 参考资料

- Intel SDM Vol.2D "SYSCALL -- Fast System Call": SYSCALL 指令伪代码，RCX/R11 自动保存，LSTAR/STAR/SFMASK MSR 行为
  - https://www.felixcloutier.com/x86/syscall
- Intel SDM Vol.4: IA32_STAR (0xC0000081), IA32_LSTAR (0xC0000082), IA32_FMASK (0xC0000084), IA32_KERNEL_GS_BASE (0xC0000102) MSR 定义
- Linux arch/x86/entry/entry_64.S: entry_SYSCALL_64 的 SWAPGS + pt_regs 流程
  - https://github.com/torvalds/linux/blob/master/arch/x86/entry/entry_64.S
- Linux Insides - SysCall: https://0xax.gitbooks.io/linux-insides/content/SysCall/linux-syscall-2.html
- OSDev Wiki System Calls: https://wiki.osdev.org/System_Calls
- xv6 syscall.c: https://pdos.csail.mit.edu/6.S081/2024/labs/syscall.html
