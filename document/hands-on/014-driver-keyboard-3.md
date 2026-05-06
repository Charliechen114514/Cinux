# 014-3 事件驱动：IRQ1 处理、环形缓冲区与键盘回显

## 导语

前两章我们搭好了全部基础：8042 控制器初始化完成，IRQ1 绑定到位，扫描码查找表和 modifier 跟踪逻辑都已就绪。这一章要把整条数据通路彻底打通：从 IRQ1 中断触发开始，handler 读取扫描码、解码、构建 KeyEvent、入队到环形缓冲区，然后 kernel_main 的轮询循环从缓冲区取出事件，回显到屏幕上。完成这一章后，你在 QEMU 窗口里敲键盘，屏幕上就会出现对应的字符——这是内核第一次真正"响应"外部输入。

知识前置：前两章的所有内容。你需要理解扫描码集 1 的 make/break 规则、查找表的设计、modifier 跟踪机制，以及环形缓冲区的基本原理。

## 概念精讲

### 环形缓冲区（Ring Buffer）

中断处理函数和主循环运行在不同的"上下文"中——一个在 IRQ1 的中断栈上，一个在内核主栈上。我们需要一个机制让这两边安全地传递数据，这个机制就是环形缓冲区。

环形缓冲区的原理是：一个固定大小的数组（我们用 64 个 KeyEvent 槽位），加上一个 head 指针（读位置）和一个 tail 指针（写位置）。enqueue 操作在 tail 位置写入数据，然后 tail 前进一步（对 64 取模实现回绕）。poll 操作从 head 位置读取数据，然后 head 前进一步。当 head == tail 时，缓冲区为空；当 tail 的下一个位置是 head 时，缓冲区满了——此时 enqueue 选择丢弃新事件而不是覆盖旧数据。

这意味着实际容量是 63 个事件（64 - 1，有一个槽位被"浪费"来区分满和空）。对于正常打字速度来说 63 个事件绑绑有余，但如果主循环被阻塞了太久（比如在做一个很长的计算），快速连续打字可能会丢事件。这是一个合理的权衡——比起阻塞中断（会导致丢失更多键），丢几个按键是可以接受的。

### EOI 时序的重要性

在 IRQ1 handler 中，EOI 的发送时序至关重要。你必须先从端口 0x60 读走扫描码，然后再发 EOI。如果反过来——先发 EOI 再读端口——PIC 可能在你读完之前就允许下一个 IRQ1 中断进来，导致前一个扫描码还没处理就被新的覆盖。8042 控制器的输出缓冲区只有一个字节深度，读慢了真的会丢数据。

另外，扩展键前缀 0xE0 被跳过时也要发 EOI。如果不发，PIC 会认为 IRQ1 还在服务中，后续所有 IRQ1（包括 0xE0 后面那个真实扫描码触发的中断）都会被 PIC 阻塞，键盘就彻底不响应了。这种 bug 非常隐蔽——你不会看到任何报错，只是键盘突然死了，而且只在按扩展键的时候才触发。

### 主循环的 poll 模式

kernel_main 的主循环采用经典的"中断驱动 + 轮询消费"模式：CPU 用 `hlt` 指令休眠等待中断，被唤醒后检查环形缓冲区是否有事件。`while (Keyboard::poll(ev))` 会一直 poll 到缓冲区为空，对每个事件检查 `ev.pressed && ev.ascii != 0`——只有"按下"且"有 ASCII 映射"的事件才会被 `console.putc()` 显示。

这意味着 modifier 键的按下/释放不会产生任何可见输出（因为它们的 `ascii` 为 0），功能键也不会（同样 `ascii` 为 0），按键释放也不会（`pressed` 为 false）。只有真正的可打印字符按键——字母、数字、标点、空格、回车、退格——才会回显到屏幕上。

之所以要在一个 `hlt` 唤醒后排空所有事件，是因为按键事件可能在 CPU 醒来之前就积攒了好几个——一次按键会产生 make code 和 break code 两个中断，如果主循环每次只 poll 一个事件，第二个事件就要等到下一次 `hlt` 唤醒才能处理，延迟感会很明显。

