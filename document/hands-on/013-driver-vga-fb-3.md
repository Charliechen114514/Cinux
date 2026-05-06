# 013-3 文本控制台与 kprintf 双输出架构

## 导语

前两章我们把帧缓冲区和字体渲染都准备好了，现在需要一套"控制台"来管理文字的显示位置——从左到右、从上到下，到了行尾自动换行，到了屏幕底部自动滚屏。同时，我们还要重构 kprintf 的输出架构：从只写串口变成同时写串口和屏幕的"双输出"模式。完成这一章后，所有通过 kprintf 打印的内核日志都会同时出现在串口终端和 QEMU 窗口上——内核终于不再"盲打"了。

知识前置：前两章的 Framebuffer 和 PSFFont 必须已经就绪。你需要理解回调函数（callback）的概念，因为 kprintf 的多后端架构基于函数指针 + 上下文指针的模式。如果你对 C 语言中的 `qsort` 比较函数或者 pthread 的线程启动函数有了解，那么这个模式对你来说不会陌生——本质上都是"把一个函数指针和一个 void* 上下文打包传递"的做法。

## 背景与动机

控制台（Console）是操作系统中最基本的用户界面——在 GUI 出现之前，用户通过键盘输入命令、通过屏幕阅读输出，这就是控制台的全部功能。即使在有了 GUI 之后，内核的启动日志、调试信息、panic 消息仍然通过控制台输出——因为控制台是最可靠的输出通道，它不依赖复杂的驱动栈。

我们之前已经有了帧缓冲区（能画像素）和字体（能画字符），但这两者之间还缺少一个"粘合层"——一个管理文字在屏幕上排列方式的管理器。没有这个管理器，你可以手动在 (0,0) 位置画一个字符、在 (8,0) 位置画下一个字符……但手动跟踪光标位置、处理换行和滚屏，这些逻辑很容易出错。

kprintf 的多后端重构也有强烈的动机：原来的 kprintf 只写串口，但在有了屏幕输出之后，我们希望所有 kprintf 输出同时出现在两个地方。如果直接在 kprintf 里硬编码"同时调 Serial::putc 和 Console::putc"，那将来加 QEMU debug console（port 0xE9）又得改一遍——每加一个输出通道就改一次 kprintf 核心，这不是可持续的架构。多后端 Sink 架构一次解决这个问题。

## 与其他系统的对比

xv6 的 `console.c` 使用了一种更直接的方式来实现双输出——在 `consputc()` 函数中直接同时调用 `uartputc()` 和 `cgaputc()`。这种方式简单直接，但扩展性较差。Cinux 的多 Sink 架构在扩展性上更接近 Linux 的 printk + console_driver 链的设计。

Linux 的 printk 子系统通过 `console_driver` 链管理多个输出后端。每个 console 驱动注册一个 `write()` 回调，printk 格式化后的字符通过这些回调发送给各个输出设备。Linux 还支持日志级别、日志缓冲区（dmesg）、以及动态启用/禁用 console 驱动——这些都是 Cinux 目前不需要的复杂功能。

SerenityOS 的控制台功能最为完整——支持 ANSI 转义序列（颜色控制、光标定位、清屏）、多虚拟终端（类似 Linux 的 Ctrl+Alt+F1-F7）、光标闪烁、文本选区等。这些功能的实现需要更复杂的状态机（解析 ANSI 转义序列需要维护一个解析状态），是 Cinux 将来可以扩展的方向。

## 概念精讲

### 文本控制台的状态机

一个最简的文本控制台本质上是一个维护光标状态（列号、行号）并处理特殊字符的有限状态机。每写一个字符：

- 如果是可打印字符：在当前位置渲染字形，列号加一。如果列号超过最大列数，换行。
- 如果是 `\n`：列号归零，行号加一。如果行号超过最大行数，滚屏。
- 如果是 `\r`：列号归零。
- 如果是 `\b`：列号减一（如果已经到行首则回到上一行末尾）。

最大列数和行数根据帧缓冲区尺寸和字体尺寸计算：`cols = fb_width / font_width`，`rows = fb_height / font_height`。对于 1024x768 的分辨率和 8x16 的字体，就是 128 列 x 48 行——比标准 VGA 文本模式的 80x25 大得多。这意味着同样面积的屏幕能显示更多的文字，对于调试输出来说非常友好——一行能放 128 个字符，远远超过 80 列的终端宽度。

