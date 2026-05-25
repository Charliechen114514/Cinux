---
title: 004-boot-load-mini-kernel-b-2 · 内核加载 (B)
---

# Bootloader 开发日记 004B（中）：从 4KB 到 416KB——分块磁盘读取与栈冲突惊魂记

> 标签：x86, INT 13h, 分块读取, 段地址计算, 栈冲突, Real Mode
> 前置：[004B（上）BootInfo 的诞生](004-boot-load-mini-kernel-b-1.md)

## 写在前面

上一篇我们定义了 BootInfo 结构体，确立了 flat binary 加载策略。现在进入 004B 最硬核的部分：把磁盘读取从 8 扇区（4KB）扩展到 832 扇区（416KB），用循环分块读取绕过 BIOS 的 127 扇区限制。这一步涉及大量的段地址动态计算、寄存器保护、DAP 结构重建——每一个环节都有坑。

更刺激的是，在这个过程中我们会遇到一个让我血压拉满的栈冲突 bug：最初把内核加载到 0x10000，结果磁盘写入覆盖了 Real Mode 栈上保存的返回地址，`ret` 跳飞到不知名的地方。这个 bug 花了我大半天才发现——症状是 `load_kernel_from_disk` 返回后系统直接 triple fault，用 GDB 看返回地址已经被磁盘数据覆盖成了乱码。

## BIOS 127 扇区限制和分块策略

004A 读了 8 个扇区（4KB），一次 INT 13h 调用就搞定了。现在我们要读 832 个扇区（416KB），一次读不完——OSDev Wiki 的 [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) 页面明确指出，INT 13h AH=42h 扩展读取的单次上限是 127 个扇区（某些 BIOS 实现甚至更低）。这不是 INT 13h 规范本身的硬限制，而是 BIOS 实现中 DMA 传输缓冲区的约束——BIOS 内部需要把 DMA 传输限制在 64KB 边界内，127 个扇区 = 65024 字节，恰好不超过这个限制。

所以我们必须分块读取。832 个扇区按每次 127 个算，需要 7 次循环：6 次读满 127 个扇区（762 个扇区），最后一次读 70 个扇区（832 - 762 = 70）。循环变量 BX 追踪已经读取的扇区数，每次计算 min(127, 剩余扇区数)，然后动态构建 DAP（Disk Address Packet），更新 buffer 地址和 LBA 偏移，调一次 INT 13h，然后 BX 加上本次读取量，继续下一轮。

先看常量定义，这里每一条背后都有故事：

```asm
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set MINI_KERNEL_SECTORS,     832         // Total sectors (416KB)

.set MINI_KERNEL_LOAD_PHYS,   0x20000     // Physical address where kernel is loaded
.set MINI_KERNEL_LOAD_SEG,    0x2000      // Segment: 0x2000 (0x2000 << 4 = 0x20000)
.set MINI_KERNEL_LOAD_OFF,    0x0000      // Offset

.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
.set DISK_MAX_SECTORS_PER_CALL, 127       // Max sectors per BIOS call
```

和 004A 相比最大的变化是加载地址从 0x10000 变成了 0x20000。这个改动背后的故事就是那个让我血压拉满的栈冲突 bug——我们待会儿单独说。扇区数从 8 变成了 832，新增了 `DISK_MAX_SECTORS_PER_CALL = 127`。LBA 还是 16——小内核在磁盘镜像上从第 16 个扇区开始，紧接在 Stage2（扇区 1~15）之后。

## 栈冲突惊魂记：0x10000 到 0x20000

在讲循环逻辑之前，我们得先说清楚为什么加载地址是 0x20000 而不是 0x10000。这不是一开始就定好的——最初的设计就是 0x10000，和 xv6 的 `bootmain.c` 以及 Linux 0.01 的 boot sector 一样。0x10000 是经典的"临时加载区"，几乎所有 x86 bootloader 教程都把数据加载到这里。

但问题是，我们忘了算 Real Mode 栈的位置。

Stage2 在启动时把栈设在 `SS=0x0900, SP=0xFFFE`，这意味着 Real Mode 栈占据的物理地址范围是 0x9000（SS<<4 = 0x9000）到 0x19000（0x9000 + 0xFFFE = 0x19000）。栈是向下生长的，所以从 0x19000 往下压栈，push 的数据写在高地址，新数据往低地址走。当小内核从 0x10000 开始加载时，832 个扇区的数据会从 0x10000 写到 0x10000 + 832*512 = 0x78000。看起来 0x78000 远大于栈顶 0x19000，应该没问题？

