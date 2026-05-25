---
title: 004-boot-load-mini-kernel-c-2 · 内核加载 (C)
---

# 004 通读版 · 内核启动、BSS 陷阱与符号冲突

## 概览

在上一篇文章里，我们看着 bootloader 在 Long Mode 中把 BootInfo 填好，往 debugcon 敲了一个 `'J'`，然后 `jmp *%rax` 头也不回地跳进了 0xFFFFFFFF80020000。从那一刻起，CPU 执行的就是我们自己写的内核代码了。但内核要真正"活过来"，还得经历一系列初始化步骤：关中断、开 SSE、设栈、清 BSS、调用全局构造函数，最后才能进入 C++ 的 `main` 函数。

本文覆盖内核侧的核心代码——`kernel/mini/linker.ld`（链接器脚本和内存布局）、`kernel/mini/arch/x86_64/boot.S`（内核入口汇编序列，含 SSE 启用、BSS 清除和符号冲突修复）。下一篇覆盖 C++ 运行时支撑（`crt_stub.cpp`）、`main.cpp`（C++ 特性验证）、构建配置和设计决策。

在这个过程中，我们会遇到两个非常经典的内核启动 bug：`rep stosb` 指令意外破坏了 `%rdi` 中保存的 BootInfo 指针，以及链接器把汇编定义的 `.bss` 符号和 C++ 全局变量分配到了同一个地址。这两个 bug 的调试过程分别记录在 `document/notes/004-C/` 目录下，我会在讲到相关代码时把踩坑经历融入讲解。

关键设计决策一览：BootInfo 指针保存在 `.data` 段而非 `.bss` 段，以避免 BSS 清除时被意外清零；`.init_array` 的全局构造函数由内核自身遍历调用；链接脚本使用 `AT()` 指定物理加载地址（LMA），VMA 则指向高半核地址空间。

## 架构图

内核入口到 C++ `main` 的完整执行序列，以及每一步的寄存器/内存状态变化：

```
  Bootloader 跳入内核
  %rdi = 0x7000 (BootInfo*)
  %rip = 0xFFFFFFFF80020000 (_start)
     |
     v
  boot.S: _start
     ├── cli                    关中断
     ├── outb '1'               debugcon: 到达 _start
     ├── movq stack_top, %rsp   设栈 (8KB, 在 .bss 中)
     ├── outb '2'               debugcon: 栈就绪
     ├── movq %rdi -> .data     保存 BootInfo 指针到 __boot_info_ptr
     ├── rep stosb              清除 .bss 段 (注意: 会破坏 %rdi!)
     ├── outb '3'               debugcon: BSS 清除完成
     ├── call _init_global_ctors 调用 .init_array 中的全局构造函数
     ├── outb '4'               debugcon: 全局构造完成
     ├── movq .data -> %rdi     从 __boot_info_ptr 恢复 BootInfo*
     └── call mini_kernel_main  进入 C++ 主函数
           |
           v
     main.cpp: mini_kernel_main
        ├── 输出 "===CPP"
        ├── Test 1: SimpleClass (构造/析构) -> "C1"
        ├── Test 2: 虚函数/vtable -> "V", "2"
        ├── Test 3: 全局对象验证 -> "3"
        ├── BootInfo 验证 -> "B"
        └── 输出 "===END", hlt
```

内核的内存布局（链接器脚本定义）：

```
  虚拟地址 (VMA)                    物理地址 (LMA)    段名      内容
  ──────────────────────────────────────────────────────────────────
  0xFFFFFFFF80020000               0x20000          .text     _start 入口
  0xFFFFFFFF80020xxx               ...              .text     其他代码
  0xFFFFFFFF80022xxx               ...              .rodata   只读数据
  ──────────────────────────────────────────────────────────────────
  0xFFFFFFFF800226e0               ...              .data     __boot_info_ptr
  0xFFFFFFFF800226e8              ...              .data     .init_array (全局构造函数指针)
  ──────────────────────────────────────────────────────────────────
  0xFFFFFFFF800227000              --               .bss      global_counter 等
  ...                               ...              .bss      __mini_stack (8KB)
  0xFFFFFFFF800229000              --               .bss      __mini_stack_top
```

预期 debugcon 输出：`OPLJ123G4===CPPGC1V123B===END`

