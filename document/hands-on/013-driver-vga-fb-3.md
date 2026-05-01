# 013-3 文本控制台与 kprintf 双输出架构

## 导语

前两章我们把帧缓冲区和字体渲染都准备好了，现在需要一套"控制台"来管理文字的显示位置——从左到右、从上到下，到了行尾自动换行，到了屏幕底部自动滚屏。同时，我们还要重构 kprintf 的输出架构：从只写串口变成同时写串口和屏幕的"双输出"模式。完成这一章后，所有通过 kprintf 打印的内核日志都会同时出现在串口终端和 QEMU 窗口上——内核终于不再"盲打"了。

知识前置：前两章的 Framebuffer 和 PSFFont 必须已经就绪。你需要理解回调函数（callback）的概念，因为 kprintf 的多后端架构基于函数指针 + 上下文指针的模式。

## 概念精讲

### 文本控制台的状态机

一个最简的文本控制台本质上是一个维护光标状态（列号、行号）并处理特殊字符的有限状态机。每写一个字符：

- 如果是可打印字符：在当前位置渲染字形，列号加一。如果列号超过最大列数，换行。
- 如果是 `\n`：列号归零，行号加一。如果行号超过最大行数，滚屏。
- 如果是 `\r`：列号归零。
- 如果是 `\b`：列号减一（如果已经到行首则回到上一行末尾）。

最大列数和行数根据帧缓冲区尺寸和字体尺寸计算：`cols = fb_width / font_width`，`rows = fb_height / font_height`。对于 1024x768 的分辨率和 8x16 的字体，就是 128 列 x 48 行——比标准 VGA 文本模式的 80x25 大得多。

### kprintf 多后端（Sink）架构

原来的 kprintf 直接调用串口对象的 putc。现在我们要把它改成"广播"模式：维护一张 sink 表（最多 8 个），每个 sink 是一个函数指针 + 一个 void* 上下文指针。每次格式化输出一个字符时，遍历所有启用的 sink 并调用。串口是一个 sink，Console 是另一个 sink。

这种设计的好处是扩展性极强——将来加一个日志缓冲区 sink、网络 sink、或者 QEMU debug console sink（port 0xE9）都只需要调用注册函数，不需要改 kprintf 的核心逻辑。

回调函数的签名约定是 `void(char c, void* ctx)`：第一个参数是要输出的字符，第二个参数是在注册时传入的不透明指针。Console 的 sink adapter 把这个指针 cast 回 Console* 然后调用 putc。

## 动手实现

### Step 1: 重构 kprintf 为多后端架构

**目标**: 将 kprintf 的单串口输出改为支持注册多个输出后端的架构。

**设计思路**: 在 kprintf.hpp 中定义一个函数指针类型别名 `OutputSink`（`void(*)(char, void* ctx)`）和一个最大 sink 数量的编译期常量（8）。在 kprintf.cpp 中，用一个静态数组存储已注册的 sink（每个 sink 包含函数指针、上下文指针和启用标志），以及一个当前 sink 计数器。原来的全局 Serial 对象不再直接被 kprintf 使用，而是通过一个 adapter 函数包装成 OutputSink 格式，在 `kprintf_init()` 时注册为第一个 sink。

kprintf、kvprintf、kpanic 的核心逻辑不变，只是把原来直接调用 `g_serial.putc(c)` 的 lambda 改成遍历 sink 数组、对每个启用的 sink 调用其回调函数的 lambda。

**实现约束**: sink 注册函数 `kprintf_register_sink` 需要处理两种情况：如果数组中有空位（enabled=false 的条目），就复用那个位置；否则在末尾追加（不超过上限）。注册时要检查函数指针是否为 nullptr。sink 表和计数器放在匿名命名空间中（文件作用域静态变量）。

**踩坑预警**: kprintf_init() 必须在任何 kprintf 调用之前执行，而且必须先注册串口 sink——否则后续的初始化日志就没地方输出了。另外，kpanic 中遍历 sink 表时要考虑到 panic 可能发生在 sink 表还没初始化的时候，但这种情况在当前流程中不太可能出现（因为 kprintf_init 是最早的初始化步骤之一）。

**验证**: 修改后先不加 Console sink，只注册串口 sink，确认串口输出和之前完全一样——行为不能退化。

### Step 2: 实现 Console 类

**目标**: 创建一个文本控制台类，管理光标位置、处理控制字符、自动换行和滚屏。

**设计思路**: Console 类持有三个指针/引用：Framebuffer 指针、PSFFont 指针、以及前景色和背景色。初始化时从 Framebuffer 和 PSFFont 计算出 cols 和 rows，然后清屏。putc 方法按上面描述的状态机逻辑处理字符。scroll 方法调用 Framebuffer 的 scroll_up。Console 还提供一个静态方法作为 sink adapter，把 `(char, void* ctx)` 转换成对 Console 实例的 putc 调用。

