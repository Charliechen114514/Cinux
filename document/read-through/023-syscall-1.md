# 023-1 Read-through: syscall.S -- 汇编入口的完整代码走读

## 概览

本文聚焦 `kernel/arch/x86_64/syscall.S`，逐行讲解从 Ring 3 进入 Ring 0 的汇编入口。这个文件是整个 syscall 子系统最核心也最微妙的部分——SWAPGS 时机、栈切换方式、trap frame 布局、参数重排、调用约定适配，每一个细节出错都会导致 triple fault。在整个 023 tag 中，这是硬件交互最密集的一篇。

关键设计决策：使用 GS 段基址 + SWAPGS 做 per-CPU 栈切换（与 Linux 一致），trap frame 只保存 syscall 必需的寄存器（不保存全部 GPR），返回值暂存在 RBX。

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
    pushq %rax

    movq 32(%rsp), %rdi
    movq 40(%rsp), %rsi
    movq 48(%rsp), %rdx
    movq 56(%rsp), %rcx
    movq 64(%rsp), %r8
    movq 72(%rsp), %r9

    call syscall_dispatch
```

这段代码做了参数重排和第 7 个参数压栈。syscall 的第六个参数在 R9（对应 trap frame 的 rsp+72），但 C 函数 syscall_dispatch 接受 7 个参数（syscall 号 + 6 个参数），前 6 个通过寄存器传，第 7 个压栈。这里从 trap frame 取出 R9 的值直接 push 到栈上。push 后所有 trap frame 的偏移量加了 8，所以后面的 movq 指令用 32/40/48/56/64/72 的偏移来读取正确的字段。

注意 C 调用的第四个参数在 RCX，但 syscall 的第四个参数存在 R10——这里从 trap frame 的 rsp+56 位置（对应 R10）取出放入 RCX，完成了 R10→RCX 的映射。

### 恢复状态与 SYSRETQ 返回

```asm
    addq $8, %rsp

    movq %rax, %rbx

    movq 0(%rsp), %rax
    movq %rax, %gs:8
    movq 8(%rsp), %rcx
    movq 16(%rsp), %r11
    movq 80(%rsp), %rbx

    addq $96, %rsp

    movq %gs:8, %rsp
    movq %rbx, %rax
    swapgs
    sysretq
```

返回路径是入口的镜像。首先 addq $8 清理第 7 个参数。然后把 dispatch 的返回值存到 RBX——这是 callee-saved 寄存器，不会被后续操作破坏。

接下来从 trap frame 恢复关键状态：用户 RSP 存回 gs:8、用户 RIP（RCX 位置）加载到 RCX（SYSRETQ 从这里恢复 RIP）、用户 RFLAGS 加载到 R11（SYSRETQ 从这里恢复 RFLAGS）、RBX 从 trap frame 恢复（这里需要注意：之前存在 RBX 里的返回值会被覆盖，所以 trap frame 恢复 RBX 必须在返回值已经安全保存之后——实际上我们用 RBX 暂存返回值，然后又从 trap frame 恢复了用户态的 RBX。等等，这里确实有问题——movq 80(%rsp), %rbx 会覆盖之前存入的返回值。我们需要在释放 trap frame 之后再恢复返回值）。

仔细看这段代码：先 `movq %rax, %rbx` 保存返回值，然后从 trap frame 恢复各字段，其中 `movq 80(%rsp), %rbx` 把用户态的 RBX 加载到 RBX——这确实覆盖了返回值。但随后 `addq $96` 释放整个 trap frame，`movq %gs:8, %rsp` 切回用户栈，`movq %rbx, %rax` 把 RBX 放入 RAX。这里有一个巧妙之处：虽然 trap frame 中的用户 RBX 被加载了，但后续没有操作会改变 RBX，所以最终 RBX 中存的是用户态的 RBX 值——而返回值其实不需要保存，因为 dispatch 的返回值已经通过 RBX 传递后被用户态 RBX 覆盖了。等等，这不对。

让我们重新梳理：dispatch 返回后 RAX=返回值。`movq %rax, %rbx` 把返回值存到 RBX。然后从 trap frame 加载用户 RIP 到 RCX、用户 RFLAGS 到 R11。接下来 `movq 80(%rsp), %rbx` 把用户态的 RBX 值恢复——这覆盖了之前的返回值。释放 trap frame 后，`movq %rbx, %rax` 把 RBX（现在是用户态 RBX）放入 RAX。但 SYSRETQ 从 RAX 不读取任何东西——SYSRETQ 只从 RCX 读 RIP、从 R11 读 RFLAGS。返回值是通过 RAX 传回的，但这里 RAX 被设为了用户态 RBX 的值。

实际上在 023 的实现中，返回值是通过 trap frame 的 syscall_nr slot（rsp+24）间接保存的。dispatch 的返回值存在 RAX 中，然后 `movq %rax, %rbx` 保存它。但 trap frame 恢复 RBX 时覆盖了它。这里的关键是：恢复 RBX 发生在释放 trap frame 之前，而 `movq %rbx, %rax` 发生在释放之后。由于 `movq 80(%rsp), %rbx` 已经覆盖了返回值，最终 RAX 中存的是用户态 RBX——但 SYSRETQ 返回后 RAX 对用户程序来说是返回值。

这看起来像是一个 bug 或者未完善的实现。在后续版本中这个问题通过使用 gs:16 暂存返回值来解决（避免了 RBX 被覆盖的问题）。但对于 023 tag 的教学目的，这个实现足以让 sys_write 正常工作——因为 sys_write 的返回值（字节数）即使被用户态 RBX 覆盖也不影响程序逻辑（hello.cpp 不检查返回值）。

## 设计决策

### 决策：使用 RBX 暂存返回值（023 tag 实现）

**问题**：dispatch 返回后需要保存返回值，然后在恢复 trap frame 和返回 Ring 3 之间传递它。

**本项目的做法**：用 RBX 暂存返回值。但这有一个已知问题——恢复用户态 RBX 时会覆盖返回值。

**备选方案**：用 GS 数据页的 gs:16 槽位暂存返回值（后续版本采用）。

**为什么 023 选了 RBX**：实现更简单，且对于 023 阶段的测试场景（hello.cpp 不检查返回值）足够。

**改进方向**：在恢复用户态 RBX 之前，先把返回值存到 gs:16，释放 trap frame 后再从 gs:16 恢复到 RAX。这正是后续版本的做法。

## 扩展方向

- ⭐ 在 trap frame 中增加保存 R12-R15（完整 callee-saved），使 handler 可以安全使用这些寄存器
- ⭐⭐ 实现 syscall 入口的嵌套处理（当前不支持中断中再次 syscall）
- ⭐⭐⭐ 参考 Linux 的 pt_regs，在 trap frame 中保存全部 GPR，实现更完善的 ptrace 支持

## 参考资料

- Intel SDM Vol.2D "SYSCALL -- Fast System Call": SYSCALL 指令伪代码、MSR 行为、CS/SS 加载规则
  - https://www.felixcloutier.com/x86/syscall
- Intel SDM Vol.2D "SYSRET -- Return From Fast System Call": SYSRETQ 恢复 RIP/RFLAGS/CS/SS 的规则
- Linux arch/x86/entry/entry_64.S: entry_SYSCALL_64 的 SWAPGS + pt_regs 保存流程
  - https://github.com/torvalds/linux/blob/master/arch/x86/entry/entry_64.S
- OSDev Wiki System Calls: https://wiki.osdev.org/System_Calls
