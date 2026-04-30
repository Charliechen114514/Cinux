# 004 通读版 · 从磁盘加载小内核

到 tag 003 为止，我们的 Cinux 已经能从 MBR 起步，穿过 Stage2 的 VESA 初始化，切换保护模式，建立页表，最终进入 64-bit Long Mode——然后在 `hlt` 死循环里优雅地停住。说实话，能走到这一步已经很不容易了，但一个停在 Long Mode 里什么都不做的内核，除了能让你拍张截图发朋友圈之外，意义有限。

现在我们要做的事情听起来很直接：从磁盘上把小内核读进内存，在 Long Mode 里填好启动信息结构，然后跳过去。但"从磁盘读数据"这件事在 Real Mode 下必须依赖 BIOS 中断，而 BIOS 中断有两个大坑——E820 内存探测和 INT 13h 扩展磁盘读取，每一个都能让人调试到凌晨三点。这一章我们就来把这两个坑踩平，同时搭好 mini kernel 的占位骨架和完整的构建流水线。

本章的策略是分阶段推进：先在 Real Mode 下完成 E820 内存探测和 ELF header 的读取（只读 4KB，拿到文件头信息），真正的完整内核加载（解析 Program Header、遍历 PT_LOAD 段、重定位、BSS 清零）留到 tag 004B。这种两阶段设计的好处是调试更容易——如果你一开始就尝试读几百个扇区，出错了很难判断是读取逻辑的问题还是 DAP 填写的问题。

## 架构图

先看全局，搞清楚这一章新增的所有东西在启动链条里处于什么位置：

```
                    Cinux Tag 004A 启动流程
                    =======================

  Power On
     |
     v
  [MBR @ 0x7C00]  ---  加载 Stage2 到 0x8000，跳转
     |
     v
  [Stage2 @ 0x8000]
     ├── enable_a20
     ├── VESA 初始化 (0x6000 / 0x6200 / 0x6400)
     |
     ├── query_memory_map()       <-- NEW: E820 -> 0x5000
     ├── load_kernel_from_disk()  <-- NEW: INT 13h -> 0x10000 (ELF header only)
     |
     ├── 切换保护模式 (CR0.PE = 1)
     ├── setup_page_tables()      (0x1000 ~ 0x3FFF)
     ├── enter_long_mode()
     |
     └── [Long Mode]
          └── hlt (004A 只读取，不跳转; 004C 填 BootInfo 并跳转)
```

内存布局视角——低 1MB 里哪些区域被谁占了：

```
  物理地址        内容                     所属 Tag
  ─────────────────────────────────────────────────
  0x0000~0x0400   IVT (中断向量表)          BIOS
  0x07C00         MBR (512B)               000
  0x08000         Stage2 (最大 7680B)       001~003
  0x5000          E820 内存映射表            004A <-- NEW
  0x6000          VBE Controller Info       002
  0x6200          VBE Mode Info             002
  0x6400          Framebuffer Info          002
  0x7B00          DAP (磁盘读取参数包)       004A <-- NEW
  0x9000          Real Mode 栈               001
  ─────────────────────────────────────────────────
  0x10000         Mini Kernel ELF Header     004A <-- NEW (4KB, 8 sectors)
  0x90000         Protected/Long Mode 栈    002
  ─────────────────────────────────────────────────
  0x200000        Mini Kernel 运行地址       004 (后续 tag)
```

磁盘布局——镜像文件的扇区分配：

```
  扇区号          内容                     大小限制
  ─────────────────────────────────────────────────
  0              MBR                      512B (1 扇区)
  1 ~ 15         Stage2                   最大 7680B (15 扇区)
  16+            Mini Kernel (ELF)        按需扩展
```

你会发现整个设计有一条非常清晰的主线：在 Real Mode 最后的阶段（VESA 之后、保护模式之前），把所有依赖 BIOS 中断的事情一次性做完——问清楚内存布局，把小内核的 ELF header 从磁盘读进内存——然后一去不回头，再也不回 Real Mode。

## 代码精讲

### 常量定义：与硬件约定的魔法数字

`boot/common/boot.S` 开头是一大段 `.set` 常量定义。这些不是随便起的变量名，每一个背后都有一段硬件规范或者踩坑历史。

```asm
// E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24
```

E820 的结果存到物理地址 0x5000，前 4 字节是条目数量（一个 uint32），之后是连续的 24 字节条目数组，最多 32 条。这个 24 字节来自 ACPI 3.0 扩展：base(8B) + length(8B) + type(4B) + acpi_extended(4B)。如果你用老的 20 字节缓冲区，有些 BIOS 确实能工作，但遇到返回 ACPI 扩展属性的 BIOS 就会出问题。QEMU 默认返回 6 到 7 条记录，其中 type=1 的才是可用 RAM。

