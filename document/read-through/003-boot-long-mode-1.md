---
title: 003-boot-long-mode-1 · Long Mode
---

# 003-1 页表构建：setup_page_tables() 完整代码讲解

> 标签：x86_64, 长模式, 四级页表, 2MB 大页, 恒等映射
> 前置：[002 保护模式切换](../tutorial/002-boot-gdt-protected-2.md)

## 概览

本文讲解 `boot/common/long_mode.S` 中的 `setup_page_tables` 函数——整个 Long Mode 切换的前半段。这个函数的职责很单纯：在固定的物理地址上搭建一套三级页表结构（PML4 → PDPT → PD），用 2MB 大页恒等映射前 8MB 物理内存。代码量不大，但每一行都和 x86_64 的分页机制紧密相关，理解这套页表怎么建，是理解 Long Mode 为什么"强制要求分页"的基础。

关键设计决策一览：固定地址布局（PML4 在 0x1000、PDPT 在 0x2000、PD 在 0x3000）、2MB 大页而非 4KB 小页、只映射 8MB 的极简策略、用 `rep stosl` 批量清零。

## 内存布局图

```
物理内存布局（页表区域）：

0x0000 ┌──────────────────┐
       │   IVT / BDA      │
0x1000 ├──────────────────┤ ← PML4 (4KB, 512 entries × 8B)
       │   PML4[0]→0x2003 │   其余全零
0x1FFF ├──────────────────┤
0x2000 ├──────────────────┤ ← PDPT (4KB, 512 entries × 8B)
       │   PDPT[0]→0x3003 │   其余全零
0x2FFF ├──────────────────┤
0x3000 ├──────────────────┤ ← PD (4KB, 512 entries × 8B)
       │   PD[0] = 0x0083 │   → 0x00000000 - 0x001FFFFF (0-2MB)
       │   PD[1] = 0x200083│  → 0x00200000 - 0x003FFFFF (2-4MB)
       │   PD[2] = 0x400083│  → 0x00400000 - 0x005FFFFF (4-6MB)
       │   PD[3] = 0x600083│  → 0x00600000 - 0x007FFFFF (6-8MB)
       │   ...             │   其余全零
0x3FFF ├──────────────────┤
0x5000 │  E820 内存映射    │
0x6000 │  VESA 信息       │
0x8000 │  Stage2 代码     │
       └──────────────────┘
```

地址翻译流程：

```
虚拟地址 0x00000000_00345678
         ↓
PML4[0] ─→ PDPT 表 @ 0x2000
         ↓
PDPT[0] ─→ PD 表 @ 0x3000
         ↓
PD[1] (PS=1, 2MB页) ─→ 物理基址 0x00200000
         ↓
页内偏移: 0x145678
         ↓
物理地址: 0x00345678  (= 0x00200000 + 0x145678) ✓ 恒等映射
```

## 常量定义

```asm
// Page table physical addresses (fixed layout)
.set PML4_PHYS_ADDR,      0x1000      // PML4 table location
.set PDPT_PHYS_ADDR,      0x2000      // PDPT table location
.set PD_PHYS_ADDR,        0x3000      // Page Directory location

// Page table entry flags
.set PAGE_PRESENT,        0x01        // Present bit
.set PAGE_WRITABLE,       0x02        // Read/Write bit
.set PAGE_LARGE,          0x80        // Page Size bit (2MB pages)

// Page entry with flags: present + writable + large
.set PAGE_FLAGS,          (PAGE_PRESENT | PAGE_WRITABLE | PAGE_LARGE)
```

三个页表地址选在 0x1000、0x2000、0x3000，这是一个精心挑选的布局。0x0000-0x04FF 被 IVT（中断向量表）和 BIOS 数据区占用，0x0500-0x7FFF 基本空闲，所以从 0x1000 开始放页表是安全的。每张表占 4KB，三张表总共 12KB，占用 0x1000-0x3FFF 这个范围，和后续的 E820 内存映射（0x5000 起）、VESA 信息（0x6000 起）、stage2 代码（0x8000 起）都不冲突。

`PAGE_FLAGS` 把 Present、Writable、Large 三个位组合在一起，值为 0x83。Present（bit 0）表示这个页表条目在内存中有效，Writable（bit 1）表示可写，Large（bit 7）表示这是一个 2MB 大页而不是指向下一级页表的指针。这三个标志在 PD 条目中使用，告诉 CPU"这是一个 2MB 的物理页，存在且可写"。

另外还有一组控制位常量，虽然不属于 `setup_page_tables` 函数本身，但在同文件中定义：

