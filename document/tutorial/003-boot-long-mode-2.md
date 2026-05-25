---
title: 003-boot-long-mode-2 · Long Mode
---

# 从保护模式到 Long Mode：状态机、EFER 陷阱与 64 位远跳转

> 标签：x86_64, EFER, CR0/CR3/CR4, 远跳转, 64 位 GDT, 状态机切换
> 前置：[003-1 页表构建与恒等映射](003-boot-long-mode-1.md)

## 前言

上一篇我们把四级页表搭好了——PML4 在 0x1000、PDPT 在 0x2000、PD 在 0x3000，用 2MB 大页恒等映射了前 8MB 物理内存。舞台搭好了，现在该让 CPU 登场了。但和直觉可能不太一样的是，从保护模式进入 Long Mode 不是一个"拨一下开关"的操作，而是一个严格有序的状态机——五个步骤必须按固定顺序执行，任何一步跳过、调换或者参数写错，CPU 就直接三重故障。没有报错信息，没有错误码，QEMU 窗口闪一下就没了。

我们开发这一步的时候踩了一个很经典的坑：EFER.LME 的位定义写错了（0x1000 而不是 0x100），导致 CPU 从未真正进入 Long Mode 状态，却在开启分页时被强制要求已经处于 Long Mode——结果当然是三重故障。那个 bug 定位了不短的时间，因为 `wrmsr` 不会报错，CPU 很乖地把错误的位设上了，直到开分页那一刻才炸。这一篇我们就把这个状态机的每一步拆开讲清楚，包括每一步为什么必须存在、为什么在这个位置、如果错了会怎样。

## 环境说明

代码涉及两个文件：`boot/common/long_mode.S` 中的 `enter_long_mode` 函数（模式切换核心逻辑），以及 `boot/stage2.S` 中的 GDT 扩展和 `long_mode_entry` 入口点（64 位初始化）。stage2.S 在 tag 002 的基础上追加了 64 位 GDT 描述符和 `.code64` 入口代码。调试手段和之前一样：GDB 断点 + debugcon 输出 + QEMU `-d int` 异常日志。

## 调用链：从 pm_entry 到 Long Mode

先看 stage2.S 中的调用链。保护模式入口 `pm_entry` 在输出 `P` 之后，不再进入停机循环，而是调用两个新函数：

```asm
    call setup_page_tables          // Setup page tables at 0x1000-0x3FFF
    call enter_long_mode            // Jump to 64-bit mode
```

`setup_page_tables` 在上一篇中已经讲过了——它在固定物理地址上搭建三级页表。`enter_long_mode` 是本篇的主角——它执行五步状态切换，最后做一次远跳转，把 CPU 带入 64 位 Long Mode。远跳转之后执行流到达 `long_mode_entry`（也在 stage2.S 中），完成 64 位环境初始化。

调用顺序不可调换：页表必须先建好，`enter_long_mode` 才能安全地加载 CR3 并开启分页。`enter_long_mode` 不会返回（最后是一条 `ljmp`），所以它后面的 `cli; hlt` 只是保护性代码。

## 状态机第一步：加载 CR3

```asm
enter_long_mode:
    movl $PML4_PHYS_ADDR, %eax       // PML4 table physical address
    movl %eax, %cr3                  // Load page table base
```

CR3 寄存器存储当前页表的 PML4 表物理基地址。把 0x1000 写入 CR3，CPU 就知道页表从哪开始查了。这一步必须在开启分页（CR0.PG）之前完成——道理很简单：分页开启后 CPU 立即开始用页表翻译地址，如果 CR3 还没有指向合法的 PML4 表，CPU 会去一个不确定的位置查表，大概率读到垃圾数据然后三重故障。

写 CR3 的副作用是使 TLB（Translation Lookaside Buffer，页表缓存）中所有非全局页的条目失效。不过此时分页还没开启，TLB 里本来就没有有效条目，所以这个副作用无所谓。Intel SDM Vol.3A §2.5 对 CR3 的行为有完整描述。

## 状态机第二步：启用 PAE

```asm
    movl %cr4, %eax                  // Read CR4
    orl $CR4_PAE, %eax               // Set PAE bit (bit 5)
    movl %eax, %cr4                  // Write CR4 with PAE
```

CR4.PAE（bit 5，值 0x20）启用 Physical Address Extension，让 CPU 支持 36 位以上的物理地址和四级页表结构。Long Mode 强制要求 PAE——没有 PAE 就无法使用 PML4→PDPT→PD→PT 的四级结构，没有四级结构就无法进入 Long Mode。Intel SDM Vol.3A §10.8.5 明确要求 PAE 必须在 EFER.LME 之前启用，这是状态机的硬性约束。

