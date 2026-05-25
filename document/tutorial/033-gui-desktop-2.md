---
title: 033-gui-desktop-2 · 桌面环境
---

# 033-gui-desktop-2: 延迟启动的艺术——从 Shell 管道到终端窗口

> 标签：GUI, application launcher, delayed creation, pipe IPC, virtual dispatch
> 前置：[033-gui-desktop-1](033-gui-desktop-1.md)

## 前言

上一篇我们实现了桌面图标的注册、渲染和点击检测，用户点击 Shell 图标后 `pending_icon_action_` 被设置为 `OpenShell`。但点击之后呢？谁来把这个 action 变成一个真正的终端窗口？这就是应用启动器要做的事情。

这个 tag 中最精妙的设计不是图标本身，而是从"用户点击图标"到"终端窗口弹出"之间那套延迟创建和管道解耦的机制。init.cpp 在 GUI 启动前就创建好了管道，通过 `set_shell_pipes()` 存储管道指针。当用户点击 Shell 图标时，tick callback 直接调用 `create_shell_terminal()`，创建 Terminal 并连接预存储的管道。

## 环境说明

本篇涉及的文件主要是 `gui_init.cpp`、`gui_init.hpp`、`window.hpp`、`terminal.hpp` 和 `terminal.cpp`。工具链和环境与前面完全一致。核心变更是 `gui_start()` 的接口简化（不再返回任何东西），新增的 `set_shell_pipes()` 和 `create_shell_terminal()` 内部 helper，以及 init.cpp 中管道创建时序的调整。

## 控制反转——init.cpp 不再直接操作 Terminal

我们先从 init.cpp 的视角看变化。在新的架构中，init.cpp 仍然创建管道，但不再直接操作 Terminal：

```cpp
#ifdef CINUX_GUI
    // 创建 stdin/stdout 管道（和之前一样）
    auto* stdin_pipe = new cinux::ipc::Pipe();
    // ... PipeReadOps, Inode, File, fd_table.set(0, ...) ...

    auto* stdout_pipe = new cinux::ipc::Pipe();
    // ... PipeWriteOps, Inode, File, fd_table.set(1, ...) ...

    // 存储管道指针（新增）
    cinux::gui::set_shell_pipes(stdin_pipe, stdout_pipe);

    // 启动 GUI（不再返回 Terminal*）
    cinux::gui::gui_start();
#endif
```

init.cpp 的变化是：删除了 `auto* term = gui_start()` 和 `term->set_stdin_pipe/set_stdout_pipe`，新增了 `set_shell_pipes()` 调用，gui_start() 不再返回任何东西。init.cpp 不再 include terminal.hpp——Terminal 的创建完全封装在 gui_init.cpp 内部。

这种"控制反转"在操作系统设计中很常见。Linux 的 init 进程不直接管理 GUI——它启动 display manager（如 GDM），display manager 再启动窗口系统。每一层只知道自己需要知道的东西。Cinux 的 init.cpp 只负责"创建管道 + 启动 GUI 子系统"，GUI 子系统自己决定什么时候创建终端。

### set_shell_pipes——管道指针的延迟绑定

gui_init.cpp 中用两个模块变量存储管道指针：

```cpp
namespace {
cinux::ipc::Pipe* g_stdin_pipe  = nullptr;
cinux::ipc::Pipe* g_stdout_pipe = nullptr;
}

void set_shell_pipes(cinux::ipc::Pipe* stdin_pipe, cinux::ipc::Pipe* stdout_pipe) {
    g_stdin_pipe  = stdin_pipe;
    g_stdout_pipe = stdout_pipe;
    cinux::lib::kprintf("[GUI] Shell pipes stored: stdin=%p stdout=%p\n",
                        reinterpret_cast<void*>(stdin_pipe),
                        reinterpret_cast<void*>(stdout_pipe));
}
```

set_shell_pipes 必须在 gui_start 之前调用。管道指针存入匿名命名空间的模块变量，其他文件无法直接访问，只能通过这个公开函数设置。这个封装确保了管道指针不会被意外修改。

注意这里没有原子变量——管道指针只被设置一次（init.cpp 调用 set_shell_pipes），之后只被读取（create_shell_terminal 使用），不存在并发写入的问题，所以普通的 Pipe 指针就足够了。

### 延迟创建——点击图标才弹出终端

当 gui_tick_callback 消费到 `OpenShell` action 时，直接调用 `create_shell_terminal()`。这个 helper 做的事情是：计算位置和尺寸、创建 Terminal 对象、从模块变量取出预存储的管道指针并连接。

