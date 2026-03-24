# 002 · GDT 与进入保护模式

## 章节导语

说真的，如果你跟着上一章把环境搭起来了，现在手里应该有一个能在 QEMU 里跑一段 16 位实模式代码的雏形。但这还不够，我们现在的处境就像拿着一台只能运行 DOS 程序的 8086 机器 —— 地址空间被限制在 1MB 以内，内存访问还得靠段地址：偏移这种别扭的方式，保护机制？不存在的。接下来我们要干的就是让 CPU 切换到保护模式（Protected Mode），这是 x86 处理器能够真正展现威力的第一个转折点。

完成本章后，你将在 QEMU 的 debugcon 终端看到一个孤零零的字母 `P` —— 这个看似简单的输出实际上标志着我们成功跨越了实模式与保护模式之间的鸿沟，为后续启用分页、加载 64 位长模式打下了基础。如果你现在还没完成上一章的环境搭建，建议先 `git checkout 000_env_toolchain` 把那部分搞定，否则接下来的代码大概率起不来。

---

## 概念精讲

### 什么是保护模式（Protected Mode）？

你可以把保护模式理解为 CPU 从"野蛮生长"到"文明社会"的一次升级。在实模式下，任何程序都可以随意读写任意内存地址，没有权限检查，没有内存隔离 —— 这在单任务 DOS 时代还能凑合，但一旦要跑多任务操作系统就是灾难。保护模式引入了几个关键机制：

1. **段描述符（Segment Descriptor）**：不再用简单的段地址，而是用描述符表来定义段的基地址、大小和访问权限
2. **特权级（Ring 0~3）**：区分内核态和用户态，防止应用程序乱搞硬件
3. **分页机制**：虽然本章不启用，但保护模式是分页的前置条件

从启动流程上看，BIOS 把控制权交给我们的 Bootloader 时 CPU 还在实模式，我们需要手动把它切换到保护模式，然后再跳转到 64 位长模式 —— 这条路不能跳过。

### GDT（全局描述符表）是什么？

GDT 是保护模式下的"段定义手册"。在实模式下，段寄存器（CS、DS、ES 等）直接存储段地址，但在保护模式下，它们存储的是"段选择子"（Selector），这个选择子实际上是一个索引，指向 GDT 中的某个描述符。

每个段描述符占 8 字节，格式如下（比较复杂，我们先看个大概）：

```
Base Address (32 bit)     +-------------------+
Limit (20 bit)            |                   |
                          |   Descriptor      |
Present, DPL, Type, etc.  |                   |
Granularity, 32-bit flag  +-------------------+
        Bytes: 0  1  2  3  4  5  6  7
```

我们需要定义至少三个描述符：
- **Null Descriptor**：索引 0，必须全 0，Intel 规定的空段
- **Code Segment**：可执行的 32 位代码段
- **Data Segment**：可读写的 32 位数据段

### 从实模式切换到保护模式的步骤

这个过程有几个关键步骤，顺序不能乱：

1. **定义 GDT**：创建段描述符表
2. **加载 GDTR**：用 `lgdt` 指令告诉 CPU GDT 在哪里
3. **设置 CR0 的 PE 位**：`movl %cr0, %eax; orl $1, %eax; movl %eax, %cr0`
4. **远跳转**：用 `ljmp` 刷新 CS 寄存器，跳转到 32 位代码段
5. **设置数据段**：把 DS、ES、SS 等段寄存器指向数据段选择子

⚠️ 这里有几个经典的坑点：第一步加载 GDTR 的时候要确保 GDT 的地址是正确的，否则 `lgdt` 会加载错误的地址；第二步设置 CR0 后必须马上用远跳转刷新流水线，否则 CPU 还会尝试用 16 位的方式解码指令，直接崩溃。

### AT&T 语法速查

我们用的是 GNU Assembler，语法和 NASM 有些区别：

