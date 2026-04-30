# Bootloader 开发日记 004A：E820 内存探测、INT 13h 磁盘读取，以及踩不完的段地址坑

> 标签：bootloader, E820, INT 13h, 磁盘读取, x86 汇编, ELF
> 前置：[003-2 从保护模式到 Long Mode](003-boot-long-mode-2.md)

## 写在前面

到 tag 003 为止，我们的 Cinux 已经能从 MBR 起步，穿过 Stage2 的 VESA 初始化，切换保护模式，建立页表，最终进入 64-bit Long Mode——然后在一个 `hlt` 死循环里优雅地停住。说实话，能走到这一步已经很不容易了，但一个停在 Long Mode 里什么都不做的内核，除了让你拍张截图之外，意义确实有限。

接下来我们要做的事情听起来很直接：从磁盘上把小内核读进内存，等以后 Long Mode 里填好启动信息结构再跳过去。但"从磁盘读数据"和"搞清楚内存长什么样"这两件事在 x86 上必须依赖 BIOS 中断，而 BIOS 中断只能在 Real Mode 下用。一旦我们进了保护模式，中断向量表就被 IDT 替换了，BIOS 中断全部失效——想用也用不了。

所以这一章我们要在 Stage2 的 VESA 初始化完成之后、切换保护模式之前，把两件依赖 BIOS 的大事一次性做完：用 E820 探测物理内存布局，用 INT 13h 从磁盘把小内核的 ELF header 读进内存。两件事每一个都能让人调试到凌晨三点，段地址计算、寄存器残留、DAP 结构填错……踩坑密度相当高。我们一步步来。

## 一、为什么这两件事必须在 Real Mode 做

先别急着看代码，我们先把"为什么"想清楚。

E820（INT 0x15, AX=0xE820）是 BIOS 提供的内存探测接口。它的原理是调用 BIOS 固件里的程序，让固件去读硬件的内存映射信息然后返回给你。这个程序通过 INT 0x15 中断向量调用，而 INT 0x15 的中断向量只在 Real Mode 下有效——保护模式下 IVT 被替换成了 IDT，INT 0x15 指向的不再是 BIOS 的服务例程。Intel SDM Vol.3A §15.3.1 明确定义了 E820 的调用规范，它就是一个 BIOS 服务，仅限 Real Mode 调用。

磁盘读取也是同一个道理。INT 13h 是 BIOS 提供的磁盘服务，通过中断向量表跳转到固件代码执行。保护模式下没有 IVT，INT 13h 自然不可用。

你可能会问：能不能进保护模式之后再切回 Real Mode 来调？技术上可以，这叫"unreal mode"或者"Thunk"——在保护模式和 Real Mode 之间反复切换。但这样做只会增加不必要的复杂度，而且每次模式切换都有踩坑的风险（段寄存器缓存、CR0 位残留等等）。更合理的方案是：在 Real Mode 最后的阶段，把所有依赖 BIOS 的事情一次性做完，然后一去不回头。

这就是我们在 Stage2 里安排调用时序的核心逻辑：VESA 初始化之后、保护模式切换之前，插入 E820 和 INT 13h 调用。先把所有需要大缓冲区的操作做完（VESA 用了 0x6000~0x6400），再做 E820（结果写到 0x5000，不跟任何人冲突），最后做磁盘读取（目标地址 0x10000），然后 `cli` 关中断，正式进入模式切换的临界区。

## 二、E820 内存枚举

### 调用约定

E820 是一个迭代式接口。Intel SDM Vol.3A §15.3.1 的定义是这样的：你第一次调用时把 EBX 设为 0，BIOS 返回第一条内存区域记录和一个新的 EBX 值（称为"续接值"）；你把新 EBX 原样传入再调一次，得到第二条记录和又一个 EBX；当 EBX 变回 0 时，说明所有记录都已返回，迭代结束。

每次调用时你需要设置：EAX=0x0000E820（注意高 16 位必须为 0），EDX=0x534D4150（ASCII 'SMAP'，BIOS 用于验证调用合法性的签名），ECX=缓冲区大小（告诉 BIOS 你能接收多少字节），ES:DI=目标缓冲区地址。调用返回后需要检查三个条件：CF=0 表示调用成功，EAX 应回显 'SMAP' 签名，ECX 不应小于 20（BIOS 至少返回 20 字节的基本结构）。

