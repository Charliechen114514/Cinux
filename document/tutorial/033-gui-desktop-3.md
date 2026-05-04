# 033-gui-desktop-3: 把所有部件连起来——GUI 初始化、合成管线与测试验证

> 标签：GUI, kernel init, compositing, testing, CMake, constexpr
> 前置：[033-gui-desktop-2](033-gui-desktop-2.md)

## 前言

前两篇我们分别实现了桌面图标管理和应用启动器。现在要做的是把这些新代码和内核的启动序列正确地串起来，确保整个 GUI 子系统在新架构下工作正常，并且用充分的测试来验证。这篇教程还会覆盖一个容易被忽略但很实用的修复——icon_data.hpp 中 consteval 到 constexpr 的降级。

说实话，这个 tag 最让我满意的部分不是某个特定的算法或数据结构，而是整个系统的启动流程变得清晰了。之前 init.cpp 要知道 Terminal 的类型、创建方式、管道连接方式——现在它只做一件事：创建管道并交给 GUI 子系统。GUI 子系统自己决定什么时候、怎么创建终端窗口。这种"各管各的"的解耦让代码更容易理解和扩展。

## 环境说明

本篇涉及 `init.cpp`、`icon_data.hpp`、`kernel/test/test_desktop.cpp`、`test/unit/test_desktop.cpp` 和两个 CMakeLists.txt。新增测试代码约 1658 行，是本 tag 代码量最大的部分。

## 启动序列的重构

init.cpp 的 GUI 部分变不大（大约从 20 行变成 15 行），但每一步的语义都变了。关键变化是管道连接和 GUI 启动的顺序。

旧顺序是这样的：挂载 ext2 → gui_start()（创建 Terminal 并返回指针） → 创建管道 → 连接管道到 Terminal。这个顺序有一个问题——Terminal 在管道创建之前就存在了，但管道连接在之后。如果 gui_start 和管道创建之间出了任何问题（比如 new Pipe 失败），Terminal 就处于一个半初始化状态。

新顺序是：挂载 ext2 → 创建管道 → set_shell_pipes（存储指针） → gui_start（注册图标）。

```cpp
#ifdef CINUX_GUI
    auto* stdin_pipe = new cinux::ipc::Pipe();
    auto* stdin_read_ops = new cinux::ipc::PipeReadOps(stdin_pipe);
    auto* stdin_read_inode = new cinux::ipc::PipeReadNode(stdin_read_ops);
    auto* stdin_file = new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY);
    cinux::fs::g_global_fd_table().set(0, stdin_file);

    auto* stdout_pipe = new cinux::ipc::Pipe();
    auto* stdout_write_ops = new cinux::ipc::PipeWriteOps(stdout_pipe);
    auto* stdout_write_inode = new cinux::ipc::PipeWriteNode(stdout_write_ops);
    auto* stdout_file = new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY);
    cinux::fs::g_global_fd_table().set(1, stdout_file);

    cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);
    cinux::gui::gui_start();
#endif
```

管道创建的代码完全没变——两对管道，分别绑定到 fd 0（stdin）和 fd 1（stdout）。Shell 进程通过 syscall read(0) 和 write(1) 使用这些管道。变化是管道创建之后立即调用 `set_shell_pipes` 存储指针，然后才调用 `gui_start()`。

init.cpp 中删除了 `#include "kernel/gui/terminal.hpp"`——init.cpp 不再需要知道 Terminal 的存在。这是一个小但重要的变化：头文件依赖变少了，编译变快了，而且 init.cpp 和 Terminal 的实现彻底解耦了。

### gui_start 中的图标注册

gui_start 的变化我们已经在前一篇看过了。它现在不创建终端窗口，而是注册两个桌面图标：

```cpp
DesktopIcon shell_icon{
    .x      = 40,
    .y      = 40,
    .bitmap = icons::data::k_shell_icon.data(),
    .label  = "Shell",
    .width  = icons::ICON_SIZE,
    .height = icons::ICON_SIZE,
    .action = IconAction::OpenShell,
};
wm.add_desktop_icon(shell_icon);

DesktopIcon calc_icon{
    .x      = 40,
    .y      = 120,
    .bitmap = icons::data::k_calc_icon.data(),
    .label  = "Calculator",
    .width  = icons::ICON_SIZE,
    .height = icons::ICON_SIZE,
    .action = IconAction::OpenCalculator,
};
wm.add_desktop_icon(calc_icon);
```

