# 为什么 Long Mode 必须要分页：从四级页表到 8MB 恒等映射

> 标签：x86_64, 长模式, 四级页表, 2MB 大页, 恒等映射, identity mapping
> 前置：[002-2 保护模式切换全流程](002-boot-gdt-protected-2.md)

## 前言

上一篇我们把 CPU 从实模式带到了保护模式，输出一个 `P` 就算大功告成。说实话，2026 年了还在 32 位保护模式里折腾，多少有点考古的意思——寄存器只有 32 位宽，地址空间上限 4GB，各种特权级机制还停留在 80386 时代的设计。我们要写的是一个"现代"操作系统，后续 C++ 内核的编译器默认输出 64 位代码，不进 Long Mode 的话内核根本跑不起来。所以这一章的目标很明确：从 32 位保护模式切换到 64 位 Long Mode。

但和上一章的保护模式切换不同，Long Mode 不是简单拨几个控制位就能进去的。x86_64 架构做了一个硬性规定：Long Mode 强制要求分页。没有页表？对不起，进不去。这意味着在设置那些控制寄存器之前，我们必须先手工搭建一套完整的四级页表结构——PML4、PDPT、PD 三张表，每张 4KB，放在固定物理地址上，用 2MB 大页恒等映射前 8MB 内存。整个流程比保护模式切换复杂得多，而且踩坑点也更隐蔽——我们开发时确实被 EFER.LME 的位定义坑过，花了好一阵子才定位到问题。

本篇聚焦页表构建部分：为什么 Long Mode 需要分页、四级页表的结构是什么、2MB 大页怎么工作、Cinux 的页表为什么这样设计。下一篇讲解模式切换本身——CR4/EFER/CR0 的状态机、64 位 GDT 描述符、远跳转机制。

## 环境说明

我们仍然在 QEMU 上工作，工具链是 GAS（AT&T 语法）+ ld.bfd（GNU Linker）+ CMake。本篇涉及的核心代码位于 `boot/common/long_mode.S`（新文件，198 行），这是 tag 003 中新增的 Long Mode 初始化模块，从 stage2.S 的保护模式入口 `pm_entry` 被 `call` 调用。调试手段和之前一样是 QEMU debugcon（端口 0xE9），GDB 断点调试需要用 ELF 文件 `build/boot/stage2`。

## Long Mode 与分页的强制绑定

在保护模式下，分页是可选的——你可以只用分段机制做地址翻译（设好 GDT 的 Base 和 Limit），完全可以不开 CR0.PG。但 Long Mode 把这条路堵死了。Intel SDM Vol.3A §4.1 明确指出：在 64 位模式下，所有线性地址都必须通过页表翻译成物理地址，分段机制的 Base/Limit 被忽略（除了 FS/GS 的 base 可以通过 MSR 做特殊用途）。这意味着如果你想进入 Long Mode，就必须在 CR0.PG=1 之前准备好一套合法的页表结构，没有捷径。

为什么 Intel/AMD 要做这个设计？原因大致有两方面。首先是历史包袱的清理：x86 的分段机制从 8086 时代一路沿用到保护模式，各种段描述符的 Base/Limit/Access 组合极其复杂，和安全相关的漏洞层出不穷（比如通过修改段描述符绕过特权级检查）。Long Mode 直接废弃了分段的地址翻译功能，统一用分页来管理内存，简化了 CPU 的地址翻译逻辑和操作系统的内存管理代码。其次是 64 位地址空间的需要：分段机制最多只能描述 32 位段内偏移，而 64 位地址空间需要至少 48 位的有效虚拟地址（理论上限 256TB），只有页表的多级索引结构才能高效地表达这么大的地址空间。

所以我们的处境很清楚：要进 Long Mode，先建页表，没有商量余地。

## 四级页表结构

Long Mode 使用四级页表来翻译虚拟地址。Intel SDM Vol.3A §4.5 对这套结构有完整的描述，我们来拆解一下它到底是什么。

四级页表的全称是 4-Level Paging，从上到下依次是 PML4（Page Map Level 4）→ PDPT（Page Directory Pointer Table）→ PD（Page Directory）→ PT（Page Table）。每一级是一张 4KB 的表格，包含 512 个条目，每个条目 8 字节（64 位）。为什么是 512？因为 4KB / 8B = 512，一张 4KB 页正好放下 512 个条目。为什么每个条目 8 字节？因为 64 位地址需要更多位来存储物理页号和标志位。

