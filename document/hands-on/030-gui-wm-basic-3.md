---
title: 030-gui-wm-basic-3 · 窗口管理器
---

# 030-3 鼠标交互与 GUI 集成

## 导语

前两章我们分别搞定了输入（鼠标驱动+事件队列）和输出（Window + WindowManager + compositing），但它们还是割裂的。这一章要把两者串起来——让窗口管理器真正响应鼠标事件，实现窗口拖拽、关闭、焦点切换，并且把整个 GUI 的启动流程集成到内核的初始化序列里。另外我们还要讨论一个有趣的问题：QEMU 里为什么会出现两个光标。

完成本章后，你会看到一个完整可交互的 GUI 桌面：鼠标移动时光标跟随，点击窗口可以 raise 和拖拽，点击关闭按钮可以销毁窗口。

知识前置：前两章的全部内容。

## 概念精讲

### 鼠标事件的状态机

窗口管理器处理鼠标事件本质上是一个简单的状态机。Idle 状态下收到 MouseDown 时，先做 hit test（从顶层到底层遍历窗口数组，找到第一个包含点击坐标的可见窗口），然后根据点击位置决定行为：如果命中关闭按钮就销毁窗口，如果命中标题栏就进入 Dragging 状态并记录拖拽偏移量，如果命中内容区就只 raise。Dragging 状态下收到 MouseMove 时，用当前鼠标坐标减去偏移量算出新位置，更新焦点窗口的坐标，触发重绘。收到 MouseUp 时退出 Dragging 状态回到 Idle。

注意 MouseDown 的处理顺序：先检查关闭按钮，然后 raise，最后检查标题栏。这意味着即使你点的是标题栏上的关闭按钮区域，关闭按钮检测也会优先触发——这是正确的行为，因为关闭按钮的优先级高于拖拽。

拖拽偏移量（drag offset）的设计很关键。它记录的是"点击时鼠标相对于窗口左上角的偏移"，这样在 MouseMove 时用鼠标坐标减去偏移量就能得到窗口应该移到的新位置，而不是让窗口左上角直接跳到鼠标位置。没有这个偏移量的话，窗口会在你点击标题栏的那一刻突然跳一下，体验非常差。

### hit test 的遍历方向

hit test 必须从最顶层（数组末尾）向下遍历，因为上面的窗口遮挡下面的。如果我们从 index 0 开始遍历，点击一个被其他窗口遮挡的窗口时，底层窗口会"抢到"这个点击事件——显然不对。倒序遍历保证了最上面的窗口总是先被检测到。

我们的 hit_test 方法从 count_ 开始递减到 1，用 i-1 作为索引。同时还要检查窗口的 visible 标记——不可见的窗口不参与 hit test。

### PIT tick 回调驱动的 GUI 事件循环

我们的 GUI 没有独立的事件循环线程（至少在这个 tag 里没有），而是借用 PIT 定时器的 tick 回调来驱动。每次 PIT 中断（100Hz，每 10ms 一次），回调函数就会做两件事：第一，清空事件队列，把每个事件分发到窗口管理器（MouseMove/MouseDown/MouseUp 给 handle_mouse，KeyDown/KeyUp 给 handle_key）；第二，调用 composite 合成一帧。这意味着 GUI 的刷新率被 PIT 频率绑定了——100Hz 的 PIT 给我们最多 100fps 的 GUI 刷新。

这种设计的好处是实现简单，不需要额外的线程调度。缺点是 compositing 占用的时间会吃掉一部分 PIT 中断的处理时间。如果 compositing 耗时超过 10ms，tick 回调会堆积，导致中断延迟。不过在 1024x768 分辨率下不太可能超时。对于我们的教学 OS 来说完全够用。

## 动手实现

### Step 1: 窗口管理器的鼠标事件处理

**目标**: 实现 handle_mouse 方法，处理 MouseDown、MouseMove、MouseUp 三种事件。

**设计思路**: handle_mouse 首先更新内部跟踪的鼠标坐标。然后根据事件类型分发：

