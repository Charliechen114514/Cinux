---
title: 029-gui-canvas-3 · GUI Canvas
---

# PSF2 字体渲染、定时刷新与 3MB 内存分配引发的全局修复

> 标签：PSF2, draw_text, PIT callback, heap safety, direct map, E820
> 前置：[029 图元绘制](029-gui-canvas-2.md)

## 前言

如果说前两章是在搭积木，那这一章就是在搭积木的过程中发现地基有裂缝。事情是这样的：Canvas 需要大约 3MB 的后备缓冲区，这个数字在之前只有 64KB 堆使用量的内核里从未出现过。当 `new uint32_t[786432]` 第一次触发堆扩展时，一系列隐藏了十几个 tag 的内存管理问题全部浮出水面——堆无限扩展覆盖了 MMIO 区域、phys_to_virt() 对高地址物理页失效、测试用例的假设被打破。这一章除了实现字体渲染和定时刷新，我们还要把这个「3MB 引发的全局修复」完整走一遍。

## 环境说明

x86_64 QEMU + VBE 1024x768x32bpp。本章新增的硬件交互：PIT（8254 定时器）的 tick 回调机制。物理内存总量约 8GB（QEMU 默认配置），bootloader 需要映射全部物理内存。

## 第一步——PSF2 字体格式与 glyph() 访问器

Linux 控制台使用的字体格式叫 PSF（PC Screen Font），PSF2 是第二个版本。它的结构非常简单：一个 32 字节的头部，后面紧跟所有字形的位图数据。头部包含魔数（0x864AB572）、字形数量（通常是 256）、每个字形的字节数、以及宽高。

我们的字体文件通过汇编 `.incbin` 指令嵌入内核二进制，在运行时通过链接器符号 `font_psf_start` 访问：

```cpp
// kernel/drivers/video/font.cpp
extern "C" {
extern const uint8_t  font_psf_start[];
extern const uint8_t  font_psf_end[];
extern const uint32_t font_psf_size[];
}

struct PSF2Header {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t flags;
    uint32_t length;
    uint32_t charsize;
    uint32_t height;
    uint32_t width;
} __attribute__((packed));

static constexpr uint32_t PSF2_MAGIC = 0x864AB572;
```

PSF2Header 的结构和 OSDev Wiki 上描述的完全一致。我们加了 `__attribute__((packed))` 确保编译器不会插入 padding——虽然在这个 32 字节全 uint32_t 的结构中 padding 不会出现，但好习惯总不会错。

PSFFont::init() 解析头部，存储字形数据指针。然后新增的 glyph() 方法让 Canvas 能直接拿到字形位图：

```cpp
const uint8_t* PSFFont::glyph(uint8_t c) const {
    if (glyphs_ == nullptr) return nullptr;
    if (c >= num_glyphs_) c = 0;
    return glyphs_ + static_cast<uint32_t>(c) * bytes_per_glyph_;
}
```

逻辑就是 `base + index * size`——加上空指针检查和越界保护（超出范围则回退到字形 0，通常是一个方框符号）。返回的指针指向 bytes_per_glyph_ 字节的位图数据，每字节代表一行，MSB 是最左像素。

## 第二步——draw_text 文字渲染

有了 glyph() 访问器，draw_text 就是逐字符、逐行、逐位地把前景色像素画到 Canvas 上：

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

三重循环的结构：外层遍历字符串，中层遍历字形的行，内层遍历每行的位。`(bits >> (7 - col)) & 1` 从 MSB 开始检查——bit 7 对应最左像素，bit 0 对应最右像素。只有位为 1 的位置才画前景色像素，位为 0 的位置跳过——文字是「透明」的，叠加在背景上。

换行符 '\n' 让光标回到行首（x，不是 0）并下移一个字形高度。这是最基本的文本排版——没有自动换行、没有字距调整、没有 Unicode 支持。对于一个内核级的位图文字渲染器来说，这些已经够用了。后续的终端模拟器会在此基础上添加更复杂的文本处理逻辑。

## 第三步——PIT 定时回调

GUI 画面需要定期刷新。我们给 PIT 驱动加了一个回调机制——注册一个函数，每次 IRQ0 tick 时自动调用：

```cpp
// kernel/drivers/pit/pit.hpp — 新增
#ifdef CINUX_GUI
    static void set_tick_callback(void (*cb)(void*), void* ctx = nullptr);
    static void invoke_tick_callback();
private:
    static void (*tick_callback_)(void*);
    static void* tick_callback_ctx_;
#endif
```

