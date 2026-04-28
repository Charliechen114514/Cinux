# 从 BIOS 到 MBR：x86 实模式下那 512 字节的求生之旅

> 标签：x86, 实模式, MBR, bootloader, GNU AS, AT&T 语法
> 前置：[000 环境与工具链](000-env-toolchain-1.md)

## 前言

说实话，我琢磨写操作系统这事儿有好几年了。每次看到那些所谓的"操作系统教程"一上来就直接 GRUB 跳转保护模式，我心里总觉得少了点什么——BIOS 把 MBR 加载到 0x7C00 这一段，x86 几十年遗产堆出来的实模式引导，才是整个启动链里最让人血压飙升、也最有收获的部分。你被扔到一个只有 512 字节的荒岛上，没有标准库、没有虚拟内存、连段寄存器都可能不靠谱，每一行汇编都得精打细算。

上一章我们搭好了工具链，CMake 能编译一个什么都不干的 MBR stub，QEMU 启动后打印一个字符然后停机。现在我们要让这个 stub 变成真正的引导程序——在屏幕上打出 `Cinux Booting...`，从磁盘读取第二阶段引导程序，然后跳转过去。这一章只覆盖 MBR 部分；Stage2 的 A20 和 VESA 初始化放在下一章。

完成这两章后，你会在 QEMU 里看到一个完整的实模式引导流程：文本消息依次出现，然后屏幕一黑——切换到了 1024×768 的图形模式。

## 环境说明

我们的实验环境是 WSL2 上的 Ubuntu，QEMU 7.0+ 作为虚拟机。编译用 GNU AS（AT&T 语法），整个构建链是 CMake + GCC + objcopy。这里有一个很多人会忽略的要点：虽然我们跑在 x86_64 机器上，但 MBR 代码是 16 位实模式的，所以编译选项用 `-Wa,--32`（32 位目标文件格式）配合链接选项 `-Wl,-m,elf_i386`（32 位 ELF），代码内部用 `.code16` 标注为 16 位指令。验证方式就是 QEMU 的图形窗口——看屏幕上有没有出现我们打印的字。

## 起点——BIOS 把接力棒交给我们

按一下电源键，CPU 从物理地址 0xFFFF0 开始执行 BIOS ROM 里的代码。根据 Intel SDM Vol.3A §9.1 的描述，此刻处理器处于实模式：CR0 的 PE 位为 0，没有内存保护，没有分页，段寄存器直接参与物理地址计算，公式是 `段 × 16 + 偏移`。BIOS 做完 POST 自检后，把启动盘的第一个扇区读到 0x7C00，检查最后两个字节是不是 `0x55 0xAA`，然后跳过去执行——这就是我们的 MBR。

OSDev Wiki 的 MBR 页面记录了一个必须注意的事实：BIOS 跳转时 CS:IP 可能是 `0x0000:0x7C00`，也可能是 `0x07C0:0x0000`，取决于 BIOS 实现。其他所有寄存器——DS、ES、SS、SP——的值都是未定义的。DL 寄存器是个例外：BIOS 会把启动盘号放在里面（0x00=软驱，0x80=硬盘）。这就是我们拿到接力棒时的全部"已知条件"。

```
BIOS → MBR 的初始环境：
  CS:IP = 0x0000:0x7C00 或 0x07C0:0x0000  ← 不确定！
  DL    = 启动盘号                          ← 唯一可靠的值
  其他  = 全部未定义                         ← 别信任何寄存器
```

## 第一步——规范化 CS，搞定段寄存器和栈

我们现在要做的是让 MBR 在一个确定的环境中开始工作。打开 `boot/mbr.S`，第一段代码是这样的：

```asm
_start:
    ljmp $0, $real_start

real_start:
    cli
    xorw %ax, %ax
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    cld
    movw $STACK_BASE_ADDR, %sp
    sti
    movb %dl, boot_drive
```

`ljmp $0, $real_start` 这条远跳转是整个 MBR 里最容易被忽略、但最致命的一行。它同时修改 CS 和 IP——CS 被强制设为 0，IP 设为 `real_start` 的链接地址。没有它，某些 BIOS 上 CS 不为 0，后面所有基于段寄存器的地址计算都会算错。

跳转之后立刻 `cli` 关中断——这不是可选的。接下来要修改 SS 和 SP，如果这个过程中发生了中断，中断处理程序会用一个半成品的栈，后果完全不可预测。把所有段寄存器设成和 CS 一样（都是 0），确保所有内存访问使用统一的地址模型：段=0 时，物理地址就等于偏移量。`cld` 清除方向标志，让后续 `lodsb` 字符串操作正向递增 SI——这个标志位的状态也是未知的，不清零的话字符串会反向读取。

