# 024-shell-2: sys_read 系统调用与 RBX 寄存器保存修复

## 导语

上一章我们修复了 GDT 布局和 SYSRETQ 的 SS.RPL 问题，让 shell 程序能在 Ring 3 稳定运行不再被中断搞崩。但如果你现在启动 shell，输入 `echo hello` 再按回车，你会发现屏幕上什么也没输出——所有命令都像石沉大海一样。这不是 shell 逻辑的 bug，而是 SYSCALL 入口代码中一个更隐蔽的问题：内核在保存/恢复寄存器时破坏了用户态的 RBX。本章我们要实现 sys_read 系统调用（从键盘读取输入），同时修复 syscall 入口的 RBX 保存问题。完成后，shell 的 read_line 将能正确逐字符读取键盘输入，命令分派也能正常工作。

前置知识：上一章的 GDT/STAR 修复、SYSCALL trap frame 结构、x86_64 ABI callee-saved 寄存器约定。

## 概念精讲

### x86_64 ABI 与 Callee-Saved 寄存器

x86_64 System V ABI 规定了一组 callee-saved 寄存器：RBX、RBP、R12-R15。这意味着如果一个函数（或等价的调用路径）使用了这些寄存器，它必须在返回前将它们恢复到进入时的值。SYSCALL 指令不自动保存任何寄存器（除了 RCX 被用来存返回地址、R11 被用来存 RFLAGS），所以内核的 SYSCALL 入口代码必须手动保存所有 callee-saved 寄存器。

在 Cinux 的 syscall.S 中，入口将所有通用寄存器 push 到 trap frame（12 个 8 字节 slot，共 96 字节）。出口需要从 trap frame 恢复这些寄存器。问题出在出口路径——之前用 RBX 暂存 syscall 返回值（因为 SYSRETQ 不修改 RBX），但忘记在恢复用户状态时从 trap frame 恢复用户态的原始 RBX。trap frame 被释放后（addq $96, %rsp），用户态的 RBX 值彻底丢失了。

你可能会问：为什么编译器会用到 RBX？因为 RBX 是 callee-saved 寄存器，编译器在优化时喜欢把循环变量放到 callee-saved 寄存器中——它们不会被函数调用破坏，省去了在内存中反复 load/store 的开销。shell 的 read_line 函数中，pos 变量（当前写入位置）被编译器分配到了 RBX。每次 sys_read/sys_write 返回后，RBX 被覆盖为 syscall 返回值（通常是 1），导致所有字符都写到 buffer 的同一个位置。

### 这个 Bug 的实际执行流

让我们追踪一下输入 "echo hello" 时的实际执行过程。编译器把 read_line 内联到 shell_main 后，核心循环大概是：调用 sys_read 读取一个字符到局部变量 c，回显这个字符，然后把 c 存到 line[pos]，pos 递增。pos 被分配到 RBX。

第一次迭代：RBX=0（pos 初始值），sys_read 读到 'e'，返回 1。SYSCALL 出口把 RBX 设为返回值 1。回显 'e'。然后执行 `line[RBX] = 'e'`——但 RBX 现在是 1 而不是 0，所以 'e' 被写到了 line[1]。接着 sys_write 回显也把 RBX 覆盖为 1。然后 RBX = RBX + 1 = 2。

第二次迭代：RBX=2，sys_read 读到 'c'，返回 1。RBX 被覆盖为 1。`line[1] = 'c'`，覆盖了之前的 'e'。然后 RBX = 1 + 1 = 2。

如此循环，每个字符都被写到 line[1]，前一个字符被覆盖。最终 line[0] 从未被写入（值为 0），line[1] 存的是最后一个字符。按下回车时，sys_write 返回 1，RBX 被覆盖为 1，line[1] 被设为 '\0'。结果：line 是空字符串，len=1。

### GS Scratch Slot 的使用

修复方案的核心是把返回值暂存位置从 RBX 改为 GS 段的一个空闲 slot。Cinux 已经使用了 GS 段基址来存放 per-CPU 数据——gs:0 是内核栈指针（RSP0），gs:8 是临时存放用户态 RSP 的 scratch slot。gs:16 是第三个 slot，目前没有被使用。因为 SYSCALL 入口/出口是单线程的（同一时刻只有一个 CPU 在执行 syscall 路径），所以这个 slot 不会被并发覆盖，可以安全使用。

### sys_read 的设计考量

