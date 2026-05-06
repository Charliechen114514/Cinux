# Bresenham 直线与图元绘制 — 内核级 2D 图形原语

> 标签：Bresenham, draw_line, draw_rect_outline, draw_bitmap, blit
> 前置：[029 Canvas 双缓冲](029-gui-canvas-1.md)

## 前言

上一章我们搭好了 Canvas 的双缓冲骨架，能在屏幕上画实心矩形和清屏了。但一个只会画方块的图形系统显然不够——窗口需要描边、光标需要斜线、图标需要位图渲染、窗口管理需要画布间的像素拷贝。这些就是本章要实现的内容：Bresenham 直线、矩形描边、位图渲染和 blit 操作。

说实话，Bresenham 算法是本 tag 里我最喜欢的部分。这个诞生于 1962 年的算法，用纯整数加减法就能在离散网格上逼近连续直线——没有浮点运算、没有乘法、没有查表。在一个没有 FPU 支持的内核环境中（虽然 x86_64 有 SSE，但内核代码通常不用浮点指令），这种「只用整数」的特性简直是量身定做。SerenityOS 的 Painter 类同样使用 Bresenham（并扩展了抗锯齿版本），Linux 的 fbdev 驱动也用它来画内核 logo 的线条。

## 环境说明

和上一章完全相同：x86_64 QEMU + VBE 1024x768x32bpp + GCC 14 + CMake + freestanding 内核。本章的所有代码都在 `kernel/drivers/canvas.cpp` 中，不涉及新的硬件配置。

## 第一步——矩形描边的四边拆分

矩形描边看起来简单——画四条线——但有几个容易忽略的边界情况。我们先把代码摆出来再看细节：

```cpp
void Canvas::draw_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    // Top edge
    for (uint32_t col = x; col < x + w && col < width_; col++)
        draw_pixel(col, y, color);
    // Bottom edge
    if (y + h > 0) {
        uint32_t bottom = y + h - 1;
        if (bottom < height_)
            for (uint32_t col = x; col < x + w && col < width_; col++)
                draw_pixel(col, bottom, color);
    }
    // Left edge
    for (uint32_t row = y; row < y + h && row < height_; row++)
        draw_pixel(x, row, color);
    // Right edge
    if (x + w > 0) {
        uint32_t right = x + w - 1;
        if (right < width_)
            for (uint32_t row = y; row < y + h && row < height_; row++)
                draw_pixel(right, row, color);
    }
}
```

上边和左边不需要特殊处理——直接循环 draw_pixel 就行。下边和右边有一个 `y + h > 0` / `x + w > 0` 的检查，这是为了防止 `y + h - 1` 在 h=0 时发生 uint32_t 下溢。`y + h - 1` 当 h=0 时会变成 0xFFFFFFFF，后续的 `bottom < height_` 检查虽然也会把它过滤掉，但显式地先检查 h > 0 更安全也更清晰。

四条边在角落处会有像素重叠（比如左上角被上边和左边各画了一次），但因为写入的是同一个颜色值，这不是问题。如果未来需要支持半透明描边，需要改用更精确的四边合并来避免角落的 alpha 值翻倍。

## 第二步——Bresenham 直线算法