每条返回的记录是一个 24 字节的结构体（ACPI 3.0 扩展版）：base（8 字节 uint64）+ length（8 字节 uint64）+ type（4 字节 uint32）+ ACPI 扩展属性（4 字节 uint32）。Type 值中最重要的是 1（可用 RAM）和 2（保留区），另外还有 3（ACPI Reclaimable）、4（ACPI NVS）、5（坏内存）等。

### 常量定义与段地址计算

我们先把 `boot/common/boot.S` 开头的常量定义过一遍，这里每一条背后都有一段硬件规范或者踩坑历史：

```asm
// E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24

// Pre-calculated segment/offset values
// Real mode: physical = segment << 4 + offset
// For physical 0x5000: segment = 0x0500 (0x0500 << 4 = 0x5000)
.set E820_COUNT_SEG,             0x0500
.set E820_COUNT_OFF,             0x0000
.set E820_ENTRIES_SEG,           0x0500
.set E820_ENTRIES_OFF,           0x0004
```

E820 的结果存到物理地址 0x5000。前 4 字节是条目数量（一个 uint32），之后从 0x5004 开始是连续的 24 字节条目数组，最多 32 条。这个 24 字节来自 ACPI 3.0 扩展——base(8B) + length(8B) + type(4B) + acpi_extended(4B)。如果你用老的 20 字节缓冲区（ACPI 3.0 之前的版本），有些 BIOS 确实能工作，但遇到返回 ACPI 扩展属性的 BIOS 就会出问题。OSDev Wiki 的 [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) 页面明确建议总是使用 ECX=24。

现在我们要说段地址那个坑了。Real Mode 的段地址计算公式是 `物理地址 = 段寄存器 << 4 + 偏移`，所以你要访问物理 0x5000，段寄存器应该设 0x0500 而不是 0x5000。写成 `movw $0x5000, %es` 会把 ES 指向物理 0x50000——地址直接偏出去十倍。这个 bug 足够隐蔽，因为 QEMU 不会为此触发任何异常，你只是读到了一片未初始化的内存，然后在后续某个阶段莫名其妙地崩掉。说实话，这个坑真的坑了我半天——直到用 QEMU monitor 手动 `xp /4bx 0x5000` 检查才发现那里全是 0，然后才意识到是段寄存器设错了。

### E820 完整代码与讲解

现在我们来看 `query_memory_map` 的完整实现。这个函数通过 BIOS INT 0x15 / AX=0xE820 接口，迭代式地查询系统物理内存布局，把结果存到 0x5000 缓冲区。

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

    // EBX = 0 (first call)
    xorl %ebx, %ebx
```

函数入口先保存所有寄存器。`pushaw` 保存 8 个通用寄存器，`pushw %es` 和 `pushw %ds` 保存段寄存器——这是 Real Mode 下函数调用的基本礼仪，你不知道调用者寄存器里存了什么关键数据。然后把 DS 设为 0x0500（指向物理 0x5000），把 count 清零；ES:DI 指向 entries 的起始位置 0x0500:0x0004（物理 0x5004）。EBX 初始化为 0 表示"从第一条记录开始"。

接下来是循环体，这是 E820 调用的核心逻辑：

```asm
.e820_loop:
    movl E820_COUNT_OFF, %eax
    cmpl $E820_MAX_ENTRIES, %eax
    jae .e820_done

    // EAX=0x0000E820, EBX=continuation, ECX=bufsize, EDX='SMAP', ES:DI=buffer
    movl $E820_SIGNATURE, %edx       // EDX = 'SMAP'
    movl $E820_CMD, %eax             // EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx      // ECX = 24 (buffer size)

    int $0x15
    jc .e820_failed

    cmpl $E820_SIGNATURE, %eax
    jne .e820_failed

    // BIOS 可能返回 <24，至少要有 20
    cmpl $20, %ecx
    jb .e820_failed