| 操作 | AT&T 语法 | NASM 语法 |
|------|-----------|-----------|
| 立即数 | `movl $1, %eax` | `mov eax, 1` |
| 寄存器 | `%eax`, `%ebx` | `eax`, `ebx` |
| 内存访问 | `movl (%eax), %ebx` | `mov ebx, [eax] |
| 操作数顺序 | 源, 目标 | 目标, 源 |
| 远跳转 | `ljmp $SELECTOR, $offset` | `jmp SELECTOR:offset` |

注意我们在 16 位代码里用的是 `.code16`，切换到 32 位后要显式声明 `.code32`，否则汇编器会生成错误的机器码。

---

## 动手实现

### Step 1：在 stage2.S 中定义 GDT

**目标**：创建符合 x86 规范的段描述符表，包含空段、代码段和数据段。

**代码**（文件路径：`boot/stage2.S`）：

```asm
// ============================================================
// GDT (Global Descriptor Table)
// ============================================================
// 放在 .rodata 段，确保 8 字节对齐
.section .rodata
.align 8

// GDT Entry 0: 空描述符（Intel 硬件要求，必须存在）
gdt_null:
    .long 0, 0                    // 8 字节全零

// GDT Entry 1: 32 位代码段
// 选择子 = 0x08 (index=1, TI=0, RPL=0)
// 属性：基地址=0, 大小=4GB, 可执行, 可读, DPL=0
gdt_code:
    .word 0xFFFF                  // Limit[15:0]  = 0xFFFF
    .word 0x0000                  // Base[15:0]   = 0x0000
    .byte 0x00                    // Base[23:16]  = 0x00
    .byte 0x9A                    // P=1, DPL=0, S=1, Type=1010 (代码段, 可执行, 可读)
    .byte 0xCF                    // G=1 (4KB 粒度), D=1 (32位), Limit[19:16]=0xF
    .byte 0x00                    // Base[31:24]  = 0x00

// GDT Entry 2: 32 位数据段
// 选择子 = 0x10 (index=2, TI=0, RPL=0)
// 属性：基地址=0, 大小=4GB, 可读写, DPL=0
gdt_data:
    .word 0xFFFF                  // Limit[15:0]  = 0xFFFF
    .word 0x0000                  // Base[15:0]   = 0x0000
    .byte 0x00                    // Base[23:16]  = 0x00
    .byte 0x92                    // P=1, DPL=0, S=1, Type=0010 (数据段, 可写)
    .byte 0xCF                    // G=1 (4KB 粒度), D=1 (32位), Limit[19:16]=0xF
    .byte 0x00                    // Base[31:24]  = 0x00

// 预留几个描述符给 TSS、LDT 等后续扩展
gdt_reserved1:
    .long 0, 0
gdt_reserved2:
    .long 0, 0
gdt_reserved3:
    .long 0, 0
```

**解释**：

这里我们定义了一个最小可用的 GDT。第一个描述符必须全零 —— 这是 Intel 硬件规定的，虽然用不上但必须占位。第二个是代码段，第三个是数据段。

重点说一下这些魔法数字的含义：`0x9A` 这个字节包含多个字段：
- `P=1`（Present）：表示这个段在内存中
- `DPL=0`（Descriptor Privilege Level）：内核级权限
- `S=1`（System）：表示这是代码/数据段，不是系统段
- `Type=1010`：代码段，可执行，可读

`0xCF` 这个字节：
- `G=1`（Granularity）：段界限以 4KB 为单位，所以 0xFFFFF 实际代表 4GB
- `D=1`（Default size）：32 位代码段
- `Limit[19:16]=0xF`：配合 Limit[15:0] 组成完整的 20 位段界限

**验证**：此时编译应该不会报错，但运行的话还没加载 GDT，所以不会有实际效果。

---

### Step 2：定义 GDT 指针结构

**目标**：创建 `lgdt` 指令所需的伪描述符（limit + base）。

**代码**（文件路径：`boot/stage2.S`）：

```asm
// ============================================================
// GDT Pointer (lgdt 指令需要的伪描述符)
// ============================================================
// 格式：16 位 limit + 32 位 base
gdt_ptr:
    .word gdt_ptr - gdt_null - 1  // Limit = GDT 大小 - 1
    .long 0x8000 + gdt_null       // Base = stage2 基址 + GDT 偏移
