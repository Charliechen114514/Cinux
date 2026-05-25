---
title: 005-mini-kernel-entry-3 · 内核入口
---

# 踩过的两个大坑：PDPT 索引幽灵与链接器脚本陷阱

> 标签：PDPT 索引, 页表映射, linker script, LMA, SIZEOF, GDB, VSCode, 远程调试
> 前置：[005B 从数字到文字的魔法：kprintf](005-mini-kernel-entry-2.md)

## 写在前面

前两篇我们写了串口驱动和 kprintf 格式化引擎，代码量不大，逻辑也不算复杂。按理说写完之后内核应该能愉快地通过串口输出 `Cinux Mini Kernel v0.1.0` 了——但如果你跟着 tag 005 的代码一路走来，你一定记得有那么几个时刻，内核启动后串口毫无输出，debugcon 的字符序列突然中断，全局构造函数跳转到了垃圾地址。我血压拉满地调了半天，最后发现新写的代码一点问题都没有，真正的罪魁祸首是两个在更早阶段就埋下的 bug。

第一个 bug 出在页表映射上：高半核地址 `0xFFFFFFFF80000000` 的 PDPT 索引被写成了 511，正确的应该是 510——差了那么一个 bit。第二个 bug 出在链接脚本里：用 `SIZEOF()` 累加计算段的 LMA（Load Memory Address），但 `SIZEOF()` 不包含段之间的对齐填充，导致 `.init_array` 里的全局构造函数指针指向了错误的位置，读出来的全是 `0xFFFFFFFF`。

这两个 bug 有一个共同特征：identity mapping 下一切正常，只有高半核地址才会触发。这意味着如果你只测了低地址访问就以为"页表没问题"，或者只看了 `.text` 段就以为"链接脚本没问题"，你可能会被蒙蔽很久。这一篇我们就来拆解这两个幽灵 bug 的侦探过程，顺便把 QEMU + GDB 远程调试和 VSCode 调试配置的实战流程一起讲了。

## 环境 & 背景

本篇涉及的代码修改不在新写的驱动或库里，而是在之前 tag 就已经存在的文件中：`boot/common/long_mode.S`（页表设置）和 `kernel/mini/linker.ld`（链接脚本）。另外还有 `boot/stage2.S` 中新增的映射验证代码，以及 `.vscode/launch.json` 和 `.vscode/tasks.json` 的调试配置。

调试工具方面，我们用到了 QEMU 的 `-s -S` 参数启动 GDB stub、命令行 GDB 远程调试、以及 VSCode 的图形化调试界面。三者各有适用场景：debugcon 输出用于快速定位问题区域，GDB 命令行用于精确检查寄存器和内存，VSCode 用于日常的单步调试。

## 幽灵 Bug 一：PDPT 索引差了一位

### 症状

内核启动后 debugcon 输出停在了 `OPL`，没有出现后续的字符。`OPL` 分别代表 Stage2 完成（O）、保护模式进入（P）、Long Mode 进入（L）——说明 bootloader 执行到了 Long Mode 之后就没有然后了。

但奇怪的是，如果我在 `boot.S` 里用 identity mapping 地址（0x20000）来测试，内核是能跑的。只有在跳转到高半核地址（0xFFFFFFFF80020000）时才会炸。

### 根本原因

问题出在 `long_mode.S` 的 `setup_page_tables` 函数中。之前的代码是：

```asm
// 错误：PDPT[511] -> PD
movl $PD_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PDPT_PHYS_ADDR + (511 * 8)
```

直觉上"511 看起来更像是最高位"，很多人（包括我）会顺手写 511。但正确答案是 510。让我们重新拆解虚拟地址 `0xFFFFFFFF80000000` 在四级页表中的索引路径。x86-64 的 4-level paging 把 48 位有效虚拟地址分成四个 9 位的索引，加上 12 位的页偏移。Intel SDM Vol. 3A Chapter 4 对此有完整的描述：

```
虚拟地址: 0xFFFFFFFF80000000
二进制:   1111...1111 10000000 00000000 00000000 00000000

PML4 索引 (bits 47:39):  111111111 = 511 = 0x1FF
PDPT 索引 (bits 38:30):  100000000 = 510 = 0x1FE   <- 关键！
PD   索引 (bits 29:21):  000000000 = 0   = 0x000
```

关键在 PDPT 索引。很多人会以为 `0x80000000` 的 bit 30 是 1（"毕竟 8 的二进制是 1000"），但实际上 `0x80000000` 的二进制表示是 `10000000 00000000 00000000 00000000`——bit 31 是 1，bit 30 是 0。所以 bits 38:30 = `111111110` = 510，不是 511。

