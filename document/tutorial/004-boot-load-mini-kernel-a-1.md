---
title: 004-boot-load-mini-kernel-a-1 · 内核加载 (A)
---

# Bootloader 开发日记 004A：E820 内存探测、INT 13h 磁盘读取，以及踩不完的段地址坑

> 标签：bootloader, E820, INT 13h, 磁盘读取, x86 汇编, flat binary
> 前置：[003 从保护模式到 Long Mode](003-boot-long-mode-1.md)

## 写在前面

到 tag 003 为止，我们的 Cinux 已经能从 MBR 起步，穿过 Stage2 的 VESA 初始化，切换保护模式，建立页表，最终进入 64-bit Long Mode——然后在一个 `hlt` 死循环里优雅地停住。说实话，能走到这一步已经很不容易了，但一个停在 Long Mode 里什么都不做的内核，除了让你拍张截图之外，意义确实有限。

接下来我们要做的事情听起来很直接：从磁盘上把小内核读进内存，等 Long Mode 里填好 BootInfo 再跳过去。但"从磁盘读数据"和"搞清楚内存长什么样"这两件事在 x86 上必须依赖 BIOS 中断，而 BIOS 中断只能在 Real Mode 下用。一旦我们进了保护模式，中断向量表就被 IDT 替换了，BIOS 中断全部失效——想用也用不了。

所以这一章我们要在 Stage2 的 VESA 初始化完成之后、切换保护模式之前，把两件依赖 BIOS 的大事一次性做完：用 E820 探测物理内存布局，用 INT 13h 从磁盘把小内核的完整 flat binary 读进内存。两件事每一个都能让人调试到凌晨三点，段地址计算、寄存器残留、DAP 结构填错、BIOS 破坏循环计数器……踩坑密度相当高。我们一步步来。

## 一、为什么这两件事必须在 Real Mode 做

先别急着看代码，我们先把"为什么"想清楚。

E820（INT 0x15, AX=0xE820）是 BIOS 提供的内存探测接口。它的原理是调用 BIOS 固件里的程序，让固件去读硬件的内存映射信息然后返回给你。这个程序通过 INT 0x15 中断向量调用，而 INT 0x15 的中断向量只在 Real Mode 下有效——保护模式下 IVT 被替换成了 IDT，INT 0x15 指向的不再是 BIOS 的服务例程。

磁盘读取也是同一个道理。INT 13h 是 BIOS 提供的磁盘服务，通过中断向量表跳转到固件代码执行。保护模式下没有 IVT，INT 13h 自然不可用。

你可能会问：能不能进保护模式之后再切回 Real Mode 来调？技术上可以，这叫"unreal mode"或者"Thunk"——在保护模式和 Real Mode 之间反复切换。但这样做只会增加不必要的复杂度，而且每次模式切换都有踩坑的风险（段寄存器缓存、CR0 位残留等等）。更合理的方案是：在 Real Mode 最后的阶段，把所有依赖 BIOS 的事情一次性做完，然后一去不回头。

这就是我们在 Stage2 里安排调用时序的核心逻辑：VESA 初始化之后、保护模式切换之前，插入 E820 和 INT 13h 调用。先把所有需要大缓冲区的操作做完（VESA 用了 0x6000~0x6400），再做 E820（结果写到 0x5000，不跟任何人冲突），最后做磁盘读取（目标地址 0x20000），然后 `cli` 关中断，正式进入模式切换的临界区。

## 二、E820 内存枚举

### 调用约定

E820 是一个迭代式接口。你第一次调用时把 EBX 设为 0，BIOS 返回第一条内存区域记录和一个新的 EBX 值（称为"续接值"）；你把新 EBX 原样传入再调一次，得到第二条记录和又一个 EBX；当 EBX 变回 0 时，说明所有记录都已返回，迭代结束。

每次调用时你需要设置：EAX=0x0000E820（注意高 16 位必须为 0），EDX=0x534D4150（ASCII 'SMAP'，BIOS 用于验证调用合法性的签名），ECX=缓冲区大小（告诉 BIOS 你能接收多少字节），ES:DI=目标缓冲区地址。调用返回后需要检查三个条件：CF=0 表示调用成功，EAX 应回显 'SMAP' 签名，ECX 不应小于 20（BIOS 至少返回 20 字节的基本结构）。

每条返回的记录是一个 24 字节的结构体（ACPI 3.0 扩展版）：base（8 字节 uint64）+ length（8 字节 uint64）+ type（4 字节 uint32）+ ACPI 扩展属性（4 字节 uint32）。Type 值中最重要的是 1（可用 RAM）和 2（保留区），另外还有 3（ACPI Reclaimable）、4（ACPI NVS）、5（坏内存）等。OSDev Wiki 的 [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) 页面对此有完整说明。

