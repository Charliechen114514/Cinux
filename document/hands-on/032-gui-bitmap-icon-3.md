# 032-3 DesktopIcon 集成与测试验证

## 导语

前两篇我们已经完成了两个关键组件：Canvas 的 draw_bitmap 方法能正确渲染带透明度的位图，编译期图标数据系统能生成 32x32 的像素数组。但这两个组件目前还是孤立的——draw_bitmap 不知道要画什么图标，图标数据也不知道要被画在哪里。这一篇要把它们串起来：用 DesktopIcon 结构体封装图标的完整信息（位置、像素、标签、动作），然后把这套机制集成到 WindowManager 中，最后建立双轨测试来验证整个链路。

完成本篇后，我们的桌面将不再是空荡荡的深色背景——上面会有可点击的图标，点击后能触发打开 Shell 或 Calculator 的动作。这是 GUI 系统从"窗口管理器"进化到"桌面环境"的关键一步。

## 概念精讲

### DesktopIcon 结构体——图标的数据模型

一个桌面图标需要包含哪些信息？首先是屏幕上的位置（x, y 坐标），因为窗口管理器需要知道把图标画在哪里。然后是像素数据——指向之前编译期生成的 uint32_t 数组的指针。还需要一个文字标签（"Shell"、"Calculator"），显示在图标下方。当然还有图标的尺寸（宽和高），以及一个关键字段——动作（Action），描述用户点击这个图标后应该发生什么。

把这些字段组合在一起就形成了 DesktopIcon 结构体。它的设计哲学是"数据驱动"——结构体本身不包含任何渲染逻辑或事件处理逻辑，只是一个纯粹的数据容器。渲染由 WindowManager 的 draw_desktop_icons() 负责，事件处理由 handle_mouse() 中的命中测试负责。这种分离让每个组件的职责清晰，也方便单独测试。

### 命中测试——contains 方法的几何逻辑

命中测试的核心问题是：给定一个鼠标坐标，判断它是否落在某个图标的矩形范围内。这在几何上就是一个点-矩形包含测试：点的 x 坐标必须在 [icon.x, icon.x + icon.width) 范围内，y 坐标必须在 [icon.y, icon.y + icon.height) 范围内。注意右边界和下边界是"开区间"——这意味着图标占据的最后一个像素是 x + width - 1 和 y + height - 1，而不是 x + width 和 y + height。这是图形学中的标准约定，和 Canvas 的 draw_rect 裁剪逻辑保持一致。

contains 方法还支持负坐标——图标的 x 和 y 可以是负数，表示图标部分在屏幕外。虽然这种情况下图标不可见部分不会被渲染，但命中测试仍然能正确工作，因为 contains 纯粹是数值比较，不涉及缓冲区访问。

### 双轨测试策略

这个 tag 的测试体系有一个特别之处：同一个功能（draw_bitmap 和 DesktopIcon）有两组测试，分别运行在 QEMU 内核环境和宿主端（Host）环境中。内核端测试使用真实的 Framebuffer 和 Canvas，通过 `g_fb.get_pixel()` 直接读取硬件帧缓冲区的像素来验证渲染结果。宿主端测试使用 MockCanvas——一个用 std::vector 模拟后端缓冲区的轻量级实现，不需要 QEMU 就能在开发机上运行。

为什么要双轨？因为内核环境的测试需要完整的启动流程（Bootloader → GDT → IDT → 堆 → Framebuffer），调试周期较长。宿主端测试可以秒级完成，适合开发阶段快速迭代。但某些问题只能在真实内核环境中暴露（比如 VBE pitch 不等于 width * 4 时的像素偏移），所以两者缺一不可。

## 动手实现

### Step 1: 定义 IconAction 枚举和 DesktopIcon 结构体

**目标**: 创建 DesktopIcon 头文件，包含图标动作枚举和图标数据结构。

**设计思路**: IconAction 用 scoped enum（enum class）定义，底层类型是 uint8_t 以节省内存。DesktopIcon 使用聚合体设计——所有成员都是 public，没有构造函数或方法（除了 contains），可以用聚合初始化直接赋值。这种设计让创建一个图标变得非常直观：按照字段顺序赋值即可。

**实现约束**: 结构体的 bitmap 字段是一个指向 const uint32_t 的指针，指向编译期生成的像素数组。label 是一个 C 字符串指针（const char*）。contains 方法标记为 [[nodiscard]]，提醒调用者不要忽略返回值。

**踩坑预警**: DesktopIcon 的 x 和 y 字段是 int32_t（有符号），但 width 和 height 是 uint32_t（无符号）。contains 方法在做加法 `x + width` 时需要注意——有符号加无符号的结果会被提升为无符号，所以 static_cast 是必要的，否则在 x 为负值时会得到错误的结果。

**验证**: 可以通过编写简单的命中测试用例来验证——创建一个位于 (10, 20)、大小 32x32 的 DesktopIcon，检查 (10, 20)、(25, 35)、(41, 51) 返回 true，(9, 20)、(42, 51) 返回 false。

### Step 2: 将图标支持集成到 WindowManager

