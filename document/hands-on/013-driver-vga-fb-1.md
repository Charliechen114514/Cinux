# 013-1 底座：线性帧缓冲区与 MMIO 页表映射

## 导语

在上一章（012_driver_serial）中，我们搞定了串口驱动和 kprintf 的格式化输出，但内核到现在还只能通过串口 "盲打"——屏幕上什么都没有。这一章我们要把屏幕点亮：让内核获得对 VBE 线性帧缓冲区的直接控制权，能够在屏幕上画像素、填矩形、滚动画面。完成这一章后，你会看到 QEMU 窗口从一个黑屏变成可以编程控制像素的画布——虽然还不能写字，但那是下一章的事了。

这一章涉及两个关键问题：第一，bootloader 已经帮我们设好了 VBE 图形模式，但帧缓冲区的物理地址（通常是 0xE0000000 附近）不一定在内核的页表映射范围内，所以我们得自己扩展页表；第二，拿到映射后的地址之后，我们需要一套简洁的绘图原语来操作像素。

知识前置：你需要理解 x86_64 四级页表的基本结构（PDPT -> PD -> PT -> Page），以及 VBE 线性帧缓冲区的工作原理（上一章 bootloader 中的 VESA 调用已经把模式设好了）。如果不清楚大页映射的概念，建议先回顾 tag 003 中 bootloader 建立初始页表的部分——本篇的 `map_mmio` 函数就是在那套页表基础上添加新条目。

## 背景与动机

在 PC 的早期（VGA 时代），屏幕显示是通过文本模式实现的——显卡内置了一个字符发生器（character generator），你只需要往 0xB8000 处的显存写入字符编码和属性字节，硬件自动帮你把字符渲染成像素。这种模式非常方便，但分辨率被限制在 80x25 或者 132x50，而且不支持自定义字体和图形。

VBE（VESA BIOS Extensions）的出现改变了这一切——它允许软件把显卡切换到图形模式，直接操作一个线性帧缓冲区。在图形模式下，没有硬件字符发生器，没有自动换行，没有光标——一切都得自己实现。代价是更高的编程复杂度，好处是完全的自由度。

Cinux 选择图形模式而不是 VGA 文本模式，原因有三。第一，VGA 文本模式的分辨率太低（80x25），在 1024x768 的屏幕上显得非常粗糙。第二，文本模式只能显示 256 种预定义字符，无法渲染中文或自定义图形。第三，几乎所有现代 OS 都使用图形模式——学习图形模式的帧缓冲区编程更贴近实际。

## 与其他系统的对比

Linux 的帧缓冲区抽象（fbdev）比我们复杂得多——它支持多种像素格式（8bpp 调色板、16bpp RGB565、32bpp XRGB8888）、硬件加速操作（BitBLT、光标叠加）、以及双缓冲机制。Cinux 目前只支持 32bpp XRGB，但这个子集已经足够实现完整的控制台输出。

SerenityOS 的帧缓冲区驱动更接近 Cinux 的设计——直接通过 volatile 指针写入线性帧缓冲区，没有额外的抽象层。但 SerenityOS 的帧缓冲区是在 higher-half 的专用虚拟地址空间中映射的，而我们使用更简单的 identity mapping。

xv6 使用的是 VGA 文本模式（0xB8000），完全不需要页表映射和像素操作——CGA 控制器硬件自动处理字符渲染。这使得 xv6 的控制台代码非常简洁，但也限制了它只能显示 80x25 的文本。

## 概念精讲

### 线性帧缓冲区（Linear Framebuffer）

VBE 2.0 以后，显卡可以提供一个连续的物理内存区域作为帧缓冲区。在这块内存里，每一个像素的颜色值被存储为一个固定长度的整数——在我们的场景中是 32 位（0x00RRGGBB 格式，每个通道 8 位）。屏幕上第 x 列、第 y 行的像素在内存中的位置是 `帧缓冲区基地址 + y * pitch + x * 4`，其中 pitch 是每行占用的字节数。

