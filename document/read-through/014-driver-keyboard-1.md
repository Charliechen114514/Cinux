# 014-1 通读：控制器握手——8042 PS/2 键盘初始化与 IRQ1 绑定

## 概览

本文是 tag `014_driver_keyboard` 两篇通读教程的第一篇，聚焦于 PS/2 键盘控制器的硬件初始化和中断绑定。在上一章 tag 013 中我们让屏幕拥有了显示文字的能力，现在该让内核"听"到用户的输入了——一个能接收按键事件的内核，才算真正具备了交互能力。

我们从三个层面展开讲解：首先是 `keyboard.hpp` 中 `KeyEvent` 结构体和 `Keyboard` 类的完整接口设计，它定义了驱动和上层之间的契约；然后是 `keyboard.cpp` 中 `Keyboard::init()` 的完整实现，涵盖 8042 控制器的端口协议、命令序列、配置字节和自检流程；最后是 `interrupts.S` 中 IRQ1 中断桩的绑定和 `kernel_main` 中的初始化集成。整篇文章大约涉及 250 行代码，但它们打通了从硬件控制器到内核中断系统的完整链路。

## 架构图

```
                    硬件层
    ┌─────────────────────────────────────┐
    │  PS/2 Keyboard  ──wire──► 8042     │
    │  (scan code set 2)      Controller │
    │                          │         │
    │  I/O Ports: 0x60 (data)  0x64 (cmd)│
    │  IRQ1 ──────────────────► PIC      │
    └──────────┬──────────────────┬──────┘
               │                  │
    ┌──────────▼──────────────────▼──────┐
    │            内核层                    │
    │                                     │
    │  interrupts.S                       │
    │    irq1_stub ──► keyboard_irq1_handler
    │                       │             │
    │  keyboard.cpp         ▼             │
    │    Keyboard::init()  ◄─── 控制器初始化│
    │    Keyboard::irq1_handler()         │
    │        └──读 0x60 → enqueue()      │
    │    Keyboard::poll() ←── dequeue    │
    │                                     │
    │  kernel_main                        │
    │    Keyboard::init()                 │
    │    PIC::unmask(1)                   │
    │    sti                              │
    │    hlt → poll → Console::putc       │
    └─────────────────────────────────────┘

    初始化序列（本文重点）:

    init()
      ├── send_command(0xAD)   禁用第一端口
      ├── send_command(0xA7)   禁用第二端口
      ├── flush output buffer  清空残留数据
      ├── READ_CONFIG(0x20)    读取配置字节
      ├── 修改: IRQ1=on, IRQ12=off, Translation=on
      ├── WRITE_CONFIG(0x60)   写回配置
      ├── SELF_TEST(0xAA)      控制器自检 → 期望 0x55
      ├── ENABLE_PORT1(0xAE)   重新启用键盘端口
      └── reset state          清空缓冲区/修饰键
```

## 代码精讲

### keyboard.hpp —— 接口设计与 KeyEvent 结构体

我们从驱动对外的接口开始看起。这个头文件定义了整个键盘驱动的契约——上层代码只需要知道 `KeyEvent` 里面有什么字段，以及 `Keyboard` 类提供了哪些静态方法。

```cpp
#pragma once

#include <stdint.h>

// Forward declaration -- InterruptFrame is defined in idt.hpp
namespace cinux::arch {
struct InterruptFrame;
}  // namespace cinux::arch

namespace cinux::drivers {
```

头文件只依赖 `<stdint.h>`，不依赖任何内核内部头文件。`InterruptFrame` 通过前向声明引入，避免了对 `idt.hpp` 的编译期依赖，这是一个很干净的做法——驱动头文件不应该把中断框架的实现细节暴露给所有使用者。

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

这个结构体的设计有几个值得说道的地方。首先，`ascii` 在按键释放和非可打印键时为 0，这意味着上层代码只需要检查 `ev.pressed && ev.ascii != 0` 就能过滤出可打印字符，逻辑非常直观。其次，`scancode` 保存了原始的扫描码——包括最高位的 make/break 标志，这样上层如果需要区分 F1 (0x3B) 和 F2 (0x3C) 这种没有 ASCII 映射的键，可以通过 scancode 做到。最后，三个 modifier 布尔值是事件发生时刻的快照，不是全局状态的引用——这一点在后续第二篇中会看到，irq handler 在构建事件时会把当前的 modifier 状态拷贝进 KeyEvent，所以即使后续 modifier 发生变化，已经入队的事件不会受到影响。

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

