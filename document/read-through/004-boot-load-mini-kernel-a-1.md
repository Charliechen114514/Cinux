---
title: 004-boot-load-mini-kernel-a-1 · 内核加载 (A)
---

# 004 通读版 · 从磁盘加载小内核（上篇）—— E820 内存探测与 ELF header 预读

## 章节概览

前几章我们把启动链条搭好了：MBR -> Stage2 -> Protected Mode -> Long Mode。但到上一章为止，stage2 进入 Long Mode 后只是输出了一个 `L` 字符然后 halt，这就有点尴尬了——我们费这么大劲进入 64 位模式，结果什么都没干就停机了？本章的目标非常直接：在 Real Mode 阶段完成 E820 内存探测，并从磁盘读取小内核的 ELF header（4KB）到 0x10000，为后续完整的内核加载打下基础。听上去简单？这一步涉及到 E820 迭代调用、INT 13h 扩展读、Real Mode 段地址计算等一系列坑，调试时遇到的 DL 寄存器没设 0x80 导致的读取失败就是个典型的例子。

在整个 Cinux OS 的架构中，本章扮演着"连接 bootloader 和真正内核"的桥梁角色。上一章我们进入了 Long Mode，但没有任何实质性的数据准备工作。本章要做两件非常关键的事：第一，在 Real Mode 阶段完成 E820 内存探测（把内存布局保存到固定地址 0x5000）；第二，通过 INT 13h AH=0x42 扩展读取指令从磁盘 LBA=16 处读取小内核的 ELF header 到 0x10000。注意，本章**只读 ELF header 的前 4KB（8 个扇区）**，不是完整的内核——完整的 ELF 解析和内核加载留到后续 tag（004B）在 Protected Mode 下用 C 语言完成。这种分阶段验证的设计是为了逐步排查问题，如果 4KB 都读不进来，后面的事情就更不用说了。

### 关键设计决策一览

* **Real Mode 内完成 E820**：在切换到 Protected Mode 之前调用 INT 15h E820，结果保存到 0x5000
* **固定地址约定**：E820 buffer 在 0x5000，DAP 在 0x7B00，ELF header 临时加载到 0x10000
* **单次 INT 13h 读取 8 扇区（4KB）**：仅读取 ELF header，不需要分块循环
* **先验证后加载**：本章只验证磁盘读取链路通畅，真正的 ELF 解析和跳转留给 004B
* **Mini kernel 是 ELF64 格式**：链接地址 0x200000，本章只读到 0x10000 用于验证读取

---

## 架构图

下面是本章涉及的内存布局和调用关系图：

```
+---------------------------------------------------------------------+
|                        低内存布局（Real Mode 可用）                   |
+---------------------------------------------------------------------+
|  0x00005000  +----------------------------------------------+       |
|              |  E820 Buffer                                  |       |
|              |  [0x0000] count (u32)                         |       |
|              |  [0x0004] entries[32] (24B each)              |       |
|  0x00007B00  +----------------------------------------------+       |
|              |  DAP (Disk Address Packet, 16B)               |       |
|              |  [0x00] size = 0x10                           |       |
|              |  [0x02] count = 8 sectors (4KB)               |       |
|              |  [0x04] buffer = 0x1000:0x0000 (->0x10000)    |       |
|              |  [0x08] lba = 16 (low 32 bits)                |       |
|  0x00008000  +----------------------------------------------+       |
|              |  Stage2 Bootloader                            |       |
|  0x00010000  +----------------------------------------------+       |
|              |  Mini Kernel ELF Header (仅 4KB)               |       |
|              |  [0x00] 0x7F 0x45 0x4C 0x46  (\x7FELF)        |       |
|              |  [0x18] e_phoff -> program headers            |       |
|              |  ...                                          |       |
+---------------------------------------------------------------------+

磁盘布局：
  LBA 0:       MBR (1 sector)
  LBA 1-15:    Stage2 (15 sectors max)
  LBA 16+:     Mini Kernel ELF (完整 ELF 文件，本章只读前 4KB)
```

---

## 关键代码精讲

本章的新增逻辑主要位于 `boot/common/boot.S`，这个文件封装了两个关键函数：`query_memory_map` 执行 E820 内存探测，`load_kernel_from_disk` 从磁盘读取 ELF header。stage2.S 在适当的位置调用这些函数，在进入 Protected Mode 之前完成 Real Mode 的最后一项工作。

