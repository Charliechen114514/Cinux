# 从保护模式到 Long Mode：一次惨痛但收获满满的模式切换

> 标签：x86-64, bootloader, long mode, paging, identity mapping, GNU AS, AT&T 汇编, QEMU, 裸机开发

---

## 本章概览

上一章我们终于从 16 位实模式逃到了 32 位保护模式，输出了那个来之不易的 `P` 字母。当时你以为踩完保护模式的坑就能解脱了，但现实是 —— 保护模式只是中转站，真正的现代 x86-64 时代还在前面等着我们。说实话，写这一章的时候我经历了整个项目中最惨痛的一次调试，连续熬了好几个晚上，对着 GDB 的寄存器输出反复比对，才把那个让人崩溃的 bug 给揪出来。

为什么说这一章是最惨痛的？因为从保护模式到 Long Mode 的切换，涉及到 CPU 状态机的一次精密转换。任何一个寄存器设置错误，任何一个标志位写错，任何一步顺序不对，CPU 都会直接给你来个 triple fault，QEMU 窗口瞬间消失，连个错误信息都不给你留。这次我踩的坑是 EFER.LME 位写错了 —— 本来应该是 `0x100`（bit 8），我写成了 `0x1000`（bit 12，SVME 位），结果 CPU 根本没进入 Long Mode，开启分页后直接崩溃。

这一章的目标看似简单：设置页表，启用 PAE，设置 EFER.LME 位，开启分页，然后远跳转到 64 位代码，在 QEMU 的 debugcon 终端输出一个孤零零的字母 `L`。但这个简单的字母背后，藏着无数个可以让 CPU 直接跑飞的坑点。

**关键设计决策一览**：

- 使用固定地址的临时页表（0x1000-0x3FFF）简化早期内存管理
- 采用 2MB 大页减少页表层级，只需要 3 级页表就能映射前 8MB
- 严格遵循 PAE -> LME -> CR3 -> PG 的初始化顺序，任何顺序错误都会导致 triple fault
- Identity Mapping 确保模式切换时代码能继续执行
- 将 Long Mode 逻辑独立到 `boot/common/long_mode.S`，stage2 只负责调用
- 64 位 GDT 描述符必须设置 L=1、D=0，这是 Intel 硬件要求

与同类 OS 项目的设计对比：Linux 早期内核在 head_64.S 中完成了类似的 Long Mode 切换，但 Linux 需要处理多种启动场景和硬件配置。我们的实现更加直接，专注于 QEMU 环境的最小可用配置。xv6 则直接跳过了这些细节，使用 GRUB 进入 Long Mode。

---

## 架构图

```
+---------------------------------------------------------------------+
|                        物理内存布局                                   |
+---------------------------------------------------------------------+
|  0x00001000  +----------------------------------------------+       |
|              |  PML4 Table (4KB)                             |       |
|              |  [0] -> 0x2003 (PDPT + flags)                |       |
|              |  [1..511] = 0                                 |       |
|  0x00002000  +----------------------------------------------+       |
|              |  PDPT Table (4KB)                             |       |
|              |  [0] -> 0x3003 (PD + flags)                  |       |
|              |  [1..511] = 0                                 |       |
|  0x00003000  +----------------------------------------------+       |
|              |  PD Table (4KB)                               |       |
|              |  [0] -> 0x0000000000000083 (2MB page @ 0x0)   |       |
|              |  [1] -> 0x0000000000200083 (2MB page @ 2M)   |       |
|              |  [2] -> 0x0000000000400083 (2MB page @ 4M)   |       |
|              |  [3] -> 0x0000000000600083 (2MB page @ 6M)   |       |
|              |  [4..511] = 0                                 |       |
|  0x00008000  +----------------------------------------------+       |
|              |  Stage2 Bootloader                            |       |
|  0x00009000  +----------------------------------------------+       |
|              |  栈 (向下增长)                                  |       |
+---------------------------------------------------------------------+

|                        Long Mode 切换流程                            |
+---------------------------------------------------------------------+
|                                                                     |
|  Protected Mode (pm_entry)                                          |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call setup_page_tables   |  初始化页表                             |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call enter_long_mode     |  切换模式                               |
|  |  1. movl $0x1000, %cr3   |  加载 PML4 基址                        |
|  |  2. orl $0x20, %cr4      |  启用 PAE                              |
|  |  3. rdmsr / wrmsr        |  设置 EFER.LME=1 (bit 8!)              |
|  |  4. lgdt gdt64_ptr       |  加载 64-bit GDT                       |
|  |  5. orl $0x80000001, %cr0|  启用 PG+PE                            |
|  |  6. ljmp $0x18, entry    |  远跳转到 64-bit 代码                  |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  Long Mode (long_mode_entry)                                        |
|  - 设置数据段寄存器 (0x20)                                           |
|  - 设置 64-bit 栈指针 (0x90000)                                      |
|  - 输出 'L' (0xE9)                                                   |
|  - hlt 停机                                                          |
|                                                                     |
+---------------------------------------------------------------------+

|                        64-bit GDT 描述符格式                          |
+---------------------------------------------------------------------+
|                                                                     |
|  gdt_code64 (index 3, selector 0x18):                               |
|    .quad 0x00AF9A000000FFFF                                         |
|                                                                     |
|  关键位:                                                             |
|    L=1 (Long bit) 表示这是一个 64-bit 代码段                          |
|    D=0 (Default bit) 必须为 0（L=1 时 D 必须为 0）                   |
|                                                                     |
+---------------------------------------------------------------------+
```