> 参考：[OSDev Wiki - PS/2 Keyboard](https://wiki.osdev.org/PS/2_Keyboard)
> 参考：[OSDev Wiki - 8259 PIC](https://wiki.osdev.org/8259_PIC)

## 动手实现

### Step 1: 实现 IRQ1 handler —— 扫描码解码与 enqueue

**目标**: 编写中断处理函数，完成从原始扫描码到 KeyEvent 的完整转换链路。

**设计思路**: 当 IRQ1 触发时，handler 需要做以下几件事：

首先，从数据端口 0x60 读取一个字节的扫描码——这个字节就是 8042 翻译后的集 1 扫描码。读操作必须在发送 EOI 之前完成，因为如果先发 EOI，新的 IRQ1 可能在你读完 0x60 之前就到达了，导致当前扫描码被覆盖。

然后，检查是否是扩展前缀 0xE0。如果是，直接发送 EOI 然后返回——我们选择忽略所有扩展键。注意 0xE0 会触发一次 IRQ1，后面的实际扫描码会触发第二次 IRQ1，所以一个扩展按键会调用两次 handler，第一次被忽略，第二次的扫描码不是 0xE0 所以会被正常处理——但此时 make code 的含义已经不对了（因为它本来是 0xE0 后面跟着的键码，不是独立的键码）。为了彻底避免这个问题，我们的做法是：遇到 0xE0 就跳过整个事件，不做任何 enqueue。这样扩展键的第二次中断处理的是一个孤立的扫描码，它会被当作一个普通键来解码——但由于大多数扩展键的扫描码在查找表中映射为 0（比如方向键），所以实际上不会产生错误的 ASCII 输出。

接下来，判断按下还是松开：bit 7 为 0 是按下，为 1 是松开。提取 make code：和 0x7F 做 AND。

然后是 modifier 跟踪：如果 make code 是左 Shift（0x2A）或右 Shift（0x36），更新 `shift_held_` 为 pressed 的值。如果是左 Ctrl（0x1D），更新 `ctrl_held_`。如果是左 Alt（0x38），更新 `alt_held_`。注意这一步必须在构建 KeyEvent 之前完成，因为我们需要"当前的"modifier 状态。

构建 KeyEvent：填入 scancode、pressed、三个 modifier 的当前值。ASCII 翻译只在按键按下且 make code < 128 时执行：如果 `shift_held_` 为真，查 `kScToUpper`，否则查 `kScToLower`。松开事件的 ascii 固定为 0。

最后，调用 `enqueue()` 把事件放进环形缓冲区，然后调用 `PIC::send_eoi(1)` 通知 PIC 中断处理完毕。

**实现约束**: handler 的签名是 `static void irq1_handler(cinux::arch::InterruptFrame* frame)`。参数 `frame` 我们不用（键盘中断不需要中断帧中的信息），但签名必须和汇编 stub 调用的约定一致。所有操作都是确定性的，不涉及动态内存分配或者阻塞等待。

**踩坑预警**: EOI 必须在读扫描码之后发送。如果你先发 EOI 再读端口 0x60，有可能在两者之间又来了一个 IRQ1，此时你再读 0x60 拿到的就是新扫描码，旧的就丢了。另外一个容易忽略的点：modifier 跟踪对松开事件也要处理。如果 Shift 按下时设置了 `shift_held_ = true`，但松开时忘了设置 `shift_held_ = false`，那之后所有按键都会被当作 Shift 被按下的状态来翻译——你会发现打出来的全是大写字母和符号，而且永远不会恢复。

**验证**: 运行 QEMU in-kernel 测试（`ctest` 的 kernel 测试部分），test_keyboard.cpp 中的解码测试应该通过。关键测试用例包括：扫描码 0x1E 产生 ascii='a'、pressed=true；扫描码 0x9E 产生 pressed=false、ascii=0；先发 0x2A（Shift 按下）再发 0x1E（A 按下）产生 ascii='A'、shift=true。

### Step 2: 实现环形缓冲区的 enqueue 和 poll

**目标**: 编写环形缓冲区的写入和读取操作。

**设计思路**: `enqueue()` 的工作是：计算 tail 的下一个位置 `next = (tail + 1) % SIZE`。如果 `next == head`，说明缓冲区满了，直接返回（丢弃事件）。否则在 `queue_[tail]` 写入事件，然后 `tail = next`。`poll()` 的工作是：如果 `head == tail`，返回 false（缓冲区空）。否则从 `queue_[head]` 读出事件到输出参数，然后 `head = (head + 1) % SIZE`，返回 true。

这两个函数不需要锁或者原子操作，因为我们的执行环境保证了安全：enqueue 只在 IRQ1 handler 中被调用（中断上下文），poll 只在 kernel_main 的主循环中被调用（正常上下文）。x86 的单字节写入是原子的，而且 `cli`/`sti` 和中断门本身就保证了 enqueue 不会被 poll 的执行打断（反过来可以：poll 被 enqueue 打断是安全的，因为 enqueue 只修改 tail 而 poll 只修改 head）。

**实现约束**: 两个方法都是 Keyboard 类的 static 方法。缓冲区大小用 `static constexpr uint32_t KEY_QUEUE_SIZE = 64` 定义。取模运算可以用 `%` 操作符（编译器会优化为位运算，因为 64 是 2 的幂）。`enqueue` 是 private 方法，只有 irq1_handler 可以调用。`poll` 是 public 方法，供外部调用。

**踩坑预警**: 环形缓冲区最经典的 bug 是"满/空判断混淆"。head == tail 既可能是空也可能是满——这取决于你是先写再推进 tail，还是先推进 tail 再写。我们采用的是"先写再推进"的方式，判断满的条件是 `tail+1 == head`。如果你把判断条件写反了，要么永远不认为满了（导致覆盖未读数据），要么永远认为满了（导致所有事件被丢弃）。另外，取模千万别忘了——如果 tail 一直加到超过 63 然后变成 64、65、66...，数组就越界了。

**验证**: host 端测试中的 ring buffer 测试用例应该覆盖这些场景：空缓冲区 poll 返回 false、单个事件 enqueue+poll 正确、多个事件 FIFO 顺序正确、缓冲区满时丢弃事件、缓冲区回绕后仍然正确。如果你自己写测试，可以构造一个"填满 63 个事件 -> 尝试填第 64 个 -> 验证被丢弃 -> 逐个 poll 验证前 63 个"的场景。

### Step 3: 实现 kernel_main 的键盘轮询循环

**目标**: 在内核主循环中轮询键盘事件并回显到屏幕。

**设计思路**: 前一章我们把 kernel_main 的 idle loop 改成了 `while(1) { hlt; }`，这是一个 WFI（Wait For Interrupt）模式——CPU 执行 `hlt` 指令进入低功耗状态，直到下一个中断到来时被唤醒。现在我们要在每次被唤醒之后，把环形缓冲区里所有待处理的事件全部排空。具体做法是：`hlt` 之后接一个 while 循环，反复调用 `Keyboard::poll(ev)`，直到返回 false（缓冲区空了）。对于每个取出的事件，只处理"按下且 ascii 非零"的情况——调用 `console.putc(ev.ascii)` 把字符输出到屏幕。松开事件和非打印键事件被忽略。

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

这会运行 host 端测试，覆盖扫描码翻译、modifier 跟踪、环形缓冲区操作等所有逻辑。

## 调试技巧

**按键没反应（屏幕无输出）**: 首先检查串口日志——如果 `[KBD]` 初始化日志正常出现了，说明控制器初始化没问题。然后在 kernel_main 的 poll 循环中加一条调试日志（比如在 `Keyboard::poll(ev)` 返回 true 时打印 `ev.scancode` 和 `ev.ascii`），看看是不是 handler 根本没被调用（说明 IRQ1 没有到达）还是 handler 调用了但 poll 取不到事件（说明 enqueue 有问题）。如果 handler 没被调用，检查 `PIC::unmask(1)` 是否被调用了、`sti` 是否执行了。

**按键输出乱码**: 大概率是查找表填错了。用 host 端测试验证 `kScToLower[0x1E] == 'a'` 和 `kScToUpper[0x1E] == 'A'`。如果这些基本映射都错了，检查表的索引排列——确保你把值填在了正确的偏移位置上。

**Shift 键"粘住"（一直是大写）**: 说明 Shift 的 break code 没有被正确处理。检查 modifier 跟踪逻辑：在收到 0xAA（左 Shift 松开）时，`shift_held_` 应该被设为 false。如果条件判断里只检查了 make code 而没有检查 pressed 状态，Shift 的按下和松开都会设为 true。

**第一个按键丢失**: 这可能是因为初始化时输出缓冲区里有一个残留字节没有被 flush 掉。检查 `Keyboard::init()` 的第二步（刷新输出缓冲区）是否正确实现——应该循环读取直到状态寄存器 bit 0 变为 0。

**ring buffer 测试失败**: 如果 host 端测试中 wrap-around 测试失败了，检查取模运算是否正确——`% KEY_QUEUE_SIZE` 中的 KEY_QUEUE_SIZE 必须是 64。如果你不小心写成了 63 或者 65，回绕点就错了。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| IRQ1 handler | 读 0x60 -> 检查 0xE0 -> make/break 判定 -> modifier 更新 -> ASCII 翻译 -> enqueue -> EOI |
| 环形缓冲区 | 64 槽位，容量 63，head/tail 指针，满时丢弃 |
| EOI 时序 | 先读 0x60 再发 PIC::send_eoi(1)，防止扫描码被覆盖 |
| Poll 循环 | hlt 唤醒后 while(poll(ev)) 排空缓冲区，只回显 pressed && ascii!=0 |
| 扩展键 0xE0 | 检测到直接跳过，发 EOI 返回，不 enqueue |
| 中断 vs 主循环 | handler 只做 enqueue，渲染留给主循环，避免中断延迟 |
