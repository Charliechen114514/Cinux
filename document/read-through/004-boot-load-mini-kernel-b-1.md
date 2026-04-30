# 004 通读版 · Real Mode 完整加载小内核

到 tag 004A 为止，我们的 bootloader 已经能在 Real Mode 下完成 E820 内存探测和 ELF header 的初次读取（4KB），但离"把完整小内核加载到内存"这个目标还有一段距离。004A 留下的状态是：物理地址 0x10000 处放着 4KB 的 ELF header，Stage2 还不知道内核到底有多大、需要读多少扇区、加载到哪里。现在我们要把这件事彻底做完。

tag 004B 做的事情可以用一句话概括：**在 Real Mode 下把完整的小内核 flat binary 一次性读到目标地址，然后 Protected Mode 什么都不做，直接穿过进入 Long Mode**。听起来简单，但这一句话背后牵出了好几个设计决策和至少一个让我血压拉满的踩坑——栈冲突。我们新增了 BootInfo 结构体定义（bootloader 和内核的交接契约），把磁盘读取从 8 扇区扩展到 832 扇区（分块循环），把小内核从 ELF 格式切换到 flat binary，把加载地址从 0x10000 改到 0x20000（避开 Real Mode 栈），还给小内核加上了真正的入口代码和 BSS 清零逻辑。

这一章的内容虽然涉及 18 个文件的变更，但主题非常内聚——所有东西都围绕"怎么把小内核完整地搬进内存"这个核心问题展开。

## 架构图

先看全局启动流程，搞清楚 004B 在链条中做了什么、跳过了什么：

```
                    Cinux Tag 004B 启动流程
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
     ├── query_memory_map()         (004A, E820 -> 0x5000)
     ├── load_kernel_from_disk()    <-- CHANGED: 832 扇区 -> 0x20000
     |
     ├── 切换保护模式 (CR0.PE = 1)
     ├── [pm_entry]                 <-- 004B: 无操作，直接穿过
     ├── setup_page_tables()        (0x1000 ~ 0x3FFF)
     ├── enter_long_mode()
     |
     └── [Long Mode]
          └── hlt (004B 仍不跳转; 004C 填 BootInfo 并跳转)
```

内存布局视角——注意 004B 新增的加载区域和 BootInfo 结构体约定位置：

```
  物理地址          内容                       所属 Tag
  ──────────────────────────────────────────────────────
  0x0000~0x0400     IVT (中断向量表)            BIOS
  0x5000            E820 内存映射表              004A
  0x6000            VBE Controller Info         002
  0x6200            VBE Mode Info               002
  0x6400            Framebuffer Info            002
  0x7000            BootInfo 结构体              004B (约定位置，004C 填充)
  0x7B00            DAP (磁盘读取参数包)         004A
  0x9000            Real Mode 栈 (SS=0x0900)     001
  ──────────────────────────────────────────────────────
  0x20000           Mini Kernel (flat binary)   004B <-- 最大 416KB
  ~0x88000          Mini Kernel 结束             004B
  ── 32KB gap ──
  0x90000           Protected/Long Mode 栈      002
```

BootInfo 结构体的字段布局：

```
  BootInfo @ 0x7000
  ┌─────────────────────────────────────────┐
  │ uint64_t entry_point            (8B)    │  高半核虚拟入口地址
  │ uint64_t kernel_phys_base       (8B)    │  物理加载基址 (0x20000)
  │ uint64_t kernel_size            (8B)    │  内核大小
  ├─────────────────────────────────────────┤
  │ uint64_t fb_addr                (8B)    │  帧缓冲物理地址
  │ uint32_t fb_width               (4B)    │  宽度 (px)
  │ uint32_t fb_height              (4B)    │  高度 (px)
  │ uint32_t fb_pitch               (4B)    │  扫描线字节数
  │ uint32_t fb_bpp                 (4B)    │  位深 (通常 32)
  ├─────────────────────────────────────────┤
  │ uint32_t mmap_count             (4B)    │  内存映射条目数
  │ uint32_t _pad                   (4B)    │  对齐填充
  │ MemoryMapEntry mmap[32]   (32*24=768B)  │  E820 条目数组
  └─────────────────────────────────────────┘
  总大小 ≈ 820 字节
```

