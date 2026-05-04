# 033-gui-desktop-1: 从零搭建桌面环境——图标管理与点击响应

> 标签：GUI, desktop environment, icon management, hit testing, window manager
> 前置：[032-gui-bitmap-icon-3](032-gui-bitmap-icon-3.md)

## 前言

到上一个 tag 为止，我们的 GUI 子系统已经有了窗口管理器、位图渲染和桌面图标数据结构，但说实话整个系统看起来还不太像一个"桌面"。启动后直接弹出一个终端窗口占据了全部注意力，没有背景、没有图标、没有那种"这就是一个桌面操作系统"的感觉。这个 tag 我们要做的，就是把 WindowManager 从一个简单的窗口容器升级为一个真正的桌面环境。

这个升级涉及三个核心能力：管理桌面图标（注册、渲染、命中测试）、响应图标点击（通过 pending action 模式延迟处理）、以及正确的合成管线顺序（图标始终在窗口之下）。这篇教程聚焦在第一个和第二个能力上——从设计到实现，我们一步步把桌面图标管理做好。

## 环境说明

这次的开发环境和前几个 GUI tag 完全一致：QEMU 作为运行时环境，x86_64 目标，GCC/G++ 14 编译器，CMake 构建系统。新增的代码集中在 `window_manager.hpp/cpp` 中，大约 96 行。测试代码新增约 1658 行（宿主端 1123 + 内核端 535）。

## 从哪里开始——桌面图标管理的架构选择

我们先想清楚桌面图标需要做什么。最基本的功能是：在桌面上显示一些小图标，每个图标有一张位图和一个文字标签，用户点击图标后触发一个动作（比如打开 Shell 或 Calculator）。

这个需求翻译成数据结构就是：一个图标数组 + 一个命中测试函数 + 一个"当前待处理动作"变量。听起来简单，但魔鬼在细节里。

第一个问题是存储。我们选择在 WindowManager 内部用一个固定容量的 `DesktopIcon` 数组，上限 16 个。为什么不用链表或者动态数组？因为内核态没有 STL 的 vector，而且桌面图标数量通常不会超过两位数。固定数组零分配、缓存友好、实现简单。在我们的 `window_manager.hpp` 中这个定义是这样的：

```cpp
static constexpr uint32_t MAX_ICONS = 16;

DesktopIcon icons_[MAX_ICONS] = {};
uint32_t    icon_count_       = 0;
IconAction  pending_icon_action_ = IconAction::None;
```

`icons_` 用 `= {}` 做零初始化，`icon_count_` 从 0 开始，`pending_icon_action_` 初始为 None。三个数据成员分别对应"存储"、"计数"、"动作"三个关注点。

### 注册——尾部追加

注册 API 就两步：容量检查和尾部追加：

```cpp
bool WindowManager::add_desktop_icon(const DesktopIcon& icon) {
    if (icon_count_ >= MAX_ICONS) {
        return false;
    }
    icons_[icon_count_] = icon;
    icon_count_++;
    return true;
}
```

这里做的是值拷贝——DesktopIcon 中的 bitmap 指针和 label 指针被原样复制。调用方必须确保这些指针在图标注册后保持有效。在我们的使用场景中这不是问题，因为 bitmap 指向编译期 constexpr 数组，label 指向字符串字面量。

### 命中测试——反向遍历

命中测试是点击检测的核心。当用户点击屏幕上某个位置时，我们需要判断这个位置是否落在某个图标的矩形区域内。因为多个图标可能重叠，所以需要一个优先级规则——我们选择"后注册的优先级高"，因为后添加的图标在视觉上通常更靠近用户（就像 Z 序越高的窗口越"近"）。

```cpp
const DesktopIcon* WindowManager::hit_test_icon(int32_t mx, int32_t my) const {
    for (uint32_t i = icon_count_; i > 0; i--) {
        uint32_t idx = i - 1;
        if (icons_[idx].contains(mx, my)) {
            return &icons_[idx];
        }
    }
    return nullptr;
}
```

