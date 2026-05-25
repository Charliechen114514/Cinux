---
title: 003-boot-long-mode-3 · Long Mode
---

# 003-3 模式切换：从保护模式到 Long Mode 的五步状态机

> 标签：x86_64, EFER, CR0/CR3/CR4, 远跳转, 64 位 GDT, 状态机切换
> 前置：[003-2 页表构建](003-boot-long-mode-2.md)

## 本篇目标

上一篇我们把页表搭好了——

PML4 在 0x1000、PDPT 在 0x2000、PD 在 0x3000，

用 2MB 大页恒等映射了前 8MB 物理内存，还建立了 Higher-Half Kernel 映射。

本篇的核心任务是实现 `enter_long_mode` 函数和 `long_mode_entry` 入口点，

把 CPU 从保护模式带入 64 位 Long Mode。

这五个步骤必须按固定顺序执行，

任何一步跳过、调换或者参数写错，CPU 就直接三重故障。

没有报错信息，没有错误码，QEMU 窗口闪一下就没了。

## 模式切换五步走

### Step 1: 实现 enter_long_mode——第一步：加载 CR3

**目标**：

在 `enter_long_mode` 函数的开头，把 PML4 的物理地址（0x1000）写入 CR3 寄存器。

**设计思路**：

CR3 寄存器存储当前页表的 PML4 表物理基地址。

把 0x1000 写入 CR3，CPU 就知道页表从哪开始查了。

这一步必须在开启分页（CR0.PG）之前完成——

道理很简单：分页开启后 CPU 立即开始用页表翻译地址，

如果 CR3 还没有指向合法的 PML4 表，

CPU 会去一个不确定的位置查表，大概率读到垃圾数据然后三重故障。

写 CR3 的副作用是使 TLB（Translation Lookaside Buffer，页表缓存）中所有非全局页的条目失效，

不过此时分页还没开启，TLB 里本来就没有有效条目，所以这个副作用无所谓。

**实现约束**：

用 `movl` 把立即数 0x1000 加载到 EAX，再把 EAX 写入 CR3。

CR3 不能直接接受立即数写入，必须通过通用寄存器中转。

**验证**：

GDB 中在 CR3 写入后设断点，用 `info registers cr3` 查看，应该显示 0x1000。

### Step 2: 第二步：启用 PAE

**目标**：

设置 CR4 的 bit 5（PAE，Physical Address Extension），让 CPU 支持四级页表结构。

**设计思路**：

CR4.PAE（bit 5，值 0x20）启用物理地址扩展，

让 CPU 支持 36 位以上的物理地址和四级页表结构。

Long Mode 强制要求 PAE——

没有 PAE 就无法使用 PML4→PDPT→PD→PT 的四级结构，

没有四级结构就无法进入 Long Mode。

Intel SDM Vol.3A §10.8.5 明确要求 PAE 必须在 EFER.LME 之前启用，这是状态机的硬性约束。

**实现约束**：

CR4 不能直接做位运算（`orl $0x20, %cr4` 是非法指令），

必须通过通用寄存器中转：

先 `movl %cr4, %eax` 读出来、用 `orl $CR4_PAE, %eax` 设置 bit 5、

再 `movl %eax, %cr4` 写回去。

这和上一章中 CR0 的操作方式一样，是 x86 ISA 的架构限制。

**验证**：

GDB 中用 `info registers cr4` 查看，应该包含 0x20（PAE 位）。

### Step 3: 第三步：设置 EFER.LME——最危险的一步

**目标**：

通过 MSR 0xC0000080（EFER 寄存器）设置 LME 位（bit 8，值 0x100），

告诉 CPU "我要进入 Long Mode"。

**设计思路**：

EFER（Extended Feature Enable Register）是一个 MSR（Model-Specific Register），地址 0xC0000080。

`rdmsr` 把 ECX 指定的 MSR 读入 EDX:EAX（高 32 位在 EDX，低 32 位在 EAX），

我们用 OR 指令设置 EAX 中的 bit 8（LME），然后 `wrmsr` 写回去。

EDX 在整个过程中保持不变——我们不需要修改 EFER 的高 32 位。

**踩坑预警**：

这一步是整个 Long Mode 切换中最容易出错的地方，也是我们踩过最大坑的地方。

我们在开发时把 EFER_LME 定义成了 0x1000 而不是 0x100。

0x1000 是 bit 12，对应 AMD 的 SVME 位。

`wrmsr` 不会报错——CPU 很乖地设置了 bit 12 而不是 bit 8。

然后继续执行后续步骤、开启分页——

CPU 在 CR0.PG 从 0 变成 1 的瞬间检查 EFER.LME 是否为 1，

发现 LME=0 但 PG=1，直接 #GP 然后三重故障。

GDB 里的线索是 EFER=0x1000，只有 SVME 没有 LME。

