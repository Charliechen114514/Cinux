---
title: 002-boot-gdt-protected-3 · GDT 与保护模式
---

# 调试实战与设计对比：保护模式切换的坑和别人的做法

> 标签：x86, 调试, triple fault, GDT, 保护模式, 设计对比
> 前置：[002-2 切换全流程](002-boot-gdt-protected-2.md)

## 前言

前两篇我们已经把从实模式到保护模式的完整切换流程走了一遍：CLI、DS 清零、LGDT、CR0.PE、远跳转、段寄存器重载。代码本身其实不复杂——总共也就十几条指令——但保护模式切换的调试却是 x86 内核开发中最让人头疼的环节之一。

原因很简单：大部分错误的症状都是同一个——三重故障（Triple Fault），CPU 直接复位，QEMU 窗口闪一下就没了，连给你看报错的机会都不给。

这篇我们做两件事。第一，看看其他项目是怎么处理保护模式切换的——这不是简单的"对照表"，而是想通过对比来理解不同工程约束下的不同选择。第二，整理出一套系统化的调试方法论，包括 GDB 断点调试、QEMU 异常日志、debugcon 面包屑调试，以及一个按出现频率排序的常见 triple fault 原因排查表。

如果你在跑 tag 002 的代码时遇到了三重故障，直接跳到排查表，按顺序检查。

## 环境说明

本篇不涉及新的代码改动，所有调试手段都基于前两篇已经完成的代码。调试工具包括 QEMU 的 `-d int` 选项、GDB 远程调试（通过 `make run-debug` 启动）、以及 debugcon（端口 `0xE9`）的面包屑输出。如果你想在 GDB 里单步跟踪，需要用 ELF 文件（`build/boot/stage2`）而非 bin 文件。

## xv6 是怎么做的

xv6 的 `bootasm.S`（https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S ）把整个 real-to-protected mode 切换塞进了单个 512 字节的引导扇区里。核心流程和 Cinux 完全一致——`cli`、段寄存器清零、`lgdt`、CR0.PE、`ljmp`、段寄存器重载——毕竟 Intel SDM 的步骤就那么多，谁来了都得按这个顺序走。但细节上的差异很有意思。

首先是 GDT 定义。xv6 用宏 `SEG_ASM` 来生成描述符，展开后和 Cinux 手写的完全等价，都是 Flat Model（Base=0, Limit=4GB）。GDT 放在 `.p2align 2` 后面，和代码在同一个文件里，没有单独的 section。

Cinux 则把 GDT 放在独立的 `.section .gdt,"a"` 中并 8 字节对齐，更干净但也更"重"——因为 Cinux 的 stage2 没有 512 字节的空间限制，有条件做更好的工程组织。

其次是 A20 开启方式。xv6 用键盘控制器 8042（端口 0x64/0x60，所谓的 "fast A20" 方式），而 Cinux 用 BIOS INT 0x15 AX=0x2401。8042 方式更快（不需要 BIOS 调用的开销），但兼容性稍差；BIOS 方式更可靠但需要依赖 BIOS 实现。

对于 xv6 来说，8042 方式是合理的——它的目标环境是 QEMU 和 Bochs，这些仿真器对 8042 的支持很完善。

然后是栈的位置。这是差异最大的地方。xv6 在保护模式切换后把栈指针设为 `0x7C00`——也就是 boot sector 自身的位置，栈从这里向下增长。这意味着栈空间从 `0x7C00` 一直往下到 `0x0000`，理论上有 31KB。

但这片区域里有中断向量表（`0x0000`-`0x03FF`）和 BIOS 数据区（`0x0400`-`0x04FF`），稍不留神就会覆盖。Cinux 使用 `0x90000`，远离所有这些敏感区域，64KB 的空间也很宽裕。

这个差异的根源在于架构不同：xv6 的单扇区设计让它的代码只能从 `0x7C00` 开始，栈跟着放在这里是顺理成章的；Cinux 的两阶段设计让 stage2 从 `0x8000` 开始，栈可以选一个更独立的地址。