Shell 图标在 (40, 40)，Calculator 在 (40, 120)。它们在桌面左侧纵向排列，80 像素的间距（32 像素图标高度 + 48 像素间隔加标签空间）。图标位图使用 tag 032 中 constexpr 生成的像素数组。

## consteval → constexpr——一个实用的妥协

icon_data.hpp 中有三个函数的声明从 `consteval` 改为了 `constexpr`。consteval 是 C++20 的"强制编译期求值"——如果编译器无法在编译时确定函数的返回值，直接编译失败。constexpr 则是"如果可以就在编译期求值，不行就在运行时求值"。

为什么要降级？在某些编译器配置下（特别是 freestanding kernel build 加上 -O0 调试模式），consteval 可能因为编译器内联限制或调试选项导致编译失败。降级为 constexpr 保留了编译期求值的可能性（因为我们的输入全是字面量），同时允许在特殊情况下退化为运行时求值。

这个妥协在教学 OS 的语境下是合理的——我们的首要目标是代码能编译能运行，而不是追求纯粹的 C++20 特性。如果将来编译器支持变好了，随时可以改回来。

### 与其他操作系统的测试对比

说到测试，xv6-gui 几乎没有 GUI 测试。它的 GUI 是在 xv6 的基础上做的最小扩展，测试主要靠"看得对不对"——在 QEMU 中运行，目测检查窗口和图形是否正确。这种"视觉测试"在教学项目中是常见的，但不可靠——你不能在 CI 中自动运行"目测检查"。

Cinux 的双轨测试策略让 GUI 逻辑可以在 CI 中自动验证。宿主端测试在每次构建时运行，内核端测试在 QEMU 中自动执行。这套测试基础设施是从 tag 029（Canvas）开始逐步建立的，到这个 tag 已经相当成熟。每一个 GUI 组件（Canvas、Window、WindowManager、DesktopIcon、Terminal）都有对应的宿主端 mock 测试和内核端集成测试，累计测试代码量已经超过了实现代码量。

SerenityOS 也有类似的测试策略——它的 LibGfx 和 LibGUI 都有宿主端单元测试（使用自己的 TestFramework），以及集成测试（在 SerenityOS 实例中运行）。不过 SerenityOS 的测试基础设施更完善，支持 GUI 快照对比测试（渲染结果与参考图片比较），这是 Cinux 目前没有的。ToaruOS 的测试相对简单，主要是通过 Python 脚本在 QEMU 中启动系统并验证串口输出——类似于 Cinux 的内核端测试，但没有宿主端 mock 测试。

## 双轨测试——为什么 1658 行测试是必要的

这个 tag 新增了约 1658 行测试代码：宿主端 1123 行 + 内核端 535 行。这个量看起来很大，但考虑到桌面环境涉及多个组件的交互（WindowManager、DesktopIcon、Canvas、Event、Terminal），充分测试是必要的。

### 宿主端测试——算法验证

宿主端测试在开发机上直接运行，编译快、调试方便。它完全重新实现了 WindowManager 的桌面图标相关类型和逻辑——MockCanvas、MockPSFFont、MockWindow、MockWindowManager。为什么要重新实现而不是链接内核代码？因为 WindowManager 依赖太多内核类型（Canvas、PSFFont、Pipe 等），完全解耦的代价大于重新实现。

测试覆盖了几个关键维度。add_desktop_icon 组验证基本注册、多图标注册、容量限制、init 重置。hit_test_icon 组验证命中/未命中/空数组/重叠优先级/边界像素/原点。consume 组验证空消费、一次性消费、多次连续消费。点击分派组验证图标点击设置 action、桌面空白不设置、窗口点击不触发图标。composite 组验证图标像素正确渲染。

一个典型的边界测试是这样的——验证 hit_test 在图标边缘的行为：

