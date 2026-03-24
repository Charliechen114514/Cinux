# 003 通读版 · 进入 Long Mode

## 章节概览

如果你在 2026 年还在用 32 位保护模式写内核，多少有点"考古"的感觉。但 x86 架构的历史包袱实在太重，启动链条必须按部就班地走：Real Mode -> Protected Mode -> Long Mode。本章的目标非常明确：让 CPU 从 32 位保护模式切换到 64 位 Long Mode，并通过 debugcon 输出一个 `L` 字符确认成功。听上去简单？这一步实际上踩过非常多坑，我自己在调试时遇到的 EFER.LME 位写错导致的 triple fault 就是个典型例子。

在整个 Cinux OS 的架构中，本章扮演着进入现代 x86-64 时代的钥匙角色。上一章我们进入了保护模式，输出一个 `P` 字母就算成功。但保护模式本质上还是 32 位时代的东西，寄存器只有 32 位、地址空间只有 4GB、各种特权级机制还停留在 80386 时代。要写一个"现代"的操作系统，我们必须进入 Long Mode。完成这一步后，64 位寄存器（RAX、RBX、RSP...）、64 位地址空间（理论上限 256TB）、现代 C++ 编译器默认输出——这一切都向我们敞开了大门。

本章的核心设计决策包括：使用固定地址的临时页表（0x1000-0x3FFF）简化早期内存管理；采用 2MB 大页减少页表层级；严格遵循 PAE -> LME -> CR3 -> PG 的初始化顺序避免 triple fault；将 Long Mode 逻辑独立到 `boot/common/long_mode.S` 提高代码可维护性。与 Linux 0.11 的启动代码相比，我们的实现更加直接 —— Linux 需要处理多种启动场景和硬件配置，而我们专注于 QEMU 环境的最小可用配置。

### 关键设计决策一览

* **固定地址页表**：PML4/PDPT/PD 分别位于 0x1000/0x2000/0x3000，避免动态内存分配的复杂性
* **2MB 大页映射**：跳过 PT 级别，用 3 级页表映射前 8MB 内存，减少页表占用空间
* **Identity Mapping**：虚拟地址等于物理地址，确保模式切换时代码能继续执行
* **严格初始化顺序**：PAE -> LME -> CR3 -> PG，任何顺序错误都会导致 triple fault
* **模块化设计**：Long Mode 逻辑独立到 `common/long_mode.S`，stage2 只负责调用

---

## 架构图

下面是本章涉及的内存布局、页表结构和控制流关系图，帮助你理解各个组件是如何协作的：

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
|              |  .text: 16-bit real mode code                 |       |
|              |  .text: 32-bit protected mode code            |       |
|              |  .text: 64-bit long mode code                 |       |
|              |  .gdt:  GDT (40B with 64-bit descriptors)     |       |
|  0x00009000  +----------------------------------------------+       |
|              |  栈 (向下增长)                                  |       |
+---------------------------------------------------------------------+
|                        页表翻译流程（Identity Mapping）             |
+---------------------------------------------------------------------+
|                                                                     |
|  Virtual Address 0x00000000 (64-bit)                                 |
|       |                                                              |
|       |  Split into 9-bit indices                                   |
|       v                                                              |
|  +----------------+                                                 |
|  | PML4[0] @ 0x1000   | -> 0x2003                                    |
|  +----------------+         |                                        |
|                             v                                        |
|  +----------------+                                                 |
|  | PDPT[0] @ 0x2000   | -> 0x3003                                    |
|  +----------------+         |                                        |
|                             v                                        |
|  +----------------+                                                 |
|  | PD[0] @ 0x3000     | -> 0x83 (flags: P=1, RW=1, PS=1)           |
|  +----------------+         |                                        |
|                             v                                        |
|                    Physical Address 0x00000000                       |
|                                                                     |
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
|  |  3. rdmsr / wrmsr        |  设置 EFER.LME=1                       |
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
|  解码:                                                               |
|    [15:0]  Limit    = 0xFFFF                                        |
|    [39:16] Base     = 0x00000000                                    |
|    [47:40] Access   = 0x9A (P=1, DPL=0, L=1, Type=0xA)              |
|    [51:48] Flags    = 0xA (G=1, L=1, D=0)                           |
|    [63:56] Base[31:24] = 0x00                                       |
|                                                                     |
|  关键位:                                                             |
|    L=1 (Long bit) 表示这是一个 64-bit 代码段                          |
|    D=0 (Default bit) 必须为 0（L=1 时 D 必须为 0）                   |
|                                                                     |
+---------------------------------------------------------------------+
```

---

## 关键代码精讲

接下来我们要逐段拆解本章的核心代码。本章的主要逻辑位于 `boot/common/long_mode.S`，这个文件封装了所有与 Long Mode 切换相关的功能，包括页表初始化和 CPU 状态切换。stage2.S 只需要在适当的位置调用这些函数即可。

### 常量定义：Long Mode 的魔法数字

代码的开头定义了一系列常量，这些数字背后都有其硬件层面的含义：

```asm
// Page table physical addresses (fixed layout)
.set PML4_PHYS_ADDR,      0x1000      // PML4 table location
.set PDPT_PHYS_ADDR,      0x2000      // PDPT table location
.set PD_PHYS_ADDR,        0x3000      // Page Directory location