所有成员都是 `static` 的。这是因为 x86 PC 上只有一个 PS/2 键盘控制器，不存在多实例的场景。使用全静态设计避免了动态分配，也不需要在内核堆上 new 一个对象——毕竟我们现在连堆分配器都没有。ring buffer 的容量硬编码为 64 个 `KeyEvent`，实际可用容量是 63（因为环形缓冲区需要浪费一个槽位来区分"满"和"空"两种状态），对于一个打字速度正常的场景已经绑绑有余了。

把 `enqueue()` 放在 `private` 里、只把 `poll()` 暴露出去，这个访问控制是有意为之的：只有 irq handler 自己会调用 `enqueue()`，外部代码只应该通过 `poll()` 来消费事件。这是一种经典的 producer-consumer 隔离——ISR 是 producer，main loop 是 consumer，中间隔着 ring buffer。

### keyboard.cpp —— PS/2 常量与辅助函数

实现文件的开头是一组精心组织的命名空间，把 PS/2 控制器的端口、命令、状态位和扫描码常量分门别类地收纳起来。

```cpp
#include "keyboard.hpp"

#include <stdint.h>

#include "kernel/arch/x86_64/io.hpp"
#include "kernel/arch/x86_64/pic.hpp"
#include "kernel/lib/kprintf.hpp"

using cinux::arch::PIC;
using cinux::io::io_inb;
using cinux::io::io_outb;
using cinux::io::io_wait;
using cinux::lib::kprintf;

namespace cinux::drivers {
```

驱动的依赖很克制：`io.hpp` 提供端口 I/O（`io_inb`/`io_outb`/`io_wait`），`pic.hpp` 提供 EOI 发送和 IRQ unmask，`kprintf.hpp` 提供日志输出。这三个是键盘驱动运行所需的全部外部依赖。

```cpp
namespace Ps2Port {
constexpr uint16_t DATA    = 0x60;  ///< PS/2 data register (read/write)
constexpr uint16_t STATUS  = 0x64;  ///< PS/2 status register (read)
constexpr uint16_t COMMAND = 0x64;  ///< PS/2 controller command (write)
}  // namespace Ps2Port
```

8042 控制器只使用两个 I/O 端口，但它们承担了全部的通信职责。端口 0x60 是数据寄存器，读写方向取决于上下文：读的时候取走的是控制器输出缓冲区里的数据（键盘发来的扫描码，或者控制器对命令的响应），写的时候则是往输入缓冲区放数据（发给控制器的命令参数）。端口 0x64 是双角色的：读它得到状态寄存器，写它是发送控制器命令。两个端口地址复用 0x64 并不冲突，因为 IN 和 OUT 是不同的 x86 指令，Intel SDM Vol.2A 的 IN/OUT 指令章节对端口的读写语义有明确描述。

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

这些命令字节全部发送到 0x64 端口（通过 `send_command()` 辅助函数）。OSDev Wiki 的 [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) 页面有一个完整的命令列表，我们这里只用了初始化过程中必要的 7 个。命令编号的规律很有意思——0xA0 系列都是控制器级别的管理命令，0x20/0x60 是配置读写，这两组是最常用的。

```cpp
namespace Ps2Status {
constexpr uint8_t OUTPUT_FULL = 0x01;
constexpr uint8_t INPUT_FULL  = 0x02;
}  // namespace Ps2Status
```

状态寄存器有很多位，但我们只关心 bit 0（输出缓冲区满，表示控制器有数据等着被读走）和 bit 1（输入缓冲区满，表示上一次写入的命令还没被控制器处理完）。这两个标志决定了所有的等待逻辑。

接下来是扫描码常量，这部分和本文的初始化主题关系不大（它们主要用在 irq handler 的解码逻辑中，第二篇会详细讲解），但这里先列出来作为参照：

