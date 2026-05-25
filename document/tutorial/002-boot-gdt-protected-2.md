---
title: 002-boot-gdt-protected-2 · GDT 与保护模式
---

# 跨越鸿沟：实模式到保护模式切换全流程

> 标签：x86, 保护模式, CR0, far jump, GDT, bootloader
> 前置：[002-1 GDT 与工程修正](002-boot-gdt-protected-1.md)

## 前言

上一篇我们把 GDT 定义好了、linker script 修正了、debugcon 配置到位了，整个舞台已经搭好，就差最后一幕：真正让 CPU 从实模式切换到保护模式。你可能觉得，既然 GDT 都已经放对位置了，设置一个位的事情能有多难？说实话，这一步是 x86 boot 过程中最容易翻车的环节。Intel 在设计这套切换流程时留了不少"必须这样做否则炸"的硬性要求，而且大部分要求踩了坑之后的表现都是同一个症状——三重故障（Triple Fault），CPU 直接复位，QEMU 窗口闪一下就没了，连给你看报错的机会都不给。

我们现在要做的事情是，从 `cli` 关中断开始，到 `outb 'P'` 验证输出结束，把整个保护模式切换流程拆开揉碎地讲一遍。每一步为什么必须存在、为什么在这个位置、如果省略或调换会发生什么，这些都会讲清楚。整个流程严格遵循 Intel SDM Vol.3A §10.9.1 给出的官方推荐步骤，容不得半点自由发挥的空间。

先别急，我们先回顾一下全貌。保护模式切换是一个六步严格有序的过程：CLI 禁用中断、DS 清零、LGDT 加载 GDT 指针、CR0.PE 置 1、远跳转刷新 CS、最后在 32 位环境下初始化段寄存器和栈。顺序不可调换，far jump 不可跳过，任何一步出错都会让你收获一个漂亮的三重故障。我们接下来按代码的实际顺序，逐段推进。

## 环境说明

我们仍然在 QEMU 上工作，工具链是 GAS（GNU Assembler）+ ld.bfd（GNU Linker），通过 CMake 构建。本篇聚焦的核心代码全部位于 `boot/stage2.S` 中，从大约第 124 行的 `cli` 开始，到第 164 行的 `outb %al, $0xE9` 结束。调试手段是 QEMU 的 debug console（端口 0xE9），配置方法在上一篇中已经讲解过。如果你想在 GDB 里单步跟踪，需要用 ELF 文件（`build/boot/stage2`）而非 bin 文件——这一点在踩坑记录中会反复提到。

## 第一步：关闭中断与清零 DS

在 VESA 帧缓冲信息保存完毕、E820 内存映射获取完成、内核已经从磁盘加载到内存之后，stage2 的实模式使命终于画上了句号。接下来要进入保护模式了，而第一步就是两条看起来平淡无奇的指令：

```asm
    cli                           // disable interrupts again
    // ============================================================
    // Switch to Protected Mode
    // ============================================================
    // IMPORTANT: lgdt in .code32 uses absolute address, but in real mode
    // CPU calculates physical address as DS*16 + offset. We must set DS=0
    // so that the address calculation is correct: 0*16 + 0x8160 = 0x8160
    movw $0, %ax                  // Clear AX
    movw %ax, %ds                 // Set DS = 0 (required for lgdt in real mode)
```

`cli` 指令清除 EFLAGS 寄存器中的 IF（Interrupt Flag）位，让 CPU 不再响应可屏蔽硬件中断。为什么要在这里关中断？原因很直接，但也容易被忽略：保护模式下的中断处理依赖 IDT（Interrupt Descriptor Table），而我们此时还没有设置 IDT。如果中断开着，任何一个硬件中断——定时器中断、键盘中断、随便什么——到达时，CPU 会尝试去 IDT 查找处理函数。但 IDT 还是一片空白，结果就是触发 General Protection Fault（#GP），而 #GP 本身也需要 IDT 来处理，于是又触发 Double Fault，Double Fault 还是找不到处理函数，最终变成 Triple Fault，CPU 直接复位。你可能会注意到 `cli` 在 stage2 一开始就执行过一次（在 `_start` 入口处），后来 `sti` 重新开了中断给 VESA 的 BIOS 调用和内核加载用。现在所有实模式操作全部结束，是时候再次关掉了。