这里有一个很容易踩的坑：pitch 不一定等于 `width * bytes_per_pixel`。有些显卡会在每行末尾添加填充字节来对齐到某个边界。所以你必须用 bootloader 传递的 pitch 值来计算偏移，而不是自己算。OSDev Wiki 的 VESA BIOS Extensions 页面专门强调了这一点——很多新手用自己的 width * 4 来代替 pitch，在大多数情况下碰巧能工作，但遇到对齐要求不同的硬件就会出问题。

另一个常见误解是像素格式的字节序。在 Bochs/QEMU 中，32bpp 的像素格式是 XRGB（0x00RRGGBB），最高字节未使用。这与某些 ARM 平台的 BGRX 格式不同。如果你把红色通道写到了蓝色通道的位置，画面不会黑屏但颜色会全错——这种 bug 比"完全不工作"更难发现。

### MMIO 映射与 Huge Page

帧缓冲区的物理地址通常在高地址区域（Bochs 模拟器默认用 0xE0000000，即 3.5GB 处）。我们的 bootloader 在进入 long mode 时设置了一套基本的页表，只映射了前几 MB 的物理内存。所以访问帧缓冲区之前，必须先在页表中添加对应的映射。

一个 1024x768x32bpp 的帧缓冲区大约占 3MB（1024 * 768 * 4 = 3,145,728 字节）。用 2MB 大页（Huge Page）来映射的话只需要 2 个 PDE 项。如果物理地址超过 1GB 边界，还可以用 1GB 大页来映射，但需要先检查 CPU 是否支持（CPUID.80000001H:EDX bit 26）。

大页映射的好处是简单——只需要填充一个 PDE 或 PDPTE 条目，不需要创建完整的四级页表。对于 MMIO 设备来说，这完全够用了。PDE 中的标志位 0x83 表示 Present + Read/Write + Page Size（2MB 大页）。

为什么选择大页而不是普通的 4KB 页？两个原因。第一是效率——一个大页条目替代了 512 个 PT 条目，减少了页表占用的内存。第二是 TLB 利用率——一个大页只占一个 TLB 条目，而覆盖同样范围需要 512 个 4KB TLB 条目。帧缓冲区是频繁访问的区域（每次渲染字符都要写像素），TLB 命中率对性能有直接影响。

还有一个经常被忽略的细节：identity mapping 的选择。我们的 `map_mmio` 函数使用 identity mapping（虚拟地址 = 物理地址），这意味着映射后的虚拟地址直接等于帧缓冲区的物理地址。这在内核开发初期很方便——不需要维护额外的虚拟地址分配器。但 Linux 不这么做，Linux 用专门的 `ioremap()` 函数把 MMIO 区域映射到 higher-half 的专用虚拟地址范围，好处是不会与直接映射区域冲突，也更容易区分"普通内存"和"设备内存"。

## 动手实现

### Step 1: 修改 VBE 模式号

**目标**: 将 bootloader 中的 VBE 模式从 0x118 切换到 0x144（Bochs VBE 变体），确保在 QEMU/Bochs 环境下兼容性更好。

**设计思路**: 模式号 0x118 和 0x144 都表示 1024x768x32bpp，但不同模拟器的 VBE 实现可能只支持其中一种。Bochs 原生支持 0x144。模式号与 bit14（0x4000）组合后得到传给 INT 0x10 AX=0x4F02 的 BX 值：0x4144。

bit14（0x4000）这个标志的含义是"请求线性帧缓冲区"——如果不设置这个位，显卡可能使用 banked 模式（窗口切换模式），那意味着你需要通过 VBE 的窗口切换接口来访问帧缓冲区的不同区域，编程复杂度急剧上升。所以在调用 VBE set mode 时，务必设置这个位。

**实现约束**: 在 bootloader 汇编文件中找到 VESA_TARGET_MODE 的定义位置和两个使用位置（vesa_get_mode_info 和 vesa_set_mode），将 0x0118/0x4118 替换为 0x0144/0x4144。三个位置必须保持一致。

**踩坑预警**: 如果你只改了一个地方没改另外两个，VBE 调用会成功但模式设置可能失败，屏幕黑屏但内核还在跑——串口有输出但画面空白，这种 bug 很难定位。建议用一个 `.set` 伪指令定义 `VESA_TARGET_MODE`，所有引用都通过这个符号名进行，避免手动同步三个数字。

