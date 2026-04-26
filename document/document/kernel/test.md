# 内核测试基建

> 里程碑: 全部 (支撑性)

## 功能概述

内核在 QEMU 内运行的轻量级测试框架。与 host 端单元测试互补，用于验证需要真实硬件环境的内核功能。

## 框架
- `kernel/test/big_kernel_test.h` — 测试宏定义
- `kernel/test/main_test.cpp` — 测试入口

## 测试覆盖

| 子系统 | 测试文件 |
|--------|----------|
| 视频 | `test_video.cpp` |
| ramdisk/VFS | `test_ramdisk.cpp` |
| VFS syscall | `test_vfs_syscall.cpp` |
| Pipe | `test_pipe.cpp` |
| sys_pipe | `test_sys_pipe.cpp` |
| 窗口虚函数 | `test_window_vtable.cpp` |
| Terminal | `test_terminal.cpp` |
| Terminal-Shell | `test_terminal_shell.cpp` |
| 位图图标 | `test_bitmap_icon.cpp` |
| 桌面 | `test_desktop.cpp` |
| fork/exec | `test_fork_exec.cpp` |
| 多终端 | `test_multi_terminal.cpp` |
| Shell 重定向 | `test_shell_redirect.cpp` |

## 构建与运行
```bash
cd build && make run-kernel-test
```

## 源码位置
- `kernel/test/` — 所有内核测试文件
