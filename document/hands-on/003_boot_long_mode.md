# 003_boot_long_mode · 进入 Long Mode

> 本章完成后的可见效果：QEMU 不崩溃，debugcon 输出 `L`（确认进入 64-bit long mode）
>
> 前置要求：已完成 `002_boot_gdt_protected`，理解保护模式基本概念

---

## 一、前言：为什么必须进入 Long Mode

说实话，在 2026 年还在用 32 位保护模式写内核，多少有点"考古"的感觉。但我们没办法——x86 架构的历史包袱太重，启动链条必须按部就班地走：Real Mode → Protected Mode → Long Mode。

上一章我们进入了保护模式，输出一个 `P` 字母就算成功。但保护模式本质上还是 32 位时代的东西，寄存器只有 32 位、地址空间只有 4GB、各种特权级机制还停留在 80386 时代。要写一个"现代"的操作系统，我们必须进入 Long Mode（即 64 位模式）。

这一章的目标非常明确：让 CPU 从 32 位保护模式切换到 64 位 Long Mode，并通过 debugcon 输出一个 `L` 字符确认成功。听上去简单？这一步实际上踩过非常多坑，包括我自己在调试时遇到的 EFER.LME 位写错导致的 triple fault。

**这一步之后，我们就能：**
- 使用 64 位寄存器（RAX、RBX、RSP...）
- 访问 64 位地址空间（理论上限 256TB，实际上我们只会用一部分）
- 为后续 C++ 内核铺路（现代 C++ 编译器默认就是 64 位）

---

## 二、核心概念精讲

### 2.1 Long Mode 是什么？

Long Mode 是 AMD 在 2003 年引入的 x86-64 架构的核心特性，后来被 Intel 采用。它本质上是一种新的 CPU 运行模式，类似于从 Real Mode 切换到 Protected Mode，但这次是从 32 位切换到 64 位。

你可以把 Long Mode 理解为：在 Protected Mode 基础上，把所有东西都"加倍"——寄存器 64 位、地址 64 位、指令集也扩展了。

### 2.2 启动 Long Mode 的必要条件

这一点非常关键，我把它写成一个状态机：

```
CPU 启动
    ↓
[Real Mode] ← 默认状态，16 位
    ↓ (ljmp 跳转)
[Protected Mode] ← 上一章完成，32 位
    ↓ (本章节)
[Long Mode] ← 本章目标，64 位
```

要进入 Long Mode，必须**同时满足**以下条件：

1. **CR4.PAE = 1**（Physical Address Extension）——开启物理地址扩展
2. **EFER.LME = 1**（Long Mode Enable）——开启 Long Mode，这一步最容易漏
3. **CR3 指向有效的页表**——必须有合法的 4 级页表结构
4. **CR0.PG = 1**（Paging Enable）——开启分页，这一步会让 CPU 实际进入 Long Mode

注意顺序很重要！如果顺序错了，CPU 会直接 triple fault。我在调试时就是因为 LME 位写错，导致 CPU 在开启分页时崩溃。

### 2.3 页表结构（4 级）

Long Mode 强制使用 4 级页表结构：

```
虚拟地址 → [PML4] → [PDPT] → [PD] → [PT] → 物理页
           (512条)  (512条)  (512条) (512条)
```

每一级都是 512 个条目，每个条目 8 字节，所以每张表正好占一页（4KB）。但我们可以用 2MB 大页跳过 PT 级别，这样只需要 3 级页表就够了。

### 2.4 Identity Mapping

在刚切换到 Long Mode 时，我们需要 identity mapping（恒等映射）：虚拟地址 = 物理地址。这是因为我们的代码还在使用物理地址执行，如果虚拟地址和物理地址不一致，CPU 会立即找不到下一条指令。

通俗地说：刚切换模式时，我们需要一个"过渡阶段"，让虚拟地址 0x1000 映射到物理地址 0x1000，这样代码才能继续运行。

### 2.5 GDT 扩展

Long Mode 需要新的 GDT 描述符格式。64 位代码段描述符的 L 位（Long bit）必须设为 1，D 位（Default bit）必须为 0。具体来说，64 位代码段描述符的值是 `0x00AF9A000000FFFF`。

---

## 三、AT&T 汇编语法速查

