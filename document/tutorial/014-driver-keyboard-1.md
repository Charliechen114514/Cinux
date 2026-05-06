# 014-1 控制器握手：让键盘"上线"——PS/2 8042 初始化与 IRQ1 绑定

> 标签：PS/2, 8042, keyboard controller, I/O 端口, IRQ1, scan code translation
> 前置：[013 VGA 帧缓冲区与控制台](013-driver-vga-fb-3.md)

## 前言

到了这一章，我们的内核已经能往屏幕上画字了——tag 013 让 Cinux 拥有了控制台输出的能力。但说实话，一个只会自说自话的操作系统多少有点寂寞：屏幕上全是内核自己打印的启动信息，用户在键盘上敲什么都毫无反应。这一章我们要做的事情，就是打通键盘输入这条链路，让内核从"只能看"变成"能对话"。

你可能会觉得"键盘驱动不就是读端口 0x60 吗，有什么难的"——如果你真的这么想，那你大概还没被 PS/2 控制器的初始化序列折磨过。8042 控制器有一套自己的命令-响应协议，配置字节里藏着几个一不留神就翻车的位域，而且 OSDev Wiki 推荐的完整初始化流程足足有 10 步。我们先别急，一步一步来拆解。

整条链路涉及三个层面的配合：硬件层面的 8042 控制器初始化（这是本篇的核心），中断层面的 IRQ1 桩函数绑定（修改 `interrupts.S` 中的一行），以及内核主循环的轮询集成。大约 250 行代码，但它们打通了从物理键盘到内核事件队列的完整通路。

## 环境说明

我们运行在 QEMU 的默认配置下，QEMU 会模拟一个标准的 Intel 8042 PS/2 控制器和一个 PS/2 键盘。8042 的两个 I/O 端口是 0x60（数据）和 0x64（状态/命令），这在所有 x86 PC 上都是固定的。QEMU 模拟的 8042 行为非常标准——响应几乎是即时的，自检一定会通过，不会有真机上那些奇奇怪怪的时序问题。正因为如此，Cinux 的初始化序列采用了简化版本：跳过了 USB legacy support handoff、双通道检测、端口接口测试和设备复位这些步骤，只保留让键盘在 QEMU 里跑起来所必需的最小集。如果你将来打算在物理机器上跑，这些步骤需要补回来。

IDT 在 tag 010/011 中已经设置完毕，IRQ0-7 映射到 IDT 向量 0x20-0x27，IRQ1 对应向量 0x21。PIC 的 mask/unmask 机制也在 tag 011 中实现好了。也就是说，中断基础设施已经就绪，我们只需要把键盘的中断处理函数挂上去就行。

## 先搞清楚 8042 是什么

在写代码之前，我们花一点时间搞清楚 8042 PS/2 控制器的前世今生。这个芯片最早是 Intel 8042 微控制器——一个独立的 40-pin DIP 芯片，焊在主板上负责键盘和系统的串行通信。随着主板集成度越来越高，8042 的功能被吸收进了主板芯片组的 Super I/O 芯片，再后来变成了 Advanced Integrated Peripheral (AIP) 的一部分。今天的 x86 主板上你已经找不到一颗单独叫"8042"的芯片了，但它的编程接口——I/O 端口 0x60 和 0x64——从 1984 年 IBM AT 时代到现在四十多年了一直没变。这是一种典型的 PC 兼容性遗产：硬件在进化，软件接口永远向后兼容。

8042 控制器管理两个 PS/2 端口：第一端口（keyboard port）连接键盘，第二端口（auxiliary port）连接鼠标。两个端口共享同一套 I/O 端口地址，通过命令字节来区分操作对象。控制器内部有一个输出缓冲区和一个输入缓冲区，各自只有一个字节深度——这意味着如果你读慢了，新的数据就会覆盖旧的，所以中断处理函数必须尽快把数据取走。

