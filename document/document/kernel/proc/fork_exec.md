# fork / execve / waitpid

> 里程碑: `034_process_fork_exec` (子迭代 2-4)

## 功能概述

完整的 Unix 风格进程创建与执行机制。`fork()` 通过 Copy-on-Write 复制页表，`execve()` 从 VFS 加载新 ELF 替换地址空间，`waitpid()` 回收子进程。

## fork() — CoW 页表复制

### 流程 (`kernel/proc/process.cpp`)
1. 复制当前 TCB → 分配新 PID
2. CoW 复制页表: 标记所有 PTE 只读
3. 继承文件描述符表
4. 子进程返回 0，父进程返回 child PID

### CoW 缺页处理
- page fault handler 检测: 只读 PTE + CoW 标记
- 分配新物理页 → 复制内容 → 更新 PTE 为可写 → 清除 CoW 标记

## execve() — ELF 加载

### 流程 (`kernel/proc/process.cpp`)
1. 从 VFS 读取 ELF 文件
2. 解析 program headers
3. 加载到地址空间 (新的 `AddressSpace`)
4. 设置入口点
5. 清理旧映射

## waitpid() — 子进程回收

### 流程 (`kernel/syscall/sys_waitpid.hpp/cpp`)
1. 父进程阻塞等待指定子进程退出
2. 收集 `exit_status`
3. 清理子进程 TCB (防 zombie)

### 进程状态
- `Zombie`: 子进程已退出，等待父进程 `waitpid` 回收
- `Dead`: TCB 已清理完毕

## 系统调用
- `SYS_fork` — 创建子进程
- `SYS_execve` — 替换进程映像
- `SYS_waitpid` — 等待子进程
- `SYS_exit` — 终止当前进程

## 源码位置
- `kernel/proc/process.hpp/cpp` — fork/execve 实现
- `kernel/syscall/sys_fork.cpp` — fork 系统调用
- `kernel/syscall/sys_exit.cpp` — exit 系统调用
- `kernel/syscall/sys_waitpid.hpp/cpp` — waitpid 系统调用
- `kernel/proc/elf_types.hpp/cpp` — ELF 类型定义