你会发现整个设计的核心思路是：在 Real Mode 里把所有需要 BIOS 中断的事情全部做完（E820 探测 + 完整磁盘读取），然后保护模式只是过渡，什么额外工作都不做，直接穿过去进 Long Mode。这种设计意味着保护模式阶段的代码极度简洁，但也意味着 Real Mode 阶段的磁盘读取必须一次性完成——不能偷懒，不能分段，416KB 的数据必须一口气搬进来。

## 代码精讲

### BootInfo 结构体（boot/boot_info.h）

我们先从 BootInfo 结构体开始看，因为它是 bootloader 和内核之间的"交接契约"——后续所有 tag 都依赖这个结构来传递启动信息。

```c
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed)) MemoryMapEntry;

static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
```

`MemoryMapEntry` 直接对应 E820 BIOS 返回的内存映射条目格式：base 是物理基地址，length 是区域长度，type 标识内存类型（1=可用 RAM，2=保留，3=ACPI 可回收等），acpi 是 ACPI 扩展属性（通常为 0）。24 字节的大小来自 ACPI 3.0 的扩展定义——老的 BIOS 只返回 20 字节（没有 acpi 字段），但我们统一按 24 字节接收。`__attribute__((packed))` 禁止编译器插入填充字节，`static_assert` 在编译时验证大小——如果因为 ABI 差异导致结构体大小不是 24 字节，编译直接报错。

这里有一个容易忽略的关键点：这个头文件同时被两套编译环境使用。bootloader 的代码以 `-m32` 编译（32 位），内核代码以 `-m64` 编译（64 位）。如果不加 `packed`，32 位和 64 位编译器可能对结构体做不同的对齐填充，导致两边对同一块内存的解读不一致。所有字段使用显式大小的类型（uint32_t、uint64_t），配合 packed 属性，确保两个编译环境看到的内存布局完全相同。

```c
typedef struct {
    uint64_t entry_point;
    uint64_t kernel_phys_base;
    uint64_t kernel_size;

    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;

    uint32_t mmap_count;
    uint32_t _pad;
    MemoryMapEntry mmap[32];
} __attribute__((packed)) BootInfo;
```

`BootInfo` 结构体包含三大块信息。前三个字段是内核信息：entry_point 是高半核虚拟入口地址（0xFFFFFFFF80020000），kernel_phys_base 是物理加载基址，kernel_size 是实际大小。中间四个字段是帧缓冲信息，来自 VESA BIOS 调用保存在 0x6400 的数据。最后是 E820 内存映射数组，最多 32 条。

注意 `_pad` 字段——它在 mmap_count 之后、mmap 数组之前，把 mmap 数组对齐到 8 字节边界。虽然 packed 属性会禁止编译器自动填充，但显式写一个 padding 字段有两个好处：第一，代码的意图一目了然（读者知道这里有个对齐间隙）；第二，如果我们将来去掉 packed 属性，结构体布局不会因为这个间隙而改变。

头文件里的注释还列出了完整的地址约定表——BootInfo 结构体放在物理 0x7000，E820 缓冲区在 0x5000，帧缓冲信息在 0x6400。这些固定地址在调试时非常有用：你可以在 QEMU monitor 或者 GDB 里直接 `x/8x 0x7000` 查看 BootInfo 的内容，不用猜测数据在哪里。

### 分块磁盘读取（boot/common/boot.S — load_kernel_from_disk）

接下来是本章最核心的代码变更：`load_kernel_from_disk` 从 004A 的"读 8 个扇区"升级为"读 832 个扇区（416KB）"。这个函数的工作量暴增了两个数量级，也引入了好几个新坑。

先看常量定义的变化：

```asm
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set MINI_KERNEL_SECTORS,     832         // Total sectors (416KB)

.set MINI_KERNEL_LOAD_PHYS,   0x20000     // Physical address where kernel is loaded
.set MINI_KERNEL_LOAD_SEG,    0x2000      // Segment: 0x2000 (0x2000 << 4 = 0x20000)
.set MINI_KERNEL_LOAD_OFF,    0x0000      // Offset

.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
.set DISK_MAX_SECTORS_PER_CALL, 127       // Max sectors per BIOS call (int13 limit)
```

