# 窗口管理器 & 事件系统

> 里程碑: `030_gui_wm_basic`

## 功能概述

窗口管理器负责 Z-order 管理、鼠标/键盘事件分发、屏幕合成 (compositing)。通过 PIT tick 定时刷新屏幕。仅在 `CINUX_GUI` 模式下编译。

## 事件系统 (`kernel/gui/event.hpp/cpp`)

### 类型
```cpp
enum class EventType { MouseMove, MouseDown, MouseUp, KeyDown, KeyUp };
struct Event {
    EventType type_;
    union { MouseEvent mouse; KeyEvent key; };
};
```

### EventQueue
- 环形缓冲 `buf_[128]` + head/tail
- `enqueue(Event)` / `dequeue(Event&)` — 满/空边界处理

## 窗口管理器 (`kernel/gui/window_manager.hpp/cpp`)

### 属性
```cpp
class WindowManager {
    Window* windows_[64];
    int count_;
    Window* focused_;
    int mouse_x_, mouse_y_;
    DesktopIcon icons_[16];
    int icon_count_;
    IconAction pending_icon_action_;
};
```

### API
- `init(canvas)` — 初始化 WM
- `create(title, w, h)` — 创建窗口
- `destroy(id)` — 销毁窗口
- `raise(id)` — 提升到最前 Z-order
- `composite()` — 从低到高 blit 各窗口到 back_buf → `flip()`

### 事件处理
- `handle_mouse(Event&)` — 拖动检测 (left button + 移动)，更新 focused window 位置，关闭按钮点击 → `destroy()`
- `handle_key(Event&)` — 转发到 focused window 的 `on_key`

### 桌面图标
- `add_desktop_icon(DesktopIcon)` — 注册图标
- `hit_test_icon(mx, my)` — 点击检测
- `consume_pending_icon_action()` — 消费待处理的图标动作
- `composite()` 流程: clear → draw_desktop_icons → blit 窗口

## 集成
- PIT tick → `composite()` 定时刷新
- Mouse IRQ → `handle_mouse()`
- Keyboard IRQ → `handle_key()` + key_queue (CLI fallback)

## 源码位置
- `kernel/gui/event.hpp/cpp` — 事件定义
- `kernel/gui/window_manager.hpp/cpp` — 窗口管理器