栈指针设到 0x7000，在 MBR 代码（0x7C00）下方 3KB 处向下增长。这里我踩过一个坑：最初把栈放在 0x7B00（紧贴 MBR），结果 BIOS 自己也会用栈，加上我们的 push 操作，直接踩到了 MBR 代码区域。改成 0x7000 之后留出 3KB 安全距离，再也没有这个问题。栈设好后 `sti` 开中断，因为后面要调用 BIOS 服务。最后把 DL 里的启动盘号存到 `boot_drive` 变量——读磁盘时必须用到它，而 BIOS 中断调用会修改 DX 寄存器。

### 其他 OS 怎么做

这一点值得对比一下。xv6 的 `bootasm.S` 也做了完全相同的初始化流程——远跳转规范化 CS、清零段寄存器、设置栈——说明这是 x86 实模式引导的"标准开局"。Linux 的 boot protocol 则更进一步：内核自带一段实模式 setup 代码（`arch/x86/boot/header.S`），外部 bootloader（GRUB 等）加载内核后会跳到这段代码，它负责做类似的初始化但还要处理版本协商、E820 内存映射查询等。SerenityOS 走的是完全不同的路——直接用 GRUB/Limine 的 multiboot 协议跳过实模式，内核入口就是保护模式。Cinux 选择自己写 MBR，目的是让读者理解从 BIOS 到内核的完整链条，而不是用 bootloader 帮你跳过最关键的部分。

## 第二步——在屏幕上打印点什么

我们先实现一个极简的字符串输出函数。MBR 的空间太紧张了，不能链接外部文件，所以我们在文件内部定义一个精简版：

```asm
print_string_mbr:
    cld
._loop:
    lodsb
    test %al, %al
    jz ._done
    mov $0x0E, %ah
    int $0x10
    jmp ._loop
._done:
    ret
```

`lodsb` 是一条非常方便的指令——从 DS:SI 读一个字节到 AL，然后 SI 自动加 1。正好适合遍历 null 结尾的字符串。Intel SDM Vol.1 §3.3.4 提到实模式下 DS:SI 是数据访问的标准方式，`lodsb` 直接利用了这个约定。`INT 0x10 AH=0x0E` 是 BIOS 的 teletype 输出函数，在当前光标位置打印 AL 中的字符，自动处理换行回车。

你会发现这个版本没有 push/pop 来保存寄存器——这是故意的。MBR 里调用打印函数的次数很少，调用者知道哪些寄存器会被修改，不需要函数自己保存。省掉的这几条指令在 512 字节的限制下非常珍贵。不过后面 Stage2 里的完整版 `print_string`（在 common/serial.S 中）是会保存寄存器的，因为调用更频繁，调用者不总能预测哪些寄存器会被破坏。

这里有一个真正的坑：BIOS INT 调用会污染寄存器——不只修改返回值相关的寄存器，DS、ES、FLAGS、BX、SI 都可能被改掉。如果你的代码在调用 BIOS 前后依赖这些寄存器的值，必须自己保存。这个坑我在调试时花了好半天——print 之后正常，下一个函数突然飞了，最后发现是 BX 被 INT 0x10 改掉了。

## 第三步——从磁盘读取 Stage2

MBR 只有 512 字节，还得减去签名 2 字节和分区表的空间。这点空间塞个字符串输出和磁盘读取就差不多了，更复杂的功能（A20、VESA）必须放到 Stage2 里。Stage2 从磁盘的 LBA 1 开始（紧接 MBR 之后），我们用 BIOS 的扩展读盘接口把它读进来：

```asm
load_stage2:
    movw $DAP_STORE_ADDR, %si
    movb $DAP_SIZE, (%si)
    movw $STAGE2_SECTORS, 2(%si)
    movw $STAGE2_LOAD_ADDR, 4(%si)
    movw $0, 6(%si)
    movl $STAGE2_LBA, 8(%si)
    movl $0, 12(%si)
    movb boot_drive, %dl
    movw $0x4200, %ax
    int $0x13
    jc disk_error
    ret
```

DAP（Disk Address Packet）是 BIOS 扩展磁盘读取用的 16 字节数据结构。OSDev Wiki 的 INT 13h 页面详细描述了它的布局：偏移 0 是结构大小（固定 0x10），偏移 2 是扇区数（15 个扇区 = 7.5KB），偏移 4 和 6 是目标缓冲区地址（0x0000:0x8000），偏移 8-15 是 64 位 LBA 起始号（低 32 位=1，高 32 位=0）。我们把 DAP 构造在 0x7B00（紧贴 MBR 下方），设置好参数后调用 `INT 0x13 AH=0x42`——如果成功 CF=0，Stage2 的代码已经在 0x8000 等着我们了。

读取成功后，打印启动消息然后跳转：