和 004A 相比有几个关键变化。加载地址从 0x10000 变成了 0x20000——这个改动背后的故事等一下展开讲，它涉及一个很恶心的栈冲突 bug。扇区数从 8 变成了 832（416KB），这意味着不能一次读完，必须分块循环。新增了 `DISK_MAX_SECTORS_PER_CALL = 127`，这是 INT 13h 扩展读取的单次上限——有些 BIOS 实现甚至只支持更少的扇区，127 是一个经过实测的安全值。

现在看函数主体。整个函数的核心结构是一个 `.read_loop` 循环：

```asm
load_kernel_from_disk:
    pusha                                // save all 16-bit registers
    pushw %es                            // save ES
    pushw %ds                            // save DS

    xorw %bx, %bx                        // BX = 0 (no sectors read yet)
    xorw %si, %si                        // SI = 0 (buffer offset, stays 0)

    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %es
```

函数入口保存所有寄存器——注意这里从 004A 的 `pushaw` 改成了 `pusha`，两者在 16 位模式下效果一样，`pusha` 是更简洁的写法。BX 寄存器被用作"已读扇区计数器"，初始化为 0。SI 保持为 0——它在后面会被用来指向 DAP 的偏移地址。

接下来是循环本体：

```asm
.read_loop:
    cmpw $MINI_KERNEL_SECTORS, %bx       // Compare with total
    jae .read_done                       // Done if BX >= total

    movw $MINI_KERNEL_SECTORS, %ax       // AX = total sectors
    subw %bx, %ax                        // AX = remaining sectors
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax // Compare with max
    jbe .read_count_ok                   // Use AX if <= 127
    movw $DISK_MAX_SECTORS_PER_CALL, %ax // Cap at 127

.read_count_ok:
    movw %ax, %bp                        // BP = sectors to read
```

循环开头先检查是否读完了——BX（已读扇区数）大于等于 832 则退出。然后计算本次应该读多少扇区：取 min(127, 剩余扇区数)。这个计算逻辑虽然只有几条指令，但它处理了两种情况——最后一次迭代可能不满 127 扇区，这时候应该精确读取剩余量。BP 寄存器暂存本次读取量，后面更新 BX 时要用到。

接下来是 DAP（Disk Address Packet）的构建——这部分每次循环都要重新填写，因为 buffer 地址和 LBA 都在变化：

```asm
    // CRITICAL: Re-set ES before building DAP (BIOS may clobber it)
    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %es                        // ES = DAP segment

    movw $DAP_OFFSET, %di                // DI = 0x0000

    movb $16, %es:(%di)                  // DAP size = 16
    movb $0, %es:1(%di)                  // DAP reserved = 0

    movw %bp, %es:2(%di)                 // DAP count = BP (sectors this round)
```

这里有一个很容易翻车的细节：每次循环迭代都必须重新设置 ES 为 DAP 段地址（0x07B0）。为什么？因为 INT 13h 调用可能破坏 ES 的值——BIOS 内部实现不保证保留调用者的段寄存器。如果你偷懒只设置一次 ES 然后循环复用，第二轮开始 ES 已经不是 0x07B0 了，你往 `es:(%di)` 写的 DAP 字段全部写到了错误的位置。这个 bug 在 QEMU 里可能不触发（QEMU 的 SeaBIOS 实现比较规矩），但在真实硬件或者 Bochs 里几乎必现。

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

### Mini Kernel 入口（kernel/mini/arch/x86_64/boot.S + main.cpp）

004B 给小内核加上了真正的入口代码，不再是 004A 里那个 `cli; hlt` 死循环。

先看汇编入口 `kernel/mini/arch/x86_64/boot.S`：

```asm
.section .text
.code64

.global _start
.type _start, @function

_start:
    cli

    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    movq $__bss_start, %rdi
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb

    movq %rdi, __boot_info_ptr

    movq __boot_info_ptr, %rdi
    call mini_kernel_main

.halt:
    cli
    hlt
    jmp .halt
```