### 常量定义：与硬件约定的魔法数字

代码开头定义了一系列与 BIOS 调用相关的常量，这些数字背后都有其硬件层面的含义：

```asm
// E820 memory layout
.set E820_BUFFER_ADDR,          0x5000
.set E820_BUFFER_COUNT_ADDR,    0x5000
.set E820_BUFFER_ENTRIES_ADDR,  0x5004
.set E820_MAX_ENTRIES,          32
.set E820_ENTRY_SIZE,           24

// Pre-calculated segment/offset values
// Real mode: physical = segment << 4 + offset
.set E820_COUNT_SEG,            0x0500      // 0x0500 << 4 = 0x5000
.set E820_COUNT_OFF,            0x0000
.set E820_ENTRIES_SEG,          0x0500
.set E820_ENTRIES_OFF,          0x0004      // 0x5000 + 4 = 0x5004
```

这里有一个关键细节：Real Mode 的地址计算方式是 `物理地址 = 段寄存器 << 4 + 偏移`。所以要访问物理地址 0x5000，我们需要把段寄存器设置为 0x0500（0x0500 << 4 = 0x5000），偏移为 0。我在调试时踩过一个坑：直接把段寄存器设置为 0x5000，结果访问的是 0x50000，完全错位了。这就是为什么代码中定义了 `E820_COUNT_SEG = 0x0500` 而不是 `0x5000`。

接下来是磁盘读取相关的常量。和上一版的区别在于，我们不再一次性读取整个内核，而是只读 ELF header 的前 4KB：

```asm
// DAP (Disk Address Packet) structure offsets (16 bytes)
.set DAP_SIZE,                0
.set DAP_RESERVED,            1
.set DAP_COUNT,               2
.set DAP_BUFFER_OFFSET,       4
.set DAP_BUFFER_SEGMENT,      6
.set DAP_LBA,                 8

// DAP fixed location in low memory (MBR DAP area, safe to reuse after MBR)
.set DAP_PHYS_ADDR,           0x7B00
.set DAP_SEGMENT,             0x07B0      // 0x07B0 << 4 = 0x7B00
.set DAP_OFFSET,              0x0000

// Disk read constants
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set KERNEL_LOAD_SEGMENT,     0x1000      // 0x10000 = 0x1000:0x0000
.set KERNEL_LOAD_OFFSET,      0x0000

.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
```

注意这里的常量命名是 `KERNEL_LOAD_SEGMENT` 和 `KERNEL_LOAD_OFFSET`，而不是 `MINI_KERNEL_LOAD_SEG`。目标地址是 `0x1000:0x0000`，对应物理地址 0x10000。`MINI_KERNEL_LBA = 16` 表示小内核 ELF 从磁盘第 16 扇区开始，这与 `scripts/build_image.sh` 中的写入位置完全对应。

### E820 内存探测：询问 BIOS "内存长什么样"

`query_memory_map` 函数负责枚举系统内存布局，这是操作系统物理内存管理的基础：

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

函数入口先保存所有寄存器。`pushaw` 保存 8 个通用寄存器，`pushw %es` 和 `pushw %ds` 保存段寄存器。然后把 DS 设为 0x0500（指向物理 0x5000），把 count 清零；ES:DI 指向 entries 的起始位置 0x0500:0x0004（物理 0x5004）。EBX 初始化为 0 表示"从第一条记录开始"。

E820 是迭代式接口：你第一次调用时 EBX=0，BIOS 返回第一条记录和一个新的 EBX 续接值；你把新 EBX 原样传入再调一次，得到第二条记录和又一个 EBX；当 EBX 变回 0 时，说明迭代结束。