**验证**: 修改后重新构建 bootloader 和内核，启动 QEMU。串口输出应该能看到 `[BIG] Framebuffer initialised: 1024x768 32bpp` 之类的信息。QEMU 窗口应该从文字模式变成黑色画面（因为我们还没画东西）。

### Step 2: 实现页表映射函数

**目标**: 编写一个 `map_mmio` 函数，能将任意物理地址范围映射到虚拟地址空间中（利用已有的 identity mapping）。

**设计思路**: 我们的 bootloader 已经设置了 identity mapping（物理地址 = 虚拟地址）和 higher-half mapping。页表的虚拟地址是已知的常量（PD 在 0xFFFFFFFF80003000，PDPT 在 0xFFFFFFFF80002000）。函数需要：

1. 对于 1GB 以内的物理地址范围，填充 PD 中对应的 PDE 条目（2MB 大页，flags = 0x83）。
2. 对于 1GB 以上的范围，填充 PDPT 中对应的 PDPTE 条目（1GB 大页，flags = 0x83），但先检查 CPU 是否支持 1GB 页。
3. 每填充一个 PDE 后调用 `invlpg` 刷新 TLB；如果修改了 PDPT 则重新加载 CR3。

需要两个辅助函数：一个用 CPUID 检查 1GB 页支持，一个重新加载 CR3 刷新全部 TLB。所有涉及的页表虚拟地址通过命名空间常量定义。

对于帧缓冲区来说，Bochs 默认的物理地址 0xE0000000（3.5GB）超出了 PD 覆盖的第一个 1GB 范围，所以会走第二部分的 1GB 大页映射路径。映射一个 1GB 大页条目就够了——帧缓冲区只有 3MB，远小于 1GB。

**实现约束**: 函数签名为接收物理基地址和大小两个参数，返回 void。命名空间为 `cinux::arch`。PD 和 PDPT 的虚拟地址作为编译期常量定义在匿名命名空间中。PDE flags 使用 0x83（Present + R/W + PS），PDPT 1GB flags 同样是 0x83。每个 PDE 条目需要检查是否为空（避免覆盖已有映射）。函数不分配新页——只填充已有页表结构中的空条目。

物理地址在计算 PDE 索引之前需要对齐到 2MB 边界——因为大页必须对齐到自身大小。做法是 `phys & ~(PAGE_2MB_SIZE - 1)`，把低位清零。类似地，1GB 大页需要对齐到 1GB 边界。

**踩坑预警**: 千万别覆盖 PDPT[0]，因为那个条目指向我们的 PD，覆盖了等于把所有低地址映射全部干掉。还有，`invlpg` 指令的参数是一个地址（不是页号），它 invalidate 包含该地址的 TLB 条目。用 `__asm__ volatile("invlpg (%0)" : : "r"(addr))` 的写法。

**验证**: 在 kernel_main 中调用 `map_mmio(fb_phys, fb_size)` 后，尝试通过物理地址指针写入一个像素值，然后用 `get_pixel` 读回来验证。或者等 Step 3 的 framebuffer 测试。

### Step 3: 实现 Framebuffer 类

**目标**: 创建一个 `Framebuffer` 类，提供基本的绘图原语。

**设计思路**: Framebuffer 类在 `init` 时从 BootInfo 获取帧缓冲区的物理地址、宽度、高度、pitch 和 bpp。然后调用 `map_mmio` 映射这片物理内存。之后所有绘图操作直接通过一个 volatile 指针写入。内部存储四个成员变量：基地址指针（volatile uint32_t*）、宽度、高度、pitch。

`init` 方法中帧缓冲区总大小的计算公式是 `pitch * height`，不是 `width * 4 * height`。这是因为 pitch 是"每行的实际字节数"，它可能大于 `width * 4`（如果行尾有对齐填充）。用 width 来计算会少映射一部分内存，访问行尾填充区域时触发 page fault。

**实现约束**: 类放在 `cinux::drivers` 命名空间中。需要提供以下公开方法：

