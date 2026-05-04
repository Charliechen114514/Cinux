# 033-gui-desktop-2: GUI 初始化与应用启动器代码精讲

## 概览

本文精讲 tag 033 中 `gui_init.hpp`、`gui_init.cpp`、`window.hpp`、`terminal.hpp` 的变更。核心变化是 GUI 启动流程的架构性重构：`gui_start()` 不再创建终端窗口，改为注册桌面图标；终端窗口延迟到用户点击 Shell 图标时才创建；管道指针通过模块变量存储，在创建终端时连接。

关键设计决策：控制反转（init.cpp 不再操作 Terminal）、延迟终端创建（点击图标才弹出）、is_terminal() 类型安全 downcast。

## 架构图

```
旧流程 (tag 032):
init.cpp → gui_start() → Terminal*
         → term->set_stdin_pipe()
         → term->set_stdout_pipe()

新流程 (tag 033):
init.cpp → set_shell_pipes(stdin, stdout)  // 存储管道指针
         → gui_start()                     // 注册图标，void 返回
         → [用户点击 Shell 图标]
         → gui_tick_callback:
             consume_pending_icon_action()
             → create_shell_terminal()      // 延迟创建
               → new Terminal
               → term->set_stdin_pipe(g_stdin_pipe)
               → term->set_stdout_pipe(g_stdout_pipe)
               → wm.add_window(term)
```

## 代码精讲

### gui_init.hpp 接口变更

头文件的变更反映了接口的根本性变化。首先新增了 Pipe 的前向声明：

```cpp
namespace cinux::ipc { class Pipe; }
```

前向声明替代了完整 include，避免了头文件循环依赖。原来的 `class Terminal;` 前向声明被删除——gui_init.hpp 不再暴露 Terminal 类型。

然后是新的 `set_shell_pipes` 声明：

```cpp
void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe);
```

这个函数的文档注释很清楚地说明了调用时机约束："Must be called before gui_start()"。参数说明中标注了 stdin_pipe 是 "Terminal writes to it"（Terminal 将用户的键盘输入写入 stdin 管道，Shell 从中读取），stdout_pipe 是 "Terminal reads from it"（Terminal 从 stdout 管道读取 Shell 的输出并显示）。

最关键的变化是 gui_start 的签名：

```cpp
// 旧签名：
Terminal* gui_start();

// 新签名：
void gui_start();
```

删除返回值意味着调用方（init.cpp）无法拿到 Terminal 指针。这是故意的——Terminal 的创建被封装在 gui_init 内部，外部不需要知道何时、如何创建。

### gui_init.cpp 管道存储

匿名命名空间中新增了两个模块级静态变量：

```cpp
namespace {
cinux::ipc::Pipe* g_stdin_pipe  = nullptr;
cinux::ipc::Pipe* g_stdout_pipe = nullptr;
}
```

这两个指针的生命周期是从 `set_shell_pipes()` 调用到内核关机。它们存储的是 init.cpp 中创建的管道对象的地址，gui_init 不拥有这些管道——只持有引用。

`set_shell_pipes` 的实现非常简单：

```cpp
void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe) {
    g_stdin_pipe  = stdin_pipe;
    g_stdout_pipe = stdout_pipe;
    cinux::lib::kprintf("[GUI] Shell pipes stored: stdin=%p stdout=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));
}
```

kprintf 打印管道地址是调试利器——如果地址是 0x0，说明 set_shell_pipes 没被调用或者传了 nullptr。

### create_shell_terminal helper

这个内部函数封装了"创建终端 + 连接管道 + 添加到 WM"的完整流程：

```cpp
void create_shell_terminal() {
    auto& wm = WindowManager::instance();

    uint32_t term_w = Terminal::COLS * 8;   // 80 * 8 = 640
    uint32_t term_h = Terminal::ROWS * 16;  // 25 * 16 = 400

    uint32_t term_x = 80;
    uint32_t term_y = 60;

    if (g_screen != nullptr) {
        uint32_t sw = g_screen->width();
        uint32_t sh = g_screen->height();
        if (term_w + 80 < sw) {
            term_x = (sw - term_w) / 2;
        }
        if (term_h + 60 < sh) {
            term_y = (sh - term_h) / 2;
        }
    }

    auto* term = new Terminal(term_x, term_y, "Cinux Terminal");
    term->set_font(g_font);

    if (g_stdin_pipe != nullptr) {
        term->set_stdin_pipe(g_stdin_pipe);
    }
    if (g_stdout_pipe != nullptr) {
        term->set_stdout_pipe(g_stdout_pipe);
    }

    wm.add_window(term);
}
```

这段代码做了三件事。首先计算终端的尺寸和屏幕居中位置。终端尺寸来自 Terminal 的编译期常量 `COLS * 8` 和 `ROWS * 16`。居中逻辑检查屏幕是否足够大（终端宽度加 80 像素边距 < 屏幕宽度），如果是则居中，否则使用默认偏移。

然后创建 Terminal 对象。`new` 在内核堆上分配，字体通过 `set_font` 设置。注意这里使用了 g_font 模块变量——这个变量在 `gui_init()` 中被设置，所以 gui_init 必须在 create_shell_terminal 之前被调用。

