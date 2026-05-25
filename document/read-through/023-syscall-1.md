---
title: 023-syscall-1 · 系统调用
---

# 023-1 Read-through: syscall.S -- 汇编入口的完整代码走读

## 概览

本文聚焦 `kernel/arch/x86_64/syscall.S`，逐行讲解从 Ring 3 进入 Ring 0 的汇编入口。这个文件是整个 syscall 子系统最核心也最微妙的部分——SWAPGS 时机、栈切换方式、trap frame 布局、参数重排、调用约定适配，每一个细节出错都会导致 triple fault。在整个 023 tag 中，这是硬件交互最密集的一篇。

关键设计决策：使用 GS 段基址 + SWAPGS 做 per-CPU 栈切换（与 Linux 一致），trap frame 只保存 syscall 必需的寄存器（不保存全部 GPR），返回值暂存在 gs:16 槽位（避免破坏用户态 RBX）。

## 架构图

```
Ring 3 用户态                  Ring 0 内核态
┌──────────────┐               ┌──────────────────────────────┐
│  syscall 指令 │               │                              │
│  RAX=syscall# │               │  syscall_entry:              │
│  RDI/RSI/RDX │──SYSCALL──►  │   swapgs                     │
│  R10/R8/R9   │               │   movq %gs:0, %rsp (栈切换) │
│  RCX←RIP     │               │   push trap frame (96 bytes) │
│  R11←RFLAGS  │               │   call syscall_dispatch      │
└──────────────┘               │   restore trap frame         │
                               │   swapgs                     │
                               │   sysretq (→ Ring 3)         │
                               └──────────────────────────────┘

GS 数据页布局 (per-CPU):
┌────────┐
│ gs:0   │ kernel_rsp  (syscall 入口加载到 RSP)
│ gs:8   │ user_rsp    (暂存用户 RSP)
│ gs:16  │ ret_val     (暂存 syscall 返回值)
└────────┘
```

## 代码精讲

### 文件头部注释

文件顶部的注释块详细记录了 SYSCALL 指令的硬件行为和 trap frame 布局。当用户程序执行 syscall 时，CPU 自动完成：RCX 获得 RIP（返回地址）、R11 获得 RFLAGS、RIP 从 LSTAR MSR 加载、CS/SS 从 STAR[47:32] 派生、RFLAGS 被 SFMASK 屏蔽。这些是硬件行为，软件无法改变。注释还标注了完整的 trap frame 内存布局（rsp+0 到 rsp+88 的每个字段），这在调试时非常有用。

### SWAPGS 与栈切换

```asm
syscall_entry:
    swapgs
    movq %rsp, %gs:8
    movq %gs:0, %rsp
```

入口的前三行是整个 handler 的基石。swapgs 把 GS 段基址与 IA32_KERNEL_GS_BASE MSR 交换——执行后 GS 指向内核的 per-CPU 数据页。然后把当前 RSP（还是用户栈）存到 gs:8 暂存，再从 gs:0 加载内核栈指针到 RSP。这三行必须严格按顺序执行：先 swapgs 再访问 GS 偏移，先存用户 RSP 再覆盖。

### 构建 Trap Frame

```asm
    pushq %rbp
    pushq %rbx
    pushq %r9
    pushq %r8
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax
    pushq %r11
    pushq %rcx
    movq %gs:8, %rax
    pushq %rax
```

这里依次 push 了 12 个寄存器，构建了完整的 trap frame。push 顺序是从底到顶的——最后 push 的 user_rsp 在栈顶（rsp+0），最先 push 的 rbp 在栈底（rsp+88）。你会发现 callee-saved 寄存器（RBP、RBX）被放在最后 push，因为它们需要在恢复时最后弹出，中间不会破坏。用户 RSP 通过 gs:8 读回并 push 到栈顶——因为之前保存用户 RSP 时用的是 gs:8，这里通过 RAX 中转是因为不能直接 push 内存操作数到栈。

### 参数重排与 C 调用约定适配

```asm
    movq 72(%rsp), %rax
    subq $8, %rsp
    pushq %rax

    movq 40(%rsp), %rdi
    movq 48(%rsp), %rsi
    movq 56(%rsp), %rdx
    movq 64(%rsp), %rcx
    movq 72(%rsp), %r8
    movq 80(%rsp), %r9

    call syscall_dispatch
```

