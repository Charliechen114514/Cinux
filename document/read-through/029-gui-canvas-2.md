# 图元绘制：Bresenham 直线、矩形描边、位图与 Blit

> 标签：Bresenham, draw_line, draw_rect_outline, draw_bitmap, blit
> 前置：[029 Canvas 初始化](029-gui-canvas-1.md)

## 概览

本文是 029_gui_canvas Read-through 系列的第二篇，聚焦于 Canvas 的高级图元绘制方法。我们要拆解的代码包括：`draw_rect_outline()`（矩形描边）、`draw_line()`（Bresenham 直线）、`draw_bitmap()`（位图渲染）和 `blit()`（画布间像素拷贝）。这些方法构成了一个完整的 2D 图形原语库。

关键设计决策一览：
- Bresenham 算法处理全象限，使用 int32_t 避免浮点
- 描边矩形拆分为四条独立线段
- 位图渲染使用色彩键透明（0x00000000 = 透明）
- blit 支持有符号目标坐标（窗口部分在屏幕外）

## 架构图

```
Canvas 绘制方法层级:

  draw_pixel (底层)
    ├── draw_rect_outline (四条线段组合)
    ├── draw_line (Bresenham)
    ├── draw_text (下一篇文章)
    └── draw_rect (直接写 back_buf_)

  独立路径:
    ├── draw_bitmap (像素数组 → back_buf_, 色彩键透明)
    └── blit (src.back_buf_ → dst.back_buf_, 有符号坐标)
```

## 代码精讲

### Canvas::draw_rect_outline()

矩形描边的实现比填充矩形更微妙——需要分别画四条 1 像素宽的边，并正确处理退化情况和角落像素。

```cpp
void Canvas::draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    // Top edge
    for (uint32_t col = x; col < x + w && col < width_; col++) {
        draw_pixel(col, y, color);
    }
    // Bottom edge
    if (y + h > 0) {
        uint32_t bottom = y + h - 1;
        if (bottom < height_) {
            for (uint32_t col = x; col < x + w && col < width_; col++) {
                draw_pixel(col, bottom, color);
            }
        }
    }
    // Left edge
    for (uint32_t row = y; row < y + h && row < height_; row++) {
        draw_pixel(x, row, color);
    }
    // Right edge
    if (x + w > 0) {
        uint32_t right = x + w - 1;
        if (right < width_) {
            for (uint32_t row = y; row < y + h && row < height_; row++) {
                draw_pixel(right, row, color);
            }
        }
    }
}
```

上边和左边直接循环调用 draw_pixel，没有特殊处理。下边和右边有一个 `if (y + h > 0)` / `if (x + w > 0)` 检查——这是因为 `y + h - 1` 在 h=0 时会产生 uint32_t 下溢（变成 0xFFFFFFFF），然后 `bottom < height_` 检查会把它过滤掉，但更安全的做法是先检查 h 或 w 是否大于 0。

四条边在角落处会有像素重叠（每个角落被画了两次），但因为写入的是同一个颜色值，这不影响视觉效果。如果后续需要支持透明度混合，需要改用更精确的四边合并绘制来避免重复。

### Canvas::draw_line() — Bresenham 直线算法

这是整个 Canvas 中算法含量最高的方法。Bresenham 算法用纯整数运算在离散网格上逼近连续直线。

```cpp
void Canvas::draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    if (back_buf_ == nullptr) return;

    int32_t dx = static_cast<int32_t>(x1) - static_cast<int32_t>(x0);
    int32_t dy = static_cast<int32_t>(y1) - static_cast<int32_t>(y0);

    int32_t step_x = (dx >= 0) ? 1 : -1;
    int32_t step_y = (dy >= 0) ? 1 : -1;

    dx = (dx >= 0) ? dx : -dx;
    dy = (dy >= 0) ? dy : -dy;

    int32_t err = dx - dy;

    uint32_t cx = x0;
    uint32_t cy = y0;

    while (true) {
        draw_pixel(cx, cy, color);
        if (cx == x1 && cy == y1) break;

        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            cx = static_cast<uint32_t>(static_cast<int32_t>(cx) + step_x);
        }
        if (e2 < dx) {
            err += dx;
            cy = static_cast<uint32_t>(static_cast<int32_t>(cy) + step_y);
        }
    }
}
```

算法的核心逻辑分三步理解。第一步：计算 dx、dy 的绝对值和步进方向。通过将坐标转为 int32_t 来计算差值，确保能处理负方向（比如从右下到左上的直线）。step_x 和 step_y 记录了原始方向。

第二步：初始化误差 err = dx - dy。这个误差值表示「在主方向和副方向上应该各走多少步」。当 err 偏向 dx（err 比较大）时，在 x 方向多走一步；偏向 -dy（err 比较小）时，在 y 方向多走一步。

第三步：循环。每次画当前像素，然后根据 `2*err` 的值决定步进。`e2 > -dy` 意味着 err 足够大，x 方向走一步；`e2 < dx` 意味着 err 足够小，y 方向走一步。两个条件可以同时满足（对角线移动）。当到达终点时退出循环。

cx 和 cy 的更新使用了 `static_cast<uint32_t>(static_cast<int32_t>(cx) + step_x)` 这种看起来冗余的写法。原因是我们需要先做有符号加法（step_x 可能是 -1），然后再转回无符号。直接 `cx += step_x` 在 step_x = -1 且 cx = 0 时会下溢。

### Canvas::draw_bitmap() — 像素数组渲染

