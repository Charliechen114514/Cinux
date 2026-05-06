# 002-2 · Linker Script 修正与 GDT 定义

## 导语

上一篇我们理清了保护模式的核心概念，修掉了 `serial.S` 的 `.code16gcc` 汇编模式隐患。这一篇我们继续打地基：修正 linker script 的基地址、新增 QEMU debugcon 调试支持，然后在 `stage2.S` 中定义一张完整的 GDT。

这几个改动看起来都不起眼，但说实话，每一个不动都会在后续的保护模式切换中让你收获一个漂亮的三重故障（Triple Fault）。

我们先回顾一下内存布局的大图。MBR 通过 BIOS INT 0x13 扩展读把 stage2.bin 加载到物理地址 `0x8000`。linker script 必须以 `0x8000` 为基址，这样所有绝对地址引用（尤其是 GDT pointer 里的 base 地址）才能在运行时指向正确的物理内存。

如果 base 地址和加载地址不一致，`lgdt` 读到的 GDT 地址就是错的，CPU 加载一堆垃圾描述符，后续保护模式切换直接炸——而且这个问题的排查难度极高，因为症状和"代码写错了"完全一样（三重故障），但你得先意识到"不是我代码写错了，是我给链接器的指示写错了"才能找到根因。

---

## 动手实现

### Step 2: 修正 Linker Script 基地址

**目标**: 把 Stage2 的 linker script 中 `.text` section 的起始地址从 `0x0` 改为 `0x8000`，同时新增一个 `.gdt` section 放在 `.text` 之后、`.rodata` 之前，并设置 8 字节对齐。在末尾添加断言，确保 stage2 总大小不超过 32KB。

**设计思路**: 为什么 linker base 地址必须匹配加载地址？

因为 MBR 把 Stage2 从磁盘读出来后，是直接加载到物理内存 `0x8000` 的（在 `mbr.S` 中定义的 `STAGE2_LOAD_ADDR`）。如果 linker script 里写的 base 地址是 `0x0`，那链接器在解析所有绝对地址引用时（比如 GDT 指针里的 base 地址），都会基于 `0x0` 来计算。

但运行时这段代码实际在 `0x8000`，于是 `lgdt` 读到的 GDT 地址会指向错误的位置——比如 `gdt` 符号被链接成 `0x0160`，但实际物理地址应该是 `0x8160`，中间差了 `0x8000`。轻则加载了垃圾数据，重则直接 triple fault。

把 base 改成 `0x8000` 之后，链接器算出来的所有绝对地址都会加上 `0x8000` 的偏移，和运行时的物理地址完全匹配。

这个问题的排查难度极高，因为症状和"代码写错了"完全一样——三重故障——但你得先意识到"不是我代码写错了，是我给链接器的指示写错了"才能找到根因。

新增的 `.gdt` section 是为了让 GDT 数据结构有自己独立的、8 字节对齐的存储区域，而不是混在 `.text` 或 `.rodata` 里。Intel SDM Vol.3A Section 3.5.1 建议描述符表 8 字节对齐以提高性能，虽然不是强制要求，但对齐访问确实更高效，而且描述符本身就是 8 字节的，对齐后每个描述符的起始地址都是整齐的。

如果 GDT 定义放在 `.text` 中间，会有两个问题：一是 GDT 可能不被 8 字节对齐，二是 GDT 数据会被夹杂在可执行代码中间，影响 `.text` 的 cache 局部性。

linker script 还需要加一个断言：Stage2 的总大小不能超过 `0x10000`（即 32KB，从 0x8000 到 0x10000），这是 MBR 加载时的空间限制。修改位置在 `boot/CMakeLists.txt` 中通过 `file(WRITE ...)` 生成的 linker script。

修改后的 linker script 结构如下：`.text` 在最前面（从 `0x8000` 开始），`.gdt` 紧随其后并 8 字节对齐，然后是 `.rodata`、`.data` 和 `.bss`。最后的 `ASSERT(. <= 0x10000, ...)` 确保整个 stage2 不超过 32KB。

**踩坑预警**: 你可能会问：为什么 stage2 被加载到 `0x8000` 而不是别的地址？

这是在 MBR 的 `mbr.S` 里决定的——MBR 通过 BIOS INT 0x13 扩展读把 stage2 加载到 `0x0000:0x8000`。这个地址选择没有特殊含义，只要不跟 MBR 自身（`0x7C00`）、BIOS 数据区（`0x400`-`0x4FF`）、VESA 缓冲区（`0x6000`-`0x64FF`）冲突就行。

