# 002-2 · 从实模式到保护模式

## 概览

上一篇我们把 GDT 定义好了、linker script 修正了、debugcon 配置到位了——整个舞台已经搭好，只差最后也是最关键的一幕：让 CPU 真正从实模式切换到保护模式。这一步涉及一系列必须严格按序执行的操作：关中断、清 DS、加载 GDTR、设置 CR0 的 PE 位、执行远跳转，然后在 32 位环境下初始化段寄存器和栈。整个过程容不得半点差错——任何一步的顺序或操作码出问题，等待你的都是经典的三重故障（Triple Fault），QEMU 窗口直接关闭，连报错的机会都不给你。

本文我们就逐行拆解 `stage2.S` 中从 `cli` 到 `hlt` 的完整保护模式切换代码，搞清楚每一步为什么必须存在、为什么在这个位置、如果省略或调换会发生什么。

## 流程图

保护模式切换是一个严格有序的过程，Intel SDM Vol.3A §9.9 给出了官方推荐的步骤。下面是 Cinux stage2 的完整切换流程：

```
  Real Mode (16-bit)
        │
        ▼
  ┌─────────────────────────────────┐
  │  cli   禁用中断                  │  ← IDT 还没设置，中断会触发 #GP
  └─────────────────────────────────┘
        │
        ▼
  ┌─────────────────────────────────┐
  │  DS = 0   清零数据段             │  ← lgdt 地址计算需要 DS*16+offset=offset
  └─────────────────────────────────┘
        │
        ▼
  ┌─────────────────────────────────┐
  │  lgdt gdt_ptr   加载 GDTR        │  ← CPU 记住 GDT 位置，不验证内容
  └─────────────────────────────────┘
        │
        ▼
  ┌─────────────────────────────────┐
  │  CR0.PE = 1   设置保护使能位     │  ← CPU "标记"自己准备进入保护模式
  └─────────────────────────────────┘
        │
        ▼
  ┌─────────────────────────────────┐
  │  ljmp $0x08, $pm_entry           │  ← 远跳转刷新 CS 隐藏缓存
  │  (far jump 不可省略!)            │     CPU 开始用 32 位解码
  └─────────────────────────────────┘
        │
        ▼
  ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─   保护模式分界线
        │
        ▼
  ┌─────────────────────────────────┐
  │  .code32 pm_entry                │
  │  DS/ES/FS/GS/SS = 0x10          │  ← 加载数据段选择子
  │  ESP = 0x90000                   │  ← 设置保护模式栈
  └─────────────────────────────────┘
        │
        ▼
  ┌─────────────────────────────────┐
  │  outb 'P', 0xE9                  │  ← debugcon 验证输出
  └─────────────────────────────────┘
        │
        ▼
  ┌─────────────────────────────────┐
  │  hlt + jmp .pm_halt              │  ← 死循环停机
  └─────────────────────────────────┘
        │
        ▼
  Protected Mode (32-bit)
```

这个流程图里的每一步都有不可省略的理由，我们接下来按照代码的实际顺序逐一拆解。

## 代码精讲

### Step 1: 禁用中断与 DS 清零

在 VESA 帧缓冲信息保存完毕之后（上一篇的内容），stage2 的实模式使命已经完成，接下来要进入保护模式了。第一步非常简单但绝对不能跳过：

```asm
    cli                           // disable interrupts again
    // ============================================================
    // Switch to Protected Mode
    // ============================================================
    // IMPORTANT: lgdt in .code32 uses absolute address, but in real mode
    // CPU calculates physical address as DS*16 + offset. We must set DS=0
    // so that the address calculation is correct: 0*16 + 0x8160 = 0x8160
    movw $0, %ax                  // Clear AX
    movw %ax, %ds                 // Set DS = 0 (required for lgdt in real mode)
```

`cli` 指令清除 EFLAGS 寄存器中的 IF（Interrupt Flag）位，让 CPU 不再响应可屏蔽硬件中断。为什么要在这里关中断？原因很直接：保护模式下的中断处理依赖 IDT（Interrupt Descriptor Table），而我们此时还没有设置 IDT。如果中断开着，任何一个硬件中断（定时器、键盘中断等）到达时，CPU 会尝试去 IDT 查找处理函数——但 IDT 还是一片空白，结果就是触发 General Protection Fault（#GP），而 #GP 本身也需要 IDT 来处理，于是又触发 Double Fault，Double Fault 还是找不到处理函数，最终变成 Triple Fault，CPU 直接复位，QEMU 窗口闪一下就没了。

