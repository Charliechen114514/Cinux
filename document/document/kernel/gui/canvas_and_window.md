# Canvas & Window

> 里程碑: `029_gui_canvas`

## 功能概述

GUI 渲染基础层: Canvas 提供双缓冲 2D 绘图，Window 是可管理的 GUI 窗口单元，具有标题栏和关闭按钮。仅在 `CINUX_GUI` 模式下编译。

## Canvas (`kernel/drivers/canvas.hpp/cpp`)
- `front_buf_`: Framebuffer 显存指针
- `back_buf_`: `kmalloc` 分配的离屏缓冲
- 绘图: `draw_pixel/draw_rect/draw_rect_outline/draw_line/draw_text/blit/flip/clear`
- `draw_line`: Bresenham 算法
- `flip()`: back → front 按 pitch 逐行 memcpy
- CMake: `option(CINUX_GUI "Enable GUI mode" ON)`

## Window (`kernel/gui/window.hpp/cpp`)

### 属性
```cpp
class Window {
    int x_, y_, w_, h_;
    char title_[64];
    Canvas* canvas_;
    bool visible_, focused_;
    int id_;
};
```

### 外观
- 标题栏: 蓝色背景 + 白色标题文字
- 关闭按钮: 红色小方块
- off-screen content canvas

### API
- `draw_title_bar()` / `draw_content()` — 绘制各部分
- `blit_to(screen_canvas)` — 将内容 blit 到屏幕
- `set_position(x, y)` / `resize(w, h)`
- `is_close_button_hit(mx, my)` / `contains(mx, my)`
- `on_key(KeyEvent&)` / `on_paint(Canvas&)` — 虚函数 (031 添加)

### CLI 模式
- Canvas 和 Window 不参与编译
- Console 直接写 Framebuffer front buffer

## 源码位置
- `kernel/drivers/canvas.hpp/cpp` — Canvas 绘图
- `kernel/gui/window.hpp/cpp` — Window 窗口