**目标**: 在 WindowManager 中添加图标注册、渲染和命中测试的支持。

**设计思路**: WindowManager 新增三个成员：一个固定大小的 DesktopIcon 数组（最大 16 个）、一个图标计数器、一个待处理的图标动作（pending_icon_action_）。composite() 方法在清除桌面背景后、blit 窗口之前调用 draw_desktop_icons()，这样图标被窗口覆盖——符合桌面环境的常规层级关系。handle_mouse() 在处理窗口事件之前先检查是否点击了桌面图标，如果是则记录 pending action 而不是传递给窗口。

**实现约束**: add_desktop_icon() 返回 bool 表示是否添加成功（数组满了就失败）。hit_test_icon() 反向遍历图标数组，后注册的图标优先。consume_pending_icon_action() 是一次性消费接口——调用后自动重置为 None。

**踩坑预警**: 图标的标签文字需要居中显示在图标下方。计算居中位置时需要考虑字体的字符宽度——标签总宽度等于字符数乘以字符宽度，起始 x 坐标等于图标中心 x 减去标签宽度的一半。

**验证**: 注册几个图标后调用 composite()，在 QEMU 中应该能看到桌面上的图标图案和下方的文字标签。点击图标区域时，consume_pending_icon_action() 应该返回对应的动作。

### Step 3: 编写内核端和宿主端测试

**目标**: 建立完整的测试覆盖，验证 draw_bitmap 的像素正确性和 DesktopIcon 的命中测试边界条件。

**设计思路**: 内核端测试（kernel/test/）启动一个真实的 Framebuffer，对每种场景（不透明渲染、透明跳过、边界裁剪、null 防护、32x32 实际图标）创建 Canvas、执行 draw_bitmap、调用 flip()、然后用 g_fb.get_pixel() 逐像素验证。宿主端测试（test/unit/）使用 MockCanvas 做同样的事，额外增加了棋盘格模式、零宽零高、多层叠加等边界用例。

**实现约束**: 内核端测试需要在 main_test.cpp 中注册入口函数。宿主端测试使用自定义的 TEST 宏和 ASSERT_EQ / ASSERT_TRUE / ASSERT_FALSE 宏。两组测试的 MockCanvas 在 draw_bitmap 的实现上完全镜像 kernel/drivers/canvas.cpp 中的逻辑。

**踩坑预警**: 内核端测试用的 Framebuffer 需要从 BootInfo 初始化（物理地址 0x7000），如果 BootInfo 中的帧缓冲区信息不正确（比如在非图形模式的 QEMU 配置下），测试会崩溃。另外，宿主端测试不能直接 include icon_data.hpp（因为 icon_data.hpp 依赖内核头文件路径），需要手动定义测试用的图标数据。

**验证**: 运行内核测试需要在 QEMU 中查看串口输出，所有测试应该 PASS。宿主端测试直接在开发机上运行可执行文件，应该返回退出码 0。

## 构建与运行

内核端测试的构建需要在 CMakeLists.txt 中注册新的测试源文件。宿主端测试需要在 test/CMakeLists.txt 中添加新的测试目标。两组测试的编译条件不同：内核端需要 CINUX_GUI 宏，宿主端需要 CINUX_HOST_TEST 宏。

构建后，内核端测试随内核一起在 QEMU 中启动，串口输出会显示测试结果。宿主端测试可以单独运行，不需要 QEMU。建议在开发 draw_bitmap 和 DesktopIcon 的过程中频繁运行宿主端测试来快速验证，完成后再用 QEMU 做完整的集成验证。

## 调试技巧

**命中测试位置不对**: 如果点击图标没有反应，但点击旁边的位置有反应，说明图标的 hit test 坐标和渲染坐标不一致。检查 add_desktop_icon 时的 x/y 值和 draw_desktop_icons 中 draw_bitmap 的 x/y 值是否相同。

**测试中断言失败**: 如果某个像素验证失败，串口输出会显示期望值和实际值。先确认期望值是否正确（对照调色板定义），再检查实际值是否来自错误的像素位置（行偏移计算错误）。

**图标之间有重叠**: 如果两个图标显示在同一位置，检查 add_desktop_icon 时传入的坐标是否有误。WindowManager 本身不做图标位置的冲突检测——所有图标的位置完全由注册时的参数决定。

## 本章小结

| 概念 | 要点 |
|------|------|
| DesktopIcon | 聚合体结构体：位置 + 像素指针 + 标签 + 尺寸 + 动作 |
| IconAction | scoped enum，描述点击图标后触发的动作 |
| contains | 矩形命中测试，支持负坐标，右下边界为开区间 |
| WindowManager 集成 | 图标数组、渲染层级（图标在窗口下面）、点击优先级 |
| 双轨测试 | 内核端（真实 Framebuffer）+ 宿主端（MockCanvas） |

到这里 tag 032 的全部内容就完成了。我们给 Canvas 增加了位图渲染能力，用 constexpr 模板在编译时生成了图标数据，封装了 DesktopIcon 结构体和命中测试，并集成到了窗口管理器中。桌面上现在有了可以点击的图标，GUI 系统迈出了从"窗口"到"桌面"的关键一步。
