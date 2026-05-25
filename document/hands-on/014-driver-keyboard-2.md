---
title: 014-driver-keyboard-2 · 键盘驱动
---

# 014-2 扫描码解码：查找表与修饰键跟踪

## 导语

上一章我们跟 8042 PS/2 控制器完成了握手——初始化序列跑通了，自检过了，IRQ1 也 unmask 了。但到头来按键还是没反应，因为我们的 IRQ1 handler 还是个空壳。这一章开始，我们要构建完整的扫描码解码管线。

具体来说，这一章我们聚焦两件事：第一，建立从扫描码到 ASCII 字符的翻译机制——两张 128 项的查找表（小写和大写），按 US QWERTY 布局填写；第二，实现 Shift/Ctrl/Alt 三个 modifier 键的状态跟踪，让按键的翻译结果随 modifier 变化。IRQ1 handler 的完整实现和环形缓冲区是下一章的事，但这一章要把 handler 需要的所有数据结构和解码逻辑准备好。

知识前置：上一章的 PS/2 控制器初始化必须已经完成，你需要理解 scan code set 1 的基本格式（make code 是单字节，break code 是 make code | 0x80）。

## 概念精讲

### 扫描码集 1 的 Make/Break 规则

在上一章中我们开启了 8042 控制器的翻译模式，所以软件层面看到的始终是扫描码集 1。集 1 的规则极其简单，可以用两句话概括：按键按下（Make）时，控制器发送一个字节，bit 7 为 0；按键松开（Break）时，控制器发送同一个字节但 bit 7 置 1。举个例子，字母 A 的 make code 是 0x1E，那么 break code 就是 0x1E | 0x80 = 0x9E。左 Shift 的 make code 是 0x2A，break code 是 0xAA。

这个规则的推论也很直观：判断一个字节是按下还是松开，只需要检查 bit 7——为 0 就是按下，为 1 就是松开。提取 make code（键位标识），只需要和 0x7F 做 AND 运算把 bit 7 清掉。整个解码逻辑完全不需要状态机，一个 if-else 就搞定了。

不过有两类特殊情况我们需要知道但暂时不处理。第一类是扩展前缀 0xE0——方向键、右 Ctrl、右 Alt 等键会在真正的扫描码之前多发送一个 0xE0 字节。0xE0 本身也会触发一次 IRQ1。我们的策略是：如果读到的扫描码是 0xE0，直接发 EOI 然后返回，忽略这次中断。第二类是 Pause/Break 键，它会发送一个 6 字节的序列（0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5），而且没有 break code——这个更罕见，我们也不处理。

### 查找表驱动的设计

把扫描码翻译成 ASCII 字符，最直观的方式是一个 128 项的查找表——索引是 make code，值是对应的 ASCII 字符。我们准备两张表：一张是 `kScToLower`（无 Shift 时的映射，字母映射为小写，数字键映射为数字本身），另一张是 `kScToUpper`（有 Shift 时的映射，字母映射为大写，数字键映射为符号——比如 '1' 变成 '!'、'2' 变成 '@'）。

索引 0 表示"无映射"——功能键（F1-F10，扫描码 0x3B-0x44）、modifier 键（Shift、Ctrl、Alt）和其它非打印键在表中的值都是 0。这样解码的时候只需要检查表的返回值是否非零就能判断是不是可打印字符。两张表各 128 个字节，总共 256 字节，用 `static constexpr char` 数组定义，编译期完全确定，放到 `.rodata` 段里。

这种数据驱动的设计比 if-else 链或者 switch-case 好得多：表格是声明式的、可审计的（一眼就能看出映射是否正确），而且添加新的映射只需要改表不需要改逻辑。

### Modifier 跟踪机制

Shift、Ctrl、Alt 这三个 modifier 键比较特殊——它们自己不产生可打印字符，但会影响其他键的翻译结果。我们需要在每次按键事件到来时更新 modifier 状态，然后在翻译 ASCII 时根据当前状态查不同的表。