### 常量定义与段地址计算

我们先把 `boot/common/boot.S` 开头的常量定义过一遍：

```asm
// E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24

// Pre-calculated segment/offset values
// Real mode: physical = segment << 4 + offset
.set E820_COUNT_SEG,             0x0500
.set E820_COUNT_OFF,             0x0000
.set E820_ENTRIES_SEG,           0x0500
.set E820_ENTRIES_OFF,           0x0004
```

E820 的结果存到物理地址 0x5000。前 4 字节是条目数量（一个 uint32），之后从 0x5004 开始是连续的 24 字节条目数组，最多 32 条。这个 24 字节来自 ACPI 3.0 扩展——base(8B) + length(8B) + type(4B) + acpi_extended(4B)。OSDev Wiki 明确建议总是使用 ECX=24。

现在我们要说段地址那个坑了。Real Mode 的段地址计算公式是 `物理地址 = 段寄存器 << 4 + 偏移`，所以你要访问物理 0x5000，段寄存器应该设 0x0500 而不是 0x5000。写成 `movw $0x5000, %es` 会把 ES 指向物理 0x50000——地址直接偏出去十倍。这个 bug 足够隐蔽，因为 QEMU 不会为此触发任何异常，你只是读到了一片未初始化的内存，然后在后续某个阶段莫名其妙地崩掉。说实话，这个坑真的坑了我半天——直到用 QEMU monitor 手动 `xp /4bx 0x5000` 检查才发现那里全是 0，然后才意识到是段寄存器设错了。

### E820 完整代码与讲解

现在我们来看 `query_memory_map` 的完整实现：

```asm
.global query_memory_map
.type query_memory_map, @function
query_memory_map:
    pushaw
    pushw %es
    pushw %ds

    movw $0x0, %ax
    movw $E820_COUNT_SEG, %dx
    movw %dx, %ds
    movw %ax, E820_COUNT_OFF     // count = 0

    movw $E820_ENTRIES_SEG, %ax
    movw %ax, %es
    movw $E820_ENTRIES_OFF, %di

    xorl %ebx, %ebx
```

函数入口先保存所有寄存器。然后把 DS 设为 0x0500（指向物理 0x5000），把 count 清零；ES:DI 指向 entries 的起始位置 0x0500:0x0004（物理 0x5004）。EBX 初始化为 0 表示"从第一条记录开始"。

接下来是循环体：

```asm
.e820_loop:
    movl E820_COUNT_OFF, %eax
    cmpl $E820_MAX_ENTRIES, %eax
    jae .e820_done

    movl $E820_SIGNATURE, %edx       // EDX = 'SMAP'
    movl $E820_CMD, %eax             // EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx      // ECX = 24

    int $0x15
    jc .e820_failed

    cmpl $E820_SIGNATURE, %eax
    jne .e820_failed

    cmpl $20, %ecx
    jb .e820_failed

    addl $E820_ENTRY_SIZE, %edi

    movl E820_COUNT_OFF, %eax
    incl %eax
    movl %eax, E820_COUNT_OFF

    testl %ebx, %ebx
    jnz .e820_loop

.e820_done:
    movl E820_COUNT_OFF, %eax
    movl %eax, %ecx

    popw %ds
    popw %es
    popaw
    ret
```

每次循环开始先检查是否超过 32 条上限，防止缓冲区溢出。然后设置 BIOS 调用参数并调用 `int $0x15`。调用后做三重验证：CF=0、EAX='SMAP'、ECX>=20。成功后 DI 前进 24 字节，count 加 1，检查 EBX 是否为 0。循环退出时 CX 里是条目总数，作为返回值交给调用者。

你可能会注意到这里没有做 type 过滤——我们把 BIOS 返回的所有条目都存下来了。Type 过滤是内核物理内存管理器（PMM）的事情，Bootloader 阶段只负责忠实地记录 BIOS 报告的信息。

## 三、INT 13h 分块循环磁盘读取

### 为什么用扩展读取而不是传统 CHS

传统 INT 13h AH=0x02 使用 CHS（柱面-磁头-扇区）寻址，最大支持约 8GB 磁盘。INT 13h AH=0x42 使用 LBA（Logical Block Addressing）寻址，通过 DAP（Disk Address Packet）结构指定 64 位起始扇区号，理论上支持天文数字大小的磁盘。OSDev Wiki 的 [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) 页面对两种方式都有详细描述。

### 分块读取：127 扇区的 BIOS 上限