最后是切换后的动作。xv6 在 `start32` 里设置完段寄存器和栈之后，直接 `call bootmain` 进入 C 代码——开始从磁盘读取内核。Cinux 在切换后先 `outb 'P'` 到 debugcon 做验证输出，然后才继续后续的 long mode 切换。

这个差异反映的是调试哲学：Cinux 在每个关键步骤后都插入了验证点（debugcon 输出），这样在出问题时可以精确定位是哪一步失败了；xv6 更"自信"，切完就直接往前走。

## Linux 和 GRUB 的做法

Linux 的做法和 xv6、Cinux 都不一样——它根本不在内核里做 real-to-protected mode 切换。Linux 使用 GRUB（或其他符合 Multiboot 规范的 bootloader）来启动，GRUB 在把内核加载到内存之前就已经完成了 real mode -> protected mode -> 32-bit flat mode 的全部切换。内核被加载时，CPU 已经运行在 32 位保护模式下，内核的第一条指令就是 32 位代码。

这个选择背后的权衡很明确：Linux 内核的开发者认为 bootloader 的工作不应该由内核来承担。GRUB 是一个成熟的外部项目，它处理了各种硬件初始化的脏活累活——A20 开启、VESA 模式设置、保护模式切换、甚至可以从文件系统读取内核映像。

Linux 内核的 `arch/x86/boot/` 目录下确实有 real mode 的 setup 代码，但那是为了支持 Linux 自己的 boot protocol，让非 GRUB 的 bootloader 也能启动 Linux。

对于 Cinux 来说，选择自己写 bootloader 而不是用 GRUB，是一个有意识的权衡。教学目的下的自写 bootloader 有两个好处：第一，你能看到完整的启动链条（从 BIOS POST 到实模式代码执行到保护模式切换），每一步都是显式的、可调试的；第二，你能精确控制内存布局，比如 Cinux 的 VESA 初始化就是在 GRUB 不会帮你做的范围之内。

代价当然是更多的代码量和更多的调试时间——但如果你是在学习操作系统，这些恰恰是你想经历的。

## SerenityOS 和 ToaruOS

SerenityOS 使用 GRUB，和 Linux 一样跳过了 bootloader 的编写。ToaruOS 有自写 bootloader，但它直接从实模式跳到 long mode（64 位），跳过了 32 位保护模式这个中间阶段——这在现代系统上是完全可行的，因为 x86 的架构允许从实模式直接切换到 long mode（通过设置 CR0.PE 和 EFER.LME）。

Cinux 的分阶段设计（MBR -> Stage2 real mode -> Stage2 protected mode -> long mode）在教学上更有价值，因为它让读者能看到每一步的完整过程。如果直接跳到 long mode，你就错过了理解 GDT、段描述符、保护模式地址翻译这些基础知识的机会——而这些知识在调试 x86 相关的问题时是不可或缺的。

值得注意的是，所有这些项目虽然架构差异很大，但在 GDT 的设计上有一个共同的选择：Flat Memory Model。所有项目的代码段和数据段都是 Base=0、Limit=4GB，不做任何分段隔离。这不是巧合，而是现代操作系统设计的共识——真正的内存隔离交给后续的页表实现。

## 调试实战

### GDB 断点调试

如果你在 QEMU 中启动了 GDB stub（`-s -S` 参数），可以在保护模式切换的关键位置设置断点。最关键的两个断点是 `pm_entry` 和切换前的最后一条 16 位指令：

```gdb
(gdb) target remote :1234
(gdb) file build/boot/stage2
(gdb) b *pm_entry
(gdb) c
```

注意这里有一个非常重要的点：必须用 `file build/boot/stage2` 加载 ELF 文件，而不是 bin 文件。GDB 需要符号信息来解析断点地址，bin 文件里没有这些信息。我们早期调试时犯过一个低级错误——用 `file build/boot/stage2.bin` 加载纯二进制文件，结果 GDB 找不到 `pm_entry` 符号，断点设不上。

另外，在实模式和保护模式切换前后的 GDB 体验也有差异：实模式下用 `info registers` 看到的是 16 位寄存器（IP、SP 等），切换后变成 32 位（EIP、ESP）。如果在切换之前就试图查看 EIP，GDB 可能报 `Invalid register` 错误——这不是 bug，而是 CPU 此时确实还在实模式。

