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
void Canvas::blit(uint32_t dst_x, uint32_t dst_y, Canvas& src,
                  uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
    if (back_buf_ == nullptr || src.back_buf_ == nullptr) return;

    uint32_t dst_pixels_per_row = pitch_ / 4;
    uint32_t src_pixels_per_row = src.pitch_ / 4;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t src_row = sy + row;
        uint32_t dst_row = dst_y + row;

        if (src_row >= src.height_ || dst_row >= height_)
            break;

        for (uint32_t col = 0; col < w; col++) {
            uint32_t src_col = sx + col;
            uint32_t dst_col = dst_x + col;

            if (src_col >= src.width_ || dst_col >= width_)
                break;

            back_buf_[dst_row * dst_pixels_per_row + dst_col] =
                src.back_buf_[src_row * src_pixels_per_row + src_col];
        }
    }
}
```

blit 遍历 h 行 w 列，将源 Canvas 中 `(sx, sy)` 起始的矩形区域逐像素拷贝到当前 Canvas 的 `(dst_x, dst_y)` 位置。行和列方向的边界检查（`src_row >= src.height_`、`dst_row >= height_`、`src_col >= src.width_`、`dst_col >= width_`）确保不会越界读写。两个 Canvas 可能有不同的 pitch，所以分别计算 `pixels_per_row`。

注意 tag 029 的 blit 使用 `uint32_t` 坐标——不支持负数目标坐标（窗口部分在屏幕外的情况）。这个能力将在后续 tag 030（窗口管理器）中添加，届时窗口拖动到屏幕边缘外时需要负坐标裁剪。

## 设计决策

### 决策：blit 目标坐标用有符号还是无符号

**问题**: blit 的目标坐标应该用什么类型？

**tag 029 的做法**: uint32_t，所有坐标必须非负。blit 只做简单的越界检查（src_row >= src.height_ / dst_row >= height_ 等）。

**后续方向（tag 030）**: int32_t，支持窗口部分在屏幕外（被拖到屏幕边缘外）。届时 blit 需要处理负数 dst_x/dst_y——计算 col_skip 跳过屏幕外的源列，调整有效起始位置来补偿偏移量。这种「底层处理裁剪」的设计让窗口管理器只需要传入实际坐标（可能为负），不需要自己做复杂的可见区域计算。

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
- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/VBE) — fillrect 优化技巧
- SerenityOS: [LibGfx Painter](https://github.com/SerenityOS/serenity) — draw_line 支持 anti-aliasing、fill_rect 支持 alpha blend
