# 013-2 通读：字模引擎——PSF2 位图字体解析与渲染

## 概览

本文是 tag `013_driver_vga_fb` 三篇通读教程的第二篇。上一篇我们搞定了帧缓冲区的像素读写能力，这一篇要在像素之上构建"文字"能力——把 PSF2 格式的位图字体嵌入内核二进制，解析头部结构，然后逐像素地把字形渲染到帧缓冲区上。

本篇涉及三个文件：`gen_psf_font.py`（字体生成脚本）、`font_data.S`（GAS 汇编嵌入二进制数据）和 `font.cpp`（PSF2 解析 + 字形渲染）。其中字体生成脚本虽然不是内核代码，但它决定了字体文件的结构和内容，是理解整个流程的关键一环。

## 架构图

```
编译时流程:
  gen_psf_font.py  ──►  assets/font.psf (4128 bytes)
                              │
  font_data.S (.incbin)  ◄────┘
      │
      ├── font_psf_start (符号)
      ├── font_psf_end   (符号)
      └── font_psf_size  (符号)
              │
              ▼
  链接到内核 ELF 的 .rodata 段

运行时流程:
  PSFFont::init()
      │
      ├── reinterpret font_psf_start → PSF2Header*
      ├── 验证 magic == 0x864AB572
      ├── 提取 length, charsize, width, height
      └── glyphs_ = font_psf_start + header_size

  PSFFont::render_char(fb, 'A', x, y, fg, bg)
      │
      ├── glyph = glyphs_ + c * bytes_per_glyph_
      ├── for row in 0..height-1:
      │       bits = glyph[row]
      │       for col in 0..width-1:
      │               on = (bits >> (7 - col)) & 1
      │               fb.put_pixel(x+col, y+row, on ? fg : bg)
      └──

  字形位图示意 (字母 'A', 8x16):
  Row 0:  0x38 →  ●●●○○○○○  (MSB = 左边像素)
  Row 1:  0x6C →  ○●●○●●○○
  Row 2:  0xC6 →  ●●○○○●●○
  ...
  Row 8:  0xC6 →  ●●○○○●●○
```

## 代码精讲

### gen_psf_font.py —— 字体生成脚本

这个 Python 脚本负责生成一个 8x16 像素、256 字符的 PSF2 字体文件。我们来看关键部分。

```python
PSF2_MAGIC = 0x864AB572
PSF2_VERSION = 0
PSF2_HEADER_SIZE = 32
PSF2_FLAGS = 0
PSF2_LENGTH = 256
PSF2_CHARSIZE = 16    # 16 rows * 1 byte/row
PSF2_HEIGHT = 16
PSF2_WIDTH = 8
```

PSF2 格式的头部有 8 个 uint32_t 字段，共 32 字节。几个关键参数的含义：`FLAGS = 0` 表示没有 Unicode 映射表（每个字形的位置就是其编码值，字形 0x41 就是 'A'）；`LENGTH = 256` 表示共 256 个字形（覆盖整个 8 位字符集）；`CHARSIZE = 16` 表示每个字形占 16 字节（8 像素宽 × 16 像素高，每行 1 字节）。

```python
FONT_DATA = [
    # 0x00 - 0x0F (控制字符，全部留空)
    [0x00,0x00,...,0x00],  # NUL
    ...
    # 0x20 space (空格)
    [0x00,0x00,...,0x00],
    # 0x21 !
    [0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00],
    ...
    # 0x41 A
    [0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00],
    ...
]
```

字形数据采用 IBM PC 经典的 8x16 位图字体（CP437 编码）。每个字形是一个 16 字节的列表，每个字节编码一行的 8 个像素：bit 7（最高位）是最左边的像素，bit 0（最低位）是最右边的像素。以感叹号 `!` 为例，`0x18`（二进制 `00011000`）表示第 3、4 列有像素——正好是感叹号竖线的位置。

0x80-0xFF 的高半区字形全部填充为空白——Cinux 目前不需要 CP437 的制表符和方块字符。

```python
with open(out_path, "wb") as f:
    f.write(struct.pack("<IIIIIIII",
        PSF2_MAGIC, PSF2_VERSION, PSF2_HEADER_SIZE, PSF2_FLAGS,
        PSF2_LENGTH, PSF2_CHARSIZE, PSF2_HEIGHT, PSF2_WIDTH))
    for glyph in FONT_DATA[:256]:
        f.write(bytes(glyph))
```

输出部分先写 32 字节的头部（小端序，`<` 前缀），然后依次写入 256 个字形。最终文件大小是 `32 + 256 * 16 = 4128` 字节。

### font_data.S —— 用 .incbin 嵌入二进制数据

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

这段汇编代码做的事情极其简单：把 `assets/font.psf` 文件的原始字节直接插入到 ELF 的 `.rodata`（只读数据）段中，并用三个全局符号标记起止位置和大小。

`.incbin` 是 GAS 的伪指令，接受一个文件路径，将文件内容原封不动地插入到当前位置。它的效果等价于在编译时把整个文件内容变成一系列 `.byte` 指令，但更高效且可读性更好。文件路径相对于汇编器的 include path——CMake 构建时会确保项目根目录在 include path 中。

三个导出符号的含义：`font_psf_start` 是字体数据的第一个字节地址，`font_psf_end` 是最后一个字节的下一个字节地址（C++ 的 end() 语义），`font_psf_size` 是一个 4 字节的整数，值为字体数据的总字节数。C++ 代码通过 `extern "C"` 声明这些符号来访问。

### font.hpp —— PSFFont 类声明

