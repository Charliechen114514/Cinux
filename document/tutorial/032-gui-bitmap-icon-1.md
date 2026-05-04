# 032-1 从零开始给内核加位图渲染

> 标签：Cinux, GUI, 位图渲染, 透明度, Canvas, framebuffer
> 前置：[031-gui-native-app](031-gui-native-app-3.md) — 窗口管理器与原生应用

## 前言

说实话，做到 031 tag 的时候，我们的桌面已经有窗口了——能拖动、能关闭、有标题栏、里面还能跑应用。但如果你仔细看一眼桌面，除了那几个灰蒙蒙的窗口之外，整个桌面空空荡荡的，什么都没有。真正的桌面系统，打开第一眼看到的是图标——终端、文件管理器、回收站——这些东西定义了桌面的"身份"。没有图标的桌面就像搬进了新房子但还没摆家具，虽然能住但总觉得少了点什么。

这一步的关键问题在于：窗口系统用 draw_rect 画色块就行，但图标不一样，图标是有形状的——圆角、不规则边缘、半透明过渡。我们需要一种新的渲染原语：不是画矩形，不是画线，而是画一张"小图片"。这就是 draw_bitmap 的由来。

当然，做位图渲染这件事本身并不难——遍历像素数组，一个一个写到 framebuffer 上，小学生都能想出来。真正有意思的设计决策在透明度处理上：是搞完整的 alpha blending（每像素 8-bit alpha 通道，Porter-Duff 混合公式），还是简单粗暴地约定一个"透明色"？我们选了后者——用一个特定的颜色值 `0x00000000` 代表"这个像素不画"，遇到它就跳过。这是早期 Windows 位图的经典做法，叫做 color-key transparency。

## 环境说明

我们的开发环境和之前的 tag 完全一致。工具链是 GCC 14 + CMake，目标平台是 x86_64 QEMU。图形输出通过 VBE（VESA BIOS Extensions）设置的线性帧缓冲区，分辨率和色深由 Bootloader 在实模式时配置。所有 GUI 代码通过 `CINUX_GUI` 宏控制编译——关闭这个宏时 GUI 子系统完全不参与编译。

## 第一步——分析 Canvas 已有的能力

我们先回头看一眼 Canvas 类已经能做什么：draw_pixel 画单点、draw_rect 画填充矩形、draw_rect_outline 画矩形边框、draw_line 画直线、draw_text 画文字、blit 做 Canvas 之间的区域复制、clear 清屏、flip 把后端缓冲区刷到 framebuffer。这些操作有一个共同特点——它们操作的都是"几何图形"（点、线、矩形）或者"另一个 Canvas 的内容"。没有任何一个方法接受"裸的像素数组"作为输入。

这就是 draw_bitmap 要填补的空缺。它的定位非常明确：给定一个像素数组（raw pixel data），把它原样画到 Canvas 的指定位置。和 blit 的区别是，blit 的源是另一个 Canvas（有自己的 stride、自己的缓冲区），而 draw_bitmap 的源是一个紧凑排列的像素数组（行紧密排列，没有 stride padding）。

## 第二步——draw_bitmap 的实现

draw_bitmap 的实现只有不到 30 行代码，但每一行都有讲究。

首先是防御性检查——如果 Canvas 没有初始化（back_buf_ 为空）或者像素指针为空，直接返回。这和 Canvas 其他方法的行为一致。

然后是两层循环遍历位图。外层循环遍历行，内层循环遍历列。每一行的第一步是检查是否超出 Canvas 底部——如果是就 break 结束整个渲染。每一列的第一步是检查是否超出 Canvas 右侧——如果是就 break 结束当前行的渲染。这两个 break 构成了完整的四边裁剪，覆盖了位图部分超出或完全超出 Canvas 的所有情况。

接下来从像素数组读取颜色值。索引计算是 `row * w + col`——用的是位图的宽度 w，不是 Canvas 的 pitch。这是因为像素数组是紧凑存储的，每行恰好 w 个 uint32_t，没有填充。