具体做法是维护三个布尔变量：`shift_held_`、`ctrl_held_`、`alt_held_`。每当收到一个 modifier 键的 make code（比如左 Shift 0x2A），把对应的标志设为 true；收到 break code（比如 0xAA），设为 false。注意左 Shift（0x2A）和右 Shift（0x36）都影响同一个 `shift_held_` 标志——这是因为对于字符大小写来说，左右 Shift 的效果是等价的。Ctrl 和 Alt 暂时只跟踪左键（0x1D 和 0x38），因为右 Ctrl/右 Alt 的扫描码带 0xE0 前缀，我们目前不处理扩展键。

每个 KeyEvent 结构体在被创建时，都会把当前的 modifier 状态快照进去。这意味着消费者（kernel_main 的轮询循环）拿到的每个事件都是自包含的——不需要自己去查"此刻 Shift 到底有没有被按下"。

### KeyEvent 结构体

每个按键事件被封装为一个 KeyEvent 结构体，包含六个字段：`ascii`（ASCII 字符，如果是非打印键或者松开事件则为 0）、`scancode`（原始扫描码字节，包含 bit 7 的 press/release 信息）、`pressed`（true=按下，false=松开）、`shift`、`ctrl`、`alt`（事件发生时的 modifier 快照）。这个结构体的设计让消费者拥有完整的信息——你可以只看 `ascii` 来处理字符输入，也可以看 `scancode` 来处理功能键，还可以看 modifier 组合来实现快捷键。

> 参考：[OSDev Wiki - PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard)

## 动手实现

### Step 1: 设计 KeyEvent 结构体和 Keyboard 类骨架

**目标**: 定义按键事件的数据结构和键盘驱动的类接口。

**设计思路**: KeyEvent 是一个纯数据结构（POD 类型），包含上面提到的六个字段。Keyboard 类的所有成员都是 static 的——因为整个系统只有一个 PS/2 键盘控制器，没有理由支持多实例。类的公开接口只有三个方法：`init()`（上一章已经实现）、`irq1_handler()`（IRQ1 中断处理，下一章实现）、`poll()`（从环形缓冲区取出一个事件，返回 bool 表示是否成功取到）。私有成员包括：64 个 KeyEvent 的数组（环形缓冲区）、head 和 tail 指针（uint32_t）、三个 modifier 布尔标志、以及一个 `enqueue()` 方法。

**实现约束**: 头文件 `kernel/drivers/keyboard/keyboard.hpp` 中，KeyEvent 放在 `cinux::drivers` 命名空间中。Keyboard 类也在同一命名空间。前向声明 `cinux::arch::InterruptFrame`（避免在头文件中 include idt.hpp）。所有 static 成员变量需要在 `.cpp` 文件中定义（不仅是声明），否则链接时找不到符号。这是 C++ static 成员变量的经典规则——头文件里只是声明，.cpp 里要写 `KeyEvent Keyboard::queue_[64] = {};` 这样的定义行。

**踩坑预警**: 如果你只在头文件里声明了 static 成员变量但没有在 .cpp 中定义，链接阶段会报 "undefined reference" 错误，而且错误信息指向的是你访问这些变量的每一行代码，非常烦人。另外，KeyEvent 结构体的字段顺序建议是：ascii 在前，然后是 scancode、pressed、三个 modifier——保持和声明顺序一致，避免结构体对齐填充导致不必要的内存浪费。

**验证**: 这一步只涉及类型定义，不需要独立验证。构建成功就说明结构体和类定义没有语法错误。

### Step 2: 实现扫描码查找表

**目标**: 创建两张 128 项的查找表，实现扫描码集 1 到 ASCII 的翻译。

**设计思路**: `kScToLower` 表按照 US QWERTY 布局填写。扫描码 0x02-0x0B 对应数字行的 1-9 和 0（注意不是 0-9，'1' 在 0x02，'0' 在 0x0B），0x10-0x1B 对应 QWERTYUIOP 这一行，0x1E-0x28 对应 ASDFGHJKL 这一行，0x2C-0x35 对应 ZXCVBNM 这一行。特殊键：0x01 是 Esc（映射为 ASCII 27），0x0E 是退格（映射为 '\b'），0x1C 是回车（映射为 '\n'），0x39 是空格。`kScToUpper` 表在字母位置映射大写字母，在数字行映射 shift 符号（'1'->'!'、'2'->'@'、'3'->'#'、'4'->'$'、'5'->'%'、'6'->'^'、'7'->'&'、'8'->'*'、'9'->'('、'0'->')'），在标点符号位置也是对应的 shift 版本（'-'->'_'、'='->'+'、'['->'{'，等等）。

