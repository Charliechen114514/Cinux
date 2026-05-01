# 013-2 字模引擎：PSF2 位图字体解析与渲染

## 导语

上一章我们把帧缓冲区搞定了——可以画像素、填矩形了。但光有像素还不够，我们要在屏幕上显示文字，就需要一套字体。这一章要做的是：把一个 PSF2 格式的位图字体嵌入内核二进制，然后解析它的头部结构，逐像素地把字形渲染到帧缓冲区上。完成这一章后，你会能够在屏幕上的任意位置渲染单个 ASCII 字符——虽然还不能自动换行和滚动，但"能写字"这个关键能力就算具备了。

知识前置：上一章的 Framebuffer 类必须已经可用（put_pixel 工作正常），你需要理解位图字体的基本概念（每个字符是一个像素矩阵，1 表示前景色，0 表示背景色）。

## 概念精讲

### PSF2 字体格式

PSF2（PC Screen Font version 2）是 Linux 内核控制台使用的位图字体格式。它的结构非常简单：一个 32 字节的头部，后面跟着所有字形（glyph）的位图数据。头部包含魔数（0x864AB572）、字形数量、每个字形占多少字节、字形的宽和高等信息。

对于 8x16 的字体来说，每个字形占 16 字节（16 行，每行 1 字节恰好编码 8 个像素）。第 0 个字形对应字符编码 0，第 1 个对应编码 1，依此类推——如果没有 Unicode 映射表的话。字形的第 N 个字节表示第 N 行的像素分布：最高位（bit 7）是最左边的像素，最低位（bit 0）是最右边的像素。

### 内核嵌入二进制数据的方法

内核没有文件系统（至少现在还没有），所以不能在运行时加载字体文件。标准做法是用汇编器的 `.incbin` 伪指令把二进制文件直接嵌入到 ELF 的 `.rodata` 段中。GAS（GNU Assembler）会自动生成 `_start`、`_end`、`_size` 三个符号，C++ 代码通过 `extern "C"` 声明这些符号就能访问嵌入的数据了。

另一种方案是用 `objcopy` 把 .psf 文件转成 .o 目标文件再链接——Cinux 选择 `.incbin` 方案，因为它更直观，只需要一个 .S 文件。

## 动手实现

### Step 1: 生成 PSF2 字体文件

**目标**: 用 Python 脚本生成一个 8x16、256 字符的 PSF2 字体文件。

**设计思路**: IBM PC 的经典 8x16 位图字体（CP437 编码）是公开可用的。我们用 Python 脚本将字形数据打包成 PSF2 格式：先写 32 字节的头部（magic=0x864AB572, version=0, headersize=32, flags=0, numglyph=256, bytesperglyph=16, height=16, width=8），然后写 256*16 = 4096 字节的字形数据。前 128 个字符（0x00-0x7F）是标准的 ASCII 字形，后 128 个留空。

**实现约束**: 脚本用 Python 3 编写，接受一个可选的输出路径参数（默认 assets/font.psf）。使用 `struct.pack` 以小端序写入头部。字形数据用一个列表的列表表示：外层 256 个元素，每个元素是 16 个字节的列表。0x20（空格）到 0x7E（波浪号）之间的可打印 ASCII 字符需要包含正确的字形数据。

**踩坑预警**: 字形数据中的每个字节代表一行像素，bit 7 是最左边的像素。如果搞反了（把 bit 0 当最左边），渲染出来的文字会是水平镜像的。

**验证**: 运行脚本后检查输出文件大小：应该是 32 + 256*16 = 4128 字节。用 `xxd assets/font.psf | head -3` 检查头部前 4 字节应该是 `72 B5 4A 86`（小端序的魔数）。

### Step 2: 用 .incbin 嵌入字体数据

**目标**: 编写一个汇编文件，用 GAS 的 `.incbin` 伪指令将 PSF2 字体文件嵌入内核。

**设计思路**: 在 `.rodata` 段中声明三个全局符号：`font_psf_start`（字体数据起始地址）、`font_psf_end`（字体数据结束地址）、`font_psf_size`（4 字节的大小值）。`.incbin` 指令直接将二进制文件的内容插入到当前位置。

**实现约束**: 文件放在 `kernel/drivers/video/font_data.S`。使用 `.section .rodata` 将数据放在只读段。汇编器的 include path 必须包含项目根目录，这样 `.incbin "assets/font.psf"` 才能正确找到文件。`.type` 伪指令标注符号类型为 @object。C++ 代码通过 `extern "C"` 声明这些符号为 `const uint8_t font_psf_start[]` 和 `const uint32_t font_psf_size[]`。

