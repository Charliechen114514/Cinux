# 从零构建内核 2D 画布 — 双缓冲与像素操作

> 标签：canvas, double buffering, framebuffer, pitch, 2D graphics
> 前置：[028e init 线程重构](028e-activate-init-thread-3.md)

## 前言

如果你一直跟着 Cinux 走到这里，我们终于要碰一件每个 OS 开发者都梦寐以求的事情了——在屏幕上画出真正的图形。不是 VGA 文字模式那种 80x25 的字符格子，而是像素级别的、可以画任意形状和颜色的图形。说实话，当我们第一次在 QEMU 窗口里看到彩色的矩形和 "Cinux GUI" 文字时，那种成就感真的不亚于第一次看到内核成功启动。

但画面撕裂这个老朋友很快就会出现。如果我们直接往硬件 framebuffer 上画——画一条线还没画完，显示控制器已经把半截画面扫出去了——用户看到的就是上半个屏幕是新内容、下半个是旧内容的撕裂画面。解决这个问题的标准答案就是双缓冲：先在一个离屏缓冲区上画完一整帧，然后一次性拷贝到硬件 framebuffer。这也是 Linux fbdev、SerenityOS LibGfx、乃至 Windows GDI 都采用的基本方案。

本章我们要从 Framebuffer 接口出发，构建一个完整的双缓冲 Canvas 类。它提供像素绘制、矩形填充和缓冲区翻转三个核心能力——这是整个 GUI 子系统的第一块砖。

## 环境说明

我们运行在 x86_64 QEMU 环境下，使用 VBE 设置的 1024x768x32bpp 线性 framebuffer 模式。Bootloader 通过 BootInfo 结构传递 framebuffer 的物理地址、宽高、pitch 和 bpp 给内核。内核用 `CINUX_GUI` CMake 选项控制 GUI 代码的编译——开启时会编译 Canvas、Font 等模块，关闭时走传统的 Console 文字模式路径。工具链是 GCC 14 + CMake，内核 freestanding（无标准库）。

## 第一步——理解 Framebuffer 的 pitch 概念

在动手写 Canvas 之前，我们需要先搞清楚一个很容易被忽略但一旦出错就非常坑的概念：pitch。

pitch 是「每条扫描线占用的字节数」。直觉上，1024x768 的 32 位色模式每行应该是 1024 * 4 = 4096 字节，pitch 等于 width * bytes_per_pixel。但在某些 VBE 实现中，硬件出于对齐或性能原因会在每行末尾加 padding，导致 pitch > width * 4。如果你用 width 来计算像素偏移，画面就会出现诡异的「倾斜」——每一行都比上一行偏移了一点。

我们来看 Framebuffer 接口新增的 `data()` 方法，它暴露了原始 MMIO 指针给 Canvas 使用：

```cpp
// kernel/drivers/video/framebuffer.hpp
volatile uint32_t* data() const { return addr_; }
```

就这么一行代码，但有两个要点。返回类型是 `volatile uint32_t*`——volatile 告诉编译器「这个内存地址是设备 I/O，不要对写入做任何优化」。如果不用 volatile，`flip()` 中的批量拷贝可能被编译器优化掉部分写操作（它认为「你写了同样的地址两次，第二次覆盖了第一次，所以第一次是冗余的」），导致画面不完整。其次是这个方法让 Canvas 绕过了 Framebuffer 的 put_pixel 逐像素接口，可以批量操作整行数据。

## 第二步——Canvas 类的整体设计

现在我们来看看 Canvas 类的核心结构。它的设计哲学是「一个 Canvas 就是一个可绘制的像素平面」，可以绑定到硬件 framebuffer（屏幕 Canvas），也可以独立存在（离屏 Canvas，用于窗口内容区）：

