---
title: 024-shell-3 · Shell
---

# 024-shell-3: 用户态 Shell REPL 与 ANSI Console 支持

## 导语

前两章我们修复了内核侧的两个关键 bug——SYSRETQ 的 SS.RPL 和 SYSCALL 入口的 RBX 保存，同时实现了 sys_read 系统调用。现在内核基础设施已经就绪，本章我们要在用户态实现一个完整的交互式 shell，并在内核的 Console 驱动中添加 ANSI 转义序列支持来配合 clear 命令。完成后，Cinux 将拥有一个可用的命令行界面，支持 echo、help、clear 三个内置命令，以及 backspace 行编辑功能。

前置知识：前两章的 GDT/STAR/RBX 修复和 sys_read 实现、用户态编译基础（tag 023 的 user/CMakeLists.txt 和链接脚本）。

## 概念精讲

### Shell 的基本架构：REPL 循环

几乎所有交互式 shell 都遵循同一个模式——REPL（Read-Eval-Print Loop）。在我们的场景中，这个循环被简化为：打印提示符（prompt）、读取一行输入（read_line）、按空格切分词法单元（tokenize）、查找并执行对应命令（dispatch）、然后回到开头继续等待下一条命令。

这个设计看起来简单，但每个环节都有值得注意的细节。read_line 需要处理 backspace 行编辑——用户打错字时按退格键，需要从 buffer 中删除上一个字符并在屏幕上擦除。tokenize 需要处理连续空格、前导空格和尾部空格。dispatch 需要一种可扩展的方式来注册新命令。

### 为什么把 Shell 放在用户态

你可能注意到，Cinux 的 shell 是一个完全运行在 Ring 3 的用户态程序，而不是内核线程。这个设计选择是有意为之的——shell 的核心功能（读键盘、打印字符、解析命令）都不需要内核特权。把 shell 放在用户态有几点好处：一是验证 SYSCALL/SYSRETQ 路径在频繁调用下的稳定性；二是强制使用 sys_read 和 sys_write 的完整端到端通路；三是为将来的多进程支持（fork+exec）奠定基础——shell 本身就是用户态进程，将来只需要实现进程创建就能让它启动子进程执行外部命令。

### ANSI 转义序列与 Console 支持

终端清屏的经典方法是发送 ANSI/VT100 转义序列 `ESC[2J`（清除整个屏幕）和 `ESC[H`（光标移到左上角）。VT100 是 1978 年 DEC 公司推出的终端，ANSI 转义序列是它引入的事实标准，至今几乎所有终端模拟器都支持。但在 Cinux 的 framebuffer Console 中，字符是直接画到显存上的——没有终端模拟器层来解析这些序列。所以我们需要在 Console 驱动内部实现一个最小的 ANSI CSI（Control Sequence Introducer）解析器。

CSI 序列的格式是 `ESC [` 后跟一系列参数字节（0x30-0x3F 范围）、可选的中间字节（0x20-0x2F 范围），以及一个终结字节（0x40-0x7E 范围）。终结字节决定命令类型——'J' 是 Erase in Display，'H' 是 Cursor Position。参数字节提供命令参数——"2J" 表示清除整个屏幕（参数为 2），"H" 无参数时默认将光标移到 (1,1) 即左上角。

### 用户态 libc：string 和 printf

由于用户态程序不能链接标准库（没有 libstdc++、glibc 等），我们需要自己实现基本的字符串操作和格式化输出。这些函数放在 `user/libc/` 目录下，以静态库 user_libc 的形式链接到 shell 程序中。

string 工具包括 strlen（字符串长度）、strcmp（字符串比较）、memset（内存填充）、memcpy（内存拷贝）、memcmp（内存比较），这五个函数覆盖了 shell 需要的所有基本操作。printf 实现使用内部缓冲区（256 字节）积累输出，满时通过 sys_write 刷新，支持 %c、%s、%d、%u、%x、%p、%% 格式说明符以及 %l/%ll 长度修饰符。