`cli` 在 stage2 一开始就执行过一次（在 `_start` 入口处），后来 `sti` 重新开了中断给 VESA 的 BIOS 调用用。现在 VESA 操作全部结束，是时候再次关掉了。

紧接着 `cli` 之后，我们把 `%ds` 清零。这个操作和 `lgdt` 指令的地址计算方式直接相关。在实模式下，CPU 计算内存地址的公式是 `物理地址 = 段寄存器 × 16 + 偏移`。当我们写 `lgdt gdt_ptr` 时，`gdt_ptr` 是一个链接器算出的绝对地址（比如 `0x8160`），CPU 会用 `DS × 16 + 0x8160` 来计算实际要读的内存位置。如果此时 `DS` 还是之前 VESA 操作时设的某个非零值（比如 `0x600`，因为 VESA 缓冲区在 `0x6000` 附近），那实际读到的地址就变成了 `0x600 × 16 + 0x8160 = 0xE160`——完全不是 `gdt_ptr` 的真正位置，读出来的 GDTR 内容是垃圾数据。

把 `DS` 清零后，地址计算变成 `0 × 16 + 0x8160 = 0x8160`，正好是 `gdt_ptr` 的真实物理地址。说实话，这个坑在调试时真的让人血压拉满——`lgdt` 本身不会报错，CPU 很乖地把 GDTR 加载了一个错误的 base 地址，然后 `ljmp` 时去 GDT 查描述符查到垃圾数据，直接三重故障，你甚至不知道是 `lgdt` 那一步出了问题还是后面的步骤。

### Step 2: 加载 GDT (lgdt)

DS 清零之后，现在可以安全地执行 `lgdt` 了：

```asm
    // 1. Load GDT pointer (with DS=0, physical address = 0x8160)
    lgdt gdt_ptr                  // Load GDTR from absolute address
```

就一行，但背后的 CPU 行为值得展开说说。`lgdt` 指令做的事情是把 `gdt_ptr` 指向的 6 字节数据加载到 CPU 内部的 GDTR（Global Descriptor Table Register）。GDTR 是一个 48 位寄存器，由两部分组成：16 位的 Limit（GDT 的大小减 1）和 32 位的 Base（GDT 的线性基地址）。

在上一篇里我们定义了 `gdt_ptr`：

```asm
gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1 = 23 (3 descriptors * 8 - 1)
    .long gdt                      // Linear address of GDT base (32-bit)
```

执行 `lgdt gdt_ptr` 后，CPU 内部的 GDTR 变成了 `Limit = 0x17`（23）、`Base = gdt 的链接地址`（大约 `0x8140` 左右，取决于 `.text` section 的大小）。但请注意一个非常重要的细节：`lgdt` **只是把数据搬进了 GDTR，它不会验证 GDT 内容的正确性**。如果 GDT 里的描述符是乱写的——比如 Code 段的 Access Byte 写成 `0x00`——`lgdt` 不会报错。错误会在后续使用段选择子访问 GDT 条目时才暴露：`ljmp` 尝试加载 CS 时 CPU 去查描述符，发现 Present 位是 0，直接触发 #GP。

这意味着如果你的 GDT 定义有误（比如字段写反了、Limit 不够大、Access Byte 拼错了），`lgdt` 这一步是看不出问题的，你只会看到 CPU 在执行 `ljmp` 时突然三重故障，然后需要回头去检查 GDT 的每个字节是否正确——我们确实踩过这个坑，调试了半天才发现是 Access Byte 写错了一位。

### Step 3: 设置 CR0.PE 位

GDTR 就绪，接下来是真正"拨动开关"的一步：

```asm
    // 2. Set PE bit in CR0
    movl %cr0, %eax               // Read CR0
    orb $0x1, %al                 // Set PE bit (bit 0)
    movl %eax, %cr0               // Write CR0
```