```

**解释**：

`lgdt` 指令需要一个 6 字节的内存结构：前 2 字节是 GDT 的长度（limit - 1），后 4 字节是 GDT 的线性地址。我们这里用了一个小技巧：`gdt_ptr - gdt_null - 1` 自动计算出 GDT 的大小，不用每次手动改数字。

基址的计算方式是 `0x8000 + gdt_null`：`0x8000` 是 stage2 被加载到的物理地址，`gdt_null` 是链接器计算出的 GDT 在 stage2 内部的偏移量。这里要注意，`lgdt` 在 16 位模式下执行时需要正确处理分段地址的问题。

⚠️ **常见陷阱**：如果直接写 `.long gdt_null`，链接器生成的是一个基于 `0` 的地址，在实模式下是错的。必须加上 stage2 的加载基址。

**验证**：此时可以编译检查链接器是否正确计算了地址，但运行前需要完成后续步骤。

---

### Step 3：在 16 位代码中加载 GDT

**目标**：在切换到保护模式前，用 `lgdt` 指令加载 GDT。

**代码**（文件路径：`boot/stage2.S`，放在 `_start` 函数的 VESA 初始化之后）：

```asm
    // ... VESA 初始化代码 ...

    // ============================================================
    // 切换到保护模式
    // ============================================================

    // Step 1: 加载 GDTR
    cli                             // 先关中断，保护模式下 IDT 还没设置
    movw $gdt_ptr, %ax             // AX = gdt_ptr 的偏移
    movw %ax, %dx                  // DX = 偏移（用于远指针）
    movw $0x8000 >> 4, %ax         // AX = stage2 段地址 (0x8000 / 16 = 0x800)
    movw %ax, %ds                  // DS = 0x800
    lgdt (%dx)                     // lgdt [DS:DX] 加载 GDT

    // Step 2: 设置 CR0 的 PE 位
    movl %cr0, %eax                // 读取 CR0
    orl $1, %eax                   // 设置 bit 0 (PE = Protection Enable)
    movl %eax, %cr0                // 写回 CR0

    // Step 3: 远跳转到 32 位代码段
    ljmp $0x08, $pm_entry          // 跳到代码段选择子 0x08, 偏移 pm_entry
```

**解释**：

这里有几个关键点：首先我们在加载 GDT 前关中断（`cli`），因为保护模式下的中断处理需要 IDT（中断描述符表），而我们还没设置 IDT。如果这时候发生中断，CPU 会尝试从 IDT 读中断处理函数地址，结果大概率触发 triple fault。

`lgdt` 指令在 16 位模式下执行时有个坑：它需要内存地址的段:偏移形式，不能直接用 `.code16` 下的近指针。所以我们手动构造了远指针：`DS:DX` 指向 `gdt_ptr`。

设置 CR0 后必须立即远跳转 —— 这一点非常关键。现代 CPU 会预取和译码指令，如果我们不用远跳转强制刷新流水线，CPU 可能还会尝试用 16 位的方式解析后续指令，导致立即崩溃。

远跳转的 `ljmp $0x08, $pm_entry` 中，`0x08` 是代码段选择子：`index=1`（指向第二个描述符），`TI=0`（使用 GDT），`RPL=0`（内核级权限）。

**验证**：此时运行会卡死（因为还没写 32 位入口点），但可以通过 QEMU monitor 检查 CPU 状态。

---

### Step 4：编写 32 位保护模式入口点

**目标**：创建 `.code32` 入口函数，设置数据段并输出确认字符。

**代码**（文件路径：`boot/stage2.S`）：

```asm
// ============================================================
// 保护模式入口点（32 位代码）
// ============================================================
.section .text
.code32                         // 显式声明 32 位代码生成
.global pm_entry
pm_entry:
    // Step 4: 设置所有数据段寄存器
    movw $0x10, %ax             // AX = 数据段选择子 (index=2, TI=0, RPL=0)
    movw %ax, %ds               // DS = 数据段
    movw %ax, %es               // ES = 数据段
    movw %ax, %fs               // FS = 数据段
    movw %ax, %gs               // GS = 数据段
    movw %ax, %ss               // SS = 数据段（栈保持同一个位置）

    // 设置栈指针（确保是高地址，往低地址增长）
    movl $0x90000, %esp         // 栈顶设在 0x90000

    // Step 5: 输出 'P' 到 QEMU debugcon 确认进入保护模式
    movb $0x50, %al             // 'P' 的 ASCII 码
    outb %al, $0xE9             // 写入 debugcon 端口

    // 停机循环
    cli