```asm
.e820_loop:
    movl E820_COUNT_OFF, %eax
    cmpl $E820_MAX_ENTRIES, %eax
    jae .e820_done

    movl $E820_SIGNATURE, %edx       // EDX = 'SMAP'
    movl $E820_CMD, %eax             // EAX = 0x0000E820
    movl $E820_ENTRY_SIZE, %ecx      // ECX = 24 (buffer size)

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

每次循环开始先检查是否超过 32 条上限，防止缓冲区溢出。然后设置 BIOS 调用参数：EDX='SMAP'（0x534D4150），EAX=0x0000E820，ECX=24。调用 `int $0x15` 后做三重验证：CF=0 表示调用成功，EAX 应返回 'SMAP' 签名（BIOS 回显），ECX 不应小于 20。成功后 DI 前进 24 字节指向下一个 entry 的位置——注意 DI 必须手动递增，BIOS 不会帮你递增。然后 count 加 1，检查 EBX 是否为 0——为 0 则迭代结束。循环退出时 CX 里是条目总数，作为返回值交给调用者。

这里有几个容易踩坑的地方。EAX 必须是完整的 `0x0000E820`，高 16 位必须为 0，有些 BIOS 对这个值非常敏感。ECX 我们传 24（请求 BIOS 返回完整条目），但 BIOS 可能只返回 20 字节，所以验证时用 `cmpl $20` 而不是 24。我们的内核只会使用 Type=1 的内存区域，其他区域都必须避开，但这个过滤是内核物理内存管理器的事情，Bootloader 阶段只负责忠实地记录 BIOS 报告的信息。

### INT 13h 扩展读取：单次调用读取 ELF header

`load_kernel_from_disk` 是本章最核心的函数，它负责从磁盘读取小内核的 ELF header。和我们最初设想的"分块循环读取整个内核"完全不同，这个函数只做一件简单的事情：一次 INT 13h 调用，读取 8 个扇区（4KB）的 ELF header 到 0x10000。

```asm
.global load_kernel_from_disk
.type load_kernel_from_disk, @function
load_kernel_from_disk:
    pushaw
    pushw %es
    pushw %ds

    // Set ES to DAP segment (for DAP access at fixed low memory)
    movw $DAP_SEGMENT, %ax
    movw %ax, %es

    // Step 1: Build DAP at fixed location 0x7B00
    movw $DAP_OFFSET, %di

    // Step 2: Fill DAP.size = 16
    movb $16, %es:(%di)

    // Step 3: Fill DAP.count = 8 (4KB = 8 sectors)
    movw $8, %es:2(%di)

    // Step 4: Fill DAP.buffer = 0x10000 (segment:offset format)
    movw $0x0000, %es:4(%di)             // buffer offset
    movw $0x1000, %es:6(%di)             // buffer segment

    // Step 5: Fill DAP.lba = MINI_KERNEL_LBA (16)
    movl $MINI_KERNEL_LBA, %eax
    movl %eax, %es:8(%di)                // LBA low 32 bits
    xorl %eax, %eax
    movl %eax, %es:12(%di)               // LBA high 32 bits
```

DAP（Disk Address Packet）的构造过程清晰明了。size 字段必须是 16（DAP 结构的固定大小），count 设为 8 表示读取 8 个扇区（8 x 512 = 4096 字节 = 4KB），这足以覆盖完整的 ELF64 header 和 Program Header 表。buffer 字段用 segment:offset 格式，0x1000:0x0000 对应物理地址 0x10000。LBA 字段是 64 位的，低 32 位设为 16（`MINI_KERNEL_LBA`），高 32 位清零。

接下来是调用 BIOS 并检查结果：

```asm
    // Step 6: Call INT 0x13 AH=0x42 extended read
    movw $DAP_SEGMENT, %dx
    movw %dx, %ds                        // DS = 0x07B0
    movw $DAP_OFFSET, %si
    movb $0x80, %dl                      // DL = 0x80 (first hard disk)
    movb $DISK_READ_CMD, %ah             // AH = 0x42
    int $0x13

    // Step 7: Check error
    jc disk_read_failed
    cmpb $0, %ah
    jne disk_read_failed

    // Step 8: Restore registers and return sector count (8)
    popw %ds
    popw %es
    popaw
    movw $8, %ax                         // return 8 sectors
    ret
