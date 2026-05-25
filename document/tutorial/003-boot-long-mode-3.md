---
title: 003-boot-long-mode-3 · Long Mode
---

# Higher-Half Kernel：为什么 0xFFFFFFFF80000000 能找到物理地址 0x0

> 标签：x86_64, 高半内核, Higher-Half, 双映射, PML4[511], PDPT[510]
> 前置：[003-2 模式切换](003-boot-long-mode-2.md)

## 前言

前两篇我们把四级页表搭好了、把 CPU 从保护模式带到了 Long Mode，输出 `L` 宣告胜利。

但如果你仔细看了 `setup_page_tables` 的完整代码，会发现恒等映射的循环之后、`ret` 之前还有一段代码——

它设置了 PML4[511] 和 PDPT[510]，

把虚拟地址 0xFFFFFFFF80000000 映射到物理地址 0x00000000。

这一段代码只有 8 条指令，但背后的地址计算和设计决策值得单独用一篇来展开。

因为如果不理解 "为什么 PDPT 索引是 510 而不是 511" 这个问题，

将来你自己设计页表映射时一定会踩坑。

## Higher-Half 是什么，为什么需要它

操作系统的内核通常运行在虚拟地址空间的高地址区域，把低地址区域留给用户进程。

Linux、Windows、macOS 都是这么做的。

以 Linux 为例，在典型的 48 位地址空间配置下，

用户空间占低地址的 0x0000000000000000 到 0x00007FFFFFFFFFFF（128TB），

内核空间占高地址的 0xFFFF800000000000 到 0xFFFFFFFFFFFFFFFF

（也是 128TB，但实际使用范围取决于配置）。

这种设计的核心好处是隔离。

用户程序的虚拟地址空间从 0 开始，内核的虚拟地址空间在高处，两者互不干扰。

当内核需要访问用户空间时，可以通过 `copy_to_user` / `copy_from_user` 等机制安全地跨越边界。

而且，高半内核的地址（比如 0xFFFFFFFF80000000）对于用户程序来说是"不可达"的——

canonical address 检查会阻止用户态代码访问高地址区域。

Cinux 的内核入口地址被设定为 0xFFFFFFFF80020000。

内核被加载到物理地址 0x20000（在第一个 2MB 大页范围内），

但 C++ 代码是按高半地址编译链接的——

函数指针、全局变量的地址都是 0xFFFFFFFF80XXXXXX 的形式。

如果 Bootloader 不建立 Higher-Half 映射，

内核代码跳转到 0xFFFFFFFF80020000 时会发现那个虚拟地址没有对应的物理页，直接 page fault。

所以 Higher-Half 映射不是一个可选项，而是让内核"能跑起来"的必要条件。

## 逆向工程：从虚拟地址到页表索引

要理解 PML4[511] 和 PDPT[510] 是怎么来的，

我们需要把虚拟地址 0xFFFFFFFF80000000 拆开来看。

这个过程就像逆向工程——

从最终结果（一个 64 位地址）反推它在四级页表中的查找路径。

x86-64 使用 48 位有效虚拟地址。

虚拟地址的高 16 位是符号扩展（canonical address 要求），bit 47 到 bit 0 是实际参与页表查找的地址。

这 48 位被拆分为四段 9 位索引和一段 12 位页内偏移：

```
虚拟地址 0xFFFFFFFF80000000:
高位: 0xFFFF = 符号扩展 (bit 63:48)
PML4 索引 (bit 47:39) = 0x1FF = 511
PDPT 索引 (bit 38:30) = 0x1FE = 510
PD 索引   (bit 29:21) = 0x000 = 0
PT 索引   (bit 20:12) = 0x000 = 0 (不使用，因为是 2MB 大页)
页内偏移  (bit 11:0)  = 0x000 = 0
```

关键在于 bit 30。

0x80000000 在二进制中是 `1000 0000 0000 0000 0000 0000 0000 0000`，bit 30 的值是 0。

