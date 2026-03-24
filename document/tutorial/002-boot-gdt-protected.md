# 从实模式到保护模式：x86 GDT 构建与模式切换完整踩坑记录

> 标签：x86, 保护模式, GDT, bootloader, GNU AS, AT&T 汇编, QEMU, 裸机开发

---

## 本章概览

上一章我们花了大量时间在实模式的泥潭里打滚：MBR 512 字节限制、段寄存器不匹配、栈踩内存、BIOS 寄存器污染等等。当时你以为踩完这些坑就能解脱了，但现实是更残酷的 —— 实模式只是开始，接下来要面对的是 x86 架构的第一个真正转折点：从 16 位实模式切换到 32 位保护模式。

为什么说这是转折点？在实模式下，你的 CPU 就像一辆被限速的跑车，只能用 16 位寄存器，内存访问被限制在 1MB 以内，没有任何保护机制。任何程序都可以随意读写任意内存地址，没有权限检查，没有内存隔离 —— 这在单任务 DOS 时代还能凑合，但一旦要跑多任务操作系统就是灾难。保护模式引入了段描述符、特权级、分页机制，这才是现代操作系统的起点。

这一章的目标看似简单：构建 GDT（全局描述符表），完成从实模式到保护模式的切换，然后在 QEMU 的 debugcon 终端输出一个孤零零的字母 `P`。但你很快会发现，这个简单的字母背后藏着无数个可以让 CPU 直接跑飞的坑点。说实话，写这一章的时候我连续熬了几个晚上，GDB 单步跟到凌晨，对着 objdump 的输出反复比对，才把每一个细节都搞明白。

**关键设计决策一览**：

- 使用 GDT（全局描述符表）定义代码段和数据段，而非继续使用实模式的段地址
- GDT 放置在独立的 `.gdt` 段（allocatable），确保 8 字节对齐
- 通过 `lgdt` 指令加载 GDTR，在实模式下必须先清零 DS 确保地址正确
- 设置 CR0 的 PE 位后必须用远跳转刷新 CS，否则 CPU 不会真正进入保护模式
- 使用 QEMU 的 debugcon 端口（0xE9）输出调试字符，避免复杂的串口初始化
- 保护模式下使用 `.code32` 显式声明，数据段寄存器全部更新为数据段选择子

与同类 OS 项目的设计对比：Linux 早期的 setup.S 使用了类似的 GDT 结构和模式切换流程，但 Linux 直接在 setup.S 中完成切换，而 Cinux 将这部分逻辑放在 Stage2 中，保持了更清晰的模块划分。xv6 则跳过了这些细节，直接使用 GRUB 进入保护模式。

---

## 架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        实模式 → 保护模式切换流程                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  实模式 (Real Mode)                                                     │
│  │                                                                      │
│  ├─ 16 位寄存器和寻址                                                   │
│  ├─ 段地址:偏移 地址计算                                                │
│  └─ 1MB 内存限制                                                       │
│                                                                         │
│  切换步骤（顺序严格）：                                                  │
│  │                                                                      │
│  1. cli                    关闭中断（保护模式下 IDT 未设置）            │
│  2. movw $0, %ds           清零 DS（确保 lgdt 地址正确）                │
│  3. lgdt gdt_ptr           加载 GDT                                     │
│  4. movl %cr0, %eax        读取 CR0                                     │
│  5. orb $0x1, %al          设置 PE 位（注意是 orb 不是 orl）            │
│  6. movl %eax, %cr0        写回 CR0                                     │
│  7. ljmp $0x08, $pm_entry  远跳转刷新 CS                                │
│                                                                         │
│  保护模式 (Protected Mode)                                              │
│  │                                                                      │
│  ├─ 32 位寄存器和寻址                                                   │
│  ├─ 段选择子 → GDT 描述符                                               │
│  └─ 4GB 内存可访问                                                     │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                           GDT 结构布局                                   │
├─────────────────────────────────────────────────────────────────────────┤
│  索引    选择子    描述符类型     说明                                   │
│  ────    ──────    ─────────     ────                                   │
│  0       0x00      Null          空描述符（必须存在，Intel 硬件要求）     │
│  1       0x08      Code          32 位代码段，可执行可读                 │
│  2       0x10      Data          32 位数据段，可读写                     │
│                                                                         │
│  选择子格式：[Index 13bit][TI 1bit][RPL 2bit]                           │
│  例如 0x08 = 0000_1000b → Index=1, TI=0(GDT), RPL=0                     │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 环境说明

