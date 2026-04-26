# 桌面 & 图标

> 里程碑: `032_gui_bitmap_icon` `033_gui_desktop`

## 功能概述

GUI 桌面环境，包含桌面背景、可点击图标 (Shell、Calculator)。点击 Shell 图标弹出终端窗口。32x32 像素图标定义为 `constexpr` 数据。

## 位图图标 (`kernel/gui/icon.hpp`)
- `constexpr uint32_t ICON_SIZE = 32`
- `k_shell_icon[1024]` — 黑色终端 + 白色 `>_` 提示符
- `k_calc_icon[1024]` — 灰色机身 + 按键网格

### 渲染
- `Canvas::draw_bitmap(x, y, w, h, pixels[])` — 逐像素绘制
- `0x00000000` 视为透明跳过
- clip 到画布边界

## 桌面图标 (`kernel/gui/desktop_icon.hpp`)
```cpp
enum class IconAction : uint8_t { None, OpenShell, OpenCalculator };
struct DesktopIcon {
    int x, y;
    uint32_t* bitmap;
    const char* label;
    int width, height;
    IconAction action;
    bool contains(int mx, int my); // inline hit test
};
```

## 桌面集成 (`kernel/gui/gui_init.hpp/cpp`)
- `gui_start()` 中注册两个 DesktopIcon:
  - Shell @ (40, 40)
  - Calculator @ (40, 120)
- `gui_tick_callback` 检查 `consume_pending_icon_action()`
- `OpenShell` → `create_shell_terminal()`

## WM 合成流程
1. `clear()` 背景色
2. `draw_desktop_icons()` — 绘制图标位图 + 居中标签
3. blit 所有窗口 (低→高 Z-order)
4. `flip()`

## 交互
- MouseDown + 无窗口命中 → `hit_test_icon()`
- 命中 → 设 `pending_icon_action_`
- 下次 tick → 消费 action → 创建对应窗口

## 源码位置
- `kernel/gui/icon.hpp` — constexpr 图标像素数据
- `kernel/gui/desktop_icon.hpp` — DesktopIcon 定义
- `kernel/gui/gui_init.hpp/cpp` — 桌面初始化
- `kernel/gui/window_manager.hpp/cpp` — 图标绘制 & hit test
- `kernel/drivers/canvas.hpp/cpp` — draw_bitmap
