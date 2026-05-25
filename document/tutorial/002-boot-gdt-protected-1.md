---
title: 002-boot-gdt-protected-1 · GDT 与保护模式
---

# 从实模式到保护模式：GDT 与工程修正

> 标签：x86, 保护模式, GDT, 段描述符, bootloader
> 前置：[001 实模式启动](001-boot-real-mode-1.md)

## 前言：为什么我们不能再待在实模式里了

上一篇文章结束时，我们的 stage2 已经在实模式下跑通了 A20 开启、VESA 图形模式初始化、帧缓冲信息保存等一系列操作。看起来一切顺利——但实模式终究是个八十年代设计的笼子。1MB 的地址上限、没有权限检查、段地址左移四位加偏移的寻址方式，这些东西在现代操作系统里是完全不可用的。如果你想在保护模式下获得 4GB 的平坦地址空间，想用分页机制实现进程隔离，想让内核和用户态程序运行在不同的特权级，那你就必须先跨过这道门槛：从实模式切换到保护模式。

保护模式是 x86 处理器在 80286 时代引入的一种运行模式（后来被 80386 扩展到 32 位）。在保护模式下，段寄存器不再存储段基地址，而是存储一个叫段选择子（Segment Selector）的东西——它本质上是一个索引，指向一张叫做 GDT（Global Descriptor Table，全局描述符表）的数据结构中的某个条目。CPU 每次访问内存时，会根据选择子去 GDT 里查对应的段描述符，从中取出基地址、限长和权限信息，然后做地址翻译和权限校验。这个机制比实模式的"段寄存器左移四位"要复杂得多，但也强大得多。

现在我们要做的，就是在进入保护模式之前，先把工程里的几处隐患修干净，然后定义一张 GDT，为后续的模式切换做好准备。说实话，这几个"小修小补"看起来都不起眼，但每一个如果不动，后续都会让你收获一个漂亮的三重故障。

## 环境说明

我们仍然在 QEMU 上工作，工具链是 GAS（GNU Assembler）+ ld.bfd（GNU Linker），通过 CMake 构建。tag 002 的变更涉及五个文件：`boot/common/serial.S` 的汇编模式修正，`boot/CMakeLists.txt` 的 linker script 修正，`cmake/qemu.cmake` 新增 debugcon 支持，`boot/stage2.S` 新增 GDT 定义，以及对应的构建配置调整。所有代码都是纯汇编，没有引入 C 代码。

## 先把地基打好——汇编模式修正

我们先来看一个改动最小但影响最深远的修复：`boot/common/serial.S` 的汇编模式修正。

这个文件里全是实模式下使用的工具函数——`print_string`、`enable_a20`、VESA 相关的一系列操作。问题出在文件开头的一行汇编指示上：

```asm
.section .text
.code16                        // Generate 16-bit code (works with --32)
```

现在的代码已经改成了 `.code16`，但在 tag 002 之前，这里写的是 `.code16gcc`。两者都是告诉 GAS "接下来生成 16 位代码"，区别在于对指令宽度的态度完全不同。`.code16gcc` 是专门为 GCC 的 C 编译器输出设计的——它允许汇编器生成带 32 位操作数前缀（`0x66`）的指令，因为 GCC 在 `-m16` 模式下生成的汇编确实可能混合 16 位和 32 位操作。而 `.code16` 严格得多：它要求所有指令都是 16 位编码，你写 `push %ax` 它就老老实实生成 16 位的 push。

真正的问题在于：在 `.code16gcc` 下，裸写的 `push`/`pop`（不带宽度后缀）可能被汇编器按照自己的理解编码成 32 位的 `pushl` 而非 16 位的 `pushw`。在实模式下，栈操作宽度的不一致是致命的——你 push 了 4 字节出去，pop 只收回 2 字节，SP 寄存器就偏了。我们在调试时确实踩到了这个坑：SP 从 `0xFFFE` 跳到了 `0x000A`，栈帧完全错位，后续的 `ret` 跳到了一个完全无关的地址，CPU 三重故障重启。

修复方案是两步走。第一步，把 `.code16gcc` 改成 `.code16`。第二步，把所有裸写的 `push`/`pop` 加上显式宽度后缀 `pushw`/`popw`。我们来看 `print_string` 函数的修复：