虚拟地址的翻译过程是这样的：CPU 收到一个 48 位虚拟地址（高 16 位必须是符号扩展，叫做 canonical address），把它拆成四段 9 位索引加一段 12 位页内偏移。第一段 9 位是 PML4 索引，用来在 CR3 指向的 PML4 表中查找条目，找到后获得 PDPT 的物理地址。第二段 9 位是 PDPT 索引，用来在 PDPT 中查找条目，获得 PD 的物理地址。第三段 9 位是 PD 索引，在 PD 中查找条目。第四段 9 位是 PT 索引，在 PT 中查找条目，获得最终的 4KB 物理页基地址。最后 12 位页内偏移加到物理页基地址上，得到最终的物理地址。

```
48位虚拟地址的拆解：
┌─────┬────────┬────────┬────────┬────────┬──────────┐
│sign │ PML4i  │ PDPTi  │  PDi   │  PTi   │ offset   │
│ext  │ 9 bits │ 9 bits │ 9 bits │ 9 bits │ 12 bits  │
└─────┴────────┴────────┴────────┴────────┴──────────┘
  16b    9b       9b       9b       9b       12b     = 64b

翻译路径：
CR3 → PML4[PML4i] → PDPT → PDPT[PDPTi] → PD → PD[PDi] → PT → PT[PTi] → 物理页 + offset
```

PML4 的一级索引覆盖 512 个区域，每个区域 512GB，所以 512 × 512GB = 256TB，这就是为什么 Long Mode 的虚拟地址空间理论上限是 256TB。

但这套完整的四级结构对 Bootloader 来说太重了。四级页表映射 4KB 小页的话，每映射一个 4KB 页需要四级表各有一个条目——映射 8MB 需要多少个 PT 页面？8MB / 4KB = 2048 个页，一个 PT 有 512 个条目，所以需要 4 个 PT，加上 PML4、PDPT、PD 各一页，总共 7 页 = 28KB。在 Bootloader 阶段没有动态内存分配，每页都要手工安排物理地址、确保不和其他数据冲突，这个复杂度不划算。

## 2MB 大页：跳过 PT 的捷径

好在 x86_64 提供了一个简化方案：大页。Intel SDM Vol.3A §4.5.1 描述了两种大页——2MB（在 PD 条目中设置 PS=1）和 1GB（在 PDPT 条目中设置 PS=1）。设置 PS 位后，该条目不再指向下一级页表，而是直接映射一个大的物理页，跳过下面所有层级。

Cinux 用的是 2MB 大页。具体来说，PD 条目的 bit 7（PS，Page Size）设为 1，bit 0（Present）和 bit 1（Writable）也设为 1，剩下的位存储物理页基地址。由于 2MB 页必须 2MB 对齐，基地址的低 21 位全部为 0（bit 0-12 是标志位，bit 13-20 也保留为 0），只需要存储 bit 21 以上的地址位。一个 64 位的 PD 条目中，bit 21-39（或更高，取决于 MAXPHYADDR）存储物理基地址的高位部分，bit 12-7 是标志位，bit 0-5 也是标志位。

这样我们只需要 3 张表就够了：PML4、PDPT、PD。PT 被完全跳过。3 页 = 12KB，映射 8MB 物理内存，每张表只需要设置少量条目，复杂度大幅降低。OSDev Wiki 的 [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode) 教程也是用这个方案，说明这是社区公认的 Bootloader 最佳实践。

## Cinux 的页表实现

现在我们来看具体的代码。整个页表构建封装在 `setup_page_tables` 函数中，位于 `boot/common/long_mode.S`。

### 常量与地址布局

```asm
.set PML4_PHYS_ADDR,      0x1000
.set PDPT_PHYS_ADDR,      0x2000
.set PD_PHYS_ADDR,        0x3000

.set PAGE_PRESENT,        0x01
.set PAGE_WRITABLE,       0x02
.set PAGE_LARGE,          0x80
.set PAGE_FLAGS,          (PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE)
```