**实现约束**: 类放在 `cinux::drivers` 命名空间。成员变量包括：Framebuffer 指针、PSFFont 指针、列号、行号、最大列数、最大行数、前景色、背景色。所有指针初始化为 nullptr。init 方法接收 Framebuffer 引用、PSFFont 引用、前景色和背景色四个参数。

putc 方法先检查 fb_ 和 font_ 是否为 nullptr（安全守卫），然后用 switch 处理三种控制字符（\n、\r、\b）和 default（可打印字符）。可打印字符的处理：先检查是否需要换行（col >= cols），然后调用 font 的 render_char 在当前位置渲染，最后 col++。

new_line 私有方法：col 归零，如果 row+1 >= rows 则调用 scroll，否则 row++。scroll 私有方法：调用 fb 的 scroll_up(font_height, font_height, bg_)。这里 scroll_up 的第一个参数是滚动像素行数（等于一个字符高度），第二个参数是底部需要清除的区域高度。

静态 sink adapter 方法：接收 char 和 void*，把 void* static_cast 为 Console*，非空则调用 putc。

**踩坑预警**: `col >= cols` 的判断要在渲染之前——如果当前列已经是最后一列的下一列（等于 cols），需要先换行再渲染。另外，backspace 只移动光标不擦除内容——这是标准行为，但如果你期望 backspace 擦除字符，需要额外处理。

**验证**: 编写测试用例——创建 Console，putc 一个 'X'，检查 (0,0) 到 (7,15) 范围内有白色像素。putc 'A' 再 putc 'B'，检查 'B' 的像素出现在 x=8 到 x=15 的范围。调用 clear 后检查所有像素都变黑。

### Step 3: 在 kernel_main 中集成双输出

**目标**: 在 kernel_main 中依次初始化 Framebuffer、PSFFont、Console，然后将 Console 注册为 kprintf 的第二个 sink。

**设计思路**: 初始化顺序是固定的：Framebuffer init -> PSFFont init -> Console init -> kprintf_register_sink(Console adapter)。这条链必须在中断使能之前完成，因为我们不想在初始化过程中被中断打断。Console 注册完成后，后续所有 kprintf 调用都会同时输出到串口和屏幕。

**实现约束**: Console 对象在 kernel_main 中声明为局部变量。使用 `Console::console_sink_adapter` 作为 sink 回调，`&console` 作为上下文指针。前景色使用白色（0x00FFFFFF），背景色使用黑色（0x00000000）。

**验证**: 启动 QEMU 后，`[BIG] Console initialised -- dual output active.` 这条消息之后的 kprintf 输出应该同时出现在串口和 QEMU 窗口上。QEMU 窗口应该显示白字黑底的内核启动日志。

### Step 4: 驱动目录重组

**目标**: 将 drivers 目录下的 pit 和 serial 各自放到子目录中（pit/ 和 serial/），video 相关的文件放到 video/ 子目录中。

**设计思路**: 随着驱动数量增长，平铺在 drivers/ 下会变得混乱。每个驱动一个子目录，内部包含自己的 .hpp 和 .cpp 文件。子目录内的文件使用相对 include（`#include "pit.hpp"` 而不是 `#include "kernel/drivers/pit.hpp"`）。

**实现约束**: 在 CMakeLists.txt 中更新所有文件路径。更新所有 include 引用（irq_handlers.cpp、main.cpp、test 文件等）。git mv 跟踪文件重命名。

**验证**: 构建成功，所有测试通过。

## 构建与运行

完整构建后启动 QEMU（使用之前相同的启动参数）。关键观察点：

- 串口终端：所有日志正常输出
- QEMU 窗口：从 Console 初始化完成那一刻起，日志同时出现在屏幕上
- PIT tick 消息（每秒一条）应该同时出现在串口和屏幕上，证明双输出持续工作

## 调试技巧

**屏幕有输出但乱码**: 字体渲染问题——回到第二章的调试方法，单独测试 PSFFont 的 render_char。

**Console 初始化后串口没输出了**: 检查 kprintf_register_sink 是否正确注册了 Console sink，以及 sink 表中的串口 sink 是否还在。如果在注册 Console 时不小心覆盖了串口 sink，就会丢失串口输出。

**滚屏后画面撕裂**: scroll_up 的字节拷贝逻辑可能有问题——检查 src 和 dst 的计算是否正确，move_bytes 是否等于 (height - lines) * pitch。

**颜色不对**: 确认 32bpp 的像素格式。在 Bochs/QEMU 中通常是 XRGB（0x00RRGGBB），alpha 通道被忽略。如果你写了 0xFF000000 作为 alpha，某些模拟器可能不会显示正确。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| Console 状态机 | col/row 光标 + \n/\r/\b 处理 + 自动换行 |
| 滚屏 | scroll_up(font_height) 用字节拷贝 + 底部清空 |
| kprintf Sink | OutputSink = void(*)(char, void*)，最多 8 个后端 |
| Sink 注册 | kprintf_register_sink + 静态 adapter 函数 |
| 双输出 | 串口 sink + Console sink，初始化顺序决定输出时机 |
| 驱动目录 | drivers/{pit,serial,video}/ 各自子目录 |