最后是透明判断。如果颜色值等于 `0x00000000`，continue 跳过——不写后端缓冲区，保留该位置的已有内容。否则把颜色值写入后端缓冲区的对应位置，索引用 `(y + row) * pixels_per_row + (x + col)` 计算得出。

整个逻辑可以用一句话概括："逐像素拷贝，碰到透明就跳，碰到边界就停。"

## 和 SerenityOS 的对比

说到位图渲染，不得不提 SerenityOS。Andreas Kling 的这个项目是当代教育 OS 中 GUI 最完善的一个。SerenityOS 的 `Painter` 类（在 `Userland/Libraries/LibGfx/Painter.cpp` 中，超过 2500 行）有一个 `blit_filtered()` 方法，它的核心循环是这样的：遍历源像素，如果 `source_color.alpha() == 0` 就 continue 跳过，否则做颜色过滤或直接写入。

发现没有？跳过透明像素的逻辑和我们的 draw_bitmap 一模一样——都是"alpha 为零就 continue"。区别在于 SerenityOS 的 alpha 是真正的 8-bit alpha 通道（0-255 的范围），而我们用的是 color-key（要么零要么不零，没有中间状态）。SerenityOS 还支持 `blit_with_opacity()`，用模板参数 `BlitState::AlphaState` 在编译时选择不同的混合路径——有 SrcAlpha、DstAlpha、BothAlpha、NoAlpha 四种模式。而我们不需要这些，因为我们的图标只有"透明"和"不透明"两种状态。

从设计哲学上看，Cinux 的做法是"够用就好"。桌面图标不需要半透明效果——像素要么完全可见要么完全透明。color-key 方案用一行 `if (color == 0x00000000) continue` 就解决了问题，代码量是 SerenityOS 的百分之一。如果将来需要 alpha blending，扩展也很简单：把 32 位像素的高 8 位用作 alpha 通道，在透明判断处加一个 `else if (alpha == 0xFF)` 的快速路径，剩下的才做混合运算。

## 第三步——为什么 pitch 这么重要

如果你仔细看 draw_bitmap 的实现，会发现写入后端缓冲区时用的是 `pixels_per_row = pitch_ / 4` 而不是 `width_`。这不是多此一举。

在 VBE 图形模式中，framebuffer 的每一行可能有额外的填充字节——硬件出于对齐的原因，要求每行的字节数（pitch）不一定是 width * 4 的精确倍数。比如 1024x768 的 32-bit 模式，pitch 可能是 4096 字节而不是 1024 * 4 = 4096 字节（这个例子恰好相等），但也可能是 4128 字节——多出来的 32 字节是硬件要求的行对齐。如果用 `width_` 代替 `pitch_ / 4` 来计算行偏移，第二行开始的像素就会错位，画面会越画越歪。

OSDev Wiki 的 [Drawing In a Linear Framebuffer](https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer) 页面特别强调了这一点——像素位置的正确计算公式是 `pixel = vram + y * pitch + x * pixelwidth`，而不是 `y * width + x`。我们整个 Canvas 类从头到尾都在用 pitch，draw_bitmap 也不例外。

## 收尾

到这里 draw_bitmap 就完成了。看起来很简单对吧？20 多行代码，做了三件事：逐像素拷贝、透明跳过、边界裁剪。但这个简单的方法是后续所有图标功能的基础——没有它，再精巧的图标数据也无法渲染到屏幕上。

下一章我们会看到更有意思的部分：如何用 C++ 的 constexpr 模板在编译时把人类可读的 hex 字符串转换成 32x32 的像素数组，把图标数据直接编译进内核二进制——零文件系统依赖，零运行时解码开销。

## 参考资料

- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer) — 像素定位公式、pitch 说明、双缓冲
- SerenityOS: [Painter.cpp](https://github.com/SerenityOS/serenity/blob/master/Userland/Libraries/LibGfx/Painter.cpp) — `blit_filtered()` 的透明像素跳过逻辑
- SerenityOS Issue [#69](https://github.com/SerenityOS/serenity/issues/69) — SIMD 优化 alpha blending 的讨论