这段代码做了参数重排、第 7 个参数压栈和栈对齐。syscall 的第六个参数在 R9（对应 trap frame 的 rsp+72），但 C 函数 syscall_dispatch 接受 7 个参数（syscall 号 + 6 个参数），前 6 个通过寄存器传，第 7 个压栈。这里先从 trap frame 取出 R9 的值，然后 subq $8 对齐栈到 16 字节边界，再 push。push 和 subq 后 trap frame 的偏移量加了 16（不是 8），所以后面的 movq 指令用 40/48/56/64/72/80 的偏移来读取正确的字段。具体映射关系：rsp+40（原 RAX=syscall 号）→RDI，rsp+48（原 RDI=a1）→RSI，rsp+56（原 RSI=a2）→RDX，rsp+64（原 RDX=a3）→RCX，rsp+72（原 R10=a4）→R8，rsp+80（原 R8=a5）→R9。

### 恢复状态与 SYSRETQ 返回

```asm
    addq $16, %rsp

    movq %rax, %gs:16

    movq 0(%rsp), %rax
    movq %rax, %gs:8
    movq 8(%rsp), %rcx
    movq 16(%rsp), %r11
    movq 80(%rsp), %rbx

    addq $96, %rsp

    movq %gs:8, %rsp
    movq %gs:16, %rax
    swapgs
    sysretq
```

返回路径是入口的镜像。首先 `addq $16` 清理第 7 个参数（压栈的 R9 值）和栈对齐的 8 字节。然后把 dispatch 的返回值存到 gs:16——这是 GS 数据页的第三个槽位，专门用于暂存返回值。这样后续恢复 trap frame 时不会丢失返回值，也使得用户态 RBX 可以被正确恢复。

接下来从 trap frame 恢复关键状态：用户 RSP 存回 gs:8（之后切换栈要用）、用户 RIP 加载到 RCX（SYSRETQ 从这里恢复 RIP）、用户 RFLAGS 加载到 R11（SYSRETQ 从这里恢复 RFLAGS）、用户态 RBX 从 trap frame 的 rsp+80 位置恢复。然后 `addq $96` 释放整个 trap frame（12 个 slot * 8 字节），`movq %gs:8, %rsp` 切回用户栈，`movq %gs:16, %rax` 把返回值加载到 RAX（SYSRETQ 返回后用户程序从 RAX 拿到返回值），最后 swapgs 回用户 GS 基址，sysretq 返回 Ring 3。

你会发现这个返回路径比 Linux 简洁得多——Linux 需要恢复完整的 pt_regs（全部 GPR），还要处理 io bitmap、投机执行屏障等安全措施。Cinux 023 阶段只恢复了 syscall 必需的寄存器（RCX、R11、RSP、RBX），并用 gs:16 传递返回值。

## 设计决策

### 决策：使用 gs:16 暂存返回值

**问题**：dispatch 返回后需要保存返回值，然后在恢复 trap frame 和返回 Ring 3 之间传递它。同时需要正确恢复用户态的 RBX（callee-saved 寄存器）。

**本项目的做法**：用 gs:16 暂存返回值。dispatch 返回后 `movq %rax, %gs:16`，恢复完 trap frame（包括从 trap frame 恢复用户态 RBX）后，再 `movq %gs:16, %rax` 加载返回值。这个方案保证了用户态 RBX 不会被覆盖。

**备选方案**：用 RBX 暂存返回值（更简单但不恢复用户态 RBX）。

**为什么选了 gs:16**：虽然多用一个 GS 槽位，但能正确恢复用户态 RBX——callee-saved 寄存器如果被 syscall 隐式破坏，可能导致用户程序出现难以追踪的 bug。使用 gs:16 暂存是更健壮的方案。

## 扩展方向

- ⭐ 在 trap frame 中增加保存 R12-R15（完整 callee-saved），使 handler 可以安全使用这些寄存器（当前已经正确保存/恢复 RBX，但 R12-R15 未保存）
- ⭐⭐ 实现 syscall 入口的嵌套处理（当前不支持中断中再次 syscall）
- ⭐⭐⭐ 参考 Linux 的 pt_regs，在 trap frame 中保存全部 GPR，实现更完善的 ptrace 支持

## 参考资料

- Intel SDM Vol.2D "SYSCALL -- Fast System Call": SYSCALL 指令伪代码、MSR 行为、CS/SS 加载规则
  - https://www.felixcloutier.com/x86/syscall
- Intel SDM Vol.2D "SYSRET -- Return From Fast System Call": SYSRETQ 恢复 RIP/RFLAGS/CS/SS 的规则
- Linux arch/x86/entry/entry_64.S: entry_SYSCALL_64 的 SWAPGS + pt_regs 保存流程
  - https://github.com/torvalds/linux/blob/master/arch/x86/entry/entry_64.S
- OSDev Wiki System Calls: https://wiki.osdev.org/System_Calls