问题在于 `load_kernel_from_disk` 是一个函数调用。调用前，调用者把返回地址 push 到栈上，栈指针从 0xFFFE 往下移了 2 个字节变成 0xFFFC（物理 0x18FFC）。函数入口又 `pusha` 保存了 8 个通用寄存器（16 字节），再加 `pushw %es` 和 `pushw %ds`（4 字节），栈上现在有 22 字节的数据，物理范围大约在 0x18FEA ~ 0x18FFF。这些数据中最重要的就是返回地址——`call load_kernel_from_disk` 时 CPU 自动 push 的 IP 值，函数末尾 `ret` 要靠它跳回调用者。

现在 INT 13h 开始往 0x10000 写数据了。写啊写啊，写到偏移 0x9000 的时候（也就是第 36 个扇区左右），数据刚好覆盖了 0x19000 附近的栈区域。返回地址被磁盘数据覆盖了。等 `load_kernel_from_disk` 执行到 `ret` 的时候，pop 出来的 IP 是磁盘上某个随机的字节，CPU 跳到一个莫名其妙的地址，大概率触发 triple fault 然后重启。

这个 bug 的症状非常诡异：在 QEMU 里它不一定每次都触发（取决于栈的精确位置和磁盘数据恰好覆盖了哪里），在 Bochs 里更容易触发。用 GDB 单步跟踪时你会发现 `ret` 之后 RIP 变成了一个完全意料之外的值，然后 CPU 就 triple fault 了。说实话，这个问题真的坑了我半天——直到用 QEMU monitor 手动 `xp /4bx 0x18FFC` 检查才发现栈顶的数据已经被覆盖成了磁盘上的 ELF magic 字节，才意识到是加载地址和栈冲突了。

解决方案是把加载地址提高到 0x20000。关键不等式是：`MINI_KERNEL_LOAD_PHYS > (SS << 4 + SP)`，即 `0x20000 > 0x19000`。安全间隙 = 0x20000 - 0x19000 = 0x7000 = 28KB。这个间隙足够覆盖任何栈操作可能触及的范围——即使函数调用嵌套了好几层，28KB 的空间也绑绰有余。

接下来问题来了：Protected Mode 的栈在 0x90000（Stage2 里 `movl $0x90000, %esp` 设置的），小内核最大 416KB，加载到 0x20000~0x88000，离 Protected Mode 栈只有 0x90000 - 0x88000 = 0x8000 = 32KB 的间隙。这个间隙虽然够用，但我们要在 build_image.sh 里加上大小限制检查——如果小内核超过 416KB，构建直接失败，而不是等到运行时才发现数据写到了栈区。

## 分块循环读取的完整逻辑

现在我们来看 `load_kernel_from_disk` 的循环本体。先说入口：

```asm
load_kernel_from_disk:
    pusha                                // save all 16-bit registers
    pushw %es                            // save ES
    pushw %ds                            // save DS

    xorw %bx, %bx                        // BX = 0 (no sectors read yet)
    xorw %si, %si                        // SI = 0 (stays 0, used for DAP offset)

    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %es                        // ES points to DAP area
```

函数入口保存所有寄存器。BX 是我们的核心循环变量——"已经读了多少扇区"，初始化为 0。SI 固定为 0，后面每次循环都用它指向 DAP 的偏移地址。ES 指向 DAP 所在的段 0x07B0（物理 0x7B00）。

然后进入循环：

```asm
.read_loop:
    cmpw $MINI_KERNEL_SECTORS, %bx       // Compare with total
    jae .read_done                       // Done if BX >= 832

    movw $MINI_KERNEL_SECTORS, %ax       // AX = 832 (total)
    subw %bx, %ax                        // AX = remaining sectors
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax // Compare with 127
    jbe .read_count_ok                   // Use AX if <= 127
    movw $DISK_MAX_SECTORS_PER_CALL, %ax // Cap at 127

.read_count_ok:
    movw %ax, %bp                        // BP = sectors to read this round
```

