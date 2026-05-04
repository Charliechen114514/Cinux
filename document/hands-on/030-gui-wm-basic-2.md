# 030-2 Window 类与窗口管理器核心

## 导语

上一章我们搞定了鼠标输入和事件队列，现在数据已经能从硬件流到 GUI 子系统了。但光有数据不行，还得有东西来消费这些数据——窗口。这一章我们要搭建的是 GUI 的骨架：Window 抽象和 WindowManager 单例。Window 封装了一个窗口该有的所有属性（位置、尺寸、标题栏、离屏画布），WindowManager 负责管理所有窗口的 Z-序、焦点、创建/销毁，以及把所有窗口合成到屏幕上。另外我们还要修一个差点让整个 GUI 挂掉的 ISR 栈对齐 bug。

完成本章后，你会看到一个可以创建多个窗口、用鼠标点击切换焦点、通过 Z-序正确重叠的桌面系统。每个窗口有蓝色标题栏、白色标题文字和红色关闭按钮。

知识前置：上一章的鼠标驱动和事件系统，tag 029 的 Canvas 双缓冲渲染。

## 概念精讲

### 窗口的离屏画布与双缓冲

每个 Window 内部持有一个独立的 Canvas 作为离屏渲染缓冲区。这个 Canvas 的尺寸是窗口宽度 x（窗口高度 + 标题栏高度）。窗口的所有绘制操作（画标题栏、画内容）都先写到这个离屏画布上，最后由 WindowManager 的 compositing 过程一次性把所有窗口 blit 到屏幕画布。这样做的好处是避免窗口绘制过程中的闪烁——用户永远只看到最终合成好的完整帧。

### Z-序管理

多个窗口在屏幕上会互相遮挡，我们需要决定谁在上面。我们用数组的索引来表示 Z-序：index 0 是最底层窗口，index (count - 1) 是最顶层窗口。当用户点击一个窗口时，这个窗口被"raise"到最顶层——具体操作是把它从当前位置抽出来，后面的窗口依次往前挪一个位置，然后把它放到数组末尾。这是一个 O(n) 的操作，但对于我们最多 64 个窗口的场景完全够用。

### 焦点管理

焦点决定哪个窗口接收键盘事件。我们的策略很简单：最顶层的窗口就是焦点窗口。每次窗口创建、销毁、raise 之后都要重新计算焦点——清除所有窗口的 focused 标记，然后给最顶层的那个设上。如果所有窗口都关了，焦点指针就是 nullptr，键盘事件就没人接。

### ISR 栈对齐——一个差点被忽略的致命 bug

在写鼠标驱动的时候，Mouse::init() 操作 8042 控制器会触发一个虚假的 IRQ1。本来这不应该是问题——键盘的 IRQ handler 正常处理就好。但事实是，这个虚假中断直接触发了 #GP（General Protection Fault）。原因是 ISR stub 的栈对齐不满足 System V ABI。

x86_64 System V ABI 要求在执行 `call` 指令前 RSP 必须 16 字节对齐，这样 `call` 压入 8 字节返回地址后，被调函数入口处 RSP ≡ 8 (mod 16)。编译器依赖这个约定来生成 `movaps` 等 SSE 指令——这些指令要求操作数地址 16 字节对齐。

我们的 ISR stub 里，CPU 自动压入 5 个 qword（SS, RSP, RFLAGS, CS, RIP = 40 字节），然后 stub 压入 1 个 dummy error code + 15 个 GPR = 16 个 qword（128 字节），总共 168 字节。168 是 8 的倍数但不是 16 的倍数——等等，168 + 8（call 的返回地址）= 176，正好是 16 的倍数，意味着 handler 入口 RSP ≡ 0 (mod 16)，违反了 ABI。

修复方法很简单：在压完 GPR 之后、`call` 之前，额外 `pushq $0` 塞 8 字节 padding。然后传给 C handler 的 InterruptFrame* 指针用 `leaq 8(%rsp), %rdi` 跳过 padding，handler 返回后 `addq $8, %rsp` 弹掉 padding。这 8 字节的对齐填充让总栈量变成 184 字节，184 ≡ 8 (mod 16)，handler 入口对齐正确。

这个 bug 之前一直没触发，因为键盘 handler 恰好没有使用 XMM 寄存器。直到我们在 keyboard.cpp 里加了 GUI 双路分发代码（调用 Mouse::event_queue().enqueue），编译器才生成了 `movaps`，然后立刻在下一个虚假 IRQ1 上炸了。

## 动手实现

### Step 1: Window 类设计

**目标**: 定义窗口的数据结构，包含位置、尺寸、标题、离屏画布、可见性和焦点标记。

**设计思路**: Window 是一个非拷贝的类，通过 `new` 在堆上分配。构造函数接收标题字符串、初始位置 (x, y) 和内容区域尺寸 (w, h)，自动从静态计数器分配唯一 ID，然后调用 allocate_canvas 初始化离屏画布。画布的总高度是 h + TITLE_BAR_HEIGHT（20px）。标题字符串存储在一个固定大小的 char 数组里（64 字节），手动零填充并截断复制。

Window 提供的绘制方法包括 draw_title_bar（蓝色背景 + 白色标题文字 + 红色关闭按钮方块）和 draw_content（浅灰色内容区背景 + 虚函数 on_paint 给子类画自定义内容）。blit_to 方法把整个窗口画布 blit 到目标画布的对应屏幕坐标位置。

