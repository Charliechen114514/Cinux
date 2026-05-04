# 从零构建 PS/2 鼠标驱动：让内核"看见"鼠标

> 标签：PS/2 鼠标, IRQ12, 8042 控制器, 事件队列, 环形缓冲区
> 前置：[029 GUI Canvas](029-gui-canvas-1.md)

## 前言

说实话，当我们把 Canvas 双缓冲画布跑通之后，最想做的事情不是继续画矩形——而是让鼠标能动起来。一个只有输出没有输入的 GUI 就像一个只能看不能点的网站，再漂亮也是死的。所以我们这个 tag 要做的第一件事，就是从零写一个 PS/2 鼠标驱动，让内核真正"看见"用户的鼠标操作。

你可能会问：为什么是 PS/2 而不是 USB？毕竟 USB 鼠标才是主流。原因很简单——PS/2 是 x86 平台上最简单的输入协议，8042 控制器是 QEMU 和 Bochs 默认支持的硬件，而且它的中断驱动模型非常适合教学。USB HID 虽然功能强大，但需要先搞定 USB 主机控制器驱动（xHCI/EHCI），那是一个巨大的工程。等我们后面有精力了再考虑。

折腾背景也很直接：之前 tag 014 我们写好了键盘驱动，同样是 PS/2 设备，同样走 8042 控制器。鼠标驱动的基本框架可以参考键盘，只是初始化序列和包格式不同。

## 环境说明

- 平台：x86_64 (QEMU, 默认 PS/2 鼠标)
- 工具链：GCC 14 + GNU AS (AT&T 语法) + CMake
- 语言：C++ 内核（freestanding，无标准库）
- 特殊约束：PS/2 协议只报告相对位移，不支持绝对坐标查询

## 第一步——理解 8042 的双通道架构

8042 控制器有两个端口——第一端口接键盘（IRQ1），第二端口叫"辅助端口"（auxiliary port），也就是鼠标口（IRQ12）。两者的通信端口一样：0x60 数据口，0x64 命令/状态口。但给鼠标发命令需要先通过 0xD4 前缀告诉控制器"下一条指令是给辅助端口的"。

这个设计其实是 1980s 的遗产。当时的 PC AT 把键盘控制器芯片扩展了一下，加了个第二通道给鼠标用。虽然硬件很老，但虚拟化平台和真实硬件都还在兼容它。Intel SDM Vol.3A 的中断章节和 OSDev Wiki 的 I8042 页面都有详细的控制器命令集文档。

我们的初始化序列分成三步：

```cpp
// Step 1: Enable auxiliary device port (CMD 0xA8)
wait_input_empty();
io_outb(Ps2Port::COMMAND, Ps2Cmd::ENABLE_AUX);

// Step 2: Read config, set bit 1 (IRQ12 enable), write back
wait_input_empty();
io_outb(Ps2Port::COMMAND, Ps2Cmd::READ_CONFIG);
wait_output_full();
uint8_t config = io_inb(Ps2Port::DATA);
config |= 0x02;
wait_input_empty();
io_outb(Ps2Port::COMMAND, Ps2Cmd::WRITE_CONFIG);
wait_input_empty();
io_outb(Ps2Port::DATA, config);

// Step 3: Send CMD 0xD4 + 0xF4 to enable mouse streaming
wait_input_empty();
io_outb(Ps2Port::COMMAND, Ps2Cmd::WRITE_AUX);
wait_input_empty();
io_outb(Ps2Port::DATA, MouseCmd::ENABLE_STREAMING);
```

0xA8 启用辅助端口，0x20/0x60 读写配置字节（bit 1 控制 IRQ12），0xD4+0xF4 告诉鼠标"开始发送数据"。鼠标回复 0xFA（ACK），我们的驱动检查这个字节确认初始化成功。

这里有个细节值得说：每一步操作之前都要 `wait_input_empty`——轮询 0x64 的 bit 1 直到输入缓冲区空。读取响应之前要 `wait_output_full`——轮询 bit 0 直到输出缓冲区有数据。这些等待都有超时保护（100000 次循环），防止硬件无响应时死循环。

对比其他 OS 的做法：xv6 没有鼠标驱动。SerenityOS 的 PS2MouseDevice 使用了类似的三步初始化，但它封装成了一个更通用的 PS/2 控制器抽象层，支持 command/response 协议重试和超时检测，比我们的简单轮询更健壮。Linux 的 i8042 驱动则更进一步——它在初始化过程中做了完整的控制器复位、自测试、双通道检测和设备类型识别，代码量是我们的十倍以上。

## 第二步——3 字节包解析与 9 位有符号数

PS/2 鼠标在 streaming 模式下，每次移动或按键都会发一个 3 字节包。我们来看 OSDev Wiki 上记录的格式：