用 PDPT[511] 的话，CPU 在翻译 `0xFFFFFFFF80020000` 这个地址时会查找 PDPT[511]，而我们只设了 PDPT[510]，PDPT[511] 是空的（全零，not present），于是触发 Page Fault。这个 bug 的狡猾之处在于 identity mapping 完全正常——用 0x20000 访问内核没问题，只有高半核地址会炸。如果你不主动测试高半核地址，可能很久都不会发现。

### 修正

```asm
// 正确：PDPT[510] -> PD
movl $PD_PHYS_ADDR, %eax
orl $0x03, %eax
movl %eax, PDPT_PHYS_ADDR + (510 * 8)    // 低 32 位
movl $0, PDPT_PHYS_ADDR + (510 * 8) + 4  // 高 32 位清零
```

修正后的代码还多了一条指令：`movl $0, offset + 4` 把页表项的高 32 位清零。这是因为 x86-64 的页表项是 64 位的，但 `setup_page_tables` 运行在 32 位保护模式下，`movl` 只能写 32 位。之前只写了低 32 位，高 32 位依赖 `rep stosl` 清零后的初始值——这"恰好"是对的（因为物理地址低于 4GB），但不显式清零总让人不踏实。万一后续代码在清零和设置之间写了什么到高 32 位，就会踩坑。显式清零是一种"安全编程"的习惯。

### 预防：映射验证代码

为了不再被类似的 bug 偷袭，我们在 `stage2.S` 跳转到内核之前新增了映射验证：

```asm
// Test 1: 验证 identity mapping
movq $0x20000, %rax
movb (%rax), %al             // 读内核第一个字节 (应该是 0xFA = cli)
outb %al, $DEBUGCON_PORT     // 输出到 debugcon

// Test 2: 验证 higher-half mapping
movq $0xFFFFFFFF80020000, %rax
movb (%rax), %al             // 同一个字节通过高半核地址读
outb %al, $DEBUGCON_PORT     // 应该也是 0xFA

// Test 3: 跳转信号
movb $0x48, %al              // 'H' = Higher-half jump
outb %al, $DEBUGCON_PORT
```

Test 1 读 identity mapping 地址，Test 2 读高半核地址——如果页表映射正确，读到的应该是同一个字节。在 `debug.log` 中你应该看到两个 0xFA 字节（`0xFA` 是 `cli` 指令的机器码），然后是 'H'。如果只有一个 0xFA，说明 identity mapping 正常但高半核映射有问题；如果连第一个 0xFA 都没有，说明内核本身没被正确加载。

这种"在关键步骤前后输出诊断字符"的做法，是裸机开发中最实用的调试手段之一。它不需要任何基础设施——只要 `outb` 能工作就能用。

## 幽灵 Bug 二：链接脚本的 LMA 计算陷阱

### 症状

页表 bug 修完之后，debugcon 的字符序列变成了 `OPL\xfa\xfaH\[1234'`——说明 bootloader 跳转成功，内核入口开始执行，设栈（1）、清 BSS（2）都完成了。但在第 3 步（调用全局构造函数）时，内核崩溃了。

具体的表现是：`_init_global_ctors()` 遍历 `.init_array` 段的函数指针数组，但读到的指针值是 `0xFFFFFFFF`——这显然不是合法的函数地址，CPU 跳转过去直接 Page Fault。

### 根本原因

问题出在链接脚本的 LMA（Load Memory Address）计算上。之前的写法是：

```ld
.text : AT(KERNEL_PHYS_BASE) { ... }
.data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) { ... }
.init_array : AT(KERNEL_PHYS_BASE + SIZEOF(.text) + SIZEOF(.data)) { ... }
```

这种累加方式看起来逻辑清晰：`.text` 从 `KERNEL_PHYS_BASE` 开始，`.data` 紧随其后偏移 `SIZEOF(.text)`，`.init_array` 再偏移 `SIZEOF(.data)`。问题在于 `SIZEOF()` 只返回段内容本身的大小，不包含段之间的对齐填充（alignment padding）。

举个例子：`.text` 段可能在 VMA 0xFFFFFFFF80020000 到 0xFFFFFFFF80020FF0，内容大小为 0xFF0 字节。但因为 `.data` 需要 16 字节对齐，VMA 会跳到 0xFFFFFFFF80021000（多了 0x10 的填充）。此时 `SIZEOF(.text)` 还是 0xFF0，但 `.data` 的实际起始偏移应该是 0x1000 而不是 0x0FF0。差了 0x10 字节。

这种偏移偏差会逐段累积。到 `.init_array` 的时候，LMA 可能偏差了几十甚至几百字节。bootloader 加载内核映像到物理内存后，`.init_array` 中的数据实际上不在 `ADDR(.init_array) - KERNEL_Virt_BASE` 这个物理地址上，而是在偏移了几十个字节的位置。你读到的不是函数指针数组，而是 `.data` 段中间某个位置的数据——恰好是 `0xFFFFFFFF`（或者别的垃圾值）。