---

## 环境说明

在开始踩坑之前，先说一下我的运行环境。这些细节对排查问题很重要：

- **平台**：WSL2 + QEMU system-x86_64 6.2+
- **工具链**：GNU AS（AT&T 语法），ld 链接器
- **调试方式**：QEMU debugcon（端口 0xE9）写入 debug.log + GDB 远程调试
- **前置要求**：已完成上一章的保护模式实现，能正常输出 `P`

调试方式这一章还是用 QEMU 的 debugcon 端口（0xE9），这是最轻量的调试手段。但这次我们还需要更强大的工具 —— GDB 远程调试，因为 triple fault 的时候 debugcon 什么都看不到。

看一下我们的 QEMU 调试配置（cmake/qemu.cmake）：

```cmake
set(QEMU_DEBUG_FLAGS
    -s
    -S
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
)
```

`-s -S` 让 QEMU 启动时暂停等待 GDB 连接，这是调试 Long Mode 切换的关键。

---

## 第一阶段 —— 理解 Long Mode：64 位时代的入场券

在保护模式下，你的 CPU 有 32 位寄存器、4GB 地址空间，但还算不上真正的"现代"。Long Mode 是 AMD 扩展的 x86-64 架构的核心，它引入了 64 位寄存器（RAX、RBX、RSP...）、64 位地址空间（理论上限 256TB）、以及新的操作模式。

### Long Mode 的前置条件

要进入 Long Mode，CPU 必须满足以下条件：

1. **CPU 支持 x86-64**：现代 CPU 都支持，但需要检查 CPUID
2. **CR4.PAE = 1**：必须先启用 PAE（Physical Address Extension）
3. **EFER.LME = 1**：设置 Long Mode Enable 位
4. **CR3 指向有效的 PML4 表**：页表基址必须 4KB 对齐
5. **CR0.PG = 1**：启用分页后，CPU 才真正进入 Long Mode

这些条件缺一不可，任何一个不满足都会导致 triple fault。

### EFER 寄存器：扩展功能启用寄存器

EFER（Extended Feature Enable Register）是一个 MSR（Model Specific Register），地址为 `0xC0000080`。我们用 `rdmsr` 和 `wrmsr` 指令读写它。

EFER 的重要位：

- **LME（Long Mode Enable，bit 8）**：值为 `0x100`，启用 Long Mode
- **SVME（SVM Enable，bit 12）**：值为 `0x1000`，AMD 虚拟化相关

这里有个超级重要的坑：**LME 是 bit 8，值是 0x100，不是 0x1000！** 我就是因为这里写错，踩了整个项目最惨的坑，下面会详细讲。

---

## 第二阶段 —— 页表结构：4 级页表的迷宫

Long Mode 要求 4 级页表结构：PML4 -> PDPT -> PD -> PT。但我们可以用 2MB 大页跳过 PT 级别，只需要 3 级页表就能映射前 8MB 内存。

### 页表布局

```
PML4 (0x1000)       PDPT (0x2000)        PD (0x3000)
+---------------+    +---------------+    +---------------+
| [0] -> 0x2003 | -> | [0] -> 0x3003 | -> | [0] -> 0x83   | -> 2MB page @ 0x0
| [1] = 0       |    | [1] = 0       |    | [1] -> 0x200083| -> 2MB page @ 2M
| ...           |    | ...           |    | [2] -> 0x400083| -> 2MB page @ 4M
+---------------+    +---------------+    | [3] -> 0x600083| -> 2MB page @ 6M
                                         | ...           |
                                         +---------------+
```

每个页表条目（PTE）占 8 字节，包含物理地址和标志位：

- **Present（bit 0）**：页在内存中，值 `0x01`
- **Writable（bit 1）**：页可写，值 `0x02`
- **Large（bit 7）**：2MB 大页，值 `0x80`

