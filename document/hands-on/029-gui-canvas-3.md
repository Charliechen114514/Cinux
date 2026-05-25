---
title: 029-gui-canvas-3 · GUI Canvas
---

# PSF2 字体渲染、PIT 定时刷新与内存安全修复

> 标签：PSF2 font, draw_text, PIT callback, heap safety, direct map
> 前置：[029 图元绘制](029-gui-canvas-2.md)

## 导语

前两章我们搭好了 Canvas 的双缓冲架构和图元绘制能力。现在画面能画了，但还差一个关键能力：文字渲染。没有文字的 GUI 就像没有灵魂的空壳——窗口标题、按钮标签、终端输出，全都离不开文字。本章我们要做三件事：实现 PSF2 位图字体的解析和渲染；给 PIT 加一个定时回调机制来实现自动刷新；最后处理 Canvas 引入的一个大坑——3MB 的 back buffer 分配暴露了堆扩展和物理内存映射的深层问题。

## 概念精讲

### PSF2 位图字体格式

PSF（PC Screen Font）是 Linux 内核使用的控制台字体格式。PSF2 是它的第二个版本，用一个 32 字节的头部描述字体元信息，然后紧跟所有字形（glyph）的位图数据。每个字形是一个简单的位图——对于 8x16 的字体，每个字形占 16 字节，每字节表示一行的 8 个像素，最高位（MSB）对应最左边的像素。位为 1 表示前景色，位为 0 表示背景色（但我们的 draw_text 不画背景，只在 1 的位置写前景色像素）。

PSF2 头部的关键字段包括：magic（0x864AB572，用于验证文件格式）、headersize（位图数据在文件中的偏移量，通常是 32）、numglyph（字形数量，通常是 256 或 512）、bytesperglyph（每个字形占用的字节数）、height 和 width（字形的像素尺寸）。我们把字体文件通过 `.incbin` 汇编指令嵌入内核二进制，这样在运行时就可以直接通过链接器符号访问字体数据，不需要文件系统支持。

### 字形渲染到 Canvas

draw_text 的工作流程是：遍历字符串的每个字符，用字符的 ASCII 码作为索引从字形数组中找到对应的位图数据，然后对位图的每一行、每一位检查——如果该位为 1，就在 Canvas 的对应位置写一个前景色像素。光标位置每渲染完一个字符就向右移动一个字形宽度，遇到换行符就回到行首并向下移动一个字形高度。

这里有一个细微但重要的设计决策：draw_text 只渲染前景像素（字形位为 1 的位置），不渲染背景像素（位为 0 的位置）。这意味着文字是「透明」的——它叠加在已有的背景上，不会覆盖掉背景内容。这对于在已有图形上叠加文字（比如窗口标题栏上的文字）来说很方便。如果需要带背景的文字，需要先画一个填充矩形作为背景，再画文字。

### PIT 定时回调机制

为了让 GUI 画面定期刷新，我们在 PIT 驱动中添加了一个回调机制。`set_tick_callback(fn, ctx)` 注册一个函数指针和上下文参数，PIT 的 IRQ0 中断处理函数在每次 tick 时调用这个回调。对于 GUI 模式，回调函数负责：排空输入事件队列（鼠标/键盘）、将事件分发给窗口管理器、合成所有窗口到屏幕 Canvas、调用 flip() 刷新显示。

这个回调运行在中断上下文中（IRQ0 handler），所以它不能做任何可能阻塞的操作——不能分配内存、不能 sleep、不能获取自旋锁之外的锁。重操作（比如 fork+execve 创建新终端）需要通过一个原子变量把请求排队，让一个独立的内核工作线程在进程上下文中执行。

### 堆扩展安全：3MB 分配引发的血案

这是本 tag 最精彩的排查故事。Canvas 初始化时需要分配约 3MB 的后备缓冲区（1024x768x4 字节）。内核堆的初始大小只有 64KB，所以会触发 expand()。问题在于，原来的 expand() 没有上限检查——它会一直扩展堆的虚拟地址空间，直到 VMM 把物理页映射到不该映射的地方。