CR4 不能直接做位运算（`orl $0x20, %cr4` 是非法指令），必须通过通用寄存器中转：先读出来、修改、再写回去。这和上一章中 CR0 的操作方式一样，是 x86 ISA 的架构限制。

## 状态机第三步：设置 EFER.LME——最危险的一步

```asm
    movl $MSR_EFER, %ecx             // MSR_EFER address = 0xC0000080
    rdmsr                            // Read EFER into edx:eax
    orl $EFER_LME, %eax               // Set LME bit (bit 8)
    wrmsr                            // Write EFER from edx:eax
```

这一步是整个 Long Mode 切换中最容易出错的地方，也是我们踩过最大坑的地方。EFER（Extended Feature Enable Register）是一个 MSR（Model-Specific Register），地址 0xC0000080。`rdmsr` 把 ECX 指定的 MSR 读入 EDX:EAX（高 32 位在 EDX，低 32 位在 EAX），我们用 OR 指令设置 EAX 中的 bit 8（LME，Long Mode Enable，值 0x100），然后 `wrmsr` 写回去。EDX 在整个过程中保持不变。

现在说那个坑。我们在开发时把 EFER_LME 定义成了 0x1000 而不是 0x100。0x1000 是什么？那是 bit 12，对应 AMD 的 SVME（Secure Virtual Machine Enable）位。`wrmsr` 不会报错——CPU 很乖地设置了 bit 12 而不是 bit 8。然后我们继续执行后续步骤：加载 GDT、开启分页。CPU 在 CR0.PG 从 0 变成 1 的瞬间，会检查 EFER.LME 是否为 1——结果发现 LME=0，但 PG=1，这是一个非法状态组合，直接触发 #GP，然后三重故障。

GDB 里的线索是这样的：`info registers` 显示 CR4=0x20（PAE 没问题），但 EFER=0x1000——只有 SVME，没有 LME。看到这个值的时候我们花了一点时间才意识到是常量定义写错了。一个十六进制位的差距，从 0x100 到 0x1000，CPU 不报错、汇编不报错、链接不报错，运行时直接炸，这种 bug 的定位难度真的让人血压拉满。

Intel SDM Vol.3A §10.8.5 对 EFER.LME 的设置时机有明确规定：必须先启用 PAE（CR4.PAE=1），然后才能设置 EFER.LME。如果顺序反了——先设 LME 再开 PAE——某些 CPU 型号上会触发 #GP。这是一个很容易被忽略的约束，因为 OSDev Wiki 的教程里没有特别强调这个顺序。

## 状态机第四步：加载扩展 GDT

```asm
    lgdt gdt64_ptr                  // Load GDT with 64-bit descriptor
```

在远跳转之前必须加载包含 64 位描述符的 GDT。因为接下来的 `ljmp $0x18, ...` 需要用选择子 0x18 去 GDT 查找描述符——如果此时 GDTR 还指向旧的 3 条目 GDT（null、32 位代码、32 位数据），偏移 0x18 处什么都没有，CPU 查到一个 null 描述符，直接 #GP。

`gdt64_ptr` 在 stage2.S 中定义：

```asm
.global gdt64_ptr
gdt64_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // Lower 32 bits of GDT address
    .long 0                        // Upper 32 bits (zero)
```

这里用 `.long` + `.long` 而不是 `.quad` 是一个有意为之的设计选择。stage2 链接为 32 位 ELF（elf32-i386），如果写 `.quad gdt`，链接器会尝试生成 64 位的 R_X86_64_64 重定位条目——但 elf32-i386 格式不支持这种重定位类型，直接报链接错误。拆成两个 `.long` 之后，低 32 位生成普通的 R_386_32 重定位（链接器知道怎么处理），高 32 位是常量 0 不需要重定位。GDT 在低内存（0x8000 附近），高 32 位确实是 0，结果完全正确。

GDT 本身在 tag 002 的基础上追加了两个 64 位描述符：

```asm
gdt_code64:
    .quad 0x00AF9A000000FFFF       // 64-bit code descriptor (L=1, D=0)

gdt_data64:
    .quad 0x008F92000000FFFF       // 64-bit data descriptor
```