所以 `0x03` = Present + Writable，`0x83` = Present + Writable + Large。

---

## 第三阶段 —— 实现代码：long_mode.S 完整呈现

现在我们开始写代码。我把所有 Long Mode 相关的逻辑放在 `boot/common/long_mode.S`，这样 stage2 只需要调用两个函数：`setup_page_tables` 和 `enter_long_mode`。

### 完整代码：boot/common/long_mode.S

```asm
/**
 * @file boot/common/long_mode.S
 * @brief x86-64 Long Mode Initialization Functions
 *
 * This file contains functions to transition the CPU from 32-bit protected mode
 * to 64-bit long mode. Responsibilities include:
 *   - Setup temporary page tables at fixed address (0x1000-0x3FFF)
 *   - Enable PAE (Physical Address Extension)
 *   - Set LME (Long Mode Enable) bit in EFER
 *   - Extend GDT with 64-bit code segment descriptor
 *   - Perform far jump to 64-bit code segment
 *   - Verify long mode entry with debugcon output
 *
 * AT&T Syntax: source, destination order
 * Page Table Layout (Fixed at 0x1000):
 *   - PML4 at 0x1000 (4096 bytes)
 *   - PDPT at 0x2000 (4096 bytes)
 *   - PD at 0x3000 (4096 bytes)
 *   - Mapping: Identity only (PML4[0]), 2MB pages for first 8MB
 */

// ============================================================
// Constants
// ============================================================

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

// MSR addresses
.set MSR_EFER,            0xC0000080  // Extended Feature Enable Register

// EFER bits
.set EFER_LME,            0x100       // Long Mode Enable (bit 8)

// Control register bits
.set CR4_PAE,             0x20        // PAE enable (bit 5)
.set CR0_PG,              0x80000000  // Paging enable (bit 31)
.set CR0_PE,              0x01        // Protected mode enable (bit 0)

// Debugcon I/O port
.set DEBUGCON_PORT,       0xE9

// Character codes for debug output
.set CHAR_LONG_MODE,      0x4C        // 'L'

// ============================================================
// External Symbols
// ============================================================
.extern long_mode_entry            // 64-bit entry point (defined in stage2.S)
.extern gdt64_ptr                  // 64-bit GDT pointer (defined in stage2.S)

// ============================================================
// Section: .text
// ============================================================
.section .text
.code32

// ============================================================
// Function: setup_page_tables
// Responsibility: Initialize temporary page tables for long mode
// Input: None
// Output: None (modifies memory at 0x1000-0x3FFF)
// Clobbers: %eax, %ecx, %edi
// Description:
//   - Clear PML4, PDPT, and PD pages using rep stosl
//   - Setup PML4[0] -> PDPT with present+writable flags
//   - Setup PDPT[0] -> PD with present+writable flags
//   - Setup PD[0..3] for 2MB pages covering 0-8MB memory
// ============================================================
.global setup_page_tables
setup_page_tables:
    cld                             // 清零方向标志（确保 stosl 递增）

    // 清零 PML4 表（0x1000）
    movl $PML4_PHYS_ADDR, %edi       // [imm->edi] 目标地址 = PML4 地址
    xorl %eax, %eax                   // [0->eax] 要写入的值 = 0
    movl $1024, %ecx                  // [imm->ecx] 计数 = 4096/4 = 1024 个 dword
    rep stosl                         // [eax->edi] 清零 PML4，重复 ecx 次

    // 清零 PDPT 表（0x2000）
    movl $PDPT_PHYS_ADDR, %edi       // [imm->edi] 目标地址 = PDPT 地址
    xorl %eax, %eax                   // [0->eax] 要写入的值 = 0
    movl $1024, %ecx                  // [imm->ecx] 计数 = 1024 个 dword
    rep stosl                         // [eax->edi] 清零 PDPT，重复 ecx 次

    // 清零 PD 表（0x3000）
    movl $PD_PHYS_ADDR, %edi         // [imm->edi] 目标地址 = PD 地址
    xorl %eax, %eax                   // [0->eax] 要写入的值 = 0
    movl $1024, %ecx                  // [imm->ecx] 计数 = 1024 个 dword
    rep stosl                         // [eax->edi] 清零 PD，重复 ecx 次

    // 设置 PML4[0] -> PDPT
    movl $PDPT_PHYS_ADDR, %eax       // [imm->eax] 加载 PDPT 物理地址
    orl $0x03, %eax                   // [imm->eax] 添加 present+writable 标志
    movl %eax, PML4_PHYS_ADDR        // [eax->mem] PML4[0] = PDPT | flags

    // 设置 PDPT[0] -> PD
    movl $PD_PHYS_ADDR, %eax         // [imm->eax] 加载 PD 物理地址
    orl $0x03, %eax                   // [imm->eax] 添加 present+writable 标志
    movl %eax, PDPT_PHYS_ADDR        // [eax->mem] PDPT[0] = PD | flags

    // 设置 PD[0..3] 为 2MB 页（恒等映射 0-8MB）
    movl $PD_PHYS_ADDR, %edi         // [imm->edi] EDI 指向 PD 开始
    movl $4, %ecx                    // [imm->ecx] 循环 4 次
    xorl %eax, %eax                  // [0->eax] EAX = 0（循环变量）

1:
    movl %eax, %edx                  // [eax->edx] 复制循环变量到 EDX
    shll $21, %edx                   // [edx<<21] 左移 21 位（2MB 对齐）
    orl $PAGE_FLAGS, %edx            // [imm->edx] 添加标志位

    movl %edx, (%edi)                // [edx->edi] PD[i] = base_address | flags
    addl $8, %edi                    // [edi+8->edi] 移动到下一个 PD 条目

    incl %eax                        // [eax++] 循环变量递增
    loop 1b                          // [ecx--, loop] 循环直到 ECX = 0
    ret

// ============================================================
// Function: enter_long_mode
// Responsibility: Transition CPU from protected mode to long mode
// Input: None
// Output: Never returns (jumps to long_mode_entry in 64-bit mode)
// Description:
//   - Load page table base address into CR3
//   - Enable PAE by setting CR4 bit 5
//   - Enable Long Mode by setting EFER.LME
//   - Enable paging by setting CR0.PG + CR0.PE
//   - Load extended GDT (with 64-bit descriptor)
//   - Perform far jump to 64-bit code segment
// ============================================================
.global enter_long_mode
enter_long_mode:
    // 1. 加载页表基址到 CR3
    movl $PML4_PHYS_ADDR, %eax       // [imm->eax] PML4 表物理地址
    movl %eax, %cr3                  // [eax->cr3] 加载页表基址

    // 2. 启用 PAE（Physical Address Extension）
    movl %cr4, %eax                  // [cr4->eax] 读取 CR4
    orl $CR4_PAE, %eax                // [imm->eax] 设置 PAE 位（bit 5）
    movl %eax, %cr4                  // [eax->cr4] 写回 CR4

    // 3. 启用 Long Mode（EFER.LME）
    movl $MSR_EFER, %ecx             // [imm->ecx] MSR_EFER 地址 = 0xC0000080
    rdmsr                            // [msr->eax] 读取 EFER 到 edx:eax
    orl $EFER_LME, %eax               // [imm->eax] 设置 LME 位（bit 8）
    wrmsr                            // [eax->msr] 从 edx:eax 写回 EFER

    // 4. 加载扩展 GDT（包含 64 位描述符）
    lgdt gdt64_ptr                  // [mem->gdtr] 加载 GDT

    // 5. 启用分页（并确保保护模式已开启）
    movl %cr0, %eax                  // [cr0->eax] 读取 CR0
    orl $(CR0_PG | CR0_PE), %eax      // [imm->eax] 设置 PG + PE 位
    movl %eax, %cr0                  // [eax->cr0] 写回 CR0

    // 6. 远跳转到 64 位代码段
    ljmp $0x18, $long_mode_entry    // [imm->rip] 跳转到 64 位代码

    // 永远不会到这里
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

### 代码详解

`setup_page_tables` 函数负责初始化临时页表。这里使用了一个经典的汇编技巧：`rep stosl`。这个指令组合了重复前缀 `rep` 和字符串存储指令 `stosl`。`stosl` 把 `eax` 的值写入 `es:edi` 指向的内存，然后 `edi` 递增 4。`rep` 前缀让这个动作重复 `ecx` 次。4096 字节除以 4 字节等于 1024，正好清零一页内存。

清零完成后，我们建立页表层级关系：PML4[0] 指向 PDPT，PDPT[0] 指向 PD。然后我们用循环创建 4 个 2MB 页。

`shll $21, %edx` 这行是关键。左移 21 位相当于乘以 2MB（2^21 = 2097152）。循环变量 0、1、2、3 被转换为物理地址 0x000000、0x200000、0x400000、0x600000。`PAGE_FLAGS` 包含 PRESENT + WRITABLE + LARGE（0x83），这样 CPU 就知道这是 2MB 大页。最终效果是虚拟地址 0x00000000-0x007FFFFF 映射到物理地址 0x00000000-0x007FFFFF，这就是 identity mapping。

`enter_long_mode` 函数是整个切换过程的核心，每一步都必须严格按照顺序执行：

1. **加载 CR3**：告诉 CPU 页表在哪里。CR3 必须指向一个有效的 PML4 表，而且地址必须是 4KB 对齐的。我们的 PML4 位于 0x1000，正好满足这个条件。

2. **启用 PAE**：CR4 的 bit 5 控制 PAE，值为 0x20。这一步必须在启用分页之前完成，否则 CPU 会拒绝进入 Long Mode。

3. **设置 EFER.LME**：这是最关键的一步。EFER 是一个 MSR，地址为 0xC0000080。我们用 `rdmsr` 读取它的值到 edx:eax，然后用 `orl` 设置 bit 8，最后用 `wrmsr` 写回。**注意**：`EFER_LME = 0x100` 是 bit 8，不是 bit 12（0x1000 是 SVME 位），这一点必须确认正确。我之前就是因为这里写错，CPU 根本没进入 Long Mode，开启分页后直接 triple fault。

4. **加载 GDT**：Long Mode 需要新的 GDT 描述符格式，我们稍后会详细讨论。

5. **启用分页**：CR0 的 bit 31（0x80000000）控制分页，bit 0（0x1）控制保护模式。**注意**：启用分页后，CPU 会检查是否满足 Long Mode 条件（PAE=1、LME=1、CR3 有效），如果满足就自动进入 Long Mode。如果条件不满足，CPU 会触发 triple fault。

6. **远跳转**：`ljmp $0x18, $long_mode_entry`。`0x18` 是 64 位代码段选择子（index=3，TI=0，RPL=0），`long_mode_entry` 是跳转目标。这个远跳转会刷新 CS 寄存器和指令流水线，让 CPU 真正开始执行 64 位代码。

---

## 第四阶段 —— 64 位 GDT：Long Mode 的段描述符

stage2.S 中的 GDT 定义需要扩展，添加 64 位代码段和数据段描述符。

### stage2.S 中的相关代码

```asm
// ============================================================
// GDT (Global Descriptor Table) - in .gdt section
// ============================================================
.section .gdt,"a"
.align 8                          // Align to 8 bytes (2^3)