sys_read 的功能是从键盘 buffer 读取字符到用户空间。与 sys_write 类似，它需要验证用户地址的合法性。但 sys_read 有一个特殊的挑战——键盘输入是异步的，用户可能还没按键。在没有阻塞 I/O 和调度器信号量支持的情况下，Cinux 采用了 spin-wait 策略：循环调用 pause 指令并检查键盘 buffer，最多等待 100 万次迭代。这不是最优方案（阻塞式 I/O + 调度器 yield 会更好），但在当前的进程模型下是最简单的实现。

另一个设计选择是逐字符读取、遇换行停止的语义——这样 shell 的 read_line 函数每次 sys_read 调用刚好拿到一个字符，自己管理行缓冲。这与 Unix 的 read() 系统调用语义一致——底层只提供字节流，行编辑由用户态库负责。

## 动手实现

### Step 1: 实现 sys_read 系统调用处理器

**目标**: 新增 `kernel/syscall/sys_read.cpp` 和对应的 `.hpp` 头文件。

**设计思路**: sys_read 接收三个关键参数——fd（文件描述符，只支持 fd=0 即 stdin）、buf_virt（用户空间缓冲区地址）、count（最大读取字节数）。处理器首先验证 buf_virt < 0x800000000000（防止用户传内核地址）且 fd == 0。然后进入读取循环：从 PS/2 键盘的 ring buffer 中调用 Keyboard::poll() 获取事件，只接受 pressed=true 且 ascii != 0 的按键事件。将 '\r' 转换为 '\n'（PS/2 键盘的 Enter 键产生的是 '\r'，shell 期望 '\n'）。遇到 '\n' 时停止读取。如果 buffer 为空且还没有读到任何数据，spin-wait 最多 SPIN_WAIT_ITERS（1,000,000）次，每次执行 pause 指令降低功耗。如果已经有部分数据，直接返回（非阻塞语义）。

**踩坑预警**: 键盘驱动产生的 KeyEvent 包含 pressed 字段——键释放事件也会入队。如果不检查 pressed，你会把同一个键"读两次"（按下一次、释放一次）。另外，非 ASCII 按键（Shift、Ctrl、Alt 等）的 ascii 字段为 0，必须过滤掉，否则 shell 会收到一堆不可打印字符。

**验证**: 注册 sys_read（SYS_read = 0）到 syscall 分发表后，从用户态调用 sys_read(0, buf, 1)，按下任意键应该能读到字符。可以在 sys_read 中添加 debug 打印（通过 kprintf），确认每次读到的字符和 ascii 值。

### Step 2: 修复 SYSCALL 出口的 RBX 保存

**目标**: 在 syscall.S 的出口路径中，从 trap frame 恢复用户态的 RBX，并将返回值暂存位置从 RBX 改为 GS scratch slot 2（gs:16）。

**设计思路**: 修改 syscall.S 的出口路径——在 dispatch 返回后，把 RAX（返回值）存到 gs:16 而不是 RBX。然后从 trap frame 的 offset 80 恢复用户态的 RBX（`movq 80(%rsp), %rbx`）。这一步必须在 `addq $96, %rsp` 释放 trap frame 之前执行。最后把 gs:16 的值恢复到 RAX（`movq %gs:16, %rax`）。其他恢复逻辑（user RSP -> gs:8、user RIP -> RCX、user RFLAGS -> R11）保持不变。

这个修改只涉及三行汇编的变更，但影响巨大——它保证了每次 SYSCALL 调用对用户态来说就像一次普通的函数调用，callee-saved 寄存器在调用前后保持一致。

**踩坑预警**: 这个 bug 的症状非常隐蔽——shell 启动正常，键盘输入字符能正确回显，但所有命令都没有效果。看起来像是 shell 逻辑的 bug，实际上是因为 RBX（编译器用它存储 pos 变量）被 syscall 返回值覆盖，导致所有字符都写到同一个位置。如果你遇到"所有命令都失效"的现象，不要逐个命令排查——先怀疑底层基础设施（syscall 路径、字符串比较、内存布局等共性问题）。

**验证**: 修复后运行 shell，输入 `echo hello` 应该能看到 `hello` 的输出。输入 `clear` 应该能清屏。为了彻底验证，可以在 shell 的 main.cpp 中添加 debug printf，打印 read_line 返回后的 line 内容和 len——修复前应该是 `line='' len=1`，修复后应该是 `line='echo hello' len=10`。