```asm
// Pre-calculated segment/offset values
// Real mode: physical = segment << 4 + offset
// For physical 0x5000: segment = 0x0500 (0x0500 << 4 = 0x5000)
.set E820_COUNT_ADDR,           0x5000
.set E820_ENTRIES_ADDR,          0x5004
.set E820_COUNT_SEG,             0x0500      // 0x0500 << 4 = 0x5000
.set E820_COUNT_OFF,             0x0000
.set E820_ENTRIES_SEG,           0x0500
.set E820_ENTRIES_OFF,           0x0004      // 0x5000 + 4 = 0x5004
```

这里有一段血泪史。Real Mode 的段地址计算公式是 `物理地址 = 段寄存器 << 4 + 偏移`，所以你要访问物理 0x5000，段寄存器应该设 0x0500 而不是 0x5000。写成 `movw $0x5000, %es` 会把 ES 指向物理 0x50000——地址直接偏出去十倍。这个 bug 足够隐蔽，因为 QEMU 不会为此触发任何异常，你只是读到了一片未初始化的内存，然后在后续某个阶段莫名其妙地崩掉。

接下来是 DAP（Disk Address Packet）相关的常量：

```asm
// DAP (Disk Address Packet) structure offsets (16 bytes)
.set DAP_SIZE,                0
.set DAP_RESERVED,            1
.set DAP_COUNT,               2
.set DAP_BUFFER_OFFSET,       4
.set DAP_BUFFER_SEGMENT,      6
.set DAP_LBA,                 8

// DAP fixed location in low memory
.set DAP_PHYS_ADDR,           0x7B00
.set DAP_SEGMENT,             0x07B0      // DAP segment = 0x7B00 >> 4
.set DAP_OFFSET,              0x0000

// Disk read constants
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set KERNEL_LOAD_SEGMENT,     0x1000      // 0x10000 = 0x1000:0x0000
.set KERNEL_LOAD_OFFSET,      0x0000

.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
```

DAP 是 INT 13h 扩展读取需要的参数结构，总共 16 字节。我们把 DAP 放在物理地址 0x7B00，这个位置是 MBR 的 DAP 区域——MBR 执行完毕后这个区域就空出来了，可以安全重用。段寄存器 0x07B0 = 0x7B00 >> 4，和前面的 0x0500 是同一段地址计算逻辑。

磁盘读取的关键参数：小内核从 LBA 16 开始读取（扇区 0~15 分别是 MBR 和 Stage2）。本章只读 8 个扇区（4KB = ELF header 大小）到物理地址 0x10000，对应段 0x1000、偏移 0x0000。0x10000 这个位置是经典的"临时加载区"——xv6 的 `bootmain.c` 也把 ELF header 读到 0x10000，Linux 0.01 的 boot sector 同样把内核读到 0x10000 起始的内存区域。这个位置在低 1MB 之内（Real Mode 可寻址范围），又不会和 MBR（0x7C00）、Stage2（0x8000）、E820 缓冲区（0x5000）、VESA 缓冲区（0x6000~0x6400）冲突。

### E820 内存探测：询问 BIOS "内存长什么样"

`query_memory_map` 是第一个核心函数。它的任务是通过 BIOS INT 0x15 / AX=0xE820 接口，迭代式地查询系统物理内存布局，把结果存到 0x5000 缓冲区。

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

函数入口先保存所有寄存器（`pushaw` 保存 8 个通用寄存器，`pushw %es` 和 `pushw %ds` 保存段寄存器），这是 Real Mode 下函数调用的基本礼仪——你不知道调用者寄存器里存了什么关键数据。然后把 DS 设为 0x0500（指向 0x5000），把 count 清零；ES:DI 指向 entries 的起始位置 0x0500:0x0004（物理 0x5004）。EBX 初始化为 0 表示"从第一条记录开始"。

E820 是迭代式接口：你第一次调用时 EBX=0，BIOS 返回第一条记录和一个新的 EBX 续接值；你把新 EBX 原样传入再调一次，得到第二条记录和又一个 EBX；当 EBX 变回 0 时，说明迭代结束。

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

    // BIOS may return <24, must have at least 20
    cmpl $20, %ecx
    jb .e820_failed
