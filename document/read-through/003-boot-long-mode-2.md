---
title: 003-boot-long-mode-2 · Long Mode
---

# 003-2 模式切换：enter_long_mode() + long_mode_entry 完整代码讲解

> 标签：x86_64, 长模式切换, EFER, CR0/CR3/CR4, 远跳转, 64 位入口
> 前置：[003-1 页表构建](003-boot-long-mode-1.md)

## 概览

本文讲解 Long Mode 切换的后半段：从 `enter_long_mode` 函数开始，到 `long_mode_entry` 的 64 位入口初始化结束。上一篇我们已经把页表搭好了，现在要做的是让 CPU 真正"走进"Long Mode——依次设置 CR3、CR4.PAE、EFER.LME、加载扩展 GDT、开启分页，最后做一次远跳转到 64 位代码段。这个流程严格遵循 Intel SDM Vol.3A §10.8.5 的规范，顺序不可调换，任何一步跳过或错序都会导致三重故障。

关键设计决策：在 32 位保护模式下用 `ljmp` 切到 64 位代码段、GDT 指针用 `.long+.long` 避免 32 位 ELF 重定位、`movabsq` 设置 64 位栈指针。

## 模式切换状态机

```
                    CR4.PAE=1
Protected Mode ────────────────► PAE enabled
(32-bit, no paging)              (still 32-bit)
                                      │
                                  EFER.LME=1
                                      ▼
                                 LME requested
                                      │
                                  CR3 = PML4
                                      ▼
                                 Page table ready
                                      │
                                  CR0.PG=1
                                      ▼
Compatibility Mode ◄──────── LGDT + ljmp $0x18
(64-bit paging active            (CS.L=1, CS.D=0)
 but CS still 32-bit)                │
                                      ▼
                                 64-bit Long Mode ✓
```

注意 CR0.PG=1 之后 CPU 自动进入 Compatibility Mode，而不是直接进入 64 位模式。只有远跳转加载了 L=1/D=0 的代码段描述符到 CS 之后，CPU 才真正切换到 64 位模式。这个两阶段切换是 x86_64 架构设计的一个微妙之处——Intel SDM Vol.3A §10.8.5.3 对此有详细说明。

## enter_long_mode：五步状态切换

### 第一步：加载页表基址到 CR3

```asm
.global enter_long_mode
enter_long_mode:
    movl $PML4_PHYS_ADDR, %eax       // PML4 table physical address
    movl %eax, %cr3                  // Load page table base
```

CR3 寄存器存储当前活跃页表的 PML4 表物理基地址。上一篇中我们把 PML4 放在了 0x1000，所以这里把 0x1000 写入 CR3。CPU 在后续做地址翻译时会从 CR3 指向的 PML4 开始，逐级查找 PDPT、PD，最终定位到物理页。这一步必须在开启分页（CR0.PG）之前完成——否则分页开启后 CPU 不知道页表在哪，立即三重故障。

`movl %eax, %cr3` 这条指令会自动使 TLB（Translation Lookaside Buffer）中所有非全局页的缓存失效。因为此时分页还没开启，TLB 里本来就没有有效条目，所以这个副作用无所谓。

### 第二步：启用 PAE

```asm
    movl %cr4, %eax                  // Read CR4
    orl $CR4_PAE, %eax               // Set PAE bit (bit 5)
    movl %eax, %cr4                  // Write CR4 with PAE
```

CR4.PAE（bit 5，值 0x20）启用物理地址扩展，让 CPU 支持三级或四级页表结构（而不是 32 位分页的两级结构）。Long Mode 强制要求 PAE——没有 PAE 就无法使用 4 级页表，没有 4 级页表就无法进入 Long Mode。这一步必须在 EFER.LME 之前完成，因为 Intel SDM 规定设置 LME 时 PAE 必须已经启用。

CR4 不能直接做位运算，必须通过通用寄存器中转：先读出来、修改、再写回去。这是 x86 ISA 的架构限制，和控制寄存器的操作方式一致。

### 第三步：设置 EFER.LME

```asm
    movl $MSR_EFER, %ecx             // MSR_EFER address = 0xC0000080
    rdmsr                            // Read EFER into edx:eax
    orl $EFER_LME, %eax               // Set LME bit (bit 8)
    wrmsr                            // Write EFER from edx:eax
```

这一步是整个 Long Mode 切换中最容易出错的地方。EFER（Extended Feature Enable Register）是一个 MSR（Model-Specific Register），地址是 0xC0000080。我们关心的位是 bit 8（LME，Long Mode Enable），值为 0x100。

`rdmsr` 把 ECX 指定的 MSR 读入 EDX:EAX（高 32 位在 EDX，低 32 位在 EAX）。我们用 OR 指令设置 EAX 中的 bit 8，然后 `wrmsr` 把修改后的值写回 MSR。EDX 在整个过程中保持不变——我们不需要修改 EFER 的高 32 位。

