---
title: 013-driver-vga-fb-3 · VGA 帧缓冲
---

# 013-3 通读：文本控制台与 kprintf 双输出架构

## 概览

本文是 tag `013_driver_vga_fb` 三篇通读教程的第三篇，也是最终篇。前两篇我们分别搭建了帧缓冲区驱动和 PSF2 字体渲染引擎，现在要把它们组合起来构建一个完整的文本控制台，并重构 kprintf 使其同时输出到串口和屏幕。

本篇涉及四个文件：`console.hpp/cpp`（文本控制台）、`kprintf.hpp/cpp`（多后端重构）和 `main.cpp`（集成入口）。kprintf 的重构是本篇的技术亮点——从单后端串口输出进化到支持注册多个输出后端的"广播"模式，这种设计在 Linux 内核和 SerenityOS 中都有对应。

## 架构图

```
kprintf("Hello %s\n", "Cinux")
        │
        ▼
   vkprintf_impl (格式化引擎)
        │
        ▼
   lambda(char c) ─────► 遍历 sink 表
        │                       │
        │               ┌───────┴───────┐
        │               ▼               ▼
        │         sink[0]:          sink[1]:
        │         serial_sink_      console_sink_
        │         adapter           adapter
        │               │               │
        │               ▼               ▼
        │         Serial::putc()   Console::putc()
        │                               │
        │                        ┌──────┤
        │                        ▼      ▼
        │                   \n?  可打印字符?
        │                   │    │
        │                   ▼    ▼
        │              new_line() PSFFont::render_char()
        │                   │           │
        │                   ▼           ▼
        │              scroll?     Framebuffer::put_pixel()
        │                   │
        │                   ▼
        │          Framebuffer::scroll_up()

Console 状态机:
  putc(char c)
      │
      ├── '\n' → new_line(): col=0, row++ 或 scroll()
      ├── '\r' → col = 0
      ├── '\b' → col-- (wrap to prev line if needed)
      └── default:
              if col >= cols: new_line()
              font.render_char(fb, c, col*font_w, row*font_h, fg, bg)
              col++
```

## 代码精讲

### kprintf.hpp —— 多后端接口定义

```cpp
namespace cinux::lib {

using OutputSink = void(*)(char c, void* ctx);

static constexpr uint32_t KPRINTF_MAX_SINKS = 8;

void kprintf_register_sink(OutputSink fn, void* ctx);
void kprintf_init();
void kprintf(const char* fmt, ...);
void kvprintf(const char* fmt, va_list args);
[[noreturn]] void kpanic(const char* fmt, ...);

}  // namespace cinux::lib
```

重构的核心是 `OutputSink` 这个类型别名——一个函数指针，接受一个字符和一个不透明的上下文指针。这种"回调 + 上下文"的模式在 C 语言接口设计中极为常见（比如 pthread 的 `void* (*start_routine)(void*)`、qsort_r 的比较函数等），好处是在不引入虚函数或继承的情况下实现了运行时多态。

`KPRINTF_MAX_SINKS = 8` 是一个保守的上限——内核的输出后端不太可能超过 8 个（串口、控制台、QEMU debug port、内核日志缓冲区、网络日志……就算全部加上也才 5 个）。`[[noreturn]]` 属性告诉编译器 `kpanic` 永远不会返回，有助于静态分析和优化。

### kprintf.cpp —— 多后端实现

```cpp
namespace {

struct Sink {
    cinux::lib::OutputSink fn;
    void* ctx;
    bool enabled;
};

static Sink g_sinks[cinux::lib::KPRINTF_MAX_SINKS] = {};
static uint32_t g_sink_count = 0;

static Serial g_serial(SERIAL_COM1);

void serial_sink_adapter(char c, void* /*ctx*/) {
    g_serial.putc(c);
}

}  // anonymous namespace
```

