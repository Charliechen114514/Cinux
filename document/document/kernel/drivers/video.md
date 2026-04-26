# 视频子系统

> 里程碑: `013_driver_vga_fb`

## 功能概述

基于 VESA linear framebuffer 的图形输出子系统，包含帧缓冲管理、PSF2 字体渲染和文本控制台。Console 作为 kprintf 的第二个输出后端注册。

## 帧缓冲 (`kernel/drivers/video/framebuffer.hpp/cpp`)
- `Framebuffer {addr, width, height, pitch, bpp}`
- `init(BootInfo&)` — 映射 MMIO
- `put_pixel(x, y, argb)` — `addr[y*pitch/4+x]=color`
- `fill_rect(x, y, w, h, color)` / `scroll_up(lines, line_height)` / `clear(color=0)`
- `get_pixel(x, y)` — 测试用回读

## PSF2 字体 (`kernel/drivers/video/font.hpp/cpp`, `kernel/drivers/video/font_data.S`)
- 解析 PSF2 header: magic=`0x864AB572`, width/height/bytes_per_glyph
- `font_render_char(fb, c, x, y, fg, bg)` — 按位图逐 bit 渲染
- 字体数据: `assets/font.psf` (8x16, 256 glyphs)，通过 `.incbin` 汇编嵌入

## 文本控制台 (`kernel/drivers/video/console.hpp/cpp`)
- `Console {fb_, col_, row_, cols_, rows_, fg_, bg_}`
- `putc(c)` — 处理 `\n \r \b` 和自动换行
- `scroll()` — 调用 `fb_.scroll_up`
- `set_color(fg, bg)` / `clear()`
- `console_sink_adapter(char c, void* ctx)` — kprintf 后端适配器

## kprintf 双输出
```cpp
kprintf_register_sink(Console::console_sink_adapter, &console);
```

## 源码位置
- `kernel/drivers/video/framebuffer.hpp/cpp`
- `kernel/drivers/video/font.hpp/cpp`
- `kernel/drivers/video/font_data.S`
- `kernel/drivers/video/console.hpp/cpp`
- `assets/font.psf` — 字体资源