但关键是：linker script 的 base 必须与这个加载地址严格一致，否则所有绝对地址引用全错。xv6 把它的 boot sector 链接在 `0x7C00`（bootasm.S 和 MBR 放在同一个位置），而 Cinux 的两阶段设计让 stage2 有了自己独立的加载地址和链接地址。

**验证**: 构建完成后，用 `objdump` 检查 Stage2 ELF 文件中的符号地址，确认所有地址都在 `0x8000` 以上。

具体来说，`_start` 应该在 `0x8000`，GDT 相关的符号（比如 `gdt`、`gdt_ptr`）应该在 `0x8xxx` 范围内：

```bash
objdump -t build/boot/stage2 | grep -E '(gdt|_start|pm_entry)'
```

如果 `gdt` 的地址显示为 `0x0160` 这样的值（小于 `0x8000`），说明 linker script 的 base 没改对。

你也可以用 `readelf -s build/boot/stage2 | grep gdt` 来查看 gdt 相关符号的链接地址，确认它们都在 `0x8000` 以上。

---

### Step 2.5: 配置 QEMU Debug Console

**目标**: 在 `cmake/qemu.cmake` 中添加 debugcon 支持，让保护模式下的 `outb` 输出能被 QEMU 捕获。

**设计思路**: 进入保护模式之后，BIOS INT 10h 就不能用了——保护模式下没有 BIOS 中断服务。我们需要一个新的调试输出手段，而且这个手段最好不需要初始化任何硬件——因为我们正处于一个脆弱的过渡期，任何复杂的初始化都可能引入新的 bug。

QEMU 提供了一个非常方便的 debug console（调试控制台），对应 I/O 端口 `0xE9`：向这个端口写一个字节，QEMU 就会把这个字节输出到宿主机的终端或文件。不需要初始化串口，不需要配置波特率，一个简单的输出指令就能搞定。

这个功能最早是 Bochs 仿真器引入的（所以有时候你会看到它被称为 "Bochs debug port"），QEMU 兼容了这个约定，OSDev 社区也把它当成了事实标准。

我们需要在 `cmake/qemu.cmake` 的 `QEMU_COMMON_FLAGS` 中添加两行参数。

第一行是 `-debugcon file:debug.log`，告诉 QEMU 把 debug console 的输出写到 `debug.log` 文件而不是 `stdio`——我们选择写文件是因为 `stdio` 已经被串口（`-serial stdio`）占用了，混在一起不好分辨。

第二行是 `-global isa-debugcon.iobase=0xe9`，把 debug console 的 I/O 端口设为 `0xE9`。

当然，debugcon 的局限在于它是 QEMU 专有的，真实硬件上不存在。但在 Bootloader 开发阶段，我们几乎 100% 时间都在 QEMU 里调试，这个局限完全可接受。后续进入 long mode 之后我们会切换到 COM1 串口作为正式的输出通道。

使用方式极其简单：在保护模式下的代码里，只需要向端口 `0xE9` 写入一个字节，QEMU 就会把这个字节输出到 `debug.log` 文件。比如我们后面会向它写入 `0x50`（字母 `P` 的 ASCII 码），如果一切顺利，`debug.log` 里就会看到这个 `P`。

**验证**: 修改后重新构建并运行，确认 QEMU 正常启动（debugcon 参数不会导致报错）：

```bash
cd build
make run
# Ctrl+C 退出后
cat debug.log
```

此时 `debug.log` 应该是空的（因为我们还没写保护模式切换代码），但 QEMU 不应该报错。

如果你看到 QEMU 报了类似 "invalid option" 的错误，大概率是 `-debugcon` 或 `-global isa-debugcon.iobase=0xe9` 的参数格式写错了。检查 `cmake/qemu.cmake` 中的 `QEMU_COMMON_FLAGS`，确保两行参数的格式与本文描述一致。

另一个可能的问题是 `debug.log` 文件没有出现在 build 目录下——这可能是因为你的工作目录不在 build 里，或者 QEMU 因为其他原因提前退出了。用绝对路径检查一下：`ls -la $(pwd)/debug.log`。

