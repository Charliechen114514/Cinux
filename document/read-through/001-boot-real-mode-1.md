# 001 Read-through (1/3) —— MBR：512 字节里的引导艺术

## 概览

本文逐行讲解 `boot/mbr.S`——Cinux 的 Master Boot Record。它是整个操作系统第一个被执行的代码，BIOS 把它从磁盘第一个扇区加载到 0x7C00 后，我们的故事就从这里开始。MBR 的职责很明确：初始化段寄存器和栈、保存启动盘号、打印一条启动消息、把 Stage2 从磁盘读到 0x8000、然后跳转过去。整个代码不到 512 字节，每一条指令都有它存在的理由。

关键设计决策：MBR 不链接 common/serial.S 的共享函数库，而是自带一个精简版的 `print_string_mbr`。这不是偷懒，而是 512 字节硬限制下的必然选择——链接额外的目标文件会让 .text 段膨胀，超出 512 字节后 BIOS 根本不会加载后面的部分。

## 架构图

```
磁盘布局：
┌──────────────┐ Sector 0 (LBA 0)
│ MBR (512B)   │ ← BIOS 加载到 0x7C00
├──────────────┤ Sector 1 (LBA 1)
│              │
│ Stage2       │ ← MBR 用 INT 0x13 加载到 0x8000
│ (≤15 sectors)│
│              │
└──────────────┘

MBR 执行流程：
BIOS → _start (0x7C00)
  │
  ├─ ljmp $0, $real_start  ← 规范化 CS
  │
  ├─ real_start:
  │   ├─ cli
  │   ├─ 设置 DS=ES=SS=FS=GS=CS=0
  │   ├─ SP = 0x7000
  │   ├─ sti
  │   ├─ 保存 DL → boot_drive
  │   ├─ call load_stage2
  │   ├─ 打印 "Cinux Booting..."
  │   └─ ljmp $0x0800, $0  → Stage2
  │
  └─ [出错] → die: cli; hlt; jmp die
```

## 代码精讲

### 常量定义——内存布局的蓝图

```asm
.set STAGE2_LBA,          1
.set STAGE2_SECTORS,      15
.set STAGE2_LOAD_ADDR,    0x8000
.set STACK_BASE_ADDR,     0x7000
.set DAP_STORE_ADDR,      0x7B00
.set DAP_SIZE,            0x10
```

这里定义了 MBR 运行时的整个内存布局。Stage2 从 LBA 1 开始（紧接 MBR 之后），读取 15 个扇区（7.5KB）到 0x8000。栈底设在 0x7000——在 MBR 代码（0x7C00）下方 3KB 处，向下增长，不会碰到 IVT（中断向量表，0x0000-0x03FF）也不会碰到 MBR 本身。DAP 结构放在 0x7B00，紧贴 MBR 下方——这个位置的选择很讲究，它必须在 MBR 之外（不能覆盖 0x7C00 开始的代码），但又不能太远（MBR 里所有地址都是用立即数常量计算的）。

### 入口——那条必须有的远跳转

```asm
_start:
    ljmp $0, $real_start
```

这一条指令是整个 MBR 中最容易被忽略、也最致命的。BIOS 跳转到 MBR 时 CS 可能是 0x0000 也可能是 0x07C0（不同 BIOS 实现不同），IP 对应地是 0x7C00 或 0x0000。如果不做处理，后续所有基于 CS 的地址计算都不确定。`ljmp $0, $real_start` 同时修改 CS 和 IP——CS 被强制设为 0，IP 设为 `real_start` 的链接地址。从此以后 CS=0，所有段:偏移地址统一为 `0:offset`，等于纯偏移寻址。

### 段寄存器初始化——让所有地址回归统一

```asm
real_start:
    cli
    xorw %ax, %ax
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    cld
    movw $STACK_BASE_ADDR, %sp
    sti
```