gdt:
gdt_null:
    .quad 0                        // Null descriptor (required)

gdt_code:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0 (= 0x8000)
    .byte 0x00                     // Base 23:16
    .byte 0x9A                     // Present, DPL=0, Code, Executable, Readable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_data:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0 (= 0x8000)
    .byte 0x00                     // Base 23:16
    .byte 0x92                     // Present, DPL=0, Data, Writable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

// 64-bit code descriptor (L=1, D=0)
// Value: 0x00AF9A000000FFFF
gdt_code64:
    .quad 0x00AF9A000000FFFF       // 64-bit code descriptor (L=1, D=0)

// 64-bit data descriptor
// Value: 0x008F92000000FFFF
gdt_data64:
    .quad 0x008F92000000FFFF       // 64-bit data descriptor

gdt_end:

// 32-bit GDT pointer for protected mode entry
gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // Linear address of GDT base (32-bit)

// 64-bit GDT pointer for long mode reload
// Note: Use .long + .long instead of .quad to avoid 64-bit relocation
// in 32-bit ELF. GDT is at low address so upper 32 bits are zero.
.global gdt64_ptr
gdt64_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // Lower 32 bits of GDT address
    .long 0                        // Upper 32 bits (zero, GDT is in low memory)
```

### GDT 选择子计算

gdt_null 是 0x00（第 0 个，偏移 0），gdt_code 是 0x08（第 1 个，偏移 8），gdt_data 是 0x10（第 2 个，偏移 16），gdt_code64 是 0x18（第 3 个，偏移 24），gdt_data64 是 0x20（第 4 个，偏移 32）。`ljmp $0x18, $long_mode_entry` 中的 0x18 就是指向 gdt_code64。

### 64 位代码段描述符详解

`gdt_code64` 的魔法数字 `0x00AF9A000000FFFF` 需要仔细拆解。低 32 位是 `0x0000FFFF`，其中 Limit[15:0] = 0xFFFF，Base[15:0] = 0x0000。高 32 位是 `0x00AF9A00`，其中 Base[31:24] = 0x00，Access byte = 0x9A，Flags = 0xAF。

Access byte 0x9A 的含义：P=1（段在内存中），DPL=00（内核级），S=1（普通代码/数据段），Type=1010（代码段，可执行，可读）。Flags 0xAF 的含义：G=1（4KB 粒度），L=1（Long Mode，这是关键），D=0（当 L=1 时 D 必须为 0），Limit[19:16] = 0xF。

L 位（Long bit）是 64 位代码段的标志。当 L=1 时，CPU 会把这个段当作 64 位代码段处理，默认操作数大小为 64 位，地址大小也是 64 位。D 位（Default bit）必须为 0，这是 Intel 的硬性规定。

---

## 第五阶段 —— 64 位入口点：Long Mode 的第一条指令

`long_mode_entry` 是 CPU 进入 Long Mode 后执行的第一段代码。

### stage2.S 中的 64 位入口点

```asm
// ============================================================
// Long Mode Entry Point
// ============================================================
.code64                          // Now in 64-bit long mode
.global long_mode_entry
long_mode_entry:
    // GDT 已经在 enter_long_mode 中加载

    // 设置数据段寄存器
    movw $GDT_DATA64, %ax         // [imm->ax] 加载 64 位数据段选择子
    movw %ax, %ds                 // [ax->ds] 设置 DS
    movw %ax, %es                 // [ax->es] 设置 ES
    movw %ax, %fs                 // [ax->fs] 设置 FS
    movw %ax, %gs                 // [ax->gs] 设置 GS
    movw %ax, %ss                 // [ax->ss] 设置 SS

    // 设置 64 位栈
    movabsq $0x90000, %rsp        // [imm->rsp] 设置 64 位栈指针

    // 验证进入 Long Mode（输出 'L'）
    movb $CHAR_LONG_MODE, %al     // [imm->al] 'L' (0x4C)
    outb %al, $DEBUGCON_PORT      // [al->port] 输出到 debugcon

    // 暂停
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

