# F13: GUI 分离 — Kernel-GUI ABI 定义

> 定义内核侧必须为 GUI 提供的接口契约（ABI）。
> 目标：独立 GUI 仓库基于此 ABI 开发，内核提供 thin adapter。

## 核心思路

GUI 仓库不知道内核内部实现，只通过稳定 ABI 访问：

```
GUI 独立仓库
    │  只依赖 gui_abi.h
    ↓
Kernel-GUI ABI 层（gui_abi.hpp）  ← 稳定接口，不暴露内核内部
    ↓
内核 thin adapter（gui_adapter.cpp）← 实现转发到内核服务
```

## GUI 对内核的依赖分类

| 类别 | 需要的接口 | 当前直接调用 |
|------|-----------|-------------|
| **渲染** | Framebuffer 访问、Canvas 分配 | `Framebuffer::data()`, `Canvas::init()` |
| **输入** | 键盘/鼠标事件流 | `Keyboard::irq1_handler` → `EventQueue` |
| **进程** | Shell 启动、进程管理 | `fork()`, `execve()`, `waitpid()` |
| **IPC** | Pipe 创建、数据传输 | `Pipe::try_read/write`, `PipeOps` |
| **VFS** | 文件描述符管理 | `Inode`, `File`, `FDTable` |
| **内存** | Canvas 缓冲区分配 | `PMM::alloc_page`, `VMM::map` |
| **定时** | 周期刷新回调 | `PIT::set_tick_callback` |
| **日志** | 调试输出 | `kprintf` |

## 文件清单

| 文件 | 说明 |
|------|------|
| [00-gui-abi.md](00-gui-abi.md) | M1: Kernel-GUI ABI 接口定义 |
| [02-gui-adapter.md](02-gui-adapter.md) | M2: 内核侧 thin adapter 实现 |
| [03-gui-decouple.md](03-gui-decouple.md) | M3: 解耦现有 GUI 代码 |

## Milestone 依赖

```
M1 ABI 定义 ──→ M2 内核 adapter ──→ M3 解耦现有 GUI
```

## 关键代码位置

| 模块 | 文件 |
|------|------|
| GUI 初始化 | `kernel/gui/gui_init.hpp` / `.cpp` — 最复杂的耦合点 |
| 事件系统 | `kernel/gui/event.hpp` — SPSC ring buffer |
| 窗口管理 | `kernel/gui/window_manager.hpp` / `.cpp` |
| 终端 | `kernel/gui/terminal.hpp` / `.cpp` — Shell 集成 |
| 窗口 | `kernel/gui/window.hpp` / `.cpp` |
| Canvas | `kernel/drivers/canvas.hpp` — 双缓冲渲染 |
| Framebuffer | `kernel/drivers/video/framebuffer.hpp` |
| 字体 | `kernel/drivers/video/font.hpp` — PSF2 |
| Mouse | `kernel/drivers/mouse.hpp` — PS/2 事件推送 |
| Keyboard | `kernel/drivers/keyboard/keyboard.hpp` — 按键事件 |

## 验收标准

1. ABI 头文件定义完整（gui_abi.hpp），无内核内部头文件依赖
2. 独立仓库可以 `#include "gui_abi.hpp"` 编译通过
3. 现有 GUI 功能不回归
4. 每个 ABI 函数有文档说明（语义、线程安全、生命周期）