```

每次循环开始先检查是否超过 32 条上限，防止缓冲区溢出。然后设置 BIOS 调用参数：EDX='SMAP'（0x534D4150），EAX=0x0000E820（注意这里用的是 `E820_CMD` 常量，值为 0x0000E820，高 16 位必须为 0——写成 0xE820 会导致某些 BIOS 行为异常，因为高 16 位里的垃圾值会被 BIOS 检查到），ECX=24。

调用 `int $0x15` 后做三重验证：CF=0 表示调用成功，EAX 应返回 'SMAP' 签名（BIOS 回显），ECX 不应小于 20。任何一项不满足都跳转到失败处理——我们这里直接调 `panic`，因为 E820 失败意味着根本不知道内存布局，后面所有操作都不可能正确。

```asm
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

成功后，DI 前进 24 字节指向下一个 entry 的位置。注意 DI 必须手动递增——BIOS 不会帮你递增，它只负责往 ES:DI 指向的缓冲区写数据。然后 count 加 1，检查 EBX 是否为 0——为 0 则迭代结束。循环退出时 CX 里是条目总数，作为返回值交给调用者。最后恢复所有寄存器并返回。

你可能会注意到这里没有做 type 过滤——我们把 BIOS 返回的所有条目都存下来了。Type 过滤是内核物理内存管理器（PMM）的事情，Bootloader 阶段只负责忠实地记录 BIOS 报告的信息。如果你在 Bootloader 里就自作主张地过滤，后面调试时会发现某些 type=3（ACPI Reclaimable）的条目不见了，然后花一下午怀疑是 BIOS bug——别问我怎么知道的。

QEMU 默认返回 5 到 7 条记录，其中 type=1 的才是可用 RAM，type=2 的是保留区。我们在后续 tag 中会让内核把这些条目打印到串口，到时候你能直观地看到内存布局。

## 三、INT 13h 扩展磁盘读取

### 为什么用扩展读取而不是传统 CHS

传统 INT 13h AH=0x02 使用 CHS（柱面-磁头-扇区）寻址，最大支持约 8GB 磁盘。INT 13h AH=0x42 使用 LBA（Logical Block Addressing）寻址，通过 DAP（Disk Address Packet）结构指定 64 位起始扇区号，理论上支持天文数字大小的磁盘。OSDev Wiki 的 [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) 页面对这两种方式都有详细描述。

我们选择扩展读取（AH=0x42）的原因很简单：QEMU 的虚拟硬盘天然使用 LBA 编址，CHS 的柱面/磁头/扇区换算在虚拟化环境里毫无意义，只会增加出错概率。而且自 1990 年代中期以来的 BIOS 都支持 INT 13h 扩展，兼容性不是问题。

### DAP 结构

DAP（Disk Address Packet）是 INT 13h 扩展读取需要的参数结构，总共 16 字节，格式如下：

```
Offset  Size  Description
 0      1     Packet size (必须为 16)
 1      1     Reserved (必须为 0)
 2      2     要读取的扇区数
 4      2     缓冲区 offset
 6      2     缓冲区 segment
 8      4     LBA 低 32 位
12      4     LBA 高 32 位
```

我们把 DAP 放在物理地址 0x7B00，这个位置是 MBR 的 DAP 区域——MBR 执行完毕后这个区域就空出来了，可以安全重用。段寄存器 0x07B0 = 0x7B00 >> 4，和 E820 那里的 0x0500 是同一段地址计算逻辑。DAP 的 size 字段必须为 16，reserved 必须为 0——有些 BIOS 实现会检查这两个字段，不正确就返回错误。

INT 13h AH=0x42 要求 DS:SI 指向 DAP 结构（不是 ES:DI），DL=0x80 表示第一块硬盘。成功标志是 CF=0 且 AH=0。

### 磁盘读取完整代码与讲解

`load_kernel_from_disk` 的任务是把小内核的 ELF header（前 4KB = 8 扇区）从磁盘 LBA=16 读取到物理内存 0x10000。为什么只读 4KB？因为本章只需要拿到 ELF header，用它来确认文件格式正确、解析 Program Header Table 获取内核总大小。真正的完整内核加载（遍历 PT_LOAD 段、重定位、BSS 清零）留到 tag 004B。