CR0 是 x86 的控制寄存器之一，其中包含了多个控制系统级行为的标志位。我们关心的只有一个：bit 0，也叫 PE（Protection Enable）位。把它从 0 设成 1，CPU 就进入了保护模式——至少理论上是这样。

CR0 寄存器的关键位布局如下：

```
CR0 (32-bit Control Register)
Bit 31    Bit 30    Bit 29         Bit 0
  PG        CD        NW     ...     PE
 (Paging) (Cache)  (NoWrite)      (Protection
                                 Enable)
```

PE 位是最低位（bit 0），值 `0x1`。为什么我们用 `orb $0x1, %al` 而不是 `orl $0x1, %eax`？其实两种写法效果一样——都是把 bit 0 设成 1、其余位不变。用 `orb` 操作 `%al`（CR0 的低 8 位）只是编码更紧凑，省一个字节。这里的 `orb` 对应的机器码是 `0C 01`（两个字节），而 `orl $0x1, %eax` 的机器码是 `83 C8 01`（三个字节）。在 Bootloader 里省一个字节感觉有点鸡肋，但这是一个好习惯——养成只改你需要的位的习惯，避免无意中改动其他控制位。

你可能注意到了，`movl %cr0, %eax` 不能直接 `or` 再 `movl %eax, %cr0`——必须先读出来、修改、再写回去。这是因为 CR0 不能作为 ALU 指令的操作数，必须通过通用寄存器中转。这是 x86 的规定，没有什么高深的原因，就是 ISA 的限制。

有一个非常微妙的点需要特别说明：执行 `movl %eax, %cr0` 把 PE 位设成 1 之后，CPU 并没有立刻变成保护模式——至少从指令流水的角度来看是这样。此时 CPU 的预取队列里可能还缓存着按 16 位解码的指令（因为我们一直都在 `.code16` 下），CS 寄存器的隐藏部分（Cache）里存的还是实模式的段基址和属性。要让保护模式真正生效，必须做下一步：远跳转。

### Step 4: 远跳转 (ljmp)

这是整个切换流程中最不可省略的一步，也是最容易被忽略的一步：

```asm
    // 3. Far jump - manually construct for correct 16-bit encoding
    // ljmp $0x08, $pm_entry in 16-bit format: ea <offset16> <seg16>
    ljmp $0x08, $pm_entry
```

`ljmp`（Long Jump，也叫 Far Jump）和普通的 `jmp` 不同——它同时修改 CS 和 EIP，而不是只修改 IP。它的操作数由两部分组成：一个段选择子（`$0x08`）和一个目标偏移（`$pm_entry`）。

为什么这个远跳转绝对不可省略？原因在于 CS 寄存器的工作方式。x86 的段寄存器（CS、DS、ES 等）不是简单的 16 位寄存器——每个段寄存器内部都有一个"隐藏部分"（Hidden Part，也叫 Segment Cache），存储着当前段的 Base Address、Limit 和 Access Rights。在实模式下，CS 的隐藏部分存的是实模式的段属性（Base = CS × 16，Limit = 0xFFFF，16 位默认操作数大小）。当我们在 Step 3 中设置了 CR0.PE = 1 之后，CPU 的"模式"已经变成了保护模式，但 CS 的隐藏缓存还是实模式的旧值——CPU 仍然在用旧的 Base 和旧的属性来翻译地址和解码指令。

远跳转 `ljmp $0x08, $pm_entry` 做了两件事：第一，它把 CS 加载为新的段选择子 `0x08`，CPU 会用 `0x08` 去查 GDTR 指向的 GDT，找到索引 1（`0x08 >> 3 = 1`）的描述符，也就是我们定义的 `gdt_code`——一个 Base=0、Limit=4GB、32 位代码段的描述符。第二，它把目标地址 `pm_entry` 加载到 EIP 中，同时 CPU 用新描述符的属性刷新了 CS 的隐藏缓存。这意味着从此刻起，CPU 开始按 32 位模式解码和执行指令。

如果你省掉了这个远跳转，直接在 `movl %eax, %cr0` 后面继续写 32 位指令，CPU 的预取队列里还残留着按 16 位模式解码的指令，而新写的指令又是按 32 位编码的——两种编码混在一起，CPU 解析出完全错误的指令，大概率直接执行到非法操作码，然后三重故障。这个坑非常经典，Intel SDM Vol.3A §9.9 明确要求：设置 PE 位之后必须紧跟一个远跳转操作。

