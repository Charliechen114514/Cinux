# 033-gui-desktop-3: init.cpp 集成与测试代码精讲

## 概览

本文精讲 tag 033 的最后两个部分：init.cpp 启动序列的变化和双轨测试代码。init.cpp 的变更体现了"控制反转"的设计——将终端创建的职责从 init.cpp 转移到 GUI 子系统。测试方面，宿主端 1123 行和内核端 535 行的测试代码共同验证了桌面环境功能的正确性。

关键设计决策：启动序列中管道存储和 GUI 启动的顺序约束、icon_data.hpp consteval → constexpr 兼容性修复、宿主端测试完全重新实现 WindowManager 桌面逻辑。

## 架构图

```
init.cpp 启动序列 (tag 033):
  mount ext2
  create stdin_pipe, PipeReadOps, File → fd 0
  create stdout_pipe, PipeWriteOps, File → fd 1
  set_shell_pipes(stdin_pipe, stdout_pipe)  ← 新增
  gui_start()                               ← 改为 void
  launch_first_user(shell_main)             ← 不变

测试架构：
┌──────────────────────────────────────────┐
│ test/unit/test_desktop.cpp (host)        │
│ MockCanvas, MockPSFFont, MockWindow      │
│ MockWindowManager (重新实现桌面逻辑)      │
│ ~20 个 TEST_CASE                         │
├──────────────────────────────────────────┤
│ kernel/test/test_desktop.cpp (QEMU)      │
│ Framebuffer, Canvas, PSFFont (真实)       │
│ WindowManager (真实)                      │
│ 16 个 TEST_ASSERT 测试函数               │
└──────────────────────────────────────────┘
```

## 代码精讲

### init.cpp 启动序列