```cpp
class Canvas {
public:
    void init(Framebuffer& fb);      // 屏幕 Canvas
    void init(uint32_t w, uint32_t h); // 离屏 Canvas

    void draw_pixel(uint32_t x, uint32_t y, uint32_t color);
    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
    void clear(uint32_t color = 0);
    void flip();

    // ... 其他方法

private:
    Framebuffer* front_buf_ = nullptr;  // 可选的硬件 framebuffer
    uint32_t*    back_buf_  = nullptr;  // 离屏后备缓冲区
    uint32_t     width_     = 0;
    uint32_t     height_    = 0;
    uint32_t     pitch_     = 0;
};
```

front_buf_ 指向硬件 Framebuffer 对象（可以为 null——离屏 Canvas）。back_buf_ 是在内核堆上分配的 uint32_t 数组，所有绘制操作写这个数组。pitch_ 从 Framebuffer 获取或手动设置为 w*4（离屏模式没有硬件对齐）。

两个 init 的区别在于：屏幕 Canvas 的 pitch 从 Framebuffer 继承（可能有 padding），分配大小是 `(pitch/4) * height`；离屏 Canvas 的 pitch 等于 width*4（无 padding），分配大小是 `width * height`。这个区分非常重要——如果屏幕 Canvas 用了 width 而不是 pitch，后续所有行偏移计算都会和 hardware framebuffer 不一致，flip 时画面会错位。

## 第三步——init 和 draw_pixel 的实现

```cpp
void Canvas::init(Framebuffer& fb) {
    if (back_buf_ != nullptr) { delete[] back_buf_; back_buf_ = nullptr; }
    front_buf_ = &fb;
    width_     = fb.width();
    height_    = fb.height();
    pitch_     = fb.pitch();

    uint32_t stride       = pitch_ / 4;
    uint32_t total_pixels = stride * height_;
    back_buf_             = new uint32_t[total_pixels];
    memfill32(back_buf_, 0, total_pixels);
}
```

开头的 delete 检查处理了重复 init 的情况——如果 Canvas 被多次初始化，先释放旧的 back buffer。然后从 Framebuffer 获取维度，计算 stride（每行的 uint32_t 数量），分配并清零。对于 1024x768 的屏幕，这会分配大约 3MB 的内存——这个大小直接引发了后面要讲的堆安全修复。

draw_pixel 的实现很直接——边界检查加索引计算：

```cpp
void Canvas::draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width_ || y >= height_) return;
    uint32_t pixels_per_row           = pitch_ / 4;
    back_buf_[y * pixels_per_row + x] = color;
}
```

边界检查用 `>=` 不是 `>`，因为有效范围是 [0, width) 和 [0, height)。索引计算的核心是 `pitch_ / 4` 而不是 `width_`——这是 pitch-aware 索引的关键。

## 第四步——draw_rect 和 clear 的批量操作

矩形填充不通过 draw_pixel，而是直接写入 back buffer 来获得更好的性能：

```cpp
void Canvas::draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t pixels_per_row = pitch_ / 4;
    for (uint32_t row = y; row < y + h && row < height_; row++) {
        for (uint32_t col = x; col < x + w && col < width_; col++) {
            back_buf_[row * pixels_per_row + col] = color;
        }
    }
}
```

双重循环中 `row < y + h && row < height_` 同时做了两个检查——矩形内部边界和 Canvas 边界。对于 100x50 的矩形就是 5000 次直接数组写入，比 5000 次 draw_pixel 函数调用省去了函数调用开销和重复的 pitch 计算。

clear 用 memfill32 一次性填充：

```cpp
void Canvas::clear(uint32_t color) {
    if (back_buf_ == nullptr) return;
    memfill32(back_buf_, color, width_ * height_);
}
```

注意 clear 用 `width_ * height_` 不是 `(pitch_/4) * height_`。这是因为 clear 的语义是「把所有像素设为同一颜色」，pitch 的 padding 区域不需要清除（flip 也不会拷贝 padding）。

## 第五步——flip 双缓冲刷新

flip 是整个双缓冲方案的最后一环——把 back buffer 拷贝到 front buffer：