- `OPLJ`：bootloader 阶段（O = Stage2 OK, P = Protected Mode, L = Long Mode, J = Jump）
- `123G4`：内核入口序列（1 = _start 到达, 2 = 栈就绪, 3 = BSS 清除, G = 全局构造函数输出, 4 = 全局构造完成）
- `===CPPGC1V123B===END`：C++ 特性验证（`===CPP` = 开始标记, `C1` = 类构造, `G` = 全局对象, `1` = 简单类验证, `V` = 虚函数构造, `2` = 虚函数验证, `3` = 全局对象验证, `B` = BootInfo 验证, `===END` = 结束标记）

## 代码精讲

### 链接器脚本——内核内存布局的蓝图

我们先从 `kernel/mini/linker.ld` 开始看，因为它定义了所有其他代码运行的地址空间基础。

```
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;
```

`OUTPUT_FORMAT` 指定输出为 ELF64 x86-64 格式。虽然最终会用 `objcopy -O binary` 转成扁平二进制，但编译链接阶段仍然需要完整的 ELF 格式——链接器需要 section header 来定位符号、解析重定位。

`ENTRY(_start)` 告诉链接器入口点是 `_start`，这个信息在扁平二进制中会被丢弃（扁平二进制从文件头开始执行），但在 ELF 格式中有意义——如果你用 QEMU 的 `-kernel` 参数直接加载 ELF 文件，QEMU 会自动跳到 `_start`。

两个关键常量：`KERNEL_PHYS_BASE = 0x20000` 是 bootloader 加载内核的物理地址，`KERNEL_Virt_BASE = 0xFFFFFFFF80000000` 是高半核的虚拟基址。起始 VMA 设为两者之和 `0xFFFFFFFF80020000`，这就是 `_start` 最终被链接到的虚拟地址。

```
    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text.start)        /* _start must be first! */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
```

`.text` 段放代码和只读数据。`AT(ADDR(.text) - KERNEL_Virt_BASE)` 计算 LMA（Load Memory Address）——因为 VMA 是 `KERNEL_Virt_BASE + KERNEL_PHYS_BASE = 0xFFFFFFFF80020000`，减去 `KERNEL_Virt_BASE` 后得到 `KERNEL_PHYS_BASE = 0x20000`。这就是 bootloader 把内核 flat binary 放到内存中的实际物理位置。`*(.text.start)` 放在 `*(.text .text.*)` 之前，确保 `_start` 函数始终在输出文件的最前面——boot.S 把 `_start` 放在 `.text.start` 段中。

```
    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }
```

`.data` 段紧接着 `.text` 段，存放有初始值的全局变量。`.init_array` 是独立的段，不在 `.data` 内部——这样链接器可以分别管理两个段的布局。`KEEP()` 包裹 `.init_array` 防止被链接器的 section garbage collection 丢弃。LMA 都用 `ADDR() - KERNEL_Virt_BASE` 计算，确保物理地址连续排列。

`__init_array_start` 和 `__init_array_end` 是两个链接器符号，标记了 `.init_array` 段的起止位置。GCC 为每个有构造函数的全局对象在 `.init_array` 中生成一个函数指针（名为 `_GLOBAL__sub_I_XXX`），内核的启动代码需要遍历这个数组，依次调用每个函数指针来初始化全局对象。

```
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __mini_kernel_end = .;
```

`.bss` 段不需要 `AT()`——BSS 段在 ELF 文件中不占空间（它只是记录了大小和起始地址），运行时由启动代码负责清零。`__bss_start` 和 `__bss_end` 标记了 BSS 的边界，`boot.S` 中的 `rep stosb` 指令就用这两个符号来确定要清除的内存范围。

`/DISCARD/` 段丢弃了 `.comment`、`.note` 和 `.eh_frame`——这些是编译器生成的调试和注解信息，内核不需要。

### 内核入口序列——boot.S 的每一步

`kernel/mini/arch/x86_64/boot.S` 是内核的入口点。bootloader 通过 `jmp *%rax` 跳入 `_start`，此时 `%rdi` 中保存着 BootInfo 指针（0x7000），CPU 处于 Long Mode，中断已关闭（bootloader 跳转前没有显式 `cli`，但我们不能依赖这一点）。

