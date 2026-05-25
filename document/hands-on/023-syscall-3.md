---
title: 023-syscall-3 · 系统调用
---

# 023-3 Hands-on: FPU/SSE 支持、用户态编译与端到端验证

## 导语

前两章我们搭好了 syscall 的 MSR 配置、汇编入口、分发表和三个 handler。但如果你现在就编译一个用户态 C++ 程序丢进内核跑，大概率会收获一个 #GP 异常——因为 GCC 默认会用 SSE 指令（movaps）来优化字符串操作，而内核还没启用 FPU/SSE。更坑的是，就算启用了 FPU，栈对齐也可能不对，照样 #GP。本章我们解决这三个连锁问题：FPU/SSE 硬件启用、ABI 栈对齐、用户态程序编译基础设施，然后跑通第一个完整的 syscall 端到端流程。

前置知识：023-1 和 023-2 完成的 syscall 基础设施。

## 概念精讲

### 为什么用户态程序需要 FPU/SSE

GCC 在 -O2 优化级别下，会把像 `const char msg[] = "Hello"` 这样的局部字符串初始化优化为 SSE 的 movdqa/movaps 指令——一次性拷贝 16 字节。movaps 要求目标地址 16 字节对齐，如果 RSP 不满足对齐要求就直接 #GP。这个问题在 Ring 3 尤其致命，因为用户态的 #GP 会触发异常处理而不是像内核态那样有更好的恢复机制。

x86-64 的 SSE 启用需要配置两个控制寄存器：CR4 的 bit 9（OSFXSR，允许 FXSAVE/FXRSTOR 操作）和 bit 10（OSXMMEXCPT，允许 SIMD 浮点异常通过中断报告）。CR0 需要清除 bit 2（EM，模拟协处理器——清零表示不用模拟）和设置 bit 1（MP，监视协处理器），还要执行 clts 清除 TS（任务切换）位。

### x86-64 SysV ABI 栈对齐规则

这个规则说来话长但必须搞清楚。x86-64 SysV ABI 规定：在函数入口点（即 call 指令之后、被调用者第一条指令之前），RSP 必须满足 8 mod 16 的约束。为什么不是 0 mod 16？因为 call 指令会压入 8 字节返回地址，所以 call 之前 RSP 是 0 mod 16 的话，call 之后就是 8 mod 16。编译器依赖这个假设来安排 movaps 等对齐敏感的指令。

我们的用户栈顶 USER_STACK_TOP 是 0x7FFFFF000，这是 0 mod 16。用 SYSRETQ 跳到用户态时，不像 call 那样会压返回地址——SYSRETQ 直接把 RCX 加载到 RIP，不碰栈。所以用户程序的第一条指令执行时 RSP 就是 0x7FFFFF000，是 0 mod 16 而不是 8 mod 16。编译器按照 8 mod 16 的假设生成的 sub rsp, 0x28 会导致后续 movaps 的目标地址不对齐。修复方法很简单：跳转前把 RSP 减 8，让 RSP 变成 0x7FFFFEFF8（8 mod 16），满足 ABI 约定。

### 用户态编译：从 C++ 到嵌入内核的二进制

用户态程序和内核不在同一个地址空间运行，但我们需要在编译期把用户程序嵌入到内核镜像里。流程是：先用 GCC 以 -mcmodel=small -fno-pie -nostdlib 编译用户程序为 ELF，然后用 objcopy 把 ELF 转成纯二进制（flat binary），最后用 ld -r -b binary 把这个二进制包装成内核可以链接的 .o 文件。链接后内核可以通过 _binary_hello_bin_start 和 _binary_hello_bin_end 这两个符号访问用户程序的二进制数据。

用户程序的链接脚本设置 VMA=0x400000（4MB，在用户空间的低地址区域），入口点命名为 _start。

## 动手实现

### Step 1: 在 boot.S 中启用 FPU/SSE

**目标**：在内核启动的早期阶段配置 CR0 和 CR4，启用 SSE/FPU 硬件支持。

**设计思路**：在 _start 入口设置好栈指针之后、清除 BSS 之前，插入 FPU 初始化代码。先读 CR4，or 上 OSFXSR（bit 9）和 OSXMMEXCPT（bit 10），写回 CR4。再读 CR0，清除 EM（bit 2），设置 MP（bit 1），写回 CR0。最后 clts 清除 TS 位。

**踩坑预警（关键）**：FPU 初始化代码**必须放在 `movq $__kernel_stack_top, %rsp` 之后**，不能插在 cli 和 movq rsp 之间！原因：mini kernel 通过检查大内核入口的前三个字节来验证是否为真实内核——它期望看到 FA 48 BC（cli + REX.W + mov rsp, imm）。如果把 movq %cr4, %rax 插在中间，字节码变成 FA 0F 20，验证失败，所有大内核测试会被跳过。这个 bug 非常隐蔽，笔者在这里血压拉满。

**验证**：编译后检查大内核测试是否正常运行（不是 "not a real kernel" 退出）。运行 `make run-kernel-test`，确认测试数量不为 0。检查 kernel/arch/x86_64/boot.S 中 FPU 初始化代码位于 movq rsp 之后。

### Step 2: Task 结构体增加 FPU 状态字段

