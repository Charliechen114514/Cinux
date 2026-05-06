# 033-gui-desktop-2: GUI 初始化与应用启动器代码精讲

## 概览

本文精讲 tag 033 中 `gui_init.hpp`、`gui_init.cpp`、`window.hpp`、`terminal.hpp`、`terminal.cpp` 的变更。核心变化是 GUI 启动流程的架构性重构：`gui_start()` 不再创建终端窗口，改为注册桌面图标；终端窗口延迟到用户点击 Shell 图标时才创建，连接预先存储的管道指针。init.cpp 先创建管道并调用 `set_shell_pipes()`，再调用 `gui_start()`。

关键设计决策：控制反转（init.cpp 不再操作 Terminal 指针）、延迟终端创建（点击图标才弹出）、set_shell_pipes 延迟绑定管道、is_terminal() 类型安全 downcast、Terminal 析构函数简化（不关闭外部管道）。

## 架构图

```
新流程 (tag 033):
init.cpp -> 创建 stdin/stdout 管道 -> 绑定 fd 0/fd 1
         -> set_shell_pipes(stdin_pipe, stdout_pipe)
         -> gui_start()                        // 注册图标 + PIT callback

PIT tick callback:
  handle_mouse -> pending_icon_action_
  consume_pending_icon_action()
    if OpenShell -> create_shell_terminal()
      -> new Terminal (居中定位)
      -> term->set_stdin_pipe(g_stdin_pipe)   // 从模块变量取管道
      -> term->set_stdout_pipe(g_stdout_pipe)
      -> wm.add_window(term)
  if focused->is_terminal() -> poll_output + render_to_canvas
  composite
```

## 代码精讲

### gui_init.hpp 接口

gui_init.hpp 提供三个公开函数。和之前不同的是，`gui_start()` 不再返回 Terminal 指针，新增了 `set_shell_pipes()` 用于存储管道指针：

```cpp
namespace cinux::ipc { class Pipe; }

namespace cinux::gui {

void gui_init(cinux::drivers::Canvas& screen, cinux::drivers::PSFFont& font);

/**
 * Store the shell pipe pointers for later terminal creation.
 * Must be called before gui_start().
 */
void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe);

/**
 * Register the GUI tick callback on the PIT.
 * Registers desktop icons (Shell, Calculator) on the desktop.
 */
void gui_start();

}  // namespace cinux::gui
```

`gui_init` 在 kernel_main 中调用，设置 Canvas 和字体。`set_shell_pipes` 在 init.cpp 中调用，在管道创建之后、gui_start 之前。`gui_start` 在 kernel_init_thread 中调用，注册桌面图标和 PIT tick callback——注意它返回 void，不再返回 Terminal*。

gui_init.hpp 通过前向声明 `cinux::ipc::Pipe` 避免了暴露 Pipe 头文件。Terminal 的前向声明已被移除——gui_init.hpp 不再需要暴露 Terminal 类型。

### gui_init.cpp 模块内部状态

匿名命名空间中维护了四个模块级变量：

```cpp
namespace {
cinux::drivers::Canvas*  g_screen = nullptr;
cinux::drivers::PSFFont* g_font   = nullptr;

// Shell pipe pointers set by set_shell_pipes() before gui_start()
cinux::ipc::Pipe* g_stdin_pipe  = nullptr;
cinux::ipc::Pipe* g_stdout_pipe = nullptr;
}
```

`g_screen` 和 `g_font` 在 gui_init 中设置。`g_stdin_pipe` 和 `g_stdout_pipe` 在 set_shell_pipes 中设置——它们存储的是 init.cpp 预先创建的管道指针，用于在 create_shell_terminal 中连接到 Terminal。

注意这里没有原子变量。管道指针只被设置一次（init.cpp 调用 set_shell_pipes），之后只被读取（create_shell_terminal 使用），不存在并发写入的问题，所以普通的 Pipe 指针就足够了。

### set_shell_pipes——存储管道指针

```cpp
void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe) {
    g_stdin_pipe  = stdin_pipe;
    g_stdout_pipe = stdout_pipe;
    cinux::lib::kprintf("[GUI] Shell pipes stored: stdin=%p stdout=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));
}
```

一个简单的赋值函数。init.cpp 在创建好管道后调用它，将管道指针存入模块变量。日志打印管道地址，方便调试时确认存储成功。

### create_shell_terminal helper——延迟终端创建