这个坑值得再说一遍：EFER_LME 必须是 0x100（bit 8），不能写成 0x1000（bit 12）。0x1000 对应的是 AMD 的 SVME 位（Secure Virtual Machine Enable），在 Intel CPU 上这个位可能被忽略也可能触发 #GP，取决于具体型号。但即使写成功了，设的是 SVME 而不是 LME——CPU 的 Long Mode 状态机没有推进，随后开启分页时就会三重故障。我们调试时 GDB 显示 EFER=0x1000，只有 SVME 没有 LME，就是这个常量定义写错导致的。

### 第四步：加载扩展 GDT

```asm
    lgdt gdt64_ptr                  // Load GDT with 64-bit descriptor
```

在远跳转之前，我们需要加载包含 64 位描述符的 GDT。`lgdt` 把 `gdt64_ptr` 指向的 10 字节数据（2 字节 Limit + 8 字节 Base）加载到 GDTR。此时 CPU 还在 32 位保护模式下执行 32 位 `lgdt` 指令——但因为 GDTR 的 base 字段在 Long Mode 下会被解释为 64 位地址，我们必须提前加载 64 位格式的 GDT 指针。

`gdt64_ptr` 的定义在 stage2.S 中：

```asm
.global gdt64_ptr
gdt64_ptr:
    .word (gdt_end - gdt - 1)      // Limit = sizeof(GDT) - 1
    .long gdt                      // Lower 32 bits of GDT address
    .long 0                        // Upper 32 bits (zero, GDT is in low memory)
```

这里用 `.long` + `.long` 代替 `.quad` 是一个有意为之的设计。我们的 stage2 链接为 32 位 ELF（elf32-i386），如果用 `.quad gdt`，链接器会尝试生成一个 64 位的 R_X86_64_64 重定位条目，但 32 位 ELF 不支持这种重定位类型，直接报错。拆成两个 `.long` 就能绕过这个问题——低 32 位是 GDT 的链接地址，高 32 位填 0（因为 GDT 在低地址空间，不会超过 4GB 边界）。

### 第五步：开启分页 + 远跳转

```asm
    movl %cr0, %eax                  // Read CR0
    orl $(CR0_PG | CR0_PE), %eax      // Set PG+PE bits (0x80000001)
    movl %eax, %cr0                  // Write CR0 with paging enabled

    ljmp $0x18, $long_mode_entry    // Jump to 64-bit code

    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

`orl $(CR0_PG | CR0_PE), %eax` 计算 `0x80000000 | 0x01 = 0x80000001`，同时设置 PG（bit 31）和 PE（bit 0）。PG 开启分页，PE 确保保护模式保持开启。这一步是整个状态机的"激活点"——当 CR0.PG 从 0 变成 1 时，CPU 检查 PAE 是否启用、EFER.LME 是否设置、CR3 是否指向合法页表，全部满足后 CPU 自动设置 EFER.LMA（bit 10），分页正式生效。

分页生效的瞬间，CPU 立即开始使用页表翻译地址。由于我们做的是恒等映射，当前正在执行的代码的物理地址恰好等于虚拟地址，所以 CPU 能继续取指令——如果不是恒等映射，这一步之后 CPU 就会跳到错误的地址，直接三重故障。

紧接着的 `ljmp $0x18, $long_mode_entry` 是远跳转，选择子 0x18 指向 GDT 中偏移 24 字节的描述符（index = 0x18 >> 3 = 3），也就是 64 位代码段描述符 `gdt_code64`。远跳转做两件事：把 CS 刷新为新的选择子（CPU 从 GDT 加载 L=1/D=0 的描述符到 CS 的隐藏缓存），同时跳转到 `long_mode_entry` 的地址。此时 CPU 从 Compatibility Mode 正式切换到 64 位模式——因为 CS 描述符的 L=1 且 D=0，CPU 开始按 64 位解码和执行指令。

选择子 0x18 的拆解：`0x18 = 0000 0000 0001 1000b`，Index = 3（GDT 中第 4 个条目，跳过 null、32 位代码段、32 位数据段），TI = 0（使用 GDT），RPL = 0（Ring 0）。

远跳转之后的 `cli; hlt; jmp .lm_halt` 是永远不会执行的死代码——远跳转不会返回。但如果 CPU 因为某种奇怪的原因落到了这里，至少会安全地停住。

## stage2.S 中的调用链

在 stage2.S 的 `pm_entry`（保护模式入口点）中，输出 `P` 之后，代码变成了这样：

```asm
    // Transition to Long Mode
    call setup_page_tables          // Setup page tables at 0x1000-0x3FFF
    call enter_long_mode            // Jump to 64-bit mode

    cli