### QEMU `-d int` 查异常

如果你在保护模式切换中遇到了三重故障，QEMU 的 `-d int` 选项是定位问题的利器：

```bash
qemu-system-x86_64 -d int -no-reboot -D qemu_int.log -serial stdio -debugcon stdio -drive format=raw,file=build/cinux.img
```

`-d int` 会让 QEMU 在每次中断/异常发生时打印详细信息，包括异常类型、错误码、触发时的 CS:EIP。`-no-reboot` 防止 CPU 在三重故障后自动重启，这样你可以看到完整的异常链。典型的输出可能像这样：

```
check_exception old: 0xffffffff new 0xd
     0: v=0d ecode=0x00000000 eip=0x0000816c cs=0x0008
```

这告诉你 CPU 在地址 `0x816c` 处触发了一个 General Protection Fault（异常号 0x0D），CS=0x0008 说明此时已经在保护模式下（使用了选择子 0x08）。如果异常发生在 `ljmp` 之前，CS 可能还是实模式的值，说明 GDT 内容有问题。

### debugcon 面包屑调试

我们在代码中使用的 `outb 'P', 0xE9` 是最简单的验证手段。但你可以在切换流程的每一步后面都插一个不同的字符输出，形成一条"面包屑"式的调试链。

比如在 lgdt 前后各输出一个字符，在 CR0 写入后输出一个字符，在远跳转后输出一个字符——通过观察哪些字符出现了、哪些没出现，就能快速定位崩溃发生在哪一步。

如果 `debug.log` 里有 `123` 但没有 `4`，你就知道问题出在 CR0.PE 和 far jump 之间。这种逐步插入调试点的方法虽然朴素，但在 Bootloader 这种"没有任何输出手段"的环境里，它往往是最有效的。

### 结合 GDB 和 debugcon 的混合调试

在实际调试中，最有效的方式是结合使用 GDB 和 debugcon 面包屑。

先用面包屑法快速定位崩溃发生在哪一步——比如你发现 `debug.log` 里只有 `12` 没有 `3`，说明崩溃发生在 `lgdt` 这一步。然后再用 GDB 在 `lgdt` 前后设断点，单步执行，观察 GDTR 寄存器的值是否正确。

在 GDB 中查看 GDTR 没有直接命令，但你可以通过检查内存来间接验证。比如在 `lgdt` 执行前，先打印 `gdt_ptr` 指向的 6 字节内容，确认 limit 和 base 的值是否符合预期。然后单步执行 `lgdt`，如果 CPU 没有 triple fault，说明 GDTR 已经成功加载了。

这种"面包屑快速定位 + GDB 精确分析"的组合，比单独使用任何一种方法都要高效。

### objdump 反汇编验证

另一个常用的调试手段是用 `objdump` 反汇编 stage2 的 ELF 文件，检查指令编码是否正确：

```bash
objdump -d build/boot/stage2 | less
```

在反汇编输出中，你可以验证几个关键点。首先，`pm_entry` 标签之前的指令应该都是 16 位编码（操作码前没有 `0x66` 前缀），而 `pm_entry` 之后的指令应该都是 32 位编码。

其次，远跳转指令应该紧跟在 `movl %eax, %cr0` 之后，中间不能有其他指令。

最后，检查 `.gdt` section 的内容是否正确：

```bash
objdump -s -j .gdt build/boot/stage2
```

逐字节对照描述符格式，确认 null 描述符全零、code 描述符 Access Byte 是 `0x9A`、data 描述符 Access Byte 是 `0x92`、Flags 都是 `0xCF`。

## 常见 Triple Fault 原因排查表

下面是我们实际踩过的坑和对应的排查方向，按出现频率排序。如果你在跑 tag 002 时遇到了三重故障，按照这个表的顺序逐一检查。