如果你在 `debug.log` 里看到了意料之外的字符（比如乱码），那可能是因为之前的运行残留了旧的日志。删掉 `debug.log` 后重新运行即可。

---

### Step 3: 定义 GDT

**目标**: 在 `stage2.S` 中新增一个 `.gdt` section，在里面定义三个段描述符（null、code32、data32）和 GDT 指针结构 `gdt_ptr`。

**设计思路**: 我们先理清楚需要定义什么，再逐个解释每个字段的含义。GDT 的定义放在 `stage2.S` 的末尾，位于独立的 `.section .gdt,"a"` 中，用 `.align 8` 确保 8 字节对齐。`"a"` 标记表示 allocatable（可分配），这是段属性的标准写法。

首先是一个 Null 描述符，占 8 字节全零，放在 GDT 的第 0 个位置（索引 0）。Intel 硬件规定这个条目必须存在且必须全零，段选择子值为 `0x00` 时指向它，CPU 试图用 null selector 访问内存会触发 General Protection Fault。

这不是可选的装饰，是强制的。这个设计的目的是让"忘记初始化段寄存器"这种编程错误能被立即捕获，而不是静默地读到随机数据。这是一个非常典型的硬件辅助调试设计——x86 架构里有很多类似的"帮你踩刹车"的机制。

第二个是 32 位代码段描述符，放在索引 1，选择子值为 `0x08`。这个描述符定义了保护模式下的代码段：基地址（Base）全部为 0，大小限制（Limit）的低 16 位填 `0xFFFF`、高 4 位填 `0xF`，合起来 20 位就是 `0xFFFFF`。

粒度位 G 设为 1（表示以 4KB 为单位），所以 `0xFFFFF * 4096 = 4GB`，覆盖整个地址空间。Access Byte 的值是 `0x9A`，我们拆开来看：最高位 P=1 表示段在内存中存在，接下来两位 DPL=00 表示 Ring 0 内核级权限，S=1 表示这是代码/数据段（不是系统段），Type 字段的 4 位是 `1010`，在代码段类型中表示可执行且可读。

Flags 字节是 `0xCF`，其中高 4 位是 `1100`：G=1（4KB 粒度）、D=1（32 位默认操作数大小），剩下两位是 Limit 的高 4 位 `0xF`。

第三个是 32 位数据段描述符，放在索引 2，选择子值为 `0x10`。布局和代码段几乎一模一样，唯一的区别在 Access Byte：值是 `0x92`，P=1、DPL=00、S=1，Type 是 `0010` 表示可读写的数据段。

数据段不需要可执行权限，但需要可写权限——栈和数据都在这个段里。Limit 和 Base 的值与代码段完全相同，实现平坦内存模型。你会发现两个描述符的 Base 都是 `0x00000000`，Limit 都是 4GB——整个地址空间被完全平铺，没有任何分段隔离。这就是所谓的 Flat Memory Model。

然后是 GDT 指针结构 `gdt_ptr`，这是 `lgdt` 指令需要读取的 6 字节数据：前 2 字节是 GDT 的大小减 1（limit 字段），后 4 字节是 GDT 的线性基地址。

大小可以用汇编表达式 `gdt_end - gdt - 1` 自动计算——三个描述符各 8 字节共 24 字节，减 1 得到 `0x17`（23）。Limit 之所以要减 1，是因为 limit 字段只有 16 位，能表示的最大值是 65535，所以约定是"GDT 的最后一个有效字节地址"，也就是总大小减 1。

基地址直接写 `gdt` 标号即可，因为 linker script 已经把 base 设成了 `0x8000`，链接器会自动算出正确的绝对地址。

这里有一个非常关键的点需要注意：`lgdt` 指令在实模式下执行时，CPU 计算物理地址的方式是 `DS * 16 + offset`。所以我们在 `stage2.S` 中调用 `lgdt` 之前，必须先确保 `%ds` 为 0，否则 `lgdt` 实际读到的 `gdt_ptr` 地址就会偏移 `DS * 16`，读到的 GDT base 地址就是错的。这个问题我们下一篇实操时再详细展开。

**实现约束**: `.gdt` section 必须用 `.align 8` 确保 8 字节对齐；null 描述符必须全零；`gdt_ptr` 的格式是严格的 2 字节 limit + 4 字节 base，顺序不能反。另外要注意 `.bss` section 在 linker script 里的通配符必须写成 `*(.bss*)`（带星号），因为某些汇编器可能给 `.bss` section 加上额外标记。

