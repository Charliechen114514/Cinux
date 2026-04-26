# 系统调用 (Syscall)

> 里程碑: `023_syscall` `024_shell` `027_fs_vfs` `028b_fs_ext2_write` `028c_fs_cwd_stat` `034_process_fork_exec`

## 功能概述

基于 `syscall`/`sysretq` 指令的快速系统调用机制。通过 MSR (LSTAR/SFMASK/STAR) 配置入口，保存/恢复用户态寄存器，通过函数指针分发表路由到具体实现。所有用户指针参数均做 canonical address 校验。

## 入口/出口路径 (`kernel/arch/x86_64/syscall.S`)
- `syscall_entry`: `swapgs` → 切换内核栈 → 保存 `%rcx/%r11` + arg 寄存器 → `call syscall_dispatch`
- 出口: 恢复 → 切回用户栈 → `swapgs` → `sysretq`

## MSR 配置
- `LSTAR`: syscall 入口地址
- `SFMASK`: 至少清 IF
- `STAR`: 内核/用户段选择子

## 分发表 (`kernel/arch/x86_64/syscall.hpp/cpp`)
```cpp
using SyscallFn = int64_t(*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
SyscallFn syscall_table[256];
```

## 系统调用总表

| 编号 | 名称 | 功能 | 源码 |
|------|------|------|------|
| 0 | `SYS_read` | 读 fd / 键盘 / VFS | `kernel/syscall/sys_read.hpp` |
| 1 | `SYS_write` | 写 fd / 串口+Console / VFS | `kernel/syscall/` |
| 2 | `SYS_open` | VFS resolve→lookup→alloc fd | `kernel/syscall/` |
| 3 | `SYS_close` | FDTable close | `kernel/syscall/` |
| 24 | `SYS_yield` | 调度器 yield | `kernel/syscall/sys_yield.hpp` |
| 39 | `SYS_getpid` | 获取当前 PID | `kernel/proc/pid.cpp` |
| 57 | `SYS_fork` | CoW 页表复制 + TCB 复制 | `kernel/syscall/sys_fork.cpp` |
| 59 | `SYS_execve` | ELF 加载 + 地址空间替换 | `kernel/syscall/sys_fork.cpp` |
| 60 | `SYS_exit` | 标记 Dead + yield | `kernel/syscall/sys_exit.cpp` |
| 61 | `SYS_waitpid` | 阻塞等待子进程退出 | `kernel/syscall/sys_waitpid.hpp/cpp` |
| 78 | `SYS_getdents` | VFS readdir | `kernel/syscall/sys_getdents.hpp/cpp` |
| 80 | `SYS_chdir` | 更新进程 cwd | `kernel/syscall/` |
| — | `SYS_getcwd` | 返回当前 cwd | `kernel/syscall/` |
| — | `SYS_stat` / `SYS_fstat` | 返回文件元数据 | `kernel/syscall/sys_stat.hpp` |
| — | `SYS_creat` | 创建文件 | `kernel/syscall/` |
| — | `SYS_mkdir` | 创建目录 | `kernel/syscall/` |
| — | `SYS_unlink` | 删除文件 | `kernel/syscall/sys_unlink.cpp` |
| — | `SYS_rmdir` | 删除目录 | `kernel/syscall/` |
| — | `SYS_pipe` | 创建 pipe fd 对 | `kernel/syscall/` |

## 安全
- `sys_write`: 验证 `buf_virt < 0x800000000000`
- `sys_open`/`sys_read`/`sys_write`: x86_64 canonical address 校验 (bit 47 与 bits 48-63 一致)

## 源码位置
- `kernel/arch/x86_64/syscall.hpp/cpp` — 入口/分派
- `kernel/arch/x86_64/syscall.S` — 汇编入口
- `kernel/syscall/` — 各系统调用实现
