---
title: 014-driver-keyboard-3 · 键盘驱动
---

# 014-3 事件消费与验证：环形缓冲区、键盘回显与双轨测试

> 标签：ring buffer, poll loop, console, unit test, QEMU integration test, 0xD2 inject
> 前置：[014-2 扫描码解码与修饰键追踪](014-driver-keyboard-2.md)

## 前言

前两篇我们完成了 8042 控制器的硬件初始化、扫描码解码逻辑、modifier 状态追踪和 IRQ1 handler 的完整实现。handler 已经能把每一个扫描码翻译成 `KeyEvent` 结构体了，但它现在还缺一个"下游"——事件放入了哪里？谁来取走？取走之后怎么显示？这篇就要打通这最后一公里。

我们从环形缓冲区的 enqueue/poll 实现开始，然后看 kernel_main 的 poll 循环如何消费事件并回显到屏幕，最后精讲两套测试代码——host 端的纯逻辑单元测试和 QEMU 内核内的集成测试。完成这篇之后，在 QEMU 窗口中敲键盘就能看到字符回显到屏幕上——Cinux 第一次真正"听到"了外部输入。

## 环形缓冲区：ISR 和主循环之间的弹性缓冲

环形缓冲区是 ISR 和主循环之间的解耦层。ISR 在中断上下文中运行，不能阻塞，不能等主循环消费完再返回；主循环在普通上下文中运行，不知道中断什么时候来。ring buffer 就是两者之间的弹性缓冲——ISR 写入端不阻塞，主循环读取端按自己的节奏消费。

```cpp
void Keyboard::enqueue(const KeyEvent& ev) {
    uint32_t next = (tail_ + 1) % KEY_QUEUE_SIZE;

    // Drop the event if the buffer is full
    if (next == head_) {
        return;
    }

    queue_[tail_] = ev;
    tail_ = next;
}
```

`enqueue()` 是 producer 端的操作。它先计算 tail 的下一个位置 `next`，然后检查 `next == head_`——如果相等，说明缓冲区已满（tail 追上了 head），直接丢弃事件并返回。这是"满则丢"策略——在内核开发中，丢一个按键事件远比让 ISR 卡住要好。如果不满，把事件写入 `queue_[tail_]`，然后推进 tail。`KEY_QUEUE_SIZE` 是 64，所以模运算等价于 `& 63`（编译器会自动做这个优化），但 `% KEY_QUEUE_SIZE` 的写法更清晰地表达了意图。

```cpp
bool Keyboard::poll(KeyEvent& out) {
    // Buffer is empty when head equals tail
    if (head_ == tail_) {
        return false;
    }

    // Copy the event at head and advance
    out = queue_[head_];
    head_ = (head_ + 1) % KEY_QUEUE_SIZE;
    return true;
}
```

`poll()` 是 consumer 端的操作。如果 `head_ == tail_`，缓冲区为空，返回 false。否则，把 `queue_[head_]` 拷贝到 `out`，推进 head，返回 true。调用者通常在一个 `while (poll(ev))` 循环中排空所有事件，就像 `kernel_main` 中那样。

环形缓冲区的经典问题是"满"和"空"的判定——两种状态下 head 都可能等于 tail。解决方案是浪费一个槽位：满的条件是 `(tail + 1) % size == head`（tail 的下一个位置是 head），空的条件是 `head == tail`。这样 64 个槽位实际可用 63 个，换来的是无需额外计数器的简洁判定逻辑。

这个 ring buffer 不是线程安全的——但在这里它不需要是。唯一的 producer（irq1_handler）运行在中断上下文中，唯一的 consumer（kernel_main 的 poll loop）运行在普通上下文中，而且 x86 的单原子写入（32 位对齐的 head_/tail_ 赋值）保证了不会出现撕裂读（torn read）。在真正的多核环境中，这里需要加上 memory barrier 或者改用 lock-free ring buffer，但 Cinux 目前是单核运行的。

与 Cinux 的设计方案对比，xv6 完全没有 ring buffer——它的 `kbdgetc()` 直接返回一个字符（或 0 表示无数据），由上层的 `consoleintr()` 在 `INPUT_BUF` 中缓冲。ToaruOS 的做法更有趣：它用内核 pipe（`/dev/kbd`）来传递原始扫描码字节，不做任何内核态解码，全部推到用户态的 `libtoaru_kbd` 库里处理。这种设计非常灵活——用户态可以自由选择解码策略——但代价是每次按键都需要一次上下文切换（从内核态 write 到 pipe，再到用户态 read）。Linux 的 serio 子系统使用了更复杂的 kfifo 队列，配合 RCU 读侧无锁来支持多核并发，远超我们当前的需求。

## 主循环：消费键盘事件

在第一篇中我们已经看过 `kernel_main` 的完整代码，这里聚焦 poll 循环的逻辑：

