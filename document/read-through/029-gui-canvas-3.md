# PSF2 字体渲染、PIT 回调与内存安全修复

> 标签：PSF2, draw_text, PIT callback, heap bounds, direct map
> 前置：[029 图元绘制](029-gui-canvas-2.md)

## 概览

本文是 029_gui_canvas Read-through 系列的第三篇，聚焦于文字渲染机制、PIT 定时回调、以及 Canvas 引入的内存安全问题修复。我们要拆解的代码包括：`PSFFont::glyph()` 方法、`Canvas::draw_text()`、PIT 的 callback 扩展、Heap 的 max_size_ 安全机制、以及 bootloader 的全量物理内存直接映射。

关键设计决策一览：
- PSF2 字形通过 glyph() 暴露位图指针，Canvas 负责逐像素渲染
- PIT 回调机制用函数指针实现，运行在中断上下文
- Heap 的 expand() 加 max_size_ 上限防止虚拟地址空间越界
- Bootloader 用 2MB/1GB 大页映射全部物理内存

## 架构图

```
文字渲染数据流:

  font_data.S (.incbin)
    └── font_psf_start[]
          │
    PSFFont::init()
    ├── 解析 PSF2 header
    └── glyphs_ = font_psf_start + header_size
          │
    PSFFont::glyph(c)
    └── return glyphs_ + c * bytes_per_glyph_
          │
    Canvas::draw_text()
    └── 逐字符 → 逐行 → 逐位 → draw_pixel()

PIT 回调流:

  IRQ0 → pit_irq0_handler()
    ├── tick_count_++
    ├── PIC::send_eoi(0)
    ├── invoke_tick_callback()     ← 新增
    │     └── tick_callback_(ctx)
    │           └── gui_tick_callback()
    │                 ├── 排空事件队列
    │                 ├── 窗口合成
    │                 └── flip()
    └── Scheduler::tick()

内存安全修复:

  Heap::expand()
    └── if (size_ + expand_size > max_size_) return false

  Bootloader phase2:
    └── 扫描 E820 → 映射全部物理内存 (2MB/1GB 大页)
```

## 代码精讲

### PSFFont::glyph() 访问器

PSFFont 已经在 init() 中解析了 PSF2 头部。glyph() 方法暴露了字形位图的原始指针，供 Canvas::draw_text() 使用：

```cpp
// kernel/drivers/video/font.cpp
const uint8_t* PSFFont::glyph(uint8_t c) const {
    if (glyphs_ == nullptr)
        return nullptr;
    if (c >= num_glyphs_)
        c = 0;
    return glyphs_ + static_cast<uint32_t>(c) * bytes_per_glyph_;
}
```

逻辑很简单：空指针检查（字体没初始化）、越界保护（超出字形数量则回退到第 0 个字形）、然后返回 `base + index * size`。返回值是 `const uint8_t*`，指向一行一行的位图数据——每字节代表字形的一行，最高位对应最左像素。

font.hpp 中还加了一个 bytes_per_glyph() 的 getter，虽然 draw_text 不直接使用它（因为渲染逻辑是逐位检查而不是逐字节拷贝），但在某些高级渲染场景中可能有用。

### Canvas::draw_text() — 文字渲染

这是 Canvas 中最复杂的绘制方法，因为它涉及字形位图的逐位解析：

```cpp
void Canvas::draw_text(uint32_t x, uint32_t y, const char* str,
                        uint32_t color, PSFFont& font) {
    if (back_buf_ == nullptr || str == nullptr) return;

    uint32_t glyph_w = font.width();
    uint32_t glyph_h = font.height();
    if (glyph_w == 0 || glyph_h == 0) return;

    uint32_t cursor_x = x;
    uint32_t cursor_y = y;

    for (uint32_t i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            cursor_x = x;
            cursor_y += glyph_h;
            continue;
        }

        const uint8_t* g = font.glyph(static_cast<uint8_t>(str[i]));
        if (g == nullptr) continue;

        for (uint32_t row = 0; row < glyph_h; row++) {
            uint8_t bits = g[row];
            for (uint32_t col = 0; col < glyph_w; col++) {
                if ((bits >> (7 - col)) & 1) {
                    draw_pixel(cursor_x + col, cursor_y + row, color);
                }
            }
        }
        cursor_x += glyph_w;
    }
}
```

