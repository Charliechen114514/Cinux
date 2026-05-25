---
title: 003-boot-long-mode-3 · Long Mode
---

# 003-3 Higher-Half Kernel 映射：PML4[511] 与 PDPT[510] 完整代码讲解

> 标签：x86_64, 长模式, 高半内核, Higher-Half, PML4[511], PDPT[510]
> 前置：[003-2 模式切换](003-boot-long-mode-2.md)

## 概览

前两篇我们讲了页表构建的基础部分（恒等映射 0-8MB）和模式切换的五步状态机。

但如果你仔细看了 `boot/common/long_mode.S` 中 `setup_page_tables` 函数的完整代码，

会发现 `ret` 指令之前还有一段我们没有讲到的代码——那就是 Higher-Half Kernel 映射。

这段代码在恒等映射的基础上，

额外设置了 PML4[511]→PDPT 和 PDPT[510]→PD 的链接，

把虚拟地址 0xFFFFFFFF80000000 映射到物理地址 0x00000000，

让内核可以运行在高半地址空间。

本篇专门讲解这段代码的实现细节，

包括虚拟地址的二进制拆分、为什么 PDPT 索引是 510 而不是 511、

以及 x86-64 页表条目必须显式写高 32 位的原因。

关键设计决策：

复用已有的 PD 表（PD[0] 已经映射了 0-2MB 物理内存），

只添加两个链接条目就实现了 Higher-Half 映射；

显式写高 32 位为 0 以确保页表条目在 64 位视角下完全合法。

## Higher-Half 的动机

操作系统的内核通常运行在虚拟地址空间的高地址区域，把低地址区域留给用户进程。

Linux 就是这么做的——内核映射在 0xFFFFFFFF80000000 以上

（或者 0xFFFF888000000000 的直接映射区）。

这样做的好处是用户空间和内核空间有清晰的分界线，用户程序不会意外访问内核内存。

Cinux 从一开始就瞄准了这个设计。

内核被加载到物理地址 0x20000（在第一个 2MB 大页范围内），

但它的入口点被设定为 0xFFFFFFFF80020000——

也就是说，内核代码期望运行在虚拟地址 0xFFFFFFFF80020000 上。

为了在 Bootloader 阶段就让这个地址可用，我们需要在页表中建立 Higher-Half 映射。

## 虚拟地址的二进制拆分

要理解 PML4[511] 和 PDPT[510] 是怎么来的，

我们必须把虚拟地址 0xFFFFFFFF80000000 拆开来看。

x86-64 的四级页表使用 48 位有效虚拟地址

（高 16 位是符号扩展，即 canonical address 要求）。

48 位被拆分为四段 9 位索引加一段 12 位页内偏移：

```
虚拟地址 0xFFFFFFFF80000000 的二进制：
0xFFFFFFFF80000000 = 0xFFFF_FFFF_8000_0000

高 16 位（符号扩展）: 0xFFFF
PML4 索引 (bit 47:39):  0x1FF = 511 (全1)
PDPT 索引 (bit 38:30):  0x1FE = 510 (注意！不是 511)
PD 索引   (bit 29:21):  0x000 = 0
PT 索引   (bit 20:12):  0x000 = 0
页内偏移  (bit 11:0):   0x000 = 0
```

PDPT 索引为什么是 510 而不是 511？关键在于 bit 30。

0x80000000 在 bit 38:30 的范围是：

```
0x80000000 的二进制展开（关注 bit 30）：
bit 38 = 1, bit 37 = 1, ..., bit 31 = 1, bit 30 = 0
PDPT 索引 = 1 1111 1110 = 0x1FE = 510
```

所以 PDPT 索引是 510。这一点非常反直觉——

你可能以为 PML4 是最后一条（511），那 PDPT 也应该是最后一条（511），

但实际上 0x80000000 这个地址在 PDPT 索引上 bit 30 为 0，导致索引是 510 而不是 511。

如果错误地把 PDPT[511] 设成指向 PD，