## 动手实现

### Step 1: 实现用户态 string 工具库

**目标**: 在 `user/libc/string.cpp` 和 `string.hpp` 中实现 strlen、strcmp、memset、memcpy、memcmp。

**设计思路**: 所有函数放在 `cinux::user` 命名空间中，避免与内核的同名函数冲突。签名和语义与标准 C 库完全一致，确保使用时的零学习成本。strlen 和 strcmp 是经典的逐字符扫描实现——strlen 计数直到遇到 NUL，strcmp 逐字符比较直到不等或同时到达末尾。memset、memcpy、memcmp 是逐字节操作——对于教学内核来说，简单的循环实现足够了，不需要 SIMD 优化。

头文件 string.hpp 声明了所有五个函数，每个都有 Doxygen 风格的文档注释说明参数和返回值。使用 `<cstddef>` 头文件（freestanding，不需要标准库支持）来获取 size_t 类型。

**验证**: 在 user/CMakeLists.txt 中将 string.cpp 加入 user_libc 的源文件列表。编译确认无错误。运行 host 端的 test_shell 单元测试，string 部分的测试用例覆盖了空字符串、普通字符串、长字符串、零长度操作等边界情况。

### Step 2: 实现用户态 printf

**目标**: 在 `user/libc/printf.cpp` 和 `printf.hpp` 中实现最小 printf。

**设计思路**: 使用内部 256 字节缓冲区，通过 putc_buf 辅助函数逐字符写入，满时通过 sys_write(1, buf, pos) 刷新。格式解析采用简单的状态机——遇到 '%' 后检查下一个字符，如果是 'l' 则继续检查下一个字符（区分 %l 和 %ll），然后根据格式说明符分发处理。putuint_buf 使用除法取余的方式将无符号整数转换为字符串，支持十进制和十六进制（大小写），先逆序写入临时数组再正序输出。putint_buf 处理负数符号后委托给 putuint_buf。

**踩坑预警**: 不要在用户态直接用内联代码格式化数字——手动拼接整数字符串很容易出错（忘记处理 0、整数溢出、NULL 指针等）。笔者在这里踩过坑——最初在 shell 中用内联代码格式化数字，直接导致 NULL 指针解引用的 #PF 崩溃。先实现 printf，然后用 printf 来做所有的格式化输出。

**验证**: 编写简单的测试——用 printf 输出各种格式（整数、字符串、指针、十六进制），检查串口输出是否正确。确认 %d、%u、%x、%s、%c、%p、%% 都能正确工作。特别注意 %lld 和 %llu 长度修饰符对 64 位值的处理。

### Step 3: 实现 Shell REPL 主循环

**目标**: 在 `user/programs/shell/main.cpp` 中实现 shell_main() 和 _start() 入口点。

**设计思路**: _start() 是 ELF 入口点（地址 0x400000），调用 shell_main() 后执行 sys_exit(0)。shell_main() 首先输出欢迎信息 "Cinux shell - type 'help' for commands"，然后进入无限循环。每次迭代分四步：第一步用 sys_write 输出 "cinux> " 提示符；第二步调用 read_line 读取一行输入；第三步调用 tokenize 将行切分为 argv；第四步线性扫描 builtin_cmds 表匹配命令。

read_line 函数是 shell 的核心 I/O 逻辑。它逐字符调用 sys_read(0, &c, 1) 读取键盘输入。遇到 '\n' 时回显换行并停止。遇到 0x7F 或 '\b' 时，如果 pos > 0 则回退 pos、输出 `\b \b`（退格 + 空格 + 退格 = 在屏幕上擦除一个字符）。正常字符回显后存入 buffer。buffer 最后 NUL 终止。返回值是 pos（不含 NUL 的字符数）。

tokenize 函数按空格和制表符切割输入行。它直接修改原字符串——在空白处写入 NUL，然后 argv 数组的每个元素指向对应的 token 起始位置。跳过前导空白，尾部空白不产生额外 token。MAX_TOKENS 限制防止 argv 数组越界。

