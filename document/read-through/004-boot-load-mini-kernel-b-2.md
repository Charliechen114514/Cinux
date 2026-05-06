# 004 通读版 · 完整加载小内核（中）—— 分块磁盘读取的完整逻辑

> 前置：[004B（上）BootInfo 结构与分块读取开始](004-boot-load-mini-kernel-b-1.md)
> 接续：[004B（下）构建系统与设计决策](004-boot-load-mini-kernel-b-3.md)

---

另外注意到 `movb $0, %es:1(%di)` 显式清零 DAP 的 reserved 字段。004A 没有这行，因为当时只构建一次 DAP、内存碰巧是干净的。现在我们循环复用同一个 DAP 位置，必须确保每次迭代都从干净状态开始。

接下来是 buffer 地址的计算——这是整个函数里最巧妙也最容易出错的部分：

```asm
    movw %bx, %ax                        // AX = sector offset
    shlw $5, %ax                         // AX *= 32
    addw $MINI_KERNEL_LOAD_SEG, %ax      // AX = target segment
    movw %ax, %es:6(%di)                 // buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax      // AX = 0 (offset always 0)
    movw %ax, %es:4(%di)                 // buffer offset
```

这里需要把"第 BX 个扇区应该写到哪个物理地址"转换成 Real Mode 的 segment:offset 格式。每个扇区 512 字节，BX 个扇区就是 BX * 512 字节偏移。但 Real Mode 的地址不是简单的线性计算——segment 寄存器左移 4 位加上 offset 才是物理地址。我们的策略是让 offset 始终为 0，把偏移量全部编码到 segment 里。

具体推导是这样的：目标物理地址 = 0x20000 + BX * 512 = 0x20000 + (BX << 9)。Segment = 物理地址 >> 4 = (0x20000 + (BX << 9)) >> 4 = 0x2000 + (BX << 5) = 0x2000 + BX * 32。所以代码里先把 BX 左移 5 位（相当于乘 32），再加上基础段地址 0x2000，就得到了目标 segment 值。offset 固定为 0。这样每一轮循环的 buffer 起始地址都正确地前移了 BP * 512 字节。

然后用同样的思路填写 LBA：

```asm
    movw $MINI_KERNEL_LBA, %ax           // AX = base LBA (16)
    addw %bx, %ax                        // AX = current LBA
    movw %ax, %es:8(%di)                 // LBA low 16 bits
    xorw %ax, %ax                        // AX = 0
    movw %ax, %es:10(%di)                // LBA bits 16-31
    movw %ax, %es:12(%di)                // LBA bits 32-47
    movw %ax, %es:14(%di)                // LBA bits 48-63 (must be zero)
```

当前 LBA = 基础 LBA（16）+ 已读扇区数（BX）。LBA 是 64 位字段，虽然我们的 LBA 值很小（最大 848），但高 48 位必须显式清零——不能假设内存里没有残留数据。这也是 004A 里提到过的坑：DAP 的高位 LBA 如果有垃圾值，BIOS 会尝试读取一个巨大 LBA，直接返回错误。

接下来是 BIOS 调用和结果处理：

```asm
    // CRITICAL: Save BX and BP across BIOS call (BIOS clobbers both)
    pushw %bx                            // save sector counter
    pushw %bp                            // save sector count

    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %ds                        // DS = DAP segment (REQUIRED!)
    movw $DAP_OFFSET, %si                // SI = 0x0000
    movb $0x80, %dl                      // DL = 0x80 (first hard disk)
    movb $DISK_READ_CMD, %ah             // AH=0x42

    int $0x13                            // execute disk read
```

这里有两个极其重要的细节，每一个都够你调试一整个下午。

第一个细节：BX 和 BP 必须在 BIOS 调用前压栈保存。INT 13h 的实现不保证保留 BX 和 BP——很多 BIOS 实现会破坏这两个寄存器（BX 被用作内部循环计数器，BP 被用作临时存储）。如果我们不保存，调用返回后 BX 和 BP 里的值已经不知道变成什么了，循环计数直接乱套。

第二个细节：INT 13h AH=42h 要求 DS:SI 指向 DAP，不是 ES:SI。这跟大多数 BIOS 调用的 ES:DI 约定不同，是 INT 13h 扩展读取的一个特殊之处。代码里 `movw %dx, %ds` 把 DS 设为 DAP 段地址，然后 SI 设为 DAP 偏移。DL 必须设为 0x80（第一块硬盘）。

调用后的错误处理有两个路径：

```asm
    jc .disk_error_restore_bp            // CF=1 means failure
    cmpb $0, %ah                         // AH should be 0
    jne .disk_error_restore_bp           // AH!=0 means error

    // Success path
    popw %bp                             // restore sector count
    popw %bx                             // restore sector counter

    addw %bp, %bx                        // BX += sectors read
    jmp .read_loop                       // Continue reading

.disk_error_restore_bp:
    popw %bp                             // restore sector count
.disk_error_restore:
    popw %bx                             // restore sector counter
    jmp disk_read_failed                 // handle error
```

