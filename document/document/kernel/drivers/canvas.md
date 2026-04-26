# Canvas 画布

> 里程碑: `029_gui_canvas` `032_gui_bitmap_icon`

## 功能概述

双缓冲 2D 绘图画布，封装 Framebuffer 提供像素级绘制、几何图形、文本渲染和位图 blit。仅在 `CINUX_GUI` 模式下编译。

## 核心类 (`kernel/drivers/canvas.hpp/cpp`)

### Canvas
- `front_buf_`: 指向 Framebuffer 显存
- `back_buf_`: `kmalloc(width*height*4)` 分配的离屏缓冲
- `width_`, `height_`, `pitch_`

### 绘图 API
- `draw_pixel(x, y, color)` — 逐像素
- `draw_rect(x, y, w, h, color)` — 矩形填充
- `draw_rect_outline(x, y, w, h, color)` — 矩形描边
- `draw_line(x0, y0, x1, y1, color)` — Bresenham 直线
- `draw_text(x, y, str, color)` — 复用 PSFFont 渲染
- `draw_bitmap(x, y, w, h, pixels[])` — 32x32 图标渲染，`0x00000000` 视为透明跳过，clip 到画布边界
- `blit(dst_x, dst_y, src_canvas, sx, sy, w, h)` — 子区域拷贝
- `flip()` — back → front 按 pitch 逐行 memcpy
- `clear(color=0)` — 清空 back buffer

### CLI 模式兼容
- Canvas 不参与编译时，Console 直接写 Framebuffer front buffer

## 源码位置
- `kernel/drivers/canvas.hpp/cpp`
