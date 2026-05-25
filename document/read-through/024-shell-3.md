---
title: 024-shell-3 · Shell
---

# 024-shell-3 Read-through: 用户态 Shell 与 libc

## 概览

本文覆盖 tag 024 中用户态 shell 程序和用户态 libc 的完整代码。shell 是运行在 Ring 3 的交互式命令行，通过 sys_read 读取键盘输入、sys_write 输出字符，实现了 REPL 循环。libc 提供 shell 所需的基本字符串操作和格式化输出功能。

## 架构图

```
    用户态 (Ring 3)
    ================

    _start()
      │
      ▼
    shell_main()
      │
      ├── write_str("Cinux shell - type 'help' for commands\n")
      │
      └──► while (true)
            │
            ├── write_str("cinux> ")          ← 打印提示符
            │
            ├── read_line(line, 256)          ← 逐字符 sys_read
            │     │                              + 回显 + backspace
            │     └──► sys_read(0, &c, 1) × N
            │
            ├── tokenize(line, argv, 16)      ← 空格切分
            │
            └──► for (CmdEntry : builtin_cmds)
                  │
                  ├── strcmp(argv[0], name)?
                  │     ├── cmd_echo(argc, argv)  ← sys_write 参数
                  │     ├── cmd_help(argc, argv)  ← sys_write 帮助文本
                  │     └── cmd_clear(argc, argv) ← sys_write ANSI 序列
                  │
                  └── "command not found"
```

## 代码精讲

### shell.hpp — 命令分发表声明

```cpp
#pragma once
#include <cstddef>

struct CmdEntry {
    const char* name;
    void (*handler)(int argc, char** argv);
};

void cmd_echo(int argc, char** argv);
void cmd_help(int argc, char** argv);
void cmd_clear(int argc, char** argv);
```

CmdEntry 是一个简单的 name-handler 对——name 是命令名字符串，handler 是函数指针。哨兵终止用 {nullptr, nullptr}，遍历时检查 name != nullptr 即可。这种设计模式在 Unix 世界里非常常见——Linux 的内核启动参数处理、xv6 的文件系统操作表都用了类似的表驱动设计。

### main.cpp — Shell REPL 主循环

```cpp
#include "shell.hpp"
#include "libc/string.hpp"
#include "libc/syscall.h"

using cinux::user::strcmp;
using cinux::user::strlen;

constexpr size_t MAX_LINE = 256;
constexpr size_t MAX_TOKENS = 16;
constexpr const char PROMPT[] = "cinux> ";
```

shell 使用自己的 libc 函数——cinux::user::strcmp 和 strlen，而不是标准库的版本。所有常量都用 constexpr 定义，编译期求值。

```cpp
namespace {
void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}

void write_buf(const char* buf, size_t len) {
    sys_write(1, buf, len);
}
}  // anonymous namespace
```

write_str 和 write_buf 是两个 I/O 辅助函数——前者输出 NUL 终止的字符串（先计算长度），后者输出指定长度的缓冲区。它们被放在匿名命名空间中，链接时不可见，避免与其他 .cpp 文件中的同名函数冲突。

#### read_line — 逐字符读取与行编辑

```cpp
size_t read_line(char* buf, size_t cap) {
    size_t pos = 0;

    while (pos < cap - 1) {
        char c = 0;
        int64_t n = sys_read(0, &c, 1);
        if (n <= 0) {
            continue;
        }

        if (c == '\n') {
            write_buf("\n", 1);
            break;
        }

        if (c == 0x7F || c == '\b') {
            if (pos > 0) {
                --pos;
                write_buf("\b \b", 3);
            }
            continue;
        }

        // Echo back and store
        write_buf(&c, 1);
        buf[pos++] = c;
    }

    buf[pos] = '\0';
    return pos;
}
```

read_line 是 shell 的核心 I/O 函数。每次循环调用 sys_read(0, &c, 1) 读取一个字符。sys_read 返回 0 或负值时（spin-wait 超时或错误），continue 跳过不做任何处理。

换行处理：收到 '\n' 后先回显换行（write_buf("\n", 1)），然后 break 退出循环。注意换行字符本身不存入 buffer——buffer 内容不包含末尾的 '\n'。

退格处理：收到 0x7F（DEL）或 0x08（BS）时，如果 pos > 0 则回退。回显 "\b \b" 是经典的终端擦除技巧——\b 将光标左移一位，空格覆盖该位置的字，再 \b 将光标移回。这样在视觉上就"删除"了一个字符。如果 pos 已经是 0（光标在行首），退格被忽略。

正常字符：回显（write_buf(&c, 1)）并存入 buffer，pos 递增。

循环结束后 NUL 终止 buffer，返回 pos（不含 NUL 的字符数）。cap - 1 的检查确保 buffer 永远不会溢出——最后一个位置留给 NUL。

#### tokenize — 空格切分

```cpp
size_t tokenize(char* line, char** argv, size_t max_tokens) {
    size_t argc = 0;

    while (*line != '\0' && argc < max_tokens) {
        while (*line == ' ' || *line == '\t') {
            ++line;
        }
        if (*line == '\0') {
            break;
        }

        argv[argc++] = line;

        while (*line != '\0' && *line != ' ' && *line != '\t') {
            ++line;
        }

        if (*line != '\0') {
            *line++ = '\0';
        }
    }

    return argc;
}
```