**踩坑预警**: read_line 必须处理 backspace，否则用户打错字无法修正，体验极差。回显是通过 sys_write(1, &c, 1) 实现的，不是内核自动完成的——内核的 sys_read 只负责读取按键事件，不负责在屏幕上显示任何东西。另外，sys_read 在没有输入时可能返回 0（spin-wait 超时），read_line 需要处理 n <= 0 的情况（continue 跳过）。

**验证**: 编译 shell 程序（user_shell 目标），确认 ELF 格式正确。用 `readelf -h build/user/shell` 检查 entry point 是否为 0x400000。

### Step 4: 实现三个内置命令

**目标**: 分别在 cmd_echo.cpp、cmd_help.cpp、cmd_clear.cpp 中实现 echo、help、clear 命令。

**设计思路**: cmd_echo 遍历 argv[1..argc-1]，用 sys_write 输出每个参数的字符串内容（用 strlen 计算长度），参数之间加空格，最后输出换行。如果只有 "echo" 没有参数（argc==1），则只输出换行。cmd_help 输出硬编码的帮助文本字符串，列出所有可用命令。cmd_clear 发送 7 字节的 ANSI 转义序列——`\033[2J\033[H`，即先清屏再将光标移到左上角。每个命令文件包含 shell.hpp 头文件（获取 CmdEntry 定义和其他命令声明），使用 user::strlen 计算字符串长度。

**验证**: 在 shell 中分别输入 `echo hello world`（应输出 "hello world"）、`help`（应列出三个命令）、`clear`（应清屏）。特别测试 echo 的边界情况——无参数时只输出空行，多参数时正确用空格分隔。

### Step 5: 定义 CmdEntry 分发表

**目标**: 在 main.cpp 中定义 builtin_cmds 数组，在 shell.hpp 中声明 CmdEntry 结构体和命令处理函数。

**设计思路**: shell.hpp 定义 CmdEntry 结构体——包含 name（命令名字符串指针）和 handler（函数指针，签名为 `void(*)(int argc, char** argv)`）。声明 cmd_echo、cmd_help、cmd_clear 三个函数。main.cpp 中定义 builtin_cmds 为 constexpr 数组，包含三个有效条目和一个 {nullptr, nullptr} 哨兵。dispatch 逻辑是简单的线性扫描——遍历表项，用 strcmp 比较命令名，匹配则调用 handler 并 break。如果遍历完没找到，输出 "xxx: command not found"。

这种数据驱动的设计让添加新命令极其简单——只需写一个处理函数，在 shell.hpp 中声明，然后在 builtin_cmds 表中添加一个条目。不需要修改主循环逻辑。

**验证**: 输入不存在的命令名，确认显示 "xxx: command not found"。输入 `echo`、`help`、`clear` 确认各自被正确分发。

### Step 6: Console ANSI CSI 解析器