```asm
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    pushaw
    pushw %es
    pushw %ds

    movw $DAP_SEGMENT, %ax
    movw %ax, %es

    movw $DAP_OFFSET, %di

    // DAP.size = 16
    movb $16, %es:(%di)
    // DAP.reserved = 0
    // DAP.count = 8 (4KB = 8 sectors)
    movw $8, %es:2(%di)

    // DAP.buffer = 0x10000 (segment:offset format)
    movw $0x0000, %es:4(%di)             // buffer offset
    movw $0x1000, %es:6(%di)             // buffer segment (0x1000 << 4 = 0x10000)

    // DAP.lba = 16
    movl $MINI_KERNEL_LBA, %eax
    movl %eax, %es:8(%di)                // LBA low 32 bits
    xorl %eax, %eax
    movl %eax, %es:12(%di)               // LBA high 32 bits = 0
```

初始化阶段先把所有寄存器压栈保存，然后 ES 设为 0x07B0 指向 DAP 区域。接下来逐字段填充 DAP：size=16、reserved=0（没写但内存初始值为 0）、count=8（读 8 个扇区 = 4KB）、缓冲区地址为 0x1000:0x0000（物理 0x10000）、LBA=16 且高 32 位清零。

缓冲区地址 0x10000 这个选择不是随意的。0x10000 是经典的"临时加载区"——xv6 的 `bootmain.c` 也把 ELF header 读到 0x10000，Linux 0.01 的 boot sector 同样把内核读到 0x10000 起始的内存区域。这个位置在低 1MB 之内（Real Mode 可寻址范围），又不会和 MBR（0x7C00）、Stage2（0x8000）、E820 缓冲区（0x5000）、VESA 缓冲区（0x6000~0x6400）冲突。

LBA 的高 32 位必须清零——如果你只设了低 16 位就调用，高位的垃圾数据会让某些 BIOS 读取完全错误的扇区。这个 bug 在 QEMU 里不容易暴露（QEMU 的 BIOS 实现比较宽容），但在真实硬件上几乎必炸。

接下来是 BIOS 调用本身：

```asm
    // DS:SI must point to DAP
    movw $DAP_SEGMENT, %dx
    movw %dx, %ds                        // DS = 0x07B0
    movw $DAP_OFFSET, %si                // SI = 0x0000
    movb $0x80, %dl                      // DL = 0x80 (first hard disk)
    movb $DISK_READ_CMD, %ah             // AH = 0x42

    int $0x13

    jc disk_read_failed
    cmpb $0, %ah
    jne disk_read_failed
```

这里有三个关键细节。第一，INT 13h AH=42h 要求 DS:SI 指向 DAP，不是 ES:DI——这跟大多数 BIOS 调用的 ES:DI 约定不同。搞反了 BIOS 不会报错但会读到垃圾数据，或者直接超时。第二，DL 必须设为 0x80 表示"第一块硬盘"。如果你忘了设（或者 DL 里恰好还是之前 E820 调用留下的残留值），BIOS 返回 AH=0x01（Invalid Function），但神奇的是有些情况下数据其实已经读成功了——CF 才是最终的成功标志，AH 的参考价值有限。第三，我们同时检查 CF 和 AH，双重保险。

DL=0x80 那个坑值得多说两句。我们在调试时遇到过一个诡异的情况：BIOS 返回 AH=0x01 但数据看起来是正确的。折腾了半天才发现是 DL 没有显式设为 0x80——之前 `int $0x15`（E820 调用）恰好把 DL 设成了其他值，然后我们直接调了 `int $0x13`，BIOS 就用错误的驱动器号执行了。从那以后我们在每次 BIOS 调用前都会显式设置所有需要的寄存器，绝不依赖上一次调用的残留值。

```asm
    popw %ds
    popw %es
    popaw
    movw $8, %ax                         // return 8 sectors
    ret

disk_read_failed:
    popw %ds
    popw %es
    popaw
    movw $(msg_disk_read_failed), %si
    jmp panic
```

读取完成后恢复寄存器，AX 返回读取的扇区数（8）。失败则走 `panic` 流程，打印错误信息然后停机。

### Cinux 当前设计与后续演进的关系

