# 032-3 桌面图标系统——从数据结构到点击响应

> 标签：Cinux, GUI, DesktopIcon, 命中测试, 窗口管理器, 测试
> 前置：[032-gui-bitmap-icon-2](032-gui-bitmap-icon-2.md) — 编译期图标数据系统

## 前言

前两章我们把底层工具准备好了——Canvas 能画位图，编译期系统生成了像素数据。但到目前为止这些都是散落的零件，还没有组装成用户能看到的"桌面图标"。一个完整的桌面图标需要的东西远不止像素数据：它需要在屏幕上有确定的位置，需要一个文字标签告诉用户这是什么，需要在被点击时触发某个动作，还需要在窗口管理器的渲染循环中被正确地画出来。

这一章要讲的是把零件组装成产品的过程。我们会看到 DesktopIcon 结构体如何封装图标的所有信息，WindowManager 如何管理一组桌面图标的渲染和交互，以及双轨测试体系如何从像素级别验证整个链路的正确性。

## 环境说明

本篇涉及 WindowManager 的修改和两组测试文件。内核端测试需要在 QEMU 中运行（使用真实的 Framebuffer 和 Canvas），宿主端测试直接在开发机上编译运行（使用 MockCanvas）。编译条件分别是 `CINUX_GUI`（内核端）和 `CINUX_HOST_TEST`（宿主端）。

## 第一步——DesktopIcon：一个图标的完整画像

DesktopIcon 结构体的设计哲学是"数据驱动"——它只是一个纯粹的数据容器，不包含任何渲染或事件处理逻辑。这种分离让每个组件的职责清晰。

一个 DesktopIcon 有七个字段。x 和 y 是图标在屏幕上的位置（int32_t，支持负坐标——虽然不太可能在正常使用中出现，但命中测试需要处理这种边界情况）。bitmap 是指向编译期像素数组的指针（const uint32_t*）。label 是图标的文字标签（const char*）。width 和 height 是图标尺寸。action 是一个 IconAction 枚举值，描述点击后应该发生什么。

IconAction 用 scoped enum（enum class）定义，底层类型是 uint8_t。目前有三个值：None（无动作）、OpenShell（打开终端窗口）、OpenCalculator（打开计算器窗口）。使用 scoped enum 而不是普通 enum 或 #define 宏的原因和以前一样——类型安全、防止隐式转换、避免命名冲突。

contains 方法做的是标准的矩形包含测试。给定一个鼠标坐标，检查它是否落在图标的矩形范围内。四个比较条件：mx >= x（左边界）、mx < x + width（右边界，开区间）、my >= y（上边界）、my < y + height（下边界，开区间）。注意右边界和下边界用的是严格小于（< 而不是 <=），这意味着图标占据的最后一个像素是 (x + width - 1, y + height - 1)——这是图形学中半开半闭区间的标准约定。

## 第二步——WindowManager 的图标集成

WindowManager 需要三组能力来支持桌面图标：注册、渲染、命中测试。

注册通过 add_desktop_icon 方法完成——把传入的 DesktopIcon 复制到内部数组的末尾，递增计数器。数组大小固定为 16（MAX_ICONS），这是一个经验值。为什么不使用动态容器？因为我们不希望窗口管理器的图标管理依赖堆分配——减少运行时分配是内核编程的好习惯。

渲染在 composite() 方法中发生。composite 的流程是：先 clear 整个后端缓冲区为桌面背景色，然后调用 draw_desktop_icons 画所有图标，然后按 Z-order 从低到高 blit 每个窗口，最后画鼠标光标并 flip。图标画在窗口之前意味着窗口可以遮住图标——这符合真实桌面环境的行为。

draw_desktop_icons 遍历所有已注册的图标，对每个图标做两件事：调用 Canvas::draw_bitmap 画图标图案，然后计算标签的居中位置并用 draw_text 画文字标签。标签居中的计算方式是：标签总宽度 = 字符数 * 字体宽度，起始 x = 图标中心 x - 标签宽度 / 2。

