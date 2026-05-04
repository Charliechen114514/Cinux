# 032-1 Read-through: Canvas::draw_bitmap 实现

## 概览

本篇是 tag 032_gui_bitmap_icon 的第一篇 read-through，聚焦于 Canvas 类新增的 `draw_bitmap()` 方法。整个 tag 的核心目标是给 Cinux 的 GUI 系统加入位图渲染能力——具体来说就是把一个像素数组画到 Canvas 的后端缓冲区上，同时处理透明像素和边界裁剪。draw_bitmap 是这个目标的最底层实现，后续的图标数据和桌面图标系统都建立在它之上。

关键设计决策一览：使用 `0x00000000` 作为透明色标记（color-key transparency 而非 alpha blending），裁剪通过简单的 break 语句实现（不做精确的矩形求交），方法的参数设计和现有 draw_rect / draw_pixel 保持风格一致。整个方法只有不到 30 行有效代码，但每一行都有值得讨论的设计考量。

## 架构图

```
调用者
  │
  │  draw_bitmap(x, y, w, h, pixels)
  ▼
Canvas::draw_bitmap()
  │
  ├── 防御检查: back_buf_ == nullptr || pixels == nullptr → return
  │
  ├── 计算 stride = pitch_ / 4
  │
  ├── 外层循环: row = 0 .. h-1
  │     │
  │     ├── 裁剪: y + row >= height_ → break (行越界)
  │     │
  │     └── 内层循环: col = 0 .. w-1
  │           │
  │           ├── 裁剪: x + col >= width_ → break (列越界)
  │           │
  │           ├── 读取: color = pixels[row * w + col]
  │           │
  │           ├── 透明: color == 0x00000000 → continue (跳过)
  │           │
  │           └── 写入: back_buf_[(y+row)*stride + (x+col)] = color
  │
  └── 完成 (无返回值)
```

## 代码精讲

### 头文件声明

在 `kernel/drivers/canvas.hpp` 中新增了 `draw_bitmap` 方法的声明和文档注释。方法的参数设计很直观：x 和 y 是目标位置（Canvas 坐标系），w 和 h 是位图的宽高，pixels 是指向像素数据开头的指针。注释中明确说明了像素格式是 `0x00RRGGBB`，`0x00000000` 被视为完全透明，并且位图会被裁剪到 Canvas 边界内。

```cpp
/**
 * @brief Draw a pixel bitmap with transparency onto the back buffer
 *
 * Renders a w x h bitmap from a pixel array.  Each pixel is a 32-bit
 * colour value in 0x00RRGGBB format.  Pixels with the value 0x00000000
 * are treated as fully transparent and skipped.  The bitmap is clipped
 * to the canvas bounds.
 *
 * @param x       Left edge on the canvas
 * @param y       Top edge on the canvas
 * @param w       Bitmap width in pixels
 * @param h       Bitmap height in pixels
 * @param pixels  Span of w*h pixel values (row-major, top-to-bottom)
 */
void draw_bitmap(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                 const uint32_t* pixels);
```

值得留意的是参数类型的选择。x 和 y 是 uint32_t（无符号），这意味着方法不支持把位图画到 Canvas 左边界或上边界以外——比如传入一个负值的 x 会被解释为一个很大的无符号数，裁剪逻辑会正确地跳过所有像素。这和 `blit()` 方法使用 int32_t 参数的做法不同，blit 需要支持窗口被拖到屏幕边缘外的场景（dst_x 可以为负），而 draw_bitmap 主要用于图标这类不会被拖动的元素，所以不需要负坐标支持。

### draw_bitmap 实现

实现在 `kernel/drivers/canvas.cpp` 中，整体逻辑非常清晰。我们按段落来看。

```cpp
void Canvas::draw_bitmap(uint32_t x, uint32_t y, uint32_t w,
                         uint32_t h, const uint32_t* pixels) {
    if (back_buf_ == nullptr || pixels == nullptr)
        return;
```

首先做防御性检查。如果后端缓冲区还没分配（Canvas 未初始化）或者像素指针为空，直接返回。这个检查和 Canvas 其他方法的防御逻辑一致——比如 `draw_pixel` 会检查坐标越界（`if (x >= width_ || y >= height_) return`），`clear` 会检查 `back_buf_` 是否为空（`if (back_buf_ == nullptr) return`）。这里的两个检查合并成一行 if 语句，用逻辑或连接，任何一个为真都会直接返回。