在我们的内存布局中，堆区域后面紧跟着 MMIO 区域、framebuffer 映射区域和栈区域。当堆扩展到 3MB 以上时，它开始覆盖 MMIO 和栈的虚拟地址空间。更糟糕的是，phys_to_virt() 假设所有物理地址都已通过直接映射可用——但 bootloader 只映射了 ELF 段覆盖的物理内存范围（约 20MB）。当 Canvas 消耗了大量低地址物理页后，PMM 开始返回高地址物理页（>20MB），这些地址没有映射，访问就 page fault。

修复包含两部分：给 Heap 加 max_size_ 上限（128MB），以及让 bootloader 扫描 E820 内存映射并创建完整的物理内存直接映射（使用 2MB 和 1GB 大页来减少页表开销）。

## 动手实现

### Step 1: 扩展 PSFFont 的 glyph() 方法

**目标**: 在 PSFFont 类中添加一个 `glyph()` 方法，返回指定字符的位图数据指针。

**设计思路**: PSFFont 已经在 init() 中解析了 PSF2 头部，存储了字形数组的起始地址和每个字形的大小。glyph(c) 的实现就是计算 `glyphs_ + c * bytes_per_glyph_`。需要做边界检查——如果 c 超过 num_glyphs_，回退到第 0 个字形（通常是一个方框或问号）。如果字体没初始化（glyphs_ == nullptr），返回 nullptr。

**实现约束**: 返回 `const uint8_t*`（只读指针），因为字体数据是嵌入在内核二进制中的 const 数据。调用者（Canvas::draw_text）需要检查返回值是否为 nullptr。

**验证**: 初始化字体后，调用 glyph('A') 并检查返回值非空。

### Step 2: 实现 Canvas 的 draw_text 方法

**目标**: 实现基于 PSF2 字体的文字渲染。

**设计思路**: 维护一个 cursor_x 和 cursor_y 追踪当前文字位置。遍历字符串的每个字符：如果是换行符 '\n'，cursor_x 回到起始 x，cursor_y 增加字形高度；否则，获取字形位图，对每一行读取一个字节，对每一位检查是否为 1（使用 `(bits >> (7 - col)) & 1`），如果是就调用 draw_pixel 在当前位置写前景色。渲染完一个字符后 cursor_x 增加字形宽度。

**实现约束**: 字形位图是每行一个字节，MSB 在左。需要从 font 获取 width() 和 height()。如果 width 或 height 为 0（字体未初始化），直接返回。支持换行符。

**踩坑预警**: 如果你的字形宽度不是 8 的倍数（比如某些 PSF2 字体是 7 像素宽），`(bits >> (7 - col)) & 1` 的 7 应该替换为 `width - 1`。不过目前我们用的标准 PSF2 字体都是 8 像素宽，所以硬编码 7 没问题。如果后续要支持更宽的字体，需要改为通用计算。

**验证**: 在 Canvas 上渲染字符串 "Test"，位置 (10, 10)，白色前景。Flip 后检查 (10, 10) 到 (42, 26) 区域内是否有白色像素。

### Step 3: 给 PIT 添加 tick 回调机制

**目标**: 在 PIT 类中添加 set_tick_callback 和 invoke_tick_callback 方法。

**设计思路**: 添加两个 static 成员变量：`tick_callback_`（函数指针）和 `tick_callback_ctx_`（void* 上下文）。set_tick_callback 设置这两个值。invoke_tick_callback 检查 tick_callback_ 是否非空，如果是就调用 `tick_callback_(tick_callback_ctx_)`。在 irq0_handler 中，递增 tick_count 之后、发送 EOI 之后调用 invoke_tick_callback。

**实现约束**: 这些方法和成员变量用 `#ifdef CINUX_GUI` 包裹，只在 GUI 模式下编译。回调函数的签名是 `void (*)(void*)`。

**踩坑预警**: 回调运行在中断上下文中。如果你在回调里做任何可能 sleep 的操作（比如分配大块内存），整个系统会死锁。GUI 的回调只做轻量操作（事件分发、合成、flip），重操作通过原子变量排队给工作线程。

**验证**: 注册一个简单的回调（打印一条串口日志），运行后检查每秒是否输出了 100 条日志（100 Hz PIT 频率）。

### Step 4: 修复堆扩展上限

**目标**: 给 Heap 类添加 max_size_ 字段和 expand() 中的上限检查。

