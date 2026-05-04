# 033-gui-desktop-2: 延迟启动的艺术——从 Shell 管道到终端窗口

> 标签：GUI, application launcher, delayed creation, pipe IPC, virtual dispatch
> 前置：[033-gui-desktop-1](033-gui-desktop-1.md)

## 前言

上一篇我们实现了桌面图标的注册、渲染和点击检测，用户点击 Shell 图标后 `pending_icon_action_` 被设置为 `OpenShell`。但点击之后呢？谁来把这个 action 变成一个真正的终端窗口？这就是应用启动器要做的事情。

这个 tag 中最精妙的设计不是图标本身，而是从"用户点击图标"到"终端窗口弹出"之间那一套延迟创建和管道解耦的机制。Shell 进程在 boot 时就已经启动了，但终端窗口可能几秒后才被创建——中间这段时间 Shell 的输出去哪了？答案藏在我们之前实现的管道 IPC 里。

## 环境说明

本篇涉及的文件主要是 `gui_init.cpp`、`gui_init.hpp`、`window.hpp`、`terminal.hpp` 和 `terminal.cpp`。工具链和环境与前面完全一致。核心变更是 `gui_start()` 的返回类型从 `Terminal*` 变成了 `void`，以及新增的 `set_shell_pipes()` 和 `create_shell_terminal()` 内部 helper。

## 控制反转——init.cpp 不再操作 Terminal

我们先从 init.cpp 的视角看变化。在旧架构中，init.cpp 的流程是这样的：

```
gui_start() → Terminal*
term->set_stdin_pipe(stdin_pipe)
term->set_stdout_pipe(stdout_pipe)
```

init.cpp 直接拿到 Terminal 指针并操作它。这有个问题：init.cpp 和 Terminal 的具体实现耦合了。如果将来 Terminal 的构造方式变了（比如需要额外的参数），init.cpp 也要跟着改。

新架构做了一步"控制反转"——init.cpp 只做一件事：把管道指针交给 GUI 子系统。

```cpp
cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);
cinux::gui::gui_start();
```

`gui_start()` 不再返回任何东西。终端窗口什么时候创建、怎么创建，全部是 GUI 子系统的内部事务。init.cpp 只负责"资源准备"（创建管道、启动 Shell），不负责"资源使用"（创建 Terminal、连接管道）。

这个设计在 ToaruOS 中也有体现。ToaruOS 的 init 进程通过 `system()` 启动 Yutani 合成器，然后合成器自己管理桌面和应用窗口。init 不需要知道桌面怎么画、终端怎么弹出来。区别在于 ToaruOS 跨越了进程边界（init → Yutani 通过 PEX socket 通信），而 Cinux 是在同一个内核线程内通过函数调用完成解耦。

### set_shell_pipes——管道指针的暂存

gui_init.cpp 的匿名命名空间中新增了两个模块级指针：

```cpp
namespace {
cinux::ipc::Pipe* g_stdin_pipe  = nullptr;
cinux::ipc::Pipe* g_stdout_pipe = nullptr;
}
```

`set_shell_pipes()` 就是把传入的指针存到这两个变量里：

```cpp
void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe) {
    g_stdin_pipe  = stdin_pipe;
    g_stdout_pipe = stdout_pipe;
    cinux::lib::kprintf("[GUI] Shell pipes stored: stdin=%p stdout=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));
}
```

这种"在模块变量中暂存指针"的模式在内核代码中非常常见。因为它简单、不需要额外的数据结构、调用顺序约束也很清晰（set 必须在 use 之前）。kprintf 打印指针地址是调试的关键信息——如果你看到地址是 0x0，说明 set_shell_pipes 没被调用。

### 延迟创建——点击图标才弹出终端

当用户点击 Shell 图标时，`gui_tick_callback` 检测到 `pending_icon_action_ == OpenShell`，然后调用内部的 `create_shell_terminal()` helper。这个 helper 做三件事：计算位置和尺寸、创建 Terminal 对象、连接管道并添加到 WindowManager。

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

终端尺寸固定为 640x400（80 列 x 25 行的标准终端大小）。居中逻辑用整数除法——`(screen_width - terminal_width) / 2`，不会引入浮点运算。管道连接是可选的——如果 g_stdin_pipe 或 g_stdout_pipe 是 null（比如 set_shell_pipes 没被调用），终端仍然会创建，只是不会有 Shell 连接。这个防御性设计避免了"缺少管道就崩溃"的问题。

管道连接后 Terminal 的 `poll_output()` 会在每个 GUI tick 中被调用，从 stdout 管道中取出 Shell 暂存的输出数据并渲染到终端缓冲区。这意味着 Shell 在 boot 时启动后写入的所有数据（比如启动提示符）都会在终端创建的第一个 tick 中被一口气显示出来。

### gui_tick_callback 中的消费时序

我们来看消费 action 的完整上下文：