```cpp
namespace ScanCode {
constexpr uint8_t LSHIFT   = 0x2A;
constexpr uint8_t RSHIFT   = 0x36;
constexpr uint8_t LCTRL    = 0x1D;
constexpr uint8_t LALT     = 0x38;
constexpr uint8_t CAPS     = 0x3A;
constexpr uint8_t EXTENDED = 0xE0;
}  // namespace ScanCode
```

这些是 scan code set 1 中的特殊键。值得注意的是 CAPS (0x3A) 虽然被定义了，但当前驱动并没有实现 CapsLock 的切换逻辑——它只跟踪 Shift/Ctrl/Alt 的即时按下状态。EXTENDED (0xE0) 是扩展键前缀，像方向键、右 Ctrl、右 Alt 这些键都会在真实扫描码之前发送一个 0xE0 字节。

然后是静态成员变量的定义：

```cpp
KeyEvent Keyboard::queue_[KEY_QUEUE_SIZE] = {};
uint32_t Keyboard::head_ = 0;
uint32_t Keyboard::tail_ = 0;

bool Keyboard::shift_held_ = false;
bool Keyboard::ctrl_held_  = false;
bool Keyboard::alt_held_   = false;
```

C++ 的规则是，类的静态成员变量必须在 .cpp 文件中恰好定义一次，否则链接器会报 undefined reference。`= {}` 是值初始化，把整个数组的所有字节清零，确保 ring buffer 从干净状态开始。

最后是两个内部辅助函数，它们封装了和 8042 控制器通信时的等待协议：

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

`wait_input_empty()` 是整个驱动中最底层的同步原语。它的逻辑是：不断读 0x64 端口的状态寄存器，检查 bit 1（INPUT_FULL），如果为 1 说明控制器还没处理完上一条命令，继续等。`__asm__ volatile("pause")` 是 x86 的超线程优化提示——告诉 CPU "我现在在自旋等待，可以把执行资源让给同一核心上的另一个超线程"，这个习惯在内核代码中值得保持。超时计数器设为 100000，在 QEMU 里永远不会触发（模拟的 8042 响应几乎是即时的），但在真机上如果控制器出了问题，至少不会永远卡死。

`send_command()` 就是"等输入缓冲区空 → 写命令"的标准两步操作。这两个函数放在匿名命名空间里，表示它们是本编译单元私有的，不暴露给其他 .cpp 文件。

### Keyboard::init() —— PS/2 控制器初始化序列

这是本篇的核心函数，完整实现了 8042 控制器的初始化流程。OSDev Wiki 推荐了一套 10 步的完整初始化序列，Cinux 采用了简化版本——跳过了 USB legacy 支持、双通道检测、端口接口测试和设备复位/识别，只保留了让键盘能在 QEMU 中正常工作所必须的步骤。

```cpp
void Keyboard::init() {
    // Step 1: Disable both PS/2 device ports so no data arrives
    send_command(Ps2Cmd::DISABLE_PORT1);
    send_command(Ps2Cmd::DISABLE_PORT2);
```

第一步是禁用两个 PS/2 端口。命令 0xAD 禁用第一端口（键盘），0xA7 禁用第二端口（鼠标）。为什么要先禁用？因为后续操作会读写配置字节、发送命令并等待响应，如果这时候键盘或鼠标还在往控制器里塞数据，就会干扰我们的命令-响应握手。先关掉两个端口，确保控制器处于安静状态。

```cpp
    // Step 2: Flush the output buffer (discard any stale data)
    while ((io_inb(Ps2Port::STATUS) & Ps2Status::OUTPUT_FULL) != 0) {
        io_inb(Ps2Port::DATA);
    }
```

第二步是清空输出缓冲区。按键事件可能在禁用端口之前就已经到达了控制器的 FIFO，如果不把它们排干净，后续读取配置字节时可能读到的不是命令响应而是残留的扫描码。flush 的方式很简单粗暴——只要状态寄存器的 bit 0 (OUTPUT_FULL) 为 1，就读一次 0x60 端口把数据扔掉，直到缓冲区完全干净为止。这里没有超时保护，因为在禁用端口之后不会再有新数据进来，循环一定能结束。

