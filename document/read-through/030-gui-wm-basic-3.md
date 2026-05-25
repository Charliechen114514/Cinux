---
title: 030-gui-wm-basic-3 · 窗口管理器
---

# 030-3 Read-through: GUI Integration + ISR Alignment Fix

## 概览

本文 walkthrough 覆盖 tag 030 的集成层代码——gui_init.cpp（GUI 初始化和 PIT tick 回调）、interrupts.S 的 ISR 栈对齐修复、keyboard.cpp 的 GUI 双路分发修改，以及 main.cpp 和 init.cpp 的启动序列集成。另外还包括 crt_stub.cpp（C++ 运行时支持）和 irq_handlers.cpp（IRQ 路由注册）。

关键设计决策：GUI 初始化分两阶段（kernel_main 中断前 + init_thread 调度器后），PIT tick 回调驱动事件循环。

## 架构图

```
kernel_main()                         kernel_init_thread()
    |                                      |
    v                                      v
gui_init(screen, font)               gui_start()
    |                                      |
    +-> WM::init(screen, font)            +-> Mouse::init()
    +-> demo rendering                    +-> set_screen_bounds()
    +-> (static Canvas + PSFFont)         +-> create("Window 1", ...)
                                         +-> create("Window 2", ...)
                                         +-> create("Window 3", ...)
                                         +-> PIT::set_tick_callback()
                                                  |
                                                  v
                                          gui_tick_callback()  [ISR context]
                                                  |
                                              drain EventQueue
                                              handle_mouse()
                                              handle_key()
                                                  |
                                              composite()
```

## 代码精讲

### gui_init.hpp — 接口层

两个函数的接口设计很干净。gui_init 接收 Canvas 和 PSFFont 引用，在 kernel_main 里调用。gui_start 无参数，在 kernel_init_thread 里调用。这种分层设计让 main.cpp 和 init.cpp 完全不需要了解 GUI 的内部细节。

### gui_init.cpp — 初始化与 tick 回调

文件开头的匿名命名空间存放模块内部状态：g_screen 和 g_font 指针（保存初始化参数供后续使用）。只有这两个指针，没有终端计数器或延迟工作标志。

gui_init 的前半部分初始化窗口管理器，后半部分渲染一个 demo 画面——深色背景上画 10 个随机彩色矩形和一行 "Cinux GUI" 标题文字。随机数用的是 LCG（线性同余生成器），seed 是硬编码的 12345。这不是加密安全的随机数，但对于画几个装饰矩形足够了。

gui_tick_callback 是整个 GUI 的事件循环核心，每次 PIT tick（10ms）被调用一次。它先清空事件队列——循环 dequeue，根据事件类型分发给 handle_mouse 或 handle_key。没有额外的图标检测、终端轮询或统计计数器。逻辑非常简洁：

```cpp
void gui_tick_callback(void* /*ctx*/) {
    auto& wm = WindowManager::instance();
    auto& eq = Mouse::event_queue();

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

    wm.composite();
}
```

gui_start 初始化鼠标驱动、设置屏幕边界、创建三个测试窗口（Window 1/2/3）、安装 PIT tick 回调。它必须在调度器运行之后调用，因为 tick 回调里做的事情比较重。

### interrupts.S — ISR 栈对齐修复

ISR_NOERRCODE 宏在 pushq %r15 之后、call 之前，新增了对齐 padding 和指针调整：

```asm
    pushq $0                          # alignment padding (8 bytes)

    /* Pass InterruptFrame* as first argument (skip alignment padding) */
    leaq 8(%rsp), %rdi                # (RSP+8)->%rdi: point past padding to r15

    /* Call the C handler function */
    call \handler                     # call the corresponding C exception handler

    /* Remove alignment padding */
    addq $8, %rsp                     # $8->%rsp: skip alignment padding
```

pushq $0 在栈上压入 8 字节的零值作为对齐填充。leaq 8(%rsp), %rdi 让第一个参数（InterruptFrame*）跳过 padding 指向 r15 字段——这样 InterruptFrame 的结构体布局完全不需要改动，C handler 代码零修改。handler 返回后 addq $8, %rsp 把 padding 弹掉，然后正常恢复 GPR。

ISR_ERRCODE 宏做了同样的修改。两个宏加起来的数学是：

- NOERRCODE: CPU 5 qwords (40) + dummy error 1 + GPR 15 + padding 1 = 22 qwords (176 bytes)。call 压 8 字节 = 184 ≡ 8 (mod 16)。
- ERRCODE: CPU 6 qwords (48) + GPR 15 + padding 1 = 22 qwords (176 bytes)。call 压 8 字节 = 184 ≡ 8 (mod 16)。

两者都满足 System V ABI 要求的 RSP ≡ 8 (mod 16) at function entry。

### keyboard.cpp — GUI 双路分发

irq1_handler 的末尾有一段条件编译代码，在 CINUX_GUI 定义时启用。在原来的键盘事件入队之后，额外构造一个 gui::Event 并推入 Mouse::event_queue()：