因为本项目使用 GNU Assembler（AT&T 语法），这里快速复习一下关键语法：

| 操作 | Intel 语法 | AT&T 语法 |
|------|-----------|----------|
| 寄存器 | `eax` | `%eax` |
| 立即数 | `10` | `$10` |
| 目标源顺序 | `mov eax, ebx` | `movl %ebx, %eax` |
| 内存取值 | `mov eax, [ebx]` | `movl (%ebx), %eax` |
| 内存偏移 | `mov eax, [ebx+4]` | `movl 4(%ebx), %eax` |
| 绝对地址 | `mov eax, [0x1000]` | `movl 0x1000, %eax` |

**重要提示**：AT&T 语法的 `movl 源, 目标` 和 Intel 语法的 `mov 目标, 源` 是反过来的！

---

## 四、动手实现

### Step 1：创建 Long Mode 模块骨架

**目标**：创建 `boot/common/long_mode.S`，封装所有 Long Mode 相关函数

**代码**（文件路径：`boot/common/long_mode.S`）：

```asm
/**
 * @file boot/common/long_mode.S
 * @brief x86-64 Long Mode Initialization Functions
 */
```

这个文件需要包含几个核心功能：
- 页表初始化
- Long Mode 切换
- 相关常量定义

先定义一些关键常量：

```asm
// Page table physical addresses (fixed layout)
.set PML4_PHYS_ADDR,      0x1000      // PML4 table location
.set PDPT_PHYS_ADDR,      0x2000      // PDPT table location
.set PD_PHYS_ADDR,        0x3000      // Page Directory location

// Page table entry flags
.set PAGE_PRESENT,        0x01        // Present bit
.set PAGE_WRITABLE,       0x02        // Read/Write bit
.set PAGE_LARGE,          0x80        // Page Size bit (2MB pages)

// MSR addresses
.set MSR_EFER,            0xC0000080  // Extended Feature Enable Register

// EFER bits
.set EFER_LME,            0x100       // Long Mode Enable (bit 8)
```

**解释**：

这里定义了页表的物理地址布局。我们选择固定地址 0x1000-0x3FFF，这是因为在启动早期阶段，动态内存分配还不存在，只能用固定地址。PML4、PDPT、PD 各占一页（4KB）。

PAGE_PRESENT、PAGE_WRITABLE、PAGE_LARGE 是页表条目的标志位。PRESENT=1 表示页在内存中，WRITABLE=1 表示可写，LARGE=1 表示使用 2MB 大页。

EFER_LME = 0x100 是关键！这是 bit 8，表示开启 Long Mode。**千万不要写成 0x1000**（那是 bit 12，SVME 位），我之前就踩过这个坑，CPU 直接 triple fault。

---

### Step 2：实现页表初始化函数

**目标**：编写 `setup_page_tables` 函数，初始化 3 级页表结构

**代码**（文件路径：`boot/common/long_mode.S`，第 87 行）：

```asm
.global setup_page_tables
setup_page_tables:
    // 清零方向标志（确保 stosl 递增）
    cld

    // 清零 PML4 表（0x1000）
    movl $PML4_PHYS_ADDR, %edi       // 目标地址 = PML4 地址
    xorl %eax, %eax                   // 要写入的值 = 0
    movl $1024, %ecx                  // 计数 = 4096/4 = 1024 个 dword
    rep stosl                         // 清零 PML4，重复 ecx 次

    // 清零 PDPT 表（0x2000）
    movl $PDPT_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl

    // 清零 PD 表（0x3000）
    movl $PD_PHYS_ADDR, %edi
    xorl %eax, %eax
    movl $1024, %ecx
    rep stosl
```

**解释**：

这里使用了 `rep stosl` 指令来批量清零内存。这个指令的意思是：重复 `stosl`（store string）指令 `ecx` 次。`stosl` 把 `eax` 的值写入 `es:edi` 指向的内存，然后 `edi` 递增 4（因为是 `l` 后缀，long 操作）。

4096 字节 / 4 字节 = 1024 次，正好清零一页内存。

接下来建立页表层级关系：