从数据流的角度看，键盘本身发送的是 scan code set 2 的编码（这是 AT 键盘的原生编码），但 8042 控制器有一个 translation mode，可以把 set 2 自动翻译成 set 1（XT 时代的编码）。我们的驱动全部基于 set 1 编写，所以这个翻译功能必须启用——如果忘了，收到的就是 set 2 的扫描码，整个解码全部错乱，而且不会有任何报错提示，调试起来非常痛苦。OSDev Wiki 的 [PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard) 页面对这个翻译机制有详细说明。

## 接口设计：KeyEvent 和 Keyboard 类

我们从驱动对外的契约开始看起。`keyboard.hpp` 定义了上层代码需要知道的一切——`KeyEvent` 里面有什么字段，`Keyboard` 类提供了哪些方法。

```cpp
#pragma once

#include <stdint.h>

// Forward declaration -- InterruptFrame is defined in idt.hpp
namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers {
```

头文件只依赖 `<stdint.h>`，不依赖任何内核内部头文件。`InterruptFrame` 通过前向声明引入，避免了对 `idt.hpp` 的编译期依赖。这是一个很干净的做法——驱动头文件不应该把中断框架的实现细节暴露给所有使用者。你如果在其他 OS 中观察，会发现 Linux 的 `serio` 驱动层也遵循类似的原则：高层驱动只包含底层抽象接口的头文件，不直接引用硬件寄存器定义。

接下来是 `KeyEvent` 结构体，它承载了一次按键事件的全部信息：

```cpp
struct KeyEvent {
    char    ascii;     ///< ASCII character (0 if non-printable or key release)
    uint8_t scancode;  ///< Raw scan code set 1 value
    bool    pressed;   ///< true = key press (make), false = key release (break)
    bool    shift;     ///< true if either Shift is held
    bool    ctrl;      ///< true if either Ctrl is held
    bool    alt;       ///< true if either Alt is held
};
```

这个结构体的设计有几个值得说道的地方。`ascii` 在按键释放和非可打印键时为 0，这意味着上层代码只需要检查 `ev.pressed && ev.ascii != 0` 就能过滤出可打印字符，逻辑非常直观。`scancode` 保存了原始扫描码（包括最高位的 make/break 标志），这样上层如果需要区分 F1 (0x3B) 和 F2 (0x3C) 这种没有 ASCII 映射的键，可以通过 scancode 做到。三个 modifier 布尔值是事件发生时刻的快照，不是全局状态的引用——irq handler 在构建事件时会把当前的 modifier 状态拷贝进 KeyEvent，所以即使后续 modifier 发生变化，已经入队的事件不会受到影响。

这个设计和 xv6 的做法形成了鲜明对比。xv6 的 `kbd.c` 没有定义结构体——它的 `kbdgetc()` 直接返回一个 `char`，所有修饰键状态隐藏在驱动内部的 `static uint shift` 位掩码里。调用者拿到的就是一个裸字符，无法知道这个字符伴随了哪些 modifier，也无法区分"按下"和"释放"事件。xv6 的 console 驱动只能在拿到字符的那一刻去读全局状态，但此时 modifier 可能已经变了。Cinux 的 `KeyEvent` 把状态快照和字符绑在一起传递，从信息完整性的角度看更可靠。

接下来是 `Keyboard` 类：

```cpp
class Keyboard {
public:
    static void init();
    static void irq1_handler(cinux::arch::InterruptFrame* frame);
    static bool poll(KeyEvent& out);

private:
    static constexpr uint32_t KEY_QUEUE_SIZE = 64;

    static void enqueue(const KeyEvent& ev);

    // Ring buffer storage
    static KeyEvent queue_[KEY_QUEUE_SIZE];
    static uint32_t head_;
    static uint32_t tail_;

    // Modifier tracking state
    static bool shift_held_;
    static bool ctrl_held_;
    static bool alt_held_;
};

}  // namespace cinux::drivers
```