```asm
    movw $msg_booting, %si
    call print_string_mbr
    ljmp $STAGE2_LOAD_ADDR >> 4, $0
```

`ljmp $0x0800, $0` 设置 CS=0x0800，IP=0。物理地址 = 0x0800 × 16 + 0 = 0x8000，正好是 Stage2 的加载位置。注意这里不是 `ljmp $0, $0x8000`——用 CS=0x0800 的好处是 Stage2 的链接脚本可以从地址 0 开始，所有偏移都是段内偏移，更简洁。

### MBR 超 512 字节的大坑

这件事值得单独拿出来说。最初我把 mbr.S 和 common/serial.S 放在一起链接——想法很简单，共享 print_string 函数嘛。结果代码偶尔能跑偶尔崩溃，`call` 跳到随机地址。排查了半天发现：链接后 .text 段超过了 512 字节，BIOS 只加载了第一个扇区，common.S 的代码根本没被读进内存，`call` 跳到的是未初始化的内存区域。

解决方法是 MBR 完全自包含——不链接任何外部文件，自带精简版的 print_string_mbr。这就是为什么你会看到两个版本的字符串输出函数：一个在 MBR 里（精简版），一个在 common/serial.S 里（完整版，带寄存器保存）。

### 设计对比：Cinux vs xv6 vs Linux

xv6 的 `bootasm.S` 和 `bootmain.c` 采用类似的"MBR + 第二阶段"结构，但 xv6 的第二阶段直接用 C 语言编写（bootmain.c），通过 IDE 端口 I/O 读取磁盘（不使用 BIOS INT 13h），然后加载 ELF 格式的内核。这种方式更直接但只能在支持 IDE 的硬件上工作。Linux 的 boot protocol 更加复杂——内核自身包含一段实模式 setup 代码，外部 bootloader 加载带版本化协议头的 bzImage，setup 代码负责 E820 内存查询、A20 开启、视频模式设置，然后解压保护模式内核。Cinux 的方案更接近 xv6 的简洁性，但保留了 BIOS 调用（而非直接端口 I/O），因为 BIOS 在各种虚拟环境下兼容性更好。

## MBR 的最后两个字节

```asm
msg_booting:
    .asciz "Cinux Booting...\r\n"

msg_disk_error:
    .asciz "Disk: Failed to load stage2!\r\n"

boot_drive:
    .byte 0

.org 510
.word 0xAA55
```

数据段紧跟在代码后面——字符串、错误消息、启动盘号变量。`.org 510` 让当前位置跳到偏移 510，中间用 0 填充。`.word 0xAA55` 在小端序下写入 `55 AA`。Intel SDM 和 OSDev Wiki 都明确指出：BIOS 检查这两个字节，不是 55 AA 就拒绝执行。没有商量余地。

## 验证

构建并运行：

```
cmake -B build && cmake --build build
qemu-system-x86_64 -drive format=raw,file=build/cinux.img
```

如果一切正确，QEMU 屏幕上会出现 `Cinux Booting...`，然后 Stage2 被跳转过去（此刻 Stage2 可能还没写好，跳转后行为不确定——这是正常的）。如果什么都没显示就重启循环了，检查 0xAA55 签名是否正确。

GDB 调试：

```
qemu-system-x86_64 -S -gdb tcp::1234 -drive format=raw,file=build/cinux.img &
gdb
  (gdb) target remote :1234
  (gdb) set architecture i8086
  (gdb) break *0x7c00
  (gdb) continue
```

## 收尾

到这里，MBR 的 512 字节已经全部用上了——段寄存器初始化、字符串输出、DAP 磁盘读取、远跳转到 Stage2。这个设计虽然简洁，但它覆盖了 x86 实模式引导的所有核心概念：段:偏移寻址、BIOS 中断调用、DAP 磁盘操作、MBR 签名。

下一章我们要让 Stage2 干点正事——开启 A20 地址线突破 1MB 限制，通过 VESA BIOS 设置图形模式，保存帧缓冲信息给后续的内核用。事情到这里还没完，真正的图形世界在后面。

## 参考资料

- Intel SDM: Vol.3A §9.1 — Processor State After Reset (CS=0xF000, EIP=0xFFF0, 实模式初始状态)
- Intel SDM: Vol.1 §3.3.4 — Real-Address Mode (段:偏移寻址公式)
- OSDev Wiki: [MBR (x86)](https://wiki.osdev.org/MBR_(x86)) — MBR 格式、BIOS 初始环境
- OSDev Wiki: [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) — DAP 结构、扩展读盘
- xv6: `bootasm.S` — MIT xv6 的 MBR 引导实现
- Linux: [x86 boot protocol](https://www.kernel.org/doc/html/latest/x86/boot.html) — Linux 内核启动协议