这个内部函数在 gui_tick_callback 消费到 OpenShell action 时被调用。它封装了"创建 Terminal + 连接预存储的管道"的流程：

```cpp
void create_shell_terminal() {
    auto& wm = WindowManager::instance();

    // Calculate terminal dimensions
    uint32_t term_w = Terminal::COLS * 8;   // 80 * 8 = 640
    uint32_t term_h = Terminal::ROWS * 16;  // 25 * 16 = 400

    // Centre the terminal on screen if possible
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

    // Connect shell pipes if available
    if (g_stdin_pipe != nullptr) {
        term->set_stdin_pipe(g_stdin_pipe);
    }
    if (g_stdout_pipe != nullptr) {
        term->set_stdout_pipe(g_stdout_pipe);
    }

    wm.add_window(term);
    cinux::lib::kprintf("[GUI] Shell terminal created and connected.\n");
}
```

终端尺寸来自 Terminal 的编译期常量 `COLS * 8` 和 `ROWS * 16`，居中逻辑检查屏幕是否足够大。如果屏幕太小，使用默认偏移 (80, 60)。

管道连接使用的是模块变量中预存储的指针。空指针检查确保了即使 set_shell_pipes 没被调用也不会崩溃——只是终端没有管道连接。终端标题固定为 "Cinux Terminal"——不像 read-through 中描述的用递增计数器。

这个函数不创建管道、不 fork Shell。Shell 进程在 boot 时通过 `launch_first_user` 启动，管道在 init.cpp 中创建。create_shell_terminal 只是把已存在的管道连接到新创建的 Terminal 上。

### gui_tick_callback——事件处理与终端创建

```cpp
void gui_tick_callback(void* /*ctx*/) {
    auto& wm = WindowManager::instance();
    auto& eq = Mouse::event_queue();

    // 1. 排空输入事件队列
    Event ev;
    while (eq.dequeue(ev)) {
        switch (ev.type_) {
        case EventType::MouseMove:
        case EventType::MouseDown:
        case EventType::MouseUp:
            wm.handle_mouse(ev);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            wm.handle_key(ev);
            break;
        }
    }

    // 2. 消费图标动作——直接创建终端
    IconAction action = wm.consume_pending_icon_action();
    if (action == IconAction::OpenShell) {
        create_shell_terminal();
    }

    // 3. Poll 聚焦的终端窗口
    auto* focused = wm.focused();
    if (focused != nullptr && focused->is_terminal()) {
        auto* term = static_cast<Terminal*>(focused);
        term->poll_output();
        term->render_to_canvas();
    }

    // 4. Composite 所有窗口
    wm.composite();
}
```

关键设计点：consume 后直接调用 create_shell_terminal——没有原子变量、没有 worker 线程。create_shell_terminal 只是 new Terminal + 连接管道 + add_window，这些操作都是轻量的，在 tick callback 中执行没有问题。

poll 只对聚焦的窗口执行，不是所有窗口。`is_terminal()` 守卫确保了只有 Terminal 类型的窗口才会被 static_cast 并调用 poll_output/render_to_canvas。在之前没有 is_terminal() 的版本中，代码是 `if (focused != nullptr)` 直接 static_cast——这在桌面上出现非 Terminal 窗口时会导致未定义行为。

### gui_start——注册图标 + tick callback

```cpp
void gui_start() {
    cinux::lib::kprintf("[GUI] ===== Milestone 033: GUI Desktop =====\n");

    // Initialise PS/2 mouse driver
    cinux::drivers::Mouse::init();

    if (g_screen != nullptr) {
        cinux::drivers::Mouse::set_screen_bounds(g_screen->width(), g_screen->height());
    }

    // Register desktop icons on the desktop
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

    cinux::lib::kprintf("[GUI] Desktop icons registered: Shell, Calculator.\n");

    // Register the GUI tick callback for event processing + compositing
    cinux::drivers::PIT::set_tick_callback(gui_tick_callback, nullptr);
    cinux::lib::kprintf("[GUI] GUI tick callback registered on PIT.\n");
}
```

和之前的版本相比，gui_start 不再创建 Terminal 窗口，改为注册两个 DesktopIcon。日志从 "WindowManager initialised with Terminal window" 变成了 "Desktop icons registered: Shell, Calculator"。不再 return Terminal 指针——返回类型从 `Terminal*` 变成了 `void`。