- `init(const BootInfo&)`: 从 BootInfo 提取参数并调用 map_mmio 映射帧缓冲区
- `put_pixel(uint32_t x, uint32_t y, uint32_t argb)`: 写入单个像素，需要做边界检查
- `get_pixel(uint32_t x, uint32_t y) const`: 读取单个像素用于验证，同样做边界检查
- `fill_rect(x, y, w, h, argb)`: 填充矩形区域，双层循环
- `scroll_up(lines, line_height, bg)`: 向上滚动指定像素行数，底部的空行用 bg 色填充
- `clear(argb = 0)`: 清屏

关键计算公式：像素地址 = `addr_[y * (pitch_ / 4) + x]`，注意 pitch 是字节数所以除以 4 得到 uint32_t 的索引。scroll_up 用字节级别的逐字节拷贝（因为源和目标都是 volatile 指针，不能直接用 memmove），底部空行用 fill_rect 填充。

scroll_up 的两个参数需要特别注意语义：第一个参数 `lines` 是实际要滚动的像素行数，第二个参数 `line_height` 是底部要清空的区域高度。在控制台场景中，这两个值通常相等（都是字体高度），但分开设计提供了灵活性——比如你想滚动半行然后只清除半行。

**踩坑预警**: 帧缓冲区指针必须声明为 `volatile uint32_t*`，否则编译器可能优化掉看似"无用的"写入操作。另外，`pitch_ / 4` 而不是 `width_`——这是一个经典的 bug 来源。

**验证**: 编写测试代码（内核内测试）：清屏到某个颜色（比如 0x00AAAAAA），然后读回 (0,0)、(512,384)、(1023,767) 三个像素，验证是否都是那个颜色。put_pixel 写入 (100,100) 一个红色像素，读回验证。fill_rect 画一个 10x10 的绿色矩形，检查内部和边界外的像素。

### Step 3.5: 编写帧缓冲区测试

**目标**: 在 `kernel/test/test_video.cpp` 中编写一组内核内测试，验证帧缓冲区的基本功能。

**设计思路**: 内核内测试在 QEMU 中运行，可以访问真实的 VBE 帧缓冲区。测试分为几个独立的用例：初始化验证（从 BootInfo 读取的参数是否符合预期）、单像素读写验证（写入一个像素然后读回，确认值一致）、角落像素验证（检查 (0,0) 和 (1023,767) 这两个边界位置）、矩形填充验证（检查内部像素是填充色、外部像素保持不变）、清屏验证（检查三个采样点）、滚动验证（在一个位置画标记，滚动后检查标记是否移动了正确的距离）。

**实现约束**: 测试文件需要 include framebuffer.hpp 和 boot_info.h。BootInfo 的物理地址常量定义为 0x7000。使用项目的 TEST_ASSERT_EQ 等宏。Framebuffer 和 PSFFont 声明为文件作用域的静态全局变量，供多个测试函数共享。每个测试函数独立运行，不依赖其他测试的状态。

**验证**: 所有测试通过，确认帧缓冲区的基本绘图操作工作正常。如果某个测试失败，根据失败信息定位具体是哪个绘图原语有问题。

### Step 3.6: 理解 BootInfo 结构体

**目标**: 理解 BootInfo 结构体中与帧缓冲区相关的字段含义和来源。

**设计思路**: BootInfo 是 bootloader 在 16 位实模式下通过 VBE 调用获取的信息，然后打包成一个结构体放在物理地址 0x7000。与帧缓冲区相关的字段有五个：`fb_addr`（帧缓冲区物理基地址，64 位）、`fb_width`（水平像素数）、`fb_height`（垂直像素数）、`fb_pitch`（每行字节数）、`fb_bpp`（每像素位数）。

这些数据来源于 VBE 的 ModeInfoBlock 结构体。bootloader 在调用 INT 0x10 AX=0x4F01（Get Mode Info）后，BIOS 返回一个结构体，其中 `PhysBasePtr` 字段就是帧缓冲区的物理地址，`XResolution`、`YResolution`、`LinBytesPerScanLine` 和 `BitsPerPixel` 分别对应宽高、pitch 和 bpp。bootloader 把这些值复制到 BootInfo 中传递给内核。

**踩坑预警**: BootInfo 的内存布局必须与 bootloader 的写入顺序完全一致。如果 BootInfo 中字段的偏移量与 bootloader 代码中的偏移量不匹配，读到的数据就会错位——比如 fb_addr 可能读成了 fb_width 的值。这种情况不会崩溃（因为值仍然在合理范围内），但帧缓冲区初始化一定会失败。

