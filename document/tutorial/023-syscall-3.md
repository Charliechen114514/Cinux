# 踩坑实录 -- FPU/SSE、栈对齐与用户态编译

> 标签：FPU, SSE, x86-64 ABI, stack alignment, boot.S, user-space compilation
> 前置：[023-2 分发、验证、终止](023-syscall-2.md)

## 前言

前两章我们搭好了 syscall 的 MSR 配置、汇编入口、分发表和三个 handler。按理说现在应该能跑通端到端流程了——用户程序调 sys_write 打印一行字，然后调 sys_exit 干净退出。但如果你真的编译一个 C++ 用户程序丢进去跑，大概率会看到这样的串口输出：

```
RIP   = 0x0000000000400019
RSP   = 0x00000007FFFFEFD8
movaps XMMWORD PTR [rsp],xmm0  ← #GP
```

说实话，笔者在这里血压拉满。明明 syscall 基础设施都搭好了，怎么用户程序一跑就崩？翻了一圈反汇编才发现——GCC 把 `const char msg[] = "Hello"` 的初始化优化成了 SSE 的 movaps 指令，movaps 要求目标地址 16 字节对齐，但 RSP 不满足。问题还没完——就算你在 boot.S 里启用了 FPU/SSE，movaps 还是会 #GP，因为这次的根因不是 FPU 没启用，而是栈对齐不符合 x86-64 ABI 要求。修好栈对齐之后，大内核测试又全挂了——因为 boot.S 指令顺序改了，mini kernel 的入口验证过不了。

三个问题，环环相扣，每个都隐藏在前一个的"修复"里。这一章我们就来拆解这三个连环坑，顺便搭建用户态程序的编译基础设施。

## 环境说明

继续 QEMU + GCC 14 环境。用户态程序使用 -mcmodel=small -fno-pie -nostdlib 编译，链接脚本 VMA=0x400000。

## 第一步 -- 启用 FPU/SSE：boot.S 的 CR0/CR4 配置

第一个问题的根因很清楚——内核没启用 FPU/SSE，用户程序一执行 SSE 指令就 #GP。修复是在 boot.S 中配置 CR0 和 CR4 控制寄存器。

CR4 需要设置两个位：bit 9 是 OSFXSR（Operating System Support for FXSAVE/FXRSTOR），告诉 CPU 操作系统支持这些指令；bit 10 是 OSXMMEXCPT（Operating System Support for Unmasked SIMD Exceptions），让 SIMD 浮点异常通过中断报告而不是触发 #XF。CR0 需要清除 bit 2（EM，Emulate x87 FPU——清零表示不模拟，直接使用硬件 FPU），设置 bit 1（MP，Monitor coprocessor），然后执行 clts 清除 TS（Task Switched）位——如果 TS 置位，第一条 SSE 指令会触发 #NM（Device Not Available）异常。

```asm
    movq  $__kernel_stack_top, %rsp
    xorq  %rbp, %rbp
    movq  %cr4, %rax
    orq   $((1 << 9) | (1 << 10)), %rax
    movq  %rax, %cr4
    movq  %cr0, %rax
    andq  $(~(1 << 2)), %rax
    orq   $(1 << 1), %rax
    movq  %rax, %cr0
    clts
```

这里有一个极其隐蔽的坑——FPU 初始化代码必须放在 `movq $__kernel_stack_top, %rsp` **之后**，不能插在 cli 和 movq rsp 之间。原因是 mini kernel 通过检查大内核入口的前三个字节来验证是否为真实内核——它期望看到 FA 48 BC（cli + REX.W 前缀 + mov rsp, imm）。如果你把 movq %cr4, %rax（字节码 0F 20）插到中间，字节码变成了 FA 0F 20，验证失败，所有大内核测试全部跳过。这个 bug 表面上是"测试全挂"，实际上只是指令顺序不对，笔者在这里排查了半天才找到根因。

除了 boot.S 的硬件初始化，我们还需要在 Task 结构体中保存 FPU 状态。每个 task 增加 alignas(16) uint8_t fpu_state[512]（FXSAVE 指令需要 512 字节且 16 字节对齐），创建 task 时用 fninit + fxsave 初始化。调度器在 context_switch 前对当前任务 fxsave、切换后对新任务 fxrstor——这样才能保证每个任务的 FPU 状态互不干扰。Linux 的做法类似，但用的是 xsave/xrstor（支持 AVX 等扩展），且采用"惰性保存"优化（只在任务实际使用 FPU 时才保存）。Cinux 选择每次都保存，简单但略慢。

## 第二步 -- x86-64 ABI 栈对齐：RSP 减 8 的故事

FPU 启用后 movaps 还是 #GP？这次根因不是硬件——而是 x86-64 SysV ABI 的栈对齐规则。ABI 规定函数入口（call 指令之后）RSP 必须满足 8 mod 16 的约束。为什么是 8 而不是 0？因为 call 指令压入 8 字节返回地址——call 之前 RSP 是 0 mod 16，call 之后就是 8 mod 16。编译器依赖这个假设来生成 movaps 等对齐敏感的指令。