所有成员都是 `static` 的。x86 PC 上只有一个 PS/2 键盘控制器，不存在多实例的场景。使用全静态设计避免了动态分配，也不需要在内核堆上 new 一个对象——我们现在连堆分配器都没有。ring buffer 的容量硬编码为 64 个 `KeyEvent`，实际可用容量是 63（因为环形缓冲区需要浪费一个槽位来区分"满"和"空"两种状态），对于一个打字速度正常的场景已经绑绑有余了。

把 `enqueue()` 放在 `private` 里、只把 `poll()` 暴露出去，这个访问控制是有意为之的：只有 irq handler 自己会调用 `enqueue()`，外部代码只应该通过 `poll()` 来消费事件。这是一种经典的 producer-consumer 隔离——ISR 是 producer，main loop 是 consumer，中间隔着 ring buffer。

相比之下，Linux 的 PS/2 键盘栈分成了三层：`i8042.c`（底层控制器驱动，挂在 serio 总线上）、`atkbd.c`（高层键盘驱动，绑定 serio 端口）、以及 input 子系统（`/dev/input/eventX`）。atkbd 把解码后的键码通过 `input_event()` 推入 input 子系统的事件队列，用户空间通过 `read()` 系统调用取走。这种分层比 Cinux 复杂得多，但设计理念是相同的：ISR 做最少的工作（只读扫描码入队），解码和翻译留给其他层。

## 点火！PS/2 控制器初始化序列

现在我们进入本篇的核心——`Keyboard::init()` 的完整实现。OSDev Wiki 的 [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) 页面推荐了一套 10 步的完整初始化序列，Cinux 采用了简化版本，只保留了让键盘在 QEMU 中正常工作所必需的步骤。

### 端口协议与辅助函数

在看 init 之前，先搞清楚和 8042 通信的基本协议。驱动文件开头定义了三组常量：

```cpp
namespace Ps2Port {
constexpr uint16_t DATA    = 0x60;  ///< PS/2 data register (read/write)
constexpr uint16_t STATUS  = 0x64;  ///< PS/2 status register (read)
constexpr uint16_t COMMAND = 0x64;  ///< PS/2 controller command (write)
}  // namespace Ps2Port
```

8042 控制器只使用两个 I/O 端口，但它们承担了全部的通信职责。端口 0x60 是数据寄存器，读写方向取决于上下文：读的时候取走的是控制器输出缓冲区里的数据（键盘发来的扫描码或者控制器对命令的响应），写的时候则是往输入缓冲区放数据（发给控制器的命令参数）。端口 0x64 是双角色的：读它得到状态寄存器，写它是发送控制器命令。Intel SDM Vol.2A 的 IN/OUT 指令章节对端口 I/O 的读写语义有明确描述——在保护模式下，端口访问要求 CPL <= IOPL 或者 TSS 中的 I/O permission bitmap 允许对应端口，我们运行在 ring 0 所以没有限制。

```cpp
namespace Ps2Cmd {
constexpr uint8_t READ_CONFIG   = 0x20;
constexpr uint8_t WRITE_CONFIG  = 0x60;
constexpr uint8_t DISABLE_PORT2 = 0xA7;
constexpr uint8_t ENABLE_PORT2  = 0xA8;
constexpr uint8_t DISABLE_PORT1 = 0xAD;
constexpr uint8_t ENABLE_PORT1  = 0xAE;
constexpr uint8_t SELF_TEST     = 0xAA;
}  // namespace Ps2Cmd
```

这些命令字节全部发送到 0x64 端口。命令编号的规律很有意思——0xA0 系列都是控制器级别的管理命令，0x20/0x60 是配置读写。OSDev Wiki 的 8042 文档有一个完整的命令列表，我们这里只用了初始化过程中必要的 7 个。