```asm
.global print_string
.type print_string, @function
print_string:
    pushw %ax
    pushw %bx
    pushw %si
    cld

.loop:
    lodsb
    test %al, %al
    jz .done

    mov $0x0E, %ah
    xor %bx, %bx
    int $0x10
    jmp .loop

.done:
    popw %si
    popw %bx
    popw %ax
    ret
```

改动非常机械——每个 `push` 变 `pushw`，每个 `pop` 变 `popw`。整个 `serial.S` 里所有函数都做了同样的处理：`enable_a20`、`vesa_get_controller_info`、`vesa_get_mode_info`、`vesa_save_framebuffer_info`，总共二十多处。改动本身没有技术含量，但不改的后果非常严重。回头想一下，这个 bug 的可怕之处在于隐蔽性：在简单场景下（比如函数里只 push/pop 一次），32 位和 16 位 push 的结果可能恰好"看起来对"，因为实模式下的 32 位 push 只是把 SP 多减了 2 而已，数据本身还在栈上。但当调用链变深、多个函数嵌套 push/pop 时，栈偏移就累积起来了，最终在某次 `ret` 时跳到错误地址——而且这个错误发生的位置跟真正的 bug 位置可能隔了十万八千里。

从设计决策的角度来说，我们选择 `.code16` 而非 `.code16gcc` 的核心理由是确定性：`.code16` 要求所有指令宽度都是显式的，汇编器不会自作主张地加 32 位前缀。我们的 Bootloader 是纯手写汇编，每条指令的宽度应该由程序员明确控制，而不是让汇编器猜。如果你翻看 xv6 的 `bootasm.S`（[GitHub 链接](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S)），你会发现它从第一行起就是 `.code16`——这个选择在所有手写 16 位引导代码中是共识。

## 给 Stage2 一个正确的家——Linker Script 修正

接下来是 linker script 的修正。这个改动直接决定了 GDT 能不能被正确加载——而且说实话，如果你之前 linker script 写错了，症状和代码写错了完全一样（三重故障），但排查起来要困难十倍，因为你得先意识到"不是我代码写错了，是我给链接器的指示写错了"。

先看修改后的 linker script，它在 `boot/CMakeLists.txt` 中通过 `file(WRITE ...)` 生成：

```cmake
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/stage2.ld
"OUTPUT_FORMAT(\"elf32-i386\")
ENTRY(_start)
SECTIONS
{
    . = 0x8000;
    .text : {
        *(.text)
    }

    .gdt ALIGN(8) : {
        *(.gdt)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss*)
    }

    . = 0x10000;

    ASSERT(. <= 0x10000, \"stage2 too large! exceeds 32KB\")
    /DISCARD/ : {
        *(.comment*)
        *(.note*)
    }
}
")
```

这里有两个关键改动。

第一个是 `. = 0x8000`——location counter 从 `0x0` 改成了 `0x8000`。Linker script 里的这个 `.` 叫做 location counter（位置计数器），它决定了链接器为所有符号分配地址的起点。我们之前把它设成 `0x0`，意味着 `gdt_ptr` 里的 `gdt` 符号会被链接成类似 `0x0160` 这样的地址——看起来是个小数字，但 stage2 实际被 MBR 加载到了物理地址 `0x8000`（在 `mbr.S` 第 22 行，`STAGE2_LOAD_ADDR` 定义为 `0x8000`），运行时 `gdt` 的真实物理地址应该是 `0x8160`。于是 `lgdt gdt_ptr` 读到的 GDT base 是 `0x0160`，CPU 去这个地址读段描述符——读到的是一片空内存或者完全无关的数据，保护模式切换直接炸。把 base 改成 `0x8000` 之后，链接器会把 `gdt` 符号的地址算成 `0x8160`，与实际加载位置吻合。

你可能会问：为什么 stage2 被加载到 `0x8000` 而不是别的地址？这是在 `mbr.S` 里决定的——MBR 通过 BIOS INT 0x13 扩展读把 stage2 加载到 `0x0000:0x8000`。这个地址选择没有特殊含义，只要不跟 MBR 自身（`0x7C00`）、BIOS 数据区（`0x400`-`0x4FF`）、VESA 缓冲区（`0x6000`-`0x64FF`）冲突就行。但关键是：linker script 的 base 必须与这个加载地址严格一致，否则所有绝对地址引用全错。xv6 把它的 boot sector 链接在 `0x7C00`（`bootasm.S` 和 MBR 放在同一个位置），而 Cinux 的两阶段设计让 stage2 有了自己独立的加载地址和链接地址。