这个遍历方式需要仔细看：循环变量 `i` 从 `icon_count_` 递减到 1，实际索引用 `idx = i - 1`。为什么不用 `for (int i = icon_count_ - 1; i >= 0; i--)`？因为当 `i` 是 `uint32_t` 时，`i >= 0` 永远为真（无符号数不会变成负数），会死循环。用 `i > 0` 做终止条件，`idx = i - 1` 避免了无符号下溢问题。这是内核 C++ 中非常常见的遍历模式——在我们的代码库中出现了很多次。

### 消费动作——一次性取出

`consume_pending_icon_action` 是一个经典的 "swap-and-reset" 模式：

```cpp
IconAction WindowManager::consume_pending_icon_action() {
    IconAction action = pending_icon_action_;
    pending_icon_action_ = IconAction::None;
    return action;
}
```

先保存当前值，再重置为 None，最后返回保存的值。gui_tick_callback 在每个 tick 中调用一次，如果没有待处理的动作就返回 None。这个设计保证了每个点击只被处理一次——这是 GUI 编程中非常基本的要求，但如果你忘记在 consume 后重置，同一个点击就会被处理两次（比如弹出两个终端窗口），这种 bug 在调试时非常令人头疼。

## 合成管线——图标在窗口之下

有了图标管理 API，接下来要把图标渲染到屏幕上。关键问题是渲染顺序。在 `composite()` 中，我们在 clear 和窗口 blit 之间插入图标渲染：

```cpp
void WindowManager::composite() {
    screen_->clear(DESKTOP_COLOR);
    draw_desktop_icons(*screen_);        // ← 新增
    for (uint32_t i = 0; i < count_; i++) {
        if (windows_[i]->visible()) {
            // blit window...
        }
    }
    draw_cursor(*screen_);
}
```

这个顺序决定了三层结构：桌面背景（最底） → 图标（中间） → 窗口（最上）。如果窗口覆盖了图标区域，窗口像素会覆盖图标像素，这是桌面环境的标准行为——你不会希望在 Windows 桌面上，图标显示在窗口上面。

`draw_desktop_icons` 的实现做两件事：渲染位图和渲染居中标签。位图渲染复用 tag 032 的 `Canvas::draw_bitmap`，标签居中的计算是 `(icon.width - text_width) / 2` 做水平居中，y 坐标在图标底部加 2 像素间距。

## 点击分派——窗口优先于图标

handle_mouse 的 MouseDown 分支需要处理三种点击：窗口点击、图标点击、桌面空白点击。优先级是窗口 > 图标 > 桌面，因为窗口在视觉上覆盖了图标。

```cpp
if (hit == nullptr) {
    const DesktopIcon* icon_hit = hit_test_icon(ev.mouse.x, ev.mouse.y);
    if (icon_hit != nullptr) {
        pending_icon_action_ = icon_hit->action;
        if (focused_ != nullptr) {
            focused_->set_focused(false);
            focused_ = nullptr;
        }
    } else {
        if (focused_ != nullptr) {
            focused_->set_focused(false);
            focused_ = nullptr;
        }
    }
    break;
}
```

注意点击图标时也会清除窗口聚焦——因为图标在桌面层，点击图标意味着用户的注意力从窗口转移到了桌面。这个行为和 Windows/macOS 的桌面一致：点击桌面图标时，之前激活的窗口标题栏变灰。

## 与其他操作系统的对比

### SerenityOS 桌面小程序

SerenityOS 的桌面图标系统比 Cinux 复杂得多。在 SerenityOS 中，桌面本身是一个独立的用户空间进程（桌面小程序），通过 IPC 与 WindowServer 通信。每个桌面图标是一个 `AbstractIconWidget`，有完整的拖放支持、上下文菜单、双击打开、自动网格排列。图标的布局由 LibGUI 的 Layout 系统自动计算，而不是像 Cinux 这样手动指定坐标。当用户双击桌面图标时，桌面小程序进程通过 IPC 向 Launcher 服务发送一个 `StartApp` 消息，Launcher 服务 fork/exec 新的进程来启动应用。