```cpp
namespace Ps2Status {
constexpr uint8_t OUTPUT_FULL = 0x01;
constexpr uint8_t INPUT_FULL  = 0x02;
}  // namespace Ps2Status
```

状态寄存器有很多位，但我们只关心 bit 0（输出缓冲区满，表示控制器有数据等着被读走）和 bit 1（输入缓冲区满，表示上一次写入的命令还没被控制器处理完）。这两个标志决定了所有的等待逻辑。

然后是两个底层辅助函数，它们封装了和 8042 通信时的等待协议：

```cpp
namespace {

void wait_input_empty() {
    uint32_t timeout = 100000;
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::INPUT_FULL) != 0) {
        if (--timeout == 0) {
            return;
        }
        __asm__ volatile("pause");
    }
}

void send_command(uint8_t cmd) {
    wait_input_empty();
    io_outb(Ps2Port::COMMAND, cmd);
}

}  // anonymous namespace
```

`wait_input_empty()` 是整个驱动中最底层的同步原语。它的逻辑是不断读 0x64 端口的状态寄存器，检查 bit 1（INPUT_FULL），如果为 1 说明控制器还没处理完上一条命令，继续等。`__asm__ volatile("pause")` 是 x86 的超线程优化提示——告诉 CPU "我现在在自旋等待"，让超线程体系中的另一个逻辑核能更高效地执行。超时计数器设为 100000，在 QEMU 里永远不会触发（模拟的 8042 响应几乎是即时的），但在真机上如果控制器出了问题，至少不会永远卡死。

这两个函数放在匿名命名空间里，表示它们是本编译单元私有的，不会暴露给其他 .cpp 文件。你如果去看 Linux 的 `i8042.c`，会发现它用了一组类似的等待函数，但超时机制更复杂——它会检查 jiffies 来做真正的 wall-clock 超时，而不是用一个简单的计数器。对于教学 OS 来说，计数器超时已经足够了。

### init() 逐步拆解

**第一步：禁用两个 PS/2 端口。**

```cpp
void Keyboard::init() {
    // Step 1: Disable both PS/2 device ports so no data arrives
    send_command(Ps2Cmd::DISABLE_PORT1);
    send_command(Ps2Cmd::DISABLE_PORT2);
```

命令 0xAD 禁用第一端口（键盘），0xA7 禁用第二端口（鼠标）。为什么要先禁用？因为后续操作会读写配置字节、发送命令并等待响应，如果这时候键盘或鼠标还在往控制器里塞数据，就会干扰我们的命令-响应握手。先关掉两个端口，确保控制器处于安静状态。

**第二步：清空输出缓冲区。**

```cpp
    // Step 2: Flush the output buffer (discard any stale data)
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::OUTPUT_FULL) != 0) {
        io_inb(Ps2Port::DATA);
    }
```

按键事件可能在禁用端口之前就已经到达了控制器的 FIFO，如果不把它们排干净，后续读取配置字节时可能读到的不是命令响应而是残留的扫描码。flush 的方式很简单粗暴——只要状态寄存器的 bit 0 为 1，就读一次 0x60 端口把数据扔掉，直到缓冲区完全干净为止。

**第三步和第四步：读取、修改、写回配置字节。**

接下来是整个初始化过程中最关键的操作——配置控制器的行为。

```cpp
    // Step 3: Read the current controller configuration byte
    send_command(Ps2Cmd::READ_CONFIG);
    wait_input_empty();
    uint8_t config = io_inb(Ps2Port::DATA);
```

先发 0x20 命令读出当前的配置字节。这条命令的工作方式是：先发命令到 0x64，等控制器处理完，然后从 0x60 读回一个字节的配置数据。

```cpp
    config |= 0x01;   // Enable keyboard IRQ (IRQ1)
    config &= ~0x02;  // Disable mouse IRQ (IRQ12)
    config |= 0x40;   // Enable scan code set 2 -> set 1 translation

    send_command(Ps2Cmd::WRITE_CONFIG);
    wait_input_empty();
    io_outb(Ps2Port::DATA, config);
```

