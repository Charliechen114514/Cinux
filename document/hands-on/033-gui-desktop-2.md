# 033-gui-desktop-2: 应用启动器与延迟终端创建

## 导语

上一篇我们在 WindowManager 中搭建了桌面图标的管理和点击检测。现在问题来了：用户点击 Shell 图标后，pending_icon_action_ 被设为 OpenShell，但谁来消费这个 action？谁来创建终端窗口？这就是本篇要解决的核心问题——从"图标被点击"到"终端窗口弹出来"之间，需要一套精心设计的启动器机制。

完成本篇后，点击 Shell 图标将真正弹出一个终端窗口，Shell 的输出通过管道传递到终端显示。前置知识是 tag 031 的 Terminal 窗口和 tag 024 的管道 IPC。

## 概念精讲

### 延迟终端创建——为什么不在启动时就建好

在之前的架构中，`gui_start()` 在内核初始化时就创建了一个 Terminal 窗口，然后 `init.cpp` 直接拿到 Terminal 指针连接管道。这个方案简单直接，但有一个根本性的问题：Terminal 窗口的生命周期和 GUI 系统绑定死了，无法支持"关掉终端再开一个"或者"同时打开多个终端"。

延迟创建的思路是：`gui_start()` 不再创建终端窗口，而是只注册桌面图标。终端窗口在用户点击 Shell 图标时才创建。这意味着 Shell 进程在 boot 时就启动了，但 Terminal 窗口可能几秒甚至几分钟后才被创建。中间这段时间 Shell 的输出去哪了？答案是：暂存在 pipe buffer 里。管道是一个有界缓冲区，写入端（Shell）的数据会一直留在缓冲区中，直到读取端（Terminal）通过 poll_output 取走。这就是经典的"生产者-消费者"解耦。

这个设计在实际操作系统中有广泛的对应。比如在 Linux 中，你可以在 shell 启动前就通过管道向其 stdin 写入数据——数据会暂存在管道缓冲区中，直到 shell 进程读取。Cinux 的管道实现也是类似的：Pipe 内部有一个固定大小的环形缓冲区，写入端往里塞数据，读取端从里面取数据。只要缓冲区没满，写入就不会阻塞。

### 管道所有权的变化

这个变化很微妙但很重要。之前 Terminal 的析构函数会主动关闭管道的读写端——因为 Terminal 是管道的唯一消费者，Terminal 死了管道也就没用了。但在新的架构中，管道的生命周期由 `init.cpp` 管理，Terminal 只是管道的一个"视图"。如果 Terminal 关闭了管道，Shell 进程就会收到 EOF 或写入错误，这在多终端场景下是灾难性的。

所以 Terminal 的析构函数现在只清空指针引用，不关闭管道。管道的关闭由创建它的 `init.cpp` 负责。这个设计为未来的多终端做准备——多个 Terminal 可以共享同一对管道。

你可以类比文件描述符的 dup/close 语义来理解这个变化。原来 Terminal 持有管道的"拥有型引用"——析构时 close。现在 Terminal 持有的是"借用型引用"——析构时只放弃引用，不 close 底层资源。

### is_terminal() 类型安全的 downcast

`gui_tick_callback` 需要对 focused 窗口做 Terminal 特有的操作（poll_output、render_to_canvas）。在之前只有一个 Terminal 的场景下，直接 `static_cast<Terminal*>(focused)` 是安全的——因为 focused 永远是 Terminal。但现在桌面上可以有非 Terminal 窗口（比如未来的 Calculator），盲转就会出现未定义行为。

解决方案是经典的 "type tag via virtual function" 模式：Window 基类定义一个虚函数返回 false，Terminal 子类 override 返回 true。在 gui_tick_callback 中先检查返回值，只有确认是 Terminal 时才做 downcast。这比 C++ 的 `dynamic_cast`（需要 RTTI）和 `typeid` 更轻量，因为 RTTI 在 freestanding kernel 中通常被禁用。