首先设置所有数据段寄存器指向 64 位数据段（选择子 0x20）。注意在 64 位模式下，段基址被忽略（除了 FS 和 GS，它们可以用于线程局部存储），但段选择子还是要正确设置。SS 也必须设置，否则栈操作会触发异常。

然后设置 64 位栈指针。注意用的是 `movabsq`，它可以加载 64 位立即数。之前的保护模式用的是 `movl $0x90000, %esp`，现在换成了 `movabsq $0x90000, %rsp`。`rsp` 是 64 位的栈指针寄存器。

最后输出 'L' 到 debugcon 端口（0xE9）。这是确认成功进入 Long Mode 的唯一信号 —— 如果你看到 `PL`，说明保护模式和 Long Mode 都成功了。如果只看到 `P` 然后 QEMU 崩溃，说明 Long Mode 切换失败。

---

## 第六阶段 —— 最惨痛的踩坑：EFER.LME 位写错

现在我们来讲整个项目中最惨痛的一次调试经历。

### 问题现象

启动流程执行到开启分页（CR0.PG）时，CPU 直接 triple fault，QEMU 窗口瞬间消失。GDB 显示的寄存器状态：

```
cr0  = 0x11          # 未开启分页
cr4  = 0x20          # PAE 已开启
efer = 0x1000        # 仅 SVME，没有 LME！
```