有几个容易搞错的细节需要特别注意。首先，`col >= cols` 的判断要在渲染之前——如果当前列已经是最后一列的下一列（等于 cols），需要先换行再渲染，否则字符会画到屏幕外面去。其次，`\b` 只移动光标不擦除内容——这是 Unix 终端的标准行为（退格后覆盖写入）。如果 col 已经在行首，退格会把光标移到上一行的最后一列——这种"折行退格"行为虽然不常见，但某些程序确实依赖它。

滚屏的逻辑也需要仔细思考。当光标已经在最后一行时，`new_line()` 不再递增 row——而是调用 `scroll()` 向上滚动一个字符高度（16 像素）。scroll() 不改变 row 的值，因为滚屏后光标仍然在最后一行，只是内容被向上推了一行。新行的内容通过后续的 putc() 渲染出来。这种设计确保了光标永远在有效范围内，不需要额外的"回绕"逻辑。

### kprintf 多后端（Sink）架构

原来的 kprintf 直接调用串口对象的 putc。现在我们要把它改成"广播"模式：维护一张 sink 表（最多 8 个），每个 sink 是一个函数指针 + 一个 void* 上下文指针。每次格式化输出一个字符时，遍历所有启用的 sink 并调用。串口是一个 sink，Console 是另一个 sink。

这种设计的好处是扩展性极强——将来加一个日志缓冲区 sink、网络 sink、或者 QEMU debug console sink（port 0xE9）都只需要调用注册函数，不需要改 kprintf 的核心逻辑。Linux 内核的 printk 也采用了类似的设计——格式化后的字符通过 console 驱动链发送到各种输出设备。

回调函数的签名约定是 `void(char c, void* ctx)`：第一个参数是要输出的字符，第二个参数是在注册时传入的不透明指针。Console 的 sink adapter 把这个指针 cast 回 Console* 然后调用 putc。

为什么用固定大小的静态数组而不是链表？kprintf 是内核最基础的调试工具之一，它的依赖越少越好。链表需要动态内存分配（或者静态节点池），增加了复杂度和潜在的故障点。固定数组简单、可靠、零分配。8 个位置的上限看起来很小，但仔细想想内核的输出后端不太可能超过这个数——串口、控制台、QEMU debug port、内核日志缓冲区、网络日志……就算全部加上也才 5 个。

注册逻辑分两步：先在已有条目中寻找空位（被禁用的 sink），找不到就在末尾追加。空指针检查是第一道防线——防止注册无效回调。sink 表中的 `enabled` 字段是为将来的"注销 sink"功能预留的——注销时只需把 enabled 设为 false，注册新 sink 时可以复用空位。

## 动手实现

### Step 1: 重构 kprintf 为多后端架构

**目标**: 将 kprintf 的单串口输出改为支持注册多个输出后端的架构。

**设计思路**: 在 kprintf.hpp 中定义一个函数指针类型别名 `OutputSink`（`void(*)(char, void* ctx)`）和一个最大 sink 数量的编译期常量（8）。在 kprintf.cpp 中，用一个静态数组存储已注册的 sink（每个 sink 包含函数指针、上下文指针和启用标志），以及一个当前 sink 计数器。原来的全局 Serial 对象不再直接被 kprintf 使用，而是通过一个 adapter 函数包装成 OutputSink 格式，在 `kprintf_init()` 时注册为第一个 sink。

kprintf、kvprintf、kpanic 的核心逻辑不变，只是把原来直接调用 `g_serial.putc(c)` 的 lambda 改成遍历 sink 数组、对每个启用的 sink 调用其回调函数的 lambda。格式化引擎（vkprintf_impl）本身完全不变——它只负责把格式字符串和参数转换成字符序列，不关心这些字符最终去了哪里。

**实现约束**: sink 注册函数 `kprintf_register_sink` 需要处理两种情况：如果数组中有空位（enabled=false 的条目），就复用那个位置；否则在末尾追加（不超过上限）。注册时要检查函数指针是否为 nullptr。sink 表和计数器放在匿名命名空间中（文件作用域静态变量）。

**踩坑预警**: kprintf_init() 必须在任何 kprintf 调用之前执行，而且必须先注册串口 sink——否则后续的初始化日志就没地方输出了。另外，kpanic 中遍历 sink 表时要考虑到 panic 可能发生在 sink 表还没初始化的时候，但这种情况在当前流程中不太可能出现（因为 kprintf_init 是最早的初始化步骤之一）。

**验证**: 修改后先不加 Console sink，只注册串口 sink，确认串口输出和之前完全一样——行为不能退化。

