# 014-2 从按键到字符：扫描码解码与事件队列

## 导语

上一章我们跟 8042 PS/2 控制器完成了握手——初始化序列跑通了，自检过了，IRQ1 也 unmask 了。但到头来按键还是没反应，因为我们的 IRQ1 handler 还是个空壳。这一章要把整条数据通路打通：从硬件中断触发开始，到扫描码解码、modifier 状态跟踪、ASCII 翻译，最终通过一个环形缓冲区把按键事件送到 kernel_main 的轮询循环里，回显到屏幕上。完成这一章后，你在 QEMU 窗口里敲键盘，屏幕上就会出现对应的字符——这是内核第一次真正"响应"外部输入。

知识前置：上一章的 PS/2 控制器初始化必须已经完成，IRQ1 已 unmask，IDT 中已绑定 `keyboard_irq1_handler`。你需要理解 scan code set 1 的基本格式（make code 是单字节，break code 是 make code | 0x80），以及环形缓冲区的基本原理。

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

### 环形缓冲区（Ring Buffer）

中断处理函数和主循环运行在不同的"上下文"中——一个在 IRQ1 的中断栈上，一个在内核主栈上。我们需要一个机制让这两边安全地传递数据，这个机制就是环形缓冲区。

环形缓冲区的原理是：一个固定大小的数组（我们用 64 个 KeyEvent 槽位），加上一个 head 指针（读位置）和一个 tail 指针（写位置）。enqueue 操作在 tail 位置写入数据，然后 tail 前进一步（对 64 取模实现回绕）。poll 操作从 head 位置读取数据，然后 head 前进一步。当 head == tail 时，缓冲区为空；当 tail 的下一个位置是 head 时，缓冲区满了——此时 enqueue 选择丢弃新事件而不是覆盖旧数据。

这意味着实际容量是 63 个事件（64 - 1，有一个槽位被"浪费"来区分满和空）。对于正常打字速度来说 63 个事件绑绑有余，但如果主循环被阻塞了太久（比如在做一个很长的计算），快速连续打字可能会丢事件。这是一个合理的权衡——比起阻塞中断（会导致丢失更多键），丢几个按键是可以接受的。

### KeyEvent 结构体

每个按键事件被封装为一个 KeyEvent 结构体，包含六个字段：`ascii`（ASCII 字符，如果是非打印键或者松开事件则为 0）、`scancode`（原始扫描码字节，包含 bit 7 的 press/release 信息）、`pressed`（true=按下，false=松开）、`shift`、`ctrl`、`alt`（事件发生时的 modifier 快照）。这个结构体的设计让消费者拥有完整的信息——你可以只看 `ascii` 来处理字符输入，也可以看 `scancode` 来处理功能键，还可以看 modifier 组合来实现快捷键。

> 参考：[OSDev Wiki - PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard)

## 动手实现

### Step 1: 设计 KeyEvent 结构体和 Keyboard 类骨架

**目标**: 定义按键事件的数据结构和键盘驱动的类接口。

**设计思路**: KeyEvent 是一个纯数据结构（POD 类型），包含上面提到的六个字段。Keyboard 类的所有成员都是 static 的——因为整个系统只有一个 PS/2 键盘控制器，没有理由支持多实例。类的公开接口只有三个方法：`init()`（上一章已经实现）、`irq1_handler()`（IRQ1 中断处理，本章实现）、`poll()`（从环形缓冲区取出一个事件，返回 bool 表示是否成功取到）。私有成员包括：64 个 KeyEvent 的数组（环形缓冲区）、head 和 tail 指针（uint32_t）、三个 modifier 布尔标志、以及一个 `enqueue()` 方法。