每次循环开始先检查是否读完——BX >= 832 则退出。然后计算本次读取量：`min(127, remaining)`。前几次循环 BX 还小，remaining 远大于 127，所以每次读 127 个扇区。最后几轮 remaining 小于 127，按实际剩余量读取。BP 暂存本次读取量。

接下来构建 DAP——每次循环都要重新填写，因为 buffer 地址和 LBA 都在变：

```asm
    // CRITICAL: Re-set ES before building DAP (BIOS may clobber it)
    movw $DAP_SEGMENT, %dx
    movw %dx, %es                        // ES = 0x07B0 (DAP segment)

    movw $DAP_OFFSET, %di                // DI = 0x0000

    movb $16, %es:(%di)                  // DAP size = 16
    movb $0, %es:1(%di)                  // DAP reserved = 0
    movw %bp, %es:2(%di)                 // DAP count = BP (sectors this round)
```

这里有一个很容易翻车的细节——每次循环都必须重新设置 ES 为 DAP 段地址。为什么？因为 INT 13h 调用可能破坏 ES 的值。BIOS 内部实现不保证保留调用者的段寄存器，如果你偷懒只设置一次 ES 然后循环复用，第二轮开始 ES 已经不是 0x07B0 了，你往 `es:(%di)` 写的 DAP 字段全部写到了错误的位置。这个 bug 在 QEMU 里可能不触发（SeaBIOS 实现比较规矩），但在真实硬件或者 Bochs 里几乎必现。

另外一个细节是 `movb $0, %es:1(%di)` 显式清零 reserved 字段。004A 没有这行，因为当时只构建一次 DAP、内存碰巧是干净的。现在我们循环复用同一个 DAP 位置，必须确保每次迭代都从干净状态开始——不然上一轮的残留数据可能让某些 BIOS 实现返回错误。

## buffer 地址的动态计算

真正的坑在后面——buffer 地址的计算。每一轮循环的目标物理地址都在变：第 0 轮写 0x20000，第 1 轮写 0x20000 + 127*512 = 0x2FE00，第 2 轮写 0x20000 + 254*512 = 0x3FC00，以此类推。但 Real Mode 用的是 segment:offset 格式，不是线性地址。我们需要把"第 BX 个扇区对应的物理地址"转换成段地址格式：

```asm
    // Target physical address = 0x20000 + BX * 512 = 0x20000 + (BX << 9)
    // Segment = physical >> 4 = (0x20000 + (BX << 9)) >> 4
    //         = 0x2000 + (BX << 5) = 0x2000 + BX * 32
    movw %bx, %ax                        // AX = sector offset
    shlw $5, %ax                         // AX *= 32
    addw $MINI_KERNEL_LOAD_SEG, %ax      // AX = 0x2000 + BX*32 = target segment
    movw %ax, %es:6(%di)                 // buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax      // AX = 0 (offset always 0)
    movw %ax, %es:4(%di)                 // buffer offset
```

这段代码的核心是那个位移计算。Real Mode 的地址公式是 `物理地址 = segment << 4 + offset`。我们的策略是让 offset 始终为 0，把所有偏移编码到 segment 里。推导过程是这样的：目标物理地址 = 0x20000 + BX * 512。除以 16（右移 4 位）得到 segment = 0x2000 + BX * 512 / 16 = 0x2000 + BX * 32。所以代码里 BX 左移 5 位（相当于乘 32）再加上基础段 0x2000，一步到位。

你会发现这个计算有一个隐含的假设：BX * 32 的结果不超过 16 位。BX 最大值是 832（832 * 32 = 26624 = 0x6800），确实在 16 位范围内。但如果将来内核大小超过这个范围，这个计算就会溢出——不过 build_image.sh 的大小限制检查会在编译期就拦住。

然后是 LBA 的填写：

```asm
    movw $MINI_KERNEL_LBA, %ax           // AX = 16 (base LBA)
    addw %bx, %ax                        // AX = 16 + BX = current LBA
    movw %ax, %es:8(%di)                 // LBA low 16 bits
    xorw %ax, %ax                        // AX = 0
    movw %ax, %es:10(%di)                // LBA bits 16-31
    movw %ax, %es:12(%di)                // LBA bits 32-47
    movw %ax, %es:14(%di)                // LBA bits 48-63 (must be zero!)
```

