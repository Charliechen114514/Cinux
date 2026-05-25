---
title: 003-boot-long-mode-1 · Long Mode
---

# 003-1 Long Mode 概念与页表设计

> 本章完成后的可见效果：QEMU 不崩溃，debugcon 输出 `L`（确认进入 64-bit long mode）
>
> 前置要求：已完成 `002_boot_gdt_protected`，理解保护模式切换流程和 GDT 基本概念

---

## 导语

上一章我们让 CPU 从实模式切到了保护模式，输出一个 `P` 字母宣告胜利。

但说实话，2026 年了还在 32 位保护模式里写内核，多少有点考古的意思——

寄存器只有 32 位宽，地址空间只有 4GB，各种特权级机制还停留在 80386 时代的设计。

要写一个"现代"的操作系统，我们必须进入 Long Mode（64 位模式）。

这一章的目标非常明确：从 32 位保护模式切换到 64 位 Long Mode，

并通过 debugcon 输出一个 `L` 字符确认成功。

听起来和上一章类似？没错，流程上确实有相似之处——都是设置一堆控制寄存器、做一次远跳转——

但 Long Mode 切换比保护模式切换复杂得多，因为 x86_64 架构强制要求开启分页，

而分页需要我们手工搭建一套四级页表结构。

任何一步出错，等待你的仍然是那个老朋友：三重故障。

本篇聚焦概念理解与页表设计。

下一篇讲解页表的动手构建，第三篇讲解模式切换的完整流程。

## 为什么必须进 Long Mode

保护模式下，通用寄存器最大 32 位（EAX、EBX...），地址线最多 32 根，寻址上限 4GB。

对于现代系统来说这个限制太紧了——哪怕只是做一个教学内核，

我们也希望能跑 64 位代码、用上 64 位寄存器（RAX、RSP 等），

而且后续 C++ 内核的编译器默认就是 64 位输出，不进 Long Mode 的话内核代码根本没法跑。

Long Mode 是 AMD 在 2003 年引入 x86_64 架构时定义的，后来被 Intel 采纳。

你可以把它理解为保护模式的"全面升级版"——

寄存器加倍到 64 位、地址空间理论上限提升到 256TB、指令集也做了扩展。

## Long Mode 是一个状态机

这一章最核心的认知是：进入 Long Mode 不是一个单一操作，而是一个严格有序的状态机。

必须按固定顺序满足四个前置条件，缺一个、错一个顺序，CPU 都会直接三重故障。

这四个条件和正确顺序是：

1. **开启 CR4.PAE**（Physical Address Extension，CR4 的 bit 5），让 CPU 支持 36 位（或更高）物理地址
2. **设置 EFER.LME**（MSR 0xC0000080 的 bit 8），告诉 CPU "我要进入 Long Mode"
3. **把 CR3 指向一套合法的四级页表**，因为 Long Mode 强制要求分页
4. **设置 CR0.PG**（bit 31）开启分页，CPU 在这一步真正激活 Long Mode

开启分页之后，CPU 自动进入 Compatibility Mode（兼容模式），此时还在执行 32 位代码。

只有当你做了一次远跳转、加载了一个 L=1 且 D=0 的代码段描述符到 CS 之后，

CPU 才真正进入 64 位模式。整个状态机的推进是单向的、不可跳步的。

## Long Mode 与分页的强制绑定

在保护模式下，分页是可选的——你可以只用分段机制做地址翻译（设好 GDT 的 Base 和 Limit），完全可以不开 CR0.PG。

但 Long Mode 把这条路堵死了。

Intel SDM Vol.3A §4.1 明确指出：在 64 位模式下，所有线性地址都必须通过页表翻译成物理地址，

分段机制的 Base/Limit 被忽略（除了 FS/GS 的 base 可以通过 MSR 做特殊用途）。

这意味着如果你想进入 Long Mode，就必须在 CR0.PG=1 之前准备好一套合法的页表结构，没有捷径。

原因大致有两方面：

首先是历史包袱的清理——

x86 的分段机制从 8086 时代一路沿用到保护模式，

各种段描述符的 Base/Limit/Access 组合极其复杂，和安全相关的漏洞层出不穷。

Long Mode 直接废弃了分段的地址翻译功能，统一用分页来管理内存，

简化了 CPU 的地址翻译逻辑和操作系统的内存管理代码。

其次是 64 位地址空间的需要——

分段机制最多只能描述 32 位段内偏移，而 64 位地址空间需要至少 48 位的有效虚拟地址，

只有页表的多级索引结构才能高效地表达这么大的地址空间。

## 四级页表与 2MB 大页

Long Mode 使用四级页表来翻译虚拟地址：

PML4（Page Map Level 4） → PDPT（Page Directory Pointer Table） → PD（Page Directory） → PT（Page Table）。

每一级有 512 个条目，每个条目 8 字节，所以一张表正好占一个 4KB 页。