```cpp
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

这个循环的执行模型是这样的：`hlt` 让 CPU 休眠，等待下一个中断（可能是 PIT 的 IRQ0，也可能是键盘的 IRQ1）。醒来后，检查 ring buffer 中有没有积攒的键盘事件。`while (Keyboard::poll(ev))` 会一直 poll 到缓冲区为空，对每个事件检查 `ev.pressed && ev.ascii != 0`——只有"按下"且"有 ASCII 映射"的事件才会被 `console.putc()` 显示。

这意味着 modifier 键的按下/释放不会产生任何可见输出（因为它们的 `ascii` 为 0），功能键也不会（同样 `ascii` 为 0），按键释放也不会（`pressed` 为 false）。只有真正的可打印字符按键——字母、数字、标点、空格、回车、退格——才会回显到屏幕上。对于 Cinux 当前的能力来说，这个过滤逻辑刚刚好。

## 双轨测试：host 单元测试 + QEMU 集成测试

键盘驱动有两套测试：一套是 host 端的纯逻辑单元测试（`test/unit/test_keyboard.cpp`），一套是 QEMU 内核内的集成测试（`kernel/test/test_keyboard.cpp`）。这种双轨测试策略我们在前几个 tag 中已经建立起来了——host 端测试验证数据变换逻辑的纯函数正确性，不需要跑在 QEMU 里，速度极快；in-kernel 测试验证驱动和真实硬件（QEMU 模拟的 8042）的交互，覆盖 host 端无法模拟的 I/O 端口操作。

### Host 端单元测试

这个文件有 714 行，是所有测试文件中最长的之一。它的策略是把内核驱动的纯数据逻辑复制到 host 端，在不依赖任何硬件的情况下测试。文件开头把 `kScToLower`、`kScToUpper`、`ScanCode` 常量、`KeyEvent` 结构体、ring buffer 逻辑、modifier 状态机全部复制了一份，然后围绕它们写了大量的测试用例。

我们挑几个有代表性的来看。

扫描码到 ASCII 的基本翻译测试，验证表中的关键条目没有填错：

```cpp
TEST("keyboard: scancode 0x1E -> 'a' (lowercase)") {
    ASSERT_EQ(kScToLower[0x1E], 'a');
}

