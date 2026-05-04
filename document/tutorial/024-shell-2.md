# 从 Syscall 到 Shell（中）：sys_read 与用户态 REPL

> 标签：sys_read, shell, REPL, tokenize, libc
> 前置：[024-shell-1 GDT 重构与 SYSRETQ 修复](...)

## 前言

上一章我们把 GDT 和 SYSCALL 路径的坑填完了，内核侧的基础设施终于稳了。接下来我们终于可以写 shell 了——一个运行在 Ring 3 的交互式命令行程序。这听起来像是"写个 while 循环读输入然后 if-else 分发命令"那么简单，但在一个没有标准库、没有阻塞 I/O、没有进程管理的环境中，每一个细节都需要从头实现。

## 环境说明

shell 程序是独立的用户态 ELF，通过 objcopy 转成 flat binary 后嵌入内核镜像。编译使用 `-ffreestanding -nostdlib -static -no-pie`，链接到自定义 linker script（入口 0x400000）。没有 libstdc++，没有 glibc，所有基本操作（字符串操作、格式化输出）都需要自己实现。

## 第一步——sys_read：让内核能读键盘

在 shell 能读输入之前，内核必须提供一个从键盘读取数据的系统调用。sys_read 的设计参考了 Linux 的 read() 语义——接收 fd、buffer 地址、count 三个参数，返回实际读取的字节数。

核心逻辑是从 PS/2 键盘的 ring buffer 中 poll 事件。这里有一个设计选择：如果 buffer 为空怎么办？在 Linux 中，read() 会阻塞当前进程直到有数据可读。但 Cinux 目前没有阻塞 I/O 和 wait queue 机制，所以我们用了一个简单的 spin-wait——循环执行 pause 指令并检查 buffer，最多等 100 万次。pause 指令告诉 CPU "我在等资源"，可以降低功耗并改善超线程性能。

sys_read 还需要做几层过滤。键盘驱动产生的事件包含按下和释放两种——同一个按键按下时产生一次（pressed=true），松开时又产生一次（pressed=false）。如果我们不过滤释放事件，shell 就会把同一个字符"读两次"。另外，Shift、Ctrl、Alt 等修饰键的 ascii 字段为 0，也必须跳过。

还有一个细节：PS/2 键盘的 Enter 键产生的是 '\r'（carriage return, 0x0D），而 shell 期望的是 '\n'（newline, 0x0A）。sys_read 在拷贝到用户 buffer 之前做了转换——这遵循了 Unix 终端的 cooked mode 传统。

## 第二步——用户态 libc：string 工具和 printf

shell 需要字符串操作（strlen、strcmp）和格式化输出（printf），但我们没有标准库。于是我们在 `user/libc/` 目录下实现了自己的 mini libc。

string.cpp 提供五个函数：strlen（字符串长度）、strcmp（字符串比较）、memset（内存填充）、memcpy（内存拷贝）、memcmp（内存比较）。这些都是经典的逐字节实现，没有 SIMD 优化。所有函数放在 `cinux::user` 命名空间中，避免与内核的同名函数冲突。

printf.cpp 的实现使用内部 256 字节缓冲区，满时通过 sys_write 刷新。支持 %c、%s、%d、%u、%x、%p、%% 格式说明符，以及 %l/%ll 长度修饰符。写 printf 的过程本身就是一次好的教学练习——你需要处理 va_list、解析格式字符串、处理整数到字符串的转换。

说实话，实现 printf 的最初动机不是为了 shell 本身，而是为了 debug。当 shell 命令全部失效时，笔者需要打印变量值来定位问题，但内核的 kprintf 只能在 Ring 0 使用。于是先实现了用户态的 printf，然后用它来 debug shell 的问题。

### 与 xv6 的对比

xv6 也有自己的 printf 实现（printf.c），支持类似的格式说明符集。两者的设计思路几乎相同——内部缓冲区 + sys_write 刷新。区别在于 xv6 的 printf 支持多 fd（通过第一个参数指定），而 Cinux 的 printf 只输出到 fd=1（stdout）。xv6 的 printf 也更完善——支持 %f 和字段宽度，Cinux 的版本只实现了最基本的子集。