第二个改动是新增了 `.gdt ALIGN(8)` section。在之前的 linker script 里不存在这个 section——所有代码和数据都堆在 `.text` 和 `.data` 里。如果 GDT 定义放在 `.text` 中间，会有两个问题：一是 GDT 可能不被 8 字节对齐（Intel SDM Vol.3A §3.5.1 建议描述符表 8 字节对齐以提高性能），二是 GDT 数据会夹杂在可执行代码中间，影响 `.text` 的 cache 局部性。单独拎出来放到 `.gdt` section，用 `ALIGN(8)` 保证对齐，干净又安全。

整理一下改动后的完整布局：`.text` 在最前面（从 `0x8000` 开始），`.gdt` 紧随其后并 8 字节对齐，然后是 `.rodata`、`.data` 和 `.bss`。最后的 `ASSERT(. <= 0x10000, ...)` 确保整个 stage2 不超过 32KB——这是 MBR 分配给 stage2 的空间上限。

## 换一种方式说话——Debug Console

进入保护模式之后，有一个很现实的问题马上会摆在我们面前：BIOS INT 10h 不能用了。保护模式下没有 BIOS 中断服务，你没法再通过 `int $0x10` 往屏幕上打字。我们需要一个新的调试输出手段，而且这个手段最好不需要初始化任何硬件——因为我们正处于一个脆弱的过渡期，任何复杂的初始化都可能引入新的 bug。

QEMU 提供了一个极其方便的 debug console（调试控制台），对应 I/O 端口 `0xE9`：向这个端口写一个字节，QEMU 就会把字符输出到宿主机的终端或文件。不需要初始化串口，不需要配置波特率，一行 `outb` 搞定。这个功能最早是 Bochs 仿真器引入的（所以有时候你会看到它被称为 "Bochs debug port"），QEMU 兼容了这个约定，OSDev 社区也把它当成了事实标准。

我们在 `cmake/qemu.cmake` 中把它加到了 `QEMU_COMMON_FLAGS`：

```cmake
set(QEMU_COMMON_FLAGS
    -m ${QEMU_MEMORY}
    -serial stdio
    -no-reboot
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
    ${QEMU_ACCEL}
    ${QEMU_DISPLAY}
    -usb -device usb-tablet
)
```

`-debugcon file:debug.log` 告诉 QEMU 把 debug console 的输出写到 `debug.log` 文件。我们选择写文件而不是 `stdio`，是因为 `stdio` 已经被串口（`-serial stdio`）占用了，混在一起不好分辨。`-global isa-debugcon.iobase=0xe9` 把 debug console 的 I/O 端口设为 `0xE9`。

使用方式极其简单。在保护模式下的代码里只需要两条指令：

```asm
    movb $0x50, %al               // 'P' 的 ASCII 码
    outb %al, $0xE9               // 写到 debug port
```

执行后，`debug.log` 里就会出现字符 `P`。这是我们验证保护模式切换成功的标志——如果看到了 `P`，说明 CPU 成功执行了 32 位代码，段寄存器重载正确，`outb` 指令在保护模式下工作正常。在后续进入 long mode 时，我们同样用 debugcon 输出 `L` 来验证 64 位模式的切换成功。

## 保护模式的钥匙——GDT 详解

现在来看本篇最核心的部分——GDT 的定义。GDT 是保护模式下内存寻址的基础设施，你可以把它理解为一张"段定义手册"：段寄存器里存的不再是一个简单的段地址，而是一个段选择子——一个指向 GDT 条目的索引。CPU 每次做内存访问时，会拿着选择子去 GDT 里查对应的段描述符，从中取出基地址和限长，然后才能算出真正的线性地址。

GDT 的定义在 `stage2.S` 的末尾，位于独立的 `.gdt` section 中：