64 位代码段描述符 `0x00AF9A000000FFFF` 中，最关键的是 Flags nibble（字节 6 的高 4 位）= 0xA = `1010b`：L=1、D=0、G=1。L=1 表示这是一个 64 位代码段，D=0 确认不是 32 位默认操作数大小——Intel SDM Vol.3A §3.4.5 明确规定，只有 CS.L=1 且 CS.D=0 的组合才能让 CPU 进入真正的 64 位模式。如果 L=0，即使 EFER.LME=1、CR0.PG=1，CPU 也只在兼容模式下运行，仍然是 32 位指令集。如果 D=1 且 L=1，这是 Intel SDM 定义的保留组合，行为未定义，可能触发 #GP。

完整的 GDT 从 3 个条目扩展到了 5 个：null（0x00）、32 位代码段（0x08）、32 位数据段（0x10）、64 位代码段（0x18）、64 位数据段（0x20）。选择子的值就是描述符在 GDT 中的字节偏移。

## 状态机第五步：开启分页 + 远跳转

```asm
    movl %cr0, %eax
    orl $(CR0_PG | CR0_PE), %eax      // 0x80000001
    movl %eax, %cr0

    ljmp $0x18, $long_mode_entry
```

`CR0_PG | CR0_PE = 0x80000000 | 0x01 = 0x80000001`，同时设置 PG（bit 31）和 PE（bit 0）。PG 开启分页，PE 确保保护模式保持开启（OR 操作不会清除已有的位，所以 PE 保持为 1 不受影响）。

CR0.PG 从 0 变成 1 的瞬间是整个状态机的"激活点"。Intel SDM Vol.3A §10.8.5 描述了这一刻 CPU 做的全部事情：检查 CR4.PAE=1 ✓，检查 EFER.LME=1 ✓，检查 CR3 指向合法的 PML4 表 ✓——全部满足后，CPU 自动设置 EFER.LMA（Long Mode Active，bit 10），分页正式生效，地址翻译开始使用我们搭建的四级页表。

分页生效的那一刻，CPU 立即开始用页表翻译下一条指令的地址。因为我们的页表做的是恒等映射（虚拟地址 = 物理地址），`enter_long_mode` 函数的代码地址恰好同时是有效的虚拟地址和物理地址，CPU 能继续取到正确的指令。如果不是恒等映射——比如我们把虚拟地址 0x8000 映射到了物理地址 0x50000——那么分页开启后 CPU 会去虚拟地址 0x8000 查页表，得到物理地址 0x50000，但那里可能什么都没有，直接 page fault，然后三重故障。这就是为什么 Bootloader 的初始页表必须做恒等映射。

分页开启后 CPU 进入 Compatibility Mode（兼容模式），此时还在执行 32 位代码。真正进入 64 位模式需要远跳转刷新 CS。`ljmp $0x18, $long_mode_entry` 做了两件事：把 CS 加载为选择子 0x18（64 位代码段描述符），CPU 看到 L=1/D=0 的属性，切换到 64 位模式；同时跳转到 `long_mode_entry` 的地址。

选择子 0x18 的拆解：`0x18 >> 3 = 3`（GDT 第 4 个条目，即 gdt_code64），TI=0（使用 GDT），RPL=0（Ring 0）。

远跳转之后 `enter_long_mode` 的后续代码（`cli; hlt; jmp .lm_halt`）永远不会执行——`ljmp` 不返回。但留着作为安全兜底是好习惯，万一 CPU 因为某种诡异原因落到了这里，至少会安全地停住而不是继续执行不确定的指令。

## 64 位入口点初始化

远跳转把执行流带到了 stage2.S 中的 `long_mode_entry`：

```asm
.code64
.global long_mode_entry
long_mode_entry:
    movw $GDT_DATA64, %ax           // 0x20
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
```

`.code64` 告诉 GAS 从这里开始生成 64 位指令编码。远跳转已经让 CPU 切到 64 位模式了，`.code64` 确保汇编器的输出和 CPU 的解码方式同步——这和 tag 002 中 `.code32` 配合保护模式远跳转的逻辑完全一致。

DS、ES、FS、GS、SS 全部重载为 0x20（64 位数据段选择子）。远跳转只刷新了 CS，其他段寄存器还残留着保护模式的旧值。在 64 位模式下，大部分数据段寄存器的 Base 被 CPU 忽略（强制为 0），但 SS 仍然用于栈操作的权限检查，所以必须设为有效的数据段选择子。这一步和保护模式初始化时的段寄存器重载逻辑如出一辙——上一篇我们设 DS/ES/FS/GS/SS = 0x10，这次设成 0x20，对应的描述符从 32 位数据段换成了 64 位数据段。