MouseDown 时只处理左键（检查 ev.mouse.left），做 hit test，从顶层到底层找第一个被点击到的可见窗口。没找到就清除焦点。找到了窗口就先检查关闭按钮命中（调用 Window::is_close_button_hit，把鼠标坐标从屏幕空间传入），命中则销毁窗口并立即 composite。没命中关闭按钮就 raise 这个窗口，然后检查点击是否在标题栏（鼠标 Y 减去窗口 Y < TITLE_BAR_HEIGHT），是的话进入拖拽状态并记录偏移量（ev.mouse.x - hit->x(), ev.mouse.y - hit->y()）。

MouseMove 时如果正在拖拽且焦点窗口非空，用当前鼠标坐标减去偏移量得到新位置，调用 set_position 更新窗口坐标，重绘标题栏和内容（draw_title_bar + draw_content），调用 composite。

MouseUp 时清除拖拽状态（dragging_ = false）。

**踩坑预警**: 拖拽过程中的 composite 调用可能很频繁（每次 MouseMove 都触发）。如果发现拖拽时系统卡顿，可以考虑只在 MouseUp 时做最终 composite，MouseMove 时只更新坐标不做渲染——不过目前我们的窗口数量很少，每帧都 composite 也不会有明显问题。另外注意 handle_mouse 只处理 MouseDown/MouseMove/MouseUp 三种类型，需要在 switch 里过滤其他事件类型。

**验证**: 创建 2-3 个窗口，用鼠标点击不同位置确认：点击标题栏能拖拽，点击关闭按钮能销毁，点击内容区只 raise 不拖拽，点击空白区清除焦点。

### Step 2: 键盘事件占位

**目标**: 在 WindowManager 中预留键盘事件处理接口，但当前 tag 不做任何处理。

**设计思路**: handle_key 方法目前是一个空操作——简单地 `(void)ev` 丢弃事件。键盘事件虽然在 GUI tick 回调里被分发到了 handle_key，但在 tag 030 里窗口都是静态的（只有标题栏和浅灰色内容区），没有文本输入需求。真正的键盘路由到焦点窗口要等到后续 tag 引入 Terminal 子类后才需要。提前预留 handle_key 的分发电是为了让事件分发框架完整——后续 tag 只需要在 handle_key 里加逻辑，不用改 tick 回调的 drain 循环。

**验证**: 按键盘按键不应导致任何异常。

### Step 3: GUI 初始化流水线

**目标**: 把鼠标驱动、窗口管理器、PIT tick 回调串进内核启动序列。

**设计思路**: 初始化分两个阶段，因为各组件的依赖顺序不同。第一阶段在 kernel_main 里完成（中断开启之前）：创建 static Canvas 对象（避免栈溢出——Canvas 持有大块堆内存），初始化窗口管理器（传入 Canvas 和 PSFFont 指针），渲染 demo 画面（深色背景 0x001A1A2E + 10 个 LCG 随机彩色矩形 + "Cinux GUI" 标题文字 + flip），然后 unmask IRQ12。第二阶段在 kernel_init_thread 里完成（调度器启动之后）：调用 gui_start，初始化鼠标驱动（需要中断已开启，要等 ACK）、设置屏幕边界、创建 3 个测试窗口（Window 1/2/3）、安装 PIT tick 回调。

之所以分两阶段，是因为真实的硬件依赖：Canvas 需要 Framebuffer 就绪（kernel_main 中间），鼠标驱动需要中断开启（kernel_main 末尾之后），PIT tick 回调需要调度器运行（init_thread 中）。每一层都有自己的前置条件，没法在同一个地方全部搞定。

gui_tick_callback 的实现细节：它是一个静态函数，每次 PIT tick 被调用。核心逻辑是先 drain 事件队列（while dequeue 循环，MouseMove/MouseDown/MouseUp 给 handle_mouse，KeyDown/KeyUp 给 handle_key），然后调用 composite 合成一帧。

**实现约束**: gui_init 和 gui_start 是 gui 命名空间下的两个独立函数，main.cpp 和 init.cpp 通过 gui_init.hpp 头文件引入。当 CINUX_GUI 宏未定义时，这些调用通过条件编译跳过。