- Byte 0: bit 7-6 = X/Y overflow, bit 5 = Y sign, bit 4 = X sign, bit 3 = always 1, bit 2-0 = middle/right/left button
- Byte 1: X 方向相对位移（低 8 位）
- Byte 2: Y 方向相对位移（低 8 位）

这里最容易踩的坑是 delta 值的解读。如果你直接把 byte 1/2 当 8 位有符号数，在鼠标快速移动时会出现坐标突变——因为负数的补码表示被截断了。正确做法是提取 9 位有符号值：低 8 位在 byte 1/2，第 9 位（符号位）在 byte 0 的 bit 4/5。

我们的实现是这样的：

```cpp
int32_t dx = static_cast<int32_t>(b1);
if (b0 & Packet0::X_SIGN) {
    dx -= 256;  // Sign-extend from 8 to 9 bits
}

int32_t dy = static_cast<int32_t>(b2);
if (b0 & Packet0::Y_SIGN) {
    dy -= 256;
}

mouse_x_ += dx;
mouse_y_ -= dy;  // PS/2 Y axis is inverted
```

这里 `dx -= 256` 等价于 OSDev 推荐的 `(d - ((state << 4) & 0x100))` 写法——当符号位置位时，256 被减去，相当于把 8 位正值转成了 9 位负值的补码。Y 轴注意要取反，因为 PS/2 的正 dy 是物理上移而屏幕坐标 Y 向下。

包同步也是一个要处理的问题。因为每次 IRQ12 只送来一个字节，我们需要维护一个索引和缓冲区。当索引为 0 时，我们检查当前字节的 bit 3——PS/2 规范里 byte 0 的 bit 3 永远是 1。如果收到了 bit 3 为 0 的字节，说明我们丢同步了（可能是因为中断被延迟导致字节丢失），直接丢弃等下一个合法的包开头。

## 第三步——统一事件队列

有了鼠标驱动之后，我们的 GUI 有了两种输入源：键盘（IRQ1）和鼠标（IRQ12）。如果让窗口管理器直接去两个地方轮询，代码会很乱。所以我们做了一个统一的事件队列——一个 128 容量的 ring buffer，所有输入事件都往里推。

环形缓冲区的实现非常经典：一个 Event 数组、一个 head 读指针、一个 tail 写指针。enqueue 在 (tail+1)%size == head 时丢弃事件（满了），dequeue 在 head == tail 时返回 false（空了）。没有任何锁——因为生产者（IRQ handler）不会被抢占，消费者在关中断保护下操作。

键盘驱动的双路分发是在 irq1_handler 末尾加的条件编译代码——解析完 scan code 之后，除了推入键盘自己的队列，还构造一个 GUI Event 推入全局 EventQueue。这样窗口管理器只需要从一个队列就能读到所有输入。

你会发现这里有一个设计上的不优雅：全局 EventQueue 定义在 Mouse 类里作为 g_event_queue_ 成员。键盘事件要"通过 Mouse 类"才能访问队列——从语义上说这不太合理。正确的做法应该是 GUI 子系统拥有一个独立的全局队列，鼠标和键盘都往里推。但我们为了减少全局符号和依赖关系暂时这么做了，后续重构时会调整。

## 收尾

验证方式很简单——`make run` 后观察串口输出，应该能看到 `[MOUSE] Mouse enabled (ACK received).` 和 `[MOUSE] PS/2 mouse driver initialised.`。移动鼠标不会在串口上有直接输出（事件被推入队列了），但如果你在 tick 回调里加了统计打印，就能看到 MouseMove 事件在持续进来。

有一个有趣的 QEMU 特有问题：VNC 窗口里会出现两个光标。一个是我们自己画的箭头，一个是 QEMU 的圆点覆盖层。它们之间有一个固定偏移，方向取决于我们初始化光标的位置。这不是 bug——PS/2 协议只报告相对位移，QEMU VNC 用的是宿主机绝对坐标，两者从不同原点出发必然有偏差。临时解决方案是在 QEMU 启动参数里加 `-usb -device usb-tablet`，长期方案是实现 USB HID 驱动获取绝对坐标。

到这里，我们的内核已经有了一个能工作的鼠标输入子系统。下一章我们在这基础上搭建窗口管理器，让鼠标事件真正驱动窗口的交互行为。

## 参考资料
- Intel SDM: Vol.3A Section 6.4 — IDT Gate Types (interrupt gates clear IF)
- OSDev Wiki: [PS/2 Mouse](https://wiki.osdev.org/PS/2_Mouse) — 3-byte packet format, initialization, 9-bit delta extraction
- OSDev Wiki: [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) — controller commands, config byte layout
- SerenityOS: [PS2MouseDevice](https://github.com/SerenityOS/serenity/blob/master/Kernel/Bus/PS2/MouseDevice.cpp) — production PS/2 mouse driver reference
- Linux: [drivers/input/serio/i8042.c](https://github.com/torvalds/linux/blob/master/drivers/input/serio/i8042.c) — full 8042 controller driver