// Page table entry flags
.set PAGE_PRESENT,        0x01        // Present bit
.set PAGE_WRITABLE,       0x02        // Read/Write bit
.set PAGE_LARGE,          0x80        // Page Size bit (2MB pages)

// MSR addresses
.set MSR_EFER,            0xC0000080  // Extended Feature Enable Register

// EFER bits
.set EFER_LME,            0x100       // Long Mode Enable (bit 8)
```

页表地址选择在 0x1000-0x3FFF 不是随意的。这个区域在传统上是被保留给 BIOS 数据区的，但在我们控制的环境中可以安全使用。更重要的是，这些地址都是 4KB 对齐的，满足 CPU 对页表基址的对齐要求。每一级页表占 4KB，包含 512 个 8 字节的条目。

页表条目标志位中的 `PAGE_LARGE = 0x80` 尤其重要。这个位（bit 7）被称为 PS（Page Size）位，当它为 1 时，PD 条目直接指向一个 2MB 的物理页，跳过 PT 级别。这大大简化了我们的页表结构——只需要 3 级而不是 4 级就能映射前 8MB 内存。

EFER_LME 的值是 0x100 而不是 0x1000，这一点我曾经踩过坑。0x100 是 bit 8，代表 LME（Long Mode Enable）；而 0x1000 是 bit 12，代表 SVME（SVM Enable），是 AMD 虚拟化相关的位。如果写错，CPU 会无法进入 Long Mode，开启分页时直接 triple fault。

### 页表初始化：构建内存映射的骨架

`setup_page_tables` 函数负责初始化临时页表，这是进入 Long Mode 的前置条件：

```asm
.global setup_page_tables
setup_page_tables:
    cld                             // 清零方向标志（确保 stosl 递增）

    // 清零 PML4 表（0x1000）
    movl $PML4_PHYS_ADDR, %edi       // 目标地址 = PML4 地址
    xorl %eax, %eax                   // 要写入的值 = 0
    movl $1024, %ecx                  // 计数 = 4096/4 = 1024 个 dword
    rep stosl                         // 清零 PML4，重复 ecx 次

    // 清零 PDPT 表（0x2000）
    movl $PDPT_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl

    // 清零 PD 表（0x3000）
    movl $PD_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl
```

这里使用了一个经典的汇编技巧：`rep stosl`。这个指令组合了重复前缀 `rep` 和字符串存储指令 `stosl`。`stosl` 把 `eax` 的值写入 `es:edi` 指向的内存，然后 `edi` 递增 4（因为是 `l` 后缀，long 操作）。`rep` 前缀让这个动作重复 `ecx` 次。4096 字节除以 4 字节等于 1024，正好清零一页内存。

清零完成后，我们需要建立页表层级关系：

```asm
    // 设置 PML4[0] -> PDPT
    movl $PDPT_PHYS_ADDR, %eax       // 加载 PDPT 物理地址
    orl $0x03, %eax                   // 添加 present+writable 标志
    movl %eax, PML4_PHYS_ADDR        // PML4[0] = PDPT | flags

    // 设置 PDPT[0] -> PD
    movl $PD_PHYS_ADDR, %eax         // 加载 PD 物理地址
    orl $0x03, %eax                   // 添加 present+writable 标志
    movl %eax, PDPT_PHYS_ADDR        // PDPT[0] = PD | flags