**目标**：在 Task 结构体中增加 512 字节的 FPU 状态保存区。

**设计思路**：FXSAVE/FXRSTOR 指令需要 512 字节对齐到 16 字节的目标区域。在 Task 结构体末尾添加 alignas(16) uint8_t fpu_state[512]。创建 Task 时，先执行 fninit 初始化 FPU，然后 fxsave 把初始状态保存到 fpu_state。调度器在做 context_switch 前对当前任务 fxsave，切换后对新任务 fxrstor。

**实现约束**：fpu_state 必须有 alignas(16) 属性。fxsave/fxrstor 的内联汇编使用 "m" 约束直接操作内存。

**验证**：确认 Task 的大小增加了 512 字节（但考虑到对齐可能更多）。

### Step 3: 用户栈 ABI 对齐

**目标**：修改 jump_to_usermode 调用，确保用户程序入口时 RSP 满足 8 mod 16 约束。

**设计思路**：定义常量 USER_ABI_RSP_OFFSET = 8，在传给 jump_to_usermode 的 RSP 参数上减去这个值。同时用 static_assert 编译期检查对齐：(USER_STACK_TOP - 8) % 16 == 8。这些常量定义在 usermode.hpp 头文件中。

**踩坑预警**：static_assert 放在头文件中做编译期检查，如果以后改了 USER_STACK_TOP 的值导致对齐不满足，编译直接报错。

**验证**：static_assert 如果不满足会编译失败。

### Step 4: 用户态程序和编译基础设施

**目标**：创建 user/ 目录结构，编写第一个 C++ 用户程序，配置 CMake 把它编译并嵌入内核。

**设计思路**：用户程序非常简单——入口函数 _start，声明一个字符串，调用 sys_write(1, msg, len)，然后调用 sys_exit(0)。sys_write 和 sys_exit 通过内联汇编的 syscall 指令实现（用户态 libc 封装）。CMake 配置分三步：编译用户程序为 ELF，objcopy 转 flat binary，ld -r -b binary 嵌入。

用户态 libc 的 syscall 封装是一个薄层——多个辅助函数（_syscall1, _syscall2, _syscall3）用内联汇编执行 syscall 指令（RAX=syscall 号，RDI/RSI/RDX 传参，输出 "=a"(ret)），clobber 列表包含 rcx、r11、memory。然后用这些辅助函数包装 sys_read、sys_write、sys_exit（用 _syscall1 且后跟 __builtin_unreachable()）、sys_yield 等。

**实现约束**：编译标志用 -mcmodel=small（用户态在低 2GB）、-fno-pie -nostdlib -static。链接脚本 ENTRY(_start)，VMA=0x400000。

**验证**：编译后检查 user/hello.bin 是否生成，检查内核链接后是否有 _binary_hello_bin_start 符号。

### Step 5: 修改 launch_first_user() 使用嵌入的用户程序

**目标**：把 launch_first_user() 里的手写字节码替换为嵌入的 hello 程序二进制。

**设计思路**：不再用硬编码的 kUserCode[] 数组，而是用 extern 声明 _binary_hello_bin_start 和 _binary_hello_bin_end，计算大小后拷贝到用户态代码页。GS base 数据页已在 usermode_init() 中分配和配置（gs:0、gs:8、gs:16 三个槽位），不需要在 launch_first_user 中重复设置。

**验证**：QEMU 启动后串口应显示 `[USER] Hello from Ring 3!`，然后 `[SYSCALL] sys_exit: no scheduler, halting.`。

## 构建与运行

完整流程：cmake --build build → make run（或 make run-kernel-test）。预期串口输出序列：
1. 内核初始化直到 `[SYSCALL] LSTAR=...`
2. `[USER] Setting up first user-mode program...`
3. `[USER] Jumping to Ring 3: entry=0x400000 stack=0x7FFFFF000`
4. `[USER] Hello from Ring 3!`
5. `[SYSCALL] sys_exit: no scheduler, halting.`

## 调试技巧

- 如果看到 #GP at RIP=0x400019 with movaps：栈对齐问题，检查 RSP 是否满足 8 mod 16
- 如果看到 #GP 但不是 movaps：检查 FPU 是否启用（CR4 的 OSFXSR 位）
- 如果大内核测试全部跳过：检查 boot.S 前三条指令的字节码是否是 FA 48 BC
- 如果用户程序不输出：检查 _binary_hello_bin_start 符号是否存在（nm build/kernel/big/big_kernel | grep binary）

## 本章小结

| 改动 | 文件 | 说明 |
|------|------|------|
| FPU/SSE 启用 | boot.S | CR4.OSFXSR+OSXMMEXCPT, CR0 清 EM/TS |
| FPU 状态保存 | process.hpp | Task 增加 fpu_state[512] |
| FPU 上下文切换 | scheduler.cpp | fxsave/fxrstor 在 context_switch 前后 |
| 栈 ABI 对齐 | usermode.cpp | USER_ABI_RSP_OFFSET=8 + static_assert |
| 用户态编译 | user/CMakeLists.txt | ELF→flat binary→embed .o |
| 用户态 libc | user/libc/syscall.cpp | SYSCALL 内联汇编封装 |
| 用户程序 | user/programs/hello.cpp | C++ 写的 hello world |