在开始踩坑之前，先说一下我的运行环境。这些细节对排查问题很重要，因为不同工具链版本生成的机器码可能有细微差异：

- **平台**：WSL2 + QEMU system-x86_64 6.2+
- **工具链**：GNU AS（AT&T 语法），ld 链接器
- **调试方式**：QEMU debugcon（端口 0xE9）写入 debug.log + GDB 远程调试
- **前置要求**：已完成上一章的 Real Mode 实现，Stage2 能正常加载并输出消息

这里有个关键点：我们用的是 GNU AS 的 AT&T 语法，和 NASM 的 Intel 语法有几个致命区别。操作数顺序是 `src, dest` 而不是 `dest, src`，寄存器要有 `%` 前缀，立即数要有 `$` 前缀。这些细节在写 16 位和 32 位混用代码时特别容易搞混，会导致汇编出来就炸。

调试方式这一章我们换了一种玩法。之前用 BIOS INT 0x10 屏幕输出，但进入图形模式后就失效了。这次我们用 QEMU 特有的 debugcon 端口（0xE9），直接往这个端口写字符就会输出到 debug.log 文件。这是最轻量的调试手段，比串口还简单，不需要初始化 UART。

看一下我们的 QEMU 配置（cmake/qemu.cmake）：

```cmake
set(QEMU_DEBUG_FLAGS
    -s
    -S
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
)
```

`-debugcon file:debug.log` 把 debugcon 输出重定向到文件，`-global isa-debugcon.iobase=0xe9` 设置端口地址为 0xE9。`-s -S` 让 QEMU 启动时暂停等待 GDB 连接。

---

## 第一阶段 —— 理解 GDT：保护模式的"段定义手册"

在实模式下，段寄存器（CS、DS、ES 等）直接存储段地址。比如 `DS = 0x8000`，那么访问 `DS:0x10` 实际上是在访问物理地址 `0x8000 × 16 + 0x10 = 0x80010`。这种寻址方式简单粗暴，但没有任何保护机制。

保护模式下，段寄存器存储的是"段选择子"（Selector），这是一个索引，指向 GDT 中的某个描述符。每个段描述符占 8 字节，定义了段的基地址、大小和访问权限。

你可以把 GDT 理解为保护模式下的"段定义手册"。CPU 要访问内存时，不是直接用段值计算地址，而是先查 GDT，拿到描述符，再从描述符里读出基地址和权限信息。这样就能实现内存隔离和权限检查。

### 段描述符格式

每个段描述符占 8 字节，格式如下（这个格式真的够复杂，第一次看很容易晕）：

```
Base Address (32 bit)     +-------------------+
Limit (20 bit)            |                   |
                          |   Descriptor      |
Present, DPL, Type, etc.  |                   |
Granularity, 32-bit flag  +-------------------+
        Bytes: 0  1  2  3  4  5  6  7
```

具体字段分布：

- **Base Address（32 位）**：段的起始线性地址，分散在字节 0-1、2-3、4 的 bits 24-31
- **Limit（20 位）**：段的大小，分散在字节 0-1 的 bits 0-15 和字节 6 的 bits 0-3
- **Present（P）**：字节 5 的 bit 7，表示段是否在内存中
- **DPL（Descriptor Privilege Level）**：字节 5 的 bits 6-5，特权级 0-3
- **S（System）**：字节 5 的 bit 4，0=系统段，1=代码/数据段
- **Type**：字节 5 的 bits 3-0，段的类型（代码段、数据段、可读可写等）
- **Granularity（G）**：字节 6 的 bit 7，0=字节粒度，1=4KB 粒度
- **D/B**：字节 6 的 bit 6，0=16位段，1=32位段

说实话，第一次看到这个格式我是崩溃的。为什么 Intel 要把一个地址拆到三个不同的地方？后来才明白这是历史遗留问题，为了兼容早期的 286 保护模式。你只需要记住：用汇编器宏或者手动构造描述符时，按照规范填写就行。