```cpp
void Canvas::flip() {
    if (front_buf_ == nullptr || back_buf_ == nullptr) return;
    auto* dst = reinterpret_cast<volatile uint8_t*>(front_buf_->data());
    auto* src = reinterpret_cast<const uint8_t*>(back_buf_);
    for (uint32_t row = 0; row < height_; row++) {
        memcopy(dst + static_cast<uintptr_t>(row) * pitch_,
                src + static_cast<uintptr_t>(row) * pitch_, width_ * 4);
    }
}
```

逐行拷贝——每行拷贝 `width_ * 4` 字节（不是 pitch 字节），行偏移用 pitch 计算。为什么只拷贝 width*4 而不是 pitch？因为 pitch 包含了硬件添加的 padding 字节，那些位置可能有硬件状态信息，我们不应该覆盖。front_buf_ 为 nullptr 时直接返回——离屏 Canvas 的 flip 是空操作。

这里用到了我们的自定义 memcopy 函数，它的目标参数是 `volatile void*`。普通的 libc memcpy 不接受 volatile 指针，所以我们需要自己实现一个字节级的拷贝循环。虽然字节级拷贝听起来效率不高，但对于 3MB 的数据量来说（1024 * 768 * 4 字节），在现代 x86_64 CPU 上大约需要 1-2 毫秒——以 100 Hz 的刷新频率来说完全在预算之内。

一个值得注意的细节是 `static_cast<uintptr_t>(row) * pitch_` 中的 `uintptr_t` 转换。row 是 uint32_t，pitch_ 也是 uint32_t，在 1024x768 的配置下 row * pitch 最大约为 768 * 4096 = 3,145,728，远在 uint32_t 范围内。但在更高分辨率（比如 4K）下 row * pitch 可能接近或超过 uint32_t 的上限，所以用 uintptr_t（64 位）来确保安全。

## 第六步——内核中的测试策略

Canvas 的测试采用了两层策略。内核态测试（`kernel/test/test_canvas.cpp`）在 QEMU 中运行，验证 Canvas 和真实 Framebuffer 的交互。主机端测试（`test/unit/test_canvas.cpp`）在 Linux 上编译运行，用 mock 对象模拟 Framebuffer 和 PSFFont，测试纯粹的算法逻辑。

内核态测试的关键是「写入-刷新-读回」验证模式：在 Canvas 上画一个已知颜色的矩形，flip 后通过 Framebuffer 的 get_pixel() 读取硬件 framebuffer 的对应位置。如果读回的值和写入的值一致，说明整个 pipeline（draw → back buffer → flip → front buffer）都工作正常。测试还区分了 CLI 模式和 GUI 模式——CLI 模式只验证 Framebuffer 基本功能，GUI 模式额外验证 Canvas 的所有绘制方法。

主机端测试的优势在于速度——编译和运行都是秒级的，不需要启动 QEMU。MockFramebuffer 用 std::vector 分配像素数组，MockPSFFont 允许手动设置字形位图。MockCanvas 复制了内核 Canvas 的全部算法逻辑。覆盖范围包括：像素读写、矩形填充、矩形描边、清屏、直线、文字渲染、blit、位图渲染——每一个方法都有对应的测试用例。

## 与 Linux fbdev 和 SerenityOS 的设计对比

Linux 的 fbdev 子系统采用非常相似的双缓冲方案。用户程序通过 `mmap()` 映射 `/dev/fb0` 获取前缓冲区，自己 `malloc` 后缓冲区。画完后有两个选择：用 `memcpy` 把整个后缓冲区拷贝到前缓冲区，或者用 `FBIOPAN_DISPLAY` ioctl 让硬件切换扫描地址（page flip）。后者更高效——不需要拷贝数据，只需要告诉显示控制器「从新地址开始扫描」——但需要硬件支持和双缓冲区对齐。

Cinux 目前用的是 memcpy 方案，和 Linux fbdev 的软件双缓冲等价。对于一个教学内核来说这完全够用——3MB 的拷贝在现代 CPU 上只需要不到 1ms。但如果后续想做高帧率的图形应用，可以考虑实现类似 `FBIOPAN_DISPLAY` 的 page flip 机制（需要双倍帧缓冲区物理内存）。