```cpp
    // Step 3: Read the current controller configuration byte
    send_command(Ps2Cmd::READ_CONFIG);
    wait_input_empty();
    uint8_t config = io_inb(Ps2Port::DATA);
```

第三步读取当前的"Controller Configuration Byte"。这条命令的工作方式是：先发 0x20 命令到 0x64，等控制器处理完，然后从 0x60 读回一个字节的配置数据。配置字节各位的含义在 OSDev Wiki 的 8042 文档中有完整描述，我们只关心三个位：

```cpp
    // Step 4: Modify config: enable IRQ1 (bit 0), disable IRQ12 (bit 1 clear),
    //         disable mouse translation (bit 6 clear)
    config |= 0x01;   // Enable keyboard IRQ (IRQ1)
    config &= ~0x02;   // Disable mouse IRQ (IRQ12)
    config |= 0x40;   // Enable scan code set 2 → set 1 translation

    send_command(Ps2Cmd::WRITE_CONFIG);
    wait_input_empty();
    io_outb(Ps2Port::DATA, config);
```

这里是整个初始化过程中最关键的一步。我们修改了配置字节的三个位，每一位都有明确的目的。

bit 0 置 1 是启用第一端口（键盘）的中断，即 IRQ1。如果这一位不设，键盘按键后控制器不会触发中断，irq handler 永远不会被调用，驱动就变成了一个摆设。bit 1 清零是禁用第二端口（鼠标）的中断，即 IRQ12。Cinux 当前不处理鼠标输入，关掉它可以避免收到无关中断。bit 6 置 1 是启用扫描码转换——这是 PS/2 键盘驱动中最容易踩坑的配置之一。键盘本身发送的是 scan code set 2 的编码，但控制器的转换功能会自动把 set 2 翻译成 set 1 再交给软件。我们的查找表和解码逻辑全部基于 set 1 编写，如果忘记启用这个转换位，收到的就是 set 2 的扫描码，整个解码全部错乱，而不会有任何报错提示。OSDev Wiki 的 [PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard) 页面对这个转换机制有详细说明。

写回配置的方式和读取类似：先发 0x60 命令到 0x64，等缓冲区空，然后把新的配置字节写到 0x60。

```cpp
    // Step 5: Controller self-test (command 0xAA, expect 0x55)
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

命令 0xAA 触发控制器的内置自检程序，正常情况下返回 0x55。这个自检在 QEMU 里一定会通过（模拟器不会模拟硬件故障），但在真机上如果控制器真的出了问题，这个检查能帮我们及早定位。需要注意的是，OSDev Wiki 提到在某些硬件上 0xAA 命令可能会重置控制器——也就是说之前写好的配置字节可能被清掉。Cinux 的做法是在自检之前写配置，在 QEMU 中不会有问题，但如果你打算在物理机器上跑，可能需要在自检之后重新写一次配置字节。

```cpp
    // Step 6: Re-enable the first PS/2 port (keyboard)
    send_command(Ps2Cmd::ENABLE_PORT1);
```

万事俱备，最后一步是重新启用第一端口。命令 0xAE 之后，控制器开始把键盘发来的扫描码放入输出缓冲区，并在 IRQ1 线上拉高电平通知 PIC。从这一刻起，如果 PIC 的 IRQ1 已经 unmask 且 `sti` 已执行，我们的 irq handler 就会被调用。

```cpp
    // Step 7: Reset internal state
    head_ = 0;
    tail_ = 0;
    shift_held_ = false;
    ctrl_held_  = false;
    alt_held_   = false;

    kprintf("[KBD] Keyboard driver initialised.\n");
}
```

初始化的最后一步是把驱动的内部状态清零——环形缓冲区的读写指针归零，三个 modifier 标志归 false。虽然静态变量在定义时已经初始化为 0 了，但显式重置一次是好习惯，尤其是在 QEMU 测试环境中 `init()` 可能被多次调用。

### interrupts.S —— IRQ1 中断桩绑定

控制器的硬件初始化就绪后，还需要在 IDT 中注册中断处理函数。回顾 tag 010/011 的内容，我们在 `interrupts.S` 中为每个 IRQ 都预留了桩函数，映射关系是 IRQ0-7 对应 IDT 向量 0x20-0x27。之前 IRQ1 一直绑定在默认的空操作 handler 上：

```asm
/* Master PIC IRQs */
ISR_NOERRCODE irq0_stub,  pit_irq0_handler     /* IRQ0(0x20): PIT Timer */
ISR_NOERRCODE irq1_stub,  irq_default_handler   /* IRQ1(0x21): Keyboard */
ISR_NOERRCODE irq2_stub,  irq_default_handler   /* IRQ2(0x22): Cascade */
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