```asm
    // 设置 PML4[0] → PDPT
    movl $PDPT_PHYS_ADDR, %eax       // 加载 PDPT 物理地址
    orl $0x03, %eax                   // 添加 present+writable 标志
    movl %eax, PML4_PHYS_ADDR        // PML4[0] = PDPT | flags

    // 设置 PDPT[0] → PD
    movl $PD_PHYS_ADDR, %eax         // 加载 PD 物理地址
    orl $0x03, %eax                   // 添加 present+writable 标志
    movl %eax, PDPT_PHYS_ADDR        // PDPT[0] = PD | flags

    // 设置 PD[0..3] 为 2MB 页（恒等映射 0-8MB）
    movl $PD_PHYS_ADDR, %edi         // EDI 指向 PD 开始
    movl $4, %ecx                    // 循环 4 次
    xorl %eax, %eax                  // EAX = 0（循环变量）

1:
    movl %eax, %edx                  // 复制循环变量到 EDX
    shll $21, %edx                   // 左移 21 位（2MB 对齐）
    orl $PAGE_FLAGS, %edx            // 添加标志位

    movl %edx, (%edi)                // PD[i] = base_address | flags
    addl $8, %edi                    // 移动到下一个 PD 条目

    incl %eax                        // 循环变量递增
    loop 1b                          // 循环直到 ECX = 0
    ret
```

**解释**：

这里建立了 3 级页表关系：PML4[0] 指向 PDPT，PDPT[0] 指向 PD。这是最简单的配置，只映射了低地址空间的第一个 PML4 条目。

然后我们用循环设置 PD[0..3]，每个条目映射 2MB 内存。通过 `shll $21, %edx`，我们把索引乘以 2MB（21 位），得到物理地址。`PAGE_FLAGS` 包含 PRESENT + WRITABLE + LARGE，这样 CPU 就知道这是 2MB 大页。

最终效果：虚拟地址 0x00000000-0x007FFFFF 映射到物理地址 0x00000000-0x007FFFFF（8MB 恒等映射）。

**验证**：在 GDB 中可以检查页表内容：

```
(gdb) x/8gx 0x1000   # 查看 PML4
(gdb) x/8gx 0x2000   # 查看 PDPT
(gdb) x/8gx 0x3000   # 查看 PD
```

应该看到 PML4[0] 指向 0x2003（PDPT + flags），PDPT[0] 指向 0x3003（PD + flags），PD[0..3] 分别是 0x83、0x200083、0x400083、0x600083。

---

### Step 3：实现进入 Long Mode 的核心函数

**目标**：编写 `enter_long_mode` 函数，执行 CPU 状态切换

**代码**（文件路径：`boot/common/long_mode.S`，第 149 行）：