一旦写入 `mov %eax, %cr0` 启用分页，立即崩溃。

### 排查过程

1. **确认页表结构**：用 `x/8gx 0x1000` 查看 PML4，`x/8gx 0x2000` 查看 PDPT，`x/8gx 0x3000` 查看 PD。页表结构看起来是正确的 —— 2MB 大页，identity mapping，连续映射。

2. **CR4 检查**：`CR4 = 0x20`，PAE = 1，正确。

3. **致命线索**：`efer = 0x1000`，这意味着只有 SVME 位，**没有 LME 位**。CPU 从未进入 Long Mode，却尝试开启分页，直接 triple fault。

### 核心错误

我之前是这样定义的：

```asm
.set EFER_LME, 0x1000   // 错误！这是 SVME 位
```

正确的应该是：

```asm
.set EFER_LME, 0x100    // 正确！这是 LME 位（bit 8）
```

影响对比：

| 写法   | 实际效果 |
| ------ | -------- |
| 0x100  | 开启 LME |
| 0x1000 | 设置 SVME |

这个 bug 导致 CPU 从未进入 Long Mode，开启分页后直接 triple fault。

### 正确的 Long Mode 启动顺序

这次踩坑最重要的一条经验：**严格按照以下顺序初始化**：

```
1. 开启 PAE：CR4.PAE = 1
2. 开启 Long Mode Enable：EFER.LME = 1（最关键！）
3. 设置页表：CR3 = PML4
4. 开启分页：CR0.PG = 1
5. 远跳进入 64-bit：ljmp selector, offset
```

任何顺序错误都会导致 triple fault。

### 如何快速验证

用 GDB 检查关键寄存器：

```gdb
info registers cr0
info registers cr4
info registers efer
```

正确状态应该是：

```
CR0  = 0x80000011   (PE + PG)
CR4  = 0x20         (PAE)
EFER = 0x1100       (LME + SVME)
```