**验证**: 编译通过后，检查 `.gdt` section 在 ELF 文件中的大小和内容：

```bash
objdump -s -j .gdt build/boot/stage2
```

你应该能看到三个 8 字节的描述符条目和最后的 6 字节 gdt_ptr。null 描述符全零，code 描述符第 5 字节是 `0x9A`、第 6 字节是 `0xCF`，data 描述符第 5 字节是 `0x92`。

也可以用 `readelf` 来查看 GDT 符号的链接地址：

```bash
readelf -s build/boot/stage2 | grep gdt
```

确认 `gdt` 符号的地址在 `0x8xxx` 范围内（和 linker base `0x8000` 对齐）。如果地址低于 `0x8000`，说明 linker script 的 base 没有正确设置，需要回到 `boot/CMakeLists.txt` 检查。

**进一步验证**: 为了确保 GDT 的每个字节都正确无误，你可以用 `xxd` 以十六进制方式查看 `.gdt` section 的完整内容：

```bash
objcopy -O binary -j .gdt build/boot/stage2 /tmp/gdt.bin && xxd /tmp/gdt.bin
```

正确的输出应该以 8 个零字节（null 描述符）开头，紧接着是代码段描述符（FF FF 00 00 00 9A CF 00），然后是数据段描述符（FF FF 00 00 00 92 CF 00），最后是 6 字节的 gdt_ptr。

如果你看到的不是这些值——比如 Access Byte 的位置出现了 00 或者 FF——说明描述符定义有误。逐字节对照本篇的设计思路部分，检查 Limit、Base、Access Byte 和 Flags 的每一个字段。

这种逐字节验证虽然繁琐，但在调试保护模式切换时是必不可少的——因为 `lgdt` 不会校验 GDT 内容的正确性，如果描述符有误，CPU 只会在后续使用时触发 #GP，到那时候你很难分清是 GDT 定义错了还是别的什么原因。

**关于 `.gdt` section 的位置**: 你可能注意到 GDT section 被放在了 `.text` 之后。这意味着 GDT 数据在内存中的位置紧跟着代码段。这种布局是合理的，因为 stage2 的代码区域从 `0x8000` 开始，代码段结束后紧跟着就是 GDT 数据，中间没有浪费空间。

但是要注意一个细节：如果你在 `.text` section 里增加了大量代码，GDT 的位置会后移，`gdt_ptr` 里的 base 地址也会相应改变。这不是问题——因为 `gdt_ptr` 里的地址是由链接器自动计算的，不需要你手动维护。但你需要确保 `.text` 里的代码不会超过 `.gdt` section 的空间，否则链接器会报错。这也是为什么我们在 linker script 末尾加了 `ASSERT(. <= 0x10000, ...)` 断言的原因。

---

## 本篇小结

到这里，GDT 定义好了、linker script 修正了、debugcon 配置到位了——整个舞台已经搭好，只差最后也是最关键的一幕：让 CPU 真正从实模式切换到保护模式。

回顾一下本篇的三个核心改动。第一，linker script 的 location counter 从 `0x0` 改成了 `0x8000`，确保所有绝对地址引用与运行时的物理地址匹配——这是整个 GDT 能正确工作的基础，因为 `gdt_ptr` 里的 base 地址就是由链接器根据这个 location counter 计算出来的。

第二，`cmake/qemu.cmake` 新增了 `-debugcon file:debug.log` 和 `-global isa-debugcon.iobase=0xe9` 两个参数，为保护模式下的调试输出提供基础设施。这比初始化串口简单得多，一个输出指令就能把字符写到日志文件里。

第三，`stage2.S` 末尾新增了独立的 `.gdt` section，定义了三个段描述符（null/code32/data32）和 `gdt_ptr` 伪描述符。每个描述符的 Access Byte 和 Flags 字段都经过了仔细设计，确保代码段可执行可读、数据段可读写、都覆盖完整的 4GB 地址空间。

下一篇我们就来做保护模式切换。整个过程涉及六个严格有序的步骤：`cli` 关中断、DS 清零、`lgdt` 加载 GDT 指针、CR0.PE 置 1、远跳转刷新 CS、以及 32 位环境下的段寄存器初始化和栈设置。每一步都有不可省略的理由，顺序不能调换，任何一步出错都会让你收获一个漂亮的三重故障。