## 第三步——Shell REPL：read_line、tokenize、dispatch

现在我们有 sys_read 能读键盘了，有 libc 能做字符串操作了，可以开始写 shell 主循环了。

shell 的整体架构遵循经典的 REPL 模式：打印提示符 -> 读一行输入 -> 切分成词法单元 -> 查找并执行命令 -> 回到开头。这种模式从 1970 年代的 Thompson shell 一直延续到今天的 bash 和 zsh。

### read_line：逐字符读取与行编辑

read_line 函数每次调用 sys_read(0, &c, 1) 读取一个字符。读到 '\n' 时回显换行并停止。读到 0x7F（DEL）或 0x08（BS）时回退——在屏幕上擦除一个字符的方法是输出 `\b \b`（退格+空格+退格）。正常字符回显后存入 buffer。这种"读一个字符、回显一个字符"的模式叫做 echo mode，几乎所有交互式 shell 都使用它。

这里有一个设计上的取舍。Cinux 的 sys_read 本身可以一次读多个字符（直到遇到 '\n'），这样 read_line 可以简化为一次 sys_read 调用。但我们选择了逐字符读取，因为这样可以在 read_line 层面实现行编辑（backspace 处理）。如果 sys_read 一次返回整行，那行编辑逻辑就得在内核里做（Linux 的 line discipline 就是这么干的），但这会让 sys_read 的实现复杂很多。

### tokenize：空格切分

tokenize 函数按空格和制表符将输入行切割成 argv 数组。它在原字符串的空白处写入 NUL，argv 数组的每个元素指向一个子串的起始。这种"原地修改"的设计与 Unix 的 execve argv 传递机制完全一致——不分配新内存，直接在原 buffer 上操作。

### 数据驱动的命令分发表

Cinux shell 使用 CmdEntry 结构体数组作为命令分发表。每个条目包含命令名和函数指针，末尾用 {nullptr, nullptr} 哨兵终止。新增命令只需写一个处理函数并在表中加一行，主循环逻辑完全不用改。

与 xv6 的 shell 相比，Cinux 的命令分发简单得多。xv6 的 sh.c 使用递归下降解析器（parsecmd -> parseline -> parsepipe -> parseexec），支持管道、重定向、后台执行和命令列表。Cinux 只做简单的空格切分和线性扫描——但这就够用了，因为我们没有文件系统，没有 fork/exec，不需要外部命令执行能力。

三个内置命令的实现都很直接。cmd_echo 遍历参数用 sys_write 输出，参数间加空格，末尾换行。cmd_help 输出硬编码的帮助文本。cmd_clear 发送 ANSI 转义序列清屏——不过这还需要 Console 驱动的配合，我们在下一章讲。

### 与 Linux Bash 的差距

不用说，Cinux shell 和 bash 的差距是天文数字级别的。bash 支持变量展开、命令替换、条件判断、循环、函数定义、通配符匹配等高级特性。但 Cinux shell 的目标不是取代 bash——它只是一个教学验证工具，证明 syscall 端到端通路工作正常。从架构角度看，Cinux shell 的 REPL 循环和 bash 的基本循环是一样的，只是缺少了后面的复杂层。

## 收尾

到这里，shell 的主体功能已经实现了。在下一章中，我们会完善 Console 的 ANSI 转义序列支持（让 clear 命令真正能清屏），添加测试覆盖，并做端到端验证。

## 参考资料

- xv6 shell (sh.c): 经典教学 shell 实现，支持管道/重定向
  - URL: https://github.com/mit-pdos/xv6-public/blob/master/sh.c
- MIT 6.828 Shell Lab: 理解 shell 的系统调用接口
  - URL: https://pdos.csail.mit.edu/6.828/2019/labs/sh.html
- Linux read(2) man page: Unix read 系统调用语义
- Thompson shell (1975): Unix 最早期的 shell 设计