```asm
.section .gdt,"a"
.align 8                          // Align to 8 bytes (2^3)

gdt:
gdt_null:
    .quad 0                        // Null descriptor (required)

gdt_code:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0
    .byte 0x00                     // Base 23:16
    .byte 0x9A                     // Present, DPL=0, Code, Executable, Readable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_data:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0
    .byte 0x00                     // Base 23:16
    .byte 0x92                     // Present, DPL=0, Data, Writable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_end:

gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // Linear address of GDT base
```

我们逐段拆解。

首先是 `.section .gdt,"a"`——把 GDT 放到独立的 `.gdt` section，`"a"` 标记表示 allocatable（可分配），这是段属性的标准写法。`.align 8` 保证 GDT 起始地址是 8 字节对齐的。Intel SDM Vol.3A §3.5.1 并没有严格要求 GDT 必须 8 字节对齐（只需要字节对齐就行），但对齐访问性能更好，而且描述符本身就是 8 字节的，对齐后每个描述符的起始地址都是整齐的。

### Null 描述符

`gdt_null` 是 8 字节全零。Intel 规定 GDT 的第一个条目（索引 0）必须是 null descriptor，而且 CPU 硬件会强制检查：任何试图用选择子 `0x00` 访问内存的操作都会触发 General Protection Fault（#GP）。这个设计的目的是让"忘记初始化段寄存器"这种编程错误能被立即捕获，而不是静默地读到随机数据。这是一个非常典型的硬件辅助调试设计——x86 架构里有很多类似的"帮你踩刹车"的机制。

### Code32 描述符与段描述符的 8 字节布局

`gdt_code` 定义了一个 32 位代码段。在拆解之前，我们需要先理解 Intel 段描述符的 8 字节布局——这东西是 80286 时代的设计遗产，字段被拆散在不同的位置，初看非常不直观。Intel SDM Vol.3A §3.4.5（Figure 3-8）给出了完整的格式：

```
段描述符 8 字节布局（从低地址到高地址）

Byte 0-1:  Limit 15:0         段限长的低 16 位
Byte 2-3:  Base 15:0          段基地址的低 16 位
Byte 4:    Base 23:16         段基地址的中间 8 位
Byte 5:    Access Byte        访问权限字节
Byte 6:    Flags + Limit 19:16   高 4 位标志 + 段限长的高 4 位
Byte 7:    Base 31:24         段基地址的高 8 位
```

Base 被拆成了三段（Byte 2-3、Byte 4、Byte 7），Limit 被拆成了两段（Byte 0-1、Byte 6 的低 4 位）。这种奇怪的排列方式是为了向后兼容 80286——286 的段描述符只有 6 字节，386 扩展到了 8 字节，新增的字段被塞进了空隙里。

我们的 Code32 描述符的字节值是 `FF FF 00 00 00 9A CF 00`，逐字段展开：

**Access Byte `0x9A` 的逐位分解：**

```
0x9A = 1001 1010 b

Bit 7   (P=1)    : Present，段在内存中存在
Bit 6-5 (DPL=00) : Descriptor Privilege Level = 0（最高特权级 Ring 0）
Bit 4   (S=1)    : Segment descriptor（不是 system descriptor）
Bit 3   (E=1)    : Executable，这是一个代码段
Bit 2   (DC=0)   : Direction/Conforming，向上生长 / 非一致代码段
Bit 1   (RW=1)   : Readable，代码段可读（允许读取段内数据）
Bit 0   (A=0)    : Accessed，CPU 访问时自动置 1，我们初始化为 0
```

这里有几个值得细说的点。首先是 P（Present）位——它告诉 CPU 这个段描述符是否有效。如果 P=0，任何试图使用这个描述符的内存访问都会触发段不存在异常（#NP）。然后是 DPL（Descriptor Privilege Level）——它定义了这个段的特权级，00 表示 Ring 0（最高特权），这也是内核代码运行在的级别。S 位区分的是"代码/数据段描述符"和"系统段描述符"（TSS、LDT 等），我们这里定义的是代码段，所以 S=1。E 位表示这是一个可执行的代码段（而不是数据段），RW 位在代码段上下文中表示"可读"——是的，代码段默认是不可读的，你需要显式设置 RW=1 才能在代码段内做数据读取（比如 position-independent code 中的数据引用）。

**Flags Byte `0xCF` 的逐位分解：**