```asm
.global enter_long_mode
enter_long_mode:
    // 1. 加载页表基址到 CR3
    movl $PML4_PHYS_ADDR, %eax       // PML4 表物理地址
    movl %eax, %cr3                  // 加载页表基址

    // 2. 启用 PAE（Physical Address Extension）
    movl %cr4, %eax                  // 读取 CR4
    orl $CR4_PAE, %eax               // 设置 PAE 位（bit 5）
    movl %eax, %cr4                  // 写回 CR4

    // 3. 启用 Long Mode（EFER.LME）
    movl $MSR_EFER, %ecx             // MSR_EFER 地址 = 0xC0000080
    rdmsr                            // 读取 EFER 到 edx:eax
    orl $EFER_LME, %eax               // 设置 LME 位（bit 8）
    wrmsr                            // 写回 EFER

    // 4. 加载扩展 GDT（包含 64 位描述符）
    lgdt gdt64_ptr                   // 加载 GDT

    // 5. 启用分页（并确保保护模式已开启）
    movl %cr0, %eax                  // 读取 CR0
    orl $(CR0_PG | CR0_PE), %eax     // 设置 PG + PE 位
    movl %eax, %cr0                  // 写回 CR0

    // 6. 远跳转到 64 位代码段
    ljmp $0x18, $long_mode_entry     // 跳转到 64 位代码

    // 永远不会到这里
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

**解释**：

这就是整个 Long Mode 切换的核心！让我逐行解释：

1. **加载 CR3**：告诉 CPU 页表在哪里。CR3 必须指向有效的 PML4 表。

2. **启用 PAE**：PAE 是 Long Mode 的前置条件。CR4 的 bit 5（0x20）控制 PAE。

3. **启用 LME**：这是最关键的一步！`rdmsr` 读取 MSR 寄存器，`wrmsr` 写回。EFER 的 bit 8（0x100）控制 LME。**这里千万别写错**，0x1000 是 SVME 位，不是 LME！

4. **加载 GDT**：Long Mode 需要新的 GDT 描述符。`gdt64_ptr` 我们稍后定义。

5. **启用分页**：CR0 的 bit 31（0x80000000）控制分页，bit 0（0x1）控制保护模式。**注意**：启用分页后，CPU 会检查是否满足 Long Mode 条件（PAE=1、LME=1），如果满足就自动进入 Long Mode。

6. **远跳转**：`ljmp $0x18, $long_mode_entry` 跳转到 64 位代码段。选择子 0x18 是 64 位代码段描述符（我们在 GDT 中定义）。这个跳转会刷新指令流水线，让 CPU 真正开始执行 64 位代码。

**常见陷阱**：

如果顺序错了，CPU 会直接 triple fault。正确的顺序必须是：PAE → LME → CR3 → PG → ljmp。我在调试时遇到过因为 LME 没正确设置，导致开启 PG 时崩溃的问题。

**验证**：在 GDB 中逐步执行，检查寄存器：

```
(gdb) info registers cr0
(gdb) info registers cr4
(gdb) info registers efer
```

正确状态应该是：
- CR0 = 0x80000011（PE + PG）
- CR4 = 0x20（PAE）
- EFER = 0x100（LME）

---

### Step 4：更新 stage2.S

**目标**：在 Stage2 中调用 Long Mode 函数，并定义 64 位代码段

**代码**（文件路径：`boot/stage2.S`）：

首先添加外部函数声明：

```asm
// ============================================================
// External functions from common/long_mode.S
// ============================================================
.extern setup_page_tables
.extern enter_long_mode
```

然后在保护模式入口（`pm_entry`）中调用：

```asm
.code32                          // 32 位保护模式
pm_entry:
    // 设置段寄存器（上一章已完成）
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl $0x90000, %esp           // 设置新的栈

    // Debug 输出：'P' 表示 Protected Mode
    movb $0x50, %al
    outb %al, $0xE9

    // ============================================================
    // 切换到 Long Mode
    // ============================================================

    call setup_page_tables          // 初始化页表
    call enter_long_mode            // 进入 Long Mode

    // 永远不会到这里
    cli
.pm_halt:
    hlt
    jmp .pm_halt
```

**解释**：

这里的逻辑很简单：先输出 'P' 确认在保护模式，然后调用我们刚写的两个函数。`setup_page_tables` 会初始化页表，`enter_long_mode` 会执行模式切换。

---

### Step 5：定义 64 位代码段和入口

**目标**：扩展 GDT，添加 64 位代码段和数据段描述符，并定义 Long Mode 入口点

**代码**（文件路径：`boot/stage2.S`，GDT 部分）：

```asm
// ============================================================
// GDT (Global Descriptor Table)
// ============================================================
.section .gdt,"a"
.align 8

gdt:
gdt_null:
    .quad 0                        // 空描述符（必须）

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

// 64 位代码段描述符（L=1, D=0）
// 值：0x00AF9A000000FFFF
gdt_code64:
    .quad 0x00AF9A000000FFFF

// 64 位数据段描述符
// 值：0x008F92000000FFFF
gdt_data64:
    .quad 0x008F92000000FFFF

gdt_end:
```

**解释**：

`gdt_code64` 是 64 位代码段描述符。关键在于这个魔法数字 `0x00AF9A000000FFFF`：

- `0xFFFF`：Limit 15:0
- `0x0000`：Base 15:0
- `0x00`：Base 23:16
- `0x9A`：Access byte（Present=1, DPL=0, Code=1, Executable=1, Readable=1）
- `0xAF`：Flags（L=1 表示 Long Mode，D=0 表示 64 位，4KB 粒度）
- `0x00`：Base 31:24

选择子计算：
- gdt_null: 0x00 (第 0 个，偏移 0)
- gdt_code: 0x08 (第 1 个，偏移 8)
- gdt_data: 0x10 (第 2 个，偏移 16)
- gdt_code64: 0x18 (第 3 个，偏移 24) ← 我们用这个！
- gdt_data64: 0x20 (第 4 个，偏移 32)

然后定义 GDT 指针：

```asm
// 32 位 GDT 指针（保护模式用）
gdt_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // GDT 基地址（32 位）