我们修改了配置字节的三个位。bit 0 置 1 启用第一端口的中断，即 IRQ1。如果这一位不设，键盘按键后控制器不会触发中断，驱动就变成了一个摆设——你按什么键都没反应，但不会报任何错误。bit 1 清零禁用第二端口的中断，即 IRQ12。bit 6 置 1 启用扫描码转换——这是 PS/2 键盘驱动中最容易踩坑的配置之一。如果忘记启用这个转换位，收到的就是 set 2 的扫描码，整个解码全部错乱。

你会发现，xv6 的键盘驱动完全跳过了这一步——它依赖 BIOS 已经把 8042 初始化好的默认状态。BIOS 的 POST 阶段会执行完整的 8042 初始化，包括启用翻译模式，所以 xv6 启动时控制器已经在正确状态了。但在真正的产品级 OS 中就不行了——Linux 的 `i8042.c` 会在模块初始化时重新配置一遍控制器，不信任 BIOS 留下的任何状态。

**第五步：控制器自检。**

```cpp
    send_command(Ps2Cmd::SELF_TEST);
    wait_input_empty();
    uint8_t result = io_inb(Ps2Port::DATA);

    if (result == 0x55) {
        kprintf("[KBD] PS/2 controller self-test passed.\n");
    } else {
        kprintf("[KBD] PS/2 controller self-test FAILED (got 0x%02X, expected 0x55)\n",
                result);
    }
```

命令 0xAA 触发控制器的内置自检程序，正常情况下返回 0x55。这里有一个需要注意的坑：OSDev Wiki 的 8042 文档警告说，在某些硬件上 0xAA 命令可能会重置控制器——也就是说之前写好的配置字节可能被清掉。Cinux 的做法是在自检之前写配置，在 QEMU 中不会有问题，但如果你打算在物理机器上跑，可能需要在自检之后重新写一次配置字节。Linux 的 `i8042.c` 处理得更谨慎——它在自检之后会显式地把配置字节重新写回。

**第六步：重新启用键盘端口。**

```cpp
    send_command(Ps2Cmd::ENABLE_PORT1);
```

万事俱备，最后一步是重新启用第一端口。命令 0xAE 之后，控制器开始把键盘发来的扫描码放入输出缓冲区，并在 IRQ1 线上拉高电平通知 PIC。

**第七步：重置内部状态。**

```cpp
    head_ = 0;
    tail_ = 0;
    shift_held_ = false;
    ctrl_held_  = false;
    alt_held_   = false;

    kprintf("[KBD] Keyboard driver initialised.\n");
}
```

把驱动的内部状态清零——环形缓冲区的读写指针归零，三个 modifier 标志归 false。虽然静态变量在定义时已经初始化为 0 了，但显式重置一次是好习惯。

## 上号！IRQ1 桩函数绑定

控制器的硬件初始化就绪后，还需要在 IDT 中注册中断处理函数。回顾 tag 010/011 的内容，IRQn 映射到 IDT 向量 0x20 + n，Intel SDM Vol.3A Section 6 中对中断向量分配有详细说明。之前 IRQ1 一直绑定在默认的空操作 handler 上：

```asm
ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
```

这一行修改看似不起眼，却是整个链路的关键一环：

```asm
ISR_NOERRCODE irq1_stub,  keyboard_irq1_handler /* IRQ1(0x21): Keyboard */
```

`keyboard_irq1_handler` 是一个 `extern "C"` 函数，定义在 `keyboard.cpp` 的末尾：

```cpp
extern "C" void keyboard_irq1_handler(cinux::arch::InterruptFrame* frame) {
    cinux::drivers::Keyboard::irq1_handler(frame);
}
```

