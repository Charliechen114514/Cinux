# 从 Syscall 到 Shell（下）：ANSI Console、测试与踩坑总结

> 标签：ANSI, VT100, Console, unit test, QEMU, debug
> 前置：[024-shell-2 sys_read 与用户态 REPL](...)

## 前言

前两章我们修好了 SYSRETQ 的 SS.RPL 问题、修好了 SYSCALL 出口的 RBX 保存、实现了 sys_read 和用户态 shell REPL。但如果你现在运行 shell 并输入 `clear`，屏幕不会清除——因为 Console 驱动还不认识 ANSI 转义序列。本章要完成最后一公里：让 Console 能解析 ESC[2J 和 ESC[H，添加完整的测试覆盖，然后做一次端到端的 QEMU 验证。最后我们会回顾整个 tag 024 的踩坑经历。

## 环境说明

Console 驱动运行在 Ring 0，直接操作 framebuffer 显存。shell 的 clear 命令通过 sys_write 发送 7 字节 ANSI 序列，sys_write 把字节流传给 Console::putc()。所以 ANSI 解析必须放在 Console 内部。

## 第一步——Console ANSI CSI 解析器

### VT100 转义序列基础

1978 年，DEC 公司推出了 VT100 终端，引入了一套后来成为事实标准的转义序列。这套标准后来被 ANSI 采纳，所以通常被称为 "ANSI escape sequences"。CSI（Control Sequence Introducer）是其中最常用的一类——格式是 `ESC [` 后跟参数字节和终结字节。

`ESC[2J` 的意思是 "Erase in Display, parameter 2"——清除整个屏幕。`ESC[H` 的意思是 "Cursor Position, default (1,1)"——光标移到左上角。clear 命令就是先发 ESC[2J 清屏，再发 ESC[H 把光标移回原点。

### 三状态状态机

我们在 Console::putc() 的开头插入了一个三状态的解析器。Normal 状态下正常处理字符，遇到 ESC（0x1B）进入 Esc 状态。Esc 状态下遇到 '[' 进入 Bracket 状态并开始收集参数，遇到其他字符则丢弃 ESC 回到 Normal。Bracket 状态下收集参数字节（0x30-0x3F 范围）和中间字节（0x20-0x2F 范围），遇到终结字节（0x40-0x7E 范围）时调用 handle_ansi_csi 分发。

这个设计是 VT100 解析器的最简实现。真正的终端模拟器需要处理更多的序列类型——SGR（颜色和样式设置）、光标移动（A/B/C/D）、行擦除（K）等等。但对于 Cinux 来说，支持 'J' 和 'H' 就够了。

### 与 Linux Console 和 SerenityOS 的对比

Linux 内核的 console 驱动（drivers/tty/vt/vt.c）有一个完整的 VT220 兼容的转义序列解析器，支持几十种 CSI 命令。它使用一个结构体数组来注册各种命令处理函数，比 Cinux 的 switch-case 更适合维护大量命令。SerenityOS 的 LibLine 库更进一步——它实现了一个完整的终端模拟器，包括颜色、光标移动、Unicode 支持、行编辑和历史记录。

Cinux 选择最简实现的原因很简单：我们只需要支持 clear 命令。将来如果需要更丰富的终端功能（比如彩色输出），可以在 handle_ansi_csi 中逐步添加。

## 第二步——测试覆盖

tag 024 新增了两套测试：内核态的 kernel/test/test_shell.cpp 和用户态的 test/unit/test_shell.cpp。

### 内核态测试

运行在 QEMU 内部，作为 big_kernel_test 的一部分。这些测试验证内核侧的 shell 基础设施——string 工具、tokenizer 逻辑、syscall dispatch 表的 sys_read/sys_write 注册状态、CmdEntry 分发表模式、以及 memset/memcpy/memcmp 的正确性。

因为实际的 shell 代码运行在 Ring 3，内核测试无法直接调用它。所以测试中 mirror 了 shell 的 tokenizer 逻辑——同样的算法，但在内核态运行。这种"镜像测试"虽然不直接测试目标代码，但验证了算法的正确性。

### Host 端单元测试

运行在开发机上，作为普通可执行文件编译。这些测试覆盖了更完整的场景——用户态 string 工具、tokenizer 的边界情况（空字符串、纯空白、最大 token 限制）、cmd_echo/cmd_help/cmd_clear 的输出、read_line 的 backspace 处理、以及完整的 tokenize + dispatch 流水线。

sys_write 和 sys_read 在测试中被 mock 替换——输出被捕获到缓冲区中，输入从预设的字符串数组中读取。这样测试可以完全控制 I/O，验证命令输出是否精确匹配预期。

### 与 xv6 的测试对比

xv6 基本上没有自动化单元测试——它的验证方式是手动运行 shell 并输入命令。Cinux 选择写自动化测试的原因是 shell 的组件（tokenizer、命令分发、字符串工具）都是纯函数，非常适合单元测试。这些测试在将来修改 shell 代码时提供回归保护——改坏了一个边界情况，测试立刻报红。

## 第三步——端到端 QEMU 验证

完整的构建和运行流程：

```
cmake --build build
qemu-system-x86_64 -cdrom build/cinux.iso -serial stdio
```

预期交互：

```
[BIG] ===== Milestone 023: Syscall from Ring 3 =====
[SYSCALL] LSTAR=... STAR configured SFMASK=0x200 (clear IF)
Cinux shell - type 'help' for commands
cinux> help
Available commands:
  echo <args...>  - print arguments to stdout
  help            - show this help message
  clear           - clear the screen
cinux> echo Hello World
Hello World
cinux> clear
（屏幕清空）
cinux> echo test^H^H^Hfoo
（输入 test，三次退格，输入 foo）
cinux>
```

## 踩坑总结

回顾整个 tag 024 的开发过程，两个 bug 占据了绝大部分调试时间。

### Bug 1：SYSRETQ SS.RPL

现象：shell 启动后立即 #GP 崩溃，错误码 0x28。排查方法是 addr2line 定位崩溃指令、在 PIT handler 中打印 InterruptFrame 的 CS/SS、回读 STAR MSR 确认配置。根因是 QEMU 不自动设置 SYSRETQ 的 SS.RPL。修复方法是把 RPL=3 编码进 STAR 基值（0x23）。

教训：Intel SDM 描述的硬件行为不总是和 QEMU 模拟一致。在做特权级相关操作时，不要依赖硬件的隐式行为——把所有需要的东西显式编码进去。

### Bug 2：SYSCALL 出口 RBX Clobber

现象：shell 命令全部失效，echo 无输出，clear 无效果。排查过程曲折——先怀疑 shell 逻辑、再怀疑字符串比较、最后通过反汇编发现 pos 被分配到 RBX。根因是 syscall.S 用 RBX 暂存返回值但没从 trap frame 恢复用户 RBX。

教训：callee-saved 寄存器在 SYSCALL 路径中绝对不能破坏。如果调试时发现"所有命令都有问题"，不要逐个命令排查——直接怀疑 syscall 路径本身的正确性。

### 经验法则

1. addr2line + 反汇编是排查异常的第一步，30 秒就能定位问题。
2. 在 ISR handler 中打印 InterruptFrame 的 CS/SS 是区分用户态/内核态中断的最直接手段。
3. IRETQ 的 #GP 错误码就是 CPU 试图加载的那个段 selector——结合 GDT 布局可以直接定位。
4. 当怀疑寄存器被 clobber 时，反汇编用户态和内核态的入口/出口代码是唯一的确定方法。

## 参考资料

- VT100 User Guide: ANSI escape sequences 定义
  - URL: https://vt100.net/docs/vt100-ug/chapter3.html
- Linux console driver (vt.c): 完整 VT220 兼容解析器
  - URL: https://github.com/torvalds/linux/blob/master/drivers/tty/vt/vt.c
- SerenityOS LibLine: 现代终端模拟器库
  - URL: https://github.com/SerenityOS/serenity/tree/master/Userland/Libraries/LibLine
- Intel SDM: Vol.2A SYSRET — SS/CS selector 计算
  - URL: https://www.felixcloutier.com/x86/sysret