```cpp
void Canvas::draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels) {
    if (back_buf_ == nullptr || pixels == nullptr) return;

    uint32_t pixels_per_row = pitch_ / 4;
    for (uint32_t row = 0; row < h; row++) {
        if (y + row >= height_) break;
        for (uint32_t col = 0; col < w; col++) {
            if (x + col >= width_) break;
            uint32_t color = pixels[row * w + col];
            if (color == 0x00000000) continue;
            back_buf_[(y + row) * pixels_per_row + (x + col)] = color;
        }
    }
}
```

双层循环遍历位图的每个像素。源索引 `row * w + col` 使用位图自身的宽度 w（不是 pitch），因为传入的像素数组是紧凑排列的。目标索引使用 Canvas 的 pitch 来计算行偏移。

色彩键透明就是那个 `if (color == 0x00000000) continue;`——零值像素跳过不写。这意味着黑色 (0x00000000) 无法作为位图的有效颜色（会被当作透明），但 0x00000001（几乎纯黑）可以。在实际使用中这不是问题，因为我们选的图标颜色通常不会用到 0x00000000。

### Canvas::blit() — 画布间像素拷贝

blit 是窗口系统的基础操作——把一个 Canvas 的矩形区域拷贝到另一个 Canvas 上。

```cpp
void Canvas::blit(int32_t dst_x, int32_t dst_y, Canvas& src,
                  uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
    if (back_buf_ == nullptr || src.back_buf_ == nullptr) return;

    uint32_t dst_pixels_per_row = pitch_ / 4;
    uint32_t src_pixels_per_row = src.pitch_ / 4;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t src_row = sy + row;
        int32_t  dst_row = dst_y + static_cast<int32_t>(row);

        if (dst_row < 0) continue;
        if (dst_row >= static_cast<int32_t>(height_) || src_row >= src.height_) break;

        int32_t  col_skip  = 0;
        int32_t  eff_dst_x = dst_x;
        uint32_t eff_sx    = sx;
        if (eff_dst_x < 0) {
            col_skip  = -eff_dst_x;
            eff_dst_x = 0;
            eff_sx += static_cast<uint32_t>(col_skip);
        }

        uint32_t dst_col_start = static_cast<uint32_t>(eff_dst_x);
        uint32_t col_count     = w - static_cast<uint32_t>(col_skip);

        for (uint32_t i = 0; i < col_count; i++) {
            uint32_t src_col = eff_sx + i;
            uint32_t dst_col = dst_col_start + i;
            if (src_col >= src.width_ || dst_col >= width_) break;
            back_buf_[dst_row * dst_pixels_per_row + dst_col] =
                src.back_buf_[src_row * src_pixels_per_row + src_col];
        }
    }
}
```

这段代码的核心复杂性在于处理「窗口部分在屏幕外」的情况。dst_x 和 dst_y 是 int32_t（有符号），这意味着它们可以是负数——比如窗口顶部被拖到屏幕上方，dst_y = -30。

对于行方向：如果 dst_row < 0，整行跳过（窗口在屏幕上方）。如果 dst_row >= height_，直接 break（已经过了屏幕底部）。

对于列方向：当 dst_x < 0 时，需要计算「从源区域的第几列开始拷贝」来补偿负偏移。col_skip = -dst_x 就是需要跳过的列数。eff_dst_x 调整为 0（从屏幕最左边开始），eff_sx 调整为 sx + col_skip（跳过源区域中对应屏幕外的部分）。然后逐像素拷贝，两端的边界检查确保不会越界。

## 设计决策

### 决策：blit 目标坐标用有符号还是无符号

**问题**: blit 的目标坐标应该用什么类型？

**本项目的做法**: int32_t，支持窗口部分在屏幕外（被拖到屏幕边缘外）。

**备选方案**: uint32_t，所有坐标必须非负。窗口管理器自己负责裁剪。

**为什么不选备选方案**: 如果在窗口管理器中做裁剪，需要为每个窗口计算可见矩形，逻辑复杂且容易出 bug。把裁剪逻辑下沉到 blit 中，窗口管理器只需要传入实际的窗口坐标（可能为负），blit 自动处理裁剪。这是更简洁的分层设计。

**如果要扩展**: 可以添加一个带 alpha 混合的 blit 变体（blit_with_alpha），用于半透明窗口效果。

### 决策：draw_line 中坐标转换的安全性

**问题**: cx/cy 是 uint32_t，但 step_x/step_y 可以是 -1。如何安全更新？

**本项目的做法**: `cx = static_cast<uint32_t>(static_cast<int32_t>(cx) + step_x)`——先转有符号，加减，再转回无符号。

**备选方案**: 直接用 int32_t 存储当前坐标。

**为什么不选备选方案**: draw_pixel 接受 uint32_t 参数。如果 cx/cy 是 int32_t，每次调用 draw_pixel 都需要一次转换。而且 draw_pixel 的边界检查 `x >= width_` 对 int32_t 也需要额外的正数检查。保持 uint32_t 更一致。

## 扩展方向

- 为 draw_line 添加 Wu's anti-aliasing，让线条边缘更平滑（难度：中等）
- draw_rect_outline 支持可变线宽（1px、2px、3px），用于更粗的窗口边框（难度：低）
- blit 添加 alpha 混合模式，实现半透明窗口效果（难度：较高）
- draw_bitmap 支持 RGBA 格式（真正的 alpha 通道而非色彩键）（难度：中等）

## 参考资料

- Wikipedia: [Bresenham's Line Algorithm](https://en.wikipedia.org/wiki/Bresenham%27s_line_algorithm) — 整数直线绘制算法
- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer) — fillrect 优化技巧
- SerenityOS: LibGfx Painter — draw_line 支持 anti-aliasing、fill_rect 支持 alpha blend