### Step 2: 实现 Console 类

**目标**: 创建一个文本控制台类，管理光标位置、处理控制字符、自动换行和滚屏。

**设计思路**: Console 类持有三个指针/引用：Framebuffer 指针、PSFFont 指针、以及前景色和背景色。初始化时从 Framebuffer 和 PSFFont 计算出 cols 和 rows，然后清屏。putc 方法按上面描述的状态机逻辑处理字符。scroll 方法调用 Framebuffer 的 scroll_up。Console 还提供一个静态方法作为 sink adapter，把 `(char, void* ctx)` 转换成对 Console 实例的 putc 调用。

Console 存储 Framebuffer 和 PSFFont 的裸指针（不是引用），这是因为 Console 使用 `init()` 方法而非构造函数初始化——Cinux 的统一风格。裸指针允许用 nullptr 表示"未初始化"状态，引用做不到。所有指针初始化为 nullptr，确保在 `init()` 调用之前 `putc()` 会安全退出。

**实现约束**: 类放在 `cinux::drivers` 命名空间。成员变量包括：Framebuffer 指针、PSFFont 指针、列号、行号、最大列数、最大行数、前景色、背景色。所有指针初始化为 nullptr。init 方法接收 Framebuffer 引用、PSFFont 引用、前景色和背景色四个参数。

putc 方法先检查 fb_ 和 font_ 是否为 nullptr（安全守卫），然后用 switch 处理三种控制字符（\n、\r、\b）和 default（可打印字符）。可打印字符的处理：先检查是否需要换行（col >= cols），然后调用 font 的 render_char 在当前位置渲染，最后 col++。

new_line 私有方法：col 归零，如果 row+1 >= rows 则调用 scroll，否则 row++。scroll 私有方法：调用 fb 的 scroll_up(font_height, font_height, bg_)。这里 scroll_up 的第一个参数是滚动像素行数（等于一个字符高度），第二个参数是底部需要清除的区域高度。

静态 sink adapter 方法：接收 char 和 void*，把 void* static_cast 为 Console*，非空则调用 putc。

**踩坑预警**: `col >= cols` 的判断要在渲染之前——如果当前列已经是最后一列的下一列（等于 cols），需要先换行再渲染。另外，backspace 只移动光标不擦除内容——这是标准行为，但如果你期望 backspace 擦除字符，需要额外处理。

**验证**: 编写测试用例——创建 Console，putc 一个 'X'，检查 (0,0) 到 (7,15) 范围内有白色像素。putc 'A' 再 putc 'B'，检查 'B' 的像素出现在 x=8 到 x=15 的范围。调用 clear 后检查所有像素都变黑。

### Step 3: 在 kernel_main 中集成双输出

**目标**: 在 kernel_main 中依次初始化 Framebuffer、PSFFont、Console，然后将 Console 注册为 kprintf 的第二个 sink。

**设计思路**: 初始化顺序是固定的：Framebuffer init -> PSFFont init -> Console init -> kprintf_register_sink(Console adapter)。这条链必须在中断使能之前完成，因为我们不想在初始化过程中被中断打断。Console 注册完成后，后续所有 kprintf 调用都会同时输出到串口和屏幕。

Console、Framebuffer 和 PSFFont 都是 kernel_main 的局部变量——它们的生命周期就是内核的整个运行周期（因为 kernel_main 永远不会返回，最后是一个 `while(1) { hlt; }` 循环）。所以不需要担心对象被销毁后 sink 引用悬空的问题。

**实现约束**: Console 对象在 kernel_main 中声明为局部变量。使用 `Console::console_sink_adapter` 作为 sink 回调，`&console` 作为上下文指针。前景色使用白色（0x00FFFFFF），背景色使用黑色（0x00000000）。

**验证**: 启动 QEMU 后，`[BIG] Console initialised -- dual output active.` 这条消息之后的 kprintf 输出应该同时出现在串口和 QEMU 窗口上。QEMU 窗口应该显示白字黑底的内核启动日志。

### Step 3.5: 编写控制台测试

**目标**: 在 `kernel/test/test_video.cpp` 中添加控制台相关的测试用例。

**设计思路**: 控制台测试需要验证字符输出、换行、滚屏等功能。具体用例包括：创建 Console，putc 一个 'X'，检查 (0,0) 到 (7,15) 范围内有白色像素。putc 'A' 再 putc 'B'，检查 'B' 的像素出现在 x=8 到 x=15 的范围（说明光标自动移动了）。调用 clear 后检查所有像素都变黑（说明清屏工作正常）。