### Step 3.7: 理解 volatile 语义与编译器优化

**目标**: 理解为什么帧缓冲区指针必须声明为 volatile，以及编译器优化可能带来哪些问题。

**设计思路**: `volatile` 关键字告诉编译器："这个内存位置的值可能在任何时候被外部因素修改，不要对它的读写做任何优化"。对于帧缓冲区这种 MMIO 设备内存，这个语义至关重要。

编译器在没有 volatile 的情况下可能做的优化包括：把连续两次写入合并成一次（比如 put_pixel 两次写入同一个位置，编译器可能认为第一次写入是"无用的"就跳过了）、把写入后读取优化掉（编译器认为"刚写入的值肯定等于写入的值"就直接用寄存器里的值了）、把看似无用的写入完全删除（编译器认为这个地址没有人读取，写入就是浪费）。

这些优化在普通内存上是完全正确的——但帧缓冲区不是普通内存，它的"读取者"是显示器硬件。即使软件从不读取某个像素的值，写入操作仍然有副作用（屏幕上的像素改变了）。volatile 就是告诉编译器"每个写入操作都有副作用，不能省略"。

类似地，页表条目也是需要 volatile 的——因为页表的"读取者"是 CPU 的 MMU 硬件。如果你在页表中写入了一个新的 PDE 但编译器把这次写入优化掉了，MMU 永远看不到这个新条目，访问对应的虚拟地址就会触发 page fault。

**实现约束**: addr_ 的类型必须是 `volatile uint32_t*`（指向 volatile uint32_t 的指针），不能是 `uint32_t* volatile`（volatile 的指针指向普通的 uint32_t）——前者表示指向的内存是 volatile 的，后者只是指针本身是 volatile 的。我们需要的是前者。

### Step 4: 在 kernel_main 中集成

**目标**: 在 kernel_main 的初始化流程中，在串口/GDT/IDT/PIC/PIT 之后、中断使能之前初始化 Framebuffer。

**设计思路**: BootInfo 结构体被 bootloader 放在物理地址 0x7000 处。由于我们的 identity mapping 覆盖了这个地址，可以直接将其 reinterpret_cast 为 `const BootInfo*` 来使用。Framebuffer 作为 kernel_main 的局部变量声明——它的生命周期就是内核的整个运行周期（因为 kernel_main 永远不会返回）。

**实现约束**: 初始化顺序是固定的——Framebuffer 必须在 kprintf_init 之后（因为我们用 kprintf 打印初始化日志），在 Console 之前（Console 依赖 Framebuffer）。具体位置是：在 breakpoint 测试之后、IRQ0 unmask 之前。需要将 BootInfo 物理地址定义为编译期常量（0x7000）。

**验证**: 启动 QEMU 后，串口输出应该包含 `[BIG] Framebuffer initialised: 1024x768 32bpp`。QEMU 窗口应该显示黑色画面（clear 默认填充 0）。如果 fb_addr 是 0 或者 bpp 是 0，说明 bootloader 的 VESA 调用失败了——回头看 bootloader 中 VESA mode info 的解析。

**进一步验证**: 编写内核内测试代码——清屏到某个颜色（比如 0x00AAAAAA），然后读回三个位置的像素：左上角 (0,0)、中心 (512,384)、右下角 (1023,767)。三个像素都应该返回你设置的颜色。然后 put_pixel 写入 (100,100) 一个红色像素 (0x00FF0000)，读回验证。检查 (99,100) 和 (101,100) 是否仍然是清屏颜色——确认单个像素写入不会影响相邻像素。

## 构建与运行

构建命令与之前相同。QEMU 启动后注意观察：窗口标题应该显示分辨率，如果分辨率不对说明 VBE 模式设置出了问题。串口输出中关于 framebuffer 的信息是第一手的诊断数据。

需要注意的是，VBE 模式切换发生在 bootloader 阶段（16 位实模式下通过 INT 0x10 调用），内核此时已经在 64 位长模式下运行——没有办法再调用 BIOS 中断了。所以帧缓冲区的配置完全依赖于 bootloader 传递过来的 BootInfo 数据。如果 BootInfo 里的数据不对，内核侧没有补救手段。

