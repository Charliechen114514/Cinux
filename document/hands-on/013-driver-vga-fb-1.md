# 013-1 底座：线性帧缓冲区与 MMIO 页表映射

## 导语

在上一章（012_driver_serial）中，我们搞定了串口驱动和 kprintf 的格式化输出，但内核到现在还只能通过串口 "盲打"——屏幕上什么都没有。这一章我们要把屏幕点亮：让内核获得对 VBE 线性帧缓冲区的直接控制权，能够在屏幕上画像素、填矩形、滚动画面。完成这一章后，你会看到 QEMU 窗口从一个黑屏变成可以编程控制像素的画布——虽然还不能写字，但那是下一章的事了。

这一章涉及两个关键问题：第一，bootloader 已经帮我们设好了 VBE 图形模式，但帧缓冲区的物理地址（通常是 0xE0000000 附近）不一定在内核的页表映射范围内，所以我们得自己扩展页表；第二，拿到映射后的地址之后，我们需要一套简洁的绘图原语来操作像素。

知识前置：你需要理解 x86_64 四级页表的基本结构（PDPT -> PD -> PT -> Page），以及 VBE 线性帧缓冲区的工作原理（上一章 bootloader 中的 VESA 调用已经把模式设好了）。

## 概念精讲

### 线性帧缓冲区（Linear Framebuffer）

VBE 2.0 以后，显卡可以提供一个连续的物理内存区域作为帧缓冲区。在这块内存里，每一个像素的颜色值被存储为一个固定长度的整数——在我们的场景中是 32 位（0x00RRGGBB 格式，每个通道 8 位）。屏幕上第 x 列、第 y 行的像素在内存中的位置是 `帧缓冲区基地址 + y * pitch + x * 4`，其中 pitch 是每行占用的字节数。

这里有一个很容易踩的坑：pitch 不一定等于 `width * bytes_per_pixel`。有些显卡会在每行末尾添加填充字节来对齐到某个边界。所以你必须用 bootloader 传递的 pitch 值来计算偏移，而不是自己算。

### MMIO 映射与 Huge Page

帧缓冲区的物理地址通常在高地址区域（Bochs 模拟器默认用 0xE0000000，即 3.5GB 处）。我们的 bootloader 在进入 long mode 时设置了一套基本的页表，只映射了前几 MB 的物理内存。所以访问帧缓冲区之前，必须先在页表中添加对应的映射。

一个 1024x768x32bpp 的帧缓冲区大约占 3MB（1024 * 768 * 4 = 3,145,728 字节）。用 2MB 大页（Huge Page）来映射的话只需要 2 个 PDE 项。如果物理地址超过 1GB 边界，还可以用 1GB 大页来映射，但需要先检查 CPU 是否支持（CPUID.80000001H:EDX bit 26）。

大页映射的好处是简单——只需要填充一个 PDE 或 PDPTE 条目，不需要创建完整的四级页表。对于 MMIO 设备来说，这完全够用了。PDE 中的标志位 0x83 表示 Present + Read/Write + Page Size（2MB 大页）。

## 动手实现

### Step 1: 修改 VBE 模式号

**目标**: 将 bootloader 中的 VBE 模式从 0x118 切换到 0x144（Bochs VBE 变体），确保在 QEMU/Bochs 环境下兼容性更好。

**设计思路**: 模式号 0x118 和 0x144 都表示 1024x768x32bpp，但不同模拟器的 VBE 实现可能只支持其中一种。Bochs 原生支持 0x144。模式号与 bit14（0x4000）组合后得到传给 INT 0x10 AX=0x4F02 的 BX 值：0x4144。

**实现约束**: 在 bootloader 汇编文件中找到 VESA_TARGET_MODE 的定义位置和两个使用位置（vesa_get_mode_info 和 vesa_set_mode），将 0x0118/0x4118 替换为 0x0144/0x4144。三个位置必须保持一致。

**踩坑预警**: 如果你只改了一个地方没改另外两个，VBE 调用会成功但模式设置可能失败，屏幕黑屏但内核还在跑——串口有输出但画面空白，这种 bug 很难定位。

**验证**: 修改后重新构建 bootloader 和内核，启动 QEMU。串口输出应该能看到 `[BIG] Framebuffer initialised: 1024x768 32bpp` 之类的信息。QEMU 窗口应该从文字模式变成黑色画面（因为我们还没画东西）。

### Step 2: 实现页表映射函数

**目标**: 编写一个 `map_mmio` 函数，能将任意物理地址范围映射到虚拟地址空间中（利用已有的 identity mapping）。

**设计思路**: 我们的 bootloader 已经设置了 identity mapping（物理地址 = 虚拟地址）和 higher-half mapping。页表的虚拟地址是已知的常量（PD 在 0xFFFFFFFF80003000，PDPT 在 0xFFFFFFFF80002000）。函数需要：

1. 对于 1GB 以内的物理地址范围，填充 PD 中对应的 PDE 条目（2MB 大页，flags = 0x83）。
2. 对于 1GB 以上的范围，填充 PDPT 中对应的 PDPTE 条目（1GB 大页，flags = 0x83），但先检查 CPU 是否支持 1GB 页。
3. 每填充一个 PDE 后调用 `invlpg` 刷新 TLB；如果修改了 PDPT 则重新加载 CR3。

需要两个辅助函数：一个用 CPUID 检查 1GB 页支持，一个重新加载 CR3 刷新全部 TLB。所有涉及的页表虚拟地址通过命名空间常量定义。