```
0xCF = 1100 1111 b

Bit 7   (G=1)    : Granularity = 4KB（Limit 的单位是 4KB 页而非字节）
Bit 6   (D/B=1)  : Default operation size = 32 位
Bit 5   (L=0)    : Not 64-bit code（64 位模式用另一个描述符）
Bit 4   (0)      : Reserved，必须为 0
Bit 3-0 (0xF)    : Limit 19:16 = 0xF
```

G 位（Granularity）决定了 Limit 的单位。G=0 时，Limit 以字节为单位；G=1 时，Limit 以 4KB 页为单位。我们的 Limit 值是 `0xFFFFF`（20 位全 1），配合 G=1，实际覆盖的地址范围是 `0xFFFFF * 4096 = 4GB - 1`——也就是整个 32 位地址空间。D/B 位决定了这个段的默认操作数宽度——D/B=1 表示 32 位，这意味着在保护模式下 `push`/`pop`/`call` 的默认宽度都是 32 位。L 位是后来为 64 位长模式添加的——当 L=1 且 D/B=0 时，表示这是一个 64 位代码段。

所以这个描述符定义的是：基地址 `0x00000000`，限长覆盖整个 4GB 地址空间，32 位代码段，Ring 0 特权级，可读可执行。

### Data32 描述符

`gdt_data` 和 Code32 几乎一样，唯一区别是 Access Byte 从 `0x9A` 变成了 `0x92`：

```
0x92 = 1001 0010 b

Bit 7   (P=1)    : Present
Bit 6-5 (DPL=00) : Ring 0
Bit 4   (S=1)    : Segment descriptor
Bit 3   (E=0)    : Not executable，这是一个数据段
Bit 2   (DC=0)   : Direction，向上生长
Bit 1   (RW=1)   : Writable，数据段可写
Bit 0   (A=0)    : Accessed
```

数据段不需要可执行权限（E=0），但需要可写权限（RW=1）——栈和数据都在这个段里。Flags 是一样的 `0xCF`：4KB 粒度，32 位，Limit 覆盖 4GB。

你会发现两个描述符的 Base 都是 `0x00000000`，Limit 都是 4GB——整个地址空间被完全平铺，没有任何分段隔离。这就是所谓的 Flat Memory Model（平坦内存模型），我们后面会在设计对比部分详细聊为什么这么做。

### gdt_ptr——GDTR 加载结构

`gdt_ptr` 是 `lgdt` 指令的参数，格式是 6 字节：前 2 字节是 GDT 的 limit（总大小减 1），后 4 字节是 GDT 的线性基地址。Intel SDM Vol.3A §2.4.1 详细描述了 GDTR 寄存器的结构——在 32 位模式下，GDTR 是一个 48 位寄存器，低 16 位存 limit，高 32 位存 base。

`gdt_end - gdt - 1` 由汇编器在编译时算出——三个描述符各 8 字节共 24 字节，减 1 得到 `0x17`（23）。Limit 之所以要减 1，是因为 GDT 的最大尺寸是 65536 字节（8192 个条目），而 limit 字段只有 16 位，能表示的最大值是 65535，所以约定是"GDT 的最后一个有效字节地址"，也就是总大小减 1。

`.long gdt` 是一个 32 位绝对地址引用，链接器会根据 linker script 的 base address `0x8000` 加上 `.gdt` section 的偏移量算出正确的物理地址。

这里有一个非常关键的点需要注意：`lgdt` 指令在实模式下执行时，CPU 计算物理地址的方式是 `DS * 16 + offset`。所以我们在 `stage2.S` 中调用 `lgdt gdt_ptr` 之前，必须先确保 `%ds` 为 0：

```asm
    movw $0, %ax                  // Clear AX
    movw %ax, %ds                 // Set DS = 0 (required for lgdt in real mode)
    lgdt gdt_ptr                  // Load GDTR from absolute address
```

如果 `%ds` 不为 0（比如还是之前 VESA 操作时设的某个段值），那 `lgdt` 实际读到的 `gdt_ptr` 地址就会偏移 `DS * 16`，读到的 GDT base 地址就是错的，后续进入保护模式时 CPU 加载的段描述符全是垃圾数据——又一个经典的三重故障来源。这个坑在 OSDev Wiki 的 [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial) 中也有明确提醒，属于实模式下地址计算的常规陷阱。