### 我们需要定义的描述符

最小可用的 GDT 需要至少三个描述符：

1. **Null Descriptor**：索引 0，必须全 0，这是 Intel 硬件规定的，虽然用不上但必须占位
2. **Code Segment**：可执行的 32 位代码段，选择子 = 0x08
3. **Data Segment**：可读写的 32 位数据段，选择子 = 0x10

为什么选择子是 0x08 和 0x10？因为选择子 = Index × 8。索引 1 × 8 = 0x08，索引 2 × 8 = 0x10。这里 TI=0（使用 GDT），RPL=0（内核级权限）。

---

## 第二阶段 —— 定义 GDT：8 字节对齐的坑

现在我们开始写代码。第一步是在 stage2.S 中定义 GDT。

### 代码实现

```asm
// ============================================================
// GDT (Global Descriptor Table) - in .gdt section
// ============================================================
.section .gdt,"a"
.align 8                          // Align to 8 bytes (2^3)

gdt:
gdt_null:
    .quad 0                        // Null descriptor (required)

gdt_code:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0
    .byte 0x00                     // Base 23:16
    .byte 0x9A                     // Present, DPL=0, Code, Executable, Readable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_data:
    .word 0xFFFF                   // Limit 15:0
    .word 0x0000                   // Base 15:0
    .byte 0x00                     // Base 23:16
    .byte 0x92                     // Present, DPL=0, Data, Writable
    .byte 0xCF                     // Granularity=4KB, 32-bit, Limit 19:16=0xF
    .byte 0x00                     // Base 31:24

gdt_end:
```

### 关键细节解释

这里我们定义了一个最小可用的 GDT。第一个描述符必须全零 —— 这是 Intel 硬件规定的，虽然用不上但必须占位。第二个是代码段，第三个是数据段。

重点说一下这些魔法数字的含义。`0x9A` 这个字节包含多个字段：

- `P=1`（Present）：表示这个段在内存中
- `DPL=0`（Descriptor Privilege Level）：内核级权限
- `S=1`（System）：表示这是代码/数据段，不是系统段
- `Type=1010`：代码段，可执行，可读

`0xCF` 这个字节：

- `G=1`（Granularity）：段界限以 4KB 为单位，所以 0xFFFFF 实际代表 4GB
- `D=1`（Default size）：32 位代码段
- `Limit[19:16]=0xF`：配合 Limit[15:0] 组成完整的 20 位段界限

### 坑 1：GDT 放置位置错误

我一开始把 GDT 放在 `.text` 段，结果 `lgdt` 总是加载错误的地址。后来才意识到：GDT 必须放在独立的段，并且 8 字节对齐。

正确的做法是放在 `.gdt` 段（allocatable），并用 `.align 8` 确保对齐。`.section .gdt,"a"` 中的 `"a"` 表示这个段是 allocatable 的，链接器会正确处理它。`lgdt` 指令对 GDT 的位置很敏感，如果对齐错了，读出来的内容就完全是垃圾。

注意我们用的是 `.quad 0` 而不是 `.long 0, 0` 来定义空描述符。`.quad` 直接生成 8 字节的零，更简洁。

### 坑 2：忘记预留空描述符

我一开始直接定义代码段作为第一个描述符，结果选择子应该是 0x00 而不是 0x08。但这样违反了 Intel 规范 —— 索引 0 必须是空描述符。很多资料说这是为了捕获空指针引用，如果一个程序错误地用 0 作为段选择子，CPU 会触发异常。

所以第一个描述符必须全零，虽然浪费 8 字节，但这是硬件规定，改不了。

---

## 第三阶段 —— GDT 指针：lgdt 指令的"伪描述符"

定义完 GDT 后，还需要告诉 CPU GDT 在哪里。`lgdt` 指令需要一个特殊的内存结构：伪描述符（Pseudo Descriptor）。

### 代码实现

```asm
gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1 = 23
    .long gdt                      // Linear address of GDT base
```

### 为什么需要这个结构？

`lgdt` 指令需要一个 6 字节的内存结构：前 2 字节是 GDT 的长度（limit - 1），后 4 字节是 GDT 的线性地址。这个结构就是 GDTR（Global Descriptor Table Register）的加载格式。