匿名命名空间中定义了 sink 表、计数器和串口适配器。`Sink` 结构体包含三个字段：回调函数指针、上下文指针和启用标志。`enabled` 字段的存在是为了支持将来的"注销 sink"功能——注销时只需把 enabled 设为 false，注册新 sink 时可以复用空位。

串口适配器 `serial_sink_adapter` 是一个符合 `OutputSink` 签名的自由函数，它忽略上下文参数，直接调用全局 Serial 对象的 putc。这种设计把 Serial 的接口适配成了 kprintf 的通用接口——适配器模式（Adapter Pattern）的经典应用。

```cpp
void kprintf_register_sink(OutputSink fn, void* ctx) {
    if (fn == nullptr) return;
    for (uint32_t i = 0; i < g_sink_count; i++) {
        if (!g_sinks[i].enabled) {
            g_sinks[i] = {fn, ctx, true};
            return;
        }
    }
    if (g_sink_count < KPRINTF_MAX_SINKS) {
        g_sinks[g_sink_count++] = {fn, ctx, true};
    }
}
```

注册逻辑分两步：先在已有条目中寻找空位（被禁用的 sink），找不到就在末尾追加。空指针检查是第一道防线——防止注册无效回调。

```cpp
void kprintf_init() {
    g_serial.init();
    kprintf_register_sink(serial_sink_adapter, nullptr);
}
```

`kprintf_init()` 做两件事：初始化串口硬件，然后注册串口作为第一个 sink。这保证了后续所有 kprintf 调用至少有一个可用的输出后端。上下文传 nullptr 是因为 `serial_sink_adapter` 不需要额外上下文——它直接使用全局 Serial 对象。

```cpp
void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf_impl([&](char c) {
        for (uint32_t i = 0; i < g_sink_count; i++) {
            if (g_sinks[i].enabled) {
                g_sinks[i].fn(c, g_sinks[i].ctx);
            }
        }
    }, fmt, args);
    va_end(args);
}
```

`kprintf`、`kvprintf` 和 `kpanic` 的核心变化在于格式化回调：原来是直接调用 `g_serial.putc(c)`，现在变成遍历整个 sink 表，对每个启用的 sink 调用其回调。格式化引擎（`vkprintf_impl`）本身完全不变——它只负责把格式字符串和参数转换成字符序列，不关心这些字符最终去了哪里。

这种"格式化"和"输出"的分离是 Unix 哲学的体现：printf 负责格式化，sink 负责输出。Linux 内核的 `printk` 也采用了类似的设计——格式化后的字符通过 console 驱动链发送到各种输出设备。

### console.hpp —— Console 类声明

```cpp
namespace cinux::drivers {

class Console {
public:
    void init(Framebuffer& fb, PSFFont& font, uint32_t fg, uint32_t bg);
    void putc(char c);
    void clear();
    void set_color(uint32_t fg, uint32_t bg);
    static void console_sink_adapter(char c, void* ctx);

private:
    void scroll();
    void new_line();

    Framebuffer* fb_   = nullptr;
    PSFFont*     font_ = nullptr;
    uint32_t     col_  = 0;
    uint32_t     row_  = 0;
    uint32_t     cols_ = 0;
    uint32_t     rows_ = 0;
    uint32_t     fg_   = 0x00FFFFFF;
    uint32_t     bg_   = 0x00000000;
};

}  // namespace cinux::drivers
```

Console 类的接口只有四个公开方法和一个静态适配器方法。成员变量可以分为三组：依赖引用（fb_ 和 font_）、光标状态（col_ 和 row_）、控制台尺寸（cols_ 和 rows_）、颜色设置（fg_ 和 bg_）。所有指针初始化为 nullptr，确保在 `init()` 调用之前 `putc()` 会安全退出。

`console_sink_adapter` 是一个静态方法——它的签名完全匹配 `OutputSink`，可以注册到 kprintf 的 sink 表中。静态方法之所以能工作，是因为它通过 `void* ctx` 参数接收 Console 实例的指针。

### console.cpp —— Console 实现