PDPT 索引取的是 bit 38:30，所以 bit 38 到 bit 31 全是 1（因为符号扩展）、bit 30 是 0，

结果是 `0b1_1111_1110 = 0x1FE = 510`。

这个 "510 而不是 511" 是一个很容易犯的 off-by-one 错误。

直觉上你可能觉得 PML4 用了 511（最后一条），PDPT 也应该用 511（最后一条），

但虚拟地址的位分布不是均匀的——

0x80000000 恰好在 bit 30 上是 0，导致 PDPT 索引比 PML4 索引少了 1。

如果你错误地把 PDPT[511] 设成指向 PD，

对应的虚拟地址是 0xFFFFFFFFC0000000 而不是 0xFFFFFFFF80000000——差了整整 1GB，

而且这种错误在调试时非常难发现，因为 CPU 不会报错，只是映射到了错误的地址。

## 代码详解：八条指令完成 Higher-Half 映射

整个 Higher-Half 映射只需要 8 条指令——

4 条设置 PML4[511]，4 条设置 PDPT[510]：

```asm
    // PML4[511] -> PDPT (same as PML4[0])
    movl $PDPT_PHYS_ADDR, %eax       // PDPT physical address = 0x2000
    orl $0x03, %eax                   // Add present+writable flags
    movl %eax, PML4_PHYS_ADDR + (511 * 8)  // PML4[511] low 32 bits
    movl $0, PML4_PHYS_ADDR + (511 * 8) + 4  // PML4[511] high 32 bits = 0

    // PDPT[510] -> PD (NOT 511! 0x80000000 has bit 30 = 0)
    movl $PD_PHYS_ADDR, %eax         // PD physical address = 0x3000
    orl $0x03, %eax                   // Add present+writable flags
    movl %eax, PDPT_PHYS_ADDR + (510 * 8)  // PDPT[510] low 32 bits
    movl $0, PDPT_PHYS_ADDR + (510 * 8) + 4  // PDPT[510] high 32 bits = 0
    ret
```

PML4[511] 的内存偏移是 0x1000 + (511 * 8) = 0x1000 + 0xFF8 = 0x1FF8——PML4 的最后一个条目。

PDPT[510] 的偏移是 0x2000 + (510 * 8) = 0x2000 + 0x0FE0 = 0x2FE0——PDPT 的倒数第二个条目。

这段代码的关键设计是复用。

PML4[511] 和 PML4[0] 指向同一张 PDPT 表（0x2000），

PDPT[510] 和 PDPT[0] 指向同一张 PD 表（0x3000），

PD[0] 是恒等映射阶段设好的 0-2MB 大页条目。

这样 Higher-Half 映射不需要分配任何额外的页表页面——

只需要在已有的表中添加两个链接条目。

另一个值得注意的细节是显式写高 32 位为 0。

对于恒等映射部分（PML4[0] 和 PDPT[0]），

代码只写了低 32 位，高 32 位依赖清零操作保证。

但 Higher-Half 部分显式写了 `movl $0, offset+4`。

这是一个有意为之的设计选择——

它让代码的意图更清晰，不需要读者回头确认"清零操作是否覆盖了这个位置"。

Intel SDM Vol.3A §4.5 明确要求页表条目中未实现的保留位必须为 0，否则会触发 #GP。

显式写 0 是对这个要求的直接回应。

## 双映射的威力

完成 Higher-Half 映射后，物理内存 0x00000000-0x001FFFFF（前 2MB）可以通过两条不同的虚拟地址路径访问：

```
路径 1（恒等映射）：
虚拟地址 0x0000000000000000 - 0x00000000001FFFFF
PML4[0] → PDPT → PDPT[0] → PD → PD[0] → 物理 0-2MB

路径 2（Higher-Half 映射）：
虚拟地址 0xFFFFFFFF80000000 - 0xFFFFFFFF801FFFFF
PML4[511] → PDPT → PDPT[510] → PD → PD[0] → 物理 0-2MB
```

