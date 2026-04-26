# 用户态 C 库

> 里程碑: `023_syscall` `024_shell`

## 功能概述

用户态 freestanding C 库，提供系统调用内联汇编封装、字符串操作和 printf。是 shell 和用户程序的基础。

## 系统调用封装 (`user/libc/syscall.h`)
- `_syscall(nr, a, b, c)` — 内联汇编 `syscall` 指令封装
- 高级 wrapper:
  - 文件: `open/close/read/write`
  - 进程: `fork/execve/waitpid/exit/getpid/getppid`
  - 目录: `mkdir/rmdir/chdir/getcwd/getdents`
  - 其他: `pipe/stat/yield`

## 字符串操作 (`user/libc/string.hpp/cpp`)
- `memset/memcpy/memmove/memcmp/strcmp/strncmp/strlen`

## printf (`user/libc/printf.hpp/cpp`)
- 用户态格式化输出，通过 `write(1, ...)` 系统调用

## 源码位置
- `user/libc/syscall.h` — 系统调用封装
- `user/libc/string.hpp/cpp` — 字符串操作
- `user/libc/printf.hpp/cpp` — printf