### 修正

```ld
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
KERNEL_PHYS_BASE = 0x20000;

SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) { ... }
    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) { ... }
    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) { ... }
    .bss : { ... }
}
```

修正后的公式是 `AT(ADDR(.section) - KERNEL_Virt_BASE)`。`ADDR(.section)` 返回段的实际 VMA（包含了所有对齐填充后的正确地址），减去 `KERNEL_Virt_BASE`（0xFFFFFFFF80000000）就得到正确的物理地址。无论中间有多少对齐填充，这个关系始终成立：LMA = VMA - KERNEL_Virt_BASE。

你可以用 `readelf -l build/kernel/mini/mini_kernel` 来验证。查看 LOAD 段的 VirtAddr 和 PhysAddr，确保 PhysAddr 是从 0x20000 开始的连续物理地址，而 VirtAddr 是从 0xFFFFFFFF80020000 开始的高半核地址。

这个 bug 的教训是：在链接脚本中，永远不要用 `SIZEOF()` 累加来计算 LMA。`ADDR() - offset` 公式在任何情况下都是正确的，因为它利用了 VMA 和 LMA 之间的固定关系。

## QEMU + GDB 远程调试实战

找到了 bug 的方向之后，具体的验证过程离不开调试工具。QEMU 内置了 GDB stub 功能，可以通过 `-s -S` 参数启用。`-s` 让 QEMU 在 TCP 端口 1234 上监听 GDB 连接，`-S` 让 CPU 在启动时冻结，等待 GDB 连接后再继续执行。

Cinux 的 CMake 配置已经把调试模式封装成了 `make run-debug` 目标。启动流程是这样的：

```bash
# Terminal 1: 启动 QEMU 调试模式
cd build && make run-debug
# QEMU 启动并暂停，等待 GDB 连接

# Terminal 2: 启动 GDB
gdb build/kernel/mini/mini_kernel
(gdb) target remote :1234
(gdb) break _start
(gdb) continue
```

连接成功后，GDB 会显示 CPU 当前停在的位置（通常是 `0x00007c00`，即 MBR 入口）。然后你可以在任何地址设断点——比如 `break *0xFFFFFFFF80020000` 在内核入口设断点，`break mini_kernel_main` 在 C++ 入口函数设断点。

调试页表问题时，最常用的命令是查看内存和寄存器：

```gdb
(gdb) info registers cr3       # 查看 CR3（页表基址）
(gdb) x/1gx 0x1000 + 511*8    # 查看 PML4[511]
(gdb) x/1gx 0x2000 + 510*8    # 查看 PDPT[510]
(gdb) x/1gx 0x2000 + 511*8    # 查看 PDPT[511]（应该是 0）
```

调试链接脚本问题时，关键是查看 `.init_array` 的内容：

```gdb
(gdb) x/4gx &__init_array_start
# 如果看到 0xFFFFFFFFFFFFFFFF，说明 LMA 计算有误
# 如果看到合理的函数地址（如 0xFFFFFFFF80020xxx），说明正确
```

GDB 的 Remote Serial Protocol 本身是一个基于 ASCII hex 的文本协议。GDB 发送 `$packet#checksum` 格式的数据包，QEMU 返回响应。比如 `$g#7a` 是读取所有寄存器，`$m addr,len#xx` 是读取内存。你不需要手动拼这些数据包——GDB 自动处理——但了解底层协议有助于理解调试过程中偶尔出现的超时或连接错误。

QEMU 文档（https://www.qemu.org/docs/master/system/gdb.html）和 OSDev Wiki 的 GDB 页面（https://wiki.osdev.org/GDB）有更详细的说明。

## VSCode 调试配置

对于日常开发，每次都要开两个终端、手动输 GDB 命令还是挺烦的。VSCode 的 C++ 调试扩展可以图形化地完成同样的工作。配置文件在 `.vscode/launch.json`：

```json
{
    "name": "QEMU 调试 (mini kernel)",
    "type": "cppdbg",
    "request": "launch",
    "program": "${workspaceFolder}/build/kernel/mini/mini_kernel",
    "MIMode": "gdb",
    "miDebuggerServerAddress": "localhost:1234",
    "setupCommands": [
        { "text": "-gdb-set architecture i386:x86-64" },
        { "text": "-gdb-set disassembly-flavor intel" },
        { "text": "-gdb-set pagination off" }
    ]
}
```