```

这里有一个我调试时踩过的经典坑：**DL 寄存器必须设置为 0x80 才能访问第一块硬盘**。DL 的编码方式是 0x00-0x7F 对应软盘驱动器，0x80-0xFF 对应硬盘驱动器。如果 DL 设置错误，BIOS 会返回 AH=0x01（Invalid function），读取直接失败。更阴险的是，有时候 QEMU 的 BIOS 比较宽容，即使 DL 错误也可能不报错，但读到的是垃圾数据——这种情况下排查起来就更头疼了。

INT 13h AH=0x42 的成功判断标准是 CF=0 且 AH=0。CF（Carry Flag）是 BIOS 调用的通用错误标志，任何错误都会置位。成功后恢复所有寄存器，AX 返回读取的扇区数（8）。这个返回值后续可以用来验证读取是否完整。

失败路径会跳转到 `disk_read_failed` 标签，先恢复寄存器然后走 `panic` 流程：

```asm
disk_read_failed:
    popw %ds
    popw %es
    popaw
    movw $(msg_disk_read_failed), %si
    jmp panic
```

还有一个 `disk_too_large` 标签，虽然当前函数不会走到这里（因为我们只读 4KB），但预留了扩展空间——如果将来在 004B 阶段发现内核超过 448KB，可以触发这个错误。

### Stage2 调用时机：Real Mode 的最后一舞

stage2.S 中的调用顺序非常关键。两个新函数必须在进入 Protected Mode 之前调用：

```asm
    call vesa_save_framebuffer_info

    // ============================================================
    // 004_boot_load_mini_kernel_A: Real mode 内完成
    // ============================================================

    call query_memory_map           // [->0x5000] E820 memory map
    call load_kernel_from_disk      // [->0x10000] Load mini kernel ELF header

    cli                             // disable interrupts again

    // ============================================================
    // Switch to Protected Mode
    // ============================================================
```

为什么必须在 Real Mode 调用这两个函数？因为它们依赖 BIOS 中断。一旦进入 Protected Mode，我们就不能直接调用 BIOS 中断了——BIOS 代码是 16 位 Real Mode 代码，保护模式下无法执行。E820 需要调用 INT 15h，磁盘读取需要调用 INT 13h，这些都只能在 Real Mode 完成。

`cli` 指令紧跟在 `load_kernel_from_disk` 之后，因为接下来的 GDT 加载和模式切换是临界区，中断必须禁用。在函数入口处之前我们已经 `sti` 过了——E820 和 INT 13h 都需要中断处于启用状态才能正常工作。

### Mini Kernel：极简的 ELF64 验证目标

`kernel/mini/main.cpp` 是本章的目标"内核"，它极其简单——只有一个 `_start` 入口做死循环：

```cpp
/* Cinux Mini Kernel - Minimal ELF for Disk Load Testing
 * Absolute minimal ELF to test bootloader disk reading.
 * Just halts.
 */

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

这个 mini kernel 只做一件事：无限循环执行 CLI+HLT。CLI 禁用中断，HLT 让 CPU 停机等待中断。因为没有中断源，所以 CPU 会永远停在这里。这正好用于验证：如果 bootloader 成功读取并跳转到这里，QEMU 会正常停机而不是 triple fault 崩溃。但本章我们不会跳转到它——我们只验证 ELF header 能正确读入内存。

Mini kernel 的链接脚本 `kernel/mini/linker.ld` 定义了地址布局：

```ld
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    /* Kernel loads at physical 2MB */
    . = 0x200000;

    .text : {
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    .bss : {
        *(.bss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
```

这里有一个重要的设计点需要理解：linker.ld 指定内核的 VMA（Virtual Memory Address）是 0x200000（2MB 边界），但 bootloader 在本章只把 ELF header 读到了 0x10000。**这是刻意为之的**——本章只验证"磁盘读取链路通畅"，真正的 ELF 解析和段重定位（把 .text 段搬运到 0x200000 等最终地址）留到 004B 在 Protected Mode 下用 C 代码完成。这种分阶段验证的好处是可以逐步排查问题：如果 4KB 都读不进来，后面的 ELF 解析就更不用说了。

另外注意 `kernel/mini/arch/x86_64/boot.S` 也存在于 diff 中，它定义了完整的 `_start` 入口（包括栈设置、BSS 清零、调用 `mini_kernel_main`），但由于 linker.ld 中 `ENTRY(_start)` 的设置，链接器会选择其中一个作为入口。在当前阶段 main.cpp 中的 `_start` 和 boot.S 中的 `_start` 不会同时参与链接——这取决于 CMakeLists.txt 中把哪些文件编译进了 mini_kernel 目标。当前 `kernel/mini/CMakeLists.txt` 只包含了 `main.cpp`。

