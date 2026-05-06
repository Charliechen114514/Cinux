# 014-2 通读：扫描码解码——查找表、修饰键与 IRQ1 处理管线

## 概览

本文是 tag `014_driver_keyboard` 三篇通读教程的第二篇，聚焦于从 IRQ1 中断触发到 KeyEvent 入队这段解码管线。上一篇中我们搭好了硬件层面的一切：控制器初始化完成、IRQ1 桩函数注册到位、PIC unmask、`sti` 已执行。现在当你在键盘上敲下一个键，CPU 就会被中断从 `hlt` 唤醒，跳转到 `keyboard_irq1_handler`。接下来的问题是：我们怎么把端口 0x60 上那个原始字节变成有意义的结构化事件？这就是本篇要回答的问题。

我们从扫描码集 1 的编码规则开始，然后讲解两张查找表的设计，接着是 modifier 键的状态追踪机制，最后分析 `irq1_handler()` 的完整解码流程。第三篇将覆盖环形缓冲区、主循环消费逻辑和双轨测试体系。

## 数据流图

```
    物理按键 (用户按下 'A' 键)
         |
         v
    +--------------------------+
    |  键盘硬件                |
    |  发送 scan code set 2    |
    |  的 make code            |
    +----------+---------------+
               | (serial line)
               v
    +--------------------------+
    |  8042 控制器             |
    |  Translation=ON         |
    |  set 2 -> set 1 转换     |
    |  输出: 0x1E              |
    |  触发 IRQ1               |
    +----------+---------------+
               | (interrupt)
               v
    +------------------------------------------+
    |  irq1_stub (interrupts.S)               |
    |    -> keyboard_irq1_handler (C bridge)   |
    |      -> Keyboard::irq1_handler()         |
    |                                          |
    |  1. io_inb(0x60) -> sc = 0x1E            |
    |  2. pressed = (sc & 0x80)==0 -> true     |
    |  3. make_code = sc & 0x7F -> 0x1E        |
    |  4. 更新 modifier 状态                   |
    |     (如果 sc 是 Shift/Ctrl/Alt)          |
    |  5. ASCII 翻译:                          |
    |     shift_held_?                         |
    |       kScToUpper[0x1E] -> 'A'             |
    |       kScToLower[0x1E] -> 'a'             |
    |  6. 构建 KeyEvent {                      |
    |       ascii='A', scancode=0x1E,          |
    |       pressed=true, shift=true, ...      |
    |     }                                    |
    |  7. enqueue(ev) -> ring buffer            |
    |  8. PIC::send_eoi(1)                     |
    +----------+-------------------------------+
```

## 代码精讲

### 扫描码集 1 的规则

在看代码之前，我们需要先理解 scan code set 1 的编码规则，因为整个解码逻辑都建立在这些规则之上。

PS/2 键盘原生使用的是 scan code set 2，但我们在第一篇中启用了 8042 控制器的 translation mode，控制器会自动把 set 2 转换成 set 1 再交给软件。Set 1 的编码规则比 set 2 简洁得多，这也是为什么我们选择 set 1 的原因——驱动代码可以写得更简单。

Set 1 的核心规则只有三条。第一，make code（按下）是一个单字节，bit 7 为 0，范围在 0x00-0x7F。比如字母 'A' 的 make code 是 0x1E，左 Shift 的 make code 是 0x2A。第二，break code（释放）是把对应 make code 的 bit 7 置 1，即 `make_code | 0x80`。所以 'A' 的 break code 是 0x9E，左 Shift 的 break code 是 0xAA。这个规则非常统一——给定任何一个扫描码，用 `sc & 0x80` 判断是按下还是释放，用 `sc & 0x7F` 提取 make code，两个位运算就能完成。第三，扩展键（方向键、Page Up/Down、右 Ctrl/Alt 等）在真实扫描码之前会多发送一个 0xE0 前缀字节。这个前缀本身也会触发一次中断，所以处理 E0 键需要用状态机来追踪。Cinux 当前对 E0 的处理是直接跳过——检测到 0xE0 就发 EOI 返回，不产生事件。