```

这里的 `orl $0x03, %eax` 设置了两个标志位：bit 0 (PRESENT) 表示页在内存中，bit 1 (WRITABLE) 表示页可写。值为 0x03 表示两个位都置 1。我们只设置 PML4[0] 和 PDPT[0]，这意味着页表只映射了第一个 512GB 的地址空间（更准确地说是第一个 256TB，取决于具体的实现），对于我们的启动阶段已经足够。

接下来是设置 PD 条目，我们用循环创建 4 个 2MB 页：

```asm
    // 设置 PD[0..3] 为 2MB 页（恒等映射 0-8MB）
    movl $PD_PHYS_ADDR, %edi         // EDI 指向 PD 开始
    movl $4, %ecx                    // 循环 4 次
    xorl %eax, %eax                  // EAX = 0（循环变量）

1:
    movl %eax, %edx                  // 复制循环变量到 EDX
    shll $21, %edx                   // 左移 21 位（2MB 对齐）
    orl $PAGE_FLAGS, %edx            // 添加标志位

    movl %edx, (%edi)                // PD[i] = base_address | flags
    addl $8, %edi                    // 移动到下一个 PD 条目

    incl %eax                        // 循环变量递增
    loop 1b                          // 循环直到 ECX = 0
    ret
```

`shll $21, %edx` 这行是关键。左移 21 位相当于乘以 2MB（2^21 = 2097152）。循环变量 0、1、2、3 被转换为物理地址 0x000000、0x200000、0x400000、0x600000。`PAGE_FLAGS` 包含 PRESENT + WRITABLE + LARGE（0x83），这样 CPU 就知道这是 2MB 大页。最终效果是虚拟地址 0x00000000-0x007FFFFF 映射到物理地址 0x00000000-0x007FFFFF，这就是 identity mapping。

### Long Mode 切换：CPU 状态的精密舞蹈

`enter_long_mode` 函数是整个切换过程的核心，每一步都必须严格按照顺序执行：

```asm
.global enter_long_mode
enter_long_mode:
    // 1. 加载页表基址到 CR3
    movl $PML4_PHYS_ADDR, %eax       // PML4 表物理地址
    movl %eax, %cr3                  // 加载页表基址

    // 2. 启用 PAE（Physical Address Extension）
    movl %cr4, %eax                  // 读取 CR4
    orl $CR4_PAE, %eax               // 设置 PAE 位（bit 5）
    movl %eax, %cr4                  // 写回 CR4

    // 3. 启用 Long Mode（EFER.LME）
    movl $MSR_EFER, %ecx             // MSR_EFER 地址 = 0xC0000080
    rdmsr                            // 读取 EFER 到 edx:eax
    orl $EFER_LME, %eax               // 设置 LME 位（bit 8）
    wrmsr                            // 写回 EFER

    // 4. 加载扩展 GDT（包含 64 位描述符）
    lgdt gdt64_ptr                   // 加载 GDT

    // 5. 启用分页（并确保保护模式已开启）
    movl %cr0, %eax                  // 读取 CR0
    orl $(CR0_PG | CR0_PE), %eax     // 设置 PG + PE 位
    movl %eax, %cr0                  // 写回 CR0

    // 6. 远跳转到 64 位代码段
    ljmp $0x18, $long_mode_entry     // 跳转到 64 位代码

    // 永远不会到这里
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

第一步加载 CR3 寄存器，告诉 CPU 页表在哪里。CR3 必须指向一个有效的 PML4 表，而且地址必须是 4KB 对齐的。我们的 PML4 位于 0x1000，正好满足这个条件。

第二步启用 PAE（Physical Address Extension）。PAE 是 Long Mode 的前置条件，它将物理地址从 32 位扩展到 36 位（支持最多 64GB 内存）。CR4 的 bit 5 控制 PAE，值为 0x20。这一步必须在启用分页之前完成，否则 CPU 会拒绝进入 Long Mode。