这种模式在操作系统内核中很常见。Linux 内核的 `struct file_operations` 就是一个类似的概念——通过函数指针表（vtable）来区分不同文件类型的操作。Cinux 用 C++ 的虚函数实现了同样的效果，代码更简洁。

## 动手实现

### Step 1: 在 gui_init 中添加管道存储机制

**目标**: 允许 init.cpp 在 GUI 启动前将管道指针存入 gui_init 模块。

**设计思路**: gui_init.cpp 的匿名命名空间中新增两个静态指针变量，分别存储 stdin 和 stdout 管道。`set_shell_pipes()` 函数接受两个管道指针并保存。这两个指针会在 `create_shell_terminal()` 被调用时传给新创建的 Terminal。

匿名命名空间保证了这两个变量只在 gui_init.cpp 内部可见——外部无法直接读写它们，只能通过 set_shell_pipes 这个公开 API。这是内核代码中常见的"模块级状态封装"模式。gui_init.hpp 需要前向声明 Pipe 类（`namespace cinux::ipc { class Pipe; }`），避免头文件循环依赖——因为 Pipe 的完整定义依赖很多内核类型，而 gui_init.hpp 被广泛 include。

set_shell_pipes 必须在 gui_start 之前调用——否则创建终端时管道指针是 null，终端会成功创建但不会显示任何 Shell 输出。而且没有任何错误提示，因为代码里用了 `if (pipe != nullptr)` 的防御性检查。这种"静默失败"是最难调试的。

**踩坑预警**: 如果你忘记在 init.cpp 中调用 set_shell_pipes，或者调用的顺序反了（先 gui_start 再 set_shell_pipes），那么 create_shell_terminal 时 g_stdin_pipe 和 g_stdout_pipe 都是 null。终端会创建成功，字体也会设置，但 Shell 的输出不会显示——终端窗口里是空的。排查时看串口日志中的 "[GUI] Shell pipes stored" 行是否出现在 "[GUI] ===== Milestone 033" 之前。

**验证**: 在 set_shell_pipes 中加 kprintf 打印管道指针地址，确认在 gui_start 之前被调用。地址不应该是 0x0。

### Step 2: 实现 create_shell_terminal 内部 helper

**目标**: 封装"创建终端窗口并连接管道"的逻辑为一个内部函数。

**设计思路**: 这个 helper 做三件事：计算终端尺寸和屏幕居中位置、创建 Terminal 对象并设置字体、连接管道并添加到 WindowManager。

终端尺寸固定为 80 列乘 8 像素列宽、25 行乘 16 像素行高（即 640x400 像素）。这是标准终端的经典尺寸。位置在屏幕上居中——计算方法是 `(screen_width - terminal_width) / 2`。如果屏幕太小（终端加边距超过屏幕），就使用默认偏移（80, 60）。居中计算中要注意整数除法的截断——不用浮点数，因为内核态没有浮点运算支持。

创建 Terminal 时使用 `new` 在堆上分配。WindowManager 接管所有权——add_window 后 WindowManager 负责 Terminal 的生命周期。字体通过 set_font 设置——这个 g_font 模块变量在 gui_init() 中被初始化，所以 gui_init 必须在 create_shell_terminal 之前被调用。

管道连接是可选的——如果 g_stdin_pipe 或 g_stdout_pipe 是 null，就跳过对应的连接。这个防御性设计确保即使 set_shell_pipes 没被调用，终端仍然会创建（只是没有 Shell 连接）。

**踩坑预警**: 居中计算中如果 screen 为 null（gui_init 没被调用），会使用默认偏移 (80, 60)。这个默认值在 1024x768 的屏幕上看起来偏左上角，但至少不会崩溃。如果你发现终端位置不对，先检查 g_screen 是否被正确初始化。