tokenize 是原地修改的字符串切分——在空白处写入 NUL，将输入行变成多个独立的 NUL 终止子串，argv 数组的每个元素指向一个子串的起始。这种设计与 Unix 的 execve argv 传递完全一致。

外层循环每轮处理一个 token。首先跳过前导空白（空格和制表符），遇到 NUL 说明行末尾全是空白，break 退出。然后将当前位置记为 argv[argc] 并递增 argc。接着推进到下一个空白或行尾。最后，如果当前位置不是 NUL，将其替换为 NUL 并前进一位——这就是"切割"操作。

#### 命令分发表与主循环

builtin_cmds 是编译期常量数组，包含三个命令条目和一个 {nullptr, nullptr} 哨兵。shell_main() 进入无限循环：打印 prompt、调用 read_line、tokenize、线性扫描 builtin_cmds 表匹配命令。匹配到则调用 handler，没匹配到输出 "command not found"。_start() 调用 shell_main() 后跟 sys_exit(0) 作为安全兜底。

dispatch 逻辑是简单的线性扫描——遍历 builtin_cmds 直到匹配或到达哨兵。对于三个命令来说线性扫描足够了。如果将来命令数量增多，可以改用二分查找或哈希表。

### cmd_echo.cpp

```cpp
#include "shell.hpp"
#include "libc/string.hpp"
#include "libc/syscall.h"

using cinux::user::strlen;

namespace {
void write_str(const char* s) {
    sys_write(1, s, strlen(s));
}
}  // anonymous namespace

void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (i > 1) {
            sys_write(1, " ", 1);
        }
        write_str(argv[i]);
    }
    sys_write(1, "\n", 1);
}
```

cmd_echo 从 argv[1] 开始遍历（argv[0] 是 "echo" 本身），参数之间用空格分隔，最后输出换行。如果没有参数（argc==1），只输出一个空行。这与 Unix echo 命令的行为一致。

### cmd_help.cpp

```cpp
void cmd_help(int /*argc*/, char** /*argv*/) {
    write_str(
        "Available commands:\n"
        "  echo <args...>  - print arguments to stdout\n"
        "  help            - show this help message\n"
        "  clear           - clear the screen\n"
    );
}
```

cmd_help 输出硬编码的帮助文本。C++ 的相邻字符串字面量自动拼接，所以这种多行写法和一行长字符串等价。如果将来命令增多，可以考虑让每个 CmdEntry 增加 description 字段，然后 cmd_help 自动遍历表生成帮助文本。

### cmd_clear.cpp

```cpp
void cmd_clear(int /*argc*/, char** /*argv*/) {
    sys_write(1, "\033[2J\033[H", 7);
}
```

cmd_clear 发送 7 字节的 ANSI CSI 序列。`\033` 是 ESC 字符（0x1B），`[2J` 清除整个屏幕，`[H` 将光标移到 (1,1)。Console 驱动的 ANSI 解析器会处理这两个序列。

### user/libc/string.cpp — 字符串工具

所有函数都在 cinux::user 命名空间中。strlen、strcmp 是逐字符扫描，memset/memcpy/memcmp 是逐字节操作——经典的 freestanding 实现，没有 SIMD 优化。对教学内核来说可读性比性能更重要。这些函数的签名与标准 C 库完全一致，降低学习成本。

## 设计决策

### 决策：逐字符 sys_read 而非行缓冲
**问题**: shell 如何读取键盘输入
**本项目的做法**: read_line 每次调用 sys_read(0, &c, 1) 读一个字符
**备选方案**: sys_read 内部维护行缓冲，一次返回整行
**为什么不选备选方案**: Unix 的 read() 语义是字节流，行编辑应该由用户态负责
**如果要扩展**: 可以在 sys_read 中实现 cooked mode（行缓冲+信号处理）

### 决策：线性扫描命令表
**问题**: 如何查找命令
**本项目的做法**: for 循环线性扫描 CmdEntry 数组
**备选方案**: 二分查找（要求排序）或哈希表
**为什么不选备选方案**: 三个命令的性能差异可以忽略，线性扫描最简单
**如果要扩展**: 命令数超过 20 个时考虑排序后二分查找

## 扩展方向

- 支持命令历史（上下箭头翻阅之前的输入）
- 支持 Tab 自动补全
- 实现外部程序执行（需要 fork + exec + 文件系统）
- 添加管道和重定向支持
- 将命令表从编译期改为运行时注册（允许动态加载命令模块）

## 参考资料

- xv6 shell (sh.c): MIT 经典教学 shell 实现
  - URL: https://github.com/mit-pdos/xv6-riscv/blob/riscv/user/sh.c
- MIT 6.828 Shell Lab: 理解 Unix shell 的系统调用接口
  - URL: https://pdos.csail.mit.edu/6.828/2019/labs/sh.html
- VT100 User Guide: ANSI escape sequences
  - URL: https://vt100.net/docs/vt100-ug/chapter3.html
