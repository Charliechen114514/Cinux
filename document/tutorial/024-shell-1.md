# 从 Syscall 到 Shell（上）：GDT 重构与 SYSRETQ 的坑

> 标签：x86_64, GDT, SYSCALL, SYSRETQ, STAR MSR, Ring 3
> 前置：[023 系统调用从 Ring 3 发起](...)

## 前言

说实话，当 shell 第一次在 QEMU 上跑起来并打印出 "cinux> " 提示符的时候，笔者差点从椅子上跳起来——我们终于有了交互式命令行！然而兴奋没持续两秒，紧接着就是一个 #GP 崩溃。修完 #GP，echo 命令又全部失效。这一路踩过的坑，是整个 Cinux 项目到目前为止最令人血压拉满的调试经历。

本章和后面两章要讲的是 tag 024——从内核侧的 GDT 重构、syscall 路径修复，到用户态 shell REPL 和 Console ANSI 支持。这不是简单的"加个 shell 程序"——在这之前，我们需要先把 syscall 基础设施的几个隐蔽 bug 修干净。

## 环境说明

工具链和之前一样：GNU AS (AT&T 语法) + GCC 14 + CMake，QEMU 8.x 作为测试环境。特别注意，本章踩到的 SYSRETQ SS.RPL 问题在真实 Intel 硬件上可能不会复现——这是 QEMU 特定的模拟行为差异。但修复方案（RPL 编码进 STAR 基值）在所有平台上都适用。

## 第一步——GDT 重构，为 SYSRETQ 的坑铺路

### 起因：SYSRETQ 后 SS 不对

shell 启动后，PIT 时钟中断触发了一次 #GP。崩溃日志长这样：

```
==== EXCEPTION: #GP (vector 13) ====
  RIP   = 0xFFFFFFFF81000A3C   CS  = 0x0010
  ERROR CODE = 0x0000000000000028
[FATAL] General Protection Fault in kernel mode (error code=0x0000000028)
```

用 addr2line 定位，崩溃点是 irq0_stub 的 iretq 指令。错误码 0x28 = GDT selector index 5（User Data 段），RPL=0。这意味着中断栈帧上的 SS 值是 0x28（RPL=0），而 IRETQ 返回的目标 CPL=3——SS 的 RPL 检查失败。

但 SYSRETQ 明明应该设置 SS 的 RPL=3 啊？Intel SDM Vol.2A 的 SYSRET 指令参考说得很清楚："SS is set to (STAR[63:48] + 8) | RPL3"。然而在 QEMU 中，这个 "| RPL3" 并没有生效——SS 被设为 0x20 + 8 = 0x28，少了 RPL=3。

Wikipedia 的 x86-64 词条明确指出 Intel 和 AMD 在这个行为上的分歧：Intel 硬件无条件设置 RPL=3，AMD 的 RPL 来自 STAR 值。QEMU 在某些版本中两者的行为都没有正确模拟。

### Linux 的做法：把 RPL 编码进基值

Linux 内核用了一个巧妙的办法规避这个问题——把 RPL=3 直接编码进 STAR[63:48] 的基值。具体来说，STAR[63:48] 不用 0x20，而是用 0x23。这样 SYSRETQ 的加法就变成了：SS = 0x23 + 8 = 0x2B（RPL=3 已包含），CS = 0x23 + 16 = 0x33（同理）。无论硬件是否额外设置 RPL，selector 值都是对的。

这个修改需要 GDT 布局配合调整。我们需要一个 Linux 兼容的 9 项 GDT——在 Idx 1 留 NULL 占位（TLS），Idx 2-3 是内核代码/数据段，Idx 4-6 是三个用户段（User32 Code、User Data、User64 Code），Idx 7-8 是 TSS。

selector 常量的变化很大：GDT_KERNEL_CODE 从 0x08 变成 0x10，GDT_USER_CODE 从 0x1B 变成 0x33，GDT_TSS 从 0x28 变成 0x38。由于所有引用都使用 constexpr 常量，改一处自动更新全局。