从数据结构的角度看，Linux fbdev 的 `fb_info` 结构体包含了帧缓冲区的物理地址、虚拟地址、尺寸、pitch 和像素格式——这和我们的 BootInfo + Framebuffer 类的组合是对等的。Linux 的 fbdev 驱动需要自己注册 `fb_info`，而 Cinux 的 Framebuffer 直接从 BootInfo 初始化。这种差异源于启动方式的不同——Linux 用 GRUB/UEFI 启动后由驱动自己设置显示模式，Cinux 的 bootloader 在 real mode 下用 INT 10h 设好模式后直接传给内核。

SerenityOS 的做法更进了一步。它的 LibGfx 库把像素缓冲区抽象为 Bitmap 类（支持多种像素格式如 RGBA8888、RGB888），渲染操作封装在 Painter 类中（类似 Qt 的 QPainter）。Painter 支持 clipping rectangle（只允许在指定区域内绘制）、opacity（半透明绘制）、alpha blending（源像素和目标像素的混合计算）。这些特性让 SerenityOS 的 GUI 看起来更现代——窗口可以有圆角、阴影、半透明效果。

Cinux 的 Canvas 本质上是 Bitmap + Painter 的简化版——固定像素格式（0x00RRGGBB）、无 clipping、无 alpha、直接覆盖写入。这是合理的简化——我们不需要 Photoshop 级别的渲染质量，只需要画窗口边框和图标。从架构上说，Cinux 的 Canvas 在未来可以逐步添加这些特性：clipping rectangle 只需要在 draw_pixel 中加一个矩形范围检查，alpha blending 只需要在写入时做一次 `(src * alpha + dst * (255-alpha)) / 255` 的混合计算。这些都是增量式的改进，不需要推翻现有的设计。

## 参考资料

- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer) — 像素寻址、fillrect 优化、双缓冲原理
- OSDev Wiki: [VESA BIOS Extensions](https://wiki.osdev.org/VESA_BIOS_Extensions) — linear framebuffer、pitch 概念
- Linux: [DRM Internals](https://www.kernel.org/doc/html/v5.3/gpu/drm-internals.html) — 现代 Linux 图形栈架构
- SerenityOS: [LibGfx Painter](https://github.com/SerenityOS/serenity) — Production-grade 2D graphics library

## 第七步——在 kernel_main 中集成 Canvas

Canvas 的初始化被插入到 kernel_main 的已有初始化序列中。顺序很重要——Framebuffer 必须先初始化（Canvas 依赖它的维度信息），Font 必须先加载（Canvas 的 draw_text 需要字形数据），Console 必须先初始化（kprintf 需要工作），然后才能创建和初始化 Canvas。在我们的代码中，这些步骤被编号为 13（Framebuffer）→ 14（Font）→ 15（Console）→ 15b（Canvas + GUI init）。

```cpp
// kernel/main.cpp — 步骤 15b
#ifdef CINUX_GUI
    static cinux::drivers::Canvas g_canvas;
    g_canvas.init(fb);
    cinux::gui::gui_init(g_canvas, font);
#endif
```

Canvas 声明为 static 局部变量——这保证了它的生命周期覆盖整个内核运行期间（不会被提前销毁），同时不会污染全局命名空间。gui_init 负责初始化窗口管理器、渲染 demo 画面。在中断使能之后，init 线程中会调用 gui_start 注册 PIT tick callback，开始定期刷新。

GUI 模式下还需要额外 unmask IRQ12（PS/2 鼠标中断）——因为 GUI 需要鼠标输入来驱动窗口拖动和点击。这只需要一行 `PIC::unmask(12)`，放在 IRQ0 和 IRQ1 unmask 之后。

## 下一步

到这里 Canvas 的基础能力——init、draw_pixel、draw_rect、clear、flip——都已经就位了。但这些只能画矩形块，还不能画线条和文字。下一章我们来实现 Bresenham 直线算法、矩形描边、像素位图和画布间 blit 操作——这些是构建窗口边框、图标渲染和窗口合成的核心能力。