但 SYSRETQ 不像 call 那样会压返回地址——它直接把 RCX 加载到 RIP，不碰栈。所以用户程序入口时 RSP 就是我们传入的值 0x7FFFFF000，是 0 mod 16 而不是 8 mod 16。编译器按 8 mod 16 假设生成的 sub rsp, 0x28 会让后续 movaps 的目标地址不对齐。修复极其简单：跳转前 RSP 减 8。

```cpp
constexpr uint64_t USER_ABI_RSP_OFFSET = 8;
static_assert((USER_STACK_TOP - USER_ABI_RSP_OFFSET) % 16 == 8,
              "User entry RSP must satisfy x86_64 ABI alignment");
jump_to_usermode(USER_ENTRY_BASE, USER_STACK_TOP - USER_ABI_RSP_OFFSET, 0);
```

这些常量定义在 usermode.hpp 头文件中，static_assert 是这里的神来之笔——如果以后改了 USER_STACK_TOP 的值导致对齐不满足，编译直接报错。Linux 不需要这个 hack，因为 Linux 创建用户进程走的是 fork + exec 路径，进程的初始 RSP 由内核在设置 trapframe 时就已经按 ABI 约定排好了。

## 第三步 -- 用户态编译基础设施

现在硬件和栈的问题都解决了，我们需要一个编译用户态程序并把二进制嵌入内核的构建流程。user/ 目录包含四个关键文件。

首先是用户程序本身 hello.cpp，入口函数 _start() 调用 sys_write 打印消息然后 sys_exit(0)。看起来简单，但它就是那个触发 movaps 问题的"罪魁祸首"程序。

然后是用户态的 syscall 封装 libc/syscall.cpp——多个辅助函数（_syscall1, _syscall2, _syscall3）用内联汇编执行 syscall 指令（RAX=syscall 号，RDI/RSI/RDX 传参），然后用这些辅助函数包装出 sys_read、sys_write、sys_exit（用 _syscall1 且后跟 __builtin_unreachable()）、sys_yield 等接口。clobber 列表必须包含 rcx（SYSCALL 自动覆盖）、r11（SYSCALL 自动覆盖）、memory（syscall 可能修改内存）。

链接脚本 linker.ld 设置 VMA=0x400000（和内核的 USER_ENTRY_BASE 一致），保证 _start 在二进制文件最开头。编译标志用 -mcmodel=small（用户态在低 2GB 地址空间）、-fno-pie -nostdlib -static。

最巧妙的部分是 CMakeLists.txt 的三步嵌入流程：先用 GCC 编译用户程序为 ELF，然后用 objcopy -O binary 转 flat binary（去掉 ELF 头），最后用 ld -r -b binary 把二进制包装成内核可以链接的 .o 文件。链接后内核通过 `_binary_hello_bin_start` 和 `_binary_hello_bin_end` 符号访问用户程序数据。这比用 .incbin 汇编伪指令好——CMake 自动跟踪依赖关系，增量编译更正确。Linux 的 initramfs 嵌入机制也是类似的思路，只不过它打包的是 cpio 归档而不是单个二进制。

## 验证 -- 端到端流程

全部就绪后编译运行，串口输出应该是这样的：

```
[SYSCALL] LSTAR=0x... STAR configured SFMASK=0x200 (clear IF)
[USER] Setting up first user-mode program...
[USER] Jumping to Ring 3: entry=0x400000 stack=0x7FFFFF000
[USER] Hello from Ring 3!
[SYSCALL] sys_exit: no scheduler, halting.
```

这四行输出分别对应：syscall 初始化、用户态跳转、sys_write 打印（经过完整的 Ring 3→syscall_entry→dispatch→sys_write→sysretq 路径）、sys_exit 停机。"no scheduler, halting" 是预期行为——023 阶段调度器还没启动，exit 后停机是正确的。

## 收尾

到这里，milestone 023 大功告成。从用户态的 syscall 指令，经过 SWAPGS + 栈切换 + trap frame + 分发表 + sys_write handler，最终在串口打印出 "Hello from Ring 3!"，然后通过 sys_exit 干净终止。这条完整链路就是操作系统系统调用的核心机制——虽然简化了很多（没有 copy_from_user、没有信号处理、没有 errno），但骨架和 Linux 是一样的。下一章 024 我们要在这个基础上启动调度器，让 shell 作为常驻用户进程运行。

## 参考资料

- Intel SDM Vol.3A §2.5: CR0 控制寄存器（EM/MP/TS 位）和 CR4 控制寄存器（OSFXSR/OSXMMEXCPT 位）定义
- Intel SDM Vol.1 §11.1-11.6: FXSAVE/FXRSTOR 指令和 FPU 状态管理
- x86-64 SysV ABI: 函数入口 RSP 对齐要求（RSP ≡ 8 mod 16 after call）
  - https://gitlab.com/x86-psABIs/x86-64-ABI
- OSDev Wiki System Calls: https://wiki.osdev.org/System_Calls
- Linux FPU context switch: arch/x86/kernel/fpu/core.c -- xsave/xrstor 和惰性保存机制
- xv6 user/init.c: https://pdos.csail.mit.edu/6.S081/2024/labs/syscall.html