第三步是最关键的——设置 EFER.LME 位。EFER（Extended Feature Enable Register）是一个 MSR（Model Specific Register），地址为 0xC0000080。我们用 `rdmsr` 读取它的值到 edx:eax（EDX 存放高 32 位，EAX 存放低 32 位），然后用 `orl` 设置 bit 8，最后用 `wrmsr` 写回。`EFER_LME = 0x100` 是 bit 8，不是 bit 12（0x1000 是 SVME 位），这一点必须确认正确。

第四步加载 GDT。Long Mode 需要新的 GDT 描述符格式，我们稍后会详细讨论。`gdt64_ptr` 是一个 10 字节的结构：2 字节 limit + 4 字节 base_low + 4 字节 base_high。因为我们的 GDT 在低 4GB 地址空间，高 32 位为 0。

第五步启用分页。CR0 的 bit 31（0x80000000）控制分页，bit 0（0x1）控制保护模式。**注意**：启用分页后，CPU 会检查是否满足 Long Mode 条件（PAE=1、LME=1、CR3 有效），如果满足就自动进入 Long Mode。如果条件不满足，CPU 会触发 triple fault。

第六步远跳转 `ljmp $0x18, $long_mode_entry`。`0x18` 是 64 位代码段选择子（index=3，TI=0，RPL=0），`long_mode_entry` 是跳转目标。这个远跳转会刷新 CS 寄存器和指令流水线，让 CPU 真正开始执行 64 位代码。跳转后，CPU 就完全处于 Long Mode 了。

### 64 位 GDT：Long Mode 的段描述符

stage2.S 中的 GDT 定义需要扩展，添加 64 位代码段和数据段描述符：

```asm
// 64-bit code descriptor (L=1, D=0)
// Value: 0x00AF9A000000FFFF
gdt_code64:
    .quad 0x00AF9A000000FFFF       // 64-bit code descriptor (L=1, D=0)

// 64-bit data descriptor
// Value: 0x008F92000000FFFF
gdt_data64:
    .quad 0x008F92000000FFFF       // 64-bit data descriptor
```

`gdt_code64` 的魔法数字 `0x00AF9A000000FFFF` 需要仔细拆解。低 32 位是 `0x0000FFFF`，其中 Limit[15:0] = 0xFFFF，Base[15:0] = 0x0000。高 32 位是 `0x00AF9A00`，其中 Base[31:24] = 0x00，Access byte = 0x9A，Flags = 0xAF。

Access byte 0x9A 的含义：P=1（段在内存中），DPL=00（内核级），S=1（普通代码/数据段），Type=1010（代码段，可执行，可读）。Flags 0xAF 的含义：G=1（4KB 粒度），L=1（Long Mode，这是关键），D=0（当 L=1 时 D 必须为 0），Limit[19:16] = 0xF。

L 位（Long bit）是 64 位代码段的标志。当 L=1 时，CPU 会把这个段当作 64 位代码段处理，默认操作数大小为 64 位，地址大小也是 64 位。D 位（Default bit）必须为 0，这是 Intel 的硬性规定。

选择子计算：gdt_null 是 0x00（第 0 个，偏移 0），gdt_code 是 0x08（第 1 个，偏移 8），gdt_data 是 0x10（第 2 个，偏移 16），gdt_code64 是 0x18（第 3 个，偏移 24），gdt_data64 是 0x20（第 4 个，偏移 32）。`ljmp $0x18, $long_mode_entry` 中的 0x18 就是指向 gdt_code64。

### 64 位入口点：Long Mode 的第一条指令

`long_mode_entry` 是 CPU 进入 Long Mode 后执行的第一段代码：

