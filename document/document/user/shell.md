# Shell

> 里程碑: `024_shell` `027_fs_vfs` `028b_fs_ext2_write` `028c_fs_cwd_stat`

## 功能概述

用户态 read-eval-print shell，支持 11 个内置命令和输出重定向。通过系统调用与内核交互，支持文件和目录操作。

## 架构 (`user/programs/shell/main.cpp`, `user/programs/shell/shell.hpp`)
1. `print_prompt` → 显示提示符
2. `read_line` — `sys_read(fd, buf, len)` 读取输入
3. `tokenize` — 按空格切割为 argc/argv
4. `dispatch` — 查表执行内置命令
5. 循环

## 内置命令

| 命令 | 实现 | 功能 |
|------|------|------|
| `echo` | `cmd_echo.cpp` | 输出文本，支持 `> file` 重定向 |
| `help` | `cmd_help.cpp` | 打印命令列表 |
| `clear` | `cmd_clear.cpp` | ANSI 清屏 (`\033[2J\033[H`) |
| `ls` | `cmd_ls.cpp` | 列出目录 (`open→getdents→close`) |
| `cat` | `cmd_cat.cpp` | 显示文件内容 (`open→read→write→close`) |
| `cd` | `cmd_cd.cpp` | 切换目录 (`sys_chdir`) |
| `pwd` | `cmd_pwd.cpp` | 显示当前目录 (`sys_getcwd`) |
| `stat` | `cmd_stat.cpp` | 显示文件元数据 |
| `touch` | `cmd_touch.cpp` | 创建文件 (`sys_creat`) |
| `mkdir` | `cmd_mkdir.cpp` | 创建目录 (`sys_mkdir`) |
| `rm` | `cmd_rm.cpp` | 删除文件 (`sys_unlink`) |
| `rmdir` | `cmd_rmdir.cpp` | 删除目录 (`sys_rmdir`) |

## 输出重定向
- `echo hello > /file` — 解析 `>` 符号，`sys_open(path, O_WRONLY|O_CREAT)` + `sys_write`

## 构建
- CMake 嵌入 shell ELF 到磁盘镜像
- 链接地址 `0x400000` (`user/programs/shell/linker.ld`)

## 源码位置
- `user/programs/shell/main.cpp` — 入口 & 主循环
- `user/programs/shell/shell.hpp` — Shell 类
- `user/programs/shell/cmd_*.cpp` — 各命令实现
- `user/programs/shell/linker.ld` — 链接脚本