**目标**: 在 Console::putc() 前插入一个三状态状态机，识别 ESC[2J 和 ESC[H 序列并执行对应操作。

**设计思路**: 在 console.hpp 中新增 AnsiState 枚举（Normal、Esc、Bracket）和状态字段（ansi_state_、ansi_params_[16]、ansi_pos_）。putc() 首先经过状态机处理：Normal 状态下遇到 0x1B 进入 Esc 状态并 return；Esc 状态下遇到 '[' 进入 Bracket 状态、重置参数缓冲并 return，否则回到 Normal 继续处理当前字符；Bracket 状态下收集参数字节（0x30-0x3F 范围的数字和分号字符）和中间字节（0x20-0x2F），遇到终结字节（0x40-0x7E）时调用 handle_ansi_csi 分发。malformed 序列（不符合上述规范的字节）直接 reset 到 Normal 状态。

handle_ansi_csi 只处理两个命令：'J'（Erase in Display）解析参数，如果参数为 2 则调用 Console::clear() 清屏；'H'（Cursor Position）将 col_ 和 row_ 设为 0（光标归位）。其他命令静默忽略。

**验证**: 用 shell 的 clear 命令触发 ANSI 序列，确认屏幕被清除且光标回到左上角。也可以手动发送 `sys_write(1, "\033[2J\033[H", 7)` 测试。在 Console::putc 中添加 raw 字符的 debug 打印（通过串口 kprintf），确认 ESC、[、2、J、ESC、[、H 这些字符被正确接收。

### Step 7: 更新构建系统和嵌入二进制

**目标**: 修改 `user/CMakeLists.txt` 和 `kernel/arch/x86_64/usermode.cpp`，从 hello 切换到 shell。

**设计思路**: CMake 目标从 user_hello 改为 user_shell，源文件列表包含 programs/shell/main.cpp 和三个 cmd_*.cpp 文件。user_libc 新增 libc/string.cpp 和 libc/printf.cpp。objcopy 步骤从 hello.bin 改为 shell.bin，linker -r -b binary 步骤也同步更新。内核中 usermode.cpp 的 extern 符号名从 `_binary_hello_bin_start/end` 改为 `_binary_shell_bin_start/end`。usermode.hpp 中将 USER_STACK_TOP、USER_STACK_PAGES、USER_ABI_RSP_OFFSET 常量从 usermode.cpp 移到头文件中，方便 shell 的其他模块引用。

**验证**: 完整构建后运行 QEMU，看到 "Cinux shell - type 'help' for commands" 提示。输入 echo/help/clear 确认命令工作正常。用 backspace 测试行编辑功能。

## 构建与运行

完整构建：

```
cmake --build build
```

运行：

```
qemu-system-x86_64 -cdrom build/cinux.iso -serial stdio
```

交互测试：

```
cinux> echo Hello from Cinux shell!
Hello from Cinux shell!
cinux> help
Available commands:
  echo <args...>  - print arguments to stdout
  help            - show this help message
  clear           - clear the screen
cinux> clear
（屏幕清空，光标回到左上角）
cinux> echo test
（输入 test，按三次 backspace，再输入 foo，最终显示 foo）
```

## 调试技巧

1. **ANSI 序列不生效**: 在 Console::putc 中添加 raw 字符的 debug 打印（通过串口 kprintf），确认 ESC、[、2、J、ESC、[、H 这些字符是否被正确接收。特别注意 0x1B 字符不要被其他逻辑（比如 \n 处理）拦截。
2. **shell 命令无反应**: 先确认 read_line 是否正常返回（用 printf 打印 len 和 line）。如果 len=0 或 line 为空，回看第二章的 RBX 修复是否生效。如果 read_line 正常但命令不被识别，检查 tokenize 的输出——打印 argc 和 argv[0] 确认切分正确。
3. **用户态 printf 崩溃**: 确保没有使用 %f、%n 等未实现的格式说明符。va_arg 的类型必须与格式说明符匹配——%d 对应 int、%ld 对应 long、%lld 对应 long long。类型不匹配会导致栈不平衡，后续行为完全不可预测。
4. **ELF 加载失败**: 用 `readelf -h build/user/shell` 确认 entry point 是 0x400000，用 `readelf -l build/user/shell` 确认 LOAD 段的虚拟地址从 0x400000 开始。如果不对，检查 linker.ld 脚本。

## 本章小结

| 组件 | 功能 |
|------|------|
| read_line | 逐字符 sys_read + 回显 + backspace 处理 + NUL 终止 |
| tokenize | 空格/制表符切割，原地 NUL 终止，跳过前导/尾部空白 |
| CmdEntry 分发表 | 数据驱动的命令注册/查找，哨兵终止 |
| cmd_echo / cmd_help / cmd_clear | 三个内置命令，各有独立 .cpp 文件 |
| Console ANSI parser | 三状态状态机，支持 ESC[2J 清屏 + ESC[H 光标归位 |
| user libc (string/printf) | 无标准库环境下的字符串操作和格式化输出 |