### Step 3: 更新 syscall_init 注册 sys_read

**目标**: 在 register_builtin_handlers() 中添加 sys_read 的注册。

**设计思路**: sys_read 对应的 syscall 号是 SYS_read = 0（参考 Linux 约定，read 是 0 号系统调用）。在 register_builtin_handlers() 中调用 syscall_register(SyscallNr::SYS_read, sys_read)。同时在 syscall.cpp 中添加 sys_read.hpp 的 include。还需要在 syscall_dispatch 中添加无效 syscall 号的 debug 打印——当 nr >= SYSCALL_TABLE_SIZE 时输出 nr 值，方便排查未注册的 syscall。

**验证**: 运行 shell 程序，确认键盘输入能被正确读取。如果出现 "[DISPATCH] invalid syscall nr=0" 的输出，说明 sys_read 没有被正确注册。

### Step 4: 更新测试

**目标**: 更新现有的 syscall 和 usermode 测试以匹配新的 selector 常量，添加 shell 基础设施相关的测试。

**设计思路**: 旧测试中硬编码的 selector 值（0x08、0x1B、0x23、0x28）需要替换为新的常量引用（GDT_KERNEL_CODE、GDT_USER_CODE 等）。STAR MSR 测试需要检查 STAR[63:48] == GDT_SYSRET_BASE 而非固定值 0x08。测试名字也要相应更新（test_star_msr_sysret_cs 改为 test_star_msr_sysret_base）。新增的 shell 测试覆盖 string 工具（strlen、strcmp）、tokenizer（空格切分、边界情况）、syscall dispatch（sys_read/sys_write 注册验证）、命令分发表（CmdEntry 查找、哨兵终止）和内存工具（memset、memcpy、memcmp）。

**验证**: 运行 `cmake --build build && cd build && ctest --output-on-failure`，所有测试应该 PASS。特别是新增的 shell 测试组（Shell Tests 024）应该有 30+ 个测试全部通过。

## 构建与运行

构建命令：

```
cmake --build build
```

运行 QEMU：

```
qemu-system-x86_64 -cdrom build/cinux.iso -serial stdio -display none
```

预期输出：

```
Cinux shell - type 'help' for commands
cinux> echo hello
hello
cinux> help
Available commands:
  echo <args...>  - print arguments to stdout
  help            - show this help message
  clear           - clear the screen
cinux>
```

## 调试技巧

1. **RBX 破坏的诊断**: 在 shell 的 main.cpp 中，用 printf 打印 read_line 返回后的 line 内容和 len。如果看到 `line='' len=1`（空字符串但长度为 1），说明 pos 变量被破坏了。注意这里 printf 使用的是用户态的 printf（user/libc/printf.cpp），不是内核的 kprintf。
2. **反汇编确认**: 用 `objdump -d build/user/shell` 查看 shell 的反汇编，确认编译器确实把 pos 分配到了 RBX。你会在循环中看到类似 `mov BYTE PTR [rsp+rbx+0x90], al` 和 `lea rdx, [rbx+0x1]` 的指令。只有看到汇编才能确定哪个寄存器被破坏。
3. **ISR handler 打印 InterruptFrame**: 在 PIT handler 中检查 frame->cs 和 frame->ss 的值。正常用户态中断应该是 CS=0x0033、SS=0x002B。如果 SS 不对，说明上一章的 STAR 修复没生效。
4. **syscall_dispatch trace**: 在 syscall_dispatch 中添加 kprintf 打印每次 syscall 的编号和参数。如果看到连续的 SYS_read(0) 和 SYS_write(1) 调用，说明 sys_read 基本通路正常。如果没有输出，说明 syscall 入口本身有问题。

## 本章小结

| 概念 | 要点 |
|------|------|
| callee-saved 寄存器 | RBX/RBP/R12-R15，SYSCALL 路径必须保存/恢复 |
| SYSCALL 自动保存 | 只有 RCX（RIP）和 R11（RFLAGS），其余全靠软件 |
| sys_read 设计 | spin-wait + 逐字符返回 + 地址验证 + '\r'->'\n' 转换 |
| gs:16 scratch slot | per-CPU GS page 的第三个 slot，用于暂存返回值 |
| RBX 破坏症状 | 所有命令失效，line 为空字符串但 len=1 |
| 反汇编定位 | objdump -d 确认编译器把 pos 分配到 RBX |