这段代码的执行流程是一个三重循环：外层遍历字符串的每个字符，中层遍历字形位图的每一行，内层遍历每一位。

换行符处理：遇到 '\n' 时 cursor_x 回到起始位置 x（不是 0），cursor_y 向下移动一个字形高度。这实现了基本的文本排版。

位图解析：`g[row]` 取出字形第 row 行的 8 位数据。`(bits >> (7 - col)) & 1` 从最高位（MSB）开始检查每一位——bit 7 是最左边的像素，bit 0 是最右边的像素。如果位为 1，就在 Canvas 的对应位置画一个前景色像素。

注意 draw_text 只画前景像素——位为 0 的位置不写任何东西，保持了「透明」效果。这意味着文字会叠加在已有的背景图形之上。

### PIT tick 回调扩展

PIT 驱动新增了两个 static 成员和两个方法：

```cpp
// kernel/drivers/pit/pit.hpp
#ifdef CINUX_GUI
    static void set_tick_callback(void (*cb)(void*), void* ctx = nullptr);
    static void invoke_tick_callback();
private:
    static void (*tick_callback_)(void*);
    static void* tick_callback_ctx_;
#endif
```

```cpp
// kernel/drivers/pit/pit.cpp
#ifdef CINUX_GUI
void (*PIT::tick_callback_)(void*) = nullptr;
void* PIT::tick_callback_ctx_      = nullptr;

void PIT::set_tick_callback(void (*cb)(void*), void* ctx) {
    tick_callback_     = cb;
    tick_callback_ctx_ = ctx;
}

void PIT::invoke_tick_callback() {
    if (tick_callback_ != nullptr) {
        tick_callback_(tick_callback_ctx_);
    }
}
#endif
```

实现非常简洁——一个函数指针和一个 void* 上下文。set_tick_callback 注册回调，invoke_tick_callback 在 tick 中调用。整个机制用 `#ifdef CINUX_GUI` 包裹，在非 GUI 模式下不会编译这些代码，也不会占用任何内存。

在 IRQ0 handler 中的调用位置：

```cpp
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);
    PIC::send_eoi(0);

#ifdef CINUX_GUI
    invoke_tick_callback();    // ← 在 EOI 之后、Scheduler::tick() 之前
#endif

    cinux::proc::Scheduler::tick();
}
```

回调放在 EOI 之后是因为回调可能执行时间较长（特别是合成 + flip），如果放在 EOI 之前会阻塞后续中断。放在 Scheduler::tick() 之前是因为回调本身不是调度的一部分——它是 GUI 子系统的逻辑，和进程调度是独立的关注点。

### Heap max_size_ 安全机制

Canvas 的 3MB back buffer 分配暴露了一个严重的安全漏洞。修复方案是给 Heap 加一个扩展上限：

```cpp
// kernel/mm/heap.hpp
class Heap {
private:
    uint64_t max_size_{};    // ← 新增字段
    // ...
};

// kernel/mm/heap.cpp
void Heap::init(uint64_t virt_base, uint64_t initial_size) {
    // ...
    max_size_ = cinux::arch::KMEM_HEAP_SIZE;   // ← 设置上限
    // ...
}

bool Heap::expand(size_t min_bytes) {
    // ...
    uint64_t expand_size = needed_pages * cinux::arch::PAGE_SIZE;

    if (size_ + expand_size > max_size_) {       // ← 新增检查
        cinux::lib::kprintf("[HEAP] Expansion limit reached: %u KB / %u MB\n",
                            size_ / 1024, max_size_ / (1024 * 1024));
        return false;
    }
    // ...
}
```

