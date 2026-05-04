# Framebuffer 接口与 Canvas 双缓冲架构设计

> 标签：framebuffer, canvas, double buffering, pitch, 2D graphics
> 前置：[028e init 线程](028e-activate-init-thread-3.md)

## 导语

到目前为止，我们内核的图形输出方式还停留在「直接写 framebuffer」的阶段——Console 驱动调用 `Framebuffer::put_pixel()`，每次写入都直接抵达硬件显存。这种做法在小规模文字输出时够用，但一旦我们想在屏幕上画复杂的图形（矩形、线条、文字组合），问题就来了：用户会看到画面撕裂，因为显示控制器在扫描输出的过程中，我们可能正在写入同一块显存，半新半旧的像素就出现在了屏幕上。

本章我们要构建一个完整的 2D 图形画布——Canvas。它采用经典的软件双缓冲方案：所有绘制操作都在一个离屏的后备缓冲区（back buffer）上完成，画完一整帧后一次性拷贝到硬件 framebuffer。这是所有 GUI 系统的基本功——不管你是 Linux 的 fbdev、SerenityOS 的 LibGfx，还是更早期的 Windows GDI，双缓冲都是绕不过去的第一步。

完成本章后，我们会在屏幕上看到通过 Canvas 绘制的彩色矩形和标题文字——这是我们 GUI 子系统的起点。

## 概念精讲

### 为什么需要 Framebuffer 抽象层

在 tag 013 里，我们写了一个直接操作 VBE linear framebuffer 的驱动。那个驱动把 bootloader 传来的物理地址映射到内核虚拟地址空间，然后提供 `put_pixel()` 和 `fill_rect()` 等接口。现在我们想做 GUI 了，直接操作 framebuffer 的问题就暴露出来了：每次写像素都要走 MMIO（memory-mapped I/O），速度慢；更关键的是没法实现双缓冲——你没法在「另一个 framebuffer」上画完了再切换过去，因为硬件只认一个物理地址。

所以我们引入了一层抽象：Canvas 类。Canvas 不直接操作硬件 framebuffer，而是持有一个在内核堆上分配的 `uint32_t` 数组作为后备缓冲区。Canvas 对外暴露的绘图 API（draw_pixel、draw_rect、draw_line 等）全部写这个后备缓冲区。当一帧画完后，调用 `flip()` 方法，把后备缓冲区的内容按行拷贝到硬件 framebuffer。

### pitch 的概念

这里有一个很多人第一次接触 framebuffer 编程时都会踩的坑：**pitch 不等于 width * 4**。pitch 是「每条扫描线占用的字节数」，在某些 VBE 模式下，硬件为了对齐或其他原因，会在每行末尾加 padding。比如 1024x768 的 32 位色模式，理论上每行是 1024 * 4 = 4096 字节，但有些硬件的 pitch 可能是 4128 字节。如果你用 `width * 4` 来计算像素偏移，在第一行没问题，但到了第二行开始就会偏移——画面会出现「倾斜」效果。

Canvas 在初始化时从 Framebuffer 获取 pitch，后续所有像素索引计算都使用 `pitch / 4`（每个 uint32_t 占 4 字节），而不是直接用 width。这就是为什么我们的 back buffer 的大小是 `(pitch / 4) * height` 个 uint32_t，而不是简单的 `width * height`。

### 双缓冲的内存模型

```
+--------------------+         +--------------------+
|   Back Buffer      |  flip() |  Front Buffer      |
|   (kernel heap)    | ------->|  (MMIO framebuffer)|
|   可随意读写       |  memcpy |  volatile, 只写    |
+--------------------+         +--------------------+
      draw_pixel()
      draw_rect()
      draw_line()
      draw_text()
```

后备缓冲区是普通的内核堆内存，读写毫无限制。前缓冲区是 MMIO 映射的硬件显存，需要用 volatile 指针访问，并且拷贝时要逐行处理（因为 pitch 可能和 width 不对齐）。

### 与 Linux fbdev 的对比

Linux 的 fbdev 子系统也采用类似的双缓冲方案。用户程序通过 `mmap()` 把 `/dev/fb0` 映射到用户空间作为前缓冲区，然后自己 `malloc` 一块内存作为后缓冲区。画完后要么 `memcpy` 过去，要么用 `FBIOPAN_DISPLAY` ioctl 让硬件切换扫描地址（page flip）。Cinux 的做法等价于前者的 `memcpy` 方案——简单、通用、不依赖硬件特性。Linux 的现代 DRM/KMS 子系统则提供了更高效的 atomic page flip，但那需要一个完整的 GPU 驱动程序，对我们的教学内核来说太重了。