```asm
    movabsq $0x90000, %rsp          // 64-bit stack pointer

    movb $CHAR_LONG_MODE, %al       // 'L' = 0x4C
    outb %al, $DEBUGCON_PORT        // Output to debugcon

    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

`movabsq` 是 x86_64 中把 64 位立即数加载到寄存器的指令，编码是 `REX.W + B8+rd + imm64`，总共 10 个字节。这里不能简化成 `movl`，因为 RSP 在 64 位模式下是 64 位寄存器，用 `movl` 只能设置低 32 位。栈顶在 0x90000，和之前保护模式下的位置一致，在 Flat Memory Model 下 SS.base=0，所以 0x90000 就是物理地址 0x90000。

最后输出 `L`（0x4C）到 debugcon 端口 0xE9。如果 `debug.log` 中同时出现 `P` 和 `L`，说明整个启动链条从实模式到保护模式再到 Long Mode 全部正确。如果只看到 `P` 没有 `L`，问题出在 Long Mode 切换过程中——大概率是 EFER.LME 没设对（查 GDB 中 EFER 的值），或者页表结构有误（用 `x/8gx` 检查三张表的内容）。

## xv6 为什么没做这一步

如果你看过 MIT 的 xv6（[GitHub](https://github.com/mit-pdos/xv6-public)），可能会注意到一个有趣的事实：原版 xv6 完全没有 Long Mode 切换。xv6 的 `bootasm.S` 只做到保护模式，然后 `bootmain` 在 32 位模式下从磁盘加载内核并跳转。整个 xv6 就是一个 32 位操作系统——没有 64 位寄存器，没有四级页表，地址空间只有 4GB。

这不是疏忽，而是教学设计的选择。xv6 的目标是让学生理解操作系统的核心概念（进程、文件系统、系统调用），而不是 x86 硬件的每一个细节。32 位模式对这些概念来说完全够用，而且跳过了 Long Mode 切换能减少很多"纯粹为了满足硬件要求"的样板代码。

但如果我们想写一个"现代"的操作系统——用 64 位 C++ 编译器编译内核、使用 64 位地址空间、跑在现代硬件上——就必须走完这条从 Real Mode 到 Protected Mode 再到 Long Mode 的完整路径。xv6 有一个 64 位的移植版本（swetland/xv6），那个版本就需要处理四级页表和 EFER.LME 这些问题。Cinux 从一开始就瞄准了 64 位，所以 Bootloader 必须完成全部三个模式的切换。

## 调试实战

### 用 GDB 逐步观察状态机

在 GDB 中可以在 `enter_long_mode` 的每一步之间设置断点，逐步查看控制寄存器的变化。最关键的是在 CR0.PG 写入前后各设一个断点：

```
(gdb) target remote :1234
(gdb) file build/boot/stage2
(gdb) break *enter_long_mode
(gdb) continue
```

到达断点后逐步执行，每次 `stepi` 后用 `info registers cr0`、`info registers cr4`、`info registers efer` 查看当前状态。开启分页后应该看到 CR0 = 0x80000011（PE+PG），CR4 = 0x20（PAE），EFER 包含 0x100（LME）和 0x400（LMA，由 CPU 自动设置）。

如果 EFER 里看到 0x1000 而没有 0x100，那就是我们踩过的那个坑——EFER_LME 常量写错了。如果 CR4 里没有 0x20，说明 PAE 没设上。

### QEMU `-d int` 查异常链

如果遇到三重故障，用 `qemu-system-x86_64 -d int -no-reboot ...` 启动，QEMU 会在每次异常时打印信息：

```
check_exception old: 0xffffffff new 0xd
     0: v=0d ecode=0x00000000 eip=0x0000816c cs=0x0008
```

CS=0x0008 说明还在保护模式（32 位代码段选择子），异常发生在 `enter_long_mode` 内部。如果 CS=0x0018 说明已经进入 Long Mode 了但后续出了问题。`-no-reboot` 防止 CPU 在三重故障后自动重启，这样你能看到完整的异常链。

### 面包屑调试法

在 `enter_long_mode` 的每一步后面都插一个 debugcon 输出，用不同字符标记进度：

```asm
    movl %eax, %cr3
    movb $'1', %al; outb %al, $0xE9    // CR3 done

    movl %eax, %cr4
    movb $'2', %al; outb %al, $0xE9    // PAE done

    // ...