这两条路径最终都到达 PD[0]（值 0x83，映射物理 0-2MB），

但走了不同的 PML4 和 PDPT 条目。

双映射在操作系统启动阶段非常重要——

CPU 在刚进入 Long Mode 时使用恒等映射地址执行代码（因为 EIP 还是指向低地址），

后续通过一次远跳转切换到 Higher-Half 地址。

在切换过程中，两条路径必须同时有效，否则 CPU 会在切换的瞬间找不到下一条指令。

内核入口地址 0xFFFFFFFF80020000 的翻译过程：

PML4[511]（=0x2003）找到 PDPT，PDPT[510]（=0x3003）找到 PD，

PD[0]（=0x83）映射物理 0-2MB，页内偏移 0x20000，最终物理地址 = 0x0000000000020000。

正好是内核被加载到的物理地址。

## xv6 的做法——对比

MIT 的 xv6（[GitHub](https://github.com/mit-pdos/xv6-public)）是一个 32 位操作系统，

完全没有 Higher-Half 的概念——

内核和用户程序共享同一个虚拟地址空间，内核就在低地址。

这种设计简单但不安全——用户程序可以直接读写内核内存。

xv6 的 64 位移植版本（swetland/xv6）需要处理 Higher-Half 映射，

做法和 Cinux 类似：

PML4[256] 做内核映射（对应虚拟地址 0xFFFFFF0000000000），PML4[0] 做用户空间映射。

Linux 的做法更复杂：

它用 PML4 的后半部分（index 256-511）映射内核空间，前半部分（index 0-255）留给用户空间。

内核的线性映射区域从 0xFFFFFFFF80000000 开始

（或 0xFFFF888000000000 的直接映射区，取决于 KASLR 配置），

和 Cinux 使用了相同的地址模式。

## 验证方法

在 GDB 中可以在 `setup_page_tables` 返回后检查 Higher-Half 相关的条目：

```
(gdb) x/1gx 0x1000 + 511*8       # PML4[511] = 0x2003
(gdb) x/1gx 0x2000 + 510*8       # PDPT[510] = 0x3003
(gdb) x/1gx 0x3000                # PD[0] = 0x83 (shared with identity mapping)
```

如果 PML4[511] 和 PML4[0] 的值相同（都是 0x2003），

PDPT[510] 和 PDPT[0] 的值相同（都是 0x3003），说明 Higher-Half 映射正确建立。

你也可以在 64 位模式下用 `x/1gx 0xFFFFFFFF80000000` 验证是否能正确读取到物理地址 0x0 处的数据。

## 收尾

Higher-Half 映射看起来只是多设了两个页表条目，

但背后的虚拟地址拆分、索引计算、双映射设计都是操作系统页表管理的基础知识。

理解了 "为什么 PDPT 索引是 510 而不是 511"，

你就掌握了手动计算任意虚拟地址对应的四级页表索引的能力——

这在将来调试页表相关 bug 时是最重要的技能。

Tag 003 到这里就全部完成了。

CPU 运行在 64 位 Long Mode 下，恒等映射和 Higher-Half 映射都正常工作，

64 位寄存器、四级页表、64 位栈全部就绪。

下一篇（tag 004）我们将继续往上走——

在 Long Mode 下填充 BootInfo 结构、跳转到 C++ 内核入口。

## 参考资料

- Intel SDM Vol.3A §4.5 — 4-Level Paging：PML4/PDPT/PD/PT 层级结构
- Intel SDM Vol.3A §4.5.1 — Page Size Extensions：2MB 大页的基地址计算
- Intel SDM Vol.3A §10.8.5 — Initializing IA-32e Mode：完整的 Long Mode 初始化步骤
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging)
- Linux kernel: [head_64.S](https://0xax.gitbooks.io/linux-insides/content/Booting/linux-bootstrap-4.html)
- xv6: [GitHub](https://github.com/mit-pdos/xv6-public) — 32 位版本不做 Higher-Half