```asm
.section .text.start, "ax"
.code64

.global _start
.type _start, @function

_start:
    /* Disable interrupts */
    cli

    /* Enable SSE: set CR4.OSFXSR (bit 9) and CR4.OSXMMEXCPT (bit 10) */
    movq %cr4, %rax
    orq $(1 << 9), %rax          /* OSFXSR: enable FXSAVE/FXRSTOR */
    orq $(1 << 10), %rax         /* OSXMMEXCPT: enable SIMD #XF */
    movq %rax, %cr4
    clts                          /* Clear CR0.TS (Task Switched) */

    /* Output '1' to debugcon - _start reached */
    movb $0x31, %al              /* '1' */
    outb %al, $0xE9
```

第一步是 `cli` 关中断。此时内核还没有设置 IDT（中断描述符表），如果此时发生了任何硬件中断（定时器、键盘中断等），CPU 会尝试通过 IDT 查找处理程序——但 IDT 还不存在，结果就是一个 Triple Fault，QEMU 直接重启。`cli` 确保在 IDT 设置完成之前不会有任何中断打扰。

紧接着是启用 SSE。Long Mode 下 SSE 不是默认开启的——我们需要在 CR4 寄存器中设置两个位：OSFXSR（bit 9）告诉 CPU 操作系统支持 FXSAVE/FXRSTOR 指令，OSXMMEXCPT（bit 10）告诉 CPU 操作系统能处理 SIMD 浮点异常。`clts` 指令清除 CR0 的 TS（Task Switched）位，避免使用 SSE 指令时触发 Device Not Available 异常（#NM）。虽然当前的测试代码碰巧不依赖浮点运算，但编译器可能会在某些场景下生成 SSE 指令（比如结构体拷贝优化），不启用 SSE 的话这些指令会触发 #NM 导致 Triple Fault。

然后往 debugcon 输出 `'1'`（0x31 = ASCII '1'），表示 `_start` 已经到达。这是第一个内核侧的 debugcon 输出，和 bootloader 的 `'J'` 之间不应该有任何其他字符。

```asm
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp
    movb $0x32, %al
    outb %al, $0xE9
```

设置栈指针。`__mini_stack_top` 定义在 boot.S 的 `.bss` 段末尾，是一个 8KB 栈区的顶端。x86 的栈是向下增长的（push 递减 %rsp），所以栈顶是高地址。`xorq %rbp, %rbp` 把帧指针清零——这是 C++ 运行时的约定：`main` 函数的 `%rbp` 为 0 表示没有调用者（栈回溯的终止条件）。

输出 `'2'` 表示栈已就绪。到目前为止，一切都还算平淡——接下来就是真正刺激的部分了。

### BSS 清除与 BootInfo 指针的惊险时刻

现在我们来到整个内核启动中最容易出 bug 的地方。先看代码：

```asm
    movq %rdi, __boot_info_ptr

    movq $__bss_start, %rdi
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb

    movb $0x33, %al
    outb %al, $0xE9
```

这里做的事情很简单：先把 `%rdi`（BootInfo 指针 = 0x7000）保存到 `__boot_info_ptr`，然后把 `%rdi` 设为 BSS 起始地址，用 `rep stosb` 逐字节地把 BSS 段全部清零。

但这里有两个非常隐蔽的陷阱，每一个都足够让你调试到凌晨三点。

第一个陷阱是 `rep stosb` 指令本身对 `%rdi` 的破坏。`rep stosb` 的语义是：以 `%rdi` 为目标地址，以 `%rax` 为填充值（这里是 0），以 `%rcx` 为计数器，逐字节写入并自动递增 `%rdi`。执行完毕后，`%rdi` 不再是 BSS 起始地址了——它已经递增到了 BSS 末尾之后。这意味着如果我们在 `rep stosb` 之后试图从 `%rdi` 读取 BootInfo 指针，拿到的是一个垃圾地址。

所以第一行 `movq %rdi, __boot_info_ptr` 至关重要——它必须在 BSS 清除之前执行。但是这里还有第二个陷阱，更隐蔽，也更让人血压拉满。

第二个陷阱是关于 `__boot_info_ptr` 应该放在哪个段。最初我们把 `__boot_info_ptr` 定义在 `.bss` 段：

```asm
.section .bss
.global __boot_info_ptr
.skip 8
__boot_info_ptr:
```