```cpp
void Console::init(Framebuffer& fb, PSFFont& font, uint32_t fg, uint32_t bg) {
    fb_   = &fb;
    font_ = &font;
    fg_   = fg;
    bg_   = bg;
    col_  = 0;
    row_  = 0;

    cols_ = fb.width() / font.width();
    rows_ = fb.height() / font.height();

    clear();
}
```

`init()` 把依赖引用存为指针（C++ 中保存引用的指针比存引用本身更灵活，因为可以在运行时重新绑定），从帧缓冲区和字体尺寸计算出控制台的行列数（对于 1024x768 + 8x16，就是 128 列 x 48 行），然后清屏重置光标。

```cpp
void Console::putc(char c) {
    if (fb_ == nullptr || font_ == nullptr)
        return;

    switch (c) {
    case '\n':
        new_line();
        break;
    case '\r':
        col_ = 0;
        break;
    case '\b':
        if (col_ > 0) {
            col_--;
        } else if (row_ > 0) {
            row_--;
            col_ = cols_ - 1;
        }
        break;
    default:
        if (col_ >= cols_) {
            new_line();
        }
        font_->render_char(*fb_, static_cast<uint8_t>(c),
                           col_ * font_->width(),
                           row_ * font_->height(), fg_, bg_);
        col_++;
        break;
    }
}
```

`putc()` 是一个典型的 switch 驱动的字符处理状态机。`\n` 换行、`\r` 回车、`\b` 退格各自有明确的语义。可打印字符（default 分支）的处理顺序是：先检查是否需要换行（`col_ >= cols_`），然后渲染字符，最后移动光标。

值得注意的细节：`\b` 的行为是"移动光标但不擦除"——这是 Unix 终端的标准行为（退格后覆盖写入）。如果 col 已经在行首，退格会把光标移到上一行的最后一列——这种"折行退格"行为虽然不常见，但某些程序确实依赖它。

字符的像素坐标是 `col_ * font_->width()` 和 `row_ * font_->height()`——每个字符占据一个 `8x16` 的像素块，行列号乘以字体尺寸就得到像素坐标。

```cpp
void Console::new_line() {
    col_ = 0;
    if (row_ + 1 >= rows_) {
        scroll();
    } else {
        row_++;
    }
}

void Console::scroll() {
    if (fb_ == nullptr || font_ == nullptr)
        return;
    uint32_t line_height = font_->height();
    fb_->scroll_up(line_height, line_height, bg_);
}
```

`new_line()` 和 `scroll()` 构成了滚屏逻辑。当光标已经在最后一行时，`new_line()` 不再递增 row——而是调用 `scroll()` 向上滚动一行。`scroll()` 把字体高度（16 像素）作为滚动行数传给 Framebuffer 的 `scroll_up()`。

这里有一个设计上的简化：`scroll()` 不改变 row——因为滚屏后光标仍然在最后一行，只是内容被向上推了一行。新行的内容通过后续的 `putc()` 渲染出来。

```cpp
void Console::console_sink_adapter(char c, void* ctx) {
    auto* con = static_cast<Console*>(ctx);
    if (con)
        con->putc(c);
}
```

静态适配器方法是把 Console 接入 kprintf 的桥梁。它把 `void*` 上下文转回 `Console*`，空指针检查是防御性的——如果有人传了 nullptr 作为 ctx，这里不会崩溃。