### volatile 关键字在 MMIO 中的作用

在 Canvas 的实现中，你会频繁看到 `volatile` 关键字。这不是摆设——它是正确性的保证。C++ 编译器被允许做很多优化，其中之一是「冗余存储消除」：如果你对同一个地址写了两次，编译器认为第一次的写入被第二次覆盖了，可以省略第一次。对于普通内存这是正确的优化，但对于 MMIO 设备寄存器，两次写入可能代表两个不同的命令——省略任何一个都会导致硬件行为错误。

在 `flip()` 中，我们把 back buffer 的数据拷贝到 `volatile uint8_t*`（framebuffer 地址）。volatile 告诉编译器「这个内存位置的值可能在你的控制之外改变，不要对它的读写做任何假设」。这保证了每次 `d[i] = s[i]` 都会生成一次真正的存储指令，不会被优化掉或合并。在我们的自定义 `memcopy()` 函数中，目标参数类型就是 `volatile void*`——这就是为什么我们不能用标准 `memcpy`（它的参数不是 volatile 的）。

### Canvas 的两种初始化模式

Canvas 提供了两种初始化方式，这并不是多余的——它们服务于不同的使用场景。`init(Framebuffer&)` 创建「屏幕 Canvas」，它有一个硬件前缓冲区，flip 会把内容刷新到真实的显示器上。`init(uint32_t w, uint32_t h)` 创建「离屏 Canvas」，没有硬件前缓冲区，flip 是空操作——画完的内容只存在于内存中，需要通过 blit 拷贝到屏幕 Canvas 才能显示。

为什么需要离屏 Canvas？想象一下窗口管理器的场景：每个窗口在自己的离屏 Canvas 上绘制内容（标题栏、按钮、文字），窗口管理器把所有窗口的内容合成到屏幕 Canvas 上，最后 flip 显示。这种「各自画自己的，最后合成」的架构是所有 GUI 系统的基本模式——从 X11 到 Wayland 到 Windows 都是这么做的。离屏 Canvas 就是窗口的「私有画布」。

## 动手实现

### Step 1: 扩展 Framebuffer 接口

**目标**: 在 `Framebuffer` 类中添加一个 `data()` 方法，返回 framebuffer 的原始内存指针。

**设计思路**: Canvas 的 `flip()` 需要直接访问 framebuffer 的原始内存来执行批量拷贝。之前 Framebuffer 的 `put_pixel()` 是唯一的写入接口，但它逐像素操作太慢了。我们添加 `data()` 返回 `volatile uint32_t*`，让 Canvas 可以做行级的 memcpy。注意返回类型是 volatile 的，因为 framebuffer 是 MMIO 设备内存。

**实现约束**: 返回类型为 `volatile uint32_t*`。这个方法只是 getter，不修改任何状态。

**验证**: 添加后编译通过即可。功能验证在 Canvas 集成时进行。

### Step 2: 设计 Canvas 类

**目标**: 创建 Canvas 类的头文件，定义所有必要的字段和方法签名。

**设计思路**: Canvas 需要两个核心数据——一个指向硬件 Framebuffer 的指针（front_buf_）和一个自己分配的后备缓冲区（back_buf_）。它还需要 width、height、pitch 三个维度信息来正确计算像素偏移。方法方面，我们需要：初始化（从 Framebuffer 或指定尺寸）、基本绘制（pixel、rect、line）、文字渲染（text）、缓冲区操作（flip、clear）。

一个重要的设计决策是提供两种初始化方式：`init(Framebuffer&)` 用于屏幕 Canvas（有前缓冲区，flip 会真正刷新屏幕），`init(uint32_t w, uint32_t h)` 用于离屏 Canvas（没有前缓冲区，flip 是空操作）。后者在后续的窗口系统中会大量使用——每个窗口有自己的离屏 Canvas。

**实现约束**: 禁止拷贝构造和拷贝赋值（因为内部有动态分配的缓冲区）。析构函数需要释放 back_buf_。所有方法都不应该是虚函数——我们不打算继承 Canvas，保持简单。

**踩坑预警**: back_buf_ 的分配大小必须是 `(pitch / 4) * height` 而不是 `width * height`。如果你用后者，在 pitch > width*4 的硬件上会越界写入，导致内核堆损坏。这是非常隐蔽的 bug——在 QEMU 里可能不会触发（QEMU 的 pitch 通常等于 width*4），但在真机或某些模拟器上会崩溃。

**验证**: 编译通过。后续步骤中会添加具体实现。

### Step 3: 实现 Canvas 初始化与基本像素操作