还有一个特殊情况是 Pause/Break 键，它发送一个 6 字节的序列 `0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5`，并且没有 break code。这个键在 Cinux 中完全不被处理——在我们的简化策略下这不算什么问题，毕竟 Pause 键在日常使用中极少被用到。

### 查找表：kScToLower 与 kScToUpper

了解了扫描码规则之后，最自然的"扫描码 -> ASCII"翻译方式就是一张查找表。Cinux 使用了两张并行的 `constexpr char` 数组：

```cpp
static constexpr uint32_t SCAN_TABLE_SIZE    = 128;

static constexpr char kScToLower[SCAN_TABLE_SIZE] = {
    0,   27,  '1', '2', '3', '4', '5', '6',  // 0x00-0x07
    '7', '8', '9', '0', '-', '=', '\b', 0,    // 0x08-0x0F
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',  // 0x10-0x17
    'o', 'p', '[', ']', '\n', 0,   'a', 's',  // 0x18-0x1F
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',  // 0x20-0x27
    '\'', '`', 0,   '\\', 'z', 'x', 'c', 'v', // 0x28-0x2F
    'b', 'n', 'm', ',', '.', '/', 0,   '*',   // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,    // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',  // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',  // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,    // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x58-0x5F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x60-0x67
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x78-0x7F
};
```

这张表有 128 个条目，以 make code 为索引。我们来逐段看它的布局，你会发现它其实是一张 US QWERTY 键盘的物理映射。

索引 0x00 是空的（不存在 make code 0），0x01 是 Esc (ASCII 27)，0x02-0x0B 是数字行 `1-9, 0`，0x0C 是 `-`，0x0D 是 `=`，0x0E 是 Backspace (`\b`)，0x0F 是 Tab（表中为 0，Cinux 不映射 Tab 的 ASCII）。然后 0x10-0x1B 是字母行 `q, w, e, r, t, y, u, i, o, p, [, ], \n`（Enter），0x1D 是左 Ctrl（表中为 0），0x1E-0x28 是 `a, s, d, f, g, h, j, k, l, ;, '`，0x29 是 `` ` ``，0x2A 是左 Shift（表中为 0），0x2B 是 `\`，0x2C-0x35 是 `z, x, c, v, b, n, m, ,, ., /`，0x36 是右 Shift（表中为 0），0x37 是 `*`（数字小键盘），0x38 是左 Alt（表中为 0），0x39 是空格。

后半段 0x40-0x57 是数字小键盘区域（当 NumLock 开启时的数字和运算符映射），0x58-0x7F 大部分为 0（对应 F 键和各种特殊键）。

可以看到，所有 modifier 键（左/右 Shift、左 Ctrl、左 Alt、CapsLock）在表中的值都是 0，因为它们不是可打印字符，不需要 ASCII 翻译。但它们在物理键盘上有自己的 make code，所以 irq handler 会通过专门的逻辑来追踪它们的状态。

```cpp
static constexpr char kScToUpper[SCAN_TABLE_SIZE] = {
    0,   27,  '!', '@', '#', '$', '%', '^',  // 0x00-0x07
    '&', '*', '(', ')', '_', '+', '\b', 0,   // 0x08-0x0F
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', // 0x10-0x17
    'O', 'P', '{', '}', '\n', 0,   'A', 'S', // 0x18-0x1F
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', // 0x20-0x27
    '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',  // 0x28-0x2F
    'B', 'N', 'M', '<', '>', '?', 0,   '*',  // 0x30-0x37
    0,   ' ', 0,   0,   0,   0,   0,   0,    // 0x38-0x3F
    0,   0,   0,   0,   0,   0,   0,   '7',  // 0x40-0x47
    '8', '9', '-', '4', '5', '6', '+', '1',  // 0x48-0x4F
    '2', '3', '0', '.', 0,   0,   0,   0,    // 0x50-0x57
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x58-0x5F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x60-0x67
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x68-0x6F
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x70-0x77
    0,   0,   0,   0,   0,   0,   0,   0,    // 0x78-0x7F
};
```

