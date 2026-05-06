# M4: CFBox 集成 + init 系统

> CFBox (Cinux Flat Box) BusyBox 替代品接入 + PID 1 init。
> 需要前面 M1-M3 的 libc/动态链接/TTY 完善。

## 目标

1. CFBox 在 Cinux 上编译运行
2. PID 1 init 进程启动系统服务

## 任务清单

### T1: CFBox 交叉编译

**仓库**: https://github.com/Awesome-Embedded-Learning-Studio/CFBox

- [ ] Cinux syscall 接口对齐到 Linux x86_64 ABI
- [ ] CMake 交叉编译工具链配置
- [ ] 验证基本 applets 运行：ls, cat, cp, mv, rm, mkdir, echo, pwd
- [ ] 验证 Shell applet：交互式命令执行
- [ ] 验证 pipe/redirection：ls | grep, cat > file

### T2: init 系统

**文件**: `user/init/`（或使用 CFBox 的 init applet）

PID 1 职责：
- [ ] 挂载文件系统（/proc, /dev, /tmp, /dev/shm）
- [ ] 启动系统服务（可配置）
- [ ] 作为孤儿进程的回收者（waitpid for adopted children）
- [ ] 信号处理：SIGINT → 无操作（Ctrl+C 不杀 init）
- [ ] 启动默认 shell（/bin/sh → CFBox sh）

### T3: 启动流程集成

当前：kernel → first user process (shell)
改为：kernel → /sbin/init → mount filesystems → /bin/sh

- [ ] 修改 kernel main.cpp：第一个用户进程改为 /sbin/init
- [ ] init 脚本配置文件（/etc/inittab 或硬编码）
- [ ] 控制台设置：stdin/stdout/stderr → /dev/console

### T4: 用户态工具链

- [ ] CMake 交叉编译工具链文件 (toolchain-cinux.cmake)
- [ ] sysroot 布局：/bin, /sbin, /lib, /etc, /dev, /proc, /tmp
- [ ] 示例 Makefile：用户程序编译到 Cinux

## 产出物

- [ ] CFBox 在 Cinux 运行
- [ ] init 进程启动流程
- [ ] 交叉编译工具链
- [ ] 基本文件系统布局
