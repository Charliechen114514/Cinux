# 013-2 字模引擎：把文字画到像素上——PSF2 位图字体解析与渲染

> 标签：PSF2, 位图字体, .incbin, 字形渲染, glyph bitmap
> 前置：[013-1 帧缓冲区与 MMIO 映射](013-driver-vga-fb-1.md)

## 前言

上一章我们把帧缓冲区搞定了——可以画像素、填矩形了。但如果你直接往帧缓冲区写一个 `'A'`，屏幕上显示的是 `0x41` 这个像素值对应的颜色，而不是字母 A。原因很简单：帧缓冲区只认像素，不认字符。要在屏幕上"写字"，我们需要知道每个字符"长什么样"——这就是字体要做的事情。

这一章要做的是：用 Python 脚本生成一个 PSF2 格式的 8x16 位图字体文件，用 GAS 汇编的 `.incbin` 伪指令把它嵌入内核二进制，然后写一个 PSFFont 类解析头部并逐像素地把字形渲染到帧缓冲区上。完成后，你就能在屏幕上任何位置画出单个 ASCII 字符——虽然还不能自动换行和滚动，但"能写字"这个关键能力就算具备了。

## 环境说明

字体文件在编译时嵌入内核，所以不依赖运行时的文件系统。我们使用 IBM PC 经典的 8x16 位图字体（CP437 编码），每个字符是 8 像素宽 × 16 像素高的单色位图。工具链是 GAS（GNU Assembler）+ Python 3，嵌入方式是 `.incbin` 伪指令。PSF2 格式的魔数是 0x864AB572，整个字体文件只有 4128 字节——非常小巧，不会显著增加内核二进制的大小。

## 第一步：PSF2 字体格式与生成脚本