**实现约束**: 两张表都是 `static constexpr char` 数组，128 个元素，定义在 `keyboard.cpp` 中。不可打印的键（功能键、modifier 键、0x00）全部填 0。表的排列建议每行 8 个元素，用注释标注扫描码范围，这样容易对照键盘布局检查。

**踩坑预警**: 填表的时候最容易出错的是数字行——扫描码 0x0A 是 '9'，0x0B 是 '0'，不是 0x09='9'/0x0A='0'。如果你把 9 和 0 的位置搞反了，按下 '9' 会输出 '0'，按下 '0' 什么都不输出（因为越界到了下一个条目）。另外，退格键（0x0E）映射为 '\b' 而不是 0x7F（DEL），这是 PS/2 键盘的标准行为。回车键（0x1C）映射为 '\n' 而不是 '\r'，这是我们自己的设计选择——Unix 传统。

**验证**: 构建后运行 host 端测试 `ctest --test-dir build -L keyboard`，扫描码翻译相关的测试用例应该全部通过。如果你想手动验证，可以检查 `kScToLower[0x1E]` 是否等于 'a'，`kScToUpper[0x1E]` 是否等于 'A'，`kScToLower[0x02]` 是否等于 '1'，`kScToUpper[0x02]` 是否等于 '!'。

### Step 3: 实现 modifier 跟踪逻辑

**目标**: 编写 modifier 键的状态更新代码，确保 Shift/Ctrl/Alt 的按下和释放都能正确反映到内部状态中。

**设计思路**: modifier 跟踪的核心逻辑非常简洁：收到一个扫描码后，先提取 make code（`sc & 0x7F`），然后检查这个 make code 是否属于 modifier 键。如果是左 Shift（0x2A）或右 Shift（0x36），就把 `shift_held_` 设为 `pressed` 的值（按下=true，释放=false）。如果是左 Ctrl（0x1D），更新 `ctrl_held_`。如果是左 Alt（0x38），更新 `alt_held_`。

注意这里用的是 `pressed` 而不是硬编码的 `true`——这意味着 break code 自然会把标志清零。如果只写 `shift_held_ = true`，那么 Shift 按下后永远不会"松手"，后续所有按键都会被当成大写来翻译。另外，modifier 跟踪必须在构建 KeyEvent 之前完成——因为 KeyEvent 中的 shift/ctrl/alt 字段是从当前 modifier 状态快照而来的。

**实现约束**: 这段逻辑将作为 `irq1_handler()` 的一部分实现，放在 `keyboard.cpp` 中。但你可以先写一个独立函数来验证：接收扫描码和三个 bool 引用参数，执行 modifier 更新。等下一章集成到 irq1_handler 时再内联进去。

**踩坑预警**: 如果 modifier 跟踪只检查 make code 但不检查 pressed 状态（比如只写 `if (make_code == LSHIFT) shift_held_ = true;`，缺少 break code 的 `= false` 分支），Shift 键就会"粘住"——按下 Shift 后，即使松手了，后续所有按键仍然是大写。这个 bug 非常隐蔽，因为按 Shift 本身看起来很正常，只有松开之后才发现不对劲。

**验证**: 在 host 端测试中，modifier 追踪相关的测试用例应该覆盖这些场景：LShift 按下设置 shift=true、LShift 释放设置 shift=false、RShift 按下也设置 shift=true、Ctrl 和 Alt 的完整生命周期。如果测试通过，说明 modifier 状态机的 press/release 逻辑是正确的。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 扫描码集 1 | Make: bit7=0, Break: bit7=1, make_code = sc & 0x7F |
| 查找表翻译 | kScToLower[128] 和 kScToUpper[128]，索引 0 = 无映射 |
| US QWERTY 布局 | 0x02-0x0B 数字行，0x10-0x1B QWERTY 行，0x1E-0x28 ASDF 行 |
| Modifier 跟踪 | 三个布尔变量，每次 modifier 键 press/release 时更新 |
| KeyEvent | ascii + scancode + pressed + shift/ctrl/alt 快照 |
| 数据驱动设计 | 查找表比 switch/case 更容易维护、审计和扩展 |