换行测试：连续 putc 足够多的字符使得光标到达行尾，验证自动换行是否触发。这需要 putc 128 个字符（一行 128 列），然后检查下一个字符是否出现在第二行。

滚屏测试：这比较难在单元测试中验证，因为需要写满整个屏幕（48 行）。一个简化的方法是直接调用 Console 的 scroll 方法（但它是 private 的），或者通过连续输出 128*48 个字符来触发滚屏。

**实现约束**: Console 测试需要先初始化 Framebuffer 和 PSFFont。Console 对象可以声明为测试函数的局部变量。测试之间需要调用 Console 的 clear 方法来重置状态。

**验证**: 所有控制台测试通过。如果某个测试失败，根据像素读回的结果判断是 Console 的哪个部分出了问题——光标移动、换行逻辑还是字体渲染。

### Step 3.6: 理解初始化顺序的重要性

**目标**: 理解为什么 kernel_main 中的初始化顺序不能随意调换。

**设计思路**: kernel_main 中的初始化链是一条严格的依赖链：kprintf_init -> GDT -> IDT -> PIC -> IRQ -> PIT -> Framebuffer -> PSFFont -> Console -> register sink -> unmask IRQ -> sti。每个步骤都依赖前面的步骤提供的功能。

kprintf_init 必须最先——因为后续所有初始化步骤都用 kprintf 打印日志。GDT 和 IDT 在 PIC 之前——因为中断处理需要正确的段描述符和中断描述符表。PIT 在 Framebuffer 之前——虽然两者没有直接依赖，但 PIT 初始化需要 IDT 中的 IRQ 处理程序已经注册。

Framebuffer 在 Console 之前——因为 Console 依赖 Framebuffer 来渲染字符。PSFFont 在 Console 之前——因为 Console 依赖 PSFFont 来渲染字形。Console sink 注册在 Console init 之后——因为注册需要一个已经初始化的 Console 对象。

中断使能（sti）在最后——因为一旦使能中断，PIT 的 IRQ0 就会开始触发。如果此时初始化还没完成，中断处理程序可能会访问尚未初始化的数据结构。

**踩坑预警**: 如果你把 Console 的初始化放在了 PIC::unmask(0) 之后（也就是中断使能之后），PIT 的 tick 中断会在 Console 初始化过程中触发。由于 PIT 的中断处理程序会调用 kprintf，而此时 Console 可能只初始化了一半，sink 表中的状态可能不一致——轻则输出乱码，重则 page fault。

### Step 3.7: 理解 Sink 适配器模式

**目标**: 深入理解 sink 适配器的设计模式和它的优势。

**设计思路**: 适配器模式（Adapter Pattern）的核心思想是：把一个接口转换成另一个接口，使得原本不兼容的类可以协同工作。在 kprintf 的多后端架构中，每个后端都有自己的接口（Serial 有 putc，Console 也有 putc），但 kprintf 需要一个统一的接口来调用它们。OutputSink（`void(*)(char, void* ctx)`）就是这个统一接口。

Serial 的适配器很简单——因为 Serial 不需要实例上下文（它是全局单例），所以适配器函数忽略 ctx 参数，直接调用全局 Serial 对象的 putc。Console 的适配器稍微复杂一点——因为 Console 不是全局单例，而是 kernel_main 中的局部变量，所以适配器需要通过 ctx 参数来获取 Console 实例的指针。

静态方法 `console_sink_adapter` 之所以能工作，是因为 C++ 的静态成员函数与普通函数有相同的调用约定——它们不隐含 this 指针，可以直接作为函数指针传递。非静态成员函数则不行，因为它隐含了 this 指针，调用约定不同。

这种"函数指针 + void* 上下文"的模式在 C 语言接口设计中极为常见。pthread_create 的线程启动函数（`void* (*)(void*)`）、qsort_r 的比较函数（额外参数通过 void* 传递）、Linux 内核的 file_operations 结构体——都使用了类似的模式。理解了这个模式，你在阅读其他操作系统的代码时会经常遇到。

**实现约束**: 适配器函数的签名必须完全匹配 OutputSink 类型——`void(char, void*)`。返回类型不能是 int 或 bool，参数顺序不能反。如果签名不匹配，编译器不会报错（因为函数指针可以隐式转换），但运行时调用时参数会错位，导致未定义行为。

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