**验证**: `make run` 后观察完整启动日志：[GUI] 初始化消息 → [MOUSE] 初始化消息 → 桌面画面渲染 → 鼠标移动响应。

### Step 4: QEMU 双光标问题与 USB tablet

**目标**: 理解 PS/2 鼠标在 QEMU 中的固有偏移问题，并通过 QEMU 配置缓解。

**设计思路**: PS/2 协议只报告相对位移（dx/dy），不报告绝对位置。QEMU 的 VNC 光标覆盖层使用宿主机的绝对坐标渲染，而 Guest OS 通过累积 PS/2 dx/dy 从初始位置推算光标位置。两者从不同起点出发，累积相同的位移，因此偏移恒等于初始位置的差值。这不是代码 bug，而是 PS/2 鼠标在 VM 中的已知固有缺陷。

解决方案是在 QEMU 启动参数中添加 `-usb -device usb-tablet`。USB HID Tablet 设备通过 HID Absolute Pointer 用途报告绝对坐标。虽然当前我们没有 USB 驱动，但 QEMU 在宿主侧处理 tablet 后 VNC 光标使用绝对定位渲染，解决了鼠标抓取问题。Guest 侧的初始光标位置设为 (0, 0) 以尽量减小偏移。

**验证**: `make run` 后观察 VNC 中是否仍有双光标。移动鼠标确认光标跟随流畅，不被卡住。

## 构建与运行

```
make run
```

启动后你应该能看到深色桌面背景，上面有三个交错排列的窗口（标题栏蓝色、内容区浅灰色、右上角红色关闭按钮），移动鼠标能看到白色箭头光标跟随。点击窗口标题栏可以拖拽，点击关闭按钮可以销毁窗口。

如果遇到 QEMU 双光标问题影响体验，可以在启动脚本里加 QEMU 参数：
```
-usb -device usb-tablet
```

## 调试技巧

如果鼠标完全没反应，第一步检查 IRQ12 是否被 unmask（在 main.cpp 的 CINUX_GUI 块里）。第二步检查 IDT 里向量 0x2C 是否注册了 irq12_stub，以及 irq_handlers.cpp 是否把它路由到了 mouse_irq12_handler。第三步在 mouse_irq12_handler 里加串口打印确认中断是否触发。

如果鼠标能动但窗口拖拽不工作，检查 handle_mouse 是否被调用（在 PIT tick 回调里加打印）。如果被调用了但窗口不动，检查 dragging_ 标志是否正确设置，drag_offset_x_/y_ 是否有合理值。特别注意 MouseDown 只处理左键——如果你用右键拖肯定不会触发。

如果 composite 后画面闪烁，检查是否在每次 composite 前都做了 clear——不清屏的话旧位置的窗口画面会残留在屏幕上。另外检查 draw_cursor 里的描边像素绘制——坐标可能越界导致 draw_pixel 被跳过，但这不会影响整体显示。

如果点击窗口没有反应，检查 handle_mouse 是否被调用（在 PIT tick 回调里加打印）。确认 gui_start 是否在 kernel_init_thread 里被调用，以及 PIT tick 回调是否正确注册。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 拖拽状态机 | Idle → MouseDown(hit title) → Dragging → MouseUp → Idle |
| 拖拽偏移量 | 记录点击时鼠标相对窗口左上角的偏移，避免窗口跳动 |
| hit test 方向 | 从顶层到底层（数组末尾到开头）遍历，visible 过滤 |
| 键盘处理 | handle_key 当前为空操作 (void)ev，预留给后续 tag |
| PIT tick 驱动 | 100Hz 回调：drain 事件 + composite |
| QEMU 双光标 | PS/2 相对定位 vs VNC 绝对定位的固有偏移，-usb -device usb-tablet 缓解 |
| GUI 启动两阶段 | kernel_main: Canvas+WM+demo+IRQ12 unmask; init_thread: Mouse+test windows+PIT callback |