## 别人是怎么做的——设计对比

聊完了我们自己的实现，现在退后一步，看看其他项目是怎么处理 GDT 和保护模式切换的。这部分不是简单的"看人家怎么做的"式罗列，而是想通过对比来理解为什么不同的项目做了不同的权衡。

### xv6：教科书级的极简主义

xv6 的 `bootasm.S`（[GitHub 链接](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S)）是 MIT 6.828 课程的教学操作系统，它的 bootloader 把整个 real-to-protected mode 切换塞进了单个 512 字节的引导扇区里。GDT 定义和 Cinux 几乎一模一样——null + code(flat) + data(flat)——只不过它用了宏 `SEG_ASM` 来生成描述符，展开后和我们手写的 `.word`/`.byte` 完全等价。

但 xv6 和 Cinux 的工程架构差异很大。xv6 没有独立的 stage2——它把模式切换、A20 开启、内核加载全部放在 512 字节里。这意味着 xv6 不能做 VESA 图形初始化（没有空间），也不能做复杂的错误处理（没有空间打印调试信息）。A20 开启方式也不同：xv6 用键盘控制器 8042（端口 0x64/0x60，所谓的 "fast A20" 方式），而 Cinux 用 BIOS INT 0x15 AX=0x2401。两种方式各有优劣——8042 方式更快（不需要 BIOS 调用的开销），但兼容性稍差（某些虚拟机和旧硬件上可能不支持）；BIOS 方式更可靠，但需要依赖 BIOS 实现。

xv6 的栈设置也不同于 Cinux：xv6 在保护模式切换后把栈指针设为 `0x7C00`（boot sector 自身的位置，向下增长），而 Cinux 使用 `0x90000`。xv6 的选择是合理的——在单扇区架构下，boot sector 之上的内存都可以用作栈空间，没有必要单独选一个远处的地址。但 Cinux 的两阶段设计让 stage2 的代码和数据占据 `0x8000` 开始的区域，使用 `0x90000` 作为栈顶可以避免与代码区域冲突。

另外一点值得注意：xv6 使用 `.code16` 而非 `.code16gcc`，这和我们修正后的选择一致。但 xv6 从一开始就用了 `.code16`——它不需要经历"踩坑后修正"的过程，因为 xv6 的代码量小，而且一开始就是按照手写汇编的标准来写的。Cinux 早期使用 `.code16gcc` 可能是因为项目在初始化时更关注功能实现而非编码规范，后来在模式切换的调试过程中才发现了这个隐患。

### Linux：用 GRUB 跳过整个问题

Linux 的做法和 xv6、Cinux 都不一样——它根本不在内核里做 real-to-protected mode 切换。Linux 使用 GRUB（或其他符合 Multiboot 规范的 bootloader）来启动，GRUB 在把内核加载到内存之前就已经完成了 real mode -> protected mode -> 32-bit flat mode 的全部切换。内核被加载时，CPU 已经运行在 32 位保护模式下，内核的第一条指令就是 32 位代码。

这个选择背后的权衡很明确：Linux 内核的开发者认为 bootloader 的工作不应该由内核来承担。GRUB 是一个成熟的外部项目，它处理了各种硬件初始化的脏活累活——A20 开启、VESA 模式设置、保护模式切换、甚至可以从文件系统读取内核映像。Linux 内核的 `arch/x86/boot/` 目录下确实有 real mode 的 setup 代码，但那是为了支持 Linux 自己的 boot protocol，让非 GRUB 的 bootloader 也能启动 Linux。

对于 Cinux 来说，选择自己写 bootloader 而不是用 GRUB，是一个有意识的权衡。教学目的下的自写 bootloader 有两个好处：第一，你能看到完整的启动链条（从 BIOS POST 到实模式代码执行到保护模式切换），每一步都是显式的、可调试的；第二，你能精确控制内存布局，比如 Cinux 的 VESA 初始化就是在 GRUB 不会帮你做的范围之内。代价当然是更多的代码量和更多的调试时间——但如果你是在学习操作系统，这些恰恰是你想经历的。

### SerenityOS 和 ToaruOS：不同的路径