.pm_halt:
    hlt
    jmp .pm_halt
```

**解释**：

进入保护模式后的第一件事就是把所有数据段寄存器设置为正确的数据段选择子 `0x10`。注意这里包括 `SS`（栈段），虽然栈的物理地址没变，但在保护模式下它必须通过段选择子访问。

栈指针我们设在 `0x90000` —— 这个地址在实模式阶段我们用了 `0x9000:0xFFFE`，换算成线性地址就是 `0x90000 + 0xFFFE`，所以现在直接用 `0x90000` 作为栈顶是兼容的。

最关键的一行是 `outb %al, $0xE9`：这是向 QEMU 的 debugcon 端口写一个字节。`0x50` 是字符 `'P'` 的 ASCII 码，如果 QEMU 用 `-debugcon stdio` 启动，这个字符会直接输出到终端。这是一个非常轻量的调试手段，比串口还简单，不需要初始化 UART。

**验证**：此时如果一切顺利，运行 QEMU 应该能看到终端输出 `P`，然后 CPU 进入停机循环。

---

### Step 5：更新 QEMU 启动参数

**目标**：在 CMakeLists.txt 中添加 `-debugcon stdio` 参数，让 QEMU 输出 debugcon 内容。

**代码**（文件路径：`CMakeLists.txt` 或对应的运行脚本）：

```cmake
# 在 QEMU 启动参数中添加
add_custom_target(run
    COMMAND qemu-system-x86_64
        -drive format=raw,file=${CMAKE_BINARY_DIR}/cinux.img
        -debugcon stdio           # 把 debugcon 输出到标准输出
        -serial stdio              # 保留串口输出
        -M q35                     # 使用 Q35 芯片组
        -m 128M                    # 128MB 内存
        -no-reboot                 # 禁用重启
        -no-shutdown               # 禁用关机
    DEPENDS cinux.img
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
```

**解释**：

`-debugcon stdio` 告诉 QEMU 把 debugcon 设备（端口 0xE9）的输出重定向到标准输出。这个端口是 QEMU 特有的调试接口，真实硬件上不存在，但非常适合这种早期的单字节调试。

⚠️ 注意 `-debugcon stdio` 会把输出直接打印到终端，不会像 `-serial stdio` 那样可能有缓冲问题。我们这里同时保留了串口，方便后续扩展。

**验证**：运行 `make run` 后，终端应该先输出之前的串口信息（如 "Stage2 OK"），然后在某个时刻单独输出一个 `P`，之后 CPU 停机。

---

## 构建与运行

现在我们准备完整编译运行一次。确保你在 `001_boot_real_mode` 这个 tag 上：

```bash
# 检出本章对应的代码版本
git checkout 001_boot_real_mode

# 创建构建目录
mkdir -p build
cd build

# 配置 CMake（Debug 模式，带符号信息）
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..

# 编译
make

# 运行 QEMU
make run
```

如果你看到终端输出了 `P`，恭喜 —— 你成功进入了保护模式！这个简单的字符背后，CPU 完成了从 16 位实模式到 32 位保护模式的切换，加载了 GDT，设置了段寄存器，每一步都不能出错。

### QEMU 启动参数解释

- `-drive format=raw,file=build/cinux.img`：加载我们的磁盘镜像
- `-debugcon stdio`：把 debugcon 端口（0xE9）的输出定向到终端
- `-serial stdio`：把串口输出定向到终端
- `-M q35`：使用较新的 Q35 芯片组（默认是 i440FX，但 Q35 更接近现代硬件）
- `-m 128M`：给虚拟机分配 128MB 内存
- `-no-reboot -no-shutdown`：让 QEMU 在系统崩溃时直接退出而不是重启

---

## 调试技巧

### 常见问题 1：看不到 `P` 输出

可能原因：
1. GDT 地址计算错误：检查 `gdt_ptr` 的 base 字段是否正确，应该是 `0x8000 + gdt_null` 的链接结果
2. 远跳转目标错误：确认 `pm_entry` 符号被正确导出（`.global pm_entry`）
3. CPU 在远跳转前就崩溃：可能是 `lgdt` 加载了错误地址，用 GDB 检查 `%gdtr` 寄存器

排查方法：用 QEMU 的 monitor 命令查看 GDT

```
# 在 QEMU monitor 中（按 Ctrl+A 然后按 C 进入）
(qemu) info registers
# 查看 GDTR 寄存器的值，确认 base 和 limit
```

### 常见问题 2：Triple Fault（QEMU 窗口直接关闭）

这通常是因为：
1. 进入保护模式后还没设置 IDT 就发生了中断（比如 NMI）
2. 段选择子的 TI 位或 RPL 位设置错误，导致 CPU 加载了无效的描述符
3. 栈指针设置错误，导致栈操作时访问了非法内存

排查方法：在切换保护模式前加 `cli` 确保中断关闭，并逐步注释代码验证哪一步出问题。

### 使用 GDB 调试

QEMU 支持 `-s -S` 参数启动后等待 GDB 连接：

```bash
# 在一个终端启动 QEMU
qemu-system-x86_64 -drive format=raw,file=build/cinux.img -s -S

# 在另一个终端启动 GDB
gdb build/kernel.elf  # 或你调试的目标文件
(gdb) target remote :1234
(gdb) break *pm_entry
(gdb) continue
(gdb) info registers
```

你可以用 `si`（单步执行指令）逐条执行，观察 `%cr0` 和段寄存器的变化。

---

## 本章小结

完成这一章后，我们已经跨越了 OS 开发的一个关键里程碑：从实模式进入保护模式。现在 CPU 能够使用 32 位寄存器和寻址方式，内存访问不再受 1MB 限制，我们可以访问完整的 4GB 地址空间了。

### 新增关键要素一览表

| 要素 | 位置 | 作用 |
|------|------|------|
| `gdt_null` | `boot/stage2.S` | 空段描述符（索引 0，必须存在） |
| `gdt_code` | `boot/stage2.S` | 32 位代码段，选择子 = 0x08 |
| `gdt_data` | `boot/stage2.S` | 32 位数据段，选择子 = 0x10 |
| `gdt_ptr` | `boot/stage2.S` | GDT 伪描述符，供 `lgdt` 使用 |
| `lgdt (%dx)` | `_start` 函数 | 加载 GDTR 寄存器 |
| `orl $1, %cr0` | `_start` 函数 | 设置 PE 位，开启保护模式 |
| `ljmp $0x08, $pm_entry` | `_start` 函数 | 远跳转刷新 CS，进入 32 位代码 |
| `pm_entry` | `.code32` 段 | 保护模式入口点 |
| `outb %al, $0xE9` | `pm_entry` | 向 debugcon 输出调试字符 |

### 下一章预告

现在我们已经在保护模式下了，但 CPU 还是 32 位的，无法使用 64 位寄存器和完整的 64 位地址空间。下一章（`002_boot_gdt_protected`）我们将启用分页机制，设置 PML4 表，然后切换到 64 位长模式 —— 那才是现代 x86-64 操作系统的真正起点。

到那个时候，我们就能开始写 C++ 内核代码了，不用再手搓汇编。但在此之前，还有一些基础设施要打好 —— 敬请期待。