为什么要这个 C 链接的桥接函数？因为 `ISR_NOERRCODE` 宏生成的汇编桩会直接 `call` 一个 C 符号名，而 C++ 的 name mangling 会把 `Keyboard::irq1_handler` 变成类似 `_ZN6cinux8drivers8Keyboard12irq1_handlerEPNS_4arch14InterruptFrameE` 这样的名字，汇编代码没法直接引用。`extern "C"` 保证了符号名就是 `keyboard_irq1_handler`，汇编桩可以无障碍地 call 它。桥接函数内部一行代码转发到 C++ 的命名空间成员函数，零开销。

### kernel_main —— 初始化集成与键盘轮询

最后我们来看 `kernel_main` 中如何把键盘驱动编织进启动序列。对比 tag 013 的版本，变化集中在初始化顺序的最后几步：

```cpp
#include "kernel/drivers/keyboard/keyboard.hpp"

using cinux::drivers::Keyboard;
using cinux::drivers::KeyEvent;
```

新增了键盘驱动的头文件引用和类型别名。

```cpp
    // Step 10: Initialise the PS/2 keyboard controller
    Keyboard::init();
```

键盘初始化放在 console 初始化之后、中断使能之前。这个顺序是有讲究的：`Keyboard::init()` 会发送一系列 I/O 端口命令给 8042 控制器，这些操作不需要中断支持（它们是同步的轮询式操作），但必须在 PIC unmask 和 `sti` 之前完成。如果先 `sti` 再初始化控制器，中间如果有按键事件到达，irq handler 还没准备好，ring buffer 还没清零，后果不可预测。

```cpp
    // Step 11: Unmask IRQ0 (PIT timer) and IRQ1 (Keyboard), enable interrupts
    PIC::unmask(0);
    PIC::unmask(1);
    cinux::lib::kprintf("[BIG] IRQ0+IRQ1 unmasked, enabling interrupts...\n");
    __asm__ volatile("sti");
    cinux::lib::kprintf("[BIG] Interrupts enabled. Keyboard echo active.\n");
```

相比 tag 013 只 unmask IRQ0，这里多了一行 `PIC::unmask(1)`。回顾 tag 011 的内容，8259 PIC 的 mask 寄存器每一位对应一个 IRQ 线——bit 0 对应 IRQ0，bit 1 对应 IRQ1。`PIC::unmask(1)` 清除 bit 1，允许 PIC 在 IRQ1 线上有信号时向 CPU 发送中断。

```cpp
    // Step 12: Keyboard poll loop -- echo keypresses to console + serial
    KeyEvent ev;
    while (1) {
        __asm__ volatile("hlt");

        // Drain all pending keyboard events
        while (Keyboard::poll(ev)) {
            if (ev.pressed && ev.ascii != 0) {
                console.putc(ev.ascii);
            }
        }
    }
```

这是内核的主循环，也是一个经典的事件驱动 idle loop 模式。`hlt` 指令让 CPU 进入低功耗停机状态，直到下一个中断唤醒它。唤醒后，内层的 `while (Keyboard::poll(ev))` 循环把 ring buffer 中积攒的所有键盘事件一次性排空，对每个可打印的按键事件调用 `console.putc()` 把字符显示到屏幕上。之所以要在一个 `hlt` 唤醒后排空所有事件，是因为按键事件可能在 CPU 醒来之前就积攒了好几个——比如一次按键会产生 make code 和 break code 两个中断，如果主循环每次只 poll 一个事件，第二个事件就要等到下一次 `hlt` 唤醒才能处理，延迟感会很明显。