```cpp
TEST("desktop: hit_test_icon boundary edge is miss") {
    // ... setup ...
    DesktopIcon icon = make_icon(50, 50, 32, IconAction::None, "X", bmp);
    wm.add_desktop_icon(icon);

    // (82, 60) 是 x=50+32=82，即半开区间 [50, 82) 之外
    ASSERT_NULL(wm.hit_test_icon(82, 60));
    // (81, 81) 是最后一个有效像素
    ASSERT_NOT_NULL(wm.hit_test_icon(81, 81));
}
```

这种边界测试看似琐碎，但在矩形命中测试中 off-by-one 错误非常常见。`contains()` 的实现用的是半开区间 `[x, x+width)`，所以 `x + width` 这个位置是不包含的。如果你不小心写成了 `<=`，边界的命中测试就会差一个像素。

### 内核端测试——集成验证

内核端测试在 QEMU 中运行，使用真实的 Framebuffer、Canvas 和 WindowManager。它验证的是宿主端 mock 无法覆盖的集成问题——比如 composite 是否真的把像素写到了 framebuffer、WindowManager 的 init 是否正确初始化了所有状态。

一个关键的集成测试是 `test_desktop_composite_icons_behind_windows`——它把图标放在 (0,0)，然后创建一个覆盖图标的窗口。composite 后检查重叠区域的像素应该是窗口的内容背景色，而不是图标的颜色。这个测试验证了合成管线的渲染顺序是正确的。

另一个有价值的测试是 `test_desktop_full_scenario`——它模拟了完整的桌面操作：添加图标和窗口、点击 Shell 图标验证 action、再点击窗口验证 raise。这种端到端的测试能发现单元测试遗漏的交互问题。

## 构建与验证

最终构建步骤：

```bash
# 宿主端测试
cd build && cmake --build . --target test_host
ctest -R desktop --output-on-failure

# 内核端测试
cmake --build . --target big_kernel_test
# 在 QEMU 中运行，串口输出应显示所有测试通过

# 完整内核
cmake --build . --target big_kernel
# 在 QEMU 中启动
# 预期：深青色桌面 + 两个图标 + 点击 Shell 弹出终端
```

QEMU 启动后的完整体验应该是：系统启动后显示深青色桌面背景，左侧有两个图标（Shell 和 Calculator），鼠标光标可用。点击 Shell 图标，终端窗口弹出并显示 Shell 提示符。点击桌面空白区域，终端窗口失去焦点（标题栏变灰）。

## 收尾

到这个 tag 结束，我们的 GUI 子系统已经从一个简单的 Canvas 渲染器成长为一个具备完整桌面环境功能的系统。回顾整个 GUI 阶段的进展：tag 029 画第一个像素，tag 030 管理窗口，tag 031 跑终端，tag 032 画图标，tag 033 搭桌面。每一步都在前一步的基础上构建，最终形成了一个可交互的桌面环境。

这个 tag 的代码变更量是 +2000/-88 行——其中约 200 行是实际的功能代码，1658 行是测试代码，其余是构建系统和文档。测试代码量远超实现代码量，这在 GUI 子系统中是正常的——每一帧的渲染结果、每一次的点击响应、每一个的边界条件都需要被自动验证。

但还有很多可以改进的地方：桌面图标目前不支持拖拽排列，没有右键菜单，没有任务栏，点击 Calculator 图标还没有反应。这些都会在后续的 tag 中逐步实现。核心架构已经到位了——剩下的都是在这个基础上添砖加瓦。

从操作系统的角度看，这个 tag 标志着 Cinux 从"一个能跑的内核"进化为"一个有桌面环境的操作系统"。用户不再需要通过串口输入命令——他们可以用鼠标点击图标、在窗口中看到 Shell 的输出、拖拽窗口改变位置。这种从 CLI 到 GUI 的跨越，在操作系统的演进史上是一个经典的里程碑。

完结撒花——桌面环境能跑起来了，给屏幕截张图不过分。

## 参考资料
- CMake Testing: https://cmake.org/cmake/help/latest/command/add_test.html
- SerenityOS Test Framework: https://github.com/SerenityOS/serenity/tree/master/Meta/Lagom/Fuzzers
- xv6-gui: https://github.com/KevinVan720/xv6-gui
- ToaruOS: https://github.com/klange/toaruos