**验证**: 点击 Shell 图标后，串口输出应该显示 "[GUI] Shell terminal created and connected."，屏幕上应该弹出一个居中的终端窗口。窗口内应该显示 Shell 的提示符——因为 Shell 在 boot 时就已经启动了，它的输出暂存在管道缓冲区中，Terminal 连接后第一个 poll_output 就会取出来。

### Step 3: 修改 gui_tick_callback 消费 pending action

**目标**: 在每个 GUI tick 中检查是否有待处理的图标动作，并执行对应操作。

**设计思路**: gui_tick_callback 的执行顺序很重要：先处理输入事件（handle_mouse 设置 pending action），然后检查 pending action 并执行对应操作（消费 action + 创建终端），最后 poll focused terminal 的输出并 composite。这样保证了从"用户点击"到"终端创建"在同一帧内完成。

consume_pending_icon_action 只调用一次——不要在循环中反复调用。只处理 OpenShell action，OpenCalculator 留给未来的 tag。is_terminal() 检查放在 poll_output 之前——避免对非 Terminal 窗口调用 Terminal 特有方法（poll_output、render_to_canvas）。

**踩坑预警**: 消费 action 的位置很关键。如果放在 handle_events 之前，你会消费上一帧的 action——看起来延迟了一帧。如果放在 composite 之后，创建的终端要等下一帧才显示。正确的位置是 handle_events 之后、composite 之前。在代码中这就是 `consume → create_terminal → poll → composite` 的顺序。

**验证**: 快速点击 Shell 图标，确认终端窗口立即弹出。再点击 Shell 图标，应该创建第二个终端——目前没有去重逻辑，这是预期行为。未来的 tag 会添加"已经打开"的检查。

### Step 4: 修改 Terminal 析构函数和 Window 类型检查

**目标**: 确保终端关闭时不影响管道，并安全地区分 Terminal 和非 Terminal 窗口。

**设计思路**: Terminal 的析构函数从"关闭管道"改为"只清空指针"。Window 基类新增 `virtual bool is_terminal() const { return false; }`，Terminal override 返回 true。gui_tick_callback 中的 focused 窗口处理增加 is_terminal() 检查。

管道不关闭的原因是管道可能被多个 Terminal 共享（为未来的多终端做准备）。如果 Terminal A 关闭了管道，Shell 的 stdin EOF 了，Terminal B 也就无法和 Shell 通信了。所以管道的关闭权只归创建者（init.cpp），Terminal 只是使用者。

**验证**: 关闭终端窗口（点击标题栏的关闭按钮），确认 Shell 进程没有被杀掉（串口日志中没有 Shell 退出的信息）。重新点击 Shell 图标，应该能创建新终端并恢复通信。

## 构建与运行

构建完整内核并在 QEMU 中启动。启动后应该看到深青色桌面背景和两个图标。用鼠标点击 Shell 图标，应该弹出一个居中的终端窗口。终端中应该显示 Shell 的启动输出——因为 Shell 在 boot 时就已经启动了，它的输出在管道缓冲区中等待。如果你足够快，可能会看到 Shell 的启动提示信息。

点击桌面空白区域，终端窗口应该失去焦点（标题栏变灰）。再点击终端窗口标题栏，终端重新获得焦点。这些都是 WindowManager 原有的聚焦管理功能，不需要额外代码。

## 设计考量——为什么选择这种延迟创建模式

在动手实现完成之后，我们退后一步，看看这个延迟创建模式背后的设计权衡。

### 延迟创建 vs 即时创建的取舍

即时创建（启动时就建好 Terminal）的好处是简单——流程线性，不需要暂存管道指针。缺点是 Terminal 的创建时机和 GUI 初始化绑定死了。如果你想让桌面更灵活（比如支持"不打开终端"或者"打开多个终端"），就需要延迟创建。

延迟创建的代价是多了两个模块变量（g_stdin_pipe 和 g_stdout_pipe）和一个 helper 函数（create_shell_terminal）。init.cpp 的调用顺序也有了新的约束（set_shell_pipes 必须在 gui_start 之前）。但从长远来看，这些代价是值得的——GUI 子系统获得了完全的控制权。