```cpp
// kernel/drivers/pit/pit.cpp — IRQ0 handler 中的调用
void PIT::irq0_handler(InterruptFrame* /*frame*/) {
    tick_count_.fetch_add(1, lib::MemoryOrder::Relaxed);
    PIC::send_eoi(0);
#ifdef CINUX_GUI
    invoke_tick_callback();
#endif
    cinux::proc::Scheduler::tick();
}
```

回调放在 EOI 之后、Scheduler::tick() 之前。这个位置是精心选择的：EOI 必须尽早发送，否则后续中断会被阻塞；回调放在 Scheduler::tick() 之前是因为调度器可能会切换到另一个任务，如果回调还没执行完就被切走了，那中断上下文的状态会变得很混乱。

这个回调运行在 IRQ0 中断上下文中——意味着不能分配内存、不能 sleep、不能获取自旋锁以外的锁。GUI 的回调函数负责排空输入事件队列、合成窗口、调用 flip()。重操作（比如 fork+execve 创建新终端窗口）通过一个原子变量 `g_pending_action` 排队，让独立的 worker 线程在进程上下文中执行。这种「ISR 排队 + worker 消费」的模式在内核编程中非常常见——Linux 的 tasklet 和 workqueue 就是同一个思路。

## 第四步——3MB 引发的堆安全修复

现在我们来讲本 tag 最精彩的排查故事。Canvas init 时 `new uint32_t[786432]`（约 3MB）触发堆扩展。原来的 `Heap::expand()` 没有上限检查——它会把虚拟地址空间从 64KB 一直扩展到需要的大小，中间没有任何停止点。

在我们的内存布局中，堆区域后面紧跟着 MMIO 区域（AHCI BAR5 映射）、framebuffer 映射、栈区域。当堆扩展到 3MB 以上时，`g_vmm.map()` 开始把物理页映射到本该属于 MMIO 的虚拟地址。后续的 AHCI 操作写入这些被覆盖的地址，实际上写到了堆数据中——AHCI 超时、命令失败。更严重的是，TaskBuilder 分配内核栈时拿到的虚拟地址已经在堆的扩展范围内，栈数据和堆数据互相覆盖。

修复方案很简洁——给 Heap 加一个 max_size_ 上限：

```cpp
// kernel/mm/heap.hpp — 新增字段
uint64_t max_size_{};

// kernel/mm/heap.cpp — init 中设置
max_size_ = cinux::arch::KMEM_HEAP_SIZE;  // 128 MB

// kernel/mm/heap.cpp — expand 中检查
if (size_ + expand_size > max_size_) {
    kprintf("[HEAP] Expansion limit reached: %u KB / %u MB\n",
            size_ / 1024, max_size_ / (1024 * 1024));
    return false;
}
```

KMEM_HEAP_SIZE 从 1MB 提升到 128MB。128MB 是虚拟地址空间预留，不是物理内存消耗——物理页在 expand() 中按需通过 PMM 分配。如果 Canvas 分配了 3MB，物理内存只多了 3MB。

## 第五步——全量物理内存直接映射

堆修复之后，新的问题出现了。Canvas 的 3MB 分配消耗了大量低地址物理页，PMM 开始返回高地址物理页（>20MB）。这些高地址没有 bootloader 的直接映射——bootloader 只映射了 ELF 段覆盖的范围（约 20MB）。`phys_to_virt()` 对这些地址返回的虚拟地址没有映射，访问就 page fault。

修复在 bootloader（`big_kernel_loader.cpp`）中进行：扫描 E820 内存映射，找到最高的可用 RAM 地址，然后映射从 0 到该地址的全部物理内存。使用 2MB 大页映射 0-1GB 范围，1GB 大页映射 >=1GB 范围。8GB 物理内存只需要大约 5 个页表页面，开销极小。

这和 Linux 的做法完全一致——Linux 内核的 `direct mapping` 区域覆盖所有物理内存，`phys_to_virt()` 就是简单的加一个偏移量。我们的 `KERNEL_VMA = 0xFFFFFFFF80000000` 就是这个偏移。

## 与 Linux fbdev 的对比