// 64 位 GDT 指针（Long Mode 重新加载用）
.global gdt64_ptr
gdt64_ptr:
    .word (gdt_end - gdt - 1)      // Limit
    .long gdt                      // 低 32 位
    .long 0                        // 高 32 位（GDT 在低地址，所以为 0）
```

**解释**：

`gdt_ptr` 是保护模式用的，格式是 `[limit(16位), base(32位)]`。

`gdt64_ptr` 是 Long Mode 用的，格式略有不同：`[limit(16位), base_low(32位), base_high(32位)]`。因为我们的 GDT 在低 4GB 地址空间，高 32 位为 0。

---

### Step 6：定义 Long Mode 入口点

**目标**：编写 64 位代码入口，设置段寄存器和栈，输出 'L' 确认成功

**代码**（文件路径：`boot/stage2.S`）：

```asm
// ============================================================
// Long Mode 入口点
// ============================================================
.code64                          // 现在在 64 位 Long Mode
.global long_mode_entry
long_mode_entry:
    // GDT 已经在 enter_long_mode 中加载

    // 设置数据段寄存器
    movw $GDT_DATA64, %ax         // 加载 64 位数据段选择子
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // 设置 64 位栈
    movabsq $0x90000, %rsp        // 设置 64 位栈指针

    // 验证进入 Long Mode（输出 'L'）
    movb $CHAR_LONG_MODE, %al     // 'L' (0x4C)
    outb %al, $DEBUGCON_PORT      // 输出到 debugcon

    // 暂停
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

**解释**：

一旦 CPU 执行到这里，我们就成功进入 Long Mode 了！首先设置所有段寄存器指向 64 位数据段（选择子 0x20）。注意在 64 位模式下，段基址被忽略（除了 FS 和 GS），但段选择子还是要正确设置。

然后设置 64 位栈指针。注意用的是 `movabsq`，因为常数可能超过 32 位。

最后输出 'L' 到 debugcon，确认成功。如果你在终端看到 `PL`，说明保护模式和 Long Mode 都成功了！

**验证**：运行 QEMU，应该在终端看到：

```
PL
```

`P` 表示保护模式，`L` 表示 Long Mode。如果只看到 `P` 然后 QEMU 崩溃，说明 Long Mode 切换失败。

---

### Step 7：更新 CMakeLists.txt

**目标**：将 long_mode.S 编译并链接到 stage2

**代码**（文件路径：`boot/CMakeLists.txt`）：

```cmake
# ==============================================================
# Common Long Mode Support
# ==============================================================

add_library(boot_longmode OBJECT
    common/long_mode.S
)

target_compile_options(boot_longmode PRIVATE
    -Wa,--32                    # 生成 32 位对象（包含 .code16, .code32, .code64）
)
```

然后在 stage2 目标中添加：

```cmake
add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>   # 串口等公共函数
    $<TARGET_OBJECTS:boot_longmode> # Long Mode 函数 ← 新增
)
```

**解释**：

这里使用 CMake 的 Object Library 功能，把 `long_mode.S` 编译成对象文件，然后链接到 `stage2`。`-Wa,--32` 告诉汇编器生成 32 位对象，但代码中包含 `.code16`、`.code32`、`.code64` 指令，汇编器会自动处理。

---

## 五、编译与运行

### 5.1 编译命令

```bash
# 从项目根目录
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..
make
```

### 5.2 运行方法

```bash
# 运行 QEMU（带 debugcon）
qemu-system-x86_64 -drive format=raw,file=cinux.img -debugcon stdio
```

QEMU 参数说明：
- `-drive format=raw,file=cinux.img`：使用原始磁盘镜像
- `-debugcon stdio`：将 debugcon 输出到标准输出（终端）

### 5.3 预期输出

如果一切正常，你应该在终端看到：

```
PL
```

- `P`：进入保护模式（上一章实现）
- `L`：进入 Long Mode（本章实现）

---