```cpp
cinux::gui::Event gui_ev{};
gui_ev.type_ = ev.pressed ? cinux::gui::EventType::KeyDown
                          : cinux::gui::EventType::KeyUp;
gui_ev.key.ascii    = ev.ascii;
gui_ev.key.scancode = ev.scancode;
gui_ev.key.pressed  = ev.pressed;
gui_ev.key.shift    = ev.shift;
gui_ev.key.ctrl     = ev.ctrl;
gui_ev.key.alt      = ev.alt;
cinux::drivers::Mouse::event_queue().enqueue(gui_ev);
```

通过 Mouse 类的静态方法访问全局 EventQueue——这看起来有点奇怪（键盘事件通过 Mouse 类访问队列），原因是队列定义在 Mouse 类里作为 g_event_queue_ 成员。从逻辑上讲这个队列应该是独立的，但为了减少全局符号的数量暂时这么做。将来重构时可以把它抽成独立的 GUI 全局对象。

### main.cpp — GUI 启动集成

kernel_main 里的 GUI 集成通过 `#ifdef CINUX_GUI` 条件编译，有两处重要改动：

第一处改动涉及两个 static 变量和 gui_init 调用。原来直接在 kernel_main 里渲染 GUI demo 的代码全部移入 gui_init，kernel_main 只需：将 PSFFont 改为 static（避免栈溢出，同时生命周期覆盖整个内核运行期）、将 Canvas 也改为 static、init Canvas 传入 Framebuffer、调 gui_init(g_canvas, font)。这把 GUI 初始化细节完全封装到 gui 模块。

第二处在 IRQ0+IRQ1 unmask 之后——PIC::unmask(12) 启用鼠标中断。注意 IRQ12 的 unmask 在 Mouse::init 之前完成——Mouse::init 在 gui_start（init_thread 里）才调用。init 过程中操作 8042 控制器可能产生虚假 IRQ1/IRQ12，这些会被对应的 ISR 安全处理（Mouse::irq12_handler 读一个字节然后 EOI，Keyboard::irq1_handler 也是正常的处理流程）。

### crt_stub.cpp — C++ 运行时支持

这个文件提供了内核 C++ 程序所需的最小运行时。__cxa_pure_virtual 和 __stack_chk_fail 是编程错误的处理——cli + hlt 死循环。__cxa_atexit 是 no-op（内核不退出）。__dso_handle 是空指针。__cxa_guard_acquire/release 用于函数局部静态变量的线程安全初始化——在单核内核里就是简单的 0/1 检查。

_init_global_ctors 遍历 .init_array 段调用所有全局构造函数。operator new/delete 转发到 Heap::alloc/free。这些都是 freestanding 环境下 GCC 隐式需要的符号，不提供就会链接失败。

### irq_handlers.cpp — IRQ 路由

irq_handlers.cpp 使用 data-driven 方式注册所有 16 个 IRQ handler——一个 constexpr IRQRoute 数组把向量号和 stub 函数指针对应起来，irq_init 循环注册。irq12_stub 现在指向 mouse_irq12_handler（而非默认的 irq_default_handler），这个符号在 mouse.cpp 里通过 extern "C" 定义。

非 GUI 构建时，irq_handlers.cpp 里提供了一个默认的 mouse_irq12_handler 实现——只是 PIC::send_eoi(12)，因为 Mouse 驱动没有被编译进来。

## 设计决策

### 决策：两阶段初始化

**问题**: GUI 初始化应该放在哪里？
**本项目的做法**: gui_init 在 kernel_main（中断前），gui_start 在 kernel_init_thread（调度器后）。
**备选方案**: 全部放在 kernel_main 或全部放在 init_thread。
**为什么不选备选方案**: Canvas 初始化需要 Framebuffer 就绪（kernel_main 中间），但鼠标初始化需要中断开启（kernel_main 末尾之后），PIT tick 回调需要调度器运行（init_thread 中）。两阶段是自然的依赖顺序。

### 决策：PIT tick 回调驱动事件循环

**问题**: GUI 事件循环怎么驱动？
**本项目的做法**: 借用 PIT 定时器的 tick callback。
**备选方案**: 独立 GUI 线程 + 专用事件循环。
**为什么不选备选方案**: tick callback 实现简单，不需要额外的线程调度和同步。缺点是 GUI 帧率被 PIT 频率限制（100Hz），且 compositing 占用中断处理时间。对于教学 OS 这是合理的权衡。

## 扩展方向

- 专用 GUI 线程：把 tick callback 里的重操作（composite、事件处理）移到独立线程 (难度 ⭐⭐)
- VSync 同步：用 framebuffer 双缓冲的 VSync 信号驱动 compositing (难度 ⭐⭐)
- 动态帧率：根据事件队列深度自适应 composite 频率 (难度 ⭐⭐)
- 事件优先级：键盘事件优先于鼠标事件处理，减少输入延迟 (难度 ⭐)
- CRT stub 增强：添加 operator new 的 out-of-memory 处理和构造函数异常保护 (难度 ⭐⭐)

## 参考资料
- Intel SDM: Vol.3A Section 6.12 — 中断栈帧布局和 System V ABI 对齐要求
- OSDev Wiki: [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) — IRQ12 配置
- QEMU Mouse Cursor Offset: [torgeir.dev](https://torgeir.dev/2024/02/qemu-mouse-cursor-offset/) — PS/2 相对定位 vs VNC 绝对定位导致的偏移问题