关键配置项是 `miDebuggerServerAddress: localhost:1234`——告诉 VSCode 的 GDB 前端连接到 QEMU 的 GDB stub。`setupCommands` 中的 `-gdb-set architecture i386:x86-64` 是 GDB 17.x 的兼容性修复——新版本的 GDB 在连接 QEMU 时如果不显式设置架构，可能会用错误的反汇编模式。`-gdb-set disassembly-flavor intel` 把反汇编语法从默认的 AT&T 切换到 Intel 格式——对我们来说其实无所谓（Cinux 的汇编代码都是 AT&T 语法），但有些人可能更习惯 Intel 格式。

使用流程是：先在一个终端里启动 QEMU 调试模式（`cd build && make run-debug`），然后在 VSCode 里按 F5 启动调试。VSCode 会连接到已经运行的 QEMU 实例，加载符号文件，然后在当前 CPU 停止的位置暂停。之后你就可以用 F10 单步跳过、F11 单步进入、Shift+F11 跳出函数——和在用户态程序上调试的体验基本一样。

VSCode 的 `.vscode/tasks.json` 还提供了一个 `QEMU: Run debug mode` 任务，可以用 Ctrl+Shift+B 构建然后在后台启动 QEMU 调试模式。这样你就不需要手动开终端了——按一下快捷键构建和启动，然后 F5 连接。

## 调试决策树：从 debugcon 到 GDB

经过这两个 bug 的折腾，我总结了一套调试策略的决策树，适用于内核开发的各个阶段：

第一步永远是看 debugcon 输出。`debug.log` 里的字符序列告诉你 bootloader 执行到了哪一步。如果序列中断在 `OPL`，问题在页表跳转；中断在 `OPL\xfa\xfaH` 之后，问题在内核初始化阶段。

第二步是看串口输出。如果内核能执行到 `mini_kernel_main`，那 kprintf 应该能工作了。如果串口有输出但内容不对，问题在格式化逻辑或者数据传递。

第三步才是上 GDB。如果前两步都定位不了问题，就需要在可疑位置设断点，单步执行，检查寄存器和内存。GDB 调试的开销最大（每步暂停 CPU），但也是最精确的工具。

在 Cinux 的开发实践中，这三步的适用频率大约是 7:2:1——大部分问题通过 debugcon 字符就能定位，少部分需要串口输出验证，真正需要 GDB 的只有最棘手的 bug。

## 收尾

回头看这两个 bug，它们的根因都不是什么高深的技术问题——一个是 bit 30 的计算失误，一个是 `SIZEOF()` vs `ADDR()` 的区别。但它们的影响范围都很大：PDPT 索引错误导致整个高半核地址空间不可访问，LMA 计算错误导致所有全局对象的构造函数指针变成垃圾值。这两个 bug 的共同特征是"identity mapping 下一切正常"——这恰恰是最危险的地方，因为你可能只测了低地址就以为没问题了。

调试工具方面，debugcon 字符输出是最轻量的诊断手段，GDB 远程调试是最精确的定位工具，VSCode 把 GDB 包装成了更友好的图形界面。三者配合使用，基本上可以覆盖内核开发中遇到的绝大多数调试场景。

下一篇我们进入一个完全不同的话题：怎么给内核搭一套测试框架，让它能自动验证自己的行为是否正确——不用每次手动重启 QEMU 然后人眼检查输出。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 4.3 — 4-Level Paging：4 级页表结构（PML4 -> PDPT -> PD -> PT），虚拟地址分解（bits 47:39 为 PML4 索引，bits 38:30 为 PDPT 索引，bits 29:21 为 PD 索引），2MB 大页的 PS bit 设置。Cinux 的高半核映射利用 PD[0]~PD[3] 的 2MB 大页条目被 PML4[511]/PDPT[510] 复用。`0xFFFFFFFF80000000` 的 PDPT 索引计算：bits 38:30 = `0b111111110` = 0x1FE = 510（bit 30 = 0）。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- GNU LD Manual — `ADDR(section)` and `SIZEOF(section)`：`ADDR()` 返回段的 VMA（包含对齐后的正确地址），`SIZEOF()` 返回段内容大小（不含对齐填充）。Cinux 的链接脚本修正从 `SIZEOF()` 累加改为 `ADDR() - offset` 公式。
  https://sourceware.org/binutils/docs/ld/Builtin-Functions.html

- QEMU Documentation — GDB Usage (https://www.qemu.org/docs/master/system/gdb.html)：QEMU 内置 GDB stub 的使用文档，`-s` 和 `-S` 参数说明，远程调试协议概述。

- OSDev Wiki — GDB (https://wiki.osdev.org/GDB)：OS 开发环境下的 GDB 远程调试指南，包括 QEMU 集成和常见问题排查。

- Building a VS Code Debugging Workflow for a Custom x64 OS (https://www.sqlpassion.at/archive/2025/07/22/building-a-vs-code-debugging-workflow-for-a-custom-x64-operating-system-with-qemu-and-gdb/)：VSCode + QEMU + GDB 调试配置的实战经验。