先用 `cli` 关中断是必要的——修改 SS 和 SP 的过程中如果发生中断，中断处理程序会用一个半成品的栈，后果不可预测。把所有段寄存器设为和 CS 相同（都是 0），确保所有内存访问都使用一致的地址模型。`cld` 清除方向标志，让 `lodsb` 等字符串指令正向递增 SI。栈指针设好后用 `sti` 重新开中断，因为后面要调用 BIOS 服务——BIOS 中断依赖中断机制正常工作。

这里有个细节值得注意：代码用 `movw %cs, %ax` 而不是直接 `xorw %ax, %ax` 然后把 0 写入各段寄存器。虽然效果相同（因为前面 ljmp 已经让 CS=0），但用 CS 复制更明确地表达了"让所有段寄存器和 CS 保持一致"这个意图。

### 保存启动盘号

```asm
    movb %dl, boot_drive
```

BIOS 在跳转 MBR 前会把启动盘号放在 DL 寄存器里（0x00 = 第一个软驱，0x80 = 第一个硬盘）。这个值在后续读磁盘时必须用到，但 BIOS 中断调用可能会修改 DX，所以必须第一时间保存到内存变量里。

### print_string_mbr——精简到极致的字符串输出

```asm
print_string_mbr:
    cld
._loop:
    lodsb
    test %al, %al
    jz ._done
    mov $0x0E, %ah
    int $0x10
    jmp ._loop
._done:
    ret
```

这个函数是 common/serial.S 里 `print_string` 的精简版。区别在于：不保存和恢复寄存器（MBR 里调用频率低，调用者负责保存），不设置 BX（使用默认页号和颜色）。省掉的这几条 push/pop 在 512 字节的限制下很珍贵。`lodsb` 从 DS:SI 读一个字节到 AL 并自动递增 SI，`test %al, %al` 检查是否到了字符串末尾（null 终止符），`INT 0x10 AH=0x0E` 是 BIOS teletype 输出——自动处理换行回车，在当前光标位置显示字符。

### load_stage2——构建 DAP 并调用扩展读盘

```asm
load_stage2:
    movw $DAP_STORE_ADDR, %si
    movb $DAP_SIZE, (%si)
    movw $STAGE2_SECTORS, 2(%si)
    movw $STAGE2_LOAD_ADDR, 4(%si)
    movw $0, 6(%si)
    movl $STAGE2_LBA, 8(%si)
    movl $0, 12(%si)
    movb boot_drive, %dl
    movw $0x4200, %ax
    int $0x13
    jc disk_error
    ret
```

这是 MBR 最核心的部分。DAP 结构被逐字段写入 0x7B00 开始的 16 字节。每个字段的偏移对应 OSDev Wiki 上记录的 DAP 规范：偏移 0 是结构大小（固定 0x10），偏移 2 是扇区数（15），偏移 4 和 6 分别是缓冲区偏移（0x8000）和段（0），偏移 8-15 是 64 位 LBA 起始地址（低 32 位=1，高 32 位=0）。注意偏移 1（保留字节）没有显式写入——因为 0x7B00 处的内存在此前没有被写入过，BIOS POST 后的低地址内存区域通常为零，所以这个字节自然是 0。不过更严谨的做法是显式写 0。

`INT 0x13 AH=0x42` 是 BIOS 扩展磁盘读取。如果成功，CF（进位标志）清零，Stage2 已经在 0x8000 了。如果失败，CF=1，跳转到 `disk_error` 打印错误信息然后死循环。

### 跳转到 Stage2——远跳转的段地址计算

```asm
    ljmp $STAGE2_LOAD_ADDR >> 4, $0
```