这段代码做了四件事。第一，关中断并设置栈指针到 `__mini_stack_top`——这个符号在 BSS 段末尾定义，提供了 8KB 的栈空间。`xorq %rbp, %rbp` 清零帧指针，这是标准做法，方便调试器回溯调用栈时知道栈帧链在哪里终止。第二，清零 BSS 段——从 `__bss_start` 到 `__bss_end` 的所有字节全部填零。BSS 段是未初始化全局变量和静态变量存放的地方，C/C++ 标准要求它们初始值为 0，但 flat binary 不包含这个信息（不像 ELF 会在加载时自动处理），所以我们必须手动清零。第三，把 BootInfo 指针（从 bootloader 通过 RDI 传入）保存到全局变量 `__boot_info_ptr`。第四，把 BootInfo 指针作为第一个参数传给 `mini_kernel_main` 并调用。

栈和 BootInfo 指针存储在 BSS 段里：

```asm
.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

.skip MINI_STACK_SIZE
__mini_stack:
__mini_stack_top:

.global __boot_info_ptr
.skip 8
__boot_info_ptr:
```

8KB 的栈空间对于当前的极简内核来说绑绰有余。`__mini_stack_top` 指向栈顶（因为 x86 栈是向下增长的），`__boot_info_ptr` 预留 8 字节用于存储 BootInfo 指针。

然后是 C++ 入口 `kernel/mini/main.cpp`：

```cpp
extern "C" {
#include <stdint.h>
}

extern "C" {
    extern uint64_t __boot_info_ptr;
}

static void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    (void)boot_info_addr;

    debugcon_putc('M');

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

目前 `mini_kernel_main` 的全部工作就是输出字符 'M' 到 debugcon 然后停机。'M' 代表 "Mini kernel reached"。BootInfo 参数虽然声明了但暂时没用——真正的 BootInfo 填充和使用留到 tag 004C。

如果你把所有 debugcon 输出连起来看，启动成功的完整字符序列应该是 `PLO`——'P' 是 Stage2 进入 Protected Mode 后输出的（等等，实际上 'O' 是在 Real Mode 磁盘读取成功后输出的，'P' 是进入 PM 后输出的，'L' 是进入 Long Mode 后输出的）。但要注意这些字符的输出顺序和位置：'O' 在 `load_kernel_from_disk` 的末尾（Real Mode），'P' 在 `pm_entry`（Protected Mode），'L' 在 `long_mode_entry`（Long Mode）。所以 debugcon 日志应该显示 `OPL`——磁盘读取 OK、进入 PM、进入 LM。如果你在 004C 之后加上小内核跳转，还会看到一个 'M'，变成 `OPLM`。

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
    }
}
```

物理起始地址 0x20000 是本章所有改动的锚点——加载地址必须和链接地址完全一致。如果链接脚本写 0x20000 但 bootloader 把数据加载到了 0x30000，所有绝对地址引用（函数调用、全局变量访问）全部指向错误的内存位置。BSS 段添加了 `__bss_start` 和 `__bss_end` 符号，供 boot.S 的 BSS 清零逻辑使用。`/DISCARD/` 段扔掉了编译器自动生成的注释、note 和异常帧信息——这些在裸机环境里没有用处，留着只会增加二进制体积。

输出格式仍然是 `elf64-x86-64`——这是编译器输出的格式，然后通过 objcopy 转成 flat binary。链接器本身需要一个合法的 ELF 格式来工作（它需要解析符号、处理重定位），只是最终产物被 strip 成了纯二进制。

### 构建系统（kernel/mini/CMakeLists.txt + cmake/qemu.cmake）

构建系统的变更主要在三处：小内核的 CMakeLists 加了 boot.S 和 objcopy 后处理，boot/CMakeLists.txt 移除了 ELF loader 相关注释，qemu.cmake 改用 mini_kernel.bin。

`kernel/mini/CMakeLists.txt`：

```cmake
add_executable(mini_kernel
    arch/x86_64/boot.S
    main.cpp
)

set_target_properties(mini_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel"
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

add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel> $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)
```