**实现约束**: 头文件 `kernel/drivers/keyboard/keyboard.hpp` 中，KeyEvent 放在 `cinux::drivers` 命名空间中。Keyboard 类也在同一命名空间。前向声明 `cinux::arch::InterruptFrame`（避免在头文件中 include idt.hpp）。所有 static 成员变量需要在 `.cpp` 文件中定义（不仅是声明），否则链接时找不到符号。这是 C++ static 成员变量的经典规则——头文件里只是声明，.cpp 里要写 `KeyEvent Keyboard::queue_[64] = {};` 这样的定义行。

**踩坑预警**: 如果你只在头文件里声明了 static 成员变量但没有在 .cpp 中定义，链接阶段会报 "undefined reference" 错误，而且错误信息指向的是你访问这些变量的每一行代码，非常烦人。另外，KeyEvent 结构体的字段顺序建议是：ascii 在前，然后是 scancode、pressed、三个 modifier——保持和声明顺序一致，避免结构体对齐填充导致不必要的内存浪费。

**验证**: 这一步只涉及类型定义，不需要独立验证。构建成功就说明结构体和类定义没有语法错误。

### Step 2: 实现扫描码查找表

**目标**: 创建两张 128 项的查找表，实现扫描码集 1 到 ASCII 的翻译。

**设计思路**: `kScToLower` 表按照 US QWERTY 布局填写。扫描码 0x02-0x0B 对应数字行的 1-9 和 0（注意不是 0-9，'1' 在 0x02，'0' 在 0x0B），0x10-0x1B 对应 QWERTYUIOP 这一行，0x1E-0x28 对应 ASDFGHJKL 这一行，0x2C-0x35 对应 ZXCVBNM 这一行。特殊键：0x01 是 Esc（映射为 ASCII 27），0x0E 是退格（映射为 '\b'），0x1C 是回车（映射为 '\n'），0x39 是空格。`kScToUpper` 表在字母位置映射大写字母，在数字行映射 shift 符号（'1'->'!'、'2'->'@'、'3'->'#'、'4'->'$'、'5'->'%'、'6'->'^'、'7'->'&'、'8'->'*'、'9'->'('、'0'->')'），在标点符号位置也是对应的 shift 版本（'-'->'_'、'='->'+'、'['->'{'，等等）。

**实现约束**: 两张表都是 `static constexpr char` 数组，128 个元素，定义在 `keyboard.cpp` 的匿名命名空间外部（但仍然是文件内部链接）。不可打印的键（功能键、modifier 键、0x00）全部填 0。表的排列建议每行 8 个元素，用注释标注扫描码范围，这样容易对照键盘布局检查。

**踩坑预警**: 填表的时候最容易出错的是数字行——扫描码 0x0A 是 '9'，0x0B 是 '0'，不是 0x09='9'/0x0A='0'。如果你把 9 和 0 的位置搞反了，按下 '9' 会输出 '0'，按下 '0' 什么都不输出（因为越界到了下一个条目）。另外，退格键（0x0E）映射为 '\b' 而不是 0x7F（DEL），这是 PS/2 键盘的标准行为。回车键（0x1C）映射为 '\n' 而不是 '\r'，这是我们自己的设计选择——Unix 传统。

**验证**: 构建后运行 host 端测试 `ctest --test-dir build -L keyboard`，扫描码翻译相关的测试用例应该全部通过。如果你想手动验证，可以检查 `kScToLower[0x1E]` 是否等于 'a'，`kScToUpper[0x1E]` 是否等于 'A'，`kScToLower[0x02]` 是否等于 '1'，`kScToUpper[0x02]` 是否等于 '!'。

### Step 3: 实现 IRQ1 handler —— 扫描码解码与 enqueue

**目标**: 编写中断处理函数，完成从原始扫描码到 KeyEvent 的完整转换链路。

**设计思路**: 当 IRQ1 触发时，handler 需要做以下几件事：

首先，从数据端口 0x60 读取一个字节的扫描码——这个字节就是 8042 翻译后的集 1 扫描码。读操作必须在发送 EOI 之前完成，因为如果先发 EOI，新的 IRQ1 可能在你读完 0x60 之前就到达了，导致当前扫描码被覆盖。