你会发现本章的 `load_kernel_from_disk` 只读 8 个扇区（4KB），只包含 ELF header。这和 read-through 版本里的"分块循环读 832 扇区"不同——这里我们采用了一个更简洁的策略：先只读 header，确认格式正确并解析出内核总大小后，再在后续 tag（004B）里做完整加载。

这种两阶段设计的好处是调试更容易。如果你一开始就尝试读 800 多个扇区，出错了很难判断是读取逻辑的问题还是 DAP 填写的问题。先读 8 个扇区、验证 ELF magic，然后再逐步加大量——这种增量验证的方式在 Bootloader 开发中非常实用。

## 四、Stage2 集成与 Mini Kernel 占位

### 调用时机

在 `boot/stage2.S` 里，两个新函数的调用位置是精心安排的：

```asm
    call vesa_save_framebuffer_info // [->0x6400] Save FB info

    // ============================================================
    // 004_boot_load_mini_kernel_A: Completed in real mode
    // ============================================================

    call query_memory_map           // [->0x5000] E820 memory map

    call load_kernel_from_disk      // [->0x10000] Load mini kernel ELF header

    cli                             // disable interrupts again
```

调用顺序是 VESA 保存帧缓冲区信息之后，然后 E820，然后磁盘读取，最后 `cli` 关中断。这个顺序不是随便选的。VESA 初始化需要大量内存空间做缓冲区（0x6000~0x6400），如果在 VESA 之前做 E820，0x5000 处的内存映射表可能被 VESA 缓冲区覆盖。E820 结果写到 0x5000，和 VESA 缓冲区不冲突。磁盘读取目标是 0x10000，和前面所有区域都不冲突。

`cli` 紧跟在 `load_kernel_from_disk` 之后，因为接下来的 GDT 加载和模式切换是临界区，不能被中断打断。而在函数入口处我们已经 `sti` 过了——E820 和 INT 13h 都需要中断处于启用状态才能正常工作，BIOS 内部会使用中断来响应硬件事件。

### 最简小内核

小内核目前是一个极简的占位程序。`kernel/mini/main.cpp` 的全部内容就是：

```cpp
extern "C" {
[[noreturn]] void _start() {
    while (1) {
        __asm__ volatile(
            "cli; \
            hlt");
    }
}
}
```

一个 `cli; hlt` 的死循环。现阶段它的唯一意义是让构建系统有一个可编译、可链接、可写入磁盘镜像的 ELF 文件。我们不需要它做任何实际工作——它的存在是为了验证"从磁盘读到了正确的数据"这一步是否成功。后续 tag 会让小内核真正跑起来。

链接脚本 `kernel/mini/linker.ld` 定义了地址布局，内核加载到物理地址 0x200000（2MB，大页对齐）：

```
ENTRY(_start)
SECTIONS
{
    . = 0x200000;
    .text : { *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    .bss : { *(.bss) *(COMMON) }
}
```

### 构建系统集成

要让这一切正确运作，构建系统需要做三件事：把 boot.S 编进 Stage2、编译小内核、按正确布局打包磁盘镜像。

`boot/CMakeLists.txt` 把 boot.S 编译为对象库并链接进 Stage2：

```cmake
add_library(boot_common OBJECT
    common/serial.S
    common/boot.S
)

add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>
    $<TARGET_OBJECTS:boot_longmode>
)
```

`boot_common` 是一个 OBJECT 库，包含 serial.S（之前就有的打印/panic 功能）和新增的 boot.S。Stage2 链接时把 boot_common 的对象文件拉进来，这样 `query_memory_map` 和 `load_kernel_from_disk` 就成了 Stage2 二进制的一部分。

`kernel/mini/CMakeLists.txt` 负责编译小内核：

```cmake
target_compile_options(mini_kernel PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)
```

`-mcmodel=large` 是必须的——小内核的虚拟地址在 0xFFFFFFFF80000000 附近，超出了 `-mcmodel=kernel` 的 2GB 范围。`-mno-red-zone` 也是内核代码的标配，因为中断可能在任何时候打断执行，red zone 里的临时数据会被破坏。`-ffreestanding` 告诉编译器不要假设标准库的存在，`-fno-exceptions` 和 `-fno-rtti` 禁用 C++ 的异常和 RTTI（内核里没有运行时支持）。