但核心模式是相似的——点击检测到动作路由。SerenityOS 的 "consume action" 等价物是 IPC 消息队列中的 `StartApp` 消息，消息是异步的且有完整的进程生命周期管理。Cinux 用一个简单的 `pending_icon_action_` 变量替代了整个 IPC 消息队列，因为我们所有的 GUI 逻辑都在同一个内核线程中运行，不需要跨进程通信。

这个差异的根源在于架构选择：SerenityOS 是微内核式的 GUI 服务架构（用户空间 WindowServer + 用户空间应用），Cinux 是宏内核式的内核态 GUI（所有渲染在内核线程中完成）。两种方案各有取舍——SerenityOS 的方案更安全（一个应用崩溃不影响窗口管理器），Cinux 的方案更简单（不需要 IPC，不需要进程间共享内存的同步机制）。

SerenityOS 的 WindowServer 还有一个 Cinux 没有的重要特性——damage tracking。WindowServer 不是每帧都重绘整个屏幕，而是只重绘"脏"区域（窗口移动时被暴露的区域、被覆盖后重新显示的区域等）。这在高分辨率屏幕上能显著减少 CPU 开销。Cinux 当前的 composite 每帧都做全屏重绘，在 1024x768 的分辨率下还能接受，但如果分辨率提高就会成为性能瓶颈。

### ToaruOS Yutani 合成器

ToaruOS 的桌面图标由一个名为 `desktop-icons` 的用户空间程序管理，它通过 PEX 套接字与 Yutani 合成器通信。点击桌面图标时，`desktop-icons` 进程通过 `system()` 调用启动对应的应用程序。这种设计把图标管理和窗口合成完全分离——`desktop-icons` 崩溃了不会影响窗口管理，Yutani 崩溃了也不会影响图标的状态。

ToaruOS 的 Yutani 使用共享内存实现客户端-服务器的双缓冲：每个应用有一个自己的 backing store bitmap，应用在自己的 bitmap 上绘制内容，然后通知 Yutani 合成到屏幕上。这和 Cinux 的方式类似（每个 Window 有自己的 Canvas），但 ToaruOS 跨越了进程边界——共享内存的分配和同步比内核内的 Canvas 管理复杂得多。

Cinux 的 pending_icon_action 模式与 ToaruOS 的 PEX 消息队列在概念上类似——都是一个异步的事件传递机制。不同之处在于 Cinux 是在同一个内核线程的 tick callback 中消费 action，而 ToaruOS 跨越进程边界通过 socket 传递。ToaruOS 的方案更灵活（任何进程都可以发送消息），Cinux 的方案更简单（不需要序列化和反序列化消息）。

## 下一步

到这里我们已经完成了桌面图标管理的三个核心部分：注册、渲染和点击检测。构建内核并在 QEMU 中启动，你应该能看到深青色桌面背景上的两个图标（Shell 和 Calculator）以及白色文字标签。鼠标点击图标会设置 pending action，但还没有东西来消费它——这是下一篇教程的内容。

回顾这一篇，我们从"桌面是什么"这个问题出发，讨论了三层渲染模型、固定容量数组、pending action 模式和点击优先级。这些设计决策都不是凭空做的——它们参考了 SerenityOS、ToaruOS 和经典桌面环境的经验，但在 Cinux 的约束条件下（内核态 GUI、无 IPC、无标准库）做了大幅简化。这种"理解原理、简化实现"的方式贯穿了整个 Cinux 项目。

下一篇我们将实现应用启动器——从 pending action 到终端窗口弹出的完整链路，包括管道解耦和延迟终端创建。

## 参考资料
- SerenityOS WindowServer: https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer
- SerenityOS LibGUI Widget Framework: https://github.com/SerenityOS/serenity/tree/master/Userland/Libraries/LibGUI
- ToaruOS Yutani Compositor Architecture: https://github.com/klange/toaruos
- xv6-gui: https://github.com/KevinVan720/xv6-gui
- OSDev Wiki Drawing In a Linear Framebuffer: https://wiki.osdev.org/Drawing_In_a_Linear_Framebuffer