## 调试技巧

**帧缓冲区黑屏但串口正常**: 先检查 map_mmio 是否真的被调用了（在 kprintf 中加日志），然后检查物理地址是否正确（从 BootInfo 读出来的 fb_addr 应该是类似 0xE0000000 的值）。如果 fb_addr 是 0，说明 bootloader 的 VESA 调用失败了。

**Triple fault 在 map_mmio 之后**: 大概率是覆盖了 PDPT[0] 或者 PDE flags 写错了。用 GDB 在 map_mmio 处设断点，检查每个写入的 PDE 值。特别关注 PDPT 条目——如果你不小心写入了 PDPT[0]，所有低地址映射全部失效，连串口都访问不了。

**像素位置不对**: 检查 pitch 是否正确。如果 pitch 为 0 或者是一个奇怪的值，说明 BootInfo 中的数据不正确——回头看 bootloader 中 VESA mode info 的解析。

**map_mmio 后帧缓冲区地址仍然触发 page fault**: 可能是页表虚拟地址的常量值与 bootloader 实际设置的地址不匹配。检查 bootloader 代码中 PD 和 PDPT 被映射到的虚拟地址，确保与 `paging.cpp` 中的常量一致。另外确认 bootloader 的 higher-half 映射覆盖了 PD 和 PDPT 本身——如果页表本身没有被映射，你连访问页表都做不到。

**volatile 相关的诡异问题**: 如果你能正常写入像素但读取时得到错误值（或者写入似乎没有生效），检查 addr_ 是否声明为 volatile uint32_t*。没有 volatile 的话，编译器在 -O2 以上可能会把"看起来无用"的写入操作优化掉。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 线性帧缓冲区 | 物理地址 + y*pitch + x*4，pitch 可能大于 width*4 |
| 2MB Huge Page | PDE flags 0x83，覆盖 2MB 物理内存 |
| 1GB Huge Page | PDPTE flags 0x83，需要 CPUID 检查支持 |
| volatile 指针 | MMIO 必须用 volatile 防止编译器优化 |
| VBE 模式号 | 0x144 + bit14(0x4000) = 0x4144 for Bochs |
| identity mapping | 虚拟地址 = 物理地址，简单但不利于长期维护 |

## 扩展方向

- **双缓冲（Double Buffering）**: 分配一块与帧缓冲区同样大小的后备缓冲区，所有绘制操作先写到后备缓冲区，完成后一次性拷贝到前端。这能消除画面撕裂（tearing）现象——当滚动和绘制同时进行时，屏幕上半部分是旧内容、下半部分是新内容。难度：⭐⭐
- **硬件加速 BitBLT**: 如果模拟器支持 Bochs VBE Extensions 的虚拟显存操作，可以利用 `VBE_DISPI_INDEX_WIN` 等寄存器加速矩形填充和位块传输，避免逐像素的软件渲染。难度：⭐⭐⭐
- **多分辨率支持**: 从 BootInfo 动态读取分辨率和 bpp，支持 640x480、800x600、1920x1080 等多种模式。当前的 Framebuffer 类已经通过 BootInfo 参数化，理论上不需要改代码就能支持不同分辨率——但 Console 的行列数会随之变化。难度：⭐

## 参考资料

- Intel SDM: Vol.3A Section 4.5 (4-Level Paging) -- 2MB page PDE format (Table 4-18), 1GB page PDPTE format (Table 4-16)
- Intel SDM: Vol.3A Section 4.1.4 -- 1GB page support via CPUID.80000001H:EDX bit 26
- OSDev Wiki: VESA BIOS Extensions -- ModeInfoBlock 字段说明、线性帧缓冲区
- Linux fbdev: `drivers/video/fbdev/core/fbmem.c` 中使用 `fix.line_length * var.yres` 计算帧缓冲区大小
- SerenityOS: `Kernel/Graphics/FramebufferConsole.h` 使用类似的 volatile 指针直接写入线性帧缓冲区
- xv6: 使用 VGA 文本模式（0xB8000），不需要页表映射和像素操作
