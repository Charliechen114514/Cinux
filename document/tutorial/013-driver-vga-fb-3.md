---
title: 013-driver-vga-fb-3 · VGA 帧缓冲
---

# 013-3 双输出：让内核不再"盲打"——文本控制台与 kprintf 多后端架构

> 标签：Console, kprintf, OutputSink, 多后端, 双输出, 控制台滚屏
> 前置：[013-2 PSF2 位图字体引擎](013-driver-vga-fb-2.md)

## 前言

前两章我们把帧缓冲区和字体渲染都准备好了，但到现在为止这些能力还是散装的——你能画像素、能画字符，但没有人帮你管理"下一个字符应该画在哪个位置"。实际的控制台文本输出有一堆琐碎的状态需要维护：当前光标在第几行第几列、到了行尾怎么换行、到了屏幕底部怎么滚屏、退格键回车键怎么处理。

同时，我们的 kprintf 还只往串口写——这个架构在加了屏幕输出之后就得重构了。总不能在 kprintf 里硬编码"同时调 Serial::putc 和 Console::putc"吧？将来如果还要加 QEMU debug console（port 0xE9）、内核日志缓冲区、网络日志……每次加一个后端就改一遍 kprintf 的核心逻辑，这代码很快就会变成意大利面条。

所以这一章要做两件事：第一，实现一个管理光标和滚屏的 Console 类；第二，把 kprintf 从"单串口输出"重构为"多后端广播"架构。完成后，所有通过 kprintf 打印的内核日志都会同时出现在串口终端和 QEMU 窗口上——内核终于不再"盲打"了。

## 环境说明

Framebuffer 和 PSFFont 必须已经初始化完成。Console 在 `kernel_main` 的初始化链中排在 Framebuffer 和 PSFFont 之后、中断使能之前。kprintf 的串口后端在 `kprintf_init()` 中注册（这是整个初始化链的第一步），Console 后端在 Console 初始化完成后注册为第二个 sink。

## 第一步：重构 kprintf 为多后端架构

我们先从 kprintf 的重构开始，因为 Console 依赖 kprintf 的 sink 注册接口。

原来的 kprintf 直接调用全局 Serial 对象的 putc——这很简单但不灵活。重构的核心是引入一个 `OutputSink` 类型和对应的注册机制：

```cpp
using OutputSink = void(*)(char c, void* ctx);
static constexpr uint32_t KPRINTF_MAX_SINKS = 8;

void kprintf_register_sink(OutputSink fn, void* ctx);
```

`OutputSink` 是一个函数指针类型——接受一个字符和一个不透明的上下文指针。这种"回调 + 上下文"的模式在 C 语言接口设计中极为常见，好处是在不引入虚函数或继承的情况下实现了运行时多态。Linux 内核的 `console_driver` 链用了类似的方式——每个 console 驱动注册一个 `write()` 回调，printk 格式化后的字符通过这些回调发送给各个输出设备。

sink 表用固定大小的静态数组来管理：

```cpp
struct Sink {
    OutputSink fn;
    void* ctx;
    bool enabled;
};

static Sink g_sinks[KPRINTF_MAX_SINKS] = {};
static uint32_t g_sink_count = 0;
```

三个字段：回调函数指针、上下文指针、启用标志。`enabled` 字段是为将来的"注销 sink"功能预留的——注销时只需把 enabled 设为 false，注册新 sink 时可以复用空位。8 个位置的上限看起来很小，但仔细想想内核的输出后端不太可能超过这个数——串口、控制台、QEMU debug port、内核日志缓冲区、网络日志……就算全部加上也才 5 个。

串口作为默认后端，通过一个适配器函数包装成 OutputSink 格式：

```cpp
static Serial g_serial(SERIAL_COM1);

void serial_sink_adapter(char c, void* /*ctx*/) {
    g_serial.putc(c);
}

void kprintf_init() {
    g_serial.init();
    kprintf_register_sink(serial_sink_adapter, nullptr);
}
```

`serial_sink_adapter` 忽略上下文参数（因为 Serial 是全局对象，不需要额外上下文），直接调用 `g_serial.putc`。这就是经典的适配器模式——把 Serial 的接口适配成 kprintf 的通用 OutputSink 接口。