**踩坑预警**: 如果 CMake 没有正确设置汇编器的 include path，`.incbin` 会在编译时报 "file not found"。需要在 CMakeLists.txt 中为目标添加包含项目根目录的汇编 include 目录。

**验证**: 构建成功后，用 `nm` 或 `readelf` 检查内核 ELF 中是否存在 `font_psf_start` 和 `font_psf_size` 符号。

### Step 3: 实现 PSFFont 类

**目标**: 编写字体解析和字符渲染类。

**设计思路**: PSFFont 类在 `init()` 时解析嵌入的 PSF2 头部，提取字形数量、每个字形的字节数、宽度和高度，然后计算字形数据的起始指针（头部大小偏移之后）。`render_char()` 方法接收一个 Framebuffer 引用、字符编码、像素坐标和前景/背景色，逐行逐列地读取字形位图并调用 Framebuffer 的 put_pixel 写入像素。

**实现约束**: 类放在 `cinux::drivers` 命名空间。内部成员包括字形数据指针（const uint8_t*）、每个字形字节数、字形总数、宽度和高度。头部结构体用 `__attribute__((packed))` 标注，因为它需要精确匹配文件布局。魔数验证使用常量 0x864AB572。

`render_char` 方法的核心逻辑：对于超出范围的字符编码，降级到字形 0（通常是一个空格或占位符）。字形指针 = 字形数据基址 + 字符编码 * 每字形字节数。双重循环：外层遍历行（0 到 height-1），内层遍历列（0 到 width-1）。对于每一行的每一列，测试字形字节中的对应位（`bits >> (7 - col) & 1`），根据结果写入前景色或背景色。

需要前向声明 Framebuffer 类（因为头文件中只有引用，不需要完整定义）。

**踩坑预警**: PSF2 头部结构体必须在 `.cpp` 文件中定义（不是头文件），因为它是内部实现细节。字节序问题：PSF2 是小端序格式，在 x86_64 上直接读取 uint32_t 就行，不需要做字节序转换。

**验证**: 在 kernel_main 中初始化 PSFFont 后，检查 `width()` 返回 8、`height()` 返回 16。然后渲染字符 'A' 到 (0,0) 位置（白色前景、黑色背景），读取 (0,0) 到 (7,15) 范围内的像素，验证既存在白色像素（字形的前景部分）也存在黑色像素（背景部分）。

### Step 4: 在 CMakeLists.txt 中注册新文件

**目标**: 将 font.cpp 和 font_data.S 添加到内核构建目标中。

**实现约束**: 在 `kernel/CMakeLists.txt` 的 `big_kernel_common` 库中添加这两个文件。font_data.S 作为汇编源文件添加。确保汇编器的 include path 包含项目根目录（CMake 的 `target_include_directories` 或 `set_source_files_properties`）。

**验证**: `cmake --build build` 成功，没有 "undefined reference to font_psf_start" 之类的链接错误。

## 构建与运行

构建后启动 QEMU。此时内核初始化 PSF2 字体后应该打印 `[BIG] PSF2 font loaded: 8x16`。如果这条日志没出现或者字体尺寸是 0x0，说明头部解析失败——最可能的原因是 `.incbin` 没有正确嵌入数据。

## 调试技巧

**字体魔数不匹配**: 在 init 中加一个 kprintf 打印读到的 magic 值。如果不是 0x864AB572，说明数据没有正确嵌入或者指针算错了。

**字符渲染但画面全白或全黑**: 检查前景色和背景色的值是否搞反了——在位图测试中 `on ? fg : bg`，如果测试的是 `(bits >> col) & 1` 而不是 `(bits >> (7-col)) & 1`，字形会是镜像的。

**编译错误 "font_psf_start undeclared"**: 确认 font_data.S 被添加到了 CMakeLists.txt，并且 `extern "C"` 声明在 font.cpp 中。用 `nm` 检查 .o 文件中的符号名。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| PSF2 格式 | 32 字节头部 + N * bytesperglyph 字形数据 |
| 魔数 | 0x864AB572（小端序存储） |
| .incbin | GAS 伪指令，直接嵌入二进制文件到 .rodata 段 |
| 字形渲染 | 逐行读取字节，bit 7 = 最左像素 |
| 8x16 字体 | 每字形 16 字节，256 字形共 4096 字节 |