.pm_halt:
    hlt
    jmp .pm_halt
```

两个 `call` 的顺序不可调换——页表必须先建好，`enter_long_mode` 才能安全地加载 CR3 并开启分页。`enter_long_mode` 不会返回（最后是一条 `ljmp` 跳走了），所以后面的 `cli; hlt` 也是保护性代码。

## GDT 扩展：64 位描述符

stage2.S 的 `.gdt` section 在原有三个描述符之后追加了两个：

```asm
// 64-bit code descriptor (L=1, D=0)
// Value: 0x00AF9A000000FFFF
gdt_code64:
    .quad 0x00AF9A000000FFFF

// 64-bit data descriptor
// Value: 0x008F92000000FFFF
gdt_data64:
    .quad 0x008F92000000FFFF
```

64 位代码段描述符 `0x00AF9A000000FFFF` 的逐字节拆解：

```
字节 0-1: 0xFFFF → Limit[0:15] = 0xFFFF
字节 2-3: 0x0000 → Base[0:15] = 0x0000
字节 4:   0x00   → Base[16:23] = 0x00
字节 5:   0x9A   → Access Byte = 1001 1010
                              P=1, DPL=00, S=1, Type=1010 (Code/Exec/Read)
字节 6:   0xAF   → Flags[7:4] + Limit[16:19]
              Flags = 1010 → G=1, D=0, L=1, Reserved=0
              Limit[16:19] = 1111
字节 7:   0x00   → Base[24:31] = 0x00
```

最关键的是 Flags nibble 中的 L=1 和 D=0。L（Long）位是 bit 21，表示这是一个 64 位代码段。D（Default operation size）位是 bit 22，在 64 位代码段中必须为 0——如果 D=1，CPU 会把它当作 32 位代码段处理（兼容模式），而不是 64 位模式。G（Granularity）=1 表示 Limit 以 4KB 为单位缩放。Base=0、Limit=0xFFFFF（G=1 缩放后 = 4GB），但在 64 位模式下 Base 和 Limit 被 CPU 忽略，所有地址翻译都走页表。

64 位数据段描述符 `0x008F92000000FFFF` 的 Access Byte 是 0x92（Present、DPL=0、Data、Writable），Flags 是 0x8（G=1，D/B=0 在 64 位数据段中无意义）。在 64 位模式下，数据段描述符的 Base 和 Limit 同样被忽略，所以这个描述符的值主要就是 Access Byte 起作用（确保段是 Present 且可写的）。

完整的 GDT 布局变成 5 个条目：

| 偏移 | 选择子 | 描述符 | 用途 |
|------|--------|--------|------|
| 0x00 | 0x00 | null | 空描述符 |
| 0x08 | 0x08 | gdt_code (32-bit) | 保护模式代码段 |
| 0x10 | 0x10 | gdt_data (32-bit) | 保护模式数据段 |
| 0x18 | 0x18 | gdt_code64 | 64 位代码段 |
| 0x20 | 0x20 | gdt_data64 | 64 位数据段 |

## long_mode_entry：64 位入口初始化

远跳转之后，执行流到达 stage2.S 中的 `long_mode_entry`：

```asm
.code64
.global long_mode_entry
long_mode_entry:
    movw $GDT_DATA64, %ax           // Load 64-bit data selector (0x20)
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss
```

`.code64` 告诉 GAS 从这里开始生成 64 位指令编码。远跳转已经让 CPU 切换到了 64 位模式，`.code64` 确保汇编器的输出和 CPU 的解码方式一致——这和 tag 002 中 `.code32` 配合 `ljmp` 的逻辑完全一样。

DS、ES、FS、GS、SS 全部加载为 0x20（64 位数据段选择子）。远跳转只刷新了 CS，其他段寄存器还残留着保护模式的旧值。在 64 位模式下，大部分段寄存器的 Base 被 CPU 忽略（强制为 0），但 SS 仍然用于栈操作的权限检查，所以必须重载为有效的 64 位数据段选择子。ES、FS、GS 在 64 位模式下也有特殊用途（比如 GS.base 可以通过 WRMSR 设置为 per-CPU 数据区的地址），这里先统一设为 0x20。

```asm
    movabsq $0x90000, %rsp          // Set 64-bit stack pointer

    movb $CHAR_LONG_MODE, %al       // Load 'L' (0x4C)
    outb %al, $DEBUGCON_PORT        // Output 'L' to debugcon

    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

`movabsq` 是 x86_64 中把 64 位立即数加载到寄存器的指令。这里不能用 `movq`——`movq` 只接受 32 位符号扩展立即数，0x90000 虽然在 32 位范围内，但 GAS 在 `.code64` 模式下对 `movq $imm, %rsp` 可能生成错误的编码。用 `movabsq` 最安全，它总是生成 10 字节的 `REX.W + B8+rd + imm64` 编码。

