# 033-gui-desktop-3: 把所有部件连起来——GUI 初始化与测试验证

> 标签：GUI, kernel init, compositing, testing, CMake
> 前置：[033-gui-desktop-2](033-gui-desktop-2.md)

## 前言

前两篇我们分别实现了桌面图标管理和应用启动器。现在要做的是把这些新代码和内核的启动序列正确地串起来，确保整个 GUI 子系统在新架构下工作正常，并且用充分的测试来验证。

说实话，这个 tag 最让我满意的部分不是某个特定的算法或数据结构，而是 init.cpp 变得非常干净——它创建管道、调用 set_shell_pipes 存储管道指针、调用 gui_start 注册图标和 PIT callback，整个过程一气呵成。Terminal 的创建完全封装在 gui_init.cpp 内部，init.cpp 不再需要知道 Terminal 的存在。

## 环境说明

本篇涉及 `init.cpp`、`kernel/test/test_desktop.cpp`、`test/unit/test_desktop.cpp` 和两个 CMakeLists.txt。新增测试代码约 1658 行，是本 tag 代码量最大的部分。

## 启动序列——管道先行，GUI 在后

init.cpp 的 GUI 部分按照严格的时序执行：先创建管道并绑定 fd 0/fd 1，然后调用 set_shell_pipes 存储管道指针，最后调用 gui_start 注册图标和 PIT callback。

```cpp
#ifdef CINUX_GUI
    // 1. 创建 stdin 管道
    auto* stdin_pipe = new cinux::ipc::Pipe();
    auto* stdin_read_ops = new cinux::ipc::PipeReadOps(stdin_pipe);
    auto* stdin_read_inode = new cinux::fs::Inode();
    stdin_read_inode->ops = stdin_read_ops;
    stdin_read_inode->type = cinux::fs::InodeType::Regular;

    auto* stdin_file = new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY);
    cinux::fs::g_global_fd_table().set(0, stdin_file);

    // 2. 创建 stdout 管道
    auto* stdout_pipe = new cinux::ipc::Pipe();
    auto* stdout_write_ops = new cinux::ipc::PipeWriteOps(stdout_pipe);
    auto* stdout_write_inode = new cinux::fs::Inode();
    stdout_write_inode->ops = stdout_write_ops;
    stdout_write_inode->type = cinux::fs::InodeType::Regular;

    auto* stdout_file = new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY);
    cinux::fs::g_global_fd_table().set(1, stdout_file);

    // 3. 存储管道指针
    cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);

    cinux::lib::kprintf("[INIT] Terminal-shell pipes connected: stdin_pipe=%p stdout_pipe=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));

    // 4. 启动 GUI
    cinux::gui::gui_start();
#endif
```

之前 init.cpp 需要知道 Terminal 的类型和管道连接方式——`auto* term = gui_start()` 然后 `term->set_stdin_pipe/set_stdout_pipe`。现在它只需要 include gui_init.hpp，调用两个函数。gui_start() 不再返回 Terminal*——返回类型从 `Terminal*` 变成了 `void`。管道连接在 create_shell_terminal 内部完成，对 init.cpp 完全透明。

### 为什么 set_shell_pipes 必须在 gui_start 之前

时序很重要。set_shell_pipes 将管道指针存入 gui_init 模块的匿名命名空间变量。gui_start 注册了 PIT tick callback，之后每个 tick 都可能触发 handle_mouse -> consume_pending_icon_action -> create_shell_terminal。如果管道指针还没存储就触发了终端创建，g_stdin_pipe 和 g_stdout_pipe 都是 nullptr，Terminal 虽然会创建但不会连接管道。

### gui_start 中的图标注册

gui_start 的图标注册部分我们已经在前一篇看过了。Shell 图标在 (40, 40)，Calculator 在 (40, 120)。它们在桌面左侧纵向排列，80 像素的间距（32 像素图标高度 + 48 像素间隔加标签空间）。图标位图使用 tag 032 中 constexpr 生成的像素数组。

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

这种边界测试看似琐碎，但在矩形命中测试中 off-by-one 错误非常常见。`contains()` 的实现用的是半开区间 `[x, x+width)`，所以 `x + width` 这个位置是不包含的。

### 内核端测试——集成验证

内核端测试在 QEMU 中运行，使用真实的 Framebuffer、Canvas 和 WindowManager。它验证的是宿主端 mock 无法覆盖的集成问题——比如 composite 是否真的把像素写到了 framebuffer、WindowManager 的 init 是否正确初始化了所有状态。

一个关键的集成测试是 `test_desktop_composite_icons_behind_windows`——它把图标放在 (0,0)，然后创建一个覆盖图标的窗口。composite 后检查重叠区域的像素应该是窗口的内容背景色，而不是图标的颜色。这个测试验证了合成管线的渲染顺序是正确的。

### 与其他操作系统的测试对比

xv6-gui 几乎没有 GUI 测试——它依赖"在 QEMU 中跑起来看看"来验证 GUI 功能。这种"视觉测试"在教学项目中是常见的，但不可靠——你不能在 CI 中自动运行"目测检查"。

Cinux 的双轨测试策略让 GUI 逻辑可以在 CI 中自动验证。宿主端测试在每次构建时运行，内核端测试在 QEMU 中自动执行。SerenityOS 也有类似的测试策略——它的 LibGfx 和 LibGUI 都有宿主端单元测试，以及集成测试。不过 SerenityOS 的测试基础设施更完善，支持 GUI 快照对比测试（渲染结果与参考图片比较），这是 Cinux 目前没有的。

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

QEMU 启动后的完整体验应该是：系统启动后显示深青色桌面背景，左侧有两个图标（Shell 和 Calculator），鼠标光标可用。点击 Shell 图标，终端窗口弹出并显示 Shell 提示符——Shell 的输出在终端创建前暂存在 pipe buffer 中，终端连接后通过 poll_output 取回。点击桌面空白区域，终端窗口失去焦点。再点击终端标题栏，终端重新获得焦点。

## 收尾

到这个 tag 结束，我们的 GUI 子系统已经从一个简单的 Canvas 渲染器成长为一个具备完整桌面环境功能的系统。回顾整个 GUI 阶段的进展：tag 029 画第一个像素，tag 030 管理窗口，tag 031 跑终端，tag 032 画图标，tag 033 搭桌面。每一步都在前一步的基础上构建，最终形成了一个可交互的桌面环境。

这个 tag 的核心贡献是三个：桌面图标管理（注册、渲染、命中测试）、延迟终端创建（set_shell_pipes + create_shell_terminal）、类型安全的窗口 downcast（is_terminal()）。架构上最关键的决策是把 Terminal 的创建从 gui_start 延迟到用户点击图标时，通过 set_shell_pipes 实现管道的延迟绑定——这让 init.cpp 不再需要直接操作 Terminal，也让终端创建时机更加灵活。

但还有很多可以改进的地方：桌面图标目前不支持拖拽排列，没有右键菜单，没有任务栏，点击 Calculator 图标还没有反应，多个终端共享同一对管道限制了多终端的独立性。这些都会在后续的 tag 中逐步实现。核心架构已经到位了——剩下的都是在这个基础上添砖加瓦。

完结撒花——桌面环境能跑起来了，给屏幕截张图不过分。

## 参考资料
- CMake Testing: https://cmake.org/cmake/help/latest/command/add_test.html
- SerenityOS Test Framework: https://github.com/SerenityOS/serenity/tree/master/Meta/Lagom/Fuzzers
- xv6-gui: https://github.com/KevinVan720/xv6-gui
- ToaruOS: https://github.com/klange/toaruos