```

每次循环开始先检查是否超过 32 条上限，防止缓冲区溢出。然后设置 BIOS 调用参数：EDX='SMAP'（0x534D4150，这是 BIOS 用于验证的签名），EAX=0x0000E820（注意高 16 位必须为 0，写成 0xE820 会导致某些 BIOS 行为异常），ECX=24（缓冲区大小，告诉 BIOS 我们能接收的最大字节数）。

调用 `int $0x15` 后做三重验证：CF=0 表示调用成功，EAX 应返回 'SMAP' 签名（BIOS 回显），ECX 不应小于 20（BIOS 至少返回 20 字节的基本结构）。任何一项不满足都跳转到失败处理。

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

你可能注意到这里没有做 type 过滤——我们把 BIOS 返回的所有条目都存下来了。Type 过滤是内核物理内存管理器（PMM）的事情，bootloader 阶段只负责忠实地记录 BIOS 报告的信息。如果你在 bootloader 里就自作主张地过滤，后面调试时你会发现某些 type=3（ACPI Reclaimable）的条目不见了，然后花一下午怀疑是 BIOS bug。

### INT 13h 扩展读取：把 ELF header 搬进内存

`load_kernel_from_disk` 在本章的职责很明确：把小内核 ELF 文件的前 4KB（8 个扇区）从磁盘 LBA=16 读取到物理内存 0x10000。为什么只读 4KB？因为本章只需要拿到 ELF header，用它来确认文件格式正确、解析 Program Header Table 获取内核总大小。真正的完整内核加载（遍历 PT_LOAD 段、重定位、BSS 清零）留到 tag 004B。

```asm
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    pushaw                               // save all general registers
    pushw %es                            // save ES
    pushw %ds                            // save DS

    // Set ES to DAP segment (for DAP access at fixed low memory)
    movw $DAP_SEGMENT, %ax               // ES = 0x07B0
    movw %ax, %es

    // Step 1: Build DAP at fixed location 0x7B00
    movw $DAP_OFFSET, %di                // DI = 0x0000

    // Step 2: Fill DAP.size = 16
    movb $16, %es:(%di)                  // set DAP size

    // Step 3: Fill DAP.count = 8 (4KB = 8 sectors)
    movw $8, %es:2(%di)                  // set sector count to 8

    // Step 4: Fill DAP.buffer = 0x10000 (segment:offset format)
    movw $0x0000, %es:4(%di)             // buffer offset
    movw $0x1000, %es:6(%di)             // buffer segment (0x1000 << 4 = 0x10000)

    // Step 5: Fill DAP.lba = MINI_KERNEL_LBA (16)
    movl $MINI_KERNEL_LBA, %eax          // LBA=16
    movl %eax, %es:8(%di)                // set start LBA (low 32 bits)
    xorl %eax, %eax                      // high 32 bits = 0
    movl %eax, %es:12(%di)               // LBA high 32 bits

    // Step 6: Call INT 0x13 AH=0x42 extended read
    // DS:SI must point to DAP (0x07B0:0x0000 = physical 0x7B00)
    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %ds                        // DS = 0x07B0
    movw $DAP_OFFSET, %si                // SI = 0x0000
    movb $0x80, %dl                      // DL = 0x80 (first hard disk)
    movb $DISK_READ_CMD, %ah             // AH=0x42
    int $0x13                            // execute disk read

    // Step 7: Check error
    // CF=1 or AH!=0 means failure
    jc disk_read_failed                  // CF=1 means failure
    cmpb $0, %ah                         // AH should be 0 on success
    jne disk_read_failed                 // AH!=0 means error

    // Step 8: Restore registers and return sector count (8)
    popw %ds                             // restore DS
    popw %es                             // restore ES
    popaw                                // restore general registers
    movw $8, %ax                         // return 8 sectors
    ret