`.bss` 段的内容会被 `rep stosb` 整体清零——也就是说，你在 BSS 清除之前把 BootInfo 指针写到了 `__boot_info_ptr`，然后 `rep stosb` 把它又清回了 0。等你在 `mini_kernel_main` 里去读 `__boot_info_ptr` 的时候，拿到的是 0 而不是 0x7000。

这个 bug 的调试过程非常曲折。最初我们怀疑是 bootloader 传递的 `%rdi` 值就是错的，于是在 `_start` 入口处加了 debugcon 输出，确认 `%rdi = 0x7000` 没问题。然后又在 `movq %rdi, __boot_info_ptr` 之后立即读取 `__boot_info_ptr` 的值做输出，发现也是 0x7000——到这里保存是成功的。但是 BSS 清除之后再读，值变成了一个诡异的 `0x2a00000000`。

`0x2a00000000` 这个值不是 0，而是 `42 << 32`。42 是什么？如果你看过 `main.cpp` 里的全局构造函数测试，`global_construction_count` 被赋值为 42。这个"巧合"指向了第三个层次的问题——符号地址冲突。

### 符号冲突：当链接器把两个变量放在同一个地址

前面说到 `__boot_info_ptr` 在 BSS 清除后变成了 `0x2a00000000` 而不是 0。按理说 BSS 清除应该把它清成 0 才对。用 `objdump -t` 检查符号表，真相才浮出水面：

```
ffffffff800226e8 g       .bss  0000000000000000 __boot_info_ptr
ffffffff800226e8 g     O .bss  0000000000000001 global_counter
ffffffff800226ec l     O .bss  0000000000000004 _ZL25global_construction_count
```

`__boot_info_ptr` 和 `global_counter` 被链接器分配到了同一个地址 `0xffffffff800226e8`。

为什么会这样？问题出在汇编定义 `.bss` 符号的方式上：

```asm
.section .bss
.global __boot_info_ptr
.skip 8
__boot_info_ptr:
```

`.skip 8` 在标签之前。GNU AS 的行为是 `.skip` 在当前位置分配空间，然后标签指向 `.skip` 之后的地址。但链接器在合并来自不同目标文件的 `.bss` 段时，看到的是一个没有大小的符号（因为它只是一个标签，不像 C++ 的全局变量有明确的大小信息）。链接器在分配 `.bss` 空间时，`__boot_info_ptr` 的地址被分配到了和 `global_counter` 相同的位置。

执行流程是这样的：BSS 清除之后，`__boot_info_ptr` 的内存被清零。然后 `_init_global_ctors` 被调用，`GlobalCounter` 的构造函数执行，`global_construction_count = 42`（写入 0x800226ec，4 字节）。因为 `__boot_info_ptr` 和 `global_counter` 共享地址 0x800226e8，`global_counter` 是 1 字节对象，而 `__boot_info_ptr` 是 8 字节。当构造函数往 `global_construction_count`（0x800226ec，紧邻）写入 42 时，由于地址重叠，`__boot_info_ptr` 的 8 字节视图里看到了低 4 字节为 0（来自 `global_counter` 的默认值）、高 4 字节为 42（来自 `global_construction_count`），合在一起就是 `0x0000002a00000000`。

这个 bug 的修复方案是把 `__boot_info_ptr` 从 `.bss` 移到 `.data` 段：

```asm
.section .data
.global __boot_info_ptr
.align 8
__boot_info_ptr:
    .quad 0
```

`.data` 段不会被 BSS 清除操作触及，而且 `.data` 段和 `.bss` 段在链接器脚本中是完全独立的 section，不会有地址重叠的问题。`.quad 0` 给了一个显式的初始值，让链接器知道这个符号占用 8 字节的空间，大小信息明确。修复之后 `objdump -t` 确认 `__boot_info_ptr` 在 `.data` 段有了独立的地址：

```
ffffffff800226e8 g     O .data 0000000000000008 __boot_info_ptr
ffffffff800226f4 g     O .bss  0000000000000001 global_counter
```

两个符号现在各有各的地址，再也不会互相踩踏了。

下一篇我们将覆盖全局构造函数调用机制、C++ 运行时支撑函数（crt_stub.cpp）、内核主函数和 C++ 特性验证、以及构建配置。