### main.cpp —— 集成入口

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    // ... GDT, IDT, PIC, IRQ, PIT 初始化 ...

    auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    Framebuffer fb;
    fb.init(*boot_info);
    cinux::lib::kprintf("[BIG] Framebuffer initialised: %ux%u %ubpp\n",
                        fb.width(), fb.height(), boot_info->fb_bpp);

    PSFFont font;
    font.init();
    cinux::lib::kprintf("[BIG] PSF2 font loaded: %ux%u\n",
                        font.width(), font.height());

    Console console;
    console.init(fb, font, 0x00FFFFFF, 0x00000000);
    cinux::lib::kprintf_register_sink(Console::console_sink_adapter, &console);
    cinux::lib::kprintf("[BIG] Console initialised -- dual output active.\n");

    // ... unmask IRQ0, sti, idle loop ...
}
```

集成代码的初始化顺序很关键。Framebuffer 在 Console 之前初始化（Console 依赖 Framebuffer），PSFFont 在 Console 之前初始化（Console 依赖 Font）。Console 注册为 kprintf sink 之后，后续所有的 kprintf 调用都会同时输出到串口和屏幕。

注意"双输出激活"这条消息本身是通过 kprintf 打印的——在注册 Console sink 之前，它只会出现在串口上；注册之后的所有消息才会同时出现在两个输出上。

## 设计决策

### 决策：静态 sink 数组 vs. 链表

**问题**: kprintf 的后端列表用什么数据结构管理？

**本项目的做法**: 固定大小的静态数组（8 个元素），编译期确定上限。

**备选方案**: 使用内核链表动态管理，没有上限约束。

**为什么不选备选方案**: kprintf 是内核最基础的调试工具之一，它的依赖越少越好。链表需要动态内存分配（或者静态节点池），增加了复杂度和潜在的故障点。固定数组简单、可靠、零分配。Linux 内核的 printk 也使用类似的方式管理 console 驱动链——一个静态的 `console_driver` 数组。

**如果要扩展**: 如果确实需要超过 8 个后端，只需调大 `KPRINTF_MAX_SINKS` 常量。

### 决策：Console 存储 Framebuffer 指针而非引用

**问题**: Console 应该持有 Framebuffer 的引用还是指针？

**本项目的做法**: 存储裸指针（`Framebuffer*`）。

**备选方案**: 使用 C++ 引用（`Framebuffer&`），在构造函数中初始化。

**为什么不选备选方案**: Console 使用 `init()` 方法而非构造函数初始化——这是 Cinux 的统一风格（所有驱动都用 init/finalize 模式），因为内核对象经常需要先声明再初始化（比如作为局部变量声明后延迟初始化）。引用一旦绑定不可更改，而指针可以在 `init()` 时赋值。此外，裸指针允许用 nullptr 表示"未初始化"状态，引用做不到。

**如果要扩展**: 可以考虑使用 `std::optional<std::reference_wrapper<Framebuffer>>`（如果将来引入了合适的 stdlib 子集）或者自定义的 `NotNull<Framebuffer*>` 包装类型。

## 扩展方向

- **ANSI 转义序列支持**：实现 `\033[XXm` 颜色控制、`\033[2J` 清屏、`\033[H` 光标归位等基本的 ANSI/VT100 转义序列，使得内核日志可以有颜色和格式。难度：⭐⭐
- **QEMU Debug Console Sink**：向 I/O port 0xE9 写入字符（QEMU 的 `-debugcon` 选项），作为第三个 kprintf sink。这在调试时非常有用，因为它不受串口速率限制。难度：⭐
- **环形日志缓冲区 Sink**：把 kprintf 输出存入一个固定大小的环形缓冲区，供 dmesg-like 工具在后续读取。需要实现简单的环形缓冲区数据结构。难度：⭐⭐

## 参考资料

- Linux: `printk` 通过 `console_driver` 链管理多个输出后端，格式化后的字符通过 `console_write()` 发送给每个注册的 console 驱动。Linux 的 fbcon 子系统在 `drivers/video/console/fbcon.c` 中实现了基于帧缓冲区的控制台。
- xv6: `console.c` 使用 VGA 文本模式（0xB8000）的 80x25 字符网格，通过 CGA 控制器硬件自动渲染字形。双输出设计（串口 + CGA）与 Cinux 的多 sink 架构思路相同。
- SerenityOS: `GenericFramebufferConsole` 类与 Cinux 的 Console 功能相似，但支持更多终端特性（ANSI 转义序列、光标闪烁、选区等）。