栈指针设为 0x90000，和 tag 002 保护模式下的栈位置一致。在 64 位 Flat 模式下，SS.base=0，所以 RSP=0x90000 就是物理地址 0x90000，栈从这个位置向低地址增长。

最后输出 `L`（0x4C）到 debugcon 端口 0xE9，然后进入停机循环。如果一切正确，`debug.log` 中应该同时出现 `P`（保护模式，来自 tag 002）和 `L`（Long Mode）。这个孤零零的 `L` 就是我们的第二面胜利旗帜——它意味着从保护模式到 Long Mode 的切换全部正确执行，四级页表工作正常，64 位代码段描述符正确，CPU 现在运行在真正的 64 位模式下。

## CMake 变更

`boot/CMakeLists.txt` 中新增了 `boot_longmode` 对象库并链接到 stage2：

```cmake
add_library(boot_longmode OBJECT
    common/long_mode.S
)

target_compile_options(boot_longmode PRIVATE
    -Wa,--32
)
```

然后在 stage2 的 `add_executable` 中追加：

```cmake
add_executable(stage2
    stage2.S
    $<TARGET_OBJECTS:boot_common>
    $<TARGET_OBJECTS:boot_longmode>
)
```

`-Wa,--32` 让汇编器以 32 位目标文件格式输出。虽然 `long_mode.S` 内部包含 `.code16`、`.code32`、`.code64` 的切换，但 `-Wa,--32` 只影响目标文件格式（elf32-i386），不影响汇编器内部的模式切换指令。

## 设计决策

### 决策：在 32 位模式下 lgdt 然后 ljmp，而不是先 ljmp 再 lgdt

**问题**: GDT 加载和远跳转的顺序——是先加载 64 位 GDT 再跳，还是先跳到 64 位模式再加载？

**本项目的做法**: 在 32 位保护模式下执行 `lgdt gdt64_ptr`，然后 `ljmp $0x18, $long_mode_entry`。

**备选方案**: 先开启分页进入 Compatibility Mode，然后进入 64 位模式后再 `lgdt` 加载 GDT。

**为什么不选备选方案**: Intel SDM Vol.3A §10.8.5 明确要求在远跳转之前加载 GDT。原因是远跳转 `ljmp $0x18, ...` 需要用选择子 0x18 去 GDT 查找 64 位代码段描述符——如果此时 GDTR 还指向旧的 3 条目 GDT（没有偏移 0x18 处的描述符），CPU 会查到一个 null 描述符或者垃圾数据，直接触发 #GP。

### 决策：gdt64_ptr 用 .long+.long 而不是 .quad

**问题**: 64 位 GDT 指针的基地址字段是 8 字节，怎么在 32 位 ELF 中表示？

**本项目的做法**: `.long gdt` + `.long 0`，两个 32 位拼接。

**备选方案**: `.quad gdt`，一条指令写 64 位。

**为什么不选备选方案**: stage2 链接为 elf32-i386 格式。`.quad gdt` 会让链接器生成 R_X86_64_64 类型的重定位，但 elf32-i386 不支持 64 位重定位——链接直接报错。拆成两个 `.long` 后，低 32 位生成普通的 32 位重定位（R_386_32），高 32 位是常量 0 不需要重定位，链接通过。GDT 在低内存（0x8000 附近），高 32 位确实是 0，所以结果完全正确。

## 扩展方向

- ⭐ 在 `enter_long_mode` 的每一步之间插入 debugcon 输出（不同字符），形成面包屑调试链
- ⭐⭐ 用 CPUID 指令（leaf 0x80000001）检查 CPU 是否支持 Long Mode，不支持则输出错误信息并停机
- ⭐⭐⭐ 尝试把远跳转改成 Linux kernel 的 `lret` 技巧：先 push CS 和 EIP 到栈上，开启分页，然后 `lret` 从栈上弹出选择子和地址——这比 `ljmp` 更灵活但更复杂

## 参考资料

- Intel SDM Vol.3A §10.8.5 — IA-32e Mode Initialization：五步切换序列，一致性检查
- Intel SDM Vol.3A §10.8.5.3 — CS.L 和 CS.D 决定子模式：L=1/D=0 = 64 位模式
- Intel SDM Vol.3A §2.5 — Control Registers：CR0.PG/PE、CR3、CR4.PAE 位定义
- Intel SDM Vol.3A §3.4.5 — Segment Descriptors：L 位和 D 位的含义
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- Linux kernel: [head_64.S](https://0xax.gitbooks.io/linux-insides/content/Booting/linux-bootstrap-4.html) — Linux 的 Long Mode 切换实现对比