小内核现在编译两个源文件：`arch/x86_64/boot.S`（汇编入口）和 `main.cpp`（C++ 主函数）。编译选项里有几个值得关注的：`-mcmodel=large` 允许内核使用任意地址（不只是低 2GB），这对 higher-half 内核是必要的；`-mno-red-zone` 禁用 System V AMD64 ABI 的红区（128 字节的栈下溢保护区），因为内核代码随时可能被中断打断，红区里暂存的数据会被覆盖；`-fno-pie` 和 `-no-pie` 禁用位置无关可执行文件——我们的内核有固定加载地址，不需要 PIC 重定位。

POST_BUILD 步骤用 `objcopy -O binary` 把 ELF 转换成 flat binary。这一步非常关键：`-O binary` 剥离所有 ELF header、section header、符号表，只保留段的原始二进制内容。输出文件的第一个字节就是 `.text` 段的第一个字节，也就是 `_start` 的第一条指令。`$<TARGET_FILE:mini_kernel>` 是 CMake 的 generator expression，在构建时展开为实际的 ELF 文件路径。

`cmake/qemu.cmake` 中镜像构建改为使用 flat binary：

```cmake
set(MINI_BIN   "${CMAKE_BINARY_DIR}/kernel/mini/mini_kernel.bin")
add_custom_command(
    OUTPUT ${CINUX_IMAGE_PATH}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build_image.sh
        ${MBR_BIN}
        ${STAGE2_BIN}
        ${MINI_BIN}
        ${CINUX_IMAGE_PATH}
    DEPENDS mbr stage2 mini_kernel
)
```

注意变量名从 004A 的 `MINI_ELF` 改成了 `MINI_BIN`，路径也指向 `mini_kernel.bin` 而不是 `mini_kernel`（ELF 可执行文件）。`DEPENDS` 里的 `mini_kernel` 目标会先执行 POST_BUILD 步骤生成 .bin 文件，然后 `build_image.sh` 才会被调用。

`boot/CMakeLists.txt` 里有一段注释标记了 ELF loader 的移除：

```cmake
# ==============================================================
# C ELF Loader (32-bit freestanding)
# ==============================================================
# REMOVED: No longer needed, using flat binary format
```

原始计划中 004B 应该包含一个 32 位 C 语言编写的 ELF loader（`boot/elf_loader.c`），在 Protected Mode 下解析 ELF header、遍历 PT_LOAD 段、加载内核。这个方案在切换到 flat binary 后就不再需要了——少了几百行 C 代码，也少了一个 `-m32` 编译的 C 文件被链接进 16 位 Stage2 的麻烦。

### 构建脚本（scripts/build_image.sh）

`build_image.sh` 负责把所有组件按正确布局写入磁盘镜像。004B 的关键变更是使用 flat binary 替代 ELF 文件，并加上了大小限制检查。

```bash
MBR_BIN=${1:-${BUILD_DIR}/boot/mbr.bin}
STAGE2_BIN=${2:-${BUILD_DIR}/boot/stage2.bin}
MINI_BIN=${3:-${BUILD_DIR}/kernel/mini/mini_kernel.bin}
OUTPUT_IMAGE=${4:-${BUILD_DIR}/cinux.img}
```

三个输入文件和输出镜像路径。`MINI_BIN` 默认指向 `mini_kernel.bin`——注意不再是 ELF 文件。

磁盘布局常量：

```bash
STAGE2_LBA=1
STAGE2_MAX_SECTORS=15
MINI_KERNEL_LBA=16

MINI_KERNEL_MAX_BYTES=$((416 * 1024))    # 425984 bytes
MINI_KERNEL_MAX_SECTORS=$((MINI_KERNEL_MAX_BYTES / 512))  # 832 sectors
```

扇区 0 给 MBR，扇区 1-15 给 Stage2（最大 15 个扇区 = 7680 字节），扇区 16 开始给小内核。小内核的大小上限是 416KB（832 扇区），这个数字不是随便定的——它来自内存布局的硬约束。

脚本里有详细的大小限制验证逻辑：