```

这个函数是一次性读取——构建 DAP、调用 INT 0x13、检查结果，逻辑非常直接。但有几个关键细节值得展开。

首先，DAP 的构建方式是直接往 0x7B00 处的内存写值。ES:DI 指向 DAP 的起始位置，然后按偏移逐字段填充：size=16、count=8（4KB = 8 扇区）、buffer 地址为 0x1000:0x0000（物理 0x10000）、LBA=16 且高 32 位清零。LBA 的高 32 位必须显式清零——不能假定内存里是干净的。如果 DAP 所在的 0x7B00 区域有残留数据，高位的垃圾值会让 BIOS 尝试读取一个天文数字般的 LBA，直接返回错误。这个 bug 在 QEMU 里不容易暴露（QEMU 的 BIOS 实现比较宽容），但在真实硬件上几乎必炸。

然后是 BIOS 调用本身。INT 13h AH=42h 要求 DS:SI 指向 DAP，不是 ES:DI——这跟大多数 BIOS 调用的 ES:DI 约定不同。搞反了 BIOS 不会报错但会读到垃圾数据，或者直接超时。DL 必须设为 0x80 表示"第一块硬盘"。如果你忘了设（或者 DL 里恰好还是之前 E820 调用留下的残留值），BIOS 返回 AH=0x01（Invalid Function），但神奇的是有些情况下数据其实已经读成功了——CF 才是最终的成功标志，AH 的参考价值有限。

成功后恢复所有寄存器，AX 返回读取的扇区数（8）。

```asm
disk_read_failed:
    // Restore registers then jump to panic
    popw %ds                             // restore DS
    popw %es                             // restore ES
    popaw                                // restore general registers
    movw $(msg_disk_read_failed), %si    // load error message
    jmp panic                            // call panic
```

失败则恢复寄存器后跳转 `panic`——打印错误信息然后停机。注意错误路径也必须对称恢复寄存器，否则栈不平衡，后续代码全部乱套。

### Stage2 调用时机：Real Mode 的最后一舞

在 `boot/stage2.S` 里，两个新函数的调用位置经过精心安排：

```asm
    call vesa_save_framebuffer_info // [->0x6400] Save FB info

    // ============================================================
    // 004_boot_load_mini_kernel_A: Completed in real mode
    // ============================================================

    call query_memory_map           // [->0x5000] E820 memory map

    call load_kernel_from_disk      // [->0x10000] Load mini kernel ELF header

    cli                             // disable interrupts again
