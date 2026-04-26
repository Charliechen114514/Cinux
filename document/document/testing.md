# Host 测试基建

> 里程碑: 全部 (支撑性)

## 功能概述

Host 端轻量级单元测试框架，在开发机上通过 mock 运行内核组件测试，无需 QEMU。与内核测试互补，覆盖纯逻辑和算法。

## 框架
- `test/framework/test_framework.h` — `TEST`/`ASSERT` 宏定义
- CTest 集成，`ctest` 运行

## 测试覆盖 (50+ 文件)

### 内存管理
- `test_pmm.cpp` — mock 内存图，1000 次 alloc/free
- `test_vmm.cpp` — mock PMM，map→translate→unmap
- `test_heap.cpp` — 1000 次随机 alloc/free，double-free panic

### 文件系统
- `test_fd_table.cpp` — 24 用例
- `test_vfs_mount.cpp` — 19 用例
- `test_ramdisk.cpp` — lookup/InodeOps

### GUI
- `test_canvas.cpp` — mock Framebuffer，绘制验证
- `test_font.cpp` — PSF2 解析
- `test_console.cpp` — 字符输出
- `test_mouse.cpp` — mock 8042 I/O
- `test_event_queue.cpp` — 环形缓冲
- `test_window_manager.cpp` — mock Canvas，Z-order
- `test_terminal.cpp` — 字符缓冲
- `test_bitmap_icon.cpp` — draw_bitmap 像素验证
- `test_desktop.cpp` — 图标 hit test

### 进程
- `test_fork_exec.cpp` — PID 分配/释放/CoW

### IPC
- `test_pipe.cpp` — 26 项
- `test_sys_pipe.cpp`

### 其他
- `test_kprintf.cpp` — mock Serial，格式化验证
- `test_keyboard.cpp` — 扫描码转换
- `test_shell_redirect.cpp`

## 构建与运行
```bash
cd build && make test_host
```

## 源码位置
- `test/framework/test_framework.h` — 测试框架
- `test/unit/` — 所有 host 端单元测试