注册逻辑也不复杂：

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

先在已有条目中寻找空位（被禁用的 sink），找不到就在末尾追加。空指针检查是第一道防线。

最后是 kprintf 核心函数的变化——格式化引擎完全不变，只是把原来直接调用 `g_serial.putc(c)` 的地方改成了遍历 sink 表：

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

格式化和输出的分离是 Unix 哲学的体现——kprintf 负责格式化，sink 负责输出。xv6 的 `console.c` 也采用了类似的双输出设计，只不过它是直接在 `consputc()` 函数里同时调用 `uartputc()` 和 `cgaputc()`——没有抽象出通用接口，简单但不灵活。Cinux 的多 sink 架构在扩展性上更接近 Linux 的 printk + console_driver 链的设计。

`kpanic` 和 `kvprintf` 的修改完全一样——把 `g_serial.putc` 替换成遍历 sink 表的 lambda。

## 第二步：实现 Console 类

kprintf 的基础设施就绪了，现在来实现 Console。一个最简的文本控制台本质上是一个维护光标状态并处理特殊字符的有限状态机。

Console 的成员变量可以分为四组：

```cpp
Framebuffer* fb_   = nullptr;   // 依赖引用
PSFFont*     font_ = nullptr;   // 依赖引用
uint32_t     col_  = 0;         // 光标列号
uint32_t     row_  = 0;         // 光标行号
uint32_t     cols_ = 0;         // 最大列数
uint32_t     rows_ = 0;         // 最大行数
uint32_t     fg_   = 0x00FFFFFF; // 前景色（白色）
uint32_t     bg_   = 0x00000000; // 背景色（黑色）
```

所有指针初始化为 nullptr——这意味着在 `init()` 调用之前，`putc()` 会因为空指针检查而安全退出。这种防御性设计确保了即使初始化顺序出错，也不会导致 page fault。

`init()` 从 Framebuffer 和 PSFFont 的尺寸计算出控制台的行列数：

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

对于 1024x768 的分辨率和 8x16 的字体，就是 128 列 x 48 行——比标准 VGA 文本模式的 80x25 大了将近三倍。这意味着同样面积的屏幕能显示更多的文字，对于调试输出来说非常友好。

`putc()` 是控制台的核心，一个 switch 驱动的字符处理状态机：

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

三种控制字符的语义：`\n` 换行（列归零 + 行加一）、`\r` 回车（列归零，行不变）、`\b` 退格（列减一，如果已在行首则回到上一行末尾）。可打印字符的处理顺序是：先检查是否需要换行（`col_ >= cols_`），然后渲染字符，最后移动光标。

这里有几个容易搞错的细节。首先，`col_ >= cols_` 的判断要在渲染之前——如果当前列已经是最后一列的下一列（等于 cols），需要先换行再渲染，否则字符会画到屏幕外面去。其次，字符的像素坐标是 `col_ * font_->width()` 和 `row_ * font_->height()`——行列号乘以字体尺寸就得到像素坐标，每个字符占据一个 `8x16` 的像素块。最后，`\b` 只移动光标不擦除内容——这是 Unix 终端的标准行为。

xv6 的 `console.c` 中的 `cgaputc()` 实现了几乎相同的状态机逻辑，只不过它操作的是 VGA 文本模式的字符缓冲区（0xB8000，每个字符占 2 字节——1 字节字符编码 + 1 字节属性），不需要字体渲染。Cinux 使用图形模式帧缓冲区，所以每个字符的渲染都要经过 PSFFont::render_char() -> Framebuffer::put_pixel() 的完整路径。

滚屏逻辑在 `new_line()` 和 `scroll()` 中：

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

当光标已经在最后一行时，`new_line()` 不再递增 row——而是调用 `scroll()` 向上滚动一个字符高度（16 像素）。scroll() 不改变 row 的值，因为滚屏后光标仍然在最后一行，只是内容被向上推了一行。新行的内容通过后续的 putc() 渲染出来。