为什么要这个 C 链接的桥接函数？因为 `ISR_NOERRCODE` 宏生成的汇编桩会直接 `call` 一个 C 符号名，而 C++ 的 name mangling 会把 `Keyboard::irq1_handler` 变成类似 `_ZN6cinux8drivers8Keyboard12irq1_handlerEPNS_4arch14InterruptFrameE` 这样的名字，汇编代码没法直接引用。`extern "C"` 保证了符号名就是 `keyboard_irq1_handler`，汇编桩可以无障碍地 call 它。

这是 C++ 内核中处理中断回调的标准手法。Linux 用纯 C 所以没有这个问题；SerenityOS 也使用 `extern "C"` 桥接函数来注册中断处理程序。

## kernel_main 中的集成

最后我们来看 `kernel_main` 中如何把键盘驱动编织进启动序列。键盘初始化放在 console 之后、`sti` 之前：

```cpp
    Keyboard::init();

    PIC::unmask(0);
    PIC::unmask(1);
    __asm__ volatile("sti");
```

相比 tag 013 只 unmask IRQ0，这里多了一行 `PIC::unmask(1)`。`PIC::unmask(1)` 清除 mask 寄存器的 bit 1，允许 PIC 在 IRQ1 线上有信号时向 CPU 发送中断。

顺便一提，`pit.cpp` 里原来那个每秒打印一次 `[TICK] uptime` 的输出被删掉了。这不是偷懒——而是因为键盘回显和每秒一次的 tick 打印混在一起会非常混乱，你敲一个 `a`，屏幕上可能出现 `[TICK] uptime: 3s`a` 这种交错输出，体验很差。把噪音源去掉是正确的选择。

## 设计决策：我们为什么这么做

**全静态类 vs. 全局函数。** 把 `init()`/`poll()`/`irq1_handler()` 挂在一个 `Keyboard` 类下面，比散落成 `keyboard_init()`、`keyboard_poll()` 这样的 C 风格函数更有组织性。如果将来需要支持多键盘（比如 USB 键盘热插拔），可以把静态成员改成实例成员，接口变化很小。

**简化初始化序列。** OSDev 的 10 步完整序列包括 USB controller handoff、双通道检测、端口测试、设备复位等步骤。这些在 QEMU 中要么不需要，要么行为不确定。对于一个教学 OS 来说，先让它在 QEMU 中跑起来，真机适配是后续的事。

**不 ACK 键盘命令。** 发送命令给键盘本身（不是控制器）时，键盘会回复 0xFA (ACK)。Cinux 没有向键盘发送任何命令——比如设置 LED 灯、切换扫描码集——所以不需要 ACK 处理。这省去了不少复杂性，但代价是无法控制键盘上的 LED。

## 参考资料

- OSDev Wiki: [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) -- 8042 控制器命令列表、配置字节格式、10 步初始化序列、自检可能重置控制器的警告
- OSDev Wiki: [PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard) -- 扫描码集定义、translation mode 机制
- OSDev Wiki: [8259 PIC](https://wiki.osdev.org/8259_PIC) -- IRQ masking/unmasking、EOI 协议
- Intel SDM Vol.2A -- IN/OUT 指令说明，端口 I/O 保护模式要求 (CPL <= IOPL)
- Intel SDM Vol.3A Section 6 -- 中断/异常处理，IDT 门描述符，IRQ 到 IDT 向量的映射关系 (IRQn -> vector 0x20+n)
- xv6 `kbd.c` -- MIT 教学内核的键盘驱动实现，依赖 BIOS 初始化后的 8042 默认状态
- Linux `drivers/input/serio/i8042.c` -- 完整版 8042 控制器驱动，自检后重写配置字节
- ToaruOS `kernel/arch/x86_64/ps2hid.c` -- 键盘/鼠标合并驱动，类似 Cinux 的简化初始化步骤
- SerenityOS `Kernel/Bus/SerialIO/Controller.h` -- `SerialIOController` 基类，带设备识别的完整初始化