```asm
.code64                          // 现在在 64 位 Long Mode
.global long_mode_entry
long_mode_entry:
    // GDT 已经在 enter_long_mode 中加载

    // 设置数据段寄存器
    movw $GDT_DATA64, %ax         // 加载 64 位数据段选择子
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // 设置 64 位栈
    movabsq $0x90000, %rsp        // 设置 64 位栈指针

    // 验证进入 Long Mode（输出 'L'）
    movb $CHAR_LONG_MODE, %al     // 'L' (0x4C)
    outb %al, $DEBUGCON_PORT      // 输出到 debugcon

    // 暂停
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

首先设置所有数据段寄存器指向 64 位数据段（选择子 0x20）。注意在 64 位模式下，段基址被忽略（除了 FS 和 GS，它们可以用于线程局部存储），但段选择子还是要正确设置。SS 也必须设置，否则栈操作会触发异常。

然后设置 64 位栈指针。注意用的是 `movabsq`，它可以加载 64 位立即数。之前的保护模式用的是 `movl $0x90000, %esp`，现在换成了 `movabsq $0x90000, %rsp`。`rsp` 是 64 位的栈指针寄存器。

最后输出 'L' 到 debugcon 端口（0xE9）。这是确认成功进入 Long Mode 的唯一信号——如果你在终端看到 `PL`，说明保护模式和 Long Mode 都成功了。如果只看到 `P` 然后 QEMU 崩溃，说明 Long Mode 切换失败。

---

## 设计决策深度分析

### 决策 1：固定地址页表 vs 动态分配

**问题**：在实现 Long Mode 的页表时，我们有两种选择：使用固定地址的页表（如 0x1000-0x3FFF）或在内存中动态分配页表空间。

**本项目的做法**：我们选择了固定地址方案，PML4、PDPT、PD 分别位于 0x1000、0x2000、0x3000。这样做的好处是在启动早期阶段，动态内存分配还不存在，只能用固定地址。而且这些地址都是 4KB 对齐的，满足 CPU 对页表基址的对齐要求。

**备选方案**：另一种设计是让 bootloader 扫描 BIOS 内存地图，找到可用的内存区域，然后动态分配页表空间。GRUB 和其他高级 bootloader 采用了这种策略，以适应不同的硬件配置。

**为什么不选备选方案**：我们的目标是 QEMU 环境，内存布局是可控的。固定地址方案大大简化了代码，不需要实现复杂的内存分配逻辑。而且这些地址（0x1000-0x3FFF）在传统上是被保留给 BIOS 数据区的，但在 QEMU 中可以安全使用。

**如果要扩展/改进**：如果你想支持真实硬件，需要实现 BIOS 内存地图扫描（INT 15h, E820h），找到可用的内存区域。然后在可用内存中分配页表空间，更新 CR3 指向新的地址。这会增加代码复杂度，但能提高兼容性。

### 决策 2：2MB 大页 vs 4KB 分页

**问题**：在映射物理内存时，可以选择使用 2MB 大页（跳过 PT 级别）或标准的 4KB 分页（完整的 4 级页表）。

**本项目的做法**：我们选择了 2MB 大页，用 3 级页表（PML4 -> PDPT -> PD）映射前 8MB 内存。每个 PD 条目直接指向一个 2MB 的物理页，PS 位（Page Size）设为 1。这样只需要初始化 3 张页表，占用 12KB 内存。

**备选方案**：标准的 4KB 分页需要 4 级页表（PML4 -> PDPT -> PD -> PT），每张表占 4KB。要映射 8MB 内存需要 2048 个 4KB 页，这意味着需要额外的 4 级页表和 PT 表，总共占用更多内存。

**为什么不选备选方案**：在启动早期阶段，内存资源宝贵，2MB 大页大大减少了页表占用。而且对于 identity mapping 这种简单的场景，2MB 大页完全够用。Linux 内核在启动阶段也使用了大页来映射内核代码段。

**如果要扩展/改进**：后续可以动态切换到 4KB 分页，以支持更细粒度的内存管理。比如在实现虚拟内存时，可以按需分配 4KB 页，支持页面换出和 copy-on-write 机制。这需要在运行时重新设置页表并刷新 TLB。

### 决策 3：在 stage2 中切换 vs 在独立内核中切换

**问题**：Long Mode 的切换可以在 stage2 bootloader 中完成，也可以延迟到真正的内核代码中。

**本项目的做法**：我们在 stage2 中就完成了到 Long Mode 的切换，stage2 的 `long_mode_entry` 是第一段 64 位代码。这样做的好处是 stage2 后续可以直接调用 64 位函数，为加载 ELF64 内核做准备。

**备选方案**：另一种设计是让 stage2 完全运行在保护模式，只在把控制权交给内核前的最后一刻才切换到 Long Mode。这需要 stage2 用 32 位代码解析 ELF64 格式，然后设置页表并切换。

**为什么不选备选方案**：64 位模式下解析 ELF 更加自然，因为 ELF64 的文件头和段表都是 64 位格式。而且 64 位代码可以访问更大的地址空间，不需要担心分段问题。在 stage2 中尽早切换到 Long Mode，我们可以用更现代的汇编和 C 代码完成复杂的初始化工作。

**如果要扩展/改进**：当前的实现假设 stage2 被加载到固定的 0x8000 地址。如果你希望支持动态加载地址，需要实现位置无关代码（PIC）或在切换前重定位代码。这可以通过自解压 bootloader 或链接时优化（LTO）来实现。

---

## 常见变体与扩展方向

下面列出几个你可以尝试的扩展实验，按照难度排序：

1. **⭐ 扩展 identity mapping 范围**：当前实现只映射了前 8MB 内存。你可以增加 PD 条目数量，映射更多内存（比如前 128MB）。需要注意页表的条目数量和内存占用的平衡。

2. **⭐⭐ 添加高半内核映射**：实现高半内核映射（0xFFFFFFFF80000000 -> 0x00000000），这是现代内核的标准做法。需要设置更多的页表条目，并在切换后更新指针。

3. **⭐⭐ 实现 4 级页表结构**：放弃 2MB 大页，使用完整的 4 级页表（PML4 -> PDPT -> PD -> PT）。这需要初始化更多的页表，但能支持 4KB 粒度的内存管理。

4. **⭐⭐⭐ 动态页表分配**：实现基于 BIOS 内存地图的动态页表分配，扫描 E820 内存地图，找到可用的内存区域，然后动态分配页表空间。

5. **⭐⭐⭐ 用 C 代码设置 Long Mode**：尝试把页表初始化和切换逻辑移到 C 文件中，用内联汇编或结构体构造页表。这个实验能让你体会到混合编程的便利性和陷阱。

---

## 参考资料

### Intel/AMD 手册

* **Intel SDM Vol. 3A, Chapter 9**: Processor Management and Initialization —— 详细解释了模式切换和初始化流程
* **Intel SDM Vol. 3A, Section 9.8**: Entering Long Mode —— Long Mode 切换的官方步骤
* **Intel SDM Vol. 3A, Section 4.5**: Page Tables —— 4 级页表结构的详细说明
* **AMD APM Vol. 2, Chapter 5**: Long Mode —— AMD 对 Long Mode 的原始定义

### OSDev Wiki

* [Long Mode](https://wiki.osdev.org/Long_Mode) —— Long Mode 的通俗解释和示例代码
* [Paging](https://wiki.osdev.org/Paging) —— 页表机制详解，包括 2MB 大页和 4KB 分页
* [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode) —— 完整的 Long Mode 设置教程
* [Identity Paging](https://wiki.osdev.org/Identity_Paging) —— Identity Mapping 的原理和实现

### 其他资源

* [x86-64: From 32-bit to 64-bit](https://www.codeproject.com/Articles/45788/The-x-64-is-an-advanced-bit-computing-architectur) —— x86-64 架构的历史和设计
* [Why PAE is required for Long Mode](https://stackoverflow.com/questions/15016735/why-is-pae-needed-to-enter-long-mode) —— 关于 PAE 与 Long Mode 关系的讨论

---

到这里就大功告成了。如果你跟着教程走下来，现在应该对 Long Mode 的切换有了比较深入的理解。这一步踩过很多坑——EFER.LME 位写错、页表结构错误、初始化顺序混乱——但每一步都是宝贵的学习经验。下一章我们将加载 ELF64 内核镜像，实现高半内核映射，设置 BootInfo 结构体，然后跳转到内核入口点。到那个时候，我们就能开始写 C++ 内核代码了，不用再手搓汇编。但在此之前，还有一些基础设施要打好——敬请期待。