这是本章的核心。Bresenham 算法的思路是：从起点出发，每步决定「往 x 方向走」还是「往 y 方向走」还是「两个方向都走」，通过一个累积误差值来判断。整个过程只用整数加减法和比较。

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
    uint32_t cx = x0, cy = y0;

    while (true) {
        draw_pixel(cx, cy, color);
        if (cx == x1 && cy == y1) break;

        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; cx = static_cast<uint32_t>(static_cast<int32_t>(cx) + step_x); }
        if (e2 < dx)  { err += dx; cy = static_cast<uint32_t>(static_cast<int32_t>(cy) + step_y); }
    }
}
```

我们把这段代码拆成几步来理解。

首先是方向处理。dx 和 dy 通过 `int32_t` 转换计算——因为 x1 可能小于 x0（从右向左画），差值是负数。step_x 和 step_y 记录了原始方向（+1 或 -1）。然后 dx 和 dy 取绝对值，后续的误差计算只关心距离大小，不关心方向。

误差 err 初始化为 `dx - dy`。直观地说，如果 dx 远大于 dy（近水平线），err 起始值大，算法会在 x 方向多走；反之如果 dy 远大于 dx（近垂直线），err 起始值小，y 方向多走。

循环体内：先画当前像素，检查是否到达终点。然后 `e2 = 2 * err`——乘 2 是为了避免浮点，等价于比较 `err > -dy/2` 和 `err < dx/2`。`e2 > -dy` 时 x 走一步，`e2 < dx` 时 y 走一步。两个条件可以同时满足（对角线方向走一步），也可以都不满足（极端情况下不会出现，但代码是安全的）。

cx/cy 的更新用了 `static_cast<uint32_t>(static_cast<int32_t>(cx) + step_x)`。这种看似冗余的双重转换是为了安全地做有符号加减——step_x 是 -1，直接对 uint32_t 做减法在 cx=0 时会下溢。先转 int32_t，加减，再转回 uint32_t，就能正确处理所有情况。

## 第三步——位图渲染与色彩键透明

draw_bitmap 用于渲染一个像素数组到 Canvas 上。典型应用场景是图标和光标——它们是以 `uint32_t[]` 形式存储的像素矩阵。

```cpp
void Canvas::draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t* pixels) {
    if (back_buf_ == nullptr || pixels == nullptr) return;
    uint32_t pixels_per_row = pitch_ / 4;

    for (uint32_t row = 0; row < h; row++) {
        if (y + row >= height_) break;
        for (uint32_t col = 0; col < w; col++) {
            if (x + col >= width_) break;
            uint32_t color = pixels[row * w + col];
            if (color == 0x00000000) continue;   // 色彩键透明
            back_buf_[(y + row) * pixels_per_row + (x + col)] = color;
        }
    }
}
```

双层循环的源索引用 `row * w + col`（位图自身的紧凑排列），目标索引用 pitch-based 计算。`if (color == 0x00000000) continue;` 是色彩键透明——零值像素不写入，保持背景原样。这是最简单的透明机制：不需要混合运算，性能零开销。代价是纯黑 (0x00000000) 无法作为位图的有效颜色，但实际图标和光标几乎不会用纯黑——选择 0x00000001 或任何非零值都可以表示「几乎纯黑但不透明」。

## 第四步——blit 画布间像素拷贝

blit 是窗口系统的核心操作。每个窗口在自己的离屏 Canvas 上绘制内容，窗口管理器通过 blit 把窗口区域合成到屏幕 Canvas 上：

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

blit 遍历 h 行 w 列，将源 Canvas 中 `(sx, sy)` 起始的矩形区域逐像素拷贝到当前 Canvas 的 `(dst_x, dst_y)` 位置。行和列方向都有边界检查确保不会越界。两个 Canvas 各自用 pitch 计算行步进——因为源和目标可能有不同的 pitch。

tag 029 的 blit 使用 uint32_t 坐标，不支持负数目标坐标。窗口部分在屏幕外（负坐标裁剪）的能力将在 tag 030 中添加——届时 dst_x 和 dst_y 会改为 int32_t，并增加 col_skip 逻辑来跳过屏幕外的源列。这种「底层负责裁剪」的设计思路和 SerenityOS 的 Painter::blit() 以及 Linux DRM 的 plane composition 一致。

## 与 SerenityOS Painter 和 Linux DRM 的对比

SerenityOS 的 Painter 类是一个完整的 2D 渲染上下文，提供了比 Cinux Canvas 丰富得多的功能。它的 draw_line 支持 Wu's 抗锯齿算法，输出的线条边缘平滑无锯齿。它的 blit 支持多种混合模式（直接覆盖、alpha 混合、带透明度覆盖）。它还有 clipping rectangle——可以限制绘制只在某个矩形区域内生效。Cinux 的 Canvas 是 Painter 的极简版本：无抗锯齿、无 alpha 混合、无 clipping。这些简化是合理的——我们不需要 Photoshop 级别的渲染质量，只需要画窗口边框和图标。

从算法层面看，Bresenham 直线绘制几乎是所有 2D 图形库的起点。SerenityOS 在此基础上添加了 Wu's 抗锯齿——它在直线两侧各画一个「半亮」像素，通过计算理论位置和实际像素的距离来确定半亮像素的亮度。这需要浮点运算，但 x86_64 有 SSE 指令可以高效完成。Cinux 选择纯整数 Bresenham 是因为内核环境通常避免浮点——虽然 x86_64 允许内核使用 SSE，但需要保存/恢复 FPU 状态（kernel_fpu_begin/end），增加中断延迟。对于一个教学内核来说，锯齿线条完全可以接受。

Linux 的 DRM/KMS 子系统在内核层面实现了类似 blit 的操作。DRM 的 plane composition 就是把多个 framebuffer（代表不同窗口或图层）合成到 CRTC 的输出 framebuffer 上。硬件加速的 GPU 可以在显存中直接完成这个合成（零拷贝），而 Cinux 的 blit 是纯 CPU 逐像素拷贝。对于教学内核来说 CPU 拷贝完全够用，但这也是为什么生产级操作系统需要 GPU 驱动——纯 CPU 合成在高分辨率多窗口场景下会成为性能瓶颈。

色彩键透明（0x00000000 = 透明）是我们为 draw_bitmap 选择的最简单方案。Linux DRM 使用真正的 alpha 通道——每个像素有 8 位 alpha 值表示透明度，从 0（完全透明）到 255（完全不透明）。这允许半透明效果（alpha = 128 表示 50% 透明），但每次写入需要做混合运算。SerenityOS 的 Painter::blit_with_opacity 进一步支持整体透明度参数——可以对整个源位图应用一个 0-255 的透明度系数。这些都是 Cinux 的 draw_bitmap 没有实现的，但架构上预留了扩展空间——只需要在写入时把直接赋值改为混合计算。

## 参考资料

- Wikipedia: [Bresenham's Line Algorithm](https://en.wikipedia.org/wiki/Bresenham%27s_line_algorithm) — 算法原理和推导过程
- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/VBE) — fillrect 优化和位图渲染
- SerenityOS: [LibGfx Painter](https://github.com/SerenityOS/serenity) — Production-grade 2D rendering context
- Linux: [DRM/KMS](https://www.kernel.org/doc/html/v5.3/gpu/drm-internals.html) — kernel mode setting and plane composition

## 下一步

图元绘制能力已经完整——直线、矩形、位图、blit 全部就位。但画面上还没有文字，而且我们的双缓冲还是「手动 flip」模式。下一章我们来搞定最后两块拼图：PSF2 位图字体渲染让 Canvas 能显示文字，PIT 定时回调让 GUI 画面自动刷新。同时还要处理 Canvas 引入的一个大坑——3MB 的 back buffer 分配暴露了堆扩展和物理内存映射的深层问题。