选择子 `0x08` 的含义是：`Index = 0x08 >> 3 = 1`（GDT 中的第二个条目，即 `gdt_code`），`TI = 0`（使用 GDT 而不是 LDT），`RPL = 0`（请求特权级为 Ring 0）。这个选择子正好指向我们在上一篇中定义的 32 位代码段描述符。

这里有一个关于指令编码的细节值得一提。代码注释里写了 `ljmp $0x08, $pm_entry in 16-bit format: ea <offset16> <seg16>`。因为我们此时还在 `.code16` 模式下，GAS 会把 `ljmp` 编码为 16 位远跳转——操作码 `EA` 后面跟 2 字节偏移 + 2 字节段选择子，总共 5 字节。但在保护模式下，`ljmp` 的编码是 `EA` 后面跟 4 字节偏移 + 2 字节段选择子，总共 7 字节。所以这里的 `pm_entry` 偏移会被截断为 16 位——这没问题，因为 stage2 的代码全部在 `0x8000`-`0x10000` 范围内，16 位偏移足够表示。

远跳转之后，代码执行流进入 `pm_entry`，同时 `.code32` 指令告诉汇编器从这开始生成 32 位指令编码——CPU 和汇编器终于同步了。

### Step 5: 保护模式段寄存器初始化

远跳转成功后，CPU 已经在保护模式下运行了。但此时的段寄存器（DS、ES、FS、GS、SS）还残留着实模式的值，需要全部重载为保护模式的段选择子：

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
```

`.code32` 指令切换汇编器的输出模式——从这行开始，所有指令按 32 位编码生成。注意 `.code32` 不是一个 CPU 指令，它不会出现在最终的二进制文件中；它只是告诉 GAS "接下来请用 32 位操作码编码"。真正让 CPU 切换到 32 位执行的是上一步的 `ljmp` 刷新了 CS 的隐藏缓存。两者必须配合：`ljmp` 让 CPU 用 32 位解码，`.code32` 让汇编器生成 32 位编码，缺一不可。

接下来我们把 DS、ES、FS、GS、SS 全部设成 `0x10`。这个选择子的含义和 `0x08` 类似：`Index = 0x10 >> 3 = 2`（GDT 中的第三个条目，即 `gdt_data`），`TI = 0`（使用 GDT），`RPL = 0`（Ring 0）。它指向我们定义的 32 位数据段描述符——Base=0、Limit=4GB、可读写。

为什么需要给每个段寄存器都加载一次？因为段寄存器的"隐藏缓存"不会因为你设置了 CR0.PE 就自动更新。在远跳转时我们只刷新了 CS，其余的段寄存器（DS、ES、FS、GS、SS）还保持着实模式的旧缓存。在保护模式下，任何通过这些段寄存器的内存访问都会使用隐藏缓存中的 Base 和 Limit 来翻译地址——如果缓存里还是实模式的旧值，内存访问就会出错。逐一重载每个段寄存器，CPU 就会用新的选择子去 GDT 查对应的描述符，然后把正确的 Base、Limit、Access Rights 写入隐藏缓存。

你可能会注意到我们没有给 CS 再做一次 `movw`——因为远跳转 `ljmp $0x08, $pm_entry` 已经完成了 CS 的重载。DS、ES、FS、GS 这些数据段寄存器不能通过远跳转来刷新，必须用 `movw` 直接写入。

### Step 6: 栈设置

段寄存器全部就位后，接下来设置保护模式下的栈：

```asm
    // 5. Set up new stack
    movl $0x90000, %esp           // New stack in protected mode