**目标**: 实现 `init()`、`draw_pixel()`、`clear()` 方法。

**设计思路**: `init(Framebuffer&)` 从 Framebuffer 获取维度信息，分配后备缓冲区，用 memfill32 清零。`draw_pixel()` 做边界检查后写入 `back_buf_[y * (pitch/4) + x]`。`clear()` 用 memfill32 把整个后备缓冲区填充为指定颜色。

这里我们写了两个内部辅助函数：`memcopy()` 用于 volatile 目标的字节拷贝（替代 libc memcpy，因为内核是 freestanding 的），`memfill32()` 用于用 32 位值填充一段内存（比逐字节填充快 4 倍）。

**实现约束**: memcopy 的目标参数必须是 volatile void*，因为前缓冲区是 MMIO。分配后备缓冲区时用 `new uint32_t[total_pixels]`，这会走内核堆的 operator new。如果堆空间不足（例如只有 64KB 初始堆），你需要先确保堆的 max_size_ 足够大。

**踩坑预警**: Canvas 的 back buffer 大约需要 `width * height * 4` 字节。对于 1024x768，就是大约 3MB。如果你的内核堆只预留了 1MB 的虚拟地址空间，`new` 会触发 expand，然后 expand 发现没有上限检查就会一直扩展，最终覆盖到 MMIO 或栈的虚拟地址区域。这个坑我们在后面的第三篇文章中会详细讨论。

**验证**: 编译后运行，检查串口输出 `[HEAP] Expanded by ...` 日志。如果看到 `[HEAP] Expansion limit reached`，说明堆空间不足。

### Step 4: 实现 flip() 方法

**目标**: 把后备缓冲区的内容拷贝到硬件 framebuffer。

**设计思路**: flip() 逐行拷贝——对于每一行，计算前缓冲区中该行的起始地址（考虑 pitch），然后用 memcopy 拷贝 width*4 字节。注意只拷贝 width*4 而不是 pitch 字节——pitch 可能包含 padding 字节，那些不需要从 back buffer 拷贝（它们可能包含硬件状态信息）。

**实现约束**: 如果 front_buf_ 为 nullptr（离屏 Canvas），flip() 应该直接返回不做任何事。back_buf_ 为 nullptr 时同理。

**验证**: 编写一个简单的测试：init Canvas、draw 一个矩形、flip、然后检查 framebuffer 上对应位置的像素值是否正确。我们会在后面的测试步骤中详细展示。

### Step 5: 内核测试验证

**目标**: 编写内核态测试，验证 Canvas 初始化和基本绘制操作的正确性。

**设计思路**: 测试分两个模式——CLI 模式（始终编译）验证 Framebuffer 和 Font 基本可用性，GUI 模式（CINUX_GUI 宏控制）验证 Canvas 的初始化、draw_rect、clear、flip。测试的核心思路是：在 Canvas 上画一个已知颜色的矩形，flip 后通过 Framebuffer 的 `get_pixel()` 读取硬件 framebuffer 的对应位置，确认像素值和预期一致。这是一种「写入-刷新-读回验证」的闭环测试。

对于 CLI 模式的测试，我们需要验证即使不编译 Canvas，Framebuffer 的基本功能（put_pixel、get_pixel、fill_rect、clear）仍然正常工作。这保证了 GUI 代码是真正可选的——关闭 CINUX_GUI 后内核行为不变。

**实现约束**: 测试函数通过 `run_canvas_tests()` 入口调用。每个测试用 TEST_ASSERT_EQ/TEST_ASSERT_GT 等宏检查条件。测试中创建的 Canvas 是栈变量，析构时自动释放 back buffer。

**验证**: 运行内核，检查串口输出。CLI 模式应该看到三个测试全部 PASS。GUI 模式应该额外看到 Canvas GUI Tests 全部 PASS。

### Step 6: 主机端单元测试

**目标**: 编写可以在 Linux 主机上编译运行的单元测试，不依赖内核环境。

**设计思路**: 主机端测试使用 mock 对象模拟 Framebuffer 和 PSFFont。MockFramebuffer 用 std::vector 分配像素数组，提供和内核 Framebuffer 相同的接口。MockPSFFont 允许手动设置字形位图数据。MockCanvas 复制了 Canvas 的全部算法逻辑。测试覆盖所有绘制方法：draw_pixel、draw_rect、draw_rect_outline、clear、flip、draw_line、draw_text、blit、draw_bitmap。

这种「在主机上用 mock 测试内核算法」的模式非常有价值——编译速度快（秒级 vs. 内核编译的分钟级），可以用 GDB 调试，不需要启动 QEMU。唯一需要注意的是 mock 的行为必须和内核实现完全一致——如果 mock 和内核代码有差异，测试通过不代表内核代码正确。