`kScToUpper` 是 `kScToLower` 的"Shift 按下"版本。数字行变成了符号（`1->!`, `2->@`, `3->#` 等），字母全部大写，标点也变成了对应的上档字符（`;->:`, ''->"`", `` `->~ ``, `\->|`, `,-><`, `.->>`, `/->?`）。两张表的结构完全对称——同样的索引，不同的映射值——这使得翻译逻辑只需一行三元表达式。

`constexpr` 修饰意味着这两张表在编译期就被填充好了，不占用运行时初始化时间，编译器甚至可能把它们放到 `.rodata` 段。`static` 限定它们只在 `keyboard.cpp` 内可见，不会污染全局符号表。

### modifier 追踪

modifier 键（Shift、Ctrl、Alt）的追踪逻辑分散在三个地方：静态成员变量声明、`init()` 中的重置、以及 `irq1_handler()` 中的更新。我们先看状态变量：

```cpp
bool Keyboard::shift_held_ = false;
bool Keyboard::ctrl_held_  = false;
bool Keyboard::alt_held_   = false;
```

三个布尔变量，简洁到几乎不用解释。`shift_held_` 同时覆盖左 Shift (0x2A) 和右 Shift (0x36)，不区分左右——对于大多数场景来说这已经足够了。`ctrl_held_` 只追踪左 Ctrl (0x1D)，因为右 Ctrl 是一个 E0 扩展键（0xE0 0x1D），当前驱动不处理 E0 前缀。`alt_held_` 同理，只追踪左 Alt (0x38)。

modifier 状态的更新发生在 `irq1_handler()` 内部，每收到一个扫描码都会检查它是否是 modifier 键的 make/break code：

```cpp
    // Track modifier keys regardless of press/release
    if (make_code == ScanCode::LSHIFT || make_code == ScanCode::RSHIFT) {
        shift_held_ = pressed;
    }

    if (make_code == ScanCode::LCTRL) {
        ctrl_held_ = pressed;
    }

    if (make_code == ScanCode::LALT) {
        alt_held_ = pressed;
    }