## 六、调试技巧

### 6.1 常见问题与排查方法

**问题 1：只输出 `P` 然后 QEMU 崩溃（Triple Fault）**

可能原因：
- EFER.LME 没正确设置（检查 `EFER_LME` 常量是否为 0x100，不是 0x1000！）
- 页表结构错误
- CR4.PAE 没开启

排查方法：
```gdb
(gdb) info registers efer
(gdb) info registers cr4
```

确认 EFER 的 bit 8 为 1，CR4 的 bit 5 为 1。

**问题 2：页表设置后仍然崩溃**

可能原因：
- 页表条目格式错误
- Identity mapping 不完整

排查方法：
```gdb
(gdb) x/8gx 0x1000   # PML4
(gdb) x/8gx 0x2000   # PDPT
(gdb) x/8gx 0x3000   # PD
```

检查每个条目的值是否正确。

**问题 3：远跳转后崩溃**

可能原因：
- GDT 描述符格式错误
- 选择子计算错误

排查方法：
```gdb
(gdb) x/2gx &gdt
(gdb) x/4i *0x18    # 查看跳转目标代码
```

### 6.2 使用 GDB 调试

启动 QEMU 时添加 `-s -S` 参数：

```bash
qemu-system-x86_64 -drive format=raw,file=cinux.img -debugcon stdio -s -S
```

然后另一个终端启动 GDB：

```bash
gdb cinux.elf
(gdb) target remote :1234
(gdb) break pm_entry
(gdb) continue
```

单步执行：
- `si`（stepi）：执行一条汇编指令
- `info registers`：查看所有寄存器
- `x/10i $pc`：查看当前位置的指令

### 6.3 Debugcon 验证技巧

在关键位置添加 debugcon 输出：

```asm
// 在 enter_long_mode 中的每个步骤后
movb $'1', %al
outb %al, $DEBUGCON_PORT    // '1'：CR3 已设置

movb $'2', %al
outb %al, $DEBUGCON_PORT    // '2'：PAE 已启用

movb $'3', %al
outb %al, $DEBUGCON_PORT    // '3'：LME 已设置
```

这样可以看到 CPU 在哪一步崩溃。

---

## 七、本章小结

### 7.1 新增关键函数/结构

| 名称 | 类型 | 位置 | 功能 |
|------|------|------|------|
| `setup_page_tables` | 函数 | `boot/common/long_mode.S` | 初始化 3 级页表（PML4/PDPT/PD） |
| `enter_long_mode` | 函数 | `boot/common/long_mode.S` | 执行 Long Mode 切换 |
| `long_mode_entry` | 标号 | `boot/stage2.S` | 64 位代码入口点 |
| `gdt_code64` | 描述符 | `boot/stage2.S` | 64 位代码段 GDT 描述符 |
| `gdt_data64` | 描述符 | `boot/stage2.S` | 64 位数据段 GDT 描述符 |

### 7.2 新增寄存器/MSR

| 寄存器 | 关键位 | 说明 |
|--------|--------|------|
| CR3 | - | 页表基址寄存器 |
| CR4.PAE | bit 5 | 物理地址扩展 |
| EFER.LME | bit 8 | Long Mode 启用 |
| CR0.PG | bit 31 | 分页启用 |

### 7.3 下一步

下一章（`004_boot_load_kernel`）将加载 ELF64 内核镜像。届时我们需要：
- 实现高半核映射（`0xFFFFFFFF80000000` → `0x00000000`）
- 解析 ELF64 格式
- 设置 BootInfo 结构体
- 跳转到内核入口点

Long Mode 成功后，我们就有了 64 位执行环境，为后续 C++ 内核铺平了道路。

---

## 八、参考资源

- [AMD64 Architecture Programmer's Manual Volume 2: System Programming](https://www.amd.com/system/files/TechDocs/24593.pdf) —— Long Mode 官方文档
- [Intel SDM Vol. 3A: Chapter 9: Processor Management and Initialization](https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html) —— Intel 的模式切换说明
- [OSDev Wiki: Long Mode](https://wiki.osdev.org/Long_Mode) —— 社区维护的 Long Mode 教程
- [OSDev Wiki: Paging](https://wiki.osdev.org/Paging) —— 页表结构详解