Linux fbdev 的双缓冲和 Cinux 非常类似——用户程序分配 back buffer、渲染、然后 memcpy 或 FBIOPAN_DISPLAY 刷新。但 Linux 的字体渲染要复杂得多。Linux 的 fbcon（framebuffer console）使用 consolefont 机制（也就是 PSF 格式的字体），但渲染路径经过多层抽象：`vt.c` → `fbcon.c` → `fbdev driver` → 最终写入 framebuffer。Cinux 的 draw_text 直接从 PSF2 字形到位图到 Canvas，路径短得多。

Linux 还有一层 DRM/KMS 在 fbdev 之上。DRM 的 framebuffer 是第一类内核对象（`struct drm_framebuffer`），支持多缓冲、VSync 同步的 page flip。Cinux 的 flip 是纯软件 memcpy，没有硬件同步——这意味着在高速绘制时可能出现画面撕裂（显示控制器正在扫描时我们拷贝了新帧）。对于一个教学内核来说这是可接受的取舍，但如果后续想做流畅的动画，需要实现 VSync 等待或双缓冲区 page flip。

从内存管理的角度看，Linux 的 framebuffer 通常是通过 `dma_alloc_coherent()` 分配的——DMA 一致的内存区域，CPU 和 GPU 都可以直接访问。这意味着 DRM 的 page flip 不需要 CPU 做任何拷贝——只需要告诉显示控制器「从新的物理地址开始扫描」。Cinux 的 flip 需要 CPU 做 3MB 的 memcpy，因为我们的 back buffer 在普通堆内存中，显示控制器不知道它的存在。如果后续想让 flip 更高效，需要把 back buffer 也分配在 MMIO 可达的物理地址范围内。

## 排查过程复盘

这个 tag 的排查经历值得单独总结一下，因为它是典型的「大分配暴露深层问题」模式。问题的表象是 Canvas 测试通过后内核 hang，看起来像是 Canvas 的 bug，但实际上 Canvas 只是触发器——真正的问题在内存管理基础设施中潜伏了很久，只是之前的分配量都不够大，没有触发而已。

第一个问题（堆扩展越界）的排查思路是：看到 hang 在 `test_fifo_ordering` 中——这和 Canvas 无关。为什么会 hang？检查发现 Heap expand 后 TaskBuilder 分配栈时 VMM map 行为异常——说明堆覆盖了栈的虚拟地址空间。修复方法很自然：给堆加一个上限。

第二个问题（phys_to_virt 高地址失效）的排查更曲折。heap 修复后 hang 转移到 `test_create_user_space`——这也是 Canvas 无关的测试。AddressSpace 构造中 `phys_to_virt()` 返回了未映射的地址。原因在于 PMM 开始返回高地址物理页——而 Canvas 的 3MB 分配消耗了大量低地址页。这说明第一个问题的修复「逼着」PMM 去高地址区域找空间，暴露了 bootloader 直接映射不完整的隐患。修复方法：全量映射所有物理内存。

这种「修了一个问题暴露了下一个」的模式在 OS 开发中非常常见。根本原因是内存管理是层层依赖的——堆依赖 VMM，VMM 依赖页表，页表依赖 bootloader 的初始映射。任何一层的假设被打破，上面所有层都可能出问题。Canvas 的 3MB 分配就是打破平衡的那个「大石头」。

## 参考资料

- OSDev Wiki: [PC Screen Font](https://wiki.osdev.org/PC_Screen_Font) — PSF2 格式规范、.incbin 嵌入方法
- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/VBE) — 位图字体渲染
- Intel SDM Vol.3A Section 4.5: 4-Level Paging — 2MB (PS bit in PDE) 和 1GB (PS bit in PDPE) 大页映射
- Linux: [fbdev double buffering](https://github.com/lengfeld/fb-double-buffering-tests) — 用户空间双缓冲实现参考
- SerenityOS: [LibGfx Painter](https://github.com/SerenityOS/serenity) — 字体渲染和抗锯齿

## 收尾

到这里，029_gui_canvas 的全部功能都已经就位了。我们有了完整的双缓冲 Canvas（像素、矩形、直线、文字、位图、blit），PIT 定时回调驱动自动刷新，以及一个更健壮的内存管理基础（堆上限、全量直接映射）。QEMU 里应该能看到深色背景上散布着彩色矩形，顶部居中是白色的 "Cinux GUI" 文字。

这看起来可能还很粗糙——没有窗口、没有按钮、没有鼠标光标——但这块 Canvas 就是后续所有 GUI 组件的画布。下一步我们要在它上面构建窗口管理器。