```

在 VESA 初始化完成之后、切换保护模式之前。这个位置不是随便选的——E820 和 INT 13h 都只能在 Real Mode 下调用（它们依赖 BIOS 中断向量），而 VESA 初始化需要大量内存空间做缓冲区（0x6000~0x6400），如果在 VESA 之前做 E820，0x5000 处的内存映射表可能被 VESA 缓冲区覆盖。所以正确的顺序是：先完成所有需要大缓冲区的操作（VESA），再做不需要额外缓冲区的 E820（结果直接写到 0x5000），最后做磁盘读取（目标地址 0x10000 和前面所有区域都不冲突）。

`cli` 紧跟在 `load_kernel_from_disk` 之后，因为接下来的 GDT 加载和模式切换是临界区，不能被中断打断。而在函数入口处我们已经 `sti` 过了——E820 和 INT 13h 都需要中断处于启用状态才能正常工作，BIOS 内部会使用中断来响应硬件事件。

### Mini Kernel：极简的占位目标

小内核在当前 tag（004A）阶段是一个极简的占位程序。`kernel/mini/main.cpp` 的全部内容就是：

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

一个 `cli; hlt` 的死循环。现阶段它的唯一意义是让构建系统有一个可编译、可链接、可写入磁盘镜像的 ELF 文件。我们不需要它做任何实际工作——它的存在是为了验证"从磁盘读到了正确的数据"这一步是否成功。后续 tag 会让小内核逐步完善，加入 SSE 启用、栈设置、BSS 清零、全局构造函数调用等初始化流程。

链接脚本 `kernel/mini/linker.ld` 定义了地址布局：

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

内核加载到物理地址 0x200000（2MB，大页对齐）。这个地址在后续 tag 中会配合 higher-half 设计，VMA 设为 0xFFFFFFFF80020000，通过页表映射到物理地址。

### 构建系统集成

要让这一切正确运作，构建系统需要做三件事：把 boot.S 编进 Stage2、编译小内核、按正确布局打包磁盘镜像。

`boot/CMakeLists.txt` 把 boot.S 编译为对象库并链接进 Stage2：

```cmake
add_library(boot_common OBJECT
    common/serial.S
    common/boot.S
)
```

`boot_common` 是一个 OBJECT 库，包含 serial.S（之前就有的打印/panic 功能）和新增的 boot.S。Stage2 链接时把 boot_common 的对象文件拉进来，这样 `query_memory_map` 和 `load_kernel_from_disk` 就成了 Stage2 二进制的一部分。

`cmake/qemu.cmake` 中，镜像构建新增了对 `mini_kernel` 的依赖：

```cmake
set(MINI_ELF   "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel")
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_ELF}
        ${CINUX_IMAGE_PATH}
    DEPENDS mbr stage2 mini_kernel
)
```

`mini_kernel` 目标在 `kernel/mini/CMakeLists.txt` 中定义，编译完成后用 `objcopy -O binary` 转换成纯二进制（flat binary）。`build_image.sh` 负责把所有组件按正确扇区位置写入磁盘镜像：扇区 0 给 MBR，扇区 1-15 给 Stage2，扇区 16 开始给 Mini Kernel。

## 设计决策

### 决策1：在 Real Mode 完成 E820 vs 延迟到保护模式

我们选择在 Real Mode 下完成 E820，原因是简单的：E820 只能通过 INT 0x15 调用，而 INT 0x15 是 BIOS 中断，只在 Real Mode 下可用。进入保护模式后，中断向量表被替换成了 IDT，BIOS 中断全部失效。你当然可以切换回 Real Mode 再调（所谓的 "unreal mode" 技巧），但那只会增加不必要的复杂度。

xv6 的做法完全不同——它在保护模式下用 IDE PIO 直接端口 I/O 读磁盘，完全绕开 BIOS。但那需要自己实现磁盘驱动，对于一个教学项目来说，Real Mode + BIOS 中断是更务实的选择。

### 决策2：INT 13h 扩展读 vs 传统 CHS 读取

传统 INT 13h AH=0x02 使用 CHS（柱面-磁头-扇区）寻址，最大支持约 8GB。INT 13h AH=0x42 使用 LBA（Logical Block Addressing）寻址，通过 DAP 结构指定 64 位起始扇区号，理论上支持天文数字大小的磁盘。我们选择扩展读取，原因一是 QEMU 的虚拟硬盘天然使用 LBA 编址，二是 CHS 的柱面/磁头/扇区换算在无谓地增加出错概率——那些计算在虚拟化环境里毫无意义。

### 决策3：先读 ELF header 再做完整加载

本章只读 ELF header 的 4KB 到 0x10000，而不是一次性加载整个内核。这种两阶段设计有几个好处：第一，4KB 的读取不会因为数据量大而暴露出奇怪的 BIOS bug，更容易调试；第二，拿到 ELF header 后可以验证 magic、解析 Program Header 获取内核总大小，然后再做有针对性的完整加载；第三，完整的内核加载需要在保护模式下做（因为 Real Mode 有 1MB 内存限制），那留到 004B 再处理。

### 决策4：固定地址约定 vs 动态发现

0x5000（E820）、0x6400（FB Info）、0x7B00（DAP）、0x10000（Kernel ELF Header 临时区）——这些都是硬编码的固定地址。这在操作系统开发初期是合理的。动态发现（比如让 bootloader 和内核通过某种协议协商地址）在引导阶段会增加不必要的复杂度，而固定地址约定在调试时有一个巨大的优势：你知道每个结构在内存中的确切位置，可以直接用 GDB 或者 QEMU monitor 检查。

## 扩展方向

1. **完整 ELF 加载**（难度：中等）——当前我们只读了 ELF header 的 4KB，真正的 ELF 段加载（遍历 PT_LOAD、重定位、BSS 清零）留到了 tag 004B。到时候会在保护模式下循环读取完整内核，解析 Program Header 把每个段加载到正确的物理地址。

2. **内存映射过滤与打印**（难度：简单）——在 mini kernel 的 `main.cpp` 里加上串口输出（tag 005 之后），把 E820 返回的内存条目打印出来。QEMU 默认的 6~7 条记录很有趣，你可以观察到哪些是可用 RAM、哪些是保留区、哪些是 ACPI 可回收区。

3. **多磁盘启动支持**（难度：中等）——当前 DL 硬编码为 0x80（第一块硬盘），如果你想让 Cinux 从 USB 或者第二块硬盘启动，需要在 MBR 阶段把 BIOS 传入的 DL 值一路传递到 Stage2。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 15.3.1 — INT 15h, EAX=E820h: Query System Address Map。定义了 E820 调用规范、寄存器约定和返回值格式。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- OSDev Wiki — Detecting Memory (x86): E820 是推荐的内存探测方法。关键要点包括 ECX 应为 24（ACPI 3.0），EBX 续接值的正确使用，以及 DI 手动递增（不是 BIOS 帮你递增的）。
  https://wiki.osdev.org/Detecting_Memory_(x86)

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=0x42 扩展读取详解，包括 DAP 16 字节结构、DL=0x80 硬盘约定、buffer segment:offset 格式。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — ELF: ELF64 格式结构。ELF Header 64 字节、Program Header 56 字节、Bootloader 加载流程。
  https://wiki.osdev.org/ELF

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O 读取磁盘的参考实现，与 Cinux 的 BIOS INT 13h 方案形成对比。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c