虚拟地址被拆成四个 9 位索引（分别用于 PML4、PDPT、PD、PT 的查找）加上一个 12 位页内偏移，总共 48 位有效地址。

但这个四级结构有一个简化方案：

在 Page Directory 这一级，如果把条目的 PS（Page Size）位设为 1，就可以直接映射一个 2MB 的大页，跳过 PT 层级。

这对 Bootloader 阶段来说非常合适——

我们只需要映射前几 MB 的物理内存让代码跑起来，

用 2MB 大页只需要 3 张表（PML4、PDPT、PD），远比完整的四级结构简单。

Intel SDM Vol.3A §4.5.1 也推荐在初始化阶段使用大页来简化页表构建。

## Identity Mapping 与 Higher-Half Kernel

刚切换到 Long Mode 时，CPU 正在用物理地址执行代码。

如果虚拟地址到物理地址的映射不是恒等的（即虚拟地址 0x1000 不对应物理地址 0x1000），

CPU 就会立即找不到下一条指令，直接三重故障。

所以我们必须做 Identity Mapping（恒等映射）：虚拟地址等于物理地址。

这是一种"过渡方案"，后续内核初始化完成后可以建立更复杂的映射关系。

但 Cinux 同时还做了一个 Higher-Half Kernel 映射：

把虚拟地址 0xFFFFFFFF80000000 映射到物理地址 0x00000000。

这样内核代码运行在虚拟地址空间的高地址区域，而低地址区域留给用户进程。

具体来说，这个映射通过 PML4[511]（最后一条）指向 PDPT，PDPT[510] 指向 PD

（注意是 510 而不是 511，因为 0x80000000 在 PDPT 索引中 bit 30 为 0），

PD[0] 就是那条恒等映射 0-2MB 的大页条目。

这样内核就可以通过两个不同的虚拟地址访问同一段物理内存：

低地址的恒等映射和高地址的 Higher-Half 映射。

这个 Higher-Half 映射是后续内核真正使用的映射路径。

## 页表地址布局的设计考量

三张表的物理地址选在 0x1000、0x2000、0x3000，紧凑排列，每张占 4KB。

为什么选这里？0x0000-0x04FF 被 IVT（中断向量表）和 BIOS 数据区占用，

0x0500-0x7FFF 基本空闲，所以从 0x1000 开始放页表是安全的。

每张表占 4KB，三张表总共 12KB，占用 0x1000-0x3FFF 这个范围，

和后续的 E820 内存映射（0x5000 起）、VESA 信息（0x6000 起）、stage2 代码（0x8000 起）都不冲突。

PAGE_FLAGS 把 Present、Writable、Large 三个位组合在一起，值为 0x83，

这是 PD 大页条目的标准标志组合。

## EFER.LME 的坑——务必记住

这一章有一个我们实打实踩过的坑，需要提前警告：

EFER_LME 的值是 0x100（bit 8），绝对不能写成 0x1000。

0x1000 对应的是 bit 12，即 AMD 的 SVME（Secure Virtual Machine Enable）位。

如果写错了，CPU 不会报错——`wrmsr` 会很乖地设置 SVME 位而不是 LME 位——

但当你随后开启分页时，CPU 发现 LME 没设却开了分页，直接三重故障。

这个坑在 GDB 里看到 EFER=0x1000 只有 SVME 没有 LME 才定位到，

调试难度真的让人血压拉满。

## 本篇小结

到这里概念部分就清楚了：

Long Mode 强制要求分页，四级页表用 2MB 大页简化到只需要 3 张表，

页表放在 0x1000-0x3FFF 的固定物理地址上，同时建立恒等映射和 Higher-Half 映射。

下一篇我们将动手实现 `setup_page_tables` 函数，把这套页表真正建起来。

| 概念 | 要点 |
|------|------|
| Long Mode 状态机 | PAE → LME → CR3 → PG，严格有序 |
| 四级页表 | PML4 → PDPT → PD → PT，每级 512 条目 × 8 字节 = 4KB |
| 2MB 大页 | PD 条目设 PS=1（0x80），跳过 PT 层级 |
| Identity Mapping | 虚拟地址 = 物理地址，Bootloader 阶段必须 |
| Higher-Half Mapping | PML4[511]→PDPT[510]→PD[0]，虚拟 0xFFFFFFFF80000000 → 物理 0x0 |
| EFER.LME | MSR 0xC0000080 的 bit 8，值 0x100，不是 0x1000 |
| 页表布局 | PML4=0x1000, PDPT=0x2000, PD=0x3000 |

## 参考资料

- Intel SDM Vol.3A §4.1 — Paging Overview：Long Mode 下分页的强制性
- Intel SDM Vol.3A §4.5 — 4-Level Paging：PML4/PDPT/PD/PT 层级结构
- Intel SDM Vol.3A §4.5.1 — Page Size Extensions：2MB 和 1GB 大页
- Intel SDM Vol.3A §10.8.5 — Initializing IA-32e Mode：初始化步骤
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging)
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