接下来计算行步长并进入主循环：

```cpp
    uint32_t pixels_per_row = pitch_ / 4;

    for (uint32_t row = 0; row < h; row++) {
        // Clip to canvas bounds (vertical)
        if (y + row >= height_)
            break;
```

`pixels_per_row` 的计算和 draw_rect / draw_pixel 中完全一样——用 pitch 除以 4 得到每行的 uint32_t 元素数，而不是简单地用 width_。pitch 是 VBE 模式返回的"每扫描线字节数"，它可能大于 width * 4（某些硬件实现会做行对齐填充）。用 pitch 来计算行偏移可以保证在任何 VBE 模式下都能正确工作。

外层循环遍历位图的每一行。裁剪逻辑用 break 实现：一旦当前行超出 Canvas 底部（`y + row >= height_`），就终止整个循环。这比先计算"有效行范围"再循环更简洁，但对于位图完全在 Canvas 下方的情况（第一个 row 的 y + row 就超过 height_），循环体一次也不会执行——这是正确的。

```cpp
        for (uint32_t col = 0; col < w; col++) {
            // Clip to canvas bounds (horizontal)
            if (x + col >= width_)
                break;

            // Read pixel from the source array
            uint32_t color = pixels[row * w + col];

            // 0x00000000 is treated as fully transparent
            if (color == 0x00000000)
                continue;

            back_buf_[(y + row) * pixels_per_row + (x + col)] = color;
        }
    }
}
```

内层循环逐列处理。列方向同样用 break 裁剪——一旦列坐标超出 Canvas 右边界就结束当前行的处理。

像素读取的索引是 `row * w + col`——这里用的是位图的宽度 w 而不是 Canvas 的 stride。这是因为像素数组是紧凑存储的——每行恰好 w 个 uint32_t 元素，行与行之间没有填充字节。

透明判断 `color == 0x00000000` 在写入之前。如果像素是透明的，continue 跳过——后端缓冲区该位置保持不变，保留之前画上去的内容（比如桌面背景色或者另一个图标的像素）。这就是 color-key 透明的全部实现：一行判断，一个 continue。

写入的索引是 `(y + row) * pixels_per_row + (x + col)`——这里用的是 Canvas 的 stride（pixels_per_row），因为要写入的是后端缓冲区，后端缓冲区的行步长由 pitch 决定。注意源和目标使用了不同的行步长——源用 w，目标用 pixels_per_row。这是 draw_bitmap 和 draw_rect 的关键区别之一：draw_rect 填充的是单一颜色值，不需要从外部数组读取数据，所以不存在"源步长"和"目标步长"的差异问题。

### 和 blit 的详细对比

Canvas 已经有一个 `blit()` 方法用于 Canvas 之间的区域复制。让我们来详细比较两者。

blit 的源是另一个 Canvas 对象，有自己的 back_buf_、pitch_ 和宽高。blit 在读取源像素时需要用源 Canvas 的 stride（`src.pitch_ / 4`），写入目标时用目标 Canvas 的 stride。blit 还支持负的 dst_x / dst_y——当窗口被拖到屏幕左边或上边时，需要计算 col_skip 来跳过源数据中不可见的部分。draw_bitmap 的源是一个裸的像素数组，行紧密排列（stride 就是 w），不需要负坐标支持，所以裁剪逻辑更简单。

blit 做的是无条件拷贝——直接把源 Canvas 的像素覆盖到目标 Canvas 对应位置，不管颜色值是什么。draw_bitmap 做的是条件拷贝——透明像素被跳过，目标位置保留原有内容。这个差异导致了两者内部循环的核心区别：blit 的内层循环只有一行赋值语句（`dst[...] = src[...]`），draw_bitmap 的内层循环多了一个 if-continue 判断。

从调用场景来看，blit 主要被窗口管理器用来把窗口的内容区域合成到屏幕 Canvas 上——窗口的每个像素都需要被画出来，不存在透明的问题。draw_bitmap 主要被用来画桌面图标——图标有不规则的形状（圆角、镂空部分），需要透明来让桌面背景透出来。两个方法各司其职，虽然底层都是"从一个地方读像素、写到另一个地方"，但使用场景和处理逻辑有本质差异。

