# 033-gui-desktop-3: init.cpp 集成与测试代码精讲

## 概览

本文精讲 tag 033 的最后两个部分：init.cpp 启动序列的变化和双轨测试代码。init.cpp 的变更体现了"管道先行"的设计——先创建管道和绑定 fd，然后通过 set_shell_pipes() 存储管道指针，最后调用 gui_start()。测试方面，宿主端 1123 行和内核端 535 行的测试代码共同验证了桌面环境功能的正确性。

关键设计决策：init.cpp 创建管道并调用 set_shell_pipes()、gui_start() 不再返回 Terminal*、终端创建延迟到用户点击时。

## 架构图

```
init.cpp 启动序列 (tag 033):
  mount ext2
  vfs_mount_init()
  vfs_mount_add("/", &ext2)
  创建 stdin Pipe + PipeReadOps + Inode + File -> fd 0
  创建 stdout Pipe + PipeWriteOps + Inode + File -> fd 1
  set_shell_pipes(stdin_pipe, stdout_pipe)   // 存储管道指针
  gui_start()                                 // 注册图标 + PIT callback
  launch_first_user("/bin/sh")                // Shell 在 boot 时启动

PIT tick callback:
  handle_mouse -> pending_icon_action_
  consume -> create_shell_terminal()          // 使用预存储的管道
  focused->is_terminal() -> poll_output + render_to_canvas
  composite

测试架构：
+------------------------------------------+
| test/unit/test_desktop.cpp (host)        |
| MockCanvas, MockPSFFont, MockWindow      |
| MockWindowManager (重新实现桌面逻辑)      |
| ~20 个 TEST_CASE                         |
+------------------------------------------+
| kernel/test/test_desktop.cpp (QEMU)      |
| Framebuffer, Canvas, PSFFont (真实)       |
| WindowManager (真实)                      |
| 16 个 TEST_ASSERT 测试函数               |
+------------------------------------------+
```

## 代码精讲

### init.cpp 启动序列

init.cpp 的 GUI 部分做了三件事：创建管道并绑定 fd、调用 set_shell_pipes、调用 gui_start。

```cpp
void kernel_init_thread() {
    // ... ext2 mount, VFS init ...

#ifdef CINUX_GUI
    // Create stdin pipe: Terminal on_key -> shell sys_read(0)
    auto* stdin_pipe = new cinux::ipc::Pipe();
    auto* stdin_read_ops = new cinux::ipc::PipeReadOps(stdin_pipe);
    auto* stdin_read_inode = new cinux::fs::Inode();
    stdin_read_inode->ops = stdin_read_ops;
    stdin_read_inode->type = cinux::fs::InodeType::Regular;

    auto* stdin_file = new cinux::fs::File(stdin_read_inode, 0, cinux::fs::OpenFlags::RDONLY);
    cinux::fs::g_global_fd_table().set(0, stdin_file);

    // Create stdout pipe: shell sys_write(1) -> Terminal poll_output
    auto* stdout_pipe = new cinux::ipc::Pipe();
    auto* stdout_write_ops = new cinux::ipc::PipeWriteOps(stdout_pipe);
    auto* stdout_write_inode = new cinux::fs::Inode();
    stdout_write_inode->ops = stdout_write_ops;
    stdout_write_inode->type = cinux::fs::InodeType::Regular;

    auto* stdout_file = new cinux::fs::File(stdout_write_inode, 0, cinux::fs::OpenFlags::WRONLY);
    cinux::fs::g_global_fd_table().set(1, stdout_file);

    // Store pipe pointers for the GUI subsystem
    cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);

    cinux::lib::kprintf("[INIT] Terminal-shell pipes connected: stdin_pipe=%p stdout_pipe=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));

    // Start the GUI: mouse init, desktop icons, PIT tick callback
    cinux::gui::gui_start();
#endif

    // ... launch_first_user, syscall setup ...
}
```

和之前版本的区别在于：
1. 删除了 `#include "kernel/gui/terminal.hpp"`——init.cpp 不再直接操作 Terminal
2. 删除了 `auto* term = cinux::gui::gui_start()`——gui_start 不再返回 Terminal*
3. 删除了 `term->set_stdin_pipe/set_stdout_pipe`——管道连接在 create_shell_terminal 内部完成
4. 新增了 `set_shell_pipes()` 调用——在 gui_start 之前存储管道指针
5. gui_start() 调用移到了 set_shell_pipes 之后

注意 init.cpp 仍然创建管道和绑定 fd——这是 Shell 进程的 stdin/stdout。管道的创建时序没有变，只是 Terminal 的连接时序从"init.cpp 直接操作"变成了"通过 set_shell_pipes 间接延迟连接"。

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

值得注意的是 `test_desktop_composite_icons_behind_windows` 测试——它把图标放在 (0,0)，然后创建一个覆盖图标的窗口。composite 后检查重叠区域的像素应该是窗口的内容背景色，而不是图标的颜色。这个测试验证了 "图标在窗口之下" 的渲染顺序。

### 宿主端测试 test_desktop.cpp

宿主端测试完全重新实现了 WindowManager 的桌面图标相关逻辑。这套代码有 1123 行，但大部分是 mock 基础设施。

MockCanvas 实现了完整的像素写入、draw_bitmap、clear 等方法。关键是 `pixel()` 访问器——测试用它在 composite 后检查特定像素的值。MockWindowManager 完整复现了 add_desktop_icon、hit_test_icon、consume_pending_icon_action、handle_mouse 的桌面图标逻辑。

add_desktop_icon 测试组验证基本功能，hit_test_icon 测试组覆盖了命中、未命中、无图标、Z 优先级、边界和原点等多种场景，consume 测试组验证一次性消费行为，composite 渲染测试直接检查像素值。

### CMake 集成

两个 CMakeLists.txt 都需要更新。kernel/CMakeLists.txt 将 test_desktop.cpp 添加到 big_kernel_test 的源文件列表。test/CMakeLists.txt 新增 test_desktop 可执行文件，并将其加入 test_host、test_all、test_verbose 的依赖列表。

## 设计决策

### 决策：宿主端完全重新实现 vs 链接内核代码
**问题**: 宿主端测试如何获得 WindowManager 的桌面图标逻辑？
**本项目的做法**: 在 test/unit/test_desktop.cpp 中用 Mock 类重新实现
**备选方案**: 提取桌面图标逻辑为独立头文件，宿主端直接 include
**为什么不选备选方案**: WindowManager 依赖太多内核类型（Canvas、PSFFont、Pipe），完全解耦的代价大于重新实现。Mock 方案虽然代码量大，但完全独立，编译快，不依赖内核构建。
**如果要扩展**: 如果桌面图标逻辑变得更复杂（比如拖拽排列），可以考虑将核心算法提取为 header-only 的模板函数，宿主端和内核端共用。

### 决策：set_shell_pipes 延迟绑定 vs init.cpp 直接连接
**问题**: 管道应该如何连接到 Terminal？
**本项目的做法**: init.cpp 调用 set_shell_pipes 存储管道指针，create_shell_terminal 延迟连接
**备选方案**: gui_start 返回 Terminal*，init.cpp 直接调用 set_stdin_pipe/set_stdout_pipe
**为什么选当前方案**: gui_start 不再创建 Terminal，所以无法返回 Terminal*。延迟连接让 GUI 子系统拥有终端创建的完全控制权。
**如果要扩展**: 多终端场景下 set_shell_pipes 的单管道对模型不再适用，需要改为 per-terminal 管道。

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