三张表的物理地址选在 0x1000、0x2000、0x3000，紧凑排列，每张占 4KB。0x0000-0x04FF 被 IVT 和 BIOS 数据区占用，0x1000 开始是空闲区。0x5000-0x5FFF 是 E820 内存映射，0x6000-0x64FF 是 VESA 信息，0x8000 开始是 stage2 代码——这些都不和页表冲突。PAGE_FLAGS = 0x83（Present + Writable + Large），这是 PD 大页条目的标准标志组合。

### 清零三张表

```asm
setup_page_tables:
    cld
    movl $PML4_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl

    movl $PDPT_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl

    movl $PD_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl
```

`rep stosl` 是 x86 的块操作指令，意思是把 EAX 的值重复写入 EDI 指向的内存 ECX 次，每次写入后 EDI 自动递增 4 字节。EAX=0、ECX=1024，效果是把 EDI 指向的 4096 字节全部清零。`cld` 清除方向标志位确保是递增写入——虽然正常情况下 DF 应该是 0，但在函数入口再确认一次是好的习惯。

你可能注意到页表条目是 64 位的，但我们用 32 位的 `stosl` 写 0 来清零。连续写两次 32 位的 0 等价于写一次 64 位的 0，所以没问题。清零这一步很关键——页表中未使用的条目必须是全零（Present 位为 0 表示"不存在"），否则 CPU 在查表时可能把残留的垃圾数据当成有效的条目。

### 建立层级链接

```asm
    movl $PDPT_PHYS_ADDR, %eax
    orl $0x03, %eax
    movl %eax, PML4_PHYS_ADDR

    movl $PD_PHYS_ADDR, %eax
    orl $0x03, %eax
    movl %eax, PDPT_PHYS_ADDR
```

PML4[0] = 0x2000 | 0x03 = 0x2003，意思是"PML4 的第 0 个条目指向物理地址 0x2000 处的 PDPT，并且 Present=1、Writable=1"。PDPT[0] = 0x3000 | 0x03 = 0x3003，指向 PD。这里用 `movl` 只写了低 32 位，高 32 位保持为 0（之前清零过了）——对于 4GB 以下的物理地址这是正确的，Intel SDM Vol.3A §4.5 要求页表条目中未实现的保留位必须为 0。

为什么只设第 0 个条目？PML4[0] 覆盖的虚拟地址范围是 0x0000000000000000 到 0x0000007FFFFFFFFF，也就是第一个 512GB。PDPT[0] 在这个范围内覆盖第一个 1GB。这对 Bootloader 阶段完全足够——我们的代码、栈、页表全部在低地址。

### 填充 2MB 大页条目

```asm
    movl $PD_PHYS_ADDR, %edi
    movl $4, %ecx
    xorl %eax, %eax

1:
    movl %eax, %edx
    shll $21, %edx
    orl $PAGE_FLAGS, %edx
    movl %edx, (%edi)
    addl $8, %edi
    incl %eax
    loop 1b
    ret
```

这个循环设置 PD 的前 4 个条目，每个条目映射一个 2MB 大页。循环变量 EAX 从 0 开始，每次迭代把 EAX 左移 21 位得到物理基地址（i × 2MB），然后 OR 上 0x83 标志，写入 EDI 指向的位置。EDI 每次前进 8 字节（条目大小），EAX 自增，ECX 控制循环 4 次。

四次循环产生的条目是：PD[0] = 0x83（映射 0-2MB），PD[1] = 0x200083（映射 2-4MB），PD[2] = 0x400083（映射 4-6MB），PD[3] = 0x600083（映射 6-8MB）。全部是恒等映射——虚拟地址等于物理地址。8MB 对 Bootloader 阶段绰绰有余：stage2 代码在 0x8000 附近，栈在 0x90000，页表本身在 0x1000-0x3FFF，全部在映射范围内。

这个循环只写了每个条目的低 32 位。高 32 位呢？我们已经用 `rep stosl` 把整页清零了，所以高 32 位保持为 0——这正是我们需要的，因为这些 2MB 页的物理地址都在 8MB 以下，高 32 位确实是 0。但这里有一个要注意的点：如果以后要映射 4GB 以上的物理地址（比如映射 PCIe 的 MMIO 空间），就必须额外写一条指令来设置条目的高 32 位，因为 `movl` 只能写 32 位。

## Linux 是怎么做的——对比分析