终端尺寸固定为 640x400（80 列 x 25 行的标准终端大小）。居中逻辑用整数除法—— `(screen_width - terminal_width) / 2`，不会引入浮点运算。终端标题固定为 "Cinux Terminal"。

管道连接使用的是 init.cpp 预先创建、通过 set_shell_pipes 存储的管道指针。create_shell_terminal 不创建新管道，不 fork 新进程——它只是把已存在的管道连接到新创建的 Terminal 上。Shell 进程从 boot 起就在运行，它的 stdin/stdout 绑定了这对管道的 fd。在用户点击 Shell 图标之前，Shell 的输出暂存在 pipe buffer 里。终端窗口创建后，Terminal 通过 `poll_output()` 取回暂存的输出并显示。

### gui_tick_callback 中的消费时序

我们来看消费 action 的完整上下文：

```cpp
void gui_tick_callback(void* /*ctx*/) {
    auto& wm = WindowManager::instance();
    auto& eq = Mouse::event_queue();

    Event ev;
    while (eq.dequeue(ev)) {
        // ... handle_mouse / handle_key ...
    }

    // 消费图标动作——直接创建终端
    IconAction action = wm.consume_pending_icon_action();
    if (action == IconAction::OpenShell) {
        create_shell_terminal();
    }

    // Poll 聚焦的 Terminal 窗口
    auto* focused = wm.focused();
    if (focused != nullptr && focused->is_terminal()) {
        auto* term = static_cast<Terminal*>(focused);
        term->poll_output();
        term->render_to_canvas();
    }

    wm.composite();
}
```

消费 action 的位置在事件处理之后、composite 之前。consume 到 OpenShell 后直接调用 create_shell_terminal——没有原子变量、没有 worker 线程。create_shell_terminal 只是 new Terminal + 连接管道 + add_window，都是轻量操作。

poll 只对聚焦的窗口执行，不是所有窗口。`is_terminal()` 守卫确保了只有 Terminal 类型的窗口才会被 static_cast 并调用 poll_output/render_to_canvas。之前的代码是 `if (focused != nullptr)` 直接 static_cast——这在桌面上出现非 Terminal 窗口时会导致未定义行为。

## is_terminal()——类型安全的 downcast

这里有一个值得细讲的设计。`focused->is_terminal()` 这个检查解决了什么问题？

在桌面上可能有非 Terminal 的窗口（比如未来的 Calculator）。如果对 focused 窗口不做检查就 `static_cast<Terminal*>` 并调用 `poll_output()`，就会在错误的对象上执行 Terminal 的方法——这是未定义行为，轻则数据损坏，重则内核崩溃。

解决方案是经典的 "type tag via virtual function" 模式：

```cpp
// window.hpp
virtual bool is_terminal() const { return false; }

// terminal.hpp
bool is_terminal() const override { return true; }
```

Window 基类默认返回 false。Terminal 子类 override 返回 true。在 gui_tick_callback 中先用 `is_terminal()` 检查，只有返回 true 时才做 downcast。这比 `dynamic_cast` 更轻量——RTTI 在 freestanding kernel 中通常被禁用（编译选项 -fno-rtti），而虚函数的代价只是 vtable 中多一个函数指针。

SerenityOS 也使用了类似的模式——它的 Window 类有一个 `type()` 方法返回 `WindowType` 枚举，WindowServer 根据 type 做不同的处理。Cinux 用 bool 返回值更简单直接，因为目前只有两种窗口类型（Terminal 和非 Terminal）。

## Terminal 析构函数的简化

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

## 收尾

到这里整个应用启动器的链路就通了：用户点击 Shell 图标 -> pending_icon_action 设为 OpenShell -> tick callback consume 取出 -> create_shell_terminal 创建终端 + 连接预存储的管道 -> Terminal poll_output 显示 Shell 输出。构建内核并在 QEMU 中运行，点击 Shell 图标后应该看到一个居中的终端窗口弹出，里面显示着 Shell 暂存在 pipe buffer 中的启动输出。

当前的设计是所有终端共享同一对管道。后续 tag 会引入 per-terminal 管道——每次点击 Shell 图标 fork 新的 Shell 进程并创建独立的管道对，让每个终端完全独立。

下一篇我们将把所有部件和内核启动序列串起来，并看看覆盖桌面环境功能的双轨测试策略。

## 参考资料
- ToaruOS Yutani Window Compositor Architecture: https://github.com/klange/toaruos
- SerenityOS WindowServer: https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer
- OSDev Wiki PS/2 Mouse: https://wiki.osdev.org/PS/2_Mouse
- OSDev Wiki PC Screen Font: https://wiki.osdev.org/PC_Screen_Font