我们用了一个小技巧：`gdt_end - gdt - 1` 自动计算出 GDT 的大小。对于我们的 GDT（3 个描述符），大小是 24 字节，limit 是 23。这样以后添加描述符时不需要手动改数字，链接器会自动计算。

### 坑 3：GDT 基址计算错误

这里有个超级隐蔽的坑。我们写的是 `.long gdt`，链接器生成的是符号 `gdt` 的地址值。由于我们的链接器配置，这个地址是正确的相对地址。

但这里有个关键点：在实模式下，`lgdt` 指令执行时，CPU 会用 `DS × 16 + offset` 计算物理地址。如果 DS 不是 0，即使 `gdt_ptr` 中的值是正确的，CPU 也会读错位置。

所以我们必须在 `lgdt` 之前清零 DS：

```asm
movw $0, %ds
lgdt gdt_ptr
```

这样地址计算就是 `0 × 16 + gdt_ptr = gdt_ptr`，正确指向我们的 GDT 指针。

---

## 第四阶段 —— 加载 GDT：实模式下的地址陷阱

现在我们有了 GDT 和 GDT 指针，接下来是在 16 位实模式下加载 GDT。这部分代码在 stage2.S 的 `_start` 函数末尾，VESA 初始化之后。

### 代码实现

```asm
    cli                           // close the inter again
    // ============================================================
    // Switch to Protected Mode
    // ============================================================
    // IMPORTANT: lgdt in .code32 uses absolute address, but in real mode
    // CPU calculates physical address as DS*16 + offset. We must set DS=0
    // so that the address calculation is correct: 0*16 + offset = offset
    movw $0, %ax                  // Clear AX
    movw %ax, %ds                 // Set DS = 0 (required for lgdt in real mode)

    // 1. Load GDT pointer (with DS=0, physical address = offset)
    lgdt gdt_ptr                  // Load GDTR from absolute address

    // 2. Set PE bit in CR0
    movl %cr0, %eax               // Read CR0
    orb $0x1, %al                 // Set PE bit (bit 0)
    movl %eax, %cr0               // Write CR0

    // 3. Far jump - manually construct for correct 16-bit encoding
    // ljmp $0x08, $pm_entry in 16-bit format: ea <offset16> <seg16>
    ljmp $0x08, $pm_entry
```

### 坑 4：lgdt 在实模式下的地址计算

这是我踩过最离谱的坑。`lgdt` 指令在 16 位模式下执行时，需要正确处理分段地址。

实模式下地址计算是 `物理地址 = DS × 16 + offset`。如果 DS 不是 0，`lgdt gdt_ptr` 会读到错误的地址。

举个例子：假设 `DS = 0x8000`，那么 `lgdt gdt_ptr` 实际上会从 `0x8000 × 16 + gdt_ptr` 读取，这完全不是我们想要的。

所以必须在 `lgdt` 之前清零 DS：

```asm
movw $0, %ds
lgdt gdt_ptr
```

这样地址计算就是 `0 × 16 + gdt_ptr = gdt_ptr`，正确指向我们的 GDT 指针。

### 坑 5：忘记关中断

我们在切换保护模式前加了 `cli` 关中断。这个很重要，因为保护模式下的中断处理需要 IDT（中断描述符表），而我们还没设置 IDT。

如果这时候发生中断（比如定时器中断或者键盘中断），CPU 会尝试从 IDT 读中断处理函数地址。但 IDT 没设置，CPU 就会触发 double fault，然后 triple fault，最后 QEMU 直接重启。

### 坑 6：设置 CR0 后不跳转

设置 CR0 的 PE 位后，CPU 已经进入保护模式了，但流水线里可能还预取了一些实模式的指令。如果不用远跳转强制刷新流水线，CPU 可能还会尝试用 16 位的方式解析后续指令，导致立即崩溃。

远跳转 `ljmp $0x08, $pm_entry` 做了两件事：
1. 跳转到 `pm_entry`
2. 同时加载新的 CS 值（0x08），这会刷新 CPU 的指令流水线

`0x08` 是代码段选择子：`index=1`（指向第二个描述符），`TI=0`（使用 GDT），`RPL=0`（内核级权限）。