本章要读 832 个扇区（416KB），但 BIOS INT 13h AH=42h 对单次读取的扇区数有上限——通常是 127 个扇区。这个限制的根源是 BIOS 内部的 DMA 缓冲区大小。解决方案是分块循环读取：每次读最多 127 个扇区，读完一批后更新 DAP 里的 LBA 和 buffer 地址，再读下一批。

buffer 地址的动态计算是这样的：基础段地址 0x2000，加上已读扇区数乘以 32（每个扇区 512 字节 / 16 = 32 个"段单位"，段地址每加 1 对应物理地址加 16 字节）。LBA 的计算是 16 加上已读扇区数。

这里有一个特别阴险的坑：BIOS 调用会破坏 BX 和 BP 寄存器的值。Intel SDM 没有明确列出"哪些寄存器会被 BIOS 破坏"，但实际测试表明，很多 BIOS 实现在 INT 13h 内部会使用 BX 和 BP 作为临时寄存器且不恢复。如果你用 BX 来跟踪已读扇区数却不保存，循环第二轮的 BX 就是一个垃圾值。所以每次 BIOS 调用前必须 pushw 保存 BX 和 BP，调用后 popw 恢复。

### 加载地址为什么是 0x20000

经典的"临时加载区"是 0x10000（xv6、Linux 0.01 都用过），但如果把 416KB 的内核从 0x10000 开始加载，数据会覆盖 Real Mode 栈空间（0x9000~0x19000）。Stage2 的栈设置是 SS=0x0900、SP=0xFFFE，物理地址范围是 0x9000 到 0x19000。磁盘读取把数据写到 0x10000~0x19000 的区域，覆盖了栈里保存的返回地址，函数执行 `ret` 的时候跳到一个莫名其妙的地址，直接 triple fault。

0x20000 避开了栈空间，内核最大 416KB 从 0x20000 加到 0x88000，距离 Protected Mode 栈（0x90000）还有 32KB 安全距离。

### 磁盘读取完整代码与讲解

```asm
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    pusha
    pushw %es
    pushw %ds

    xorw %bx, %bx                        // BX = 0 (sectors read)
    xorw %si, %si

    movw $DAP_SEGMENT, %dx
    movw %dx, %es

.read_loop:
    cmpw $MINI_KERNEL_SECTORS, %bx
    jae .read_done

    // Calculate sectors to read this iteration
    movw $MINI_KERNEL_SECTORS, %ax
    subw %bx, %ax                        // remaining
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax
    jbe .read_count_ok
    movw $DISK_MAX_SECTORS_PER_CALL, %ax

.read_count_ok:
    movw %ax, %bp                        // BP = sectors this round

    movw $DAP_SEGMENT, %dx
    movw %dx, %es

    // Build DAP
    movw $DAP_OFFSET, %di
    movb $16, %es:(%di)                  // size
    movb $0, %es:1(%di)                  // reserved
    movw %bp, %es:2(%di)                 // count

    // Buffer address = MINI_KERNEL_LOAD_SEG + (BX << 5)
    movw %bx, %ax
    shlw $5, %ax
    addw $MINI_KERNEL_LOAD_SEG, %ax
    movw %ax, %es:6(%di)                 // buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax
    movw %ax, %es:4(%di)                 // buffer offset

    // LBA = MINI_KERNEL_LBA + BX
    movw $MINI_KERNEL_LBA, %ax
    addw %bx, %ax
    movw %ax, %es:8(%di)                 // LBA low
    xorw %ax, %ax
    movw %ax, %es:10(%di)                // LBA high
    movw %ax, %es:12(%di)
    movw %ax, %es:14(%di)

    // Save BX and BP before BIOS call
    pushw %bx
    pushw %bp

    movw $DAP_SEGMENT, %dx
    movw %dx, %ds                        // DS:SI -> DAP
    movw $DAP_OFFSET, %si
    movb $0x80, %dl                      // DL = 0x80
    movb $DISK_READ_CMD, %ah

    int $0x13

    jc .disk_error_restore_bp
    cmpb $0, %ah
    jne .disk_error_restore_bp

    popw %bp
    popw %bx

    addw %bp, %bx                        // BX += sectors read
    jmp .read_loop
```

每轮循环的流程：计算本轮读取量 -> 构建 DAP -> 保存 BX/BP -> 调用 BIOS -> 恢复 BX/BP -> 更新计数器 -> 继续循环。DL=0x80 那个坑值得多说两句——我们在调试时遇到过一个诡异的情况：BIOS 返回 AH=0x01 但数据看起来是正确的。折腾了半天才发现是 DL 没有显式设为 0x80——之前 `int $0x15`（E820 调用）恰好把 DL 设成了其他值，然后我们直接调了 `int $0x13`，BIOS 就用错误的驱动器号执行了。从那以后我们在每次 BIOS 调用前都会显式设置所有需要的寄存器，绝不依赖上一次调用的残留值。