```cpp
constexpr uint16_t GDT_SYSRET_BASE = 0x23;  // RPL=3 baked in
constexpr uint16_t GDT_USER_CODE   = 0x33;  // 0x23 + 16
constexpr uint16_t GDT_USER_DATA   = 0x2B;  // 0x23 + 8
```

### usermode.S 和 syscall.cpp 的同步更新

STAR MSR 的写入需要在两处同步更新：usermode.S（用立即数加载）和 syscall.cpp（用常量计算）。汇编中把 `$0x08` 改为 `$0x23`，`$0x08` 改为 `$0x10`。C++ 中把 STAR 值的计算改为 `(GDT_SYSRET_BASE << 48) | (GDT_KERNEL_CODE << 32)`。修复后，回读 STAR 确认 [63:48]=0x23、[47:32]=0x10。

### syscall_init 重构

顺带把 syscall_init() 的接口也重构了——不再需要外部传入 kernel_rsp 参数，改为内部用内联汇编获取。同时在内部注册所有 builtin syscall handlers（sys_read/sys_write/sys_exit/sys_yield），main.cpp 中只需一行调用。

## 第二步——SYSCALL 出口 RBX 恢复，让 echo 真正能工作

修完 GDT 和 STAR 后，shell 不再崩溃了。但新的问题出现了——输入 `echo hello` 按回车，什么也没输出。所有命令都失效了。

这个 bug 的根因在 syscall.S 的出口路径。之前用 RBX 暂存 syscall 返回值（因为 SYSRETQ 不修改 RBX），但忘了从 trap frame 恢复用户态的原始 RBX。编译器把 shell 的 pos 变量分配到了 RBX（callee-saved 寄存器），每次 sys_read/sys_write 后 RBX 被覆盖为返回值 1，导致所有字符都写到 buffer 的同一个位置。

修复方法是把返回值暂存从 RBX 改为 gs:16（GS 段的空闲 scratch slot），并新增 `movq 80(%rsp), %rbx` 从 trap frame 恢复用户 RBX。这个修复只涉及三行汇编的变更，但影响是决定性的——没有它，shell 就是个空壳子。

```asm
    movq %rax, %gs:16           # 返回值存到 scratch（不再用 RBX）
    ...
    movq 80(%rsp), %rbx         # 从 trap frame 恢复用户 RBX！
    addq $96, %rsp
    ...
    movq %gs:16, %rax           # 从 scratch 恢复返回值
    swapgs
    sysretq
```

这里有一个值得深入讨论的设计对比。Linux 内核的 entry_SYSCALL_64 使用 pt_regs 结构保存了所有通用寄存器，出口路径会完整恢复每一个 callee-saved 寄存器。Cinux 的 trap frame 只 push 了 syscall 必需的寄存器（RDI-RSI-RDX-RCX-RBX-RBP-R12-R13-R14 加上 RSP/RIP/RFLAGS），没有保存 R15。这在当前阶段没问题（内核代码不会用到 R15），但如果将来内核 syscall handler 变得更复杂，需要在入口处补充 R15 的保存。

xv6 的做法更直接——它用 trapframe 结构在内核栈上保存所有 32 个寄存器，包括所有的 callee-saved 寄存器。这虽然浪费了一点栈空间，但彻底避免了"哪个寄存器忘了保存"的问题。Cinux 选择了一个折中方案——只保存实际会用到的寄存器，既减少 trap frame 大小，又保持代码简洁。

## 参考资料

- Intel SDM: Vol.2A SYSRET Instruction Reference — CS/SS 计算、RPL 行为
  - URL: https://www.felixcloutier.com/x86/sysret
- Wikipedia x86-64: Intel vs AMD SYSRET RPL 行为差异
  - URL: https://en.wikipedia.org/wiki/X86-64
- Linux kernel entry_64.S: 同样的 RPL 编码技巧
  - URL: https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/arch/x86/entry/entry_64.S
- OSDev Wiki System Calls: SYSCALL/SYSRET 总览
  - URL: https://wiki.osdev.org/System_Calls