### 性能分析

对于一个 32x32 的图标（1024 个像素），draw_bitmap 的最坏情况是不需要裁剪、没有透明像素——此时执行 1024 次比较（边界检查）+ 1024 次比较（透明检查）+ 1024 次内存写入 = 3072 次操作。在 1024x768 的 32-bit framebuffer 上，这大约是 0.13% 的像素，耗时在微秒级别。

最常见的实际场景是大部分像素不透明、少量像素透明（图标四个角的透明区域）。此时透明像素的 continue 会导致一些分支预测失败，但对 CPU 来说代价极低。如果将来需要优化性能，可以考虑用 SIMD 指令批量处理像素——SerenityOS 的 issue #69 就在讨论用 SSE/AVX 指令加速 alpha blending。

## 设计决策

### 决策：透明机制——color-key vs alpha blending

**问题**: 位图渲染时如何处理透明像素？

**本项目的做法**: 使用 color-key transparency，约定 `0x00000000` 为透明色，遇到时跳过写入。

**备选方案**: 实现完整的 alpha blending（Porter-Duff source-over），每个像素有 8-bit alpha 通道，渲染时做 `result = src * alpha + dst * (255 - alpha)`。

**为什么不选备选方案**: Alpha blending 对每个像素需要 6 次乘法和 3 次加法（RGB 各一个通道），还要处理 alpha 为 0 和 255 的边界情况。虽然 1024 个像素的性能开销在现代 CPU 上微不足道，但代码复杂度会增加不少——需要处理半透明效果、预乘 alpha（premultiplied alpha）等问题。当前阶段我们的图标不需要半透明效果——像素要么完全可见要么完全透明。color-key 方案一行判断就能解决，清晰且够用。

**如果要扩展/改进**: 添加 alpha blending 支持不需要修改 draw_bitmap 的接口——可以在 32 位像素格式中启用高 8 位作为 alpha 通道（改为 `0xAARRGGBB`），然后在透明判断处改为三段式：`if (alpha == 0) continue; else if (alpha == 0xFF) 直接写; else 做 blending`。这样既保持了向后兼容（0x00000000 仍然是完全透明），又支持了半透明效果。

### 决策：裁剪方式——break vs 矩形求交

**问题**: 如何处理位图超出 Canvas 边界的情况？

**本项目的做法**: 在循环内部用 break 语句实现裁剪——越界时直接跳出循环。

**备选方案**: 先计算可见区域的矩形交集，然后只遍历交集范围内的像素。

**为什么不选备选方案**: 矩形求交需要计算 first_row、last_row、first_column、last_column 四个值，然后在循环中使用这些偏移量。这确实更高效（不需要在每次迭代中做边界检查），但代码更复杂。对于 32x32 的小位图，差异微乎其微。SerenityOS 的 Painter 用矩形求交方式（`rect.intersected(clip_rect())`），因为他们支持任意矩形裁剪区域和缩放，计算量更大，需要更精细的优化。

## 扩展方向

1. **位图缩放** — 支持将位图缩放到指定尺寸渲染，使用最近邻插值或双线性插值
2. **alpha blending** — 启用高 8 位 alpha 通道，支持半透明混合
3. **RLE 压缩位图** — 对透明像素密集的位图使用行程编码（Run-Length Encoding），跳过连续的透明像素段，减少循环次数
4. **SIMD 优化** — 使用 SSE/AVX 指令并行处理多个像素的写入和透明判断
5. **脏矩形跟踪** — 只重绘发生变化的屏幕区域，减少不必要的全屏刷新

## 参考资料

- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer) — 像素定位公式 `vram + y * pitch + x * pixelwidth`、双缓冲策略、putpixel/fillrect 优化建议
- SerenityOS: [Painter.cpp](https://github.com/SerenityOS/serenity/blob/master/Userland/Libraries/LibGfx/Painter.cpp) — `blit_filtered()` 中 `alpha == 0` 跳过逻辑、`BlitState` 模板化 alpha 路径选择
- SerenityOS Issue: [#69 SIMD optimized alpha blending](https://github.com/SerenityOS/serenity/issues/69) — 使用 SSE/AVX 加速 alpha blending 的讨论