```asm
.read_done:
    popw %ds
    popw %es
    popa
    movb $'O', %al
    outb %al, $0xe9                      // 'O' = disk OK
    movw $MINI_KERNEL_SECTORS, %ax       // return 832
    ret

disk_read_failed:
    movb $'F', %al
    outb %al, $0xe9
    popw %ds
    popw %es
    popa
    movw $(msg_disk_read_failed), %si
    jmp panic
```

读取完成后恢复寄存器，输出 'O' 到 debugcon 表示磁盘读取成功，AX 返回总扇区数。失败则输出 'F' 然后走 `panic` 流程。

## 四、Stage2 集成与构建系统

### 调用时机

在 `boot/stage2.S` 里，两个新函数的调用位置是精心安排的：

```asm
    call vesa_save_framebuffer_info

    // 004_boot_load_mini_kernel_A: Completed in real mode
    call query_memory_map
    call load_kernel_from_disk
    cli

    // Switch to Protected Mode
```

调用顺序是 VESA 之后、E820、磁盘读取，最后 `cli`。`cli` 紧跟在 `load_kernel_from_disk` 之后，因为接下来的 GDT 加载和模式切换是临界区，不能被中断打断。

### Mini Kernel 链接脚本

Mini Kernel 的物理加载地址是 0x20000（不是经典的 0x200000/2MB），虚拟地址是 0xFFFFFFFF80020000（高半核）：

```ld
KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;
    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) { ... }
    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) { ... }
    .bss : { ... }
}
```

链接完成后用 `objcopy -O binary` 转换成 flat binary。Bootloader 直接读取这个 flat binary 到 0x20000，不需要任何 ELF 解析——因为链接脚本里指定的物理地址和 Bootloader 的加载地址是同一个值，字节流被放到正确的位置后，代码里的地址引用天然就是对的。

### 构建系统

`boot/CMakeLists.txt` 把 boot.S 编译为对象库并链接进 Stage2。`kernel/mini/CMakeLists.txt` 负责编译小内核并用 `objcopy` 转换为 flat binary。`-mcmodel=large` 是必须的——小内核的虚拟地址在 0xFFFFFFFF80000000 附近，超出默认模型的范围。`-mno-red-zone` 是内核代码的标配，因为中断可能在任何时候打断执行，red zone 里的临时数据会被破坏。

## 收尾

回过头看，这一章我们做了两件事：用 E820 询问 BIOS "内存长什么样"（结果存到 0x5000），用 INT 13h 分块循环读取从磁盘把小内核的完整 flat binary（416KB，832 扇区）搬到 0x20000。踩的坑主要是段地址计算（0x5000 vs 0x0500）、E820_CMD 必须用完整 32 位（0x0000E820 而不是 0xE820）、DL=0x80 必须显式设置、以及 BIOS 调用会破坏 BX/BP 导致循环计数器被覆盖。每一个都是那种"知道了觉得理所当然、不知道时能调一整天"的坑。

下一篇（tag 004C）我们将继续推进——在 Long Mode 下把所有零散的引导信息打包成 BootInfo 结构体，设置高半核映射，然后跳转到 Mini Kernel 入口。到那个时候，debugcon 输出的 `OPLJ` 序列中最后一个 `J` 字符出现后，Bootloader 功成身退，内核接管一切。

## 参考资料

- OSDev Wiki — Detecting Memory (x86): E820 是推荐的内存探测方法。关键要点：ECX 应为 24（ACPI 3.0），EBX 续接值的正确使用，DI 手动递增（BIOS 不帮你递增），某些 BIOS 会破坏 EDX 需每次重载。
  https://wiki.osdev.org/Detecting_Memory_(x86)

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=0x42 扩展读取详解，DAP 16 字节结构、DL=0x80 硬盘约定、buffer segment:offset 格式、每次最多读 127 扇区的限制。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — ELF: ELF64 格式结构。虽然本章使用 flat binary，但了解 ELF 格式有助于理解为什么选择 flat binary 以及后续何时需要 ELF 解析。
  https://wiki.osdev.org/ELF

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O 读取磁盘的参考实现。与 Cinux 的 BIOS INT 13h 方案形成对比——xv6 自实现磁盘驱动但可在保护模式运行，Cinux 依赖 BIOS 但仅限 Real Mode。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c