```asm
// MSR addresses
.set MSR_EFER,            0xC0000080  // Extended Feature Enable Register

// EFER bits
.set EFER_LME,            0x100         // Long Mode Enable (bit 8)

// Control register bits
.set CR4_PAE,             0x20        // PAE enable (bit 5)
.set CR0_PG,              0x80000000  // Paging enable (bit 31)
.set CR0_PE,              0x01        // Protected mode enable (bit 0)
```

EFER_LME = 0x100 是 bit 8 的值。这一点必须反复强调：千万不要写成 0x1000。0x1000 是 bit 12，对应 AMD 的 SVME 位，和 Long Mode 完全无关。我们开发时确实踩过这个坑，`wrmsr` 很乖地设置了错误的位而不报错，然后 CPU 在开启分页时直接三重故障，GDB 里看到 EFER=0x1000 而不是 0x100，才定位到是常量定义写错了一位。

## 清零三张页表

```asm
.global setup_page_tables
setup_page_tables:
    // clear df again
    cld
    // Clear PML4 table at 0x1000
    movl $PML4_PHYS_ADDR, %edi       // Destination = PML4 address
    xorl %eax, %eax                   // Value to write = 0
    movl $1024, %ecx                  // Count = 4096/4 = 1024 dwords
    rep stosl                         // Zero PML4, count=ecx

    // Clear PDPT table at 0x2000
    movl $PDPT_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl

    // Clear PD table at 0x3000
    movl $PD_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl
```

`cld` 清除方向标志位（Direction Flag），确保 `stosl` 按 EDI 递增的方向写入。这一步看似多余——前面某处可能已经 `cld` 过了——但 `rep stosl` 的行为依赖 DF 位，而函数调用过程中 DF 的状态不一定可靠，所以在函数入口再清一次是防御性编程。

接下来三段代码结构完全一样，分别清零 PML4（0x1000）、PDPT（0x2000）、PD（0x3000）。每张表 4KB = 1024 个 32 位双字。`rep stosl` 的意思是：把 EAX 的值写入 ES:EDI 指向的内存地址，然后 EDI 自动 +4，重复 ECX 次。EAX=0、ECX=1024，效果就是把目标地址开始的 4096 字节全部写成 0。

你可能注意到页表条目是 64 位的（8 字节），但我们用 32 位的 `stosl` 清零——这没问题，因为 `stosl` 写入 0，连续写两次 32 位的 0 等价于写入一次 64 位的 0。1024 个 `stosl` 清零 4096 字节，正好覆盖 512 个 64 位条目。

## 建立层级链接

```asm
    // Setup PML4[0] -> PDPT
    movl $PDPT_PHYS_ADDR, %eax       // PDPT physical address
    orl $0x03, %eax                   // Add present+writable flags
    movl %eax, PML4_PHYS_ADDR        // PML4[0]=PDPT|flags

    // Setup PDPT[0] -> PD
    movl $PD_PHYS_ADDR, %eax         // PD physical address
    orl $0x03, %eax                   // Add present+writable flags
    movl %eax, PDPT_PHYS_ADDR        // PDPT[0]=PD|flags
```

这两段代码做的事情完全对称：把下一级页表的物理地址加上 Present+Writable 标志（0x03），写入当前级页表的第 0 个条目。PML4[0] 写入 `0x2000 | 0x03 = 0x2003`，PDPT[0] 写入 `0x3000 | 0x03 = 0x3003`。

这里有一个容易忽略的细节：我们用 `movl` 做的是 32 位写入，只写了条目的低 32 位。但页表条目是 64 位的——高 32 位怎么办？答案是：我们前面已经用 `rep stosl` 把整页清零了，所以高 32 位保持为 0。对于 8MB 以下的物理地址，高 32 位本来就是 0，不需要额外处理。Intel SDM Vol.3A §4.5 明确指出，页表条目中未实现的高位保留位必须为 0，否则会触发 #GP。我们的清零操作恰好保证了这一点。

为什么只设置第 0 个条目？因为 PML4[0] 覆盖的虚拟地址范围是 0x0000000000000000 到 0x0000007FFFFFFFFF（即第一个 512GB 区域），对于 Bootloader 阶段来说完全足够。PDPT[0] 覆盖的是这个 512GB 区域中的第一个 1GB，同样足够。

## 填充 PD 大页条目