### 构建系统集成

整个构建系统的变化涉及三个层面。

首先，`boot/CMakeLists.txt` 把 `boot.S` 编译为 boot_common 对象库：

```cmake
add_library(boot_common OBJECT
    common/serial.S
    common/boot.S      # 新增
)
```

其次，`kernel/mini/CMakeLists.txt` 定义了 mini_kernel 的编译规则：

```cmake
add_executable(mini_kernel
    main.cpp
)

target_compile_options(mini_kernel PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)

target_link_options(mini_kernel PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -no-pie
)
```

`-mcmodel=large` 这个选项告诉 GCC 生成的代码可以加载到任意 64 位地址，而不是假设代码在某个特定范围内。我们的 mini kernel 最终会链接到 0x200000，需要这个选项保证代码能正确运行。`-nostdlib` 和 `-no-pie` 确保生成裸机 ELF，不依赖任何系统库和位置无关代码机制。

第三，`scripts/build_image.sh` 负责把 mini kernel ELF 写入磁盘镜像。关键参数是 `MINI_KERNEL_LBA=16`，这与 `boot.S` 中定义的常量完全一致。脚本还会验证 ELF header 的 magic number（0x7F 'E' 'L' 'F'），确保写入的是有效的 ELF 文件。构建脚本还引入了 `scripts/log/logging.sh` 提供的日志工具函数（`log_info`、`log_error`、`log_success` 等），替代了之前的 `echo` 输出。

最后，`cmake/qemu.cmake` 更新了镜像构建命令，新增了 `${MINI_ELF}` 参数：

```cmake
set(MINI_ELF "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel")
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_ELF}
        ${CINUX_IMAGE_PATH}
    DEPENDS mbr stage2 mini_kernel
    ...
)
```

这样 CMake 在构建磁盘镜像时会确保 mini_kernel 已经编译完成，并把它的路径传给 `build_image.sh`。

---

## 设计决策深度分析

### 决策 1：只读 ELF header vs 一次性读取整个内核

**问题**：从磁盘读取内核有两种策略——要么一次读完整 ELF 文件，要么先读 ELF header 再按需加载。

**本项目的做法**：我们在 Real Mode 下只读 ELF header（4KB = 8 扇区），把完整的 ELF 解析和加载推迟到 Protected Mode。这是因为 Real Mode 下做复杂的 ELF 解析非常痛苦——段寄存器只有 16 位，无法直接处理 64 位地址，循环读取需要保存大量寄存器状态。而 Protected Mode 下有 32 位寄存器，可以用 C 语言编写 ELF loader，代码可读性和可靠性都大幅提升。

**备选方案**：另一种做法是直接用 `objcopy -O binary` 把 ELF 转成 flat binary，bootloader 不需要解析 ELF header，直接从固定 LBA 读到固定地址就行。Linux 0.01 和 xv6 都是这么做的。

**为什么不选备选方案**：Flat binary 的问题是缺乏灵活性。如果内核大小变化，bootloader 里的扇区计数常量也必须跟着改。更重要的是，ELF 格式提供了 Program Header 信息，让我们可以按段加载（只加载 PT_LOAD 段），而不是盲目地把整个文件读到内存。后续 tag（004B）会实现完整的 ELF 解析器，利用 Program Header 信息完成精确加载。

### 决策 2：在 Real Mode 完成 E820 vs 延迟到 Protected Mode

**问题**：E820 内存探测必须在 Real Mode 调用 BIOS INT 15h，但结果可以等到进入保护模式后再解析。

**本项目的做法**：在 Real Mode 的最后阶段调用 E820，结果直接保存到固定地址 0x5000。Bootloader 不做任何解析，只是"搬运工"角色，把 BIOS 返回的原始数据完整保存。后续内核代码会解析这个 buffer，构建自己的物理内存管理器。我们不过滤 type，不做压缩——内核侧有自己的需求，bootloader 不应该越俎代庖。

### 决策 3：加载地址 0x10000 vs 0x200000

**问题**：Mini kernel 的链接地址是 0x200000，那我们为什么把 ELF header 临时加载到 0x10000？

