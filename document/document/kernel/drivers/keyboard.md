# 键盘驱动

> 里程碑: `014_driver_keyboard`

## 功能概述

PS/2 键盘驱动，使用扫描码集 1，支持 Shift/Ctrl/Alt 修饰键，环形缓冲队列 + IRQ1 中断驱动。

## 初始化 (`keyboard_init()`)
1. 禁用 PS/2 设备 (`0xAD`/`0xA7`)
2. 刷新输出缓冲
3. 写配置字节 (IRQ1 on, mouse IRQ12 off)
4. 控制器自测 (`0xAA` → 期望 `0x55`)
5. 重新启用 (`0xAE`)

## 数据结构
- `KeyEvent {ascii, scancode, pressed, shift, ctrl, alt}`
- `sc_to_ascii_lower[128]` / `sc_to_ascii_upper[128]` — 查找表
- `key_queue[64]` 环形缓冲 (head/tail)

## API (`kernel/drivers/keyboard/keyboard.hpp/cpp`)
- `keyboard_init()` — 初始化并注册 IRQ1 handler
- `keyboard_poll(KeyEvent& out)` — dequeue 取键事件

## 中断处理
- IRQ1 handler (vector `0x21`): 读 `0x60` → enqueue → `PIC::send_eoi(1)`
- Break code = makecode | 0x80

## 源码位置
- `kernel/drivers/keyboard/keyboard.hpp/cpp`