值得注意的是，"双输出激活"这条消息（`[BIG] Console initialised -- dual output active.`）本身是通过 kprintf 打印的——在注册 Console sink 之前，它只会出现在串口上；注册之后的所有消息才会同时出现在两个输出上。你可以用这个分界点来确认双输出是否正常工作：如果这条消息之后的日志没有出现在屏幕上，说明 Console sink 注册有问题。

如果你想更直观地验证，可以在 Console 初始化之后加一行测试：`kprintf("Hello from Cinux!\nSecond line.\n")`。这行文字应该同时出现在串口和 QEMU 窗口上。

## 调试技巧

**屏幕有输出但乱码**: 字体渲染问题——回到第二章的调试方法，单独测试 PSFFont 的 render_char。如果字体渲染本身没问题但控制台输出的文字不对，检查 Console 的字符编码传递是否正确——render_char 接收的是 uint8_t，而 putc 接收的是 char，注意 static_cast 是否正确。

**Console 初始化后串口没输出了**: 检查 kprintf_register_sink 是否正确注册了 Console sink，以及 sink 表中的串口 sink 是否还在。如果在注册 Console 时不小心覆盖了串口 sink，就会丢失串口输出。kprintf_register_sink 中的"先找空位、再追加"逻辑就是为了避免这种情况——但如果 sink 表满了（8 个），新的注册会被静默忽略。

**滚屏后画面撕裂**: scroll_up 的字节拷贝逻辑可能有问题——检查 src 和 dst 的计算是否正确，move_bytes 是否等于 (height - lines) * pitch。还要注意 scroll_up 的参数语义：第一个参数是滚动的像素行数，第二个参数是底部需要清除的区域高度，两者在控制台场景中通常相等（都是字体高度），但分开设计提供了灵活性。

**颜色不对**: 确认 32bpp 的像素格式。在 Bochs/QEMU 中通常是 XRGB（0x00RRGGBB），alpha 通道被忽略。如果你写了 0xFF000000 作为 alpha，某些模拟器可能不会显示正确。Console 的默认前景色是白色（0x00FFFFFF），背景色是黑色（0x00000000）——如果你看到的是其他颜色，检查 init 调用时传入的参数是否正确。

**双输出后串口输出变慢了**: 这是正常的——因为每个字符现在要经过两个 sink 处理，Console 的 render_char 比 Serial 的 putc 慢得多（128 次 put_pixel 调用 vs 一次 I/O 端口写入）。如果性能是瓶颈，可以考虑只在 Console sink 中处理控制字符和可打印字符，跳过格式化字符串中的重复内容。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| Console 状态机 | col/row 光标 + \n/\r/\b 处理 + 自动换行 |
| 滚屏 | scroll_up(font_height) 用字节拷贝 + 底部清空 |
| kprintf Sink | OutputSink = void(*)(char, void*)，最多 8 个后端 |
| Sink 注册 | kprintf_register_sink + 静态 adapter 函数 |
| 双输出 | 串口 sink + Console sink，初始化顺序决定输出时机 |
| 驱动目录 | drivers/{pit,serial,video}/ 各自子目录 |

## 扩展方向

- **ANSI 转义序列支持**: 实现 `\033[XXm` 颜色控制、`\033[2J` 清屏、`\033[H` 光标归位等基本的 ANSI/VT100 转义序列，使得内核日志可以有颜色和格式。这需要在 Console 的 putc 中添加一个转义序列解析状态机。难度：⭐⭐
- **QEMU Debug Console Sink**: 向 I/O port 0xE9 写入字符（QEMU 的 `-debugcon` 选项），作为第三个 kprintf sink。这在调试时非常有用，因为它不受串口速率限制，而且输出到宿主机的终端。难度：⭐
- **环形日志缓冲区 Sink**: 把 kprintf 输出存入一个固定大小的环形缓冲区，供 dmesg-like 工具在后续读取。这种设计在 Linux 内核中也有对应的实现（printk 的 log_buf）。难度：⭐⭐

## 参考资料

- Linux: `kernel/printk/printk.c` 中的 printk 实现通过 `console_driver` 链管理多个输出后端
- xv6: `console.c` 的 `consputc()` 同时调用 `uartputc()` 和 `cgaputc()` 实现双输出
- SerenityOS: `Kernel/TTY/VirtualConsole.cpp` 实现了更完整的控制台功能，包括 ANSI 转义序列和多虚拟终端
- Intel SDM: Vol.3A Section 4.5 -- 页表操作相关参考