```

`ESP` 设为 `0x90000`，意味着栈从 `0x90000` 向低地址方向增长。为什么选这个地址？几个原因：第一，`0x90000` 在实模式可寻址范围内（1MB 以内），保护模式切换刚完成时 CPU 还没启用分页，访问的仍然是物理地址，用这个地址不会出问题。第二，它远离 stage2 的代码区域（`0x8000`-`0x10000`）和 VESA 缓冲区（`0x6000`-`0x64FF`），不会冲突。第三，它离 640KB 的"常规内存"上限还有一段距离，给栈留了足够的增长空间——从 `0x90000` 向下增长到 `0x80000` 有整整 64KB，对 Bootloader 阶段来说绰绰有余。

在实模式阶段，我们用的是 `SS:SP = 0x9000:0xFFFE`，对应物理地址 `0x9000 × 16 + 0xFFFE = 0x9FFFE`。进入保护模式后，段寄存器的语义变了——SS 不再存储段地址，而是存储段选择子，所以我们重新把 SS 设成 `0x10`（数据段选择子），然后把 ESP 直接设成 `0x90000`。在 Flat Memory Model 下，SS 对应的数据段 Base 是 `0x00000000`，所以 `0x90000` 就是实际的物理栈顶地址。

### Step 7: Debugcon 验证输出

所有初始化工作完成，现在用一个最简单的操作来验证保护模式确实已经正常工作了：

```asm
    // 6. Debug output: 'P' for Protected Mode
    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output
```

`0x50` 是字符 `P` 的 ASCII 码。`outb` 指令把 `%al` 的值写到 I/O 端口 `0xE9`。这个端口是 QEMU 的 debug console——在 `cmake/qemu.cmake` 里我们已经配置了 `-debugcon file:debug.log -global isa-debugcon.iobase=0xe9`，所以写入 `0xE9` 的字节会被 QEMU 捕获并写入 `debug.log` 文件。

你可能会问：为什么不继续用之前的 `INT 0x10` BIOS 中断来输出字符？原因很简单——保护模式下 BIOS 中断不可用。BIOS 中断服务程序运行在实模式下，使用实模式的段地址翻译方式。进入保护模式后，CPU 使用 GDT 中的描述符进行地址翻译，中断处理方式也完全变了（需要 IDT 而不是 IVT）。如果我们强行 `int $0x10`，CPU 会尝试在保护模式下处理这个中断——没有 IDT，结果就是 #GP，然后三重故障。所以我们不能再用任何 BIOS 服务，必须使用纯硬件 I/O 操作来输出调试信息。

debugcon 是最简单的选择：一条 `outb` 就够了，不需要初始化 UART、不需要设置波特率、不需要查询发送缓冲区是否为空。当然，debugcon 是 QEMU 专有的调试接口，真实硬件上不存在——但这在 Bootloader 开发阶段完全够用，后续进入 long mode 之后我们会切换到 COM1 串口作为正式的输出通道。

如果一切顺利，执行完 `outb %al, $0xE9` 之后，`debug.log` 文件里会出现一个 `P`。这个孤零零的字符就是我们的胜利旗帜——它意味着从 `cli` 到 `outb` 的整个保护模式切换流程全部正确执行，CPU 现在运行在 32 位保护模式下，段寄存器、栈、I/O 操作全部正常。

### Step 8: 停机循环

验证输出完成后，stage2 的保护模式阶段任务就结束了。在当前的 tag 002 里，我们还没有实现后续的 long mode 切换，所以直接进入停机：

```asm
    // 7. Infinite halt (we're in protected mode now)
    cli
.pm_halt:
    hlt
    jmp .pm_halt