```bash
if [ $MINI_SIZE -gt $MINI_KERNEL_MAX_BYTES ]; then
    log_error "Mini kernel too large!"
    log_error "       Actual:   $MINI_SIZE bytes ($MINI_SECTORS sectors)"
    log_error "       Maximum:  $MINI_KERNEL_MAX_BYTES bytes ($MINI_KERNEL_MAX_SECTORS sectors, 416KB)"
    # ... error context ...
    exit 1
fi
```

如果小内核超过 416KB，构建直接失败并打印详细的错误信息和内存布局约束说明。这种 fail-fast 的设计比运行时才发现数据写到了栈区要好得多——至少你不用对着一个无意义的 triple fault 发呆。

磁盘镜像的组装：

```bash
dd if=/dev/zero of="$OUTPUT_IMAGE" bs=1M count=1 status=none
dd if="$MBR_BIN" of="$OUTPUT_IMAGE" bs=512 count=1 conv=notrunc status=none
dd if="$STAGE2_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$STAGE2_LBA conv=notrunc status=none
dd if="$MINI_BIN" of="$OUTPUT_IMAGE" bs=512 seek=$MINI_KERNEL_LBA conv=notrunc status=none
```

先创建 1MB 的空白镜像，然后依次写入 MBR（扇区 0）、Stage2（从扇区 1 开始）、小内核（从扇区 16 开始）。`conv=notrunc` 确保 dd 不会截断输出文件——如果没有这个选项，写入 Stage2 时会把后面已经写好的数据全部删掉。`seek` 参数指定从哪个扇区开始写入，这就是为什么 Stage2 和小内核能正确地落在各自的位置上。

## 设计决策

### Flat Binary vs ELF 加载

**问题**：bootloader 应该加载 ELF 格式的内核还是 flat binary？

**本项目的做法**：使用 `objcopy -O binary` 把 ELF 转成 flat binary，bootloader 直接把二进制数据读到目标地址，不做任何格式解析。

**备选方案**：在 Protected Mode 下实现 ELF64 loader，解析 Program Header，遍历 PT_LOAD 段加载到各自的目标地址，处理 BSS 清零。

**为什么不选备选方案**：Real Mode 的 16 位环境做不了 64 位算术——ELF64 里的地址字段都是 64 位的，你需要在 16 位实模式下用多个寄存器拼接才能读写。虽然可以在 Protected Mode（32 位）下做 ELF 解析，但那意味着 Stage2 里要链接一个 32 位 C 编译的 ELF loader，增加了构建系统的复杂度。而且 004A 原来设计的两阶段方案（先读 header 再读完整内核）需要额外的扇区读取逻辑和地址计算，flat binary 方案把这些全部省掉了。

**如果要改进**：小内核加载大内核的时候（tag 008），mini kernel 已经有完整的 C++ 运行环境和内存管理，这时候用 ELF 格式加载大内核就是正确的选择了——可以处理任意数量的 PT_LOAD 段，做 BSS 清零，甚至将来加 ASLR 也很方便。bootloader 到 mini kernel 用 flat binary 是"简单"，mini kernel 到 big kernel 用 ELF 是"正确"，两者各取所长。

### Real Mode 完成全部磁盘读取

**问题**：磁盘读取应该在 Real Mode 一次性完成，还是部分放到 Protected Mode？

**本项目的做法**：全部在 Real Mode 完成，832 扇区一口气读完。

**备选方案**：Real Mode 只读 ELF header，Protected Mode 下用 IDE PIO 端口 I/O 自己实现磁盘驱动来读剩余部分（类似 xv6 的方案）。

**为什么不选备选方案**：在 Protected Mode 下自实现磁盘驱动意味着你需要了解 ATA PIO 协议、处理 BUSY/DRQ 状态位、等待中断或者轮询——这些代码量不比 Real Mode 的 INT 13h 调用少，而且调试难度更高（端口 I/O 出错了没有错误码，只能靠现象推断）。Real Mode 下有 BIOS 帮你处理所有硬件细节，你只需要填好 DAP 调一次中断。416KB 的读取确实需要 ~7 次循环（每次 127 扇区），但循环逻辑本身并不复杂。

