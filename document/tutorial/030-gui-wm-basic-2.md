# 窗口管理器：从 Canvas 到桌面合成

> 标签：窗口管理器, Z-序, 焦点管理, 双缓冲合成, ISR 栈对齐
> 前置：[030-1 PS/2 鼠标驱动](030-gui-wm-basic-1.md)

## 前言

上一章我们让内核看到了鼠标移动和按键，但事件只是躺在队列里没人消费。现在问题来了——谁来吃这些事件？答案是窗口管理器。我们这一章要从零搭一个能管理多窗口、支持 Z-序叠放、鼠标拖拽和关闭按钮的窗口管理器。听起来工作量很大，但拆开来看每个部分其实都不复杂。

不过事情到这里还没完。在写鼠标驱动的过程中我们踩到了一个差点让整个 GUI 挂掉的坑——ISR 栈对齐 bug。Mouse::init() 操作 8042 控制器时会触发一个虚假的 IRQ1，然后我们的键盘 handler 里面加了 GUI 双路分发代码之后编译器生成了 `movaps` SSE 指令，这条指令要求操作数地址 16 字节对齐。而我们的 ISR stub 恰好没有保证这个对齐——boom，#GP。真正的坑在后面，我们先搭窗口管理器，然后讲这个对齐修复。

## 环境说明

- 平台：x86_64 (QEMU, 默认 PS/2 鼠标 + VBE framebuffer)
- 工具链：GCC 14 + GNU AS + CMake，-ffreestanding -nostdlib -fno-exceptions
- 屏幕：1024x768x32bpp（默认 QEMU VBE 配置）
- 特殊约束：Window 类 non-copyable（持有堆分配 Canvas）

## 第一步——Window 抽象：一个窗口该有什么

一个窗口需要什么？位置 (x, y)、尺寸 (width, height)、标题、一个离屏画布用于双缓冲渲染、可见性标记、焦点标记、一个唯一 ID。标题栏（20px 高）在内容区上方，包含标题文字和红色关闭按钮。所有绘制先写离屏画布，最后由窗口管理器一次性 blit 到屏幕。

```cpp
Window::Window(const char* title, int32_t x, int32_t y, uint32_t w, uint32_t h)
    : id_(next_id_++), x_(x), y_(y), w_(w), h_(h), visible_(true), focused_(false) {
    for (uint32_t i = 0; i <= TITLE_MAX_LEN; i++) {
        title_[i] = '\0';
    }
    if (title != nullptr) {
        uint32_t i = 0;
        while (i < TITLE_MAX_LEN && title[i] != '\0') {
            title_[i] = title[i];
            i++;
        }
    }
    allocate_canvas();
}
```

构造函数手动复制标题字符串，因为我们的内核没有标准库——没有 strncpy，没有 std::string。allocate_canvas 初始化一个 w x (h + TITLE_BAR_HEIGHT) 的离屏 Canvas。每个 Window 独占自己的 Canvas 内存，所以 Window 必须是 non-copyable 的，否则两个对象指向同一块堆内存会在析构时 double-free。

虚拟接口 on_key、on_paint、is_terminal 给子类留了扩展点。is_terminal 看起来有点奇怪——它的存在是为了在 PIT tick 回调里安全地判断一个 Window 是不是 Terminal 子类，避免使用 unsafe static_cast。在 C++ 里没有 RTTI 的 freestanding 环境下，虚函数是类型判断的标准做法。

对比其他 OS 的窗口抽象：SerenityOS 的 Window 类（在 WindowServer 进程里）包含 backing store、damage rect、resize indicator、window type（normal/popup/tooltip）等几十个字段。X11 的窗口概念更复杂——每个窗口有独立的 colormap、input hint、WM_PROTOCOLS 等属性。我们的 Window 是最精简的版本，刚好能支持标题栏、关闭按钮和自定义内容绘制。

## 第二步——WindowManager：谁在上面，谁有焦点

窗口管理器是一个 singleton，管理一个固定 64 个槽位的 Window* 指针数组。用指针数组而不是对象数组的原因很简单——Z-序重排只需要挪指针（8 字节），不需要拷贝整个 Window 对象（Canvas 持有大量堆内存）。

Z-序通过数组索引隐式表达：index 0 是最底层窗口，index (count - 1) 是最顶层窗口。raise 操作把一个窗口移到最顶层——从当前位置抽出来，上面的窗口依次往前挪，然后放到数组末尾：

```cpp
Window* win = windows_[idx];
for (uint32_t i = idx; i < count_ - 1; i++) {
    windows_[i] = windows_[i + 1];
}
windows_[count_ - 1] = win;
```