当前 LBA = 基础 LBA（16）+ 已读扇区数（BX）。LBA 是 DAP 里的 64 位字段（8 字节），虽然我们的 LBA 值很小（最大 848），但高 48 位必须显式清零。这是 004A 里就踩过的坑——DAP 的高位 LBA 如果有残留的垃圾值，BIOS 会尝试读取一个巨大 LBA，直接返回错误。每轮循环我们只更新低 16 位，高 48 位全部写零，确保万无一失。

## BX/BP 寄存器保护和 BIOS 调用

接下来是整个函数里最需要注意的部分——BIOS 调用前后的寄存器保护：

```asm
    // CRITICAL: Save BX and BP across BIOS call
    pushw %bx                            // save sector counter
    pushw %bp                            // save sector count

    // INT 13h AH=42h requires DS:SI to point to DAP, NOT ES:SI!
    movw $DAP_SEGMENT, %dx
    movw %dx, %ds                        // DS = 0x07B0
    movw $DAP_OFFSET, %si                // SI = 0x0000
    movb $0x80, %dl                      // DL = 0x80 (first hard disk)
    movb $DISK_READ_CMD, %ah             // AH = 0x42

    int $0x13
```

这里有三件事情，每一件都够你调试一个下午。

第一，BX 和 BP 必须在 BIOS 调用前压栈保存。INT 13h 的实现不保证保留这两个寄存器——很多 BIOS 实现会把 BX 用作内部循环计数器，把 BP 用作临时存储。如果我们不保存，调用返回后 BX 和 BP 里的值已经不知道变成什么了，循环计数直接乱套，要么死循环要么少读一段数据。说实话这个坑在 QEMU 里不一定触发（SeaBIOS 可能恰好没碰 BX 和 BP），但在真实硬件上几乎必现。

第二，INT 13h AH=42h 要求 DS:SI 指向 DAP，不是 ES:SI。这跟大多数 BIOS 调用的 ES:DI 约定不同——E820 用 ES:DI，INT 10h（VESA）也用 ES:DI，但 INT 13h 扩展读取偏偏用 DS:SI。搞反了 BIOS 不会报错但会从错误的位置读取 DAP，然后要么读到垃圾数据导致读取失败，要么读到恰好看起来合法的数据然后读取到错误的地址。代码里 `movw %dx, %ds` 把 DS 设为 0x07B0，然后 SI 设为 0。

第三，DL 必须设为 0x80（第一块硬盘）。如果你忘了设（或者 DL 里恰好还是上一次调用的残留值），BIOS 会尝试从不存在的驱动器读取，返回 AH=0x01（Invalid Function）。004A 里详细讲过这个坑——我们曾在 E820 调用后直接调 INT 13h，DL 里还是 E820 留下的残留值，结果 BIOS 返回错误但数据碰巧是对的，折腾了半天才发现问题。

调用后的处理分两条路径：

```asm
    jc .disk_error_restore_bp            // CF=1 means failure
    cmpb $0, %ah                         // AH should be 0
    jne .disk_error_restore_bp           // AH!=0 means error

    // Success path: restore BP and BX
    popw %bp                             // restore sector count
    popw %bx                             // restore sector counter
    addw %bp, %bx                        // BX += sectors read
    jmp .read_loop                       // Continue reading

.disk_error_restore_bp:
    popw %bp                             // restore (stack: BP then BX)
.disk_error_restore:
    popw %bx
    jmp disk_read_failed                 // handle error
```

成功时弹出 BP 和 BX，把 BP（本次读取量）加到 BX 上，继续循环。错误时也必须先弹出 BP 和 BX——栈上有两个 word（先压 BX 后压 BP，所以栈顶是 BP），如果不恢复栈指针就直接跳走，后续的 DS/ES 恢复会从错误的位置弹出，寄存器全部乱掉。注意错误路径分成了两步 `.disk_error_restore_bp` 和 `.disk_error_restore`，这不是代码冗余——先弹 BP 再弹 BX，保证栈平衡。

循环正常退出后：

```asm
.read_done:
    popw %ds                             // restore DS
    popw %es                             // restore ES
    popa                                 // restore general registers

    movb $'O', %al                       // 'O' for disk OK
    outb %al, $0xe9                      // debugcon output

    movw $MINI_KERNEL_SECTORS, %ax       // return 832
    ret
```