Shell 图标位于 (40, 40)，Calculator 图标位于 (40, 120)。图标位图使用 tag 032 中定义的 constexpr 数组，`.data()` 返回指向底层 uint32_t 数组的指针。

### window.hpp — is_terminal 虚函数

```cpp
virtual bool is_terminal() const { return false; }
```

Window 基类默认返回 false。Terminal override 返回 true：

```cpp
// terminal.hpp
bool is_terminal() const override { return true; }
```

这个模式避免了 RTTI 开销——`dynamic_cast<Terminal*>` 需要编译器启用 RTTI 并维护 type_info 结构，在 freestanding kernel 中通常不可用。

### terminal.cpp — 析构函数简化

Terminal 的析构函数在 tag 033 中被大幅简化：

```cpp
Terminal::~Terminal() {
    // Pipes are owned externally (by the process that created them).
    // Just clear our references — do NOT close the pipe endpoints,
    // since multiple terminals may share the same pipe pair.
    stdin_pipe_  = nullptr;
    stdout_pipe_ = nullptr;
}
```

之前版本的析构函数会调用 close_writer/close_reader 关闭管道端点，然后 waitpid 回收 Shell 子进程。在 tag 033 中这些都被移除了——管道由外部（init.cpp）创建和拥有，Terminal 只持有引用。将指针设为 nullptr 而不是关闭端点，为多终端共享管道的场景留了余地。

## 设计决策

### 决策：延迟窗口创建 vs 延迟 Shell 启动
**问题**: 延迟的是什么？
**本项目的做法**: 延迟终端窗口创建，Shell 在 boot 时就启动
**备选方案**: 点击图标时才 fork + execve Shell
**为什么选当前方案**: 当前 tag 只需要单终端场景，Shell 在 boot 时通过 launch_first_user 启动是已有的基础设施。延迟窗口创建的代码改动更小——只需要把 Terminal 的创建从 gui_start 移到 create_shell_terminal。
**如果要扩展**: 多终端场景需要改为点击时 fork 新 Shell + 创建 per-terminal 管道。

### 决策：共享管道 vs per-terminal 管道
**问题**: 多个终端如何共享 Shell 通信？
**本项目的做法**: init.cpp 预创建一对管道，通过 set_shell_pipes 存储，所有终端共享
**备选方案**: 每次创建终端都新建一对管道 + fork 新 Shell
**为什么选当前方案**: 当前只有一个 Shell 进程在运行，共享管道是自然的选择。代码更简单——不需要在 GUI 代码中做 fork/execve。
**如果要扩展**: 多终端场景需要改为 per-terminal 管道，每个终端有独立的 Shell 进程。

### 决策：is_terminal() vs dynamic_cast
**问题**: 如何安全地判断窗口是否是 Terminal？
**本项目的做法**: virtual bool is_terminal() const
**备选方案**: dynamic_cast<Terminal*>(focused) != nullptr
**为什么不选备选方案**: freestanding kernel 通常禁用 RTTI（-fno-rtti），dynamic_cast 不可用。虚函数方案零额外开销。
**如果要扩展**: 如果窗口类型增多（5 种以上），可以考虑用 enum type tag 替代多个 is_xxx() 虚函数。

### 决策：Terminal 析构不关闭管道
**问题**: Terminal 关闭时如何处理管道？
**本项目的做法**: 只清空指针，不关闭端点
**备选方案**: close_writer/close_reader + waitpid 回收 Shell
**为什么选当前方案**: 管道由外部创建（init.cpp），Terminal 不是管道的所有者。关闭管道应该是所有者的责任。这个简化让 Terminal 的生命周期管理更清晰。
**如果要扩展**: per-terminal 管道场景下 Terminal 拥有管道，析构时需要关闭。

## 扩展方向

1. (⭐) 支持从文件系统读取图标配置——类似 .desktop 文件，定义图标名称、位图路径、启动命令
2. (⭐⭐) 实现 Calculator 应用——处理 OpenCalculator action，创建 Calculator 窗口
3. (⭐⭐) 实现 per-terminal 管道——每次点击 Shell 图标 fork 新 Shell + 新管道
4. (⭐⭐⭐) 实现终端关闭后的 Shell 进程管理——优雅关闭、超时强制杀掉、资源回收

## 参考资料
- SerenityOS WindowServer: https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer
- ToaruOS Yutani Compositor: https://github.com/klange/toaruos/tree/master/apps
- OSDev Wiki PS/2 Mouse: https://wiki.osdev.org/PS/2_Mouse