聊完了我们的实现，退后一步看看生产级 OS 怎么处理同一个问题。Linux kernel 的 `arch/x86/boot/compressed/head_64.S` 中有一个对应的函数负责在启动早期建立 Long Mode 页表。

Linux 的页表构建比 Cinux 复杂得多。它映射了整整 4GB 的地址空间——用 2048 个 2MB 大页条目（4GB / 2MB = 2048），分布在 4 张 PD 表中，加上 1 张 PML4 和 1 张 PDPT，总共 6 页 = 24KB。条目标志是 0x183 = P(1) + R/W(2) + PS(0x80) + Global(0x100)——比我们的 0x83 多了一个 Global 位（bit 8），让这些页表条目在 CR3 切换时不会被 TLB 刷新清除。Cinux 只映射 8MB（4 个条目），Linux 映射 4GB（2048 个条目），差距是 500 倍。

这个差异的根源在于目标不同。Cinux 的 Bootloader 在 Long Mode 下只做一件事：跳转到 C++ 内核入口。内核自己会建立完整的页表映射，所以 Bootloader 只需要"刚好够跑"的最低配置。Linux 的 early boot 阶段需要在 Long Mode 下做更多事情（解压内核、重定位、可能的 fixmap），所以需要更大的映射范围。

另一个有趣的差异是地址计算方式。Linux 使用位置无关代码（Position-Independent Code），页表地址不是硬编码的固定值，而是通过 `leal` 相对于当前指令位置计算出来的。这样内核镜像可以加载到任意物理地址上运行，不需要固定加载地址。Cinux 简单粗暴地用固定地址 0x1000-0x3FFF——因为我们控制了 MBR 的加载逻辑，知道 stage2 一定在 0x8000，页表放在 0x1000 绝对不会冲突。

在开启分页的方式上也有区别。Linux 用了一个 `lret`（long return）技巧：先把目标 CS 和 EIP 压栈，然后开启 CR0.PG，最后 `lret` 从栈上弹出——这等价于一条远跳转，但不需要把目标地址编码在指令中。Cinux 直接用 `ljmp`，更直观但也更受限（目标地址在指令中是立即数）。

这些差异反映的是一个核心权衡：Bootloader 的设计空间是"够用就行"还是"尽量通用"。Cinux 选了前者——固定地址、最小映射、最简流程；Linux 选了后者——位置无关、大范围映射、兼容各种启动环境。两种选择在各自的场景下都是合理的。

## 收尾

到这里页表构建部分就完成了。验证方式很直观——在 GDB 中用 `x/8gx 0x1000`、`x/8gx 0x2000`、`x/8gx 0x3000` 分别查看三张页表的内容，确认 PML4[0] = 0x2003、PDPT[0] = 0x3003、PD[0..3] = 0x83/0x200083/0x400083/0x600083。如果值对不上，回头检查清零和填充的逻辑——最常见的问题是清零不够彻底导致残留数据污染了页表条目。

下一篇我们将利用这套页表真正把 CPU 带入 Long Mode——CR4.PAE、EFER.LME、CR0.PG 的状态机切换，64 位 GDT 描述符的 L 位和 D 位，以及那个绝对不能省略的远跳转。那一篇里的坑比这篇更多更隐蔽，尤其是 EFER.LME 的位定义——那个 0x100 vs 0x1000 的坑，足够让你怀疑人生。

## 参考资料

- Intel SDM Vol.3A §4.1 — Paging Overview：Long Mode 下分页的强制性，分段机制的废弃
- Intel SDM Vol.3A §4.5 — 4-Level Paging：PML4/PDPT/PD/PT 层级结构，虚拟地址拆解，2MB 大页翻译过程（Figure 4-9）
- Intel SDM Vol.3A §4.5.1 — Page Size Extensions：PS 位在不同页表级别中的含义，2MB 和 1GB 大页
- Intel SDM Vol.3A §10.8.5 — Initializing IA-32e Mode：完整的 Long Mode 初始化步骤
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode) — 页表构建的完整示例，使用相同的 2MB 大页方案
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging) — 64 位分页的详细描述，页表条目格式
- Linux kernel: [head_64.S](https://0xax.gitbooks.io/linux-insides/content/Booting/linux-bootstrap-4.html) — Linux 的 Long Mode 页表构建实现
