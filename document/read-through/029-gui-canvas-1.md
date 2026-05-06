# Framebuffer 接口扩展与 Canvas 初始化

> 标签：framebuffer, canvas, double buffering, pitch, init
> 前置：[028e init 线程](028e-activate-init-thread-3.md)

## 概览

本文是 029_gui_canvas Read-through 系列的第一篇，聚焦于 Framebuffer 接口扩展和 Canvas 类的核心初始化逻辑。我们要拆解的代码包括：`framebuffer.hpp` 新增的 `data()` 访问器、`canvas.hpp` 的类声明、以及 `canvas.cpp` 中的 `init()`、`draw_pixel()`、`draw_rect()`、`clear()` 和 `flip()` 方法。

关键设计决策一览：
- Canvas 持有两个缓冲区指针——front（硬件 MMIO）和 back（堆分配）
- pitch-based 索引而非 width-based，正确处理 VBE 扫描线对齐
- 自定义 memcopy/memfill32 替代 libc（内核 freestanding 环境）
- 支持「有 front」和「无 front」两种初始化模式

## 架构图

```
kernel_main()
  │
  ├── Framebuffer fb
  │     fb.init(*boot_info)
  │     └── 物理地址映射 → MMIO volatile uint32_t*
  │
  ├── Canvas canvas
  │     canvas.init(fb)
  │     ├── front_buf_ = fb.data()         (volatile MMIO)
  │     └── back_buf_ = new uint32_t[...]   (kernel heap)
  │
  └── 绘制循环:
        canvas.clear(0x001A1A2E)   → 写 back_buf_
        canvas.draw_rect(...)       → 写 back_buf_
        canvas.draw_text(...)       → 写 back_buf_
        canvas.flip()               → back_buf_ memcpy→ front_buf_
```

## 代码精讲

### Framebuffer::data() 访问器

Canvas 的 `flip()` 需要批量访问 framebuffer 的原始内存。之前 Framebuffer 只暴露了 `put_pixel()` 这种逐像素接口，太慢了。我们加了一个 `data()` 返回 volatile 指针：

```cpp
// kernel/drivers/video/framebuffer.hpp
volatile uint32_t* data() const { return addr_; }
```

就这么简单——返回内部 `addr_` 成员。返回类型是 `volatile uint32_t*` 而不是 `uint32_t*`，因为 framebuffer 是 MMIO 设备内存，编译器不能对写入做优化（比如合并连续写入或消除「冗余」写入）。如果不用 volatile，`flip()` 中的批量拷贝可能被编译器优化掉部分写操作，导致画面不完整。

### Canvas 类声明

Canvas 的头文件定义了整个 2D 画布的接口。我们先看成员变量：

```cpp
// kernel/drivers/canvas.hpp
private:
    Framebuffer* front_buf_ = nullptr;   // 硬件 framebuffer（可为 null）
    uint32_t*    back_buf_  = nullptr;   // 后备缓冲区（堆分配）
    uint32_t     width_     = 0;
    uint32_t     height_    = 0;
    uint32_t     pitch_     = 0;         // bytes per scan line
```

四个字段各司其职。front_buf_ 是可选的——离屏 Canvas（比如窗口内容区）不需要 front。back_buf_ 是所有绘制操作的目标。pitch_ 是字节为单位的扫描线步进，和 width_ 独立是因为 VBE 模式下两者可能不同。

Canvas 禁用了拷贝构造和拷贝赋值，因为内部有 `new[]` 分配的缓冲区，浅拷贝会导致 double free。析构函数负责释放 back_buf_：

```cpp
~Canvas() {
    if (back_buf_ != nullptr) {
        delete[] back_buf_;
        back_buf_ = nullptr;
    }
}
```

### 内部辅助函数

canvas.cpp 开头定义了两个匿名命名空间的辅助函数，用来替代 libc：

```cpp
namespace {
void memcopy(volatile void* dst, const void* src, uint32_t size) {
    auto*       d = static_cast<volatile uint8_t*>(dst);
    const auto* s = static_cast<const uint8_t*>(src);
    for (uint32_t i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

void memfill32(uint32_t* dst, uint32_t value, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        dst[i] = value;
    }
}
}  // anonymous namespace
```

memcopy 的目标参数是 `volatile void*`——这是关键。普通 memcpy 不接受 volatile 目标，而我们的 front buffer 是 MMIO，必须用 volatile 指针访问。memfill32 按整个 uint32_t 填充，比逐字节填充快 4 倍，用于 clear() 和 init() 时的批量清零。

### Canvas::init(Framebuffer&)

从硬件 framebuffer 初始化 Canvas。这是最常用的初始化路径：