Linux 的 fbcon 实现了三种滚动模式。SCROLL_REDRAW 是最简单的——和我们一样用 memmove 把所有行向上移动，然后清空底部。SCROLL_PAN 则聪明得多——它在帧缓冲区中分配比屏幕更高的虚拟分辨率，然后通过改变显示起始地址（ypan）来实现"滚动"，不需要实际拷贝任何像素数据。SCROLL_WRAP 类似 PAN，但用循环缓冲区。这三种模式分别适用于不同硬件能力——SerenityOS 的 GenericFramebufferConsole 使用的也是类似 REDRAW 的简单模式。

Console 还需要提供一个静态适配器方法来接入 kprintf：

```cpp
void Console::console_sink_adapter(char c, void* ctx) {
    auto* con = static_cast<Console*>(ctx);
    if (con)
        con->putc(c);
}
```

这个静态方法把 `void*` 上下文转回 `Console*`，然后调用实例的 `putc`。这就是 Console 和 kprintf 之间的桥梁——注册时把 Console 实例的指针作为 ctx 传入，每次 kprintf 格式化一个字符时，这个适配器就会被调用。

## 第三步：在 kernel_main 中集成双输出

所有的组件都就绪了，现在在 `kernel_main` 中按顺序初始化并注册：

```cpp
// Step 9: 帧缓冲区
auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
Framebuffer fb;
fb.init(*boot_info);
cinux::lib::kprintf("[BIG] Framebuffer initialised: %ux%u %ubpp\n",
                    fb.width(), fb.height(), boot_info->fb_bpp);

// Step 10: 字体
PSFFont font;
font.init();
cinux::lib::kprintf("[BIG] PSF2 font loaded: %ux%u\n",
                    font.width(), font.height());

// Step 11: 控制台 + 双输出
Console console;
console.init(fb, font, 0x00FFFFFF, 0x00000000);
cinux::lib::kprintf_register_sink(Console::console_sink_adapter, &console);
cinux::lib::kprintf("[BIG] Console initialised -- dual output active.\n");
```

初始化顺序是严格线性的：Framebuffer -> PSFFont -> Console -> 注册 sink。每一步依赖前一步的输出，不能打乱顺序。注意"双输出激活"这条消息本身是通过 kprintf 打印的——在注册 Console sink 之前，它只会出现在串口上；注册之后的所有消息才会同时出现在串口和屏幕上。

Console、Framebuffer 和 PSFFont 都是 `kernel_main` 的局部变量——它们的生命周期就是内核的整个运行周期（因为 `kernel_main` 永远不会返回，最后是一个 `while(1) { hlt; }` 循环）。

## 验证

启动 QEMU 后，关键观察点是：

- 串口终端：从第一条 `[BIG] Big kernel running` 开始，所有日志正常输出
- QEMU 窗口：从 `[BIG] Console initialised -- dual output active.` 这条消息开始，后续日志同时出现在屏幕上（白字黑底）
- PIT tick 消息（每秒一条）应该同时出现在串口和屏幕上，证明双输出持续工作

如果你想更直观地验证，可以在 Console 初始化之后加一行测试：

```
kprintf("Hello from Cinux!\nSecond line.\n");
```

这行文字应该同时出现在串口和 QEMU 窗口上。

## 收尾

到这里，tag 013 的工作就全部完成了。我们的内核现在有了完整的图形输出能力：帧缓冲区驱动、位图字体引擎、文本控制台，以及支持多后端的 kprintf。从这一章开始，内核的所有日志都不再需要串口才能看到了——QEMU 窗口上直接显示白字黑底的启动信息，算是真正"点亮了屏幕"。

下一章我们要进入新的领域了。

## 参考资料

- Linux: `kernel/printk/printk.c` 中的 printk 实现通过 `console_driver` 链管理多个输出后端，`drivers/video/console/fbcon.c` 实现了基于帧缓冲区的控制台，支持三种滚动模式（REDRAW/PAN/WRAP）
- xv6: `console.c` 的 `consputc()` 同时调用 `uartputc()` 和 `cgaputc()` 实现双输出，使用 VGA 文本模式（0xB8000）的 80x25 字符网格
- SerenityOS: `Kernel/TTY/VirtualConsole.cpp` 和 `Kernel/Graphics/FramebufferConsole.h` 实现了更完整的控制台功能，包括 ANSI 转义序列、光标控制和多虚拟终端
- Intel SDM: Vol.3A Section 4.5 — 页表操作相关参考