对应的虚拟地址会是 0xFFFFFFFFC0000000 而不是 0xFFFFFFFF80000000——差了整整 1GB。

## 代码实现

### PML4[511] → PDPT

```asm
    // PML4[511] -> PDPT (same as PML4[0])
    movl $PDPT_PHYS_ADDR, %eax       // [imm->eax] PDPT physical address
    orl $0x03, %eax                   // [imm->eax] Add present+writable flags
    movl %eax, PML4_PHYS_ADDR + (511 * 8)  // PML4[511] low 32 bits
    movl $0, PML4_PHYS_ADDR + (511 * 8) + 4  // PML4[511] high 32 bits = 0
```

PML4[511] 的偏移是 PML4_PHYS_ADDR + (511 * 8) = 0x1000 + 4088 = 0x1FF8。

这行代码做的事情和设置 PML4[0] 几乎一样——

把 PDPT 的物理地址加上 Present+Writable 标志写入条目——

但有一个关键区别：这里显式地写了高 32 位为 0。

为什么要显式写高 32 位？

因为 PML4[511] 是一个"非常用"条目——

它在页表清零阶段确实被清零了，但前面的恒等映射部分用 `movl` 写 PML4[0] 时，

高 32 位是靠清零保证的。

对于 PML4[511] 来说，虽然清零也保证了高 32 位为 0，

但显式写入是一个更安全、更明确的做法——

它表明"我确实知道这个条目的高 32 位应该是 0"，而不是依赖一个隐含的前提。

Intel SDM Vol.3A §4.5 明确要求页表条目中保留位必须为 0，否则会触发 #GP，

所以显式写 0 是防御性编程。

PML4[511] 和 PML4[0] 指向同一个 PDPT 表（0x2000）。

这是有意为之的设计——

恒等映射和 Higher-Half 映射共享同一套 PDPT 和 PD 表，

这样只需要在 PDPT 中额外设置一个条目（PDPT[510]）就完成了整个 Higher-Half 映射，

不需要分配额外的页表页面。

### PDPT[510] → PD

```asm
    // PDPT[510] -> PD (NOT 511! 0x80000000 has bit 30 = 0)
    movl $PD_PHYS_ADDR, %eax         // [imm->eax] PD physical address
    orl $0x03, %eax                   // [imm->eax] Add present+writable flags
    movl %eax, PDPT_PHYS_ADDR + (510 * 8)  // PDPT[510] low 32 bits
    movl $0, PDPT_PHYS_ADDR + (510 * 8) + 4  // PDPT[510] high 32 bits = 0
```

PDPT[510] 的偏移是 PDPT_PHYS_ADDR + (510 * 8) = 0x2000 + 4080 = 0x2FE0。

代码注释特别强调了 "NOT 511"——这个 off-by-one 错误是实际开发中很容易犯的。

设置完 PDPT[510] 指向 PD 后，Higher-Half 映射的路径就完整了：

```
虚拟地址 0xFFFFFFFF80000000
    ↓ PML4[511]
PDPT @ 0x2000
    ↓ PDPT[510]
PD @ 0x3000
    ↓ PD[0] = 0x83 (2MB page, maps 0x00000000 - 0x001FFFFF)
物理地址 0x00000000 - 0x001FFFFF ✓
```

PD[0] 是在恒等映射阶段已经设好的条目（值 0x83，映射 0-2MB 物理内存），

Higher-Half 映射直接复用了这个条目。

所以内核的虚拟入口地址 0xFFFFFFFF80020000 会被翻译成物理地址 0x20000

（0x80000000 落在 PD[0] 映射的 0-2MB 范围内，页内偏移是 0x20000）。

## 映射关系总结

完成 Higher-Half 映射后，完整的页表结构是这样的：