**实现约束**: 虚函数 on_key 和 on_paint 默认空实现，子类可以覆盖。is_terminal 虚函数用于安全类型判断（避免 unsafe static_cast）。关闭按钮位于标题栏右上角，尺寸 14x14 像素。

**验证**: 构造几个 Window 对象，调用 draw_title_bar 和 draw_content，检查离屏画布上标题栏和内容区是否正确绘制。可以用 Canvas 的 blit 方法把窗口 blit 到屏幕画布上验证视觉效果。

### Step 2: WindowManager 单例

**目标**: 实现窗口管理器，管理最多 64 个窗口的 Z-序数组。

**设计思路**: WindowManager 使用 Meyers' singleton（static local reference），全局只有一个实例。init 方法接收屏幕 Canvas 指针和 PSFFont 指针，这两个是合成和标题栏渲染的依赖。

窗口用指针数组管理——Window* 数组，最多 64 个。用指针的好处是 Z-序重排只需要挪指针，不需要移动 Window 对象本身（Window 是 non-copyable 的）。

创建新窗口时位置交错排布（每个窗口偏移 30px），避免完全重叠。创建后立即绘制标题栏和内容，然后更新焦点。销毁窗口时先 delete 对象、shift 数组、减少计数，如果被销毁的是焦点窗口就重新计算焦点。

**实现约束**: create 方法返回窗口 ID（0 表示失败，即满了）。destroy 通过 ID 查找窗口，找不到则忽略。raise 方法如果窗口已经在最顶层则只更新焦点，不做无效移动。

**验证**: 创建 3 个窗口，检查它们的位置交错、Z-序（后创建的在上面）、焦点指向最顶层的窗口。销毁中间的窗口，检查数组 shift 是否正确。

### Step 3: Compositing（桌面合成）

**目标**: 每帧清屏，从底到顶 blit 所有可见窗口，画光标，flip。

**设计思路**: composite 方法首先用桌面背景色清屏，然后从 index 0 到 count-1 依次把每个 visible 的窗口 blit 到屏幕画布上。最后画一个 16x16 的箭头光标。光标用一个 uint16_t 数组表示（16 行，每行 16 位），值为 1 的像素画白色，周围画一圈黑色描边增加可见度。光标位置从 Mouse::x() 和 Mouse::y() 获取。全部画完后调用 screen->flip() 呈现到硬件帧缓冲区。

**踩坑预警**: draw_pixel 调用时坐标可能是负数（窗口被拖到屏幕外）。Canvas 的 draw_pixel 接受 uint32_t 坐标，所以传参前要做边界检查。blit 方法使用 int32_t 的目标坐标正是为了处理这种部分离屏的情况。

**验证**: 创建几个窗口，调用 composite，确认屏幕上能看到所有窗口正确叠加，光标在正确的位置。

### Step 4: 修复 ISR 栈对齐

**目标**: 修改 interrupts.S 中的 ISR_NOERRCODE 和 ISR_ERRCODE 宏，确保 C handler 入口栈对齐正确。

**设计思路**: 在 pushq %r15（最后一个 GPR）之后，额外 pushq $0 作为 8 字节对齐 padding。然后用 `leaq 8(%rsp), %rdi` 计算 InterruptFrame* 参数（跳过 padding 指向 r15），call handler，返回后 `addq $8, %rsp` 弹出 padding。两个宏（NOERRCODE 和 ERRCODE）都需要同样处理——对于 ERRCODE 宏，CPU 已经压入了 error code，所以 GPR 数量是 15 个（没有 dummy error code push），15 + 1 padding = 16 qword = 128 字节，加上 CPU 的 48 字节 = 176 字节，加 call 的 8 字节 = 184 ≡ 8 (mod 16)，同样满足。

**验证**: 修改后 `make run`，确认不再触发 #GP。特别注意观察启动时 [MOUSE] 初始化消息出现后系统是否继续正常运行。

## 构建与运行

```
make run
```

观察串口输出中的以下关键信息：
- `[WM] WindowManager initialised.` — 窗口管理器初始化成功
- `[GUI] Demo rendered to framebuffer.` — GUI 初始画面已渲染
- 无 #GP 异常 — ISR 栈对齐修复生效

## 调试技巧

如果窗口没有正确显示，先检查 composite 是否被调用了（加串口打印确认）。然后检查窗口的 visible 标记——默认应该是 true。如果 blit_to 没有画面，检查离屏画布是否正确初始化（allocate_canvas 是否被调用）。

如果 #GP 仍然出现，检查 interrupts.S 的修改是否正确——尤其是 `leaq 8(%rsp), %rdi` 那行，不要写成 `movq %rsp, %rdi`，否则 InterruptFrame* 会指向 padding 而不是 r15，导致 handler 读到错误的寄存器值。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| Window 离屏画布 | 每个窗口独立 Canvas，避免闪烁 |
| Z-序数组 | index 0 = 底层，count-1 = 顶层 |
| 焦点策略 | 最顶层窗口自动获得焦点 |
| ISR 栈对齐 | push $0 padding，leaq 8(%rsp) 跳过 |
| System V ABI | call 后 RSP ≡ 8 (mod 16) |
| Compositing | 清屏 → 底到顶 blit → 光标 → flip |
| 窗口指针数组 | 挪指针不挪对象，O(n) raise/remove |