TEST("keyboard: scancode 0x02 -> '1' unshifted, '!' shifted") {
    ASSERT_EQ(kScToLower[0x02], '1');
    ASSERT_EQ(kScToUpper[0x02], '!');
}
```

你可能会觉得"这不就是检查常量数组吗，有什么意义？"但现实是，手动维护一个 128 条目的查找表非常容易出错——把 `'q'` 和 `'w'` 的位置写反这种低级 bug 真的会发生，而且一旦发生，驱动会正常工作，只是按键和字符的对应关系全部错乱，调试起来非常痛苦。这些测试是防止此类低级错误的最后一道防线。

完整的 Shift+A 按键序列测试，模拟一个真实的打字场景：

```cpp
TEST("keyboard: full Shift+A sequence decodes correctly") {
    modifier_reset();
    ring_reset();

    KeyEvent ev{};

    // Press LShift
    decode_scancode(ScanCode::LSHIFT, ev);
    ring_enqueue(ev);

    // Press 'a' key
    decode_scancode(0x1E, ev);
    ring_enqueue(ev);

    // Release 'a' key
    decode_scancode(0x9E, ev);
    ring_enqueue(ev);

    // Release LShift
    decode_scancode(0xAA, ev);
    ring_enqueue(ev);

    // Dequeue and verify all four events...
    // (details omitted for brevity)
}
```

这个测试非常有价值，因为它端到端地模拟了一个完整的按键序列——按下 Shift、按下 A、释放 A、释放 Shift——四个事件依次入队，然后依次出队验证。注意 A 释放时 `shift` 仍然是 true，因为 A 释放时 Shift 还没松手，这和我们讲解 `irq1_handler()` 时分析的 modifier 快照语义完全一致。

环形缓冲区边界测试，验证满缓冲区的丢事件行为：

```cpp
TEST("keyboard: ring buffer full drops event") {
    ring_reset();
    uint32_t capacity = KEY_QUEUE_SIZE - 1;

    KeyEvent dummy{};
    dummy.ascii = 'D';
    for (uint32_t i = 0; i < capacity; i++) {
        ring_enqueue(dummy);
    }

    // Buffer is now full.  The next enqueue should be dropped.
    KeyEvent overflow{};
    overflow.ascii = 'Z';
    ring_enqueue(overflow);

    // Drain all events -- there should be exactly `capacity` events
    uint32_t count = 0;
    KeyEvent out{};
    while (ring_poll(out)) {
        count++;
    }
    ASSERT_EQ(count, capacity);
}
```

先把缓冲区填满到理论容量（63 个事件），然后尝试再塞一个，验证第 64 个被默默丢弃，最终 drain 出来的是 63 个而不是 64 个。`capacity = KEY_QUEUE_SIZE - 1` 这个常量在测试中出现了两次，它是环形缓冲区经典设计——浪费一个槽位区分满和空——的直接体现。

### QEMU in-kernel 集成测试

In-kernel 测试面临一个独特的挑战：怎么在 QEMU 中模拟按键？我们不能让测试程序等待用户物理按键，那样就不是自动化测试了。解决方案是利用 QEMU 8042 模拟器的一个特性——命令 0xD2（"Write to first PS/2 port output buffer"），它可以把一个字节直接塞进控制器的输出缓冲区，然后下一次 `io_inb(0x60)` 就会读到它。OSDev Wiki 的 [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) 页面列出了这个命令，它是专门为测试目的设计的。

```cpp
void inject_scancode(uint8_t sc) {
    // Wait for input buffer to be clear
    uint32_t timeout = 100'000;
    while ((io_inb(0x64) & 0x02) != 0) {
        if (--timeout == 0) return;
        __asm__ volatile("pause");
    }

    // Command: put next byte into first PS/2 port output buffer
    io_outb(0x64, 0xD2);

    // Wait for input buffer to be clear again
    timeout = 100'000;
    while ((io_inb(0x64) & 0x02) != 0) {
        if (--timeout == 0) return;
        __asm__ volatile("pause");
    }

    // Write the scan code byte
    io_outb(0x60, sc);
}
```

`inject_scancode()` 的三步操作是：先等输入缓冲区空，发送 0xD2 命令告诉控制器"下一个写到 0x60 的字节请放入输出缓冲区"，再等输入缓冲区空，最后把扫描码字节写到 0x60。控制器收到后会把扫描码放入输出缓冲区（就像键盘真的发了这个字节一样），同时设置 OUTPUT_FULL 状态位。

有了这个注入机制，测试就可以完全自动化地驱动键盘解码逻辑。测试覆盖了以下场景：`init()` 不崩溃、空缓冲区 poll 返回 false、扫描码 0x1E 解码为 'a'、break code 0x9E 产生 `pressed=false`、Shift modifier 追踪、多个事件的 FIFO 顺序、Ctrl/Alt modifier 追踪、功能键无 ASCII 映射、Enter 键产生 `\n`、数字键的 shift 行为。

### 为什么 host 测试要复制内核代码？

你可能会问：为什么不直接链接内核驱动的代码？答案是键盘驱动依赖 `io_inb`/`io_outb`（x86 inline assembly）和 `PIC::send_eoi`（端口 I/O），这些在 host 端（x86_64 Linux 用户空间）无法执行。复制纯数据逻辑（查找表、modifier 状态机、ring buffer 算术）到 host 端测试，可以以毫秒级的速度验证核心逻辑的正确性，而不需要每次都启动 QEMU。

代价是两份代码需要手动保持同步——如果内核驱动修改了查找表但忘了同步到测试文件，测试通过并不意味着驱动正确。这是已知的技术债务，可以在后续引入共享头文件来缓解——把查找表、`KeyEvent` 结构体、`ScanCode` 常量提取到一个共享的头文件中，host 测试和内核驱动都引用同一个头文件，从根源上消除两份代码不同步的风险。

## 收尾

到这里，Cinux 的键盘驱动从硬件初始化到事件处理已经完全打通了。回顾一下我们做了什么：第一篇初始化了 8042 控制器、绑定了 IRQ1 中断桩、unmask 了 PIC；第二篇实现了扫描码解码、modifier 追踪、ASCII 翻译、IRQ1 handler；第三篇实现了环形缓冲区、主循环 poll、以及双轨测试。现在启动 QEMU，敲键盘就能看到字符回显到屏幕上——一个真正能交互的内核。

当然，这还只是一个起点。E0 扩展键、CapsLock 切换、LED 控制、按键重复这些功能都没有实现。但核心框架已经搭好了——KeyEvent 结构体提供了完整的事件信息，ring buffer 提供了 producer-consumer 解耦，查找表的设计让键盘布局切换变成换一张表的事。后续扩展只是在这个框架上添砖加瓦。

## 参考资料

- OSDev Wiki: [PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard) -- Scan code set 1/2/3 完整表格、make/break 规则、E0 扩展键
- OSDev Wiki: [I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) -- 命令 0xD2 (Write to first PS/2 port output buffer) 的 QEMU 测试注入技巧
- OSDev Wiki: [8259 PIC](https://wiki.osdev.org/8259_PIC) -- EOI 时序：必须在读取数据之后发送 EOI
- xv6 `kbd.c` -- bitmask 式 modifier 追踪、三表映射方案（normalmap/shiftmap/ctlmap），约 100 行
- Linux `drivers/input/keyboard/atkbd.c` -- serio 总线上的 PS/2 键盘驱动，`atkbd_set2_keycode[512]` 表驱动方案，默认使用 set 2
- ToaruOS `kernel/arch/x86_64/ps2hid.c` -- pipe-based 键盘/鼠标合并驱动，raw scan code 写入内核管道交由用户态解码
- SerenityOS `Kernel/Bus/SerialIO/Controller.h` -- `SerialIOController` 基类 + typed device commands，类型安全的 serio 抽象