## 五、设计对比：Cinux vs xv6 vs Linux 早期

到这里代码讲完了，我们退后一步，从更高的视角看看 Cinux 的 Bootloader 设计和其他操作系统的差异。这个对比不只是"谁用了什么函数"这种表面差异，而是深入到设计哲学层面。

### 磁盘读取策略：BIOS 服务 vs 自己动手

Cinux 在 Real Mode 下用 INT 13h AH=0x42（BIOS 扩展磁盘读取）把 ELF header 从磁盘搬进内存。这是最"省事"的方案——BIOS 帮你屏蔽了所有磁盘控制器的差异，不管底层是 IDE、SATA 还是 NVMe，INT 13h 的调用方式都一样。代价是你只能在 Real Mode 下用这个接口，而且 BIOS 的实现质量参差不齐（比如有些 BIOS 一次最多只能读 127 个扇区）。

xv6 走的是完全不同的路线。它的 `bootmain.c` 在 32 位保护模式下用 IDE PIO 直接操作端口 0x1F0~0x1F7 读取磁盘数据。xv6 的 `readseg()` 函数调用 `waitdisk()` 等待磁盘就绪、往端口写命令、然后从数据端口 `insl` 读入数据。这种方式不依赖 BIOS，进入保护模式后照样能用，而且完全可控。但代价是你得自己实现磁盘驱动——虽然 IDE PIO 相对简单（就是往几个端口读写数据），但它只支持 IDE/SATA 硬盘，遇到 NVMe 或者 USB 启动就不灵了。xv6 的 `bootmain()` 加载流程是这样的：先读 ELF header 到 0x10000，验证 magic，然后遍历 Program Headers，逐段调用 `readseg()` 把 PT_LOAD 段读到 `ph->paddr`，最后 BSS 清零并跳转到 `elf->entry`。整个流程在 C 里完成，代码清晰易读。

Linux 0.01 用的是更原始的方案——INT 13h AH=0x02（CHS 模式）。CHS 寻址需要你自己算柱面/磁头/扇区，最大只支持 8GB 磁盘。Linux 0.01 的 boot sector 用 `movb $0x02, %ah` + `int $0x13` 把内核读到 0x10000，然后 setup.s 负责获取硬件信息（包括 E820 内存探测），最后切换保护模式跳转到内核。和 Cinux 相比，Linux 0.01 的 E820 探测是在 setup 阶段做的（不是 boot sector 阶段），而且用的是 CHS 而非 LBA。

### E820 的调用时机

Cinux 把 E820 放在 VESA 之后、保护模式之前，和其他操作一样集中在一个 Real Mode 阶段完成。Linux 的启动协议不同——它的 setup.s 在 boot sector 之后、内核之前运行，setup.s 负责 E820 探测、获取视频模式信息、检查 APM/EDD 支持等，然后把所有信息打包到一个结构体里传给内核。Linux 的方式更模块化（boot sector -> setup -> kernel），但引入了更多的协议约定。xv6 完全不做 E820——它是教学操作系统，用 QEMU 运行，内存布局是固定的（128MB RAM），直接硬编码就行了。

### 数据结构选择

Cinux 的 E820 缓冲区是一个简单的扁平数组：前 4 字节是 count，之后是连续的 24 字节条目，最多 32 条。没有链表、没有动态分配、没有排序。这种设计在 Bootloader 阶段是最合理的——你没有堆分配器，没有复杂数据结构的能力，能做的就是拿一块固定内存把 BIOS 返回的数据原样存下来。Linux 内核的 E820 实现更复杂，它会把 BIOS 返回的条目做排序、合并、冲突检测，但那是在内核启动之后才做的事情。

### ELF 加载策略

当前 tag（004A）只读了 ELF header 的前 4KB，真正的 ELF 加载逻辑还在后面。但我们可以先看看 xv6 和 Linux 是怎么做的。xv6 的 `bootmain()` 是最经典的 Bootloader ELF 加载流程：验证 magic `\x7FELF` -> 遍历 Program Headers -> 逐段 `readseg()` -> BSS 清零（`stosb`）-> 跳转 entry。Linux 0.01 更简单——它直接把内核 flat binary 读到 0x10000 然后跳过去，甚至不做 ELF 解析。现代 Linux 的 Bootloader（GRUB/systemd-boot）则使用 Multiboot2 协议，Bootloader 负责解析 ELF 并加载所有段，内核只需要从入口点开始执行。