```asm
    // Setup PD[0..3] for 2MB pages (identity map 0-8MB)
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

这是整个函数中最精巧的部分。EDI 指向 PD 表的起始地址（0x3000），ECX=4 控制循环 4 次，EAX 从 0 开始作为页索引。

循环体的逻辑是：把 EAX 的值复制到 EDX，左移 21 位得到物理基地址（因为每个大页 2MB = 2^21 字节，所以页 i 的基地址是 i × 2MB = i << 21），然后用 OR 加上 PAGE_FLAGS（0x83 = Present+Writable+Large），写入 EDI 指向的位置。每次写入后 EDI 前进 8 字节（因为每个 PD 条目是 64 位），EAX 自增。

四次循环的结果：
- EAX=0: `0 << 21 | 0x83 = 0x0083` → 映射 0x00000000-0x001FFFFF
- EAX=1: `1 << 21 | 0x83 = 0x200083` → 映射 0x00200000-0x003FFFFF
- EAX=2: `2 << 21 | 0x83 = 0x400083` → 映射 0x00400000-0x005FFFFF
- EAX=3: `3 << 21 | 0x83 = 0x600083` → 映射 0x00600000-0x007FFFFF

四条 PD 大页条目覆盖了 0 到 8MB 的物理内存，全部是恒等映射——虚拟地址等于物理地址。8MB 对 Bootloader 阶段来说绰绰有余：stage2 代码在 0x8000 附近，栈在 0x90000，页表本身在 0x1000-0x3FFF，全部在这个范围内。后续内核启动后会建立更完善的页表映射，这套临时页表只是"让 CPU 能在 Long Mode 下继续执行"的最低配置。

函数最后 `ret` 返回调用者（stage2.S 中的 pm_entry）。此时页表已经完全建好，CR3 还没有加载——那是 `enter_long_mode` 函数的事。

## 设计决策

### 决策：为什么用 2MB 大页而不是 4KB 小页

**问题**: 页表映射可以选 4KB 小页（四级页表完整使用）或 2MB 大页（跳过 PT 层级）。

**本项目的做法**: 使用 2MB 大页，只用三级（PML4、PDPT、PD），跳过 PT。

**备选方案**: 使用完整的四级页表，每个 PT 有 512 个 4KB 页条目。

**为什么不选备选方案**: 完整四级页表需要额外分配 PT 页面——映射 8MB 需要至少 4 张 PT（每张映射 2MB / 4KB = 512 页），加上 3 张上级表，总共 7 页 = 28KB。用 2MB 大页只需要 3 页 = 12KB，节省了一半以上的内存。更重要的是，Bootloader 阶段根本没有动态内存分配器，每多一页都要手工安排物理地址、确保不冲突，2MB 大页让复杂度大幅降低。Intel SDM Vol.3A §4.5.1 也推荐在初始化阶段使用大页来简化页表构建。

**如果要扩展**: 后续内核可以混用大页和小页——低地址区域用 2MB 大页做恒等映射保留给内核，高地址区域用 4KB 小页做精细的用户空间映射。甚至可以用 1GB 大页（PDPT 条目设 PS=1）来映射大段连续物理内存。

### 决策：为什么页表放在 0x1000-0x3FFF

**问题**: 页表的物理基地址需要页对齐（4KB 边界），且不能和已有的数据结构冲突。

**本项目的做法**: PML4=0x1000、PDPT=0x2000、PD=0x3000，紧凑排列。

**备选方案**: 放在更高地址，比如 0x70000-0x72FFF（栈下方）。

**为什么不选备选方案**: 0x1000-0x3FFF 这片区域在 BIOS 中断向量表和 BIOS 数据区之上、E820 内存映射之下，属于"没人用"的安全区域。连续排列的好处是地址好算——PML4+0x1000=PDPT、PDPT+0x1000=PD——不容易出错。OSDev Wiki 的 [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode) 教程也使用 0x1000 起始的布局，这是社区中的惯例做法。

## 扩展方向

- ⭐ 将映射范围从 8MB 扩展到 16MB（增加 PD[4..7]），验证 QEMU 仍能正常启动
- ⭐⭐ 在 PD 条目中加入 NX（No-Execute，bit 63）标志，观察对代码执行的影响——数据页设 NX、代码页不设
- ⭐⭐⭐ 实现一个二级恒等映射：除了 PML4[0] 的低地址映射，再设置 PML4[511] 做高半内核映射（虚拟地址 0xFFFFFFFF80000000 → 物理地址 0x00000000），这是后续 tag 会做的功能

## 参考资料

- Intel SDM Vol.3A §4.5 — 4-Level Paging：PML4/PDPT/PD/PT 层级结构，2MB 页翻译过程（Figure 4-9），页表条目格式
- Intel SDM Vol.3A §4.5.1 — 大页支持：PS 位在 PD 条目中的含义，2MB 和 1GB 页的基地址计算方式
- Intel SDM Vol.3A §10.8.5 — IA-32e Mode Initialization：初始化顺序，CR3 加载时机
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode) — 页表构建的完整示例
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging) — 64 位分页的详细描述