这是 O(n) 的数组 shift，对于最多 64 个窗口来说毫无压力。如果窗口数量到上千，可以考虑链表或平衡树，但教学 OS 没这个必要。

焦点策略也极其简单——最顶层的窗口就是焦点窗口。每次创建、销毁或 raise 之后都要重新计算焦点：清除所有窗口的 focused 标记，给最顶层的设上。这是 click-to-raise + click-to-focus 的行为，几乎所有简单的窗口管理器都这么做。Linux 的窗口管理器（i3, KWin, Mutter）支持焦点和 Z-序分离——你可以 focus 一个窗口但不把它 raise 到最前面。这对教学 OS 来说过度设计了。

桌面合成的流程是 composite 方法：清屏 → blit 所有可见窗口（从底到顶） → 画光标 → flip。这个"画家算法"保证了正确的遮挡关系——后画的窗口自然覆盖先画的。

对比 SerenityOS 的 WindowServer：它使用了 damage tracking（只重绘脏区域），这在 4K 分辨率下能显著减少 blit 操作。我们的做法是每帧全屏重绘，1024x768 分辨率下还好，更高分辨率就会成为瓶颈。不过对于教学 OS，全屏重绘的实现简单得多。

## 第三步——ISR 栈对齐：差点把 GUI 炸掉的那个 bug

现在来讲那个差点让我们前功尽弃的 bug。在 Mouse::init() 执行过程中，8042 控制器的状态变化会触发一个虚假的 IRQ1。这本身不是问题——我们的键盘 handler 应该能安全处理。但事实上，这个虚假 IRQ1 直接导致了 #GP。

原因是这样的：x86_64 System V ABI 要求在执行 `call` 指令前 RSP 必须 16 字节对齐，这样 `call` 压入 8 字节返回地址后，被调函数入口处 RSP ≡ 8 (mod 16)。编译器依赖这个约定来生成 `movaps` 等 SSE 指令。

我们的 ISR stub 里，CPU 自动压入 5 个 qword（40 字节），stub 压入 1 dummy error code + 15 GPR = 16 个 qword（128 字节），总共 168 字节。`call` 再压 8 字节 = 176 字节。176 是 16 的倍数，意味着 handler 入口 RSP ≡ 0 (mod 16)——违反了 ABI。

这个 bug 一直潜伏着，因为之前的键盘 handler 恰好没有使用 XMM 寄存器。直到我们在 irq1_handler 里加了 GUI 双路分发代码（调用 Mouse::event_queue().enqueue），编译器才生成了 `movaps`，然后立刻在 Mouse::init() 触发的那个虚假 IRQ1 上炸了。

修复方法是在 pushq %r15 之后、call 之前加 8 字节 padding：

```asm
pushq $0                          # alignment padding (8 bytes)
leaq 8(%rsp), %rdi                # skip padding, point to r15
call \handler
addq $8, %rsp                     # pop padding
```

`leaq 8(%rsp), %rdi` 确保传给 C handler 的 InterruptFrame* 跳过 padding 指向 r15 字段，InterruptFrame 结构体布局零修改。修复后总栈量 184 ≡ 8 (mod 16)，handler 入口对齐正确。

这个坑的教训是：ISR stub 必须保证栈 16 字节对齐——这不是可选的，是 System V ABI 的硬性要求。不管你的 handler 当前有没有用 SSE 指令，编译器随时可能生成它们，你必须在第一时间就把对齐做对。

## 收尾

验证方式：`make run`，观察启动过程。正常情况下你会看到 `[MOUSE] PS/2 mouse driver initialised.` 而不是 #GP 异常。如果 #GP 仍然出现，检查 interrupts.S 的修改是否正确——尤其是 `leaq 8(%rsp), %rdi` 那行不能漏。

到这里我们的窗口管理器框架已经搭好了——能创建/销毁窗口、管理 Z-序、做桌面合成。下一章要把鼠标事件和窗口管理器串起来，实现真正的交互。

## 参考资料
- Intel SDM: Vol.3A Section 6.12 — Interrupt Stack Frame layout (5 qwords on IRQ entry)
- OSDev Wiki: [PS/2 Mouse](https://wiki.osdev.org/PS/2_Mouse) — event types reference
- SerenityOS: [WindowServer](https://github.com/SerenityOS/serenity/tree/master/Userland/Services/WindowServer) — damage tracking compositor architecture
- Linux: [Early X11 WM](https://www.x.org/wiki/) — client-server window management comparison
- Cinux Notes: [gp_fault_stack_alignment.md](../../document/notes/030/gp_fault_stack_alignment.md) — detailed ISR alignment bug analysis