`STAGE2_LOAD_ADDR >> 4` = `0x8000 >> 4` = `0x0800`。这个远跳转设置 CS=0x0800，IP=0。实模式下实际物理地址 = 0x0800 × 16 + 0 = 0x8000，正好是 Stage2 被加载的位置。注意这里不是 `ljmp $0, $0x8000`——如果那样写，CS=0，Stage2 里用 DS:SI 访问字符串时，DS 必须设为 0，那字符串的链接地址就得包含 0x8000 的偏移。而使用 CS=0x0800 的好处是 Stage2 的链接脚本可以从地址 0 开始，所有偏移都是段内偏移，更简洁。

### MBR 签名——最后的两个字节

```asm
.org 510
.word 0xAA55
```

`.org 510` 让当前位置跳到偏移 510，中间如果有空隙用 0 填充。`.word 0xAA55` 在小端序下写入 `55 AA`——这正是 BIOS 检查的 MBR 签名。没有这两个字节，BIOS 拒绝执行。

## 设计决策

### 决策：MBR 自带 print_string 而非链接共享函数

**问题**：MBR 需要打印启动消息和错误信息，common/serial.S 里已经有 print_string 了。

**本项目的做法**：MBR 内部定义了一个精简版的 `print_string_mbr`，不链接 common/serial.S。

**备选方案**：让 MBR 也链接 boot_common 目标文件。

**为什么不选备选方案**：512 字节硬限制。链接 common/serial.S 会把里面的所有函数（print_string、panic、enable_a20、四个 VESA 函数）和所有 `.data` 段数据都拉进来，.text 段轻松超过 512 字节。BIOS 只加载第一个扇区，超出的部分不会被读入内存——跳过去就是执行垃圾数据。实测中，作者就是在这里踩了"MBR 超 512B"的大坑，print 正常但 call 之后飞到随机地址。

**如果要扩展**：如果 MBR 的功能增长到快逼近 512 字节，可以考虑把 print_string_mbr 做得更精简（比如不处理 \r\n），或者压缩启动消息的长度。也可以考虑用 `ljmp` 直接跳到 Stage2 不打印任何东西（最激进的方案）。

### 决策：栈底设在 0x7000 而不是 0x7C00

**问题**：MBR 代码在 0x7C00，栈应该放在哪里？

**本项目的做法**：栈底 0x7000，向下增长。

**备选方案**：栈底 0x7C00（和 MBR 代码重叠，向下增长不会碰到代码）。

**为什么不选备选方案**：0x7C00 向下增长确实不会碰到代码（栈往低地址长，代码往高地址长），但 0x7B00 处我们放了 DAP 结构。如果栈和 DAP 在同一个区域，BIOS 中断处理时 push 操作可能覆盖 DAP。0x7000 离 DAP（0x7B00）有 3KB 的安全距离，足够 BIOS 和我们的函数使用。作者在笔记里记录了"栈踩内存"这个坑——栈放在 0x7B00 附近，BIOS 用栈后 DAP 被覆盖，读盘数据写到了错误的位置。

## 扩展方向

- **添加分区表解析**（难度：⭐⭐）——在 MBR 中读取分区表，找到活动分区并加载其 VBR，实现更接近标准 MBR 的引导流程。
- **添加 CHS 回退读盘**（难度：⭐）——有些老旧 BIOS 不支持 INT 0x13 AH=0x42，可以用 AH=0x02 的 CHS 方式作为回退。
- **MBR 自重定位**（难度：⭐⭐⭐）——标准 MBR 会把自己从 0x7C00 搬到 0x0600，腾出 0x7C00 给 VBR。Cinux 不需要分区引导所以没做，但了解这个机制对理解 x86 启动链很有帮助。

## 参考资料

- Intel SDM: Vol.3A §9.1 — Processor State After Reset (CS=0xF000, EIP=0xFFF0, first instruction at 0xFFFF0)
- Intel SDM: Vol.1 §3.3.4 — Real-Address Mode (segment:offset addressing formula)
- OSDev Wiki: [MBR (x86)](https://wiki.osdev.org/MBR_(x86)) — MBR format, BIOS initial environment
- OSDev Wiki: [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) — DAP structure, extended read