PSF2（PC Screen Font version 2）是 Linux 内核控制台使用的位图字体格式，OSDev Wiki 的 [PC Screen Font](https://wiki.osdev.org/PC_Screen_Font) 页面有详细的格式说明。它的结构非常简洁：一个 32 字节的头部，后面跟着所有字形（glyph）的位图数据，没有任何花哨的压缩或索引。

头部有 8 个 uint32_t 字段：

```python
PSF2_MAGIC = 0x864AB572
PSF2_VERSION = 0
PSF2_HEADER_SIZE = 32
PSF2_FLAGS = 0        # 没有 Unicode 映射表
PSF2_LENGTH = 256     # 256 个字形
PSF2_CHARSIZE = 16    # 每个字形 16 字节（16 行 × 1 字节/行）
PSF2_HEIGHT = 16
PSF2_WIDTH = 8
```

`FLAGS = 0` 意味着没有 Unicode 映射表——字形的位置就等于其编码值（字形 0x41 就是字符 'A'）。`LENGTH = 256` 覆盖整个 8 位字符集。`CHARSIZE = 16` 是因为 8 像素宽的字体每行恰好 1 字节（8 bit），16 行就是 16 字节。

字形数据的编码方式是：每个字节表示一行 8 个像素，bit 7（最高位）是最左边的像素，bit 0（最低位）是最右边的像素。以字母 `A` 为例：

```
Row 0: 0x38 = 00111000  → ...##...
Row 1: 0x6C = 01101100  → ..##.##.
Row 2: 0xC6 = 11000110  → ##...##.
Row 3: 0xC6 = 11000110  → ##...##.
Row 4: 0xFE = 11111110  → #######.
Row 5: 0xC6 = 11000110  → ##...##.
...
```

这个位序在渲染时很重要——如果你把 `bit 7 - col` 写成了 `col`，渲染出来的文字会是水平镜像的。这个坑我也踩过，当时盯着屏幕看了半天才发现字母全是反的。

Python 脚本（`scripts/gen_psf_font.py`）把这些字形数据硬编码在一个大列表中，然后用 `struct.pack` 以小端序写入头部，最后写入 256 个字形的原始字节。输出文件大小 = 32 + 256 * 16 = 4128 字节，非常小巧。

```python
with open(out_path, "wb") as f:
    f.write(struct.pack("<IIIIIIII",
        PSF2_MAGIC, PSF2_VERSION, PSF2_HEADER_SIZE, PSF2_FLAGS,
        PSF2_LENGTH, PSF2_CHARSIZE, PSF2_HEIGHT, PSF2_WIDTH))
    for glyph in FONT_DATA[:256]:
        f.write(bytes(glyph))
```

## 第二步：用 .incbin 把字体嵌入内核

内核现在还没有文件系统（至少目前还没有），所以不能在运行时加载字体文件。标准做法是用汇编器的 `.incbin` 伪指令把二进制文件直接嵌入 ELF 的 `.rodata` 段。

```asm
.section .rodata

.global font_psf_start
.type   font_psf_start, @object
font_psf_start:
    .incbin "assets/font.psf"

.global font_psf_end
font_psf_end:

.global font_psf_size
.type   font_psf_size, @object
font_psf_size:
    .long font_psf_end - font_psf_start
```

这段汇编做的事情非常直接：把 `assets/font.psf` 文件的原始字节插入到 `.rodata`（只读数据）段，用三个全局符号标记起止和大小。`.incbin` 等价于在编译时把整个文件展开成一系列 `.byte` 指令，但更高效且可读。文件路径相对于汇编器的 include path——CMake 构建时会确保项目根目录在 include path 中，所以 `"assets/font.psf"` 可以正确解析。

C++ 代码通过 `extern "C"` 声明这些符号来访问嵌入的数据：

```cpp
extern "C" {
extern const uint8_t  font_psf_start[];
extern const uint8_t  font_psf_end[];
extern const uint32_t font_psf_size[];
}
```

`extern "C"` 是必须的——C++ 的 name mangling 会改变符号名，而 GAS 生成的符号名是原样的。加上 `extern "C"` 后，链接器就能正确地把 C++ 代码中的 `font_psf_start` 和汇编中的同名符号匹配起来。

另一种嵌入二进制数据的方案是用 `objcopy` 把 .psf 文件转成 .o 目标文件再链接——Linux 内核就用这种方式嵌入 initramfs。但 `.incbin` 方案更简洁：只需要一个 .S 文件，符号命名完全由我们控制。objcopy 自动生成的符号名基于文件路径（比如 `_binary_assets_font_psf_start`），既丑又不稳定——换个路径所有引用都得改。

## 第三步：PSFFont 类——解析头部与渲染字形

PSFFont 类的实现分两部分：`init()` 解析 PSF2 头部，`render_char()` 逐像素渲染字形。

PSF2 头部用一个 `__attribute__((packed))` 的结构体来精确匹配文件布局：

```cpp
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
```

虽然全是 uint32_t 的结构体在 x86_64 上本身就天然对齐，packed 属性更多是一种防御性编程——万一将来有人在中间加了一个 uint8_t 字段，没有 packed 的话编译器会插入填充字节，头部就与文件布局不匹配了。

`init()` 的逻辑：

```cpp
void PSFFont::init() {
    const auto* hdr = reinterpret_cast<const PSF2Header*>(font_psf_start);
    if (hdr->magic != PSF2_MAGIC)
        return;

    num_glyphs_       = hdr->length;
    bytes_per_glyph_  = hdr->charsize;
    width_            = hdr->width;
    height_           = hdr->height;
    glyphs_           = font_psf_start + hdr->header_size;
}
```

把 `font_psf_start` 解释为 PSF2Header 指针，验证魔数（0x864AB572），然后提取参数。`glyphs_` 指向头部之后的第一个字节——也就是第一个字形的起始地址。注意这里用的是 `hdr->header_size` 而不是硬编码 32，因为 PSF2 格式允许头部有扩展字段。

如果魔数不匹配（比如 `.incbin` 没有正确嵌入数据，或者 CMake 的 include path 配错了导致嵌入了空文件），函数直接返回，所有成员保持零值/空值。这种"静默失败"的设计在内核初始化代码中是合理的——我们还没到能打印错误信息的地步。

现在来看渲染的核心——`render_char()`：

```cpp
void PSFFont::render_char(Framebuffer& fb, uint8_t c, uint32_t x, uint32_t y,
                          uint32_t fg, uint32_t bg) {
    if (glyphs_ == nullptr)
        return;
    if (c >= num_glyphs_)
        c = 0;

    const uint8_t* glyph = glyphs_ + static_cast<uint32_t>(c) * bytes_per_glyph_;

    for (uint32_t row = 0; row < height_; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < width_; col++) {
            bool on = (bits >> (7 - col)) & 1;
            fb.put_pixel(x + col, y + row, on ? fg : bg);
        }
    }
}
```

输入是字符编码 `c`、像素坐标 `(x, y)`、前景色 `fg` 和背景色 `bg`。首先做越界保护——字符编码超过字形总数时降级到字形 0（空白或占位符）。然后计算目标字形的起始地址：`glyphs_ + c * bytes_per_glyph_`。

双重循环遍历字形的每一行和每一列。`glyph[row]` 取出当前行的 8 位位图，`(bits >> (7 - col)) & 1` 提取第 `col` 列的像素值。这里位移方向是 `7 - col`——因为 bit 7 是最左边的像素，col=0 时取 bit 7，col=7 时取 bit 0。根据像素值写入前景色或背景色。

你会发现这种渲染方式是"全覆盖"的——每个字符占据一个完整的 `8x16` 像素块，前景像素和背景像素都会被写入。这意味着连续渲染两个字符时不会有像素残留的问题，但也意味着字体渲染会覆盖已有的画面内容。在控制台场景下这完全没问题——每个字符位置都会被重新渲染。

每个字符的渲染需要调用 `width * height = 128` 次 `put_pixel`。对于控制台文本输出来说完全够用。作为对比，Linux 内核的 fbcon 使用类似的逐像素渲染方式（通过 `fb_imageblit()` 函数），但同时还支持硬件加速的 BitBLT 操作——如果显卡驱动提供了 `fb_imageblit` 回调，就可以直接用 GPU 来渲染字形。

SerenityOS 的字体系统则完全不同——它把字体渲染放到了用户空间的 LibGfx 库中，内核只负责把渲染好的像素数据 blit 到帧缓冲区。这种设计的优势是内核代码更简洁，字体格式的支持可以在用户空间灵活扩展；劣势是内核启动早期（用户空间还没跑起来时）需要一个独立的简单字体渲染器。Cinux 选择把字体渲染放在内核中，是因为我们的内核本身就是"单体的"——没有用户空间的概念。

## 验证

构建成功后，串口输出应该包含 `[BIG] PSF2 font loaded: 8x16`。如果这条日志没出现或者字体尺寸是 0x0，说明头部解析失败——最可能的原因是 `.incbin` 没有正确嵌入数据。

你可以手动验证字形数据是否正确嵌入：

```bash
xxd assets/font.psf | head -3
# 前 4 字节应该是 72 B5 4A 86（小端序的 PSF2 魔数）

nm build/big_kernel.elf | grep font_psf
# 应该能看到 font_psf_start、font_psf_end、font_psf_size 三个符号
```

## 收尾

到这里我们已经有了字体渲染能力——可以在屏幕上任意位置画出单个 ASCII 字符。但光有字体还不够，还需要一套"控制台"来管理文字的显示位置——从左到右、从上到下、自动换行、自动滚屏。这就是下一章的内容了。

回顾一下我们在这章建立的知识体系：首先理解了 PSF2 这种位图字体格式的结构（32 字节头部 + 紧凑的字节流字形数据），然后学会了用 `.incbin` 伪指令在编译时嵌入二进制资源（这是内核开发中非常常见的技巧），最后实现了一个逐像素渲染器把字形位图"画"到帧缓冲区上。这三个环节构成了内核文字显示的完整技术栈——从字体文件到屏幕像素。

值得强调的是，这套技术栈不仅适用于 PSF2 字体。`.incbin` 嵌入二进制资源的方法可以用于任何需要编译时打包的数据——图标、光标、配置表、初始 ramdisk 等。理解了这一个模式，你就掌握了内核开发中"没有文件系统时如何管理资源"这个通用问题的解法。

## 参考资料

- OSDev Wiki: [PC Screen Font](https://wiki.osdev.org/PC_Screen_Font) — PSF2 header 格式、字形位图编码、.incbin 嵌入方法
- Linux: `lib/fonts/font_8x16.c` 中硬编码了同样的 8x16 字形数据，内核通过 `fbcon` 子系统渲染到帧缓冲区控制台
- SerenityOS: `Userland/Libraries/LibGfx/Font.h` 将字体渲染抽象为独立的用户空间库，支持 TrueType、BitmapFont 等多种格式
- xv6: 使用 VGA 文本模式硬件字体，不需要软件渲染——CGA 控制器自动将字符编码转换为屏幕上的字形
- Linux: `lib/fonts/font_8x16.c` 中硬编码了同样的 8x16 字形数据，内核通过 `fbcon` 子系统渲染到帧缓冲区控制台
- Intel SDM: 本篇不涉及 SDM 内容，主要参考 OSDev Wiki 的 PSF2 格式文档
- GAS Manual: `.incbin` 伪指令的官方说明在 GNU Assembler 手册的 "Pseudo Ops" 章节