SerenityOS 使用 GRUB，和 Linux 一样跳过了 bootloader 的编写。ToaruOS 有自写 bootloader，但它直接从实模式跳到 long mode（64 位），跳过了 32 位保护模式这个中间阶段——这在现代系统上是完全可行的，因为 x86 的架构允许从实模式直接切换到 long mode（通过设置 CR0.PE 和 EFER.LME）。

Cinux 的分阶段设计（MBR -> Stage2 real mode -> Stage2 protected mode -> long mode）在教学上更有价值，因为它让读者能看到每一步的完整过程。如果直接跳到 long mode，你就错过了理解 GDT、段描述符、保护模式地址翻译这些基础知识的机会——而这些知识在调试 x86 相关的问题时是不可或缺的。

### Flat Memory Model：所有人的共识

上面这些项目虽然架构差异很大，但在 GDT 的设计上有一个共同的选择：Flat Memory Model。所有项目的代码段和数据段都是 Base=0、Limit=4GB，不做任何分段隔离。这不是巧合，而是现代操作系统设计的共识。

在 x86 的历史中，分段保护（Segmentation）是最早的内存保护机制——不同段有不同的 Base 和 Limit，程序不能越界访问。但在现代操作系统里，分段保护基本被废弃了，取而代之的是分页机制（Paging），它提供了更灵活的 4KB 粒度的内存保护。Linux、Windows、macOS 以及所有教学 OS 都使用 Flat Memory Model：GDT 里代码和数据都覆盖整个地址空间，Base 为 0，不做任何分段隔离，真正的内存隔离交给后续的页表实现。

我们选择 Flat Model 的原因有三个。第一，这是行业标准——如果你想参考任何现成 OS 的代码，它们都在用 Flat Model，保持一致可以减少认知负担。第二，在 Bootloader 阶段做分段保护没有意义——此时系统里只有一个"程序"在跑，就是 Bootloader 本身，没有隔离的需求。第三，后续实现分页时会重新定义整个内存布局，GDT 只是保护模式的一个"入场券"，不需要承担太多职责。

## 收尾

tag 02 完成的修改可以总结为：把工程里的几处隐患修干净，定义一张 Flat Model 的 GDT，配好 debugcon 调试手段。这些改动本身不涉及模式切换的实际执行——那部分在 tag 002 的另一半（CR0.PE 设置、far jump、段寄存器重载）中完成。但如果没有这些基础工作，后续的模式切换根本无法正确执行。

验证方式很简单：在完成 tag 002 的全部修改后，执行 `make run`（或 `make run-debug`），检查 `debug.log` 文件。如果里面出现了字符 `P`（0x50），说明保护模式切换成功——CPU 正确执行了 32 位代码，段寄存器重载正确，debugcon 输出工作正常。如果什么都没出现或者 QEMU 反复重启，那大概率是上面提到的某个坑——`.code16gcc` 的栈宽度问题、linker script base 地址不匹配、或者 `lgdt` 前 DS 没有清零。

下一章我们会详细展开实模式到保护模式的完整切换流程：CLI 禁用中断、LGDT 加载 GDT、CR0.PE 设置、far jump 刷新流水线、段寄存器重载、栈切换——以及每一步如果做错了会发生什么。

## 参考资料

- Intel SDM Vol.3A §2.4.1 — Global Descriptor Table Register (GDTR)：`lgdt` 指令加载 6 字节（16 位 limit + 32 位 base）到 GDTR
- Intel SDM Vol.3A §3.4.5 — Segment Descriptors：8 字节描述符格式，Base/Limit/Access/Flags 字段的分散排列方式
- Intel SDM Vol.3A §3.5.1 — Segment Descriptor Tables：GDT 8 字节对齐建议
- Intel SDM Vol.3A §10.9.1 — Switching to Protected Mode：完整的 6 步切换流程
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table) — GDT 结构、段描述符字段、flat 模型配置
- OSDev Wiki: [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial) — 实践教程，real mode 下 lgdt 的注意事项
- OSDev Wiki: [Protected Mode](https://wiki.osdev.org/Protected_Mode) — 保护模式进入步骤
- xv6 bootasm.S: [https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S)
- Linux Boot Protocol: [https://www.kernel.org/doc/html/latest/arch/x86/boot.html](https://www.kernel.org/doc/html/latest/arch/x86/boot.html)