### 管道暂存的替代方案

除了用模块变量暂存管道指针，还有几种替代方案。一种是在 WindowManager 中存储管道指针，让 WM 自己创建终端。这种方案的问题是 WM 知道了太多关于 Terminal 的细节——违反了单一职责原则。另一种是用全局变量而不是匿名命名空间——但全局变量容易被其他模块意外访问，不如匿名命名空间安全。

我们选择的方案（匿名命名空间 + set_shell_pipes 公开 API）在封装性和简单性之间取得了平衡。模块状态对外不可见，只能通过受控的 API 设置。

### 为什么 is_terminal() 而不是 enum type()

另一种实现类型安全 downcast 的方式是在 Window 基类中添加一个 `enum class WindowType { Generic, Terminal, Calculator }` 和一个 `virtual WindowType type() const` 方法。这种方式在有 5 种以上窗口类型时更有优势——你可以用 switch 语句统一处理，而不是一堆 if-else。

但 Cinux 目前只有两种窗口类型（Terminal 和非 Terminal），一个 bool 返回值就足够了。如果将来窗口类型增多，可以无缝迁移到 enum 方案——is_terminal() 可以变成 `type() == WindowType::Terminal` 的语法糖。

## 调试技巧

这一节列出本章最常见的四个 bug 和排查方法。

**点击 Shell 图标没反应**: 在 gui_tick_callback 的 consume 后面加 kprintf 打印 action 值。如果打印出 OpenShell，说明消费成功但 create_shell_terminal 有问题——检查 new Terminal 是否成功（内核堆是否有足够空间）。如果打印出 None，说明 pending_icon_action_ 没被设置——检查 handle_mouse 是否被执行了，以及 hit_test_icon 是否返回了非 null 指针。

**终端弹出但没有 Shell 输出**: 这是最常见的"静默失败"。检查 set_shell_pipes 是否在 gui_start 之前被调用——看串口日志中 "[GUI] Shell pipes stored" 是否出现在 "[GUI] ===== Milestone 033" 之前。打印 g_stdin_pipe 和 g_stdout_pipe 的地址。如果是 null，说明顺序错了或者根本没调用 set_shell_pipes。如果非 null，检查 Terminal::poll_output 是否被调用（在 poll_output 开头加 kprintf）。

**终端弹出后立即崩溃**: 检查 Terminal::set_font 是否被调用了。没有 font 的 Terminal 在 render_to_canvas 时会访问空指针。在 create_shell_terminal 中加 kprintf 确认 set_font 被调用了，并且 g_font 不是 null。g_font 是在 gui_init() 中设置的——如果 gui_init 没被调用或者传入的 font 是 null，Terminal 就会出问题。

**点击后创建多个终端**: 这是正常的——每次点击都会创建一个新终端，目前没有去重检查。如果这不是你想要的行为，可以在 create_shell_terminal 开头添加一个检查：遍历 WindowManager 的窗口列表，如果已经有 is_terminal() 返回 true 的窗口就跳过创建。但当前的"每次点击创建一个"的行为对于调试来说是方便的——你可以测试多个终端并存的场景。

## 本章小结

| 组件 | 职责 | 关键设计 |
|------|------|----------|
| set_shell_pipes | 存储管道指针 | 匿名命名空间封装，必须在 gui_start 前调用 |
| create_shell_terminal | 创建终端窗口 | 屏幕居中 + 可选管道连接 |
| gui_tick_callback 消费逻辑 | 处理图标动作 | handle_events 后、composite 前 |
| is_terminal() | 类型安全 downcast | virtual 函数替代 RTTI |
| Terminal::~Terminal | 管道不关闭 | 只清空指针，管道所有权归 init.cpp |
| gui_start 重构 | 只注册图标 | void 返回，延迟终端创建 |