紧接着 `cli` 之后，我们把 `%ds` 清零。这个操作看起来像是"打扫卫生"，但它和 `lgdt` 指令的地址计算方式直接相关，不做的话后面会出大问题。在实模式下，CPU 计算内存地址的公式是 `物理地址 = 段寄存器 × 16 + 偏移`。当我们写 `lgdt gdt_ptr` 时，`gdt_ptr` 是一个链接器算出的绝对地址——比如 `0x8160`——CPU 会用 `DS × 16 + 0x8160` 来计算实际要读的内存位置。如果此时 `DS` 还是之前操作时设的某个非零值（比如 `0x0060`，因为某些 BIOS 调用可能改过段寄存器），那实际读到的地址就变成了 `0x0060 × 16 + 0x8160 = 0x8760`——完全不是 `gdt_ptr` 的真正位置，读出来的 GDTR 内容是垃圾数据。

把 DS 清零后，地址计算变成 `0 × 16 + 0x8160 = 0x8160`，正好是 `gdt_ptr` 的真实物理地址。说实话，这个坑在调试时真的让人血压拉满——`lgdt` 本身不会报错，CPU 很乖地把 GDTR 加载了一个错误的 base 地址，然后 `ljmp` 时去 GDT 查描述符查到垃圾数据，直接三重故障。你甚至不知道是 `lgdt` 那一步出了问题还是后面的步骤出了问题，因为症状完全一样：QEMU 重启循环。我们在调试这个流程时确实踩过这个坑，花了大半个小时才定位到是 DS 没清零导致 lgdt 读到了错误地址。OSDev Wiki 的 [Protected Mode](https://wiki.osdev.org/Protected_Mode) 页面没有特别强调这一点，但 [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial) 中有提到 real mode 下地址计算的注意事项，属于那种"你知道了就没事，不知道的话怎么死都不知道"的坑。

## 第二步：让 CPU 知道 GDT 在哪——LGDT

DS 清零之后，现在可以安全地执行 `lgdt` 了：

```asm
    // 1. Load GDT pointer (with DS=0, physical address = 0x8160)
    lgdt gdt_ptr                  // Load GDTR from absolute address
```

就一行指令，但背后的 CPU 行为值得展开说说。`lgdt` 做的事情是把 `gdt_ptr` 指向的 6 字节数据加载到 CPU 内部的 GDTR（Global Descriptor Table Register）。GDTR 是一个 48 位寄存器，由两部分组成：16 位的 Limit（GDT 的大小减 1）和 32 位的 Base（GDT 的线性基地址），这个结构在 Intel SDM Vol.3A §2.4.1 中有详细描述。在上一篇里我们定义了 `gdt_ptr`：

```asm
gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1 = 23
    .long gdt                      // Linear address of GDT base (32-bit)
```

执行 `lgdt gdt_ptr` 后，CPU 内部的 GDTR 变成了 `Limit = 0x17`（23）、`Base = gdt 的链接地址`（大约在 `0x81xx` 左右，取决于 `.text` section 的大小）。但这里有一个非常重要的细节需要理解：`lgdt` 只是把数据搬进了 GDTR，它**不会验证 GDT 内容的正确性**。如果你的 GDT 里的描述符是乱写的——比如 Code 段的 Access Byte 写成 `0x00`——`lgdt` 不会报错，CPU 也不会触发任何异常。错误会在后续使用段选择子访问 GDT 条目时才暴露：`ljmp` 尝试加载 CS 时 CPU 去查描述符，发现 Present 位是 0，直接触发 #GP。

这意味着如果你的 GDT 定义有误（比如字段写反了、Limit 不够大、Access Byte 拼错了），`lgdt` 这一步是看不出问题的。你只会看到 CPU 在执行 `ljmp` 时突然三重故障，然后需要回头去检查 GDT 的每个字节是否正确。我们确实踩过这个坑——调试了半天才发现是 Access Byte 写错了一位。真正的坑在后面这句话：lgdt 的"静默失败"特性使得 GDT 相关的错误定位变得极其困难，因为你需要在出错点之前很远的地方找 bug。

## 第三步：拨动开关——CR0.PE

GDTR 就绪，接下来是整个流程中真正"拨动开关"的一步：

```asm
    // 2. Set PE bit in CR0
    movl %cr0, %eax               // Read CR0
    orb $0x1, %al                 // Set PE bit (bit 0)
    movl %eax, %cr0               // Write CR0
```

CR0 是 x86 的控制寄存器之一，其中包含了多个控制系统级行为的标志位。Intel SDM Vol.3A §10.1 给出了 CR0 的完整布局，我们关心的只有一个：bit 0，也叫 PE（Protection Enable）位。把它从 0 设成 1，CPU 就"标记"自己进入了保护模式——但请注意我的措辞是"标记"而不是"进入"，原因后面会解释。

CR0 的关键位布局大致如下：

```
CR0 (32-bit Control Register)
Bit 31    Bit 30    Bit 29              Bit 0
  PG        CD        NW     ...          PE
(Paging)  (Cache)  (NoWrite)         (Protection Enable)
```

PE 位是最低位（bit 0），值 `0x1`。你可能注意到我们用的是 `orb $0x1, %al` 而不是 `orl $0x1, %eax`。其实两种写法效果一样——都是把 bit 0 设成 1、其余位不变。用 `orb` 操作 `%al`（CR0 的低 8 位）只是编码更紧凑，省一个字节。`orb $0x1, %al` 对应的机器码是 `0C 01`（两个字节），而 `orl $0x1, %eax` 的机器码是 `83 C8 01`（三个字节）。在 Bootloader 里省一个字节感觉有点鸡肋，但这是一个好习惯——养成只改你需要的位的习惯，避免无意中改动其他控制位。

这里还有一个容易忽略的约束：`movl %cr0, %eax` 不能省——你不能直接对 CR0 做 `or` 操作。必须先读出来、修改、再写回去。这是因为 CR0 不能作为 ALU 指令的操作数，必须通过通用寄存器中转。这是 x86 ISA 的规定，没有什么高深的原因，就是架构限制。

现在我们来说说为什么我说 PE=1 只是"标记"而不是真正进入。执行 `movl %eax, %cr0` 把 PE 位设成 1 之后，CPU 从逻辑上已经切换到了保护模式，但从指令执行的实际效果来看，事情还没完。此时 CPU 的预取队列里可能还缓存着按 16 位解码的指令（因为我们一直都在 `.code16` 下），CS 寄存器的隐藏部分（Segment Cache）里存的还是实模式的段基址和属性。CPU 仍然在用旧的段信息翻译地址、用 16 位模式解码指令。要让保护模式真正生效，必须做下一步：远跳转。

## 第四步：不可省略的远跳转

这是整个切换流程中最不可省略的一步，也是最容易被初学者忽略的一步：

```asm
    // 3. Far jump - manually construct for correct 16-bit encoding
    // ljmp $0x08, $pm_entry in 16-bit format: ea <offset16> <seg16>
    ljmp $0x08, $pm_entry
```

`ljmp`（Long Jump，也叫 Far Jump）和普通的 `jmp` 不同——它同时修改 CS 和 EIP，而不是只修改 IP。它的操作数由两部分组成：一个段选择子（`$0x08`）和一个目标偏移（`$pm_entry`）。

为什么这个远跳转绝对不可省略？原因在于 x86 段寄存器的工作方式。段寄存器（CS、DS、ES 等）不是简单的 16 位寄存器——每个段寄存器内部都有一个"隐藏部分"（Hidden Part，也叫 Segment Cache），存储着当前段的 Base Address、Limit 和 Access Rights。这些隐藏信息是在段寄存器被加载时由 CPU 自动从 GDT 中读取并缓存的，后续所有的内存访问都直接使用缓存中的值，不会每次都去 GDT 查。在实模式下，CS 的隐藏部分存的是实模式的段属性（Base = CS × 16，Limit = 0xFFFF，16 位默认操作数大小）。当我们在上一步中设置了 CR0.PE = 1 之后，CPU 的"模式"已经变成了保护模式，但 CS 的隐藏缓存还是实模式的旧值——CPU 仍然在用旧的 Base 和旧的属性来翻译地址和解码指令。

远跳转 `ljmp $0x08, $pm_entry` 做了两件事。第一，它把 CS 加载为新的段选择子 `0x08`，CPU 会用 `0x08` 去查 GDTR 指向的 GDT，找到索引 1（`0x08 >> 3 = 1`）的描述符，也就是我们在上一篇中定义的 `gdt_code`——一个 Base=0、Limit=4GB、32 位代码段的描述符。第二，它把目标地址 `pm_entry` 加载到 EIP 中，同时 CPU 用新描述符的属性刷新了 CS 的隐藏缓存。这意味着从此刻起，CPU 开始按 32 位模式解码和执行指令。

如果你省掉了这个远跳转，直接在 `movl %eax, %cr0` 后面继续写 32 位指令，CPU 的预取队列里还残留着按 16 位模式解码的指令，而新写的指令又是按 32 位编码的——两种编码混在一起，CPU 解析出完全错误的指令，大概率直接执行到非法操作码，然后三重故障。这个坑非常经典，Intel SDM Vol.3A §10.9.1 明确要求：设置 PE 位之后必须紧跟一个远跳转操作，这不是建议，是硬性要求。

选择子 `0x08` 的含义值得仔细拆解。16 位的选择子由三个字段组成：高 13 位是 Index（索引），bit 2 是 TI（Table Indicator，0=GDT，1=LDT），低两位是 RPL（Request Privilege Level）。所以 `0x08 = 0000 0000 0000 1000 b`：Index = `0x08 >> 3 = 1`（GDT 中的第二个条目，因为第一个是 null descriptor），TI = 0（使用 GDT），RPL = 0（Ring 0）。这个选择子正好指向我们的 32 位代码段描述符 `gdt_code`。

这里还有一个关于指令编码的细节值得一提。代码注释里写了 `ljmp $0x08, $pm_entry in 16-bit format: ea <offset16> <seg16>`。因为我们此时还在 `.code16` 模式下，GAS 会把 `ljmp` 编码为 16 位远跳转——操作码 `EA` 后面跟 2 字节偏移 + 2 字节段选择子，总共 5 字节。但在保护模式下，`ljmp` 的编码是 `EA` 后面跟 4 字节偏移 + 2 字节段选择子，总共 7 字节。所以这里的 `pm_entry` 偏移会被截断为 16 位——这没问题，因为 stage2 的代码全部在 `0x8000`-`0x10000` 范围内，16 位偏移足够表示。远跳转之后，代码执行流进入 `pm_entry`，同时 `.code32` 指令告诉汇编器从这开始生成 32 位指令编码——CPU 和汇编器终于同步了。

总结一下：`ljmp` 让 CPU 用 32 位解码，`.code32` 让汇编器生成 32 位编码，两者缺一不可。如果你只写了 `.code32` 但没有做远跳转，汇编器生成的是 32 位编码但 CPU 还在 16 位模式执行，结果就是指令完全乱套。反过来如果你做了远跳转但忘了加 `.code32`，CPU 已经在 32 位模式了但汇编器还在生成 16 位编码，同样会出问题。这一对配合是整个切换流程中最精妙也最容易出错的地方。

## 第五步：新世界的大门——保护模式初始化

远跳转成功后，CPU 已经在保护模式下运行了。但此时除了 CS 之外的所有段寄存器（DS、ES、FS、GS、SS）还残留着实模式的值，需要全部重载为保护模式的段选择子。我们先来看代码：

```asm
// ============================================================
// Protected Mode Entry Point
// ============================================================
.code32                          // Now in 32-bit protected mode
pm_entry:
    // 4. Set up data segment registers
    movw $0x10, %ax               // Data selector value
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
```

`.code32` 指令切换汇编器的输出模式——从这行开始，所有指令按 32 位编码生成。注意 `.code32` 不是一个 CPU 指令，它不会出现在最终的二进制文件中；它只是告诉 GAS"接下来请用 32 位操作码编码"。我们在上一步的 `ljmp` 已经让 CPU 切换到了 32 位执行模式，`.code32` 确保汇编器生成的编码和 CPU 的解码方式一致。

接下来我们把 DS、ES、FS、GS、SS 全部设成 `0x10`。这个选择子的解码方式和 `0x08` 类似：`0x10 = 0000 0000 0001 0000 b`，Index = `0x10 >> 3 = 2`（GDT 中的第三个条目，即 `gdt_data`），TI = 0（使用 GDT），RPL = 0（Ring 0）。它指向我们定义的 32 位数据段描述符——Base=0、Limit=4GB、可读写。

为什么需要给每个段寄存器都加载一次？因为段寄存器的隐藏缓存不会因为你设置了 CR0.PE 就自动更新。在远跳转时我们只刷新了 CS，其余的段寄存器还保持着实模式的旧缓存。在保护模式下，任何通过这些段寄存器的内存访问都会使用隐藏缓存中的 Base 和 Limit 来翻译地址——如果缓存里还是实模式的旧值，内存访问就会出错。逐一重载每个段寄存器，CPU 就会用新的选择子去 GDT 查对应的描述符，然后把正确的 Base、Limit、Access Rights 写入隐藏缓存。你可能会注意到我们没有给 CS 再做一次 `movw`——因为远跳转 `ljmp $0x08, $pm_entry` 已经完成了 CS 的重载。DS、ES、FS、GS 这些数据段寄存器不能通过远跳转来刷新，必须用 `movw` 直接写入。

段寄存器全部就位后，接下来设置保护模式下的栈：

```asm
    // 5. Set up new stack
    movl $0x90000, %esp           // New stack in protected mode
```

`ESP` 设为 `0x90000`，意味着栈从 `0x90000` 向低地址方向增长。为什么选这个地址？几个原因叠在一起。首先，`0x90000` 在实模式可寻址范围内（1MB 以内），保护模式切换刚完成时 CPU 还没启用分页，访问的仍然是物理地址，用这个地址不会出问题。其次，它远离 stage2 的代码区域（`0x8000`-`0x10000`）、VESA 缓冲区（`0x6000`-`0x64FF`）和 E820 内存映射（`0x5000`-`0x5FFF`），不会冲突。再者，从 `0x90000` 向下增长到 `0x80000` 有整整 64KB 的空间，对 Bootloader 阶段来说绰绰有余。

你可能注意到了一个变化：在实模式阶段，我们用的是 `SS:SP = 0x9000:0xFFFE`，对应物理地址 `0x9000 × 16 + 0xFFFE = 0x9FFFE`。进入保护模式后，段寄存器的语义变了——SS 不再存储段地址，而是存储段选择子。我们把 SS 设成 `0x10`（数据段选择子），然后把 ESP 直接设成 `0x90000`。在 Flat Memory Model 下，SS 对应的数据段 Base 是 `0x00000000`，所以 `0x90000` 就是实际的物理栈顶地址。

事情到这里还没完。段寄存器重载了、栈设好了，但我们在保护模式下还没输出过任何东西——你怎么知道前面这些操作全都成功了？这就是最后一步验证输出的意义所在：

```asm
    // 6. Debug output: 'P' for Protected Mode
    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output
```

`0x50` 是字符 `P` 的 ASCII 码。`outb` 指令把 `%al` 的值写到 I/O 端口 `0xE9`。这个端口是 QEMU 的 debug console——在 `cmake/qemu.cmake` 里我们已经配置了 `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9`，所以写入 `0xE9` 的字节会被 QEMU 捕获并写入 `debug.log` 文件。

你可能会问：为什么不继续用之前的 `INT 0x10` BIOS 中断来输出字符？原因很简单但也很残酷——保护模式下 BIOS 中断不可用。BIOS 中断服务程序运行在实模式下，使用实模式的段地址翻译方式和中断向量表（IVT）。进入保护模式后，CPU 使用 GDT 中的描述符进行地址翻译，中断处理方式也完全变了——需要 IDT（Interrupt Descriptor Table）而不是 IVT。如果我们强行 `int $0x10`，CPU 会尝试在保护模式下处理这个中断，没有 IDT，结果就是 #GP，然后三重故障。所以我们不能再用任何 BIOS 服务，必须使用纯硬件 I/O 操作来输出调试信息。

debugcon 是最简单的选择：一条 `outb` 就够了，不需要初始化 UART、不需要设置波特率、不需要查询发送缓冲区是否为空。当然，debugcon 是 QEMU 专有的调试接口，真实硬件上不存在——但这在 Bootloader 开发阶段完全够用，后续进入 long mode 之后我们会切换到 COM1 串口作为正式的输出通道。

如果一切顺利，执行完 `outb %al, $0xE9` 之后，`debug.log` 文件里会出现一个 `P`。这个孤零零的字符就是我们的胜利旗帜——它意味着从 `cli` 到 `outb` 的整个保护模式切换流程全部正确执行，CPU 现在运行在 32 位保护模式下，段寄存器、栈、I/O 操作全部正常。

验证输出完成后，stage2 的保护模式阶段在这个 tag 里就结束了。我们还没有实现后续的 long mode 切换（那是 tag 003 的事），所以代码进入停机循环：

```asm
    // Should never reach here
    cli
.pm_halt:
    hlt
    jmp .pm_halt
```

`cli` 再次确认中断关闭——虽然前面已经 `cli` 过了，但这是防御性编程。`hlt` 让 CPU 进入 halt 状态，停止执行直到下一个外部中断到来。但因为中断已经关了，`hlt` 会一直停住。下面的 `jmp .pm_halt` 是额外的保险：万一 CPU 因为某种原因从 `hlt` 唤醒了（比如某些虚拟化场景），就跳回来再 `hlt` 一次，形成死循环。不过在 tag 004 之后，这里的代码会被替换为 `call setup_page_tables` 和 `call enter_long_mode`，继续往 long mode 推进。

## xv6 是怎么做的——对比分析

聊完了我们自己的切换流程，现在退后一步，看看 xv6 是怎么处理同一个问题的。这不是简单的"对照表"，而是想通过对比来理解不同工程约束下的不同选择。

xv6 的 `bootasm.S`（[GitHub 链接](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S)）把整个 real-to-protected mode 切换塞进了单个 512 字节的引导扇区里。核心流程和 Cinux 完全一致——`cli`、段寄存器清零、`lgdt`、CR0.PE、`ljmp`、段寄存器重载——毕竟 Intel SDM 的步骤就那么多，谁来了都得按这个顺序走。但细节上的差异很有意思。

首先是 GDT 定义。xv6 用宏 `SEG_ASM` 来生成描述符，展开后和 Cinux 手写的 `.word`/`.byte` 完全等价，都是 Flat Model（Base=0, Limit=4GB）。GDT 放在 `.p2align 2` 后面，和代码在同一个文件里，没有单独的 section。Cinux 则把 GDT 放在独立的 `.section .gdt,"a"` 中并 8 字节对齐，更干净但也更"重"——因为 Cinux 的 stage2 没有 512 字节的空间限制，有条件做更好的工程组织。

其次是 A20 开启方式。xv6 用键盘控制器 8042（端口 0x64/0x60，所谓的 "fast A20" 方式），而 Cinux 用 BIOS INT 0x15 AX=0x2401。8042 方式更快（不需要 BIOS 调用的开销），但兼容性稍差；BIOS 方式更可靠但需要依赖 BIOS 实现。对于 xv6 来说，8042 方式是合理的——它的目标环境是 QEMU 和 Bochs，这些仿真器对 8042 的支持很完善。

然后是栈的位置。这是差异最大的地方。xv6 在保护模式切换后把栈指针设为 `0x7C00`——也就是 boot sector 自身的位置，栈从这里向下增长。这意味着栈空间从 `0x7C00` 一直往下到 `0x0000`，理论上有 31KB。但这片区域里有中断向量表（`0x0000`-`0x03FF`）和 BIOS 数据区（`0x0400`-`0x04FF`），稍不留神就会覆盖。Cinux 使用 `0x90000`，远离所有这些敏感区域，64KB 的空间也很宽裕。这个差异的根源在于架构不同：xv6 的单扇区设计让它的代码只能从 `0x7C00` 开始，栈跟着放在这里是顺理成章的；Cinux 的两阶段设计让 stage2 从 `0x8000` 开始，栈可以选一个更独立的地址。

最后是切换后的动作。xv6 在 `start32` 里设置完段寄存器和栈之后，直接 `call bootmain` 进入 C 代码——开始从磁盘读取内核。Cinux 在切换后先 `outb 'P'` 到 debugcon 做验证输出，然后才继续后续的 long mode 切换。这个差异反映的是调试哲学：Cinux 在每个关键步骤后都插入了验证点（debugcon 输出），这样在出问题时可以精确定位是哪一步失败了；xv6 更"自信"，切完就直接往前走。

从 `.code16` 的使用来看，两者是一致的。xv6 从第一行起就是 `.code16`，Cinux 在 tag 002 修正后也改成了 `.code16`。这个选择在所有手写 16 位引导代码中是共识——`.code16gcc` 是给 C 编译器用的，手写汇编就应该用 `.code16`。

## 调试实战

保护模式切换的调试是 x86 内核开发中最让人头疼的环节之一，因为大部分错误的症状都是同一个：三重故障，CPU 复位，你什么都看不到。这里总结几种我们在调试过程中用过的手段，以及常见错误的排查思路。

### GDB 断点调试

如果你在 QEMU 中启动了 GDB stub（`-s -S` 参数），可以在保护模式切换的关键位置设置断点。最关键的两个断点是 `pm_entry` 和切换前的最后一条 16 位指令：

```gdb
(gdb) target remote :1234
(gdb) file build/boot/stage2
(gdb) b *pm_entry
(gdb) c
```

注意这里有一个非常重要的点：必须用 `file build/boot/stage2` 加载 ELF 文件，而不是 bin 文件。GDB 需要符号信息来解析断点地址，bin 文件里没有这些信息。我们早期调试时犯过一个低级错误——用 `file build/boot/stage2.bin` 加载纯二进制文件，结果 GDB 找不到 `pm_entry` 符号，断点设不上。另外，在实模式和保护模式切换前后的 GDB 体验也有差异：实模式下用 `info registers` 看到的是 16 位寄存器（IP、SP 等），切换后变成 32 位（EIP、ESP）。如果在切换之前就试图查看 EIP，GDB 可能报 `Invalid register` 错误——这不是 bug，而是 CPU 此时确实还在实模式。

### QEMU `-d int` 查异常

如果你在保护模式切换中遇到了三重故障，QEMU 的 `-d int` 选项是定位问题的利器：

```bash
qemu-system-x86_64 -d int -no-reboot ...
```

`-d int` 会让 QEMU 在每次中断/异常发生时打印详细信息，包括异常类型、错误码、触发时的 CS:EIP。`-no-reboot` 防止 CPU 在三重故障后自动重启，这样你可以看到完整的异常链。典型的输出可能像这样：

```
check_exception old: 0xffffffff new 0xd
     0: v=0d ecode=0x00000000 eip=0x0000816c cs=0x0008
```

这告诉你 CPU 在地址 `0x816c` 处触发了一个 General Protection Fault（异常号 0x0D），CS=0x0008 说明此时已经在保护模式下（使用了选择子 0x08）。如果异常发生在 `ljmp` 之前，CS 可能还是实模式的值，说明 GDT 内容有问题。

### debugcon 作为最简验证

我们在代码中使用的 `outb 'P', 0xE9` 是最简单的验证手段。但你可以在切换流程的每一步后面都插一个不同的字符输出，形成一条"面包屑"式的调试链：

```asm
    movb $'1', %al; outb %al, $0xE9    // 1: cli done
    movw $0, %ds
    movb $'2', %al; outb %al, $0xE9    // 2: DS=0 done
    lgdt gdt_ptr
    movb $'3', %al; outb %al, $0xE9    // 3: lgdt done
    // ...
```

如果 `debug.log` 里有 `123` 但没有 `4`，你就知道问题出在 CR0.PE 和 far jump 之间。这种逐步插入调试点的方法虽然朴素，但在 Bootloader 这种"没有任何输出手段"的环境里，它往往是最有效的。

### 常见 triple fault 原因排查表

下面是我们实际踩过的坑和对应的排查方向，按出现频率排序：

**lgdt 读到了错误的 GDT 地址。** 这是最常见的坑，原因是 `lgdt` 前 DS 没有清零。实模式下 `lgdt` 用 `DS × 16 + offset` 计算地址，DS 不为 0 就会读到错误位置。排查方法：在 `lgdt` 前后各加一个 debugcon 输出，如果前一个字符出现了后一个没有，大概率是 `lgdt` 导致的三重故障。

**GDT 描述符定义错误。** Access Byte 写错一位、Limit 不够大、Base 地址错误，这些都会导致 `ljmp` 时 CPU 查到无效描述符触发 #GP。排查方法：用 `xxd` 或 `objdump` 检查编译后的 `.gdt` section，逐字节对照 Intel SDM Vol.3A §3.4.5 的描述符格式。

**linker script base 地址不匹配。** stage2 被加载到 `0x8000`，但 linker script 的 location counter 设成了 `0x0`，导致所有绝对地址引用偏移 `0x8000`。排查方法：用 `readelf -s build/boot/stage2 | grep gdt` 查看 `gdt` 符号的链接地址，如果比预期小 `0x8000`，就是 base 没设对。

**省略了 far jump。** CR0.PE 设完之后直接写 32 位代码，没有 `ljmp` 刷新 CS 的隐藏缓存。CPU 还在用 16 位模式解码，32 位编码的指令被解析成乱码。排查方法：检查 `movl %eax, %cr0` 后面是否紧跟 `ljmp`，中间不能有任何其他指令。

**`.code16` 和 `.code32` 放错了位置。** `.code32` 放在了 `ljmp` 之前（汇编器提前生成 32 位编码但 CPU 还在 16 位模式），或者 `.code32` 放在了 `ljmp` 之后但离 `pm_entry` 标签太远（导致 `pm_entry` 处的前几条指令仍然是 16 位编码）。排查方法：用 `objdump -d build/boot/stage2` 检查反汇编，确认 `pm_entry` 后的指令是 32 位编码。

## 收尾

到这里，tag 002 的全部内容就完成了。验证方式很简单：执行构建并运行，然后检查 `debug.log` 文件。如果里面出现了字符 `P`（0x50），恭喜——保护模式切换成功，CPU 正确执行了 32 位代码，段寄存器重载正确，debugcon 输出工作正常。如果什么都没出现或者 QEMU 反复重启，大概率是上面提到的某个坑，按照排查表逐一检查。

回过头看，从实模式到保护模式的切换，核心就是六步：CLI、DS 清零、LGDT、CR0.PE、far jump、段寄存器重载。每一步都有存在的理由，顺序不能调换，任何一步省略都会导致三重故障。这套流程虽然看起来死板，但它是 x86 架构的硬性要求，从 80386 开始就没变过。理解了这套流程，你也就理解了 x86 处理器在模式切换时内部发生的全部事情。

下一篇（tag 003）我们将继续往上走——从 32 位保护模式进入 64 位长模式。长模式的切换需要设置页表、启用 PAE 和 EFER.LME，流程比保护模式切换更长也更复杂，但底层的思路是一样的：按顺序做、每步验证、不要跳步。

## 参考资料

- Intel SDM Vol.3A §10.9.1 — Switching to Protected Mode：完整的 6 步切换流程（CLI、LGDT、CR0.PE、Far Jump、段寄存器重载、第一条保护模式指令），[Intel SDM 页面](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- Intel SDM Vol.3A §10.1 — Processor Initialization：CR0.PE 位定义，处理器状态转换
- Intel SDM Vol.3A §2.4.1 — Global Descriptor Table Register (GDTR)：`lgdt` 指令的行为和 GDTR 寄存器格式
- Intel SDM Vol.3A §3.4.5 — Segment Descriptors：8 字节描述符格式，Base/Limit/Access/Flags 字段的分散排列方式
- OSDev Wiki: [Protected Mode](https://wiki.osdev.org/Protected_Mode) — 进入保护模式的步骤和常见陷阱
- OSDev Wiki: [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial) — real mode 下 lgdt 的注意事项
- xv6 bootasm.S: [https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S) — xv6 的保护模式切换实现，流程与 Cinux 几乎一致
- MIT xv6 book: [https://pdos.csail.mit.edu/6.828/2025/xv6/book-riscv-rev5.pdf](https://pdos.csail.mit.edu/6.828/2025/xv6/book-riscv-rev5.pdf)
