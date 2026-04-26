# 鼠标驱动

> 里程碑: `030_gui_wm_basic`

## 功能概述

PS/2 鼠标驱动，解析 3 字节数据包 (buttons/dx/dy)，IRQ12 中断驱动，位置 clamp 到屏幕范围。仅在 `CINUX_GUI` 模式下编译。

## 初始化 (`Mouse::init()`)
1. 8042 CMD `0xA8` 启用鼠标辅助设备
2. `0x20` 读配置 → bit1 置 1 → `0x60` 写回 (启用 IRQ12)
3. CMD `0xD4` 发 `0xF4` 激活鼠标

## 数据结构
- `MouseEvent {dx, dy, buttons, left, right, middle}`
- 内部维护 `mouse_x_/y_`，clamp 到屏幕范围

## API (`kernel/drivers/mouse.hpp/cpp`)
- `Mouse::init()` — 初始化并注册 IRQ12 handler
- `Mouse::poll(MouseEvent& out)` — 获取最新事件

## 编译条件
- 仅 `#ifdef CINUX_GUI` 时编译
- CLI 模式下鼠标驱动和 IRQ12 均不注册

## 源码位置
- `kernel/drivers/mouse.hpp/cpp`