然后，检查是否是扩展前缀 0xE0。如果是，直接发送 EOI 然后返回——我们选择忽略所有扩展键。注意 0xE0 会触发一次 IRQ1，后面的实际扫描码会触发第二次 IRQ1，所以一个扩展按键会调用两次 handler，第一次被忽略，第二次的扫描码不是 0xE0 所以会被正常处理——但此时 make code 的含义已经不对了（因为它本来是 0xE0 后面跟着的键码，不是独立的键码）。为了彻底避免这个问题，我们的做法是：遇到 0xE0 就跳过整个事件，不做任何 enqueue。这样扩展键的第二次中断处理的是一个孤立的扫描码，它会被当作一个普通键来解码——但由于大多数扩展键的扫描码在查找表中映射为 0（比如方向键），所以实际上不会产生错误的 ASCII 输出。

接下来，判断按下还是松开：bit 7 为 0 是按下，为 1 是松开。提取 make code：和 0x7F 做 AND。

然后是 modifier 跟踪：如果 make code 是左 Shift（0x2A）或右 Shift（0x36），更新 `shift_held_` 为 pressed 的值。如果是左 Ctrl（0x1D），更新 `ctrl_held_`。如果是左 Alt（0x38），更新 `alt_held_`。注意这一步必须在构建 KeyEvent 之前完成，因为我们需要"当前的"modifier 状态。

构建 KeyEvent：填入 scancode、pressed、三个 modifier 的当前值。ASCII 翻译只在按键按下且 make code < 128 时执行：如果 `shift_held_` 为真，查 `kScToUpper`，否则查 `kScToLower`。松开事件的 ascii 固定为 0。

最后，调用 `enqueue()` 把事件放进环形缓冲区，然后调用 `PIC::send_eoi(1)` 通知 PIC 中断处理完毕。

**实现约束**: handler 的签名是 `static void irq1_handler(cinux::arch::InterruptFrame* frame)`。参数 `frame` 我们不用（键盘中断不需要中断帧中的信息），但签名必须和汇编 stub 调用的约定一致。所有操作都是确定性的，不涉及动态内存分配或者阻塞等待。

**踩坑预警**: EOI 必须在读扫描码之后发送。如果你先发 EOI 再读端口 0x60，有可能在两者之间又来了一个 IRQ1，此时你再读 0x60 拿到的就是新扫描码，旧的就丢了。另外一个容易忽略的点：modifier 跟踪对松开事件也要处理。如果 Shift 按下时设置了 `shift_held_ = true`，但松开时忘了设置 `shift_held_ = false`，那之后所有按键都会被当作 Shift 被按下的状态来翻译——你会发现打出来的全是 大写字母和符号，而且永远不会恢复。

**验证**: 运行 QEMU in-kernel 测试（`ctest` 的 kernel 测试部分），test_keyboard.cpp 中的解码测试应该通过。关键测试用例包括：扫描码 0x1E 产生 ascii='a'、pressed=true；扫描码 0x9E 产生 pressed=false、ascii=0；先发 0x2A（Shift 按下）再发 0x1E（A 按下）产生 ascii='A'、shift=true。

### Step 4: 实现环形缓冲区的 enqueue 和 poll

**目标**: 编写环形缓冲区的写入和读取操作。

**设计思路**: `enqueue()` 的工作是：计算 tail 的下一个位置 `next = (tail + 1) % SIZE`。如果 `next == head`，说明缓冲区满了，直接返回（丢弃事件）。否则在 `queue_[tail]` 写入事件，然后 `tail = next`。`poll()` 的工作是：如果 `head == tail`，返回 false（缓冲区空）。否则从 `queue_[head]` 读出事件到输出参数，然后 `head = (head + 1) % SIZE`，返回 true。