顺便一提，`pit.cpp` 里原来那个每秒打印一次 `[TICK] uptime` 的输出被删掉了。这不是偷懒——而是因为键盘回显和每秒一次的 tick 打印混在一起会非常混乱，你敲一个 `a`，屏幕上可能出现 `[TICK] uptime: 3s`a` 这种交错输出，体验很差。把噪音源去掉是正确的选择。

### PIT 噪音消除

```cpp
// pit.cpp 中被删除的代码:
// Print uptime once per second (every freq_hz_ ticks)
if ((tick_count_ % freq_hz_) == 0) {
    uint64_t seconds = tick_count_ / freq_hz_;
    kprintf("[TICK] uptime: %us\n", static_cast<unsigned>(seconds));
}
```

这段代码在 tag 011 中是用来验证 PIT 中断正常工作的调试输出，到了 tag 014 它的使命已经完成。删除之后，PIT 的 irq0 handler 变成一个纯粹的 tick 计数器加 EOI，不再产生任何 I/O 副作用，这对于键盘回显的显示体验至关重要。

## 设计决策分析

**为什么用全静态类而不是全局函数？** 把 `init()`/`poll()`/`irq1_handler()` 挂在一个 `Keyboard` 类下面，比散落成 `keyboard_init()`、`keyboard_poll()` 这样的 C 风格函数更有组织性。全静态意味着不需要实例化，调用语法 `Keyboard::init()` 也足够清晰。如果将来需要支持多键盘（比如 USB 键盘热插拔），可以把静态成员改成实例成员，接口变化很小。

**为什么简化初始化序列？** OSDev 的 10 步完整序列包括 USB controller handoff、双通道检测、端口测试、设备复位等步骤。这些在 QEMU 中要么不需要（QEMU 没有 USB controller 和 PS/2 端口的竞争），要么行为不确定（设备复位在某些 QEMU 版本上可能卡住）。对于一个教学 OS 来说，先让它在 QEMU 中跑起来，真机适配是后续的事。

**为什么不 ACK 键盘命令？** 发送命令给键盘本身（不是控制器）时，键盘会回复 0xFA (ACK)。Cinux 没有向键盘发送任何命令——比如设置 LED 灯、切换扫描码集——所以不需要 ACK 处理。这省去了不少复杂性，但代价是无法控制键盘上的 LED。

## 扩展方向

- **CapsLock 支持**：当前只跟踪 Shift 的即时按下状态。CapsLock 是一个 toggle key，需要在收到 0x3A 的 make code 时翻转一个 `caps_lock_on` 标志，并在 ASCII 翻译时把它和 Shift 做 XOR。
- **E0 扩展键处理**：方向键、Page Up/Down、右 Ctrl/Alt 等扩展键的扫描码以 0xE0 开头。当前驱动检测到 0xE0 就直接跳过，意味着这些键全部被忽略。完整实现需要用一个状态机追踪 E0 前缀。
- **键盘 LED 控制**：通过向键盘发送命令 0xED + LED 位掩码可以控制 NumLock/CapsLock/ScrollLock 三个 LED。这需要实现 ACK 等待逻辑。
- **真机适配**：在 self-test (0xAA) 之后重新写回配置字节，处理 self-test 可能重置配置的情况。还需要加上 USB legacy 支持，因为很多现代主板的 PS/2 端口实际上是 USB controller 模拟出来的。
- **键盘命令队列**：如果未来需要支持 set scancode rate、enable/disable scanning 等键盘命令，需要实现一个命令队列来管理命令-ACK-响应的异步协议。

## 参考资料

- [OSDev Wiki: I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) — 8042 控制器命令列表、配置字节格式、10 步初始化序列
- [OSDev Wiki: PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard) — 扫描码集定义、键盘命令、驱动模型
- [OSDev Wiki: 8259 PIC](https://wiki.osdev.org/8259_PIC) — IRQ masking/unmasking、EOI 协议
- Intel SDM Vol.2A — IN/OUT 指令说明，端口 I/O 保护模式要求 (CPL <= IOPL)
- Intel SDM Vol.3A Section 6 — 中断/异常处理，IDT 门描述符，IRQ 到 IDT 向量的映射关系 (IRQn → vector 0x20+n)
- xv6 `kbd.c` — MIT 教学内核的键盘驱动实现，bitmask 式 modifier 追踪