```cpp
namespace cinux::drivers {

class Framebuffer;

class PSFFont {
public:
    void init();
    void render_char(Framebuffer& fb, uint8_t c, uint32_t x, uint32_t y,
                     uint32_t fg, uint32_t bg);

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    const uint8_t* glyphs_ = nullptr;
    uint32_t bytes_per_glyph_ = 0;
    uint32_t num_glyphs_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

}  // namespace cinux::drivers
```

PSFFont 类的设计很克制——只有两个公开方法 `init()` 和 `render_char()`，加上两个 getter。前向声明 `class Framebuffer` 避免了头文件循环依赖。私有成员 `glyphs_` 指向字形数据的起始位置（跳过 PSF2 头部后的第一字节），其他四个成员存储从头部解析出来的字形参数。

### font.cpp —— PSF2 解析与字符渲染

```cpp
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

`extern "C"` 块声明了 font_data.S 中定义的三个符号。PSF2Header 结构体精确对应 PSF2 文件的 32 字节头部布局，用 `__attribute__((packed))` 确保编译器不会在字段之间插入填充字节——虽然在 x86_64 上全是 uint32_t 的结构体本身就天然对齐，但 packed 属性是一种防御性编程。

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

`init()` 的逻辑一目了然：把 `font_psf_start` 解释为 PSF2Header 指针，验证魔数，然后提取五个参数。`glyphs_` 的计算是 `font_psf_start + hdr->header_size`——跳过头部，直接指向第一个字形的起始位置。如果魔数不匹配（比如 `.incbin` 没有正确嵌入数据），函数直接返回，所有成员保持零值/空值，后续 `render_char()` 会因为 `glyphs_ == nullptr` 而安全退出。

这里用 `hdr->header_size` 而不是硬编码 32，是因为 PSF2 格式允许头部有扩展字段（通过增加 header_size 的值）。虽然 Cinux 生成的字体头部就是标准的 32 字节，但这种写法兼容了第三方 PSF2 字体。

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

`render_char()` 是字形渲染的核心。输入是字符编码 `c`、像素坐标 `(x, y)`、前景色和背景色。首先做越界保护：字符编码超过字形总数时降级到字形 0（通常是空白或占位符）。然后计算目标字形的起始地址：`glyphs_ + c * bytes_per_glyph_`。

双重循环遍历字形的每一行和每一列。`glyph[row]` 取出当前行的 8 位位图，`(bits >> (7 - col)) & 1` 提取第 `col` 列的像素值。注意位移方向是 `7 - col` 而不是 `col`——因为 PSF2 格式中 bit 7 是最左边的像素。根据提取到的像素值，调用 Framebuffer 的 `put_pixel` 写入前景色或背景色。

这意味着每个字符的渲染需要调用 `width * height = 8 * 16 = 128` 次 `put_pixel`。对于控制台文本输出来说，这个开销完全可接受。如果将来需要渲染大量文本（比如 cat 一个大文件），可以考虑用 `fill_rect` 先画背景再逐像素写前景来减少函数调用开销。

## 设计决策

### 决策：.incbin vs. objcopy

**问题**: 如何把二进制字体文件嵌入内核 ELF？

**本项目的做法**: 使用 GAS 的 `.incbin` 伪指令，通过一个 .S 汇编文件把二进制文件嵌入 `.rodata` 段。

**备选方案**: 使用 `objcopy --input-target=binary --output-target=elf64-x86-64` 把 .psf 文件转成 .o 目标文件，然后链接到内核。

**为什么不选备选方案**: `.incbin` 方案只需要一个 .S 文件，符号命名完全由我们控制（`font_psf_start/end/size`），而 objcopy 自动生成的符号名基于文件路径（`_binary_assets_font_psf_start`），既丑又不稳定。此外，`.incbin` 在 CMake 中的集成更简单——只需把 .S 文件加到源文件列表中。

**如果要扩展**: 如果将来需要嵌入多个二进制资源（图标、光标、多套字体），可以考虑写一个 CMake 自定义命令自动生成 .S 文件。

### 决策：Python 脚本生成 vs. 直接使用系统字体

**问题**: 字体数据从哪里来？

**本项目的做法**: 用 Python 脚本硬编码了 IBM PC 8x16 位图字形的字节值，生成 PSF2 文件。

**备选方案**: 直接从 Linux 系统的 `/usr/share/consolefonts/` 目录复制一个 .psf 文件（比如 `Lat2-Terminus16.psf`）。

**为什么不选备选方案**: 硬编码字形数据虽然代码量大，但保证了教程的自包含性和可复现性——不依赖宿主系统的字体文件。此外，脚本只包含 ASCII 可打印字符的字形，体积小且易于理解。

**如果要扩展**: 可以添加命令行参数让脚本从 Linux 字体文件中提取字形，或者支持 Unicode 映射表以显示中文。

## 扩展方向

- **支持 PSF1 格式**：PSF1 的头部只有 4 字节，是更简单的字体格式。添加 PSF1 兼容性解析，可以直接使用更多的现有字体文件。难度：⭐
- **运行时字体加载**：将来有了文件系统后，从磁盘加载字体文件而不是编译时嵌入，支持用户自定义字体。难度：⭐⭐
- **抗锯齿渲染**：对于高分辨率显示器，可以用子像素渲染或灰度抗锯齿来改善文字显示质量。需要支持 alpha 混合的帧缓冲区操作。难度：⭐⭐⭐

## 参考资料

- OSDev Wiki: [PC Screen Font](https://wiki.osdev.org/PC_Screen_Font) — PSF2 header format, glyph bitmap encoding, embedding with objcopy/incbin
- Linux: 内核控制台使用 PSF2 作为默认字体格式，通过 `setfont` 命令在运行时加载
- SerenityOS: LibGfx::Font 分离了字体渲染到用户空间，支持多格式（ Typeface、BitmapFont）