**原因**：0x10000 是一个经典的"临时缓冲区"位置，位于 Real Mode 可寻址范围内（低 1MB），不与任何已使用区域冲突（E820 buffer 在 0x5000，DAP 在 0x7B00，Stage2 在 0x8000）。更重要的是，我们只是读 ELF header 来验证磁盘读取链路，不是把内核加载到最终执行地址。后续在 Protected Mode 下，ELF loader 会根据 Program Header 中的 `p_paddr` 字段把各个段搬运到正确的物理地址（0x200000）。

---

## 常见变体与扩展方向

1. **添加更多调试输出**：在 E820 循环中用 debugcon 输出每个条目的 type 字符（'1'=可用, '2'=保留），这样可以在 QEMU 启动时直接看到内存布局概况，不用每次都 attach GDB。

2. **验证 ELF header 完整性**：在读取 ELF header 后，检查 magic number（0x7F 'E' 'L' 'F'）和 e_machine（应该是 0x3E = x86-64），如果不对就 panic。这样可以在 004B 之前就发现 ELF 格式问题。

3. **支持 CRC32 校验**：在 mini kernel 编译时生成 checksum，bootloader 读取后验证数据完整性，防止磁盘写入错误导致内核损坏。

---

## 验证与调试

### 预期输出

如果一切正常，QEMU 的 debugcon 输出应该类似：

```
Stage2 OK
Mode info OK, switching...
PLJ
```

其中 `P` 表示进入 Protected Mode，`L` 表示进入 Long Mode，`J` 表示准备跳转到小内核。如果只看到 `PL` 然后崩溃，说明 E820 或磁盘读取可能有问题。

### 常见问题排查

**问题：看到 "DISK: Failed to read kernel!"**

首先检查 DL 是否设为 0x80。用 GDB 断在 `int $0x13` 前后，确认 DL 和 DAP 结构都正确。DAP 应该是：`10 00 08 00 00 00 00 10 10 00 00 00 00 00 00 00`。

**问题：E820 返回 0 条记录**

检查 EAX 是否为完整的 `0x0000E820`（不是 `0xE820`），EDX 是否为 `'SMAP'`（0x534D4150），ECX 是否为 24。

**问题：0x10000 处看不到 ELF magic**

检查 `build_image.sh` 是否正确执行，mini kernel ELF 是否成功写入磁盘镜像。可以用 `hexdump -C cinux.img | grep "00002000"` 查看 LBA 16 的位置是否有 `7f 45 4c 46`。

---

## 参考资料

### Intel/AMD 手册

* Intel SDM Vol. 3A, Section 15.3.1 — INT 15h, EAX=E820h: Query System Address Map
* Intel SDM Vol. 2, Chapter 3 — Instruction Set Reference (INT, CLI, HLT)
* BIOS Boot Specification (BBS) — INT 13h AH=42h Extended Read

### OSDev Wiki

* [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) — E820 和其他内存探测方法
* [INT 13h](https://wiki.osdev.org/INT_13h) — 磁盘 I/O 的完整参考
* [ELF](https://wiki.osdev.org/ELF) — ELF 格式在 OS 开发中的应用
* [Bootloader Tutorial](https://wiki.osdev.org/Bootloader_Tutorial) — 完整 bootloader 示例

### 其他资源

* [ELF-64 Object File Format](https://uclibc.org/docs/elf-64-gen.pdf) — ELF64 格式官方规范
* [Writing a Simple Operating System from Scratch](https://www.cs.bham.ac.uk/~exr/lectures/opsys/10_11/lectures/os-dev.pdf) — Nick Blundell 的 OS 开发讲义

---

到这里本章的内容就告一段落了。如果你跟着走下来，现在应该对 Real Mode 的 BIOS 调用、E820 内存探测、INT 13h 磁盘读取有了比较深入的理解。这一步踩过很多坑——段寄存器计算错误、DL 忘记设 0x80、E820 调用参数写错——但每一步都是宝贵的学习经验。

下一章（004B）我们将做真正有趣的事情：在 Protected Mode 下解析刚才读取的 ELF header，根据 Program Header 信息把内核代码段搬运到正确的物理地址（0x200000），设置好参数，然后跳转到内核的入口点。到那个时候，mini kernel 的 CLI+HLT 循环才能真正生效。