```

这段代码的逻辑是：如果提取出的 make_code 匹配某个 modifier 键，就把对应的 held 标志设为 `pressed` 的值（按下设 true，释放设 false）。注意这里用的是 `pressed` 而不是硬编码的 true，所以 break code 自然会把标志清零。对于 Shift，`||` 操作符让左 Shift 和右 Shift 都能影响同一个 `shift_held_` 标志——按下左 Shift 后 `shift_held_` 变 true，再释放左 Shift 变 false，这和用户的直觉一致。

值得一提的是，modifier 状态更新在构建 `KeyEvent` 之前完成。这意味着当 Shift 被按下时，Shift 自身的 `KeyEvent` 中 `shift` 字段就已经是 true 了（因为 `shift_held_` 刚刚被设为 true）。这是一个微妙但正确的行为——按下 Shift 的事件确实应该报告 shift 为 true，因为 Shift 已经被按下了。

### Keyboard::irq1_handler() -- 完整解码流程

这是整个驱动的核心函数，每当 IRQ1 到来时执行。它把上一节讲述的所有碎片逻辑串成一个完整的处理管线：

```cpp
void Keyboard::irq1_handler(cinux::arch::InterruptFrame* /*frame*/) {
    // Read the scan code from the PS/2 data port
    uint8_t sc = io_inb(Ps2Port::DATA);
```

第一步是从端口 0x60 读取扫描码。这一步必须在 EOI 之前完成——如果先发 EOI 再读数据，PIC 可能会在你读完之前就允许下一个 IRQ1 中断进来，导致前一个扫描码还没处理就被新的覆盖。8042 控制器的输出缓冲区只有一个字节深度（在标准配置下），所以读慢了真的会丢数据。

```cpp
    // Handle extended scan code prefix (0xE0) -- skip for now
    if (sc == ScanCode::EXTENDED) {
        PIC::send_eoi(1);
        return;
    }
```

扩展键前缀 0xE0 被检测到后直接跳过。这意味着所有扩展键（方向键、Page Up/Down、Insert/Delete、右 Ctrl/Alt 等）产生的两个中断中，第一个（E0 前缀）被丢弃，第二个（真实扫描码）也会被正常处理但可能产生一个错误的 ASCII 翻译——比如方向键"上"的序列是 0xE0, 0x48，E0 被跳过，0x48 被 `kScToLower` 翻译成数字小键盘的 '8'。不过在当前的简化策略下，这个"错误翻译"并不致命，因为 kernel_main 只在 `ev.pressed && ev.ascii != 0` 时才 putc，而且用户通常不会在没有方向键处理能力的情况下频繁按方向键。

这里有一个容易忽略的细节：0xE0 被跳过时也要发 EOI。如果不发，PIC 会认为 IRQ1 还在服务中，后续所有 IRQ1（包括 0xE0 后面那个真实扫描码触发的中断）都会被 PIC 阻塞，键盘就彻底不响应了。

```cpp
    // Determine if this is a make (press) or break (release) code
    bool pressed = (sc & 0x80) == 0;
    uint8_t make_code = sc & 0x7F;
```

两个位运算完成 make/break 判定和 make code 提取。`sc & 0x80` 检查 bit 7，为 0 表示按下，为 1 表示释放。`sc & 0x7F` 清除 bit 7 得到纯净的 make code。这一步对所有扫描码都适用——包括 modifier 键、功能键、可打印键——因为 set 1 的 break code 编码规则是统一的。

```cpp
    // Build the event
    KeyEvent ev{};
    ev.scancode = sc;
    ev.pressed  = pressed;
    ev.shift    = shift_held_;
    ev.ctrl     = ctrl_held_;
    ev.alt      = alt_held_;
    ev.ascii    = 0;
```

构建 `KeyEvent` 的默认状态。`{}` 值初始化先把所有字段清零（包括 padding 字节），然后显式赋值。`scancode` 保存原始扫描码（含 bit 7），`pressed`/`shift`/`ctrl`/`alt` 都是布尔快照，`ascii` 暂时为 0，等待下一步翻译。

这里有一个容易忽略的设计选择：`ev.shift` 等于 `shift_held_` 的当前值，而 `shift_held_` 刚刚可能被上面的 modifier 更新逻辑修改过。所以当 Shift 按下时，Shift 自身事件的 `shift` 字段是 true——因为"Shift 正在被按下"这个陈述在那个时刻是成立的。同理，当 Shift 释放时，释放事件的 `shift` 字段是 false，因为 `shift_held_` 刚被设为 false。

```cpp
    // Translate to ASCII only on key press and if the make code is in range
    if (pressed && make_code < SCAN_TABLE_SIZE) {
        ev.ascii = shift_held_ ? kScToUpper[make_code]
                               : kScToLower[make_code];
    }

    // Enqueue the event
    enqueue(ev);

    // Signal End-Of-Interrupt for IRQ1
    PIC::send_eoi(1);
}
```

ASCII 翻译只在两个条件同时满足时进行：`pressed` 为 true（按键事件，不是释放），且 `make_code < 128`（在查找表范围内）。翻译方式是一个简单的三元表达式——如果 Shift 被按下，用 `kScToUpper` 表，否则用 `kScToLower` 表。如果 make_code 对应的表项是 0（比如功能键、modifier 键、未映射的扫描码），`ev.ascii` 保持为 0。最后两步是入队和 EOI。入队操作把构建好的 `KeyEvent` 放入环形缓冲区。EOI 通知 PIC "IRQ1 处理完毕，可以接受新的中断了"。这两步的顺序是严格的——先入队再 EOI——因为入队是 O(1) 操作，几乎不会失败，而 EOI 必须在 handler 结束时发送，否则 PIC 会阻塞后续中断。

## 参考资料

- [OSDev Wiki: PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard) -- Scan code set 1/2/3 完整表格、make/break 规则、E0 扩展键、驱动模型
- [OSDev Wiki: I8042 PS/2 Controller](https://wiki.osdev.org/I8042_PS/2_Controller) -- 命令 0xD2 (Write to first PS/2 port output buffer) 的 QEMU 测试注入技巧
- [OSDev Wiki: 8259 PIC](https://wiki.osdev.org/8259_PIC) -- EOI 时序：必须在读取数据之后发送 EOI