一个十六进制位的差距，CPU 不报错、汇编不报错、链接不报错，运行时直接炸。

**验证**：

GDB 中用 `info registers efer` 查看，应该包含 0x100（LME 位）。

如果看到 0x1000 而没有 0x100，那就是常量定义写错了。

### Step 4: 第四步：加载扩展 GDT

**目标**：

用 `lgdt` 加载包含 64 位描述符的扩展 GDT。

**设计思路**：

在远跳转之前必须加载包含 64 位描述符的 GDT。

因为接下来的 `ljmp $0x18, ...` 需要用选择子 0x18 去 GDT 查找描述符——

如果此时 GDTR 还指向旧的 3 条目 GDT（null、32 位代码、32 位数据），

偏移 0x18 处什么都没有，CPU 查到一个 null 描述符，直接 #GP。

`gdt64_ptr` 是新的 64 位 GDT 指针，

格式是 2 字节 Limit + 4 字节低地址 + 4 字节高地址（全零）。

**实现约束**：

GDT 指针用 `.long` + `.long` 而不是 `.quad`——

因为在 32 位 ELF（elf32-i386）里使用 `.quad` 存储 GDT 地址会触发链接器的 64 位重定位错误。

拆成两个 `.long` 之后，

低 32 位生成普通的 R_386_32 重定位，高 32 位是常量 0 不需要重定位。

GDT 本身在 stage2.S 的 `.gdt` section 中定义，包含 5 个描述符：

- null（0x00）
- 32 位代码段（0x08）
- 32 位数据段（0x10）
- 64 位代码段（0x18）
- 64 位数据段（0x20）

64 位代码段描述符的值是 0x00AF9A000000FFFF——

L=1 且 D=0 是让 CPU 进入真正 64 位模式的关键组合。

**踩坑预警**：

如果用了 32 位版的 `gdt_ptr` 而不是 64 位版的 `gdt64_ptr`，

GDT 里没有偏移 0x18 处的 64 位描述符，远跳转时 CPU 查到 null 描述符直接 #GP。

另外 64 位代码段的 L 位必须为 1、D 位必须为 0——

如果 L=0，即使 EFER.LME=1、CR0.PG=1，CPU 也只在兼容模式下运行；

如果 D=1 且 L=1，这是保留组合，行为未定义。

**验证**：

用 `objdump -s -j .gdt build/boot/stage2` 查看 GDT section 的原始字节，

确认偏移 0x18 处有 64 位代码段描述符。

### Step 5: 第五步：开启分页 + 远跳转

**目标**：

设置 CR0.PG（bit 31）开启分页，然后执行 `ljmp $0x18, $long_mode_entry` 远跳转到 64 位代码。

**设计思路**：

CR0.PG 从 0 变成 1 的瞬间是整个状态机的"激活点"。

CPU 检查 CR4.PAE=1、EFER.LME=1、CR3 指向合法 PML4 表——

全部满足后，CPU 自动设置 EFER.LMA（Long Mode Active，bit 10），分页正式生效。

由于我们的页表做的是恒等映射，

当前正在执行的代码的物理地址恰好同时是有效的虚拟地址和物理地址，CPU 能继续取到正确的指令。

分页生效后 CPU 进入 Compatibility Mode，

然后 `ljmp $0x18, $long_mode_entry` 远跳转刷新 CS，让 CPU 真正进入 64 位模式。

**实现约束**：

CR0 的写入值是 0x80000001，同时设置 PG 和 PE——

OR 操作不会清除已有的位，所以 PE 保持为 1。

选择子 0x18 的拆解是：

Index = 0x18 >> 3 = 3（GDT 第 4 个条目），TI = 0（使用 GDT），RPL = 0（Ring 0）。

远跳转之后的代码永远不会执行——`ljmp` 不返回。

**踩坑预警**：

这五步的顺序不可调换。

尤其是 PAE 必须在 LME 之前设置，LME 必须在 PG 之前设置。

如果反过来——比如先开 PG 再开 LME——

CPU 会因为你试图在没有 Long Mode 的情况下启用分页而直接 #GP。

**验证**：

GDB 中开启分页后查看 CR0 = 0x80000011（PE+PG），CR4 = 0x20（PAE），

EFER 包含 LME（0x100）和 LMA（0x400）。

## 64 位入口点初始化

### Step 6: 实现 long_mode_entry

**目标**：

在 stage2.S 中添加 `.code64` 段的 `long_mode_entry` 入口点，完成 64 位环境初始化。

**设计思路**：

远跳转执行后，CPU 进入 64 位模式，

CS 已经刷新为 64 位代码段选择子（0x18），

但 DS、ES、FS、GS、SS 这些数据段寄存器还残留着保护模式的旧值。

我们需要把它们全部重载为 64 位数据段选择子（0x20）。

接着用 `movabsq` 指令把一个 64 位立即数（0x90000）加载到 RSP 设置 64 位栈指针。