一旦不满足，直接 triple fault。

---

## 第七阶段 —— 调试验证：那个 'L' 终于出现了

代码写完了，bug 修复了，现在来验证一下。

### 编译和运行

```bash
# 检出本章对应的代码版本
git checkout 003_boot_long_mode

# 创建构建目录
mkdir -p build
cd build

# 配置 CMake（Debug 模式，带符号信息）
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..

# 编译
make

# 运行 QEMU（正常模式）
make run

# 或者运行 QEMU（调试模式，等待 GDB 连接）
make run-debug
```

### 验证输出

如果一切正常，`debug.log` 文件应该包含：

```
PL
```

串口输出（stdio）应该包含：

```
Stage2 OK
Mode info OK, switching...
```

这个 `L` 就是我们的胜利标志。它意味着：

1. 页表被正确初始化
2. CR4.PAE 位被设置
3. EFER.LME 位被正确设置（不是 SVME！）
4. CR3 指向有效的 PML4 表
5. CR0.PG 位被设置
6. CPU 进入 Long Mode
7. 远跳转成功刷新 CS
8. CPU 能执行 64 位代码并访问 I/O 端口

### 用 GDB 验证页表

如果看不到 `L`，或者想进一步验证，可以用 GDB：

```bash
# 终端 1：启动 QEMU 等待 GDB 连接
make run-debug

# 终端 2：启动 GDB
make run-gdb
```

或者手动连接：

```gdb
gdb build/boot/stage2
(gdb) target remote :1234
(gdb) break *enter_long_mode
(gdb) continue
(gdb) x/8gx 0x1000
(gdb) x/8gx 0x2000
(gdb) x/8gx 0x3000
```

你应该能看到页表结构：

```
0x1000: 0x0000000000002000 0x0000000000000000 ...
0x2000: 0x0000000000003000 0x0000000000000000 ...
0x3000: 0x0000000000000083 0x0000000000200083 0x0000000000400083 0x0000000000600083 ...
```

---

## 常见问题排查

### 问题 1：看不到 `L` 输出

可能原因：
1. EFER.LME 位设置错误：检查是否是 `0x100` 而不是 `0x1000`
2. 页表结构错误：用 GDB 检查 0x1000、0x2000、0x3000 的内容
3. 初始化顺序错误：严格按照 PAE -> LME -> CR3 -> PG 的顺序
4. GDT 64 位描述符错误：检查 L=1、D=0

排查方法：用 GDB 检查 `%efer` 寄存器，确认 LME 位被设置。在 `enter_long_mode` 函数中设置断点，单步执行。

### 问题 2：Triple Fault（QEMU 窗口直接关闭）

这通常是因为：
1. EFER.LME 未设置，CPU 未进入 Long Mode，却开启分页
2. 页表条目错误，CR3 指向无效地址
3. PAE 未启用，Long Mode 前置条件不满足
4. 64 位 GDT 描述符格式错误

排查方法：在 `enter_long_mode` 函数的每一步后检查寄存器状态。确认 `efer` 的 bit 8 被设置。

### 问题 3：GDB 反汇编显示错误的指令

这说明指令编码与 CPU 状态不匹配。通常是 `.code32` 和 `.code64` 混用导致的。

排查方法：检查代码中是否在正确的位置切换了 `.code32` 和 `.code64`。`enter_long_mode` 必须在 `.code32` 段，`long_mode_entry` 必须在 `.code64` 段。

---

## 本章踩坑总结

写这一章的时候，我踩过的坑总结如下：

1. **EFER.LME 位写错**：`0x100` 是 bit 8（LME），`0x1000` 是 bit 12（SVME），写错会导致 CPU 未进入 Long Mode
2. **初始化顺序错误**：必须严格按照 PAE -> LME -> CR3 -> PG 的顺序，任何顺序错误都会导致 triple fault
3. **页表未清零**：页表必须完全清零，否则可能残留垃圾数据导致映射错误
4. **忘记 Identity Mapping**：模式切换时代码必须继续执行，没有 identity mapping 会立即崩溃
5. **64 位 GDT 描述符错误**：L 位必须为 1，D 位必须为 0，这是 Intel 硬件规定
6. **`.code32` 和 `.code64` 混乱**：汇编器指令只影响生成的机器码，不改变 CPU 状态
7. **远跳转选择子错误**：`0x18` 是 64 位代码段选择子，指向第 3 个描述符
8. **忘记关中断**：开启分页前必须 `cli`，否则中断可能导致 triple fault
9. **栈指针设置错误**：64 位模式下用 `rsp`，必须设置 `ss` 为 64 位数据段
10. **CR3 未对齐**：PML4 地址必须 4KB 对齐，我们用 0x1000 正好满足