Cinux 最终走的是"xv6 式"的路径——Bootloader 自己解析 ELF header、加载段、清零 BSS、跳转 entry。但分成了多个 tag 逐步实现：004A 读 header，004B 做完整加载，004C 填 BootInfo 并跳转。这种增量开发方式在教学项目里很有效，每一步都有明确的验证目标。

## 收尾

我们来验证一下本章的成果。构建并运行 QEMU，检查 `debug.log` 文件——如果 Stage2 正常通过 E820 和磁盘读取，然后输出了 `L`（Long Mode 验证字符），说明所有新功能都在 Real Mode 阶段正确完成了。如果你想更深入地验证 E820 的结果，可以在 QEMU monitor 里用 `xp /128bx 0x5000` 查看内存映射表的原始数据——前 4 字节是条目数，之后每 24 字节是一条记录（base 8B + length 8B + type 4B + extended 4B）。

回过头看，这一章我们做了三件事：用 E820 询问 BIOS "内存长什么样"（结果存到 0x5000），用 INT 13h 从磁盘读取小内核的 ELF header（读到 0x10000），以及搭好了 mini kernel 的占位骨架和构建流水线。踩的坑主要是段地址计算（0x5000 vs 0x0500）、E820_CMD 必须用完整 32 位（0x0000E820 而不是 0xE820）、以及 DL=0x80 必须显式设置。每一个都是那种"知道了觉得理所当然、不知道时能调一整天"的坑。

下一篇（tag 004B）我们将继续推进——在保护模式下做完整的 ELF 解析和内核加载，遍历 Program Header Table，把 PT_LOAD 段读到正确的物理地址，然后 BSS 清零。那一步的复杂度比本章高不少，但好消息是段地址计算的那些坑已经踩完了——保护模式下用的是线性地址，不用再跟 segment:offset 的转换打交道。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 15.3.1 — INT 15h, EAX=E820h: Query System Address Map。定义了 E820 调用规范：EAX=0x0000E820, EDX='SMAP', ECX=buffer size, ES:DI=buffer，返回 CF=0 成功、EAX='SMAP' 签名验证、EBX 续接值。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- OSDev Wiki — Detecting Memory (x86): E820 是推荐的内存探测方法。关键要点：ECX 应为 24（ACPI 3.0），EBX 续接值的正确使用，DI 手动递增（BIOS 不帮你递增），某些 BIOS 会破坏 EDX 需每次重载。
  https://wiki.osdev.org/Detecting_Memory_(x86)

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=0x42 扩展读取详解，DAP 16 字节结构、DL=0x80 硬盘约定、buffer segment:offset 格式、每次最多读 127 扇区的限制。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — ELF: ELF64 格式结构。ELF Header 64 字节（magic `\x7FELF`、e_entry、e_phoff、e_phnum），Program Header 56 字节（p_type=PT_LOAD、p_paddr、p_filesz、p_memsz）。Bootloader 加载流程：验证 magic -> 遍历 PT_LOAD -> 读文件到 paddr -> BSS 清零 -> 跳 entry。
  https://wiki.osdev.org/ELF

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O 读取磁盘的参考实现。ELF header 读到 0x10000，遍历 Program Headers 逐段加载，BSS 清零然后跳转 entry。与 Cinux 的 BIOS INT 13h 方案形成对比——xv6 自实现磁盘驱动但可在保护模式运行，Cinux 依赖 BIOS 但仅限 Real Mode。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c

- Linux 0.01 boot sector: 使用 INT 13h AH=0x02（CHS 模式）读取内核到 0x10000，setup.s 做 E820 探测和硬件信息收集，然后切换保护模式跳转内核。与 Cinux 的主要差异：Linux 用 CHS 而非 LBA，E820 在 setup 阶段而非 boot sector 阶段完成。
  https://www.kernel.org/pub/linux/kernel/Historic/linux-0.01.tar.gz