```

`cli` 再次确认中断关闭——虽然前面已经 `cli` 过了，但这是防御性编程，确保万无一失。`hlt` 指令让 CPU 进入 halt 状态，停止执行直到下一个外部中断到来。但因为中断已经关了，所以 `hlt` 会一直停住——直到 NMI（不可屏蔽中断）或者系统复位。不过 NMI 在正常情况下不会发生，所以 CPU 就停在这了。下面的 `jmp .pm_halt` 是额外的保险：万一 CPU 因为某种原因从 `hlt` 唤醒了（比如某些虚拟化场景），就跳回来再 `hlt` 一次，形成死循环。

## 设计决策

### 决策：栈地址选择 0x90000

**问题**：保护模式下的栈放在哪个地址？

**本项目的做法**：`ESP = 0x90000`，栈从 `0x90000` 向下增长到 `0x80000`，有 64KB 空间。

**备选方案**：xv6 使用 `0x7C00` 作为栈顶——即 MBR 的加载地址，栈从 `0x7C00` 向下增长到 `0x0000`。这个做法更"节约"内存，但只有 31KB 的栈空间，且栈和 BIOS 数据区（`0x400`-`0x4FF`）以及中断向量表（`0x0000`-`0x03FF`）靠得很近，稍不留神就会覆盖。

**为什么选 0x90000**：在 Bootloader 阶段的内存布局中，`0x8000`-`0x10000` 是 stage2 代码区，`0x6000`-`0x64FF` 是 VESA 缓冲区，`0x5000`-`0x5FFF` 是后续的 E820 内存映射。`0x90000` 在这些区域之上，有足够的间隔，64KB 的空间对 Bootloader 阶段来说非常宽裕。更重要的是，这个地址在实模式可寻址范围内（低于 1MB），保护模式切换刚完成时还没有分页，直接用物理地址即可。

### 决策：使用 debugcon 而非串口

**问题**：保护模式下如何输出验证信息？

**本项目的做法**：QEMU debugcon 端口 `0xE9`，一条 `outb` 即可。

**备选方案**：初始化 COM1 串口（I/O 端口 `0x3F8`），需要设置波特率除数、中断使能、FIFO 控制等多个寄存器，大约需要 5-8 条 I/O 操作。

**为什么选 debugcon**：在 tag 002 阶段，我们只验证"保护模式切换是否成功"，一个字节就够了。Debugcon 零配置——不需要初始化 UART、不需要等待发送就绪、不需要处理中断，直接写端口就完事。真正的串口初始化会留到 tag 003 进入 long mode 之后，届时我们需要一个更完善的输出通道来打印内核启动日志。

当然，debugcon 的局限在于它是 QEMU 专有的，Bochs 仿真器也支持但真实硬件上不存在。不过对于 Bootloader 开发来说，我们几乎 100% 时间都在 QEMU 里调试，这个局限完全可接受。

## 扩展方向

**1. 保护模式下的串口输出**（中等难度）—— 当前我们只用 debugcon 输出了一个字节。尝试在保护模式下初始化 COM1 串口（8-N-1, 115200 baud），实现一个 `printf` 风格的输出函数。关键步骤是：设置波特率除数（先写 `0x80` 到 LCR 寄存器打开 DLAB，然后写 DLL 和 DLH），配置 Line Control Register（8 位数据、无校验、1 位停止位），然后通过查询 LSR 寄存器的 THRE 位来逐字符发送。这将让你在真实硬件上也能看到调试输出。

**2. 保护模式下的屏幕输出（Framebuffer 写字）**（较高难度）—— 我们已经通过 VESA 设置了 `1024×768×32` 的图形模式，帧缓冲区的物理地址保存在 `0x6400`。进入保护模式后，可以尝试直接往帧缓冲区写像素数据来显示文字——你需要一个位图字体（可以从 Linux 内核的 `fonts/` 目录找一个 8×16 的 console font），然后在帧缓冲区上逐像素绘制字符。这个练习能让你深入理解"图形模式没有文字输出"意味着什么。

**3. 多段模型（Non-Flat GDT）**（进阶）—— 当前的 Flat Model 把代码段和数据段都设成 Base=0、Limit=4GB。作为学习练习，尝试定义不同的 Base 和 Limit 来创建隔离的内存区域——比如把代码段限制在 `0x8000`-`0x10000`，数据段限制在 `0x10000`-`0x90000`，看看 CPU 如何通过段级保护阻止越界访问。虽然现代 OS 不使用这种方式（全部交给分页），但理解分段保护的工作原理对理解 x86 架构的历史和设计哲学非常有帮助。

## 参考资料

- Intel SDM Vol.3A §9.9 — Switching to Protected Mode：完整的 6 步切换流程（CLI、LGDT、CR0.PE、Far Jump、段寄存器重载、第一条保护模式指令）
- Intel SDM Vol.3A §10.1 — Processor Initialization：CR0.PE 位定义，处理器状态转换
- Intel SDM Vol.3A §2.4.1 — Global Descriptor Table Register (GDTR)：`lgdt` 指令的行为和 GDTR 寄存器格式
- OSDev Wiki: [Protected Mode](https://wiki.osdev.org/Protected_Mode) — 进入保护模式的步骤和常见陷阱
- xv6 bootasm.S: [https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S](https://github.com/mit-pdos/xv6-public/blob/master/bootasm.S) — xv6 的保护模式切换实现，流程与 Cinux 几乎一致