max_size_ 在 init() 中被设置为 KMEM_HEAP_SIZE。之前这个值是 1MB（远不够 Canvas 的 3MB），现在改成了 128MB。虚拟地址空间预留 128MB 并不代表物理内存消耗——物理页是 expand() 时通过 PMM 按需分配的。如果 Canvas 分配了 3MB，物理内存只多用了 3MB，不是 128MB。

### memory_layout.hpp 的变更

```cpp
// kernel/arch/x86_64/memory_layout.hpp
constexpr uint64_t KMEM_HEAP_SIZE = 0x8000000ULL;  // 128 MB (was 1 MB)
```

从 1MB 提升到 128MB。这个数字的选取需要平衡两个因素：太大会浪费虚拟地址空间（虽然不消耗物理页），太小会限制 Canvas 和后续 GUI 组件的内存需求。128MB 对于一个 1024x768x32bpp 的 Canvas（约 3MB）加上多个窗口的离屏 Canvas 来说非常充裕。

### Bootloader 全量物理内存直接映射

这是本 tag 最复杂的修复，涉及 `kernel/mini/big_kernel_loader.cpp` 的改动。问题在于，之前 bootloader 只映射了 ELF 段覆盖的物理内存范围（约 20MB）。当 PMM 返回超过这个范围的物理地址时，`phys_to_virt()` 产生的虚拟地址没有映射，访问就 page fault。

修复方案是扫描 BootInfo 的 E820 内存映射，找到最高的可用 RAM 地址，然后映射 0 到该地址的全部物理内存。使用大页（2MB 和 1GB）来最小化页表开销——对于 8GB 物理内存，只需要大约 5 个页表页面。

## 设计决策

### 决策：PIT 回调用函数指针还是虚函数/观察者模式

**问题**: GUI 刷新应该怎么和 PIT 集成？

**本项目的做法**: 简单的函数指针 + void* 上下文。

**备选方案**: 观察者模式（PIT 维护一个 listener 列表），或者虚函数接口。

**为什么不选备选方案**: 我们只需要一个回调——GUI tick callback。观察者模式的列表管理和虚函数的 vtable 开销对于一个单回调场景是过度设计。函数指针简单、零开销、容易理解。如果未来需要多个 tick listener，可以很容易扩展为一个链表。

### 决策：全量直接映射 vs. kmap 按需映射

**问题**: phys_to_virt() 依赖直接映射，但映射全部物理内存开销大吗？

**本项目的做法**: 映射全部物理内存，用大页优化页表开销。

**备选方案**: 实现 kmap() API，需要时动态映射物理页到特定的虚拟地址区域。

**为什么不选备选方案**: kmap 需要维护一个映射跟踪数据结构（哪些虚拟地址映射了哪些物理页），还需要在不再需要时 kunmap 释放映射。对于一个教学内核来说，全量直接映射更简单——一次映射，永久有效。Linux 实际上也是这样做的（内核的 direct mapping 区域覆盖所有物理内存）。大页的使用使得页表开销极小（8GB 只需约 5 页页表）。

## 扩展方向

- draw_text 支持 Unicode（通过 PSF2 的 unicode table），渲染非 ASCII 字符（难度：中等）
- 字体渲染添加亚像素定位（subpixel rendering），改善小字号文字的清晰度（难度：较高）
- PIT 回调改为链表，支持多个独立模块注册各自的 tick handler（难度：低）
- Heap 添加 dump_stats() 可视化，帮助调试内存分配问题（难度：低）

## 参考资料

- OSDev Wiki: [PC Screen Font](https://wiki.osdev.org/PC_Screen_Font) — PSF2 格式详解、.incbin 嵌入技术
- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer) — text rendering from bitmap fonts
- Intel SDM Vol.3A Section 4.5: 4-Level Paging Structures — 2MB/1GB 大页映射
- Linux: `drivers/video/fbdev/` — fbdev 双缓冲参考实现