### 坑 7：使用了错误的 OR 指令

注意我们用的是 `orb $0x1, %al` 而不是 `orl $0x1, %eax`。在 AT&T 语法中，`orb` 是字节操作，`orw` 是字操作，`orl` 是双字操作。

这里我们只需要修改 CR0 的最低位（PE 位），所以用 `orb` 操作 AL 就够了。虽然 `orl` 也能工作，但 `orb` 更精确，而且生成的机器码更短。

---

## 第五阶段 —— 32 位入口点：CS 刷新与段寄存器初始化

远跳转后，CPU 到了 `pm_entry`，这时候是真正的 32 位保护模式了。但还有几件事要做。

### 代码实现

```asm
// ============================================================
// Protected Mode Entry Point
// ============================================================
.code32                          // Now in 32-bit protected mode
pm_entry:
    // 4. Set up data segment registers
    movw $0x10, %ax               // Data selector value
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // 5. Set up new stack
    movl $0x90000, %esp           // New stack in protected mode

    // 6. Debug output: 'P' for Protected Mode
    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // 7. Infinite halt (we're in protected mode now)
    cli                             // Disable interrupts
.pm_halt:
    hlt
    jmp .pm_halt
```

### 坑 8：数据段寄存器没更新

进入保护模式后，CS 已经被远跳转刷新了，但 DS、ES、FS、GS、SS 还是实模式下的旧值。在保护模式下，这些段寄存器必须包含有效的段选择子，否则访问内存会触发异常。

我们把所有数据段寄存器都设置为 `0x10`（数据段选择子）。注意这里包括 SS（栈段），虽然栈的物理地址没变，但在保护模式下它必须通过段选择子访问。

### 坑 9：`.code16` 和 `.code32` 混乱

这里有个非常关键的细节：`.code16` 和 `.code32` 只是**汇编器指令**，不是 CPU 指令。它们告诉汇编器生成什么样的机器码，但不会改变 CPU 的状态。

CPU 的模式切换是通过 CR0.PE 位和远跳转完成的，不是通过汇编器指令。所以正确的做法是：

1. 在切换前用 `.code16`（`_start` 函数开头）
2. 进入保护模式后用 `.code32`（`pm_entry` 处）

如果反了，或者混用了，汇编器会生成错误的机器码。比如在 `.code32` 下生成 16 位指令，或者反过来。我之前踩过这个坑：GDB 反汇编显示 `(bad)`，说明机器码根本无法正确解码。

### 坑 10：栈指针设置错误

栈指针我们设在 `0x90000`。这个地址在实模式阶段我们用了 `0x9000:0xFFFE`，换算成线性地址就是 `0x90000 + 0xFFFE`。现在直接用 `0x90000` 作为栈顶是兼容的。

但这里有个坑：保护模式下栈必须通过段选择子访问。如果 SS 还保留着实模式的旧值，`push` 和 `pop` 会访问错误的地址，直接就炸了。

---

## 第六阶段 —— 调试验证：那个 'P' 终于出现了

代码写完了，现在来验证一下。如果一切顺利，QEMU 的 debugcon 终端应该输出一个 `P` 到 debug.log 文件，然后 CPU 进入停机循环。

### 编译和运行

```bash
# 检出本章对应的代码版本
git checkout 002_boot_gdt_protected

# 创建构建目录
mkdir -p build
cd build

# 配置 CMake（Debug 模式，带符号信息）
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..

# 编译
make

# 运行 QEMU（正常模式）
make run

# 或者运行 QEMU（调试模式，等待 GDB 连接）
make run-debug
```

### 验证输出

如果一切正常，`debug.log` 文件应该包含：

```
P
```

串口输出（stdio）应该包含：

```
Stage2 OK
Mode info OK, switching...
```

这个 `P` 就是我们的胜利标志。它意味着：

1. GDT 被正确加载到 GDTR
2. CR0.PE 位被设置
3. 远跳转成功刷新 CS
4. CPU 进入 32 位保护模式
5. 数据段寄存器被正确设置
6. CPU 能执行 32 位代码并访问 I/O 端口

### 用 GDB 验证 GDTR

如果看不到 `P`，或者想进一步验证，可以用 GDB：