成功路径：弹出 BP 和 BX，然后把 BP（本次读取的扇区数）加到 BX（已读总数）上，继续循环。错误路径：也必须先弹出 BP 和 BX——栈上有两个 word（先是 BX 后是 BP），如果直接跳到 `disk_read_failed` 而不恢复栈指针，后续的 DS/ES 恢复会从错误的位置弹出，寄存器全部乱掉。注意错误路径分成了两步：先弹出 BP（因为栈顶是 BP），再弹出 BX，然后才跳转。这样保证了栈平衡。

循环正常退出后的收尾：

```asm
.read_done:
    popw %ds                             // restore DS
    popw %es                             // restore ES
    popa                                 // restore general registers

    movb $'O', %al                       // Load 'O'
    outb %al, $0xe9                      // Output 'O' to debugcon

    movw $MINI_KERNEL_SECTORS, %ax       // return sectors read
    ret
```

恢复所有寄存器后，输出字符 'O' 到 debugcon 端口 0xE9——这是我们的调试约定，'O' 代表"磁盘读取 OK"。然后 AX 返回 832（读取的总扇区数）。debugcon 输出在 QEMU 里配合 `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9` 参数使用，你可以直接查看日志文件来确认启动到了哪一步。

错误处理函数也加上了 debugcon 输出，方便区分是哪种失败：

```asm
disk_read_failed:
    movb $'F', %al                       // 'F' for disk read Failed
    outb %al, $0xe9
    popw %ds
    popw %es
    popa
    movw $(msg_disk_read_failed), %si
    jmp panic

disk_too_large:
    movb $'T', %al                       // 'T' for Too large
    outb %al, $0xe9
    popw %ds
    popw %es
    popa
    movw $(msg_disk_too_large), %si
    jmp panic
```

'F' 代表磁盘读取失败，'T' 代表内核太大。这三个 debugcon 字符（'O'/'F'/'T'）配合 Stage2 中的 'P'（Protected Mode）和 'L'（Long Mode），构成了一条完整的启动状态链：如果你看到 debugcon 输出 `PLO`，说明磁盘读取成功、进入了 Protected Mode、然后进入了 Long Mode。如果停在 `F` 之前，说明磁盘读取就失败了。

### Protected Mode 无操作（boot/stage2.S — pm_entry）

现在回头看 Stage2 里 `load_kernel_from_disk` 的调用位置，以及 Protected Mode 入口做了什么：

```asm
    call query_memory_map           // [->0x5000] E820 memory map

    call load_kernel_from_disk      // [->0x20000] Load mini kernel (416KB, 832 sectors)

    cli                             // disable interrupts again
```

调用位置和 004A 完全相同——在 VESA 之后、保护模式切换之前。注释更新了参数（832 扇区、目标地址 0x20000）。`cli` 紧跟其后，因为接下来的 GDT 加载和模式切换是临界区。

然后是 Protected Mode 入口：

```asm
.code32
pm_entry:
    movw $0x10, %ax               // Data selector
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl $0x90000, %esp           // New stack in protected mode

    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // ============================================================
    // 004_boot_load_mini_kernel_B: No operation needed
    // ============================================================
    // Mini kernel bin was already loaded to 0x20000 in real mode.
    // Entry point is fixed at 0xFFFFFFFF80020000 (high-half kernel).
    // Nothing to do in protected mode, proceed to long mode.

    call setup_page_tables
    call enter_long_mode
```

你会发现 Protected Mode 入口和 004A 相比只多了一块注释——"No operation needed"。所有数据搬运工作（E820 探测 + 832 扇区磁盘读取）在 Real Mode 下已经全部完成。Protected Mode 这里做的事情和之前一模一样：设置段寄存器、初始化栈、输出 'P' 到 debugcon、建页表、进入 Long Mode。没有新增任何一条指令。

这个"无操作"设计不是偷懒，而是架构层面的刻意选择。在最初的计划里，004B 应该在 Protected Mode 下实现 ELF 解析——读取 Program Header、遍历 PT_LOAD 段、做重定位和BSS 清零。但我们最终放弃了这个方案，转而用 flat binary（扁平二进制）替代 ELF。原因很简单：Real Mode 的 16 位环境做不了 64 位算术，连解析 ELF header 里的 64 位地址字段都非常痛苦。而 flat binary 不需要任何解析——读进来是什么地址，执行的时候就是什么地址，前提是链接地址和加载地址一致。

代价也是明显的：内核必须链接在固定的物理地址（0x20000），不能做地址随机化（ASLR），也不能利用 ELF 的段对齐特性。但对于一个教学内核来说，这些限制完全可以接受。

### 链接脚本（kernel/mini/linker.ld）

链接脚本从 004A 的 0x200000 改到了 0x20000：

```
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

SECTIONS
{
    . = 0x20000;

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
        __bss_start = .;
        *(.bss)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