**实现约束**: 函数签名为接收物理基地址和大小两个参数，返回 void。命名空间为 `cinux::arch`。PD 和 PDPT 的虚拟地址作为编译期常量定义在匿名命名空间中。PDE flags 使用 0x83（Present + R/W + PS），PDPT 1GB flags 同样是 0x83。每个 PDE 条目需要检查是否为空（避免覆盖已有映射）。函数不分配新页——只填充已有页表结构中的空条目。

**踩坑预警**: 千万别覆盖 PDPT[0]，因为那个条目指向我们的 PD，覆盖了等于把所有低地址映射全部干掉。还有，`invlpg` 指令的参数是一个地址（不是页号），它 invalidate 包含该地址的 TLB 条目。用 `__asm__ volatile("invlpg (%0)" : : "r"(addr))` 的写法。

**验证**: 在 kernel_main 中调用 `map_mmio(fb_phys, fb_size)` 后，尝试通过物理地址指针写入一个像素值，然后用 `get_pixel` 读回来验证。或者等 Step 3 的 framebuffer 测试。

### Step 3: 实现 Framebuffer 类

**目标**: 创建一个 `Framebuffer` 类，提供基本的绘图原语。

**设计思路**: Framebuffer 类在 `init` 时从 BootInfo 获取帧缓冲区的物理地址、宽度、高度、pitch 和 bpp。然后调用 `map_mmio` 映射这片物理内存。之后所有绘图操作直接通过一个 volatile 指针写入。内部存储四个成员变量：基地址指针（volatile uint32_t*）、宽度、高度、pitch。

**实现约束**: 类放在 `cinux::drivers` 命名空间中。需要提供以下公开方法：

- `init(const BootInfo&)`: 从 BootInfo 提取参数并调用 map_mmio 映射帧缓冲区
- `put_pixel(uint32_t x, uint32_t y, uint32_t argb)`: 写入单个像素，需要做边界检查
- `get_pixel(uint32_t x, uint32_t y) const`: 读取单个像素用于验证，同样做边界检查
- `fill_rect(x, y, w, h, argb)`: 填充矩形区域，双层循环
- `scroll_up(lines, line_height, bg)`: 向上滚动指定像素行数，底部的空行用 bg 色填充
- `clear(argb = 0)`: 清屏

关键计算公式：像素地址 = `addr_[y * (pitch_ / 4) + x]`，注意 pitch 是字节数所以除以 4 得到 uint32_t 的索引。scroll_up 用字节级别的逐字节拷贝（因为源和目标都是 volatile 指针，不能直接用 memmove），底部空行用 fill_rect 填充。

**踩坑预警**: 帧缓冲区指针必须声明为 `volatile uint32_t*`，否则编译器可能优化掉看似"无用的"写入操作。另外，`pitch_ / 4` 而不是 `width_`——这是一个经典的 bug 来源。

**验证**: 编写测试代码（内核内测试）：清屏到某个颜色（比如 0x00AAAAAA），然后读回 (0,0)、(512,384)、(1023,767) 三个像素，验证是否都是那个颜色。put_pixel 写入 (100,100) 一个红色像素，读回验证。fill_rect 画一个 10x10 的绿色矩形，检查内部和边界外的像素。

### Step 4: 在 kernel_main 中集成

**目标**: 在 kernel_main 的初始化流程中，在串口/GDT/IDT/PIC/PIT 之后、中断使能之前初始化 Framebuffer。

**设计思路**: BootInfo 结构体被 bootloader 放在物理地址 0x7000 处。由于我们的 identity mapping 覆盖了这个地址，可以直接将其 reinterpret_cast 为 `const BootInfo*` 来使用。

**实现约束**: 初始化顺序是固定的——Framebuffer 必须在 kprintf_init 之后（因为我们用 kprintf 打印初始化日志），在 Console 之前（Console 依赖 Framebuffer）。具体位置是：在 breakpoint 测试之后、IRQ0 unmask 之前。需要将 BootInfo 物理地址定义为编译期常量（0x7000）。

**验证**: 启动 QEMU 后，串口输出应该包含 `[BIG] Framebuffer initialised: 1024x768 32bpp`。QEMU 窗口应该显示黑色画面（clear 默认填充 0）。

## 构建与运行

构建命令与之前相同。QEMU 启动后注意观察：窗口标题应该显示分辨率，如果分辨率不对说明 VBE 模式设置出了问题。串口输出中关于 framebuffer 的信息是第一手的诊断数据。

## 调试技巧

**帧缓冲区黑屏但串口正常**: 先检查 map_mmio 是否真的被调用了（在 kprintf 中加日志），然后检查物理地址是否正确（从 BootInfo 读出来的 fb_addr 应该是类似 0xE0000000 的值）。如果 fb_addr 是 0，说明 bootloader 的 VESA 调用失败了。

**Triple fault 在 map_mmio 之后**: 大概率是覆盖了 PDPT[0] 或者 PDE flags 写错了。用 GDB 在 map_mmio 处设断点，检查每个写入的 PDE 值。

**像素位置不对**: 检查 pitch 是否正确。如果 pitch 为 0 或者是一个奇怪的值，说明 BootInfo 中的数据不正确——回头看 bootloader 中 VESA mode info 的解析。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 线性帧缓冲区 | 物理地址 + y*pitch + x*4，pitch 可能大于 width*4 |
| 2MB Huge Page | PDE flags 0x83，覆盖 2MB 物理内存 |
| 1GB Huge Page | PDPTE flags 0x83，需要 CPUID 检查支持 |
| volatile 指针 | MMIO 必须用 volatile 防止编译器优化 |
| VBE 模式号 | 0x144 + bit14(0x4000) = 0x4144 for Bochs |