```bash
# 终端 1：启动 QEMU 等待 GDB 连接
make run-debug

# 终端 2：启动 GDB
make run-gdb
```

或者手动连接：

```gdb
gdb build/boot/stage2
(gdb) target remote :1234
(gdb) break *pm_entry
(gdb) continue
(gdb) info registers gdtr
```

`info registers gdtr` 会显示 GDTR 寄存器的值，你应该看到类似：

```
gdtr    0x8000f0    0x17
```

`0x8000f0` 是 GDT 的基址（具体值取决于链接结果），`0x17`（23）是 limit。limit 应该是 GDT 大小减一（3 个描述符 × 8 字节 - 1 = 23）。

### 坑 11：ELF vs BIN 混用

调试时最容易踩的坑是混淆 ELF 和 BIN 文件。我们用 `objcopy -O binary` 生成 `stage2.bin` 给 QEMU 启动，但调试时应该用 ELF 文件（`stage2`），因为 ELF 包含符号信息。

如果用 `file build/boot/stage2.bin` 加载到 GDB，符号地址会完全错位。你可能会看到类似 `p/x pm_entry = 0x66` 这种离谱的输出，说明 GDB 把代码当数据解释了。

正确的做法是：

```gdb
file build/boot/stage2
```

只用 ELF，不要用 bin。BIN 是给 QEMU 启动的，ELF 是给调试用的。两个文件不能互相替代。

---

## 常见问题排查

### 问题 1：看不到 `P` 输出

可能原因：
1. GDT 地址计算错误：检查 DS 是否被清零
2. 远跳转目标错误：确认 `pm_entry` 符号被正确导出
3. CPU 在远跳转前就崩溃：可能是 `lgdt` 加载了错误地址

排查方法：用 GDB 检查 `%gdtr` 寄存器，确认 base 和 limit 正确。在 `lgdt` 指令后设置断点，单步执行。

### 问题 2：Triple Fault（QEMU 窗口直接关闭）

这通常是因为：
1. 进入保护模式后还没设置 IDT 就发生了中断
2. 段选择子的 TI 位或 RPL 位设置错误
3. 栈指针设置错误，导致栈操作时访问了非法内存

排查方法：在切换保护模式前加 `cli` 确保中断关闭，并逐步注释代码验证哪一步出问题。用 GDB 单步执行，观察每一步后 CPU 状态。

### 问题 3：GDB 反汇编显示 `(bad)`

这说明指令编码与 CPU 状态不匹配。通常是 `.code16` 和 `.code32` 混用导致的。

排查方法：检查代码中是否在正确的位置切换了 `.code16` 和 `.code32`。远跳转 `ljmp` 必须在 `.code16` 段，确保生成 16 位编码。`pm_entry` 必须在 `.code32` 段。

---

## 本章踩坑总结

写这一章的时候，我踩过的坑总结如下：

1. **GDT 放置位置错误**：必须放在 `.gdt` 段并 8 字节对齐，不能放在 `.text`
2. **lgdt 地址计算错误**：必须在实模式下清零 DS，否则地址计算 `DS×16+offset` 会读到错误位置
3. **忘记空描述符**：索引 0 必须是空描述符，这是 Intel 硬件规定
4. **忘记关中断**：进入保护模式前必须 `cli`，否则中断会导致 triple fault
5. **CR0 设置后不跳转**：必须用远跳转刷新 CS，否则 CPU 不会真正进入保护模式
6. **数据段寄存器没更新**：进入保护模式后必须设置所有数据段寄存器为数据段选择子
7. **`.code16` 和 `.code32` 混乱**：汇编器指令只影响生成的机器码，不改变 CPU 状态
8. **ELF vs BIN 混用**：调试用 ELF，启动用 BIN，不能互相替代
9. **使用了错误的 OR 指令**：`orb` 操作字节，`orl` 操作双字，这里用 `orb` 就够了
10. **栈指针设置错误**：保护模式下栈必须通过段选择子访问，SS 必须更新

这些坑每一个都足以让 CPU 直接跑飞，QEMU 窗口瞬间消失。写这一章的时候我真的是一边写一边调试，每一步都小心翼翼，生怕漏掉什么细节。

---

## 收尾