恢复所有寄存器后，输出字符 'O' 到 debugcon——这是我们的调试约定，'O' 代表"磁盘读取 OK"。AX 返回 832（读取的总扇区数），作为返回值交给调用者。

错误处理也加上了 debugcon 输出以区分不同的失败类型：'F' 代表磁盘读取失败，'T' 代表内核太大。这三个 debugcon 字符（'O'/'F'/'T'）配合 Stage2 中的 'P'（Protected Mode）和 'L'（Long Mode），构成了一条完整的启动状态链。

## 和 xv6 磁盘读取策略的深度对比

到这里代码讲完了，我们退后一步看看 Cinux 的分块读取和其他操作系统的磁盘读取策略有什么本质区别。

**xv6** 的 `bootmain.c` 走的是完全不同的路线。xv6 在 32 位 Protected Mode 下用 IDE PIO 直接操作端口 0x1F0~0x1F7 读取磁盘，不依赖任何 BIOS 中断。它的 `readseg()` 函数调用 `waitdisk()` 等待磁盘就绪（轮询 0x1F7 端口的 BSY 和 DRDY 位），然后往端口 0x1F2~0x1F7 写入读取命令，最后用 `insl` 从数据端口 0x1F0 读入数据。整个流程不依赖 BIOS，进入保护模式后照样能用。

这种方案的好处是灵活——你在保护模式下可以做 32 位算术，可以解析 ELF header，可以遍历 Program Headers 逐段加载到任意地址。但代价也很大——你得自己实现磁盘驱动。虽然 IDE PIO 相对简单（就是往几个端口读写数据），但它只支持 IDE/SATA 硬盘，遇到 NVMe 或者 USB 启动就不灵了。

Cinux 在 Real Mode 下用 INT 13h 的选择是另一端的权衡。好处是 BIOS 帮你屏蔽了所有磁盘控制器的差异——不管底层是 IDE、SATA、NVMe 还是 USB，INT 13h 的调用方式都一样。代价是你只能在 Real Mode 下用这个接口，而且 BIOS 的实现质量参差不齐（127 扇区限制就是其中之一），分块循环读取增加了代码复杂度。但总体来说，对于 Real Mode 阶段必须完成的磁盘读取工作，INT 13h 是最省事的方案。

**Linux** 早期（0.01）用的方案和 Cinux 更像——也是在 Real Mode 下用 INT 13h 读取内核。但 Linux 用的是 INT 13h AH=0x02（CHS 模式），不是 Cinux 用的 AH=0x42（LBA 模式）。CHS 需要你自己算柱面/磁头/扇区，最大只支持 8GB 磁盘。现代 Linux 的 Bootloader（GRUB/systemd-boot）当然早就不用 CHS 了，它们在 Real Mode 下用 INT 13h AH=42h 读取内核到内存，然后切换保护模式跳转——和 Cinux 的策略非常相似。

你会发现一个有趣的趋势：简单的教学项目（Cinux、早期 Linux）倾向于在 Real Mode 下用 BIOS 做磁盘读取，而更复杂的项目（xv6、现代 Linux bootloader）要么在保护模式下自己实现磁盘驱动，要么用 Multiboot 协议让 GRUB 代劳。这种选择不是偶然的——BIOS INT 13h 在 Real Mode 下是"够用"的，但一旦你需要更大的灵活性（读取超过 1MB 的数据、在保护模式下读取、解析 ELF），就不得不抛弃 BIOS 自己动手了。

## 小结

这一篇我们完成了从 4KB 到 416KB 的磁盘读取升级，踩了栈冲突的坑，也深入对比了 Cinux 和 xv6 的磁盘读取策略。核心要点：BIOS 127 扇区限制需要分块循环、段地址动态计算公式是 `segment = 0x2000 + BX * 32`、BX/BP 必须在 BIOS 调用前保存、0x10000 和 Real Mode 栈冲突必须改为 0x20000。

下一篇我们将看 Protected Mode 的"无操作"设计、构建系统的变更，以及 004B 的整体设计决策回顾。

## 参考资料

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=42h 扩展读取的完整规范。DAP 16 字节结构定义、DS:SI 指向 DAP 的要求、每次最多 127 扇区的限制。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — Segment:Offset Addressing: Real Mode 的段地址计算详解。
  https://wiki.osdev.org/Real_Mode

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O 读取磁盘的参考实现。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 9.1 — Processor Management and Initialization。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