命中测试在 handle_mouse 中发生。当鼠标按下时，handle_mouse 先检查是否点击了某个窗口的关闭按钮或标题栏，如果没有命中任何窗口，则检查是否命中了桌面图标。如果命中了图标，把图标的 action 记录到 pending_icon_action_ 中。外部调用者通过 consume_pending_icon_action() 获取并清空这个动作。

这里有一个设计上的"解耦"——handle_mouse 只负责"检测到点击并记录动作"，不负责"执行动作"。执行由上层的事件循环来完成，因为它可能需要创建新窗口、切换焦点等操作，这些逻辑不应该耦合在 handle_mouse 里。

## 第三步——双轨测试：像素级的正确性验证

这个 tag 的测试体系有一个很值得讲的特点——同一套功能有两组完全独立的测试。

内核端测试运行在 QEMU 中的真实内核环境里。它创建一个真实的 Framebuffer 对象（从 BootInfo 初始化），一个真实的 Canvas 对象，调用 draw_bitmap 后 flip 到 framebuffer，然后用 `g_fb.get_pixel()` 直接读取硬件帧缓冲区的像素来验证。这些测试覆盖了：不透明像素的精确位置和颜色、透明像素不覆盖背景、右边缘和底部边缘的裁剪、完全越界的位图什么都不画、null 像素指针不崩溃、32x32 大小的实际图标渲染。

宿主端测试运行在开发机上，不需要 QEMU。它用 MockCanvas 模拟后端缓冲区（内部用 std::vector<uint32_t>），draw_bitmap 的逻辑从内核代码逐字复制过来。宿主端测试覆盖了内核端的所有场景，还额外增加了一些边界用例——棋盘格模式（透明和不透明像素交替排列）、零宽零高位图（应该是 no-op）、1x1 位图在 Canvas 精确边界上、多层叠加后透明像素保留底层内容。

为什么要双轨？核心原因是开发效率。内核端测试需要完整启动流程——Bootloader 加载内核 → 进入 long mode → 初始化 GDT/IDT → 初始化堆 → 初始化 Framebuffer → 运行测试。这个周期至少需要几十秒。宿主端测试秒级完成，开发者改一行代码就能立即验证。但某些 bug 只能在真实内核环境中暴露——比如 VBE pitch 不等于 width * 4 时的像素偏移、或者 Framebuffer 初始化顺序问题。所以两者缺一不可。

## 和真实桌面环境的对比

如果拿 Cinux 的桌面图标和真正的桌面环境（Windows、macOS、GNOME）比，差距是显而易见的。真实的桌面环境有：图标的选中/高亮状态、拖拽重排、右键上下文菜单、双击 vs 单击的区分、图标缓存和异步加载、高 DPI 多分辨率变体、SVG 矢量图标……这些我们一个都没有。

但我们有的东西也不容小觑：编译期零开销的图标数据、基于 color-key 的高效透明渲染、和窗口系统正确集成的层级关系、精确到像素的命中测试、以及从单元测试到集成测试的完整覆盖。对于一个教学 OS 来说，这个桌面图标系统的完成度已经足够展示"一个 GUI 系统需要哪些组件"了——剩下的打磨和扩展是读者可以自己动手的实验方向。

## 收尾

到这里 tag 032 的全部内容就完成了。我们给 Canvas 加了 draw_bitmap 位图渲染能力，用 constexpr 模板在编译时生成了两个图标，用 DesktopIcon 结构体封装了图标的完整信息，把它们集成到了窗口管理器中，并建立了双轨测试来验证一切。桌面上现在有了可以点击的 Shell 和 Calculator 图标——虽然点击后的效果还没有完全实现（那属于后续 tag 的内容），但从渲染到命中测试的整个链路已经打通了。

完结撒花。下一站是更高级的 GUI 功能。

## 参考资料

- OSDev Wiki: [Drawing In a Linear Framebuffer](https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer) — 双缓冲渲染基础
- SerenityOS: [WindowManager](https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer) — 完整的窗口管理器和桌面图标系统
- Linux Kernel: [cfb_imageblit](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/core/cfbimgblt.c) — 帧缓冲图像渲染