### 固定地址 0x20000 vs 可配置加载地址

**问题**：小内核的加载地址应该是硬编码的固定值，还是通过某种协商机制动态决定？

**本项目的做法**：硬编码 0x20000，在链接脚本、boot.S 常量、build_image.sh 中保持一致。

**备选方案**：在磁盘镜像的固定偏移处放一个 header，包含加载地址和大小信息，bootloader 读取后动态加载。

**为什么不选备选方案**：动态发现机制在内核开发初期是过度设计。固定地址的调试优势非常明显——你知道小内核永远在 0x20000，可以直接用 GDB 的 `x/10i 0x20000` 查看前 10 条指令。而且 0x20000 这个地址不是拍脑袋定的，它来自对低 1MB 内存布局的仔细分析：避开 Real Mode 栈（0x9000~0x19000），留足安全间隙（28KB），同时不超过 Protected Mode 栈（0x90000）减去 32KB gap 的上限（0x88000），可用空间正好是 0x88000 - 0x20000 = 0x68000 = 416KB。

**如果要改进**：如果将来小内核膨胀到超过 416KB，有两个选择——要么把 Protected Mode 栈挪到更高的地址（比如 0x100000），要么在进入 Long Mode 后使用 64 位磁盘驱动把内核加载到更高的物理地址。两种方案都意味着更多的代码变更，但至少当前的固定地址设计把约束条件列得清清楚楚，改起来不懵。

## 扩展方向

1. **串口输出和 E820 打印**（难度：简单）——在 mini kernel 里加入串口驱动（tag 005 的内容），把 BootInfo 里的内存映射条目打印出来。你会看到 QEMU 的 6~7 条 E820 记录，包括低 640KB 可用 RAM、ACPI 数据区、高内存区域等。

2. **多引导协议支持**（难度：中等）——当前 BootInfo 是 Cinux 私有格式，可以扩展为兼容 Multiboot2 规议的 tag 链式结构，这样就能用 GRUB 直接引导 Cinux，省掉自写 bootloader 的麻烦。

3. **BootInfo 校验和**（难度：简单）——给 BootInfo 加一个 checksum 或 magic number 字段，内核入口处验证数据完整性。这个改动很小，但能帮你及早发现 bootloader 没有正确填充 BootInfo 的 bug。

4. **动态内核大小探测**（难度：中等）——当前内核大小（832 扇区）是编译时固定的。可以在磁盘镜像的固定位置（比如扇区 15 的最后几个字节）写入内核的实际扇区数，bootloader 读取后动态确定读取量，这样小内核大小变化时不需要同步修改 boot.S 的常量。

5. **真实硬件测试**（难度：困难）——把 Cinux 写到 U 盘上在真实 PC 上启动。需要处理的主要差异：BIOS 的 INT 13h 实现行为可能不同（有些 BIOS 的单次读取上限不到 127）、A20 gate 需要额外的启用方式、USB 启动可能需要 USB emulation 模式支持。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 9.1 — Processor Management and Initialization。Real Mode、Protected Mode、Long Mode 的状态转换和寄存器行为。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=42h 扩展读取的完整规范。单次调用上限、DAP 16 字节结构、DS:SI 指向 DAP 的要求。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — Rolling Your Own Bootloader: 自写 bootloader 的综合指南。关于选择加载地址（避开栈区和 BIOS 数据区）、flat binary vs ELF 的取舍讨论。
  https://wiki.osdev.org/Rolling_Your_own_Bootloader

- OSDev Wiki — Multiboot: Multiboot/Multiboot2 规范定义的 bootloader-kernel 交接设计。BootInfo 的设计借鉴了其思路。
  https://wiki.osdev.org/Multiboot

- Linux x86 Boot Protocol: Linux 内核的 bootloader-kernel 交接协议。`struct boot_params` 的设计理念和 Cinux 的 BootInfo 相同——固定地址 + 结构体 + 寄存器传递指针。
  https://www.kernel.org/doc/html/latest/x86/boot.html

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O 读取磁盘的实现，与 Cinux 的 Real Mode + BIOS INT 13h 方案形成对比。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c