**lgdt 读到了错误的 GDT 地址。** 这是最常见的坑，原因是 `lgdt` 前 DS 没有清零。实模式下 `lgdt` 用 `DS × 16 + offset` 计算地址，DS 不为 0 就会读到错误位置。排查方法：在 `lgdt` 前后各加一个 debugcon 输出，如果前一个字符出现了后一个没有，大概率是 `lgdt` 导致的三重故障。

**GDT 描述符定义错误。** Access Byte 写错一位、Limit 不够大、Base 地址错误，这些都会导致 `ljmp` 时 CPU 查到无效描述符触发 #GP。排查方法：用 `xxd` 或 `objdump -s -j .gdt build/boot/stage2` 检查编译后的 `.gdt` section，逐字节对照 Intel SDM Vol.3A Section 3.4.5 的描述符格式。

**linker script base 地址不匹配。** stage2 被加载到 `0x8000`，但 linker script 的 location counter 设成了 `0x0`，导致所有绝对地址引用偏移 `0x8000`。排查方法：用 `readelf -s build/boot/stage2 | grep gdt` 查看 `gdt` 符号的链接地址，如果比预期小 `0x8000`，就是 base 没设对。

**省略了 far jump。** CR0.PE 设完之后直接写 32 位代码，没有 `ljmp` 刷新 CS 的隐藏缓存。CPU 还在用 16 位模式解码，32 位编码的指令被解析成乱码。排查方法：检查 `movl %eax, %cr0` 后面是否紧跟 `ljmp`，中间不能有任何其他指令。

**`.code16` 和 `.code32` 放错了位置。** `.code32` 放在了 `ljmp` 之前（汇编器提前生成 32 位编码但 CPU 还在 16 位模式），或者 `.code32` 放在了 `ljmp` 之后但离 `pm_entry` 标签太远（导致 `pm_entry` 处的前几条指令仍然是 16 位编码）。排查方法：用 `objdump -d build/boot/stage2` 检查反汇编，确认 `pm_entry` 后的指令是 32 位编码。

**push/pop 宽度不一致的遗留问题。** 如果 `serial.S` 中还有遗漏的不带宽度后缀的 push/pop，在某些边缘情况下也可能导致栈错位。排查方法：用 `grep` 搜索 `serial.S` 中所有不带 `w` 后缀的 push/pop 指令，确保每个都显式标注了宽度。

## 收尾

回过头看，保护模式切换的调试核心就是两件事：一是用 debugcon 或 GDB 精确定位崩溃发生在哪一步，二是根据崩溃位置反推可能的原因。

最常见的三个坑——DS 没清零导致 lgdt 读错地址、GDT 描述符定义有误、linker script base 不匹配——占了所有 triple fault 问题的 80% 以上。按照排查表的顺序逐一检查，基本都能在 15 分钟内定位到问题。

下一篇（tag 003）我们将继续往上走——从 32 位保护模式进入 64 位长模式。长模式的切换需要设置页表、启用 PAE 和 EFER.LME，流程比保护模式切换更长也更复杂，但底层的调试思路是一样的：面包屑输出 + GDB + `-d int` 异常日志。

## 参考资料

- Intel SDM Vol.3A Section 10.9.1 — Switching to Protected Mode：完整的 6 步切换流程，[Intel SDM 页面](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- Intel SDM Vol.3A Section 3.4.5 — Segment Descriptors：8 字节描述符格式，用于排查 GDT 描述符定义错误
- Intel SDM Vol.3A Section 2.4.1 — Global Descriptor Table Register (GDTR)：`lgdt` 指令的行为和 GDTR 寄存器格式
- OSDev Wiki: [Protected Mode](https://wiki.osdev.org/Protected_Mode) — 进入保护模式的步骤和常见陷阱
- OSDev Wiki: [GDT Tutorial](https://wiki.osdev.org/GDT_Tutorial) — real mode 下 lgdt 的注意事项
- xv6 bootasm.S: [https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S) — xv6 的保护模式切换实现，流程与 Cinux 几乎一致
- MIT xv6 book: [https://pdos.csail.mit.edu/6.828/2025/xv6/book-riscv-rev5.pdf](https://pdos.csail.mit.edu/6.828/2025/xv6/book-riscv-rev5.pdf)