```cpp
void Canvas::init(Framebuffer& fb) {
    front_buf_ = &fb;
    width_     = fb.width();
    height_    = fb.height();
    pitch_     = fb.pitch();

    // Allocate back buffer: width * height * 4 bytes (32-bit pixels)
    uint32_t total_pixels = width_ * height_;
    back_buf_ = new uint32_t[total_pixels];

    // Clear back buffer to black
    memfill32(back_buf_, 0, total_pixels);
}
```

从 Framebuffer 获取维度信息，分配 `width_ * height_` 个 uint32_t 的 back buffer 并清零为黑色。注意分配大小是 `width_ * height_`——虽然 draw_pixel 和 draw_rect 使用 `pitch_ / 4` 作为行步进，但 back buffer 的分配在 tag 029 中并未包含 stride 乘数，pitch 只在像素索引计算和 flip 时使用。

### 离屏 Canvas（前瞻）

> **注意**：tag 029 的 Canvas 只实现了 `init(Framebuffer&)` 一种初始化方式。离屏 Canvas 的 `init(uint32_t w, uint32_t h)` 重载将在后续 tag（030 窗口管理器）中添加，届时窗口管理器需要为每个窗口创建独立的离屏渲染面。目前所有绘制都通过屏幕 Canvas 完成。

### Canvas::draw_pixel()

单个像素写入，带边界检查：

```cpp
void Canvas::draw_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= width_ || y >= height_)
        return;
    uint32_t pixels_per_row           = pitch_ / 4;
    back_buf_[y * pixels_per_row + x] = color;
}
```

边界检查用 `>=` 而不是 `>` ——坐标范围是 [0, width) 和 [0, height)。像素索引计算使用 pitch/4 作为行步进。这个方法会被 draw_rect_outline、draw_line、draw_text 等高级图元大量调用。

### Canvas::draw_rect()

实心矩形填充——不通过 draw_pixel，直接写入 back buffer：

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

双重循环同时检查了 `row < y + h`（矩形内部边界）和 `row < height_`（Canvas 边界），避免越界。直接操作 back_buf_ 比逐像素调用 draw_pixel 省去了重复的边界检查和函数调用开销。对于一个 100x50 的矩形，就是 5000 次直接写入，比 5000 次 draw_pixel 函数调用快得多。

### Canvas::clear() 和 Canvas::flip()

```cpp
void Canvas::clear(uint32_t color) {
    if (back_buf_ == nullptr) return;
    uint32_t total_pixels = width_ * height_;
    memfill32(back_buf_, color, total_pixels);
}
```

clear 用 memfill32 一次性填充整个 back buffer。注意这里用 `width_ * height_` 而不是 `(pitch_/4) * height_`——因为 clear 的语义是「填满所有像素」，而 pitch 的 padding 区域不需要清除（flip 时也不会拷贝 padding）。

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

flip 是双缓冲的核心。逐行拷贝：每行拷贝 `width_ * 4` 字节（不是 pitch 字节），行偏移用 pitch 计算（因为 dst 和 src 的行间距都是 pitch）。front_buf_ 为 nullptr 时直接返回——离屏 Canvas 不需要 flip。

## 设计决策

### 决策：back buffer 用 pitch 索引还是 width 索引

**问题**: back buffer 的分配大小和索引计算应该用 pitch 还是 width？

**本项目的做法**: 分配大小用 `(pitch/4) * height`（保证有足够空间容纳含 padding 的行），但 clear 用 `width * height`（不需要清除 padding 区域）。

**备选方案**: 直接用 `width * height` 分配，所有索引计算都用 width。

**为什么不选备选方案**: 在 pitch > width*4 的硬件上，draw_pixel 写入 `y * (width) + x` 实际上应该写入 `y * (pitch/4) + x` 才能和 flip 的行对齐。如果 back buffer 的行步进和 front buffer 不一致，flip 拷贝时数据会错位。

**如果要扩展**: 可以让 Canvas 在 pitch == width*4 时使用优化路径（直接 memcpy 整个 buffer 一次而不是逐行拷贝）。

## 扩展方向

- 使用 SIMD 指令（SSE2/AVX）加速 memfill32 和 memcopy，特别是 3MB 的 flip 拷贝（难度：中等）
- 添加 dirty rectangle tracking，flip 时只拷贝修改过的区域而不是整个屏幕（难度：中等）
- 支持 VSync 等待，在显示器的垂直消隐期间执行 flip，彻底消除撕裂（难度：较高）
- 实现离屏 Canvas 的 pitch 优化——当不需要和硬件对齐时用更紧凑的内存布局

## 参考资料

- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/VBE) — pixel addressing, fill rect optimization
- OSDev Wiki: [VESA BIOS Extensions](https://wiki.osdev.org/VBE) — linear framebuffer, pitch
- Linux: [DRM Internals](https://www.kernel.org/doc/html/v5.3/gpu/drm-internals.html) — modern framebuffer management
- SerenityOS: [LibGfx Painter](https://github.com/SerenityOS/serenity) — analogous 2D canvas abstraction