```cpp
void gui_tick_callback(void* /*ctx*/) {
    auto& wm = WindowManager::instance();

    // 1. 处理输入事件（handle_mouse 可能设置 pending_icon_action_）
    Event ev;
    while (g_event_queue.pop(ev)) {
        switch (ev.type_) {
        case EventType::MouseDown:
        case EventType::MouseUp:
        case EventType::MouseMove:
            wm.handle_mouse(ev);
            break;
        case EventType::KeyDown:
        case EventType::KeyUp:
            wm.handle_key(ev);
            break;
        }
    }

    // 2. 消费图标动作（在事件处理之后、合成之前）
    IconAction action = wm.consume_pending_icon_action();
    if (action == IconAction::OpenShell) {
        create_shell_terminal();
    }

    // 3. Poll focused terminal + composite
    auto* focused = wm.focused();
    if (focused != nullptr && focused->is_terminal()) {
        auto* term = static_cast<Terminal*>(focused);
        term->poll_output();
        term->render_to_canvas();
    }

    wm.composite();
}
```

消费 action 的位置很关键：必须在事件处理之后（这样才能拿到刚设置的 pending action），在 composite 之前（这样创建的终端在同一帧就被渲染）。如果顺序反了，你可能看到终端"闪现"——第一帧没有终端，第二帧才有。在我们的实现中，从用户点击到终端显示在同一帧内完成，用户体验是即时的。

## is_terminal()——类型安全的 downcast

这里有一个值得细讲的设计。`focused->is_terminal()` 这个检查解决了什么问题？

在之前只有一个 Terminal 的场景下，`static_cast<Terminal*>(focused)` 是安全的——因为 focused 永远指向 Terminal。但现在桌面上可以有非 Terminal 的窗口。如果 focused 指向一个 Calculator 窗口，我们盲转成 Terminal 并调用 `poll_output()`，就会在错误的对象上执行 Terminal 的方法——这是未定义行为，轻则数据损坏，重则内核崩溃。

解决方案是经典的 "type tag via virtual function" 模式：

```cpp
// window.hpp
virtual bool is_terminal() const { return false; }

// terminal.hpp
bool is_terminal() const override { return true; }
```

Window 基类默认返回 false。Terminal 子类 override 返回 true。在 gui_tick_callback 中先用 `is_terminal()` 检查，只有返回 true 时才做 downcast。这比 `dynamic_cast` 更轻量——RTTI 在 freestanding kernel 中通常被禁用（编译选项 -fno-rtti），而虚函数的代价只是 vtable 中多一个函数指针。

SerenityOS 也使用了类似的模式——它的 Window 类有一个 `type()` 方法返回 `WindowType` 枚举，WindowServer 根据 type 做不同的处理。Cinux 用 bool 返回值更简单直接，因为目前只有两种窗口类型（Terminal 和非 Terminal）。

## 管道所有权的转移

最后一个需要注意的变化是 Terminal 的析构函数。之前它会主动关闭管道的读写端：

```cpp
// 旧析构函数（已删除）
Terminal::~Terminal() {
    if (stdin_pipe_ != nullptr) {
        stdin_pipe_->close_writer();
        stdin_pipe_ = nullptr;
    }
    if (stdout_pipe_ != nullptr) {
        stdout_pipe_->close_reader();
        stdout_pipe_ = nullptr;
    }
}
```

现在只清空指针：

```cpp
// 新析构函数
Terminal::~Terminal() {
    stdin_pipe_  = nullptr;
    stdout_pipe_ = nullptr;
}
```

这个变化的动机是为多终端做准备。如果 Terminal 关闭了管道，Shell 进程的 stdin 读操作会收到 EOF（返回 0），stdout 写操作会失败（返回 -1）。在只有一个终端的世界里这不是问题——终端死了 Shell 也该死。但在多终端场景下，关掉一个终端不应该杀掉 Shell。管道的所有权现在归 init.cpp（创建者），Terminal 只是使用者。

## 收尾

到这里整个应用启动器的链路就通了：用户点击 Shell 图标 → pending_icon_action 设为 OpenShell → gui_tick_callback 消费 action → create_shell_terminal 创建终端 → Terminal 连接管道 → Shell 输出显示。构建内核并在 QEMU 中运行，点击 Shell 图标后应该看到一个居中的终端窗口弹出，里面显示着 Shell 的提示符。

不过这个方案还有一个明显的问题：每次点击 Shell 图标都会创建一个新终端，没有去重检查。如果你点三次，就会弹出三个终端——而且它们共享同一对管道，行为是未定义的。这个问题会在后续 tag 中通过窗口去重和进程管理来解决。

下一篇我们将把所有部件和内核启动序列串起来，并看看覆盖桌面环境功能的双轨测试策略。

## 参考资料
- ToaruOS Yutani Window Compositor Architecture: https://github.com/klange/toaruos
- SerenityOS WindowServer: https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer
- OSDev Wiki PS/2 Mouse: https://wiki.osdev.org/PS/2_Mouse
- OSDev Wiki PC Screen Font: https://wiki.osdev.org/PC_Screen_Font