**设计思路**: 在 init() 中设置 `max_size_ = KMEM_HEAP_SIZE`（128MB）。在 expand() 中，计算 `size_ + expand_size` 是否超过 max_size_，如果超过就打印错误日志并返回 false。这样当 Canvas 分配 3MB 后续操作触发 expand 时，堆会在 128MB 处停止扩展，不会越界。

**实现约束**: max_size_ 在 init() 中设置后就不再改变。expand() 返回 false 时，alloc_locked 会返回 nullptr，调用者需要处理分配失败的情况。

**验证**: 运行内核，检查串口输出中不再出现 `[HEAP] Expansion limit reached` 错误。

### Step 5: 扩展 bootloader 的物理内存直接映射

**目标**: 让 bootloader 扫描 E820 内存映射并映射所有物理内存到 KERNEL_VMA 偏移之上。

**设计思路**: 在 bootloader 的 phase2 中，遍历 BootInfo 的 E820 条目，找到最高的可用 RAM 地址。然后用 2MB 大页映射 0 到 1GB 范围，用 1GB 大页映射 1GB 以上范围。这样 phys_to_virt() 就能正确工作于任何物理地址。

**实现约束**: 大页映射需要 PDE 的 PS 位（bit 7）置 1。2MB 页需要 PD 指向一个 4KB 对齐的 2MB 物理区域，1GB 页需要 PDP 指向 1GB 对齐的 1GB 物理区域。页表本身占用的物理内存从 bootloader 自有的页表区域分配。

**踩坑预警**: 全量直接映射后，原来的 test_demand_page 测试会失败——它假设某个高地址没有被映射，会触发 page fault。现在所有物理地址都映射了，需要修改测试逻辑。

**验证**: 运行内核，检查不再出现 `test_create_user_space` 的 hang。确认 VMM 测试全部通过。

### Step 6: 集成到 kernel_main

**目标**: 在 kernel_main 中初始化 Canvas、调用 GUI init、注册 PIT 回调。

**设计思路**: 在 Console 初始化之后（步骤 15b），创建一个 static Canvas 对象，调用 init(fb) 初始化。然后调用 gui_init(canvas, font) 设置 GUI 子系统。在 IRQ unmask 步骤中，额外 unmask IRQ12（PS/2 鼠标中断）。

**实现约束**: Canvas 和 font 都声明为 static 局部变量，确保生命周期覆盖整个内核运行期间。gui_init 在中断使能之前调用，但 PIT 回调的注册（gui_start）在 init 线程中进行。

**验证**: 运行 QEMU，你应该看到深色背景上画着 10 个随机颜色的矩形，顶部居中显示白色的 "Cinux GUI" 文字。串口输出 `[BIG] GUI demo rendered to framebuffer.`。

### Step 7: 更新测试适配全量直接映射

**目标**: 修改 test_vmm.cpp 中的 test_demand_page 测试，使其在全量直接映射环境下仍然有效。

**设计思路**: 原来的测试假设某个高地址虚拟地址没有被映射，访问它会触发 page fault。全量直接映射后，所有物理地址都有了映射，这个假设不再成立。新的测试改为验证高地址（原来未映射的区域）现在可以通过直接映射正常读写——写一个已知值，读回来确认一致。

**实现约束**: 测试需要一个足够高的地址（比如 0xFFFFFFFF80000000 + 100MB）来验证 direct map 的覆盖范围。读写操作需要页面已经被映射（由 bootloader 的 direct map 保证）。

**踩坑预警**: 如果你忘记改这个测试，它会在全量直接映射后每次都 fail，因为 page fault 不再触发。看起来像是直接映射引入了新 bug，但实际上是测试的假设过时了。这是一个很重要的教训——当底层基础设施（内存映射）发生变化时，所有依赖旧假设的测试都需要重新审视。

**验证**: 运行内核，确认 VMM 测试全部 PASS，包括修改后的 test_demand_page。

## 构建与运行

```
mkdir -p build && cd build
cmake .. -DCINUX_GUI=ON && make -j$(nproc)
./run.sh
```

GUI 模式的完整 QEMU 命令：
```
qemu-system-x86_64 -cdrom cinux.iso -serial stdio -no-reboot \
    -m 2G
```