这两个函数不需要锁或者原子操作，因为我们的执行环境保证了安全：enqueue 只在 IRQ1 handler 中被调用（中断上下文），poll 只在 kernel_main 的主循环中被调用（正常上下文）。x86 的单字节写入是原子的，而且 `cli`/`sti` 和中断门本身就保证了 enqueue 不会被 poll 的执行打断（反过来可以：poll 被 enqueue 打断是安全的，因为 enqueue 只修改 tail 而 poll 只修改 head）。

**实现约束**: 两个方法都是 Keyboard 类的 static 方法。缓冲区大小用 `static constexpr uint32_t KEY_QUEUE_SIZE = 64` 定义。取模运算可以用 `%` 操作符（编译器会优化为位运算，因为 64 是 2 的幂）。`enqueue` 是 private 方法，只有 irq1_handler 可以调用。`poll` 是 public 方法，供外部调用。

**踩坑预警**: 环形缓冲区最经典的 bug 是"满/空判断混淆"。head == tail 既可能是空也可能是满——这取决于你是先写再推进 tail，还是先推进 tail 再写。我们采用的是"先写再推进"的方式，判断满的条件是 `tail+1 == head`。如果你把判断条件写反了，要么永远不认为满了（导致覆盖未读数据），要么永远认为满了（导致所有事件被丢弃）。另外，取模千万别忘了——如果 tail 一直加到超过 63 然后变成 64、65、66...，数组就越界了。

**验证**: host 端测试中的 ring buffer 测试用例应该覆盖这些场景：空缓冲区 poll 返回 false、单个事件 enqueue+poll 正确、多个事件 FIFO 顺序正确、缓冲区满时丢弃事件、缓冲区回绕后仍然正确。如果你自己写测试，可以构造一个"填满 63 个事件 -> 尝试填第 64 个 -> 验证被丢弃 -> 逐个 poll 验证前 63 个"的场景。

### Step 5: 实现 kernel_main 的键盘轮询循环

**目标**: 在内核主循环中轮询键盘事件并回显到屏幕。

**设计思路**: 上一章我们把 kernel_main 的 idle loop 改成了 `while(1) { hlt; }`，这是一个 WFI（Wait For Interrupt）模式——CPU 执行 `hlt` 指令进入低功耗状态，直到下一个中断到来时被唤醒。现在我们要在每次被唤醒之后，把环形缓冲区里所有待处理的事件全部排空。具体做法是：`hlt` 之后接一个 while 循环，反复调用 `Keyboard::poll(ev)`，直到返回 false（缓冲区空了）。对于每个取出的事件，只处理"按下且 ascii 非零"的情况——调用 `console.putc(ev.ascii)` 把字符输出到屏幕。松开事件和非打印键事件被忽略。

这样整个数据通路就打通了：按键 -> 8042 翻译扫描码 -> IRQ1 中断 -> handler 解码并 enqueue -> 主循环 poll 取出 -> console.putc 输出到屏幕。由于 kprintf 已经注册了 Console 作为输出后端，所以屏幕和串口会同步显示你输入的字符。

**实现约束**: 在 `kernel/main.cpp` 的 while 循环中，`hlt` 之后添加 poll 循环。需要声明一个 `KeyEvent ev` 变量（循环外部，避免每次重新分配）。`console.putc()` 调用需要确保 `console` 变量在这个位置是可访问的——它是在前面 Console 初始化时声明的局部变量。

**踩坑预警**: `console.putc()` 不能在中断上下文中被调用——它操作帧缓冲区，而帧缓冲区的滚动操作涉及大量内存拷贝，如果在 IRQ handler 中做这些事，中断延迟会变得非常大。所以我们严格遵循"中断只 enqueue、主循环做渲染"的原则。另外，poll 循环必须是一个 while 而不是 if——因为一次中断唤醒之后可能积攒了多个事件（比如你快速按了好几个键，或者扩展键触发了两次 IRQ1 但第一次被跳过了），必须全部排空。