到这里，实模式到保护模式的切换就完成了。我们从一个只有 16 位寄存器、1MB 内存限制的荒岛，跳到了有 32 位寄存器、4GB 地址空间的广阔天地。虽然只输出了一个 `P`，但这个简单的字符背后，CPU 完成了从实模式到保护模式的切换，加载了 GDT，设置了段寄存器，每一步都不能出错。

说实话，写这一章的时候我踩了不少坑。最离谱的是 GDT 地址错误，`lgdt` 加载了错误的地址，CPU 直接 triple fault，QEMU 窗口瞬间消失。后来用 GDB 单步跟了半天，才发现 DS 寄存器没清零，导致地址计算完全错了。还有一个坑是 `.code16` 和 `.code32` 混用，汇编器生成了错误的机器码，GDB 反汇编全是 `(bad)`，看了半天文档才搞明白。

现在启动 QEMU，你应该能在串口看到：

```
Stage2 OK
Mode info OK, switching...
```

在 `debug.log` 文件看到：

```
P
```

这个 `P` 标志着保护模式切换成功。到这里就大功告成了，你可以用 GDB 验证一下 GDTR 寄存器，确认 GDT 被正确加载。下一步方向：启用分页机制，设置 PML4 表，切换到 64 位长模式。那又是另一堆坑等着我们去踩。

但至少现在，我们已经有了稳定的保护模式基础，可以开始写 32 位内核代码了。不用再手搓汇编，不用再担心 1MB 内存限制，不用再搞段地址:偏移这种别扭的寻址方式。保护模式，真香。

---

## 最重要三条认知（必须记住）

写从实模式到保护模式切换这件事，最核心的认知有三条：

**实模式下 lgdt 的地址计算是 `DS × 16 + offset`**：如果 DS 不是 0，`lgdt` 会读到错误的地址。必须在 `lgdt` 之前清零 DS，确保地址计算正确。

**`.code16/.code32` 只是汇编器指令，不是 CPU 状态**：CPU 的模式切换是通过 CR0.PE 位和远跳转完成的，汇编器指令只影响生成的机器码。混用会导致指令编码与 CPU 状态不匹配，直接崩溃。

**ELF 是给调试用的，BIN 是给启动用的**：两个文件不能互相替代。调试时必须用 ELF，启动时必须用 BIN。混用会导致符号错位，GDB 把代码当数据解释。

记住这三条，能帮你省掉很多调试时间。剩下的就是耐心，一遍遍调试，终会成功的。

---

## 参考资料

### Intel/AMD 手册
- Intel SDM Vol. 3A Chapter 9: Processor Management and Initialization
- Intel SDM Vol. 3A Chapter 3: Protected Mode Memory Management
- Intel SDM Vol. 2 Chapter 3: Instruction Set Reference, A-L（LGDT 指令）
- Intel SDM Vol. 2 Chapter 3: Instruction Set Reference, M-Z（MOV to CR0）

### OSDev Wiki
- https://wiki.osdev.org/Global_Descriptor_Table
- https://wiki.osdev.org/Protected_Mode
- https://wiki.osdev.org/Unreal_Mode
- https://wiki.osdev.org/Entering_Protected_Mode
- https://wiki.osdev.org/GDT_Tutorial

### 其他资源
- x86 Bootsectors: https://github.com/warengovernance/x86-bootsectors
- Writing a Simple Operating System from Scratch: https://www.cs.bham.ac.uk/~exr/lectures/opsys/10_11/lectures/os-dev.pdf
- The "GDT in C" OSDev Wiki Page: https://wiki.osdev.org/GDT_Tutorial#A_small_GDT_in_C

---

**文件路径汇总**：

- [boot/stage2.S](../../boot/stage2.S) - Stage2 主逻辑，包含 GDT 定义和模式切换代码
- [boot/common/serial.S](../../boot/common/serial.S) - 通用函数（print、A20、VESA）
- [cmake/qemu.cmake](../../cmake/qemu.cmake) - QEMU 启动参数配置，包含 debugcon 设置

下一章预告：`003_boot_paging_long` —— 我们将启用分页机制，设置 PML4 表，切换到 64 位长模式。那才是现代 x86-64 操作系统的真正起点。