init.cpp 的 GUI 部分从 20 行左右变成了 15 行，但结构发生了根本性变化。先看完整的 GUI 初始化块：

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

    cinux::lib::kprintf("[INIT] Terminal-shell pipes connected: stdin_pipe=%p stdout_pipe=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));

    cinux::gui::gui_start();
#endif
```

管道创建的代码完全没变——创建 Pipe 对象，用 ReadOps/WriteOps 包装，绑定到全局 fd 表的 fd 0 和 fd 1。变化在管道创建之后：调用 `set_shell_pipes` 将两个管道指针存入 GUI 子系统，然后调用 `gui_start()`（不再捕获返回值）。

删除的关键行是：

```cpp
// 删除：#include "kernel/gui/terminal.hpp"
// 删除：auto* term = cinux::gui::gui_start();
// 删除：term->set_stdin_pipe(stdin_pipe);
// 删除：term->set_stdout_pipe(stdout_pipe);
```

init.cpp 不再需要知道 Terminal 的存在——它只负责创建管道和启动 GUI，具体如何创建终端窗口是 GUI 子系统的内部事务。

### icon_data.hpp constexpr 修复

三个函数的声明从 consteval 改为 constexpr：

```cpp
// 旧：
consteval uint32_t hex_nibble(char c) { ... }
consteval uint32_t palette_lookup(const uint32_t (&pal)[16], uint32_t nibble) { ... }
template <uint32_t Rows>
consteval std::array<uint32_t, 1024> build_icon(...) { ... }

// 新：
constexpr uint32_t hex_nibble(char c) { ... }
constexpr uint32_t palette_lookup(const uint32_t (&pal)[16], uint32_t nibble) { ... }
template <uint32_t Rows>
constexpr std::array<uint32_t, 1024> build_icon(...) { ... }
```

consteval 要求函数必须在编译期求值，否则编译失败。constexpr 允许编译期或运行时求值。在我们的场景中，build_icon 的输入全部是字面量（调色板数组和 hex 字符串数组），所以编译器仍然会选择编译期求值。降级为 constexpr 只是为了在特殊编译配置下（如调试模式、特定优化级别）保留回退能力。

### 内核端测试 test_desktop.cpp

内核端测试使用真实的 Framebuffer 和 Canvas 对象，直接调用 WindowManager 的 API。测试入口函数初始化硬件环境：

```cpp
extern "C" void run_desktop_tests() {
    TEST_SECTION("Desktop Tests (033_gui_desktop)");

    static constexpr uintptr_t BOOT_INFO_PHYS = 0x7000;
    auto* bi = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
    g_fb.init(*bi);
    g_font.init();
    g_fb.clear(0);
    g_screen.init(g_fb);

    fill_icon_solid(g_icon_shell, 0x00FFFFFF);  // 白色
    fill_icon_solid(g_icon_calc, 0x00FF8C00);   // 橙色
    // ... RUN_TEST 各个测试 ...
}
```

测试图标使用纯色填充（白色 Shell 图标、橙色 Calculator 图标），不需要复杂的像素图案——测试关注的是位置和命中测试，不是图像内容。

测试分组和关键测试如下：

**WM 初始化和图标注册（3 个测试）**：验证 init 后添加图标成功、容量满后返回 false、多个图标分别可命中。

**hit_test_icon 边界（3 个测试）**：空 WM 的 hit_test 返回 nullptr、边界像素（左上角命中、右下角+1 像素未命中）、重叠时后注册图标优先。

**consume 和点击分派（4 个测试）**：无 pending 时 consume 返回 None、点击图标设置 action 并可 consume、桌面空白点击不设 action、窗口点击不触发图标 action。

**composite 渲染（4 个测试）**：只有图标时 composite 不崩溃、图标和窗口共存不崩溃、空桌面不崩溃、窗口覆盖图标时窗口像素优先。

**完整场景（2 个测试）**：模拟完整的桌面操作——先添加图标和窗口、点击 Shell 图标验证 action、再点击窗口验证 raise。

值得注意的是 `test_desktop_composite_icons_behind_windows` 测试：

```cpp
void test_desktop_composite_icons_behind_windows() {
    WindowManager wm;
    wm.init(&g_screen, &g_font);
    wm.add_desktop_icon(make_shell_icon(0, 0));
    wm.create("A", 100, 50);

    g_fb.clear(0);
    wm.composite();

    TEST_ASSERT_EQ(g_fb.get_pixel(5, 25), Window::COLOR_CONTENT_BG);
}
```

图标在 (0, 0)，窗口也在 (0, 0) 且覆盖了图标区域。composite 后检查 (5, 25) 的像素——应该是窗口的内容背景色而不是图标的颜色。这个测试验证了 "图标在窗口之下" 的渲染顺序。

### 宿主端测试 test_desktop.cpp

宿主端测试完全重新实现了 WindowManager 的桌面图标相关逻辑。这套代码有 1123 行，但大部分是 mock 基础设施。让我们按组看关键测试。

MockCanvas 实现了完整的像素写入、draw_bitmap、clear 等方法。关键的是 `pixel()` 访问器——测试用它在 composite 后检查特定像素的值。MockWindowManager 完整复现了 add_desktop_icon、hit_test_icon、consume_pending_icon_action、handle_mouse 的桌面图标逻辑。

add_desktop_icon 测试组验证基本功能：

```cpp
TEST("desktop: add_desktop_icon increments icon count") {
    MockWindowManager wm;
    init_wm(wm, &screen, &font);
    ASSERT_EQ(wm.icon_count(), 0u);

    std::vector<uint32_t> bmp;
    DesktopIcon icon = make_icon(10, 10, 32, IconAction::OpenShell, "Shell", bmp);
    bool ok = wm.add_desktop_icon(icon);
    ASSERT_TRUE(ok);
    ASSERT_EQ(wm.icon_count(), 1u);
}
```

hit_test_icon 测试组覆盖了命中、未命中、无图标、Z 优先级、边界和原点等多种场景：

```cpp
TEST("desktop: hit_test_icon later icon takes priority on overlap") {
    // 两个重叠图标，后注册的应该优先命中
    DesktopIcon icon1 = make_icon(50, 50, 32, IconAction::OpenShell, "Shell", bmp1);
    DesktopIcon icon2 = make_icon(60, 60, 32, IconAction::OpenCalculator, "Calculator", bmp2);
    wm.add_desktop_icon(icon1);
    wm.add_desktop_icon(icon2);

    const DesktopIcon* hit = wm.hit_test_icon(70, 70);
    ASSERT_EQ(hit->action, IconAction::OpenCalculator);
}
```

consume 测试组验证一次性消费行为：

```cpp
TEST("desktop: consume_pending_icon_action resets to None") {
    // 点击图标 → 第一次 consume 返回 OpenShell → 第二次 consume 返回 None
    Event ev{};
    ev.type_ = EventType::MouseDown;
    ev.mouse.x = 60; ev.mouse.y = 60; ev.mouse.left = true;
    wm.handle_mouse(ev);

    IconAction action1 = wm.consume_pending_icon_action();
    ASSERT_EQ(static_cast<int>(action1), static_cast<int>(IconAction::OpenShell));

    IconAction action2 = wm.consume_pending_icon_action();
    ASSERT_EQ(static_cast<int>(action2), static_cast<int>(IconAction::None));
}
```

图标点击分派测试验证了三种点击场景的互斥性——图标点击设置 action、桌面空白点击不设 action、窗口点击不触发图标检测。

composite 渲染测试直接检查像素值：

```cpp
TEST("desktop: composite renders icon bitmap") {
    uint32_t icon_color = 0xFF00FF00;
    std::vector<uint32_t> bmp(32 * 32, icon_color);
    DesktopIcon icon{ .x = 10, .y = 10, .bitmap = bmp.data(), ... };
    wm.add_desktop_icon(icon);
    wm.composite();

    ASSERT_EQ(screen.pixel(20, 20), icon_color);       // 图标内
    ASSERT_EQ(screen.pixel(50, 50), DESKTOP_COLOR);     // 图标外
}
```

### CMake 集成

两个 CMakeLists.txt 都需要更新。kernel/CMakeLists.txt 将 test_desktop.cpp 添加到 big_kernel_test 的源文件列表。test/CMakeLists.txt 新增 test_desktop 可执行文件，并将其加入 test_host、test_all、test_verbose 的依赖列表。

## 设计决策

### 决策：宿主端完全重新实现 vs 链接内核代码
**问题**: 宿主端测试如何获得 WindowManager 的桌面图标逻辑？
**本项目的做法**: 在 test/unit/test_desktop.cpp 中用 Mock 类重新实现
**备选方案**: 提取桌面图标逻辑为独立头文件，宿主端直接 include
**为什么不选备选方案**: WindowManager 依赖太多内核类型（Canvas、PSFFont、Pipe），完全解耦的代价大于重新实现。Mock 方案虽然代码量大，但完全独立，编译快，不依赖内核构建。
**如果要扩展**: 如果桌面图标逻辑变得更复杂（比如拖拽排列），可以考虑将核心算法提取为 header-only 的模板函数，宿主端和内核端共用。

## 扩展方向

1. (⭐) 添加图标双击 vs 单击的测试——单击选中高亮，双击启动应用
2. (⭐) 测试图标文字渲染——验证标签居中和颜色
3. (⭐⭐) 添加性能测试——测量 16 个图标的 composite 帧时间
4. (⭐⭐) 测试管道缓冲区溢出场景——Shell 在 Terminal 连接前大量输出，验证不崩溃
5. (⭐⭐⭐) 端到端测试——从 boot 到桌面到点击图标到终端输出的完整流程自动化验证

## 参考资料
- CMake Test: https://cmake.org/cmake/help/latest/command/add_test.html
- SerenityOS Test Framework: https://github.com/SerenityOS/serenity/tree/master/Tests
- ToaruOS Tests: https://github.com/klange/toaruos/tree/master/tests