这些坑每一个都足以让 CPU 直接跑飞，QEMU 窗口瞬间消失。写这一章的时候我真的是一边写一边调试，每一步都小心翼翼，生怕漏掉什么细节。

---

## 收尾

到这里，保护模式到 Long Mode 的切换就完成了。我们从一个只有 32 位寄存器、4GB 地址空间的保护模式，跳到了有 64 位寄存器、256TB 地址空间的广阔天地。虽然只输出了一个 `L`，但这个简单的字符背后，CPU 完成了从保护模式到 Long Mode 的切换，设置了页表，启用了分页，每一步都不能出错。

说实话，写这一章的时候我踩了不少坑。最离谱的是 EFER.LME 位写错，我把 `0x100` 写成了 `0x1000`，结果 CPU 根本没进入 Long Mode，开启分页后直接 triple fault。QEMU 窗口瞬间消失，连个错误信息都不给我留。后来用 GDB 单步跟了半天，才发现 `efer` 寄存器的值是 `0x1000`（SVME）而不是 `0x100`（LME）。这一个位的差别，让我调试了好几个晚上。

现在启动 QEMU，你应该能在串口看到：

```
Stage2 OK
Mode info OK, switching...
```

在 `debug.log` 文件看到：

```
PL
```

这个 `L` 标志着 Long Mode 切换成功。到这里就大功告成了，你可以用 GDB 验证一下页表结构和寄存器状态，确认一切正确。下一步方向：加载 ELF64 内核镜像，实现高半内核映射，设置 BootInfo 结构体，然后跳转到内核入口点。到那个时候，我们就能开始写 C++ 内核代码了，不用再手搓汇编。

但至少现在，我们已经有了稳定的 Long Mode 基础，可以开始写 64 位内核代码了。不用再担心 4GB 内存限制，不用再搞段选择子这种别扭的机制。Long Mode，真香。

---

## 最重要三条认知（必须记住）

写从保护模式到 Long Mode 切换这件事，最核心的认知有三条：

**EFER.LME 是 bit 8，值是 0x100，不是 0x1000**：0x1000 是 SVME 位（bit 12），AMD 虚拟化相关。我之前就是因为这里写错，CPU 根本没进入 Long Mode，开启分页后直接 triple fault。这一个位的差别，让我调试了好几个晚上。

**严格按照 PAE -> LME -> CR3 -> PG 的顺序初始化**：任何顺序错误都会导致 triple fault。CPU 进入 Long Mode 的条件是 PAE=1、LME=1、CR3 有效、PG=1，这四个条件缺一不可，而且必须按这个顺序设置。

**Identity Mapping 是模式切换的前提**：切换模式时，CPU 会继续执行下一条指令。如果虚拟地址和物理地址不一致，CPU 会立即访问错误的地址，导致 triple fault。我们必须确保页表建立了 identity mapping，让代码能继续执行。

记住这三条，能帮你省掉很多调试时间。剩下的就是耐心，一遍遍调试，终会成功的。

---

## 参考资料

### Intel/AMD 手册
- Intel SDM Vol. 3A, Chapter 9: Processor Management and Initialization
- Intel SDM Vol. 3A, Section 9.8: Entering Long Mode
- Intel SDM Vol. 3A, Section 4.5: Page Tables
- AMD APM Vol. 2, Chapter 5: Long Mode

### OSDev Wiki
- https://wiki.osdev.org/Long_Mode
- https://wiki.osdev.org/Paging
- https://wiki.osdev.org/Setting_Up_Long_Mode
- https://wiki.osdev.org/Identity_Paging

### 其他资源
- x86-64: From 32-bit to 64-bit: https://www.codeproject.com/Articles/45788/The-x-64-is-an-advanced-bit-computing-architectur
- Why PAE is required for Long Mode: https://stackoverflow.com/questions/15016735/why-is-pae-needed-to-enter-long-mode

---

**文件路径汇总**：

- [boot/common/long_mode.S](../../boot/common/long_mode.S) - Long Mode 切换的核心实现，包含页表初始化和模式切换代码
- [boot/stage2.S](../../boot/stage2.S) - Stage2 主逻辑，包含 GDT 定义和 64 位入口点
- [cmake/qemu.cmake](../../cmake/qemu.cmake) - QEMU 启动参数配置，包含 debugcon 设置

下一章预告：`004_boot_elf_kernel` —— 我们将加载 ELF64 内核镜像，实现高半内核映射，设置 BootInfo 结构体，然后跳转到内核入口点。到那个时候，我们就能开始写 C++ 内核代码了，不用再手搓汇编。