```
PML4 (0x1000):
  [0]   = 0x2003 → PDPT @ 0x2000  (恒等映射路径)
  [511] = 0x2003 → PDPT @ 0x2000  (Higher-Half 映射路径，共享同一张 PDPT)

PDPT (0x2000):
  [0]   = 0x3003 → PD @ 0x3000    (恒等映射路径)
  [510] = 0x3003 → PD @ 0x3000    (Higher-Half 映射路径，共享同一张 PD)

PD (0x3000):
  [0] = 0x83      → 0-2MB         (恒等映射 + Higher-Half 共享)
  [1] = 0x200083  → 2-4MB         (恒等映射)
  [2] = 0x400083  → 4-6MB         (恒等映射)
  [3] = 0x600083  → 6-8MB         (恒等映射)
```

一个物理页面（PD[0]）可以通过两条不同的路径访问：

低地址的恒等映射（虚拟地址 0x0-0x1FFFFF）和高地址的 Higher-Half 映射

（虚拟地址 0xFFFFFFFF80000000-0xFFFFFFFF801FFFFF）。

这意味着内核代码可以在两种地址下运行——

在 Bootloader 刚进入 Long Mode 时用恒等映射地址执行，

后续切换到 Higher-Half 映射后用高地址执行。

这种双映射的设计在操作系统的早期启动阶段非常常见，

因为 CPU 在切换页表映射时必须始终有一条"能继续执行"的路径。

## 设计决策

### 决策：共享 PDPT 和 PD 而不是分配独立的页表

**问题**: Higher-Half 映射需要一套独立的页表还是可以复用恒等映射的？

**本项目的做法**:

复用同一张 PDPT 和 PD，只额外设置 PML4[511] 和 PDPT[510] 两个条目。

**备选方案**: 为 Higher-Half 映射分配独立的 PDPT 和 PD 页面。

**为什么不选备选方案**:

复用页表节省了内存（不需要额外的 8KB 页面），而且代码更简单——

只需要写两个条目而不是重建整张表。

共享 PDPT 和 PD 的前提是恒等映射和 Higher-Half 映射指向同一段物理内存（0-2MB），

这恰好是我们需要的行为——内核加载在物理地址 0x20000，两个映射路径都能访问到它。

### 决策：显式写高 32 位为 0

**问题**: x86-64 页表条目是 64 位的，但我们在 32 位模式下只能用 `movl` 写 32 位。高 32 位怎么处理？

**本项目的做法**:

对于 PML4[511] 和 PDPT[510]，在写低 32 位之后，显式用 `movl $0, offset+4` 写高 32 位为 0。

**备选方案**: 依赖前面的清零操作——清零已经把高 32 位设为 0 了，不需要再写。

**为什么不选备选方案**:

虽然依赖清零在功能上是正确的，

但显式写 0 的好处是代码的意图更清晰——

读者一眼就能看出"我确实知道高 32 位应该是 0"，而不是需要回头去确认"前面是不是清过零了"。

Intel SDM 要求页表条目中的保留位必须为 0，

显式写 0 是对这一要求的直接回应，减少了将来维护代码时的认知负担。

值得注意的是，PML4[0] 和 PDPT[0] 没有显式写高 32 位——它们依赖清零——

这是一种历史遗留的不一致性。理想情况下所有条目都应该显式写高 32 位。

## 扩展方向

- 用 Higher-Half 映射而非恒等映射执行 `long_mode_entry` 后的代码：

  需要确保远跳转的目标地址使用正确的映射路径

- 扩展 Higher-Half 映射范围：

  PD[1..3] 已经映射了 2-8MB，

  所以 Higher-Half 映射自然覆盖了 0xFFFFFFFF80200000-0xFFFFFFFF807FFFFF

- 在后续内核中，内核会建立自己的完整 Higher-Half 页表映射，

  不再依赖 Bootloader 的临时页表

## 参考资料

- Intel SDM Vol.3A §4.5 — 4-Level Paging：PML4/PDPT/PD 层级结构，页表条目格式
- Intel SDM Vol.3A §4.5.1 — Page Size Extensions：2MB 大页的基地址计算
- Intel SDM Vol.3A §10.8.5 — Initializing IA-32e Mode：完整的 Long Mode 初始化步骤
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — Higher-Half 内核设计
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging) — 64 位分页的详细描述