最后输出字符 `L`（0x4C）到 debugcon 端口 0xE9 作为验证。

**实现约束**：

`long_mode_entry` 需要声明为 `.global`，

因为 `enter_long_mode` 中的 `ljmp` 需要引用这个符号。

`.code64` 指令必须紧贴在 `long_mode_entry` 标签之前，

确保从这里开始生成 64 位指令编码。

`movabsq` 是 GAS 中加载 64 位立即数到寄存器的助记符，不是 `movq`——

`movq` 只能接受 32 位符号扩展立即数。

**验证**：

构建并运行 QEMU，检查 `debug.log` 文件内容。

如果里面同时出现 `P`（保护模式）和 `L`（Long Mode），

说明整个启动链条全部正确。

## 修改 stage2 调用链

在 stage2.S 的 `pm_entry`（保护模式入口点）中，输出 `P` 之后，需要调用新的两个函数。

调用顺序是先 `setup_page_tables` 再 `enter_long_mode`——

因为 `enter_long_mode` 内部会把 CR3 指向页表，所以页表必须提前建好。

`enter_long_mode` 不会返回（最后是一条 `ljmp` 跳走了），

所以它后面的 `cli; hlt` 只是保护性代码。

同时需要在 `boot/CMakeLists.txt` 中把 `boot_longmode` 目标的对象文件链接进 stage2——

在 `add_executable(stage2 ...)` 中追加 `$<TARGET_OBJECTS:boot_longmode>`。

用以下命令验证调用链的正确性：

```
objdump -d build/boot/stage2 | grep -A5 pm_entry
```

确认 pm_entry 之后有 `call setup_page_tables` 和 `call enter_long_mode`。

## 构建与运行

构建命令和之前一样，在项目根目录执行 CMake 构建。

运行后检查 `debug.log` 文件：

```
cat debug.log
```

预期输出是两个字符：先 `P`（保护模式切换成功，来自 tag 002），

然后 `L`（Long Mode 切换成功）。

如果什么都没有或者 QEMU 反复重启，说明切换过程中某一步出了问题。

## 调试技巧

在 GDB 中连接 QEMU（`target remote :1234`），

加载 ELF 文件（`file build/boot/stage2`），

在 `enter_long_mode` 的关键步骤之间设置断点，

逐步查看 CR0、CR4、EFER 的变化。

开启分页后的正确状态应该是 CR0 = 0x80000011（PE+PG），CR4 = 0x20（PAE），

EFER 包含 LME（0x100）和 LMA（0x400）。

如果遇到三重故障，用 `qemu-system-x86_64 -d int -no-reboot ...` 启动，

QEMU 会在每次异常时打印信息。

CS=0x0008 说明还在保护模式，异常发生在 `enter_long_mode` 内部；

CS=0x0018 说明已经进入 Long Mode 但后续出了问题。

## 本章小结

到这里 tag 003 的全部内容就完成了。

从保护模式到 Long Mode 的切换，核心就是五步状态机：

CR3 加载页表基址、CR4.PAE 启用物理地址扩展、EFER.LME 开启 Long Mode 使能、

lgdt 加载扩展 GDT、CR0.PG 开启分页 + ljmp 远跳转。

每一步都有严格的前置条件和顺序约束，任何一步出错都会导致三重故障。

验证方式很简单：构建并运行 QEMU，检查 `debug.log` 文件出现 `L` 字符。

| 概念 | 要点 |
|------|------|
| Long Mode 状态机 | CR3 → PAE → LME → LGDT → PG → ljmp，严格有序 |
| EFER.LME | MSR 0xC0000080 bit 8，值 0x100，不是 0x1000 |
| 64 位 GDT 描述符 | L=1, D=0，代码段值 0x00AF9A000000FFFF |
| 远跳转选择子 | 0x18（64 位代码段），触发真正 64 位模式 |
| GDT64 指针 | .long + .long 拼接 64 位基地址，避免 .quad 重定位 |
| 验证输出 | `outb 'L'` 到 debugcon 0xE9，debug.log 应出现 `L` |

## 参考资料

- Intel SDM Vol.3A §10.8.5 — Initializing IA-32e Mode：五步切换序列
- Intel SDM Vol.3A §10.8.5.3 — CS.L 和 CS.D 决定子模式：L=1/D=0 进入 64 位模式
- Intel SDM Vol.3A §2.5 — Control Registers：CR0.PG/PE、CR3、CR4.PAE 位定义
- Intel SDM Vol.3A §3.4.5 — Segment Descriptors：L 位和 D 位的含义
- OSDev Wiki: [Setting Up Long Mode](https://wiki.osdev.org/Setting_Up_Long_Mode)
- OSDev Wiki: [Global Descriptor Table](https://wiki.osdev.org/Global_Descriptor_Table)