需要至少 2G 内存来容纳 128MB 的堆虚拟空间和 3MB+ 的 Canvas 后备缓冲区。

### 完整验证清单

完成本 tag 后，请逐项确认以下验证点：

**CLI 模式（CINUX_GUI=OFF）**:
- 所有 356 项基线测试通过
- Console 正常输出文字
- Framebuffer get_pixel/put_pixel 正常

**GUI 模式（CINUX_GUI=ON）**:
- Canvas init 成功，串口显示 canvas 尺寸和 pitch
- draw_rect 测试通过（矩形像素值正确）
- draw_rect_outline 测试通过（描边正确、内部未填充）
- draw_line 测试通过（起点终点像素正确）
- clear + flip 测试通过（全屏颜色正确）
- draw_text 测试通过（至少一个前景像素被渲染）
- GUI demo 显示 10 个彩色矩形 + "Cinux GUI" 标题
- 堆不再越界扩展，串口无 `[HEAP] Expansion limit reached` 错误
- VMM 测试（包括修改后的 test_demand_page）全部通过

## 调试技巧

**Canvas 初始化后内核 hang**：最可能的原因是堆分配触发了 expand，expand 越界覆盖了其他区域。检查 memory_layout.hpp 中堆的大小（KMEM_HEAP_SIZE）是否足够大。

**draw_text 输出乱码或无文字**：检查 PSFFont 是否正确初始化——字体数据的链接器符号（font_psf_start）是否指向了正确的嵌入文件。如果符号为空，说明 .incbin 没有正确包含字体文件。

**flip 后屏幕无变化**：在 flip() 里加一个串口日志打印 back buffer 和 front buffer 的前几个像素值，确认数据确实被拷贝了。如果 back buffer 有数据但 front buffer 没变化，可能是 volatile 指针的问题——确认 memcopy 的目标参数是 volatile void*。

## 本章小结

| 概念 | 说明 |
|------|------|
| PSF2 font | Linux 控制台位图字体格式，magic 0x864AB572 |
| glyph() | 返回字形位图指针，每行一字节，MSB 在左 |
| draw_text | 逐字形、逐位渲染，支持换行符 |
| PIT callback | 定时回调机制，运行在中断上下文 |
| Heap max_size_ | 防止堆扩展越界到其他虚拟地址区域 |
| Full direct map | bootloader 映射全部物理内存，大页优化 |

到这里，029_gui_canvas 的全部功能都已经就位了。我们有了完整的双缓冲 Canvas、基本的 2D 图元绘制、PSF2 字体渲染、以及一个可靠的内存管理基础。下一步就是在此基础上构建窗口管理器了。

## 本 Tag 完整文件清单

以下是本 tag 新增和修改的所有文件，按照功能分组：

**新增文件（核心实现）**:
- `kernel/drivers/canvas.hpp` — Canvas 类声明（159 行）
- `kernel/drivers/canvas.cpp` — Canvas 实现（248 行）
- `kernel/drivers/video/font.hpp` — PSFFont glyph() 声明（17 行）
- `kernel/drivers/video/font.cpp` — PSFFont glyph() 实现（8 行）
- `kernel/drivers/video/framebuffer.hpp` — Framebuffer data() 访问器（10 行）
- `kernel/gui/CMakeLists.txt` — GUI 子系统构建配置（8 行）

**新增文件（测试）**:
- `kernel/test/test_canvas.cpp` — 内核态 Canvas 测试（268 行）
- `test/unit/test_canvas.cpp` — 主机端单元测试（844 行）

**修改文件**:
- `kernel/main.cpp` — Canvas 初始化、GUI init、IRQ12 unmask
- `kernel/drivers/pit/pit.hpp` — tick 回调声明
- `kernel/drivers/pit/pit.cpp` — tick 回调实现
- `kernel/mm/heap.hpp` — max_size_ 字段
- `kernel/mm/heap.cpp` — expand 上限检查
- `kernel/arch/x86_64/memory_layout.hpp` — KMEM_HEAP_SIZE 128MB
- `kernel/mini/big_kernel_loader.cpp` — 全量物理内存直接映射
- `kernel/test/test_vmm.cpp` — 适配全量直接映射

**总计**: 25 files changed, +1872/-44 lines