最后连接管道。两个 `if (pipe != nullptr)` 检查确保即使 set_shell_pipes 没被调用，终端仍然会创建（只是没有 Shell 连接）。这是一个防御性的设计选择——宁可创建一个空终端也不要崩溃。

### gui_tick_callback 修改

tick callback 新增了两个逻辑块。第一个是消费图标动作：

```cpp
IconAction action = wm.consume_pending_icon_action();
if (action == IconAction::OpenShell) {
    create_shell_terminal();
}
```

这段代码在处理完输入事件后执行。`consume_pending_icon_action()` 返回当前的 pending action 并重置为 None。如果返回的是 OpenShell，就调用 create_shell_terminal 创建终端窗口。未来添加 OpenCalculator 支持只需要在这里加一个 `else if`。

第二个修改是 focused 窗口的类型安全检查：

```cpp
auto* focused = wm.focused();
if (focused != nullptr && focused->is_terminal()) {
    auto* term = static_cast<Terminal*>(focused);
    term->poll_output();
    term->render_to_canvas();
}
```

原来只检查 `focused != nullptr`，现在加了 `is_terminal()` 检查。这样当 focused 窗口是非 Terminal 类型时（比如未来的 Calculator），不会错误地调用 Terminal 特有的 poll_output 和 render_to_canvas。

### gui_start 重构

新版 gui_start 删除了终端创建逻辑，替换为图标注册：

```cpp
void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop =====\n");
    cinux::drivers::Mouse::init();
    // ... screen bounds ...

    auto& wm = WindowManager::instance();

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

    cinux::drivers::PIT::set_tick_callback(gui_tick_callback, nullptr);
}
```

Shell 图标位于 (40, 40)，Calculator 图标位于 (40, 120)。两个图标的 x 坐标相同，y 坐标相差 80 像素（32 像素图标高度 + 48 像素间距）。这个布局让图标在桌面左侧纵向排列，类似经典桌面环境。

图标位图使用 tag 032 中定义的 constexpr 数组 `k_shell_icon` 和 `k_calc_icon`，`.data()` 返回指向底层 uint32_t 数组的指针。

### window.hpp — is_terminal 虚函数

```cpp
virtual bool is_terminal() const { return false; }
```

一行代码，但意义重大。Window 基类默认返回 false，所有非 Terminal 的窗口类型（未来的 Calculator、文件管理器等）不需要做任何事情。只有 Terminal 需要 override：

```cpp
// terminal.hpp
bool is_terminal() const override { return true; }
```

这个模式的好处是避免了 RTTI 开销——`dynamic_cast<Terminal*>` 需要编译器启用 RTTI 并维护 type_info 结构，在 freestanding kernel 中通常不可用。用虚函数做类型标签是零成本的（只是 vtable 中多了一个函数指针），而且语义清晰。

### terminal.cpp — 析构函数变更

```cpp
Terminal::~Terminal() {
    stdin_pipe_  = nullptr;
    stdout_pipe_ = nullptr;
}
```

原来的析构函数会调用 `stdin_pipe_->close_writer()` 和 `stdout_pipe_->close_reader()`。现在只清空指针。这个变化的原因是管道所有权从 Terminal 转移到了 init.cpp。如果 Terminal 关闭管道，Shell 进程会收到 EOF（stdin）或写入错误（stdout），导致 Shell 退出。在多终端场景下，关掉一个终端不应该杀掉 Shell 进程。

## 设计决策

### 决策：延迟终端创建 vs 启动时创建
**问题**: 何时创建 Terminal 窗口？
**本项目的做法**: 用户点击 Shell 图标时
**备选方案**: gui_start 时立即创建（旧方案）
**为什么不选备选方案**: 启动时创建将 GUI 初始化和终端绑定死，无法支持"关闭终端再开"或"不打开终端"。延迟创建还让桌面更接近真实桌面环境的体验。
**如果要扩展**: 可以添加"记住上次打开的窗口"功能——在启动时检查配置文件，自动打开之前打开过的应用。

### 决策：is_terminal() vs dynamic_cast
**问题**: 如何安全地判断 focused 窗口是否是 Terminal？
**本项目的做法**: virtual bool is_terminal() const
**备选方案**: dynamic_cast<Terminal*>(focused) != nullptr
**为什么不选备选方案**: freestanding kernel 通常禁用 RTTI（-fno-rtti），dynamic_cast 不可用。虚函数方案零额外开销。
**如果要扩展**: 如果窗口类型增多（5 种以上），可以考虑用 enum type tag 替代多个 is_xxx() 虚函数。

## 扩展方向

1. (⭐) 支持从文件系统读取图标配置——类似 .desktop 文件，定义图标名称、位图路径、启动命令
2. (⭐⭐) 实现"关闭终端后重启"——监听 Terminal 窗口的关闭事件，重新允许 Shell 图标点击
3. (⭐⭐) 支持多终端——每次点击 Shell 图标创建新的终端 + 新的管道 + 新的 Shell 进程
4. (⭐⭐⭐) 实现 Calculator 应用——处理 OpenCalculator action，创建 Calculator 窗口

## 参考资料
- SerenityOS WindowServer: https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer
- ToaruOS Yutani Compositor: https://github.com/klange/toaruos/tree/master/apps
- OSDev Wiki PS/2 Mouse: https://wiki.osdev.org/PS/2_Mouse