```

如果 `debug.log` 里有 `P12` 但没有 `3`，问题出在 EFER.LME 那一步。这种朴素的方法在 Bootloader 这种没有任何输出手段的环境里，往往是最有效的。

## 常见 triple fault 排查表

按出现频率排序，我们把这一章相关的常见错误整理一下。

**EFER.LME 位定义错误（0x1000 vs 0x100）**。这是我们的亲身经历。`wrmsr` 不会报错，CPU 很乖地设了 SVME 位而不是 LME 位，直到开分页时才炸。排查方法：GDB 中 `info registers efer`，看到 0x1000 而没有 0x100 就是这个问题。

**PAE 和 LME 的顺序反了**。先设 LME 再开 PAE，某些 CPU 型号直接 #GP。排查方法：检查代码中 `orl $CR4_PAE, %eax; movl %eax, %cr4` 是否在 `rdmsr; orl $EFER_LME, %eax; wrmsr` 之前。

**lgdt 加载了错误的 GDT 指针**。用了 32 位的 `gdt_ptr` 而不是 64 位的 `gdt64_ptr`。GDT 里没有偏移 0x18 处的 64 位描述符，远跳转时 CPU 查到 null 描述符，直接 #GP。排查方法：确认 `lgdt` 的操作数是 `gdt64_ptr`。

**GDT64 指针用了 .quad 导致链接失败**。`.quad gdt` 在 32 位 ELF 中触发 64 位重定位错误。排查方法：如果链接阶段就报错，检查 `gdt64_ptr` 是否用了 `.quad`——换成 `.long gdt` + `.long 0` 就行。

**64 位代码段描述符的 L 位或 D 位设错**。L=0 的话 CPU 只在兼容模式，RSP 只有 32 位有效，`movabsq` 会截断。D=1 且 L=1 是保留组合，行为未定义。排查方法：用 `objdump -s -j .gdt build/boot/stage2` 查看 GDT section 的原始字节，确认偏移 0x18 处的字节 6 高 4 位是 0xA（L=1, D=0, G=1）。

## 收尾

到这里，tag 003 的全部内容就完成了。验证方式很简单：构建并运行 QEMU，检查 `debug.log` 文件。如果里面出现了 `P`（tag 002 的保护模式验证）和 `L`（本 tag 的 Long Mode 验证），恭喜——从实模式到保护模式再到 Long Mode 的完整启动链条已经打通。CPU 现在运行在 64 位模式下，64 位寄存器、四级页表、64 位栈全部正常工作。

回过头看，从保护模式到 Long Mode 的切换，核心就是五步状态机：CR3 加载页表基址、CR4.PAE 启用物理地址扩展、EFER.LME 开启 Long Mode 使能、lgdt 加载扩展 GDT、CR0.PG 开启分页 + ljmp 远跳转。每一步都有严格的前置条件和顺序约束，任何一步出错都会导致三重故障。这套流程虽然看起来死板，但它和 tag 002 的保护模式切换一样，是 x86_64 架构的硬性要求——从 AMD 在 2003 年定义 x86-64 架构以来就没变过。

下一篇（tag 004）我们将继续往上走——在 Long Mode 下加载并跳转到 C++ 内核。那一步需要设置内核的入口参数（BootInfo 结构），把控制权从汇编 Bootloader 交给 C++ 代码，正式进入内核开发阶段。

## 参考资料

- Intel SDM Vol.3A §10.8.5 — Initializing IA-32e Mode：五步切换序列，PAE→LME→CR3→PG→ljmp 的严格顺序
- Intel SDM Vol.3A §10.8.5.3 — CS.L 和 CS.D 决定子模式：L=1/D=0 进入 64 位模式，L=0 进入兼容模式
- Intel SDM Vol.3A §2.5 — Control Registers：CR0.PG/PE、CR3、CR4.PAE 的位定义和操作约束
- Intel SDM Vol.3A §3.4.5 — Segment Descriptors：L 位（bit 21）和 D 位（bit 22）的含义，64 位代码段描述符格式
- Intel SDM Vol.3A §4.5 — 4-Level Paging：四级页表结构，CR3 和 PML4 的关系
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode) — 完整的 Long Mode 切换教程
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table) — 64 位 GDT 描述符格式
- xv6 bootasm.S: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S) — xv6 只做到保护模式，不进入 Long Mode
- Linux kernel head_64.S: [linux-insides](https://0xax.gitbooks.io/linux-insides/content/Booting/linux-bootstrap-4.html) — Linux 的 Long Mode 切换实现