**验证**: 构建并启动 QEMU。在 QEMU 窗口中点击一下让它获得键盘焦点，然后按下任意字母键——屏幕上应该出现对应的字符。按下 Shift+A，应该出现大写 'A'。按下回车键，屏幕应该换行。按下退格键，光标应该回退。串口输出应该同步显示相同的内容。

完整验证步骤：启动 QEMU -> 点击窗口获取焦点 -> 依次输入 `hello` -> 屏幕显示 `hello` -> 按回车 -> 换行 -> 按 Shift+1 -> 屏幕显示 `!`。如果一切正常，恭喜，你的内核第一次"听"到了你的声音。

## 构建与运行

构建后启动 QEMU，观察初始化日志中键盘相关的部分。然后点击 QEMU 窗口获取键盘焦点（这个很重要——如果焦点不在 QEMU 窗口上，按键事件会被宿主系统截获），开始打字。你应该能看到每一个可打印字符都出现在屏幕上。

如果你同时想看 host 端测试，运行：

```
cmake --build build && cd build && ctest -L keyboard --output-on-failure
```

这会运行 714 行的 host 端测试，覆盖扫描码翻译、modifier 跟踪、环形缓冲区操作等所有逻辑。

## 调试技巧

**按键没反应（屏幕无输出）**: 首先检查串口日志——如果 `[KBD]` 初始化日志正常出现了，说明控制器初始化没问题。然后在 kernel_main 的 poll 循环中加一条调试日志（比如在 `Keyboard::poll(ev)` 返回 true 时打印 `ev.scancode` 和 `ev.ascii`），看看是不是 handler 根本没被调用（说明 IRQ1 没有到达）还是 handler 调用了但 poll 取不到事件（说明 enqueue 有问题）。如果 handler 没被调用，检查 `PIC::unmask(1)` 是否被调用了、`sti` 是否执行了。

**按键输出乱码**: 大概率是查找表填错了。用 host 端测试验证 `kScToLower[0x1E] == 'a'` 和 `kScToUpper[0x1E] == 'A'`。如果这些基本映射都错了，检查表的索引排列——确保你把值填在了正确的偏移位置上。

**Shift 键"粘住"（一直是 大写）**: 说明 Shift 的 break code 没有被正确处理。检查 modifier 跟踪逻辑：在收到 0xAA（左 Shift 松开）时，`shift_held_` 应该被设为 false。如果条件判断里只检查了 make code 而没有检查 pressed 状态，Shift 的按下和松开都会设为 true。

**第一个按键丢失**: 这可能是因为初始化时输出缓冲区里有一个残留字节没有被 flush 掉。检查 `Keyboard::init()` 的第二步（刷新输出缓冲区）是否正确实现——应该循环读取直到状态寄存器 bit 0 变为 0。

**ring buffer 测试失败**: 如果 host 端测试中 wrap-around 测试失败了，检查取模运算是否正确——`% KEY_QUEUE_SIZE` 中的 KEY_QUEUE_SIZE 必须是 64。如果你不小心写成了 63 或者 65，回绕点就错了。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| 扫描码集 1 | Make: bit7=0, Break: bit7=1, make_code = sc & 0x7F |
| 查找表翻译 | kScToLower[128] 和 kScToUpper[128]，索引 0 = 无映射 |
| Modifier 跟踪 | 三个布尔变量，每次 modifier 键 press/release 时更新 |
| KeyEvent | ascii + scancode + pressed + shift/ctrl/alt 快照 |
| 环形缓冲区 | 64 槽位，容量 63，head/tail 指针，满时丢弃 |
| EOI 时序 | 先读 0x60 再发 PIC::send_eoi(1)，防止扫描码被覆盖 |
| Poll 循环 | hlt 唤醒后 while(poll(ev)) 排空缓冲区，只回显 pressed && ascii!=0 |
| 扩展键 0xE0 | 检测到直接跳过，不 enqueue |