**实现约束**: 使用 `#ifdef CINUX_HOST_TEST` 条件编译。测试文件放在 `test/unit/test_canvas.cpp`。CMakeLists.txt 中添加编译配置。

**验证**: 在主机上编译并运行测试：
```
cd test && mkdir build && cd build
cmake .. && make && ./test_canvas
```
所有测试应该 PASS。

## 构建与运行

在启用 GUI 模式的情况下构建：
```
mkdir -p build && cd build
cmake .. -DCINUX_GUI=ON && make -j$(nproc)
```

运行 QEMU：
```
qemu-system-x86_64 -cdrom cinux.iso -serial stdio -no-reboot
```

你应该在 QEMU 窗口中看到黑色屏幕（Canvas 已初始化但还没画东西），串口输出中会看到 `[HEAP] Expanded by ...` 和 `[BIG] Canvas initialised` 等日志。

如果你想在不启用 GUI 的情况下验证内核仍然正常工作：
```
cmake .. -DCINUX_GUI=OFF && make -j$(nproc)
./run.sh
```
此时应该只看到 CLI 模式的测试通过，Canvas GUI Tests 被跳过。

## 调试技巧

**最常见的 bug：画面倾斜或花屏**。这说明你的像素偏移计算有问题——检查是否用了 `width` 而不是 `pitch/4` 来计算行偏移。在 Canvas 的任何地方，像素索引都必须是 `y * (pitch / 4) + x`。

**第二个常见问题：flip 后屏幕全黑**。检查 draw_pixel 和 draw_rect 是否真的写入了 back buffer。可以在 draw 操作后加一个临时的串口日志，打印某个像素位置的值。

**用 QEMU monitor 调试**：按 Ctrl+A 然后按 C 进入 QEMU monitor，输入 `info mtree` 可以看到 framebuffer 的内存映射情况，确认物理地址和虚拟地址映射是否正确。

## 本章小结

| 概念 | 说明 |
|------|------|
| Double Buffering | 后备缓冲区绘制 + flip 一次性刷新 |
| pitch | 每条扫描线的字节数，可能大于 width * bytes_per_pixel |
| volatile MMIO | framebuffer 是设备内存，需要 volatile 指针访问 |
| Back buffer | 内核堆上的 uint32_t 数组，大小为 (pitch/4) * height |
| Front buffer | Framebuffer.data() 返回的 volatile 指针 |

到这里我们已经搭好了 Canvas 的骨架——初始化、像素操作和双缓冲翻转都就位了。下一章我们来实现更高级的图元绘制：直线（Bresenham）、矩形描边、位图渲染和画布间 blit 操作。

## 概念补充：为什么内核需要自己的 memcopy

你可能会问：为什么不直接用 `memcpy` 或者 `memset`？答案在于两点。首先，Cinux 内核是 freestanding 环境——没有 libc，没有标准库函数。虽然 GCC 可能会生成对 `memcpy` 的隐式调用（比如结构体赋值时），但我们显式使用的拷贝函数必须自己实现。其次，`memcopy` 的目标参数是 `volatile void*`——标准 `memcpy` 的签名不接受 volatile 指针。如果我们把 volatile 指针强转为 `void*` 传给 `memcpy`，编译器可能对写入 MMIO 的操作做「合法」的优化（比如合并连续写入），导致画面显示不正确。

`memfill32` 按整个 uint32_t 填充而不是逐字节填充，这在 32 位色模式下天然正确——每个像素就是一个 uint32_t。相比逐字节填充，这减少了 4 倍的循环迭代次数。对于 3MB 的 back buffer（约 786432 个 uint32_t），这就是 786432 次 vs. 3145728 次循环迭代的差距。

## 设计决策总结

| 决策点 | 我们的选择 | 备选方案 | 理由 |
|--------|-----------|---------|------|
| 后备缓冲区位置 | 内核堆 (new[]) | 固定物理地址 | 堆分配灵活，大小随分辨率自适应 |
| 前缓冲区指针 | volatile uint32_t* | 封装为 Framebuffer 方法 | 批量操作需要原始指针，put_pixel 太慢 |
| 离屏 Canvas | init(w, h) 无 front | 只支持屏幕 Canvas | 窗口系统需要独立的离屏渲染面 |
| 拷贝函数 | 自定义 memcopy | libc memcpy | volatile 支持和 freestanding 环境 |
| 像素格式 | 固定 0x00RRGGBB | 可配置格式 | 简化实现，VBE 32 位色模式统一 |
