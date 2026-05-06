# 内核的第一次呼吸：BSS 清除陷阱、符号冲突侦探与 C++ 运行时

> 标签：kernel entry, boot.S, BSS clearing, C++ runtime, crt_stub, linker script, 全局构造函数
> 前置：[004C-1 从 Bootloader 到内核的交接](004-boot-load-mini-kernel-c-1.md)

## 写在前面

在上一篇文章里，我们看着 bootloader 在 Long Mode 中把 BootInfo 填好，往 debugcon 敲了一个 `'J'`，然后 `jmp *%rax` 头也不回地跳进了 `0xFFFFFFFF80020000`。从那一刻起，CPU 执行的就是我们自己写的内核代码了。

但内核要真正"活过来"，还得经历一系列初始化步骤：关中断、开 SSE、设栈、清 BSS、调用全局构造函数，最后才能进入 C++ 的 `main` 函数。这些步骤中的每一步都有它存在的理由，而其中两步——BSS 清除和全局构造函数调用——藏着非常经典的坑，每一个都足够让你调试到凌晨三点。说实话，这两个 bug 是我写 Cinux 以来印象最深的调试经历：第一个是 `rep stosb` 指令意外破坏了 `%rdi` 中保存的 BootInfo 指针，第二个是链接器把汇编定义的 `.bss` 符号和 C++ 全局变量分配到了同一个地址。后者产生了一个神秘的值 `0x2a00000000`（等于 42 左移 32 位），光看这个值你完全不知道它从哪来的——直到你发现代码里有个全局变量被赋值为 42。

本文覆盖内核侧的全部代码：`kernel/mini/arch/x86_64/boot.S`（内核入口汇编序列）、`kernel/mini/arch/x86_64/crt_stub.cpp`（C++ 运行时支撑函数）、`kernel/mini/linker.ld`（链接器脚本和内存布局）、`kernel/mini/main.cpp`（内核主函数及 C++ 特性验证），以及 `kernel/mini/CMakeLists.txt`（构建配置）。在讲解过程中，我会把那两个 bug 的调试故事融入对应的代码段落——不是为了吐槽，而是因为这些踩坑经历本身就是理解底层机制最好的教材。

## 环境说明

内核运行环境的关键参数：CPU 已经处于 64-bit Long Mode，中断控制器还没有初始化（IDT 不存在），没有物理内存管理器（PMM），没有堆分配器。编译选项是 `-ffreestanding -fno-exceptions -fno-rtti -mcmodel=large -mno-red-zone`——每一个都在告诉编译器"这是内核代码，别给我用那些花哨的用户态特性"。链接脚本把内核的 VMA 设在 `0xFFFFFFFF80020000`（高半核地址），LMA 设在 `0x20000`（物理加载地址）。

预期 debugcon 输出是一个精心设计的字符序列：`OPLJ123G4===CPPGC1V123B===END`。每个字符都有明确的含义——`OPLJ` 是 bootloader 阶段的输出（上一篇的内容），`1234` 是内核启动序列中四个检查点的输出，`G` 是全局构造函数执行时的输出，`===CPPGC1V123B===END` 是 C++ 特性验证的输出。如果这个序列完整出现，说明从 bootloader 到内核的全部代码路径都是正确的。

## 链接器脚本：内核内存布局的蓝图

我们先从 `kernel/mini/linker.ld` 开始看，因为它定义了所有其他代码运行的地址空间基础。不搞清楚链接器脚本，后面的 BSS 清除和符号冲突你根本没法理解。

```
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;
```

`OUTPUT_FORMAT` 指定输出为 ELF64 x86-64 格式。虽然最终会用 `objcopy -O binary` 转成扁平二进制，但编译链接阶段仍然需要完整的 ELF 格式——链接器需要 section header 来定位符号、解析重定位。`ENTRY(_start)` 告诉链接器入口点是 `_start`，这个信息在扁平二进制中会被丢弃（扁平二进制从文件头开始执行），但在 ELF 格式中有意义。

两个关键常量：`KERNEL_PHYS_BASE = 0x20000` 是 bootloader 加载内核的物理地址，`KERNEL_Virt_BASE = 0xFFFFFFFF80000000` 是高半核的虚拟基址。起始 VMA 设为两者之和 `0xFFFFFFFF80020000`，这就是 `_start` 最终被链接到的虚拟地址。

```
    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text.start)        /* _start must be first! */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
```

`.text` 段放代码和只读数据。`AT(ADDR(.text) - KERNEL_Virt_BASE)` 计算 LMA——因为 VMA 起始于 `KERNEL_Virt_BASE + KERNEL_PHYS_BASE`，减去 `KERNEL_Virt_BASE` 后得到 `KERNEL_PHYS_BASE = 0x20000`。`*(.text.start)` 放在最前面，确保 `_start` 函数始终在输出文件的开头。VMA 和 LMA 的分离是高半核设计的核心：代码在物理内存的 0x20000 处，但 CPU 通过页表映射从 `0xFFFFFFFF80020000` 访问它。

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

`.data` 段紧接着 `.text` 段，存放有初始值的全局变量。`.init_array` 是独立的段，不在 `.data` 内部——这样链接器可以分别管理两个段的布局。`KEEP()` 包裹 `.init_array` 防止被链接器的 section garbage collection 丢弃。

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

注意 `.data` 在 `.bss` 之前——这个顺序在后面修复符号冲突 bug 时非常关键。因为 `.data` 和 `.bss` 是完全独立的 section，它们的地址空间不重叠，所以把 `__boot_info_ptr` 放在 `.data` 段就永远不会被 BSS 清除操作触及。

## boot.S 入口序列：每一步都事出有因

`kernel/mini/arch/x86_64/boot.S` 是内核的入口点。bootloader 通过 `jmp *%rax` 跳入 `_start`，此时 `%rdi` 中保存着 BootInfo 指针（0x7000），CPU 处于 Long Mode，但中断状态不确定。

### 第一步：关中断

```asm
.section .text
.code64

.global _start
.type _start, @function

_start:
    cli
    movb $0x31, %al
    outb %al, $0xE9
```

第一步是 `cli` 关中断。此时内核还没有设置 IDT（中断描述符表），如果此时发生了任何硬件中断（定时器、键盘中断等），CPU 会尝试通过 IDT 查找处理程序——但 IDT 还不存在，结果就是一个 Triple Fault，QEMU 直接重启。`cli` 确保在 IDT 设置完成之前不会有任何中断打扰。

然后往 debugcon 输出 `'1'`（0x31 = ASCII '1'），表示 `_start` 已经到达。这是第一个内核侧的 debugcon 输出，和 bootloader 的 `'J'` 之间不应该有任何其他字符。

### 第二步：设栈

```asm
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp
    movb $0x32, %al
    outb %al, $0xE9
```

设置栈指针。`__mini_stack_top` 定义在 boot.S 的 `.bss` 段末尾，是一个 8KB 栈区的顶端。x86 的栈是向下增长的（push 递减 `%rsp`），所以栈顶是高地址。`xorq %rbp, %rbp` 把帧指针清零——这是 C++ 运行时的约定：`main` 函数的 `%rbp` 为 0 表示没有调用者（栈回溯的终止条件）。输出 `'2'` 表示栈已就绪。

### 第三步：保存 BootInfo 指针（真正的坑从这里开始）

接下来就是整个内核启动中最容易出 bug 的地方。先看代码：

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

BSS 清除是 C++ 运行时的硬性要求——C/C++ 标准规定所有未初始化的全局变量和静态变量的初始值必须为零。在用户态程序中，操作系统加载 ELF 时会把 BSS 段清零（Linux 的 `load_elf_binary` 会做这件事），但在我们的场景中，bootloader 只是把 flat binary 原封不动地搬到了 0x20000，没有任何人帮你清零 BSS。OSDev Wiki 的 Multiboot 规范页面也提到，GRUB 加载 ELF 内核时会保证 BSS 被清零，但 Cinux 使用的是自写 bootloader 加载 flat binary，所以必须自己来。

`rep stosb` 的语义（Intel SDM Vol. 2A, "STOS/STOSB/STOSD/STOSQ"）是：以 `%rdi` 为目标地址，以 `%rax` 为填充值（这里是 0），以 `%rcx` 为计数器，逐字节写入并自动递增 `%rdi`。执行完毕后，`%rdi` 不再是 BSS 起始地址了——它已经递增到了 BSS 末尾之后。

这就是第一个陷阱：`rep stosb` 会破坏 `%rdi`。如果你在 `rep stosb` 之后试图从 `%rdi` 读取 BootInfo 指针，拿到的是一个垃圾地址。所以第一行 `movq %rdi, __boot_info_ptr` 至关重要——它必须在 BSS 清除之前执行。

但事情到这里还没完。真正的坑在后面。

### BSS 清除的第二个陷阱：指针被自己清零

我们最初把 `__boot_info_ptr` 定义在 `.bss` 段：

```asm
.section .bss
.global __boot_info_ptr
.skip 8
__boot_info_ptr:
```

你看到问题了吗？`.bss` 段的内容会被 `rep stosb` 整体清零——也就是说，你在 BSS 清除之前把 BootInfo 指针写到了 `__boot_info_ptr`，然后 `rep stosb` 把它又清回了 0。等你在 `mini_kernel_main` 里去读 `__boot_info_ptr` 的时候，拿到的是 0 而不是 0x7000。

这个 bug 的调试过程相当曲折。最初我们怀疑是 bootloader 传递的 `%rdi` 值就是错的，于是在 `_start` 入口处加了 debugcon 输出，确认 `%rdi = 0x7000` 没问题。然后又在 `movq %rdi, __boot_info_ptr` 之后立即读取 `__boot_info_ptr` 的值做输出，发现也是 0x7000——到这里保存是成功的。但是 BSS 清除之后再读，值变成了一个诡异的 `0x2a00000000`。

等等，`0x2a00000000`？不是 0？如果你只是在 `.bss` 里存了 0x7000 然后 BSS 清零，结果应该是 0 才对。但实际值不是 0——它是 `42 << 32`。42 是什么？这就引出了我们遇到过的最离奇的 bug。

### 符号冲突侦探故事：0x2a00000000 的谜团

42 这个数字在我们的测试代码里出现过——`global_construction_count = 42`，这是 `GlobalCounter` 全局对象构造函数里的赋值。一个巧合？绝对不是。

用 `objdump -t` 检查符号表，真相才浮出水面：

```
ffffffff800226e8 g       .bss  0000000000000000 __boot_info_ptr
ffffffff800226e8 g     O .bss  0000000000000001 global_counter
ffffffff800226ec l     O .bss  0000000000000004 _ZL25global_construction_count
```

`__boot_info_ptr` 和 `global_counter` 被链接器分配到了同一个地址 `0xffffffff800226e8`。

为什么会这样？问题出在汇编定义 `.bss` 符号的方式上。注意我们的定义：

```asm
.section .bss
.global __boot_info_ptr
.skip 8
__boot_info_ptr:
```

`.skip 8` 在标签之前。GNU AS 的行为是 `.skip` 在当前位置分配空间，然后标签指向 `.skip` 之后的地址。但链接器在合并来自不同目标文件的 `.bss` 段时，看到的是一个没有大小的符号——因为它只是一个标签，不像 C++ 的全局变量有明确的大小信息（`global_counter` 有 `O` 标记和大小 1）。链接器在分配 `.bss` 空间时，`__boot_info_ptr` 的地址被分配到了和 `global_counter` 相同的位置。

执行流程是这样的：BSS 清除之后，`__boot_info_ptr` 的内存被清零。然后 `_init_global_ctors` 被调用，`GlobalCounter` 的构造函数执行，`global_construction_count = 42`（写入 `0x800226ec`，4 字节）。因为 `__boot_info_ptr` 和 `global_counter` 共享地址 `0x800226e8`，当构造函数往 `global_construction_count`（紧邻，偏移 4 字节）写入 42 时，`__boot_info_ptr` 的 8 字节视图里看到了低 4 字节为 0（来自 `global_counter` 的默认值）、高 4 字节为 42（来自 `global_construction_count`），合在一起就是 `0x0000002a00000000`。

这个调试技巧值得记住：当你看到一个"神秘值"时，把它转成二进制看看是否和代码中的常量有关。`0x2a00000000 = 42 << 32`，而 42 = 0x2A 是我们代码中 `global_construction_count` 的值——这个线索直接指向了内存覆盖。类似地，如果你看到 `0xDEADBEEF`，那很可能是未初始化内存的填充值；如果看到 `0xFFFFFFFF`，可能是 -1 的无符号表示。

### 修复：将 `__boot_info_ptr` 移到 `.data` 段

修复方案是把 `__boot_info_ptr` 从 `.bss` 移到 `.data` 段：

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

这个 bug 的教训是：当汇编和 C++ 混合编程时，避免将汇编定义的符号放在 `.bss` 段。原因是多方面的：`.bss` 段会被运行时清零，破坏你保存的值；链接器可能将不同来源的符号分配到同一地址；C++ 全局变量的初始化可能覆盖汇编符号。把关键数据放在 `.data` 段，用 `objdump -t` 检查符号表确认没有地址冲突——这两步可以避免绝大多数此类问题。

## 全局构造函数调用：C++ 的隐藏基础设施

BSS 清除完成后，内核的内存状态终于干净了——所有未初始化的全局变量都被清零。接下来要做的是调用全局对象的构造函数。

```asm
    call _init_global_ctors

    movb $0x34, %al
    outb %al, $0xE9
```

输出 `'4'` 表示全局构造完成。

在有标准 C++ 运行时的环境中（比如 Linux 用户态），`crt0.o`（C Runtime Startup）会在调用 `main` 之前自动遍历 `.init_array` 段，调用所有注册的初始化函数。但我们用的是 `-nostdlib -ffreestanding`，没有任何 C 运行时代码——`_start` 是程序真正的入口，没有人帮我们做这件事。如果不去调用全局构造函数，`GlobalCounter global_counter` 这个全局对象的构造函数永远不会执行，`global_construction_count` 永远是 0，所有依赖全局初始化的代码都会出错。

## crt_stub.cpp：freestanding 环境下的 C++ 运行时支撑

`kernel/mini/arch/x86_64/crt_stub.cpp` 提供了 C++ 在 freestanding 环境下必须存在的一组运行时函数。这些函数在有 libstdc++ 的环境中由标准库提供，但在内核开发中必须手动实现。

```cpp
extern "C" {

[[noreturn]] void __cxa_pure_virtual() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

[[noreturn]] void __stack_chk_fail() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}
```

`__cxa_pure_virtual` 在通过虚函数表调用纯虚函数时被触发——这永远是一个编程错误。在用户态程序中它通常会打印错误信息然后 `abort()`，在内核中我们只能停机。即使没有任何代码直接调用它，vtable 的生成也会隐式引用它，所以链接器要求它必须存在。

`__stack_chk_fail` 是栈保护检测到溢出时的处理函数。`__cxa_atexit` 是 `atexit` 的底层实现——在内核中我们不支持"程序退出"，所以直接返回 0（假装注册成功）。有些编译器生成的析构代码会引用它，链接器也需要这个符号存在。

接下来是核心函数——全局构造函数初始化：

```cpp
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    for (void (**func)() = __init_array_start; func != __init_array_end; func++) {
        (*func)();
    }
}
```

`__init_array_start` 和 `__init_array_end` 是链接器脚本中定义的符号，标记了 `.init_array` 段的起止。`.init_array` 段的内容是一个函数指针数组——GCC 为每个有非平凡构造函数的全局对象生成一个名为 `_GLOBAL__sub_I_XXX` 的函数，这个函数负责调用该对象的构造函数，然后把函数指针放在 `.init_array` 段中。

遍历方式很直接：从 `__init_array_start` 开始，逐个调用函数指针，直到 `__init_array_end`。这是 OSDev Wiki 上推荐的 "ARM BPABI method"——简单、可靠、不依赖 `.init` 段的级联调用链。OSDev Wiki 的 [Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors) 页面详细描述了两种方式：一种是 System V ABI 的 `crt*.o` 级联方案（`crti.o` 提供 prologue，`crtbegin.o` 插入调用代码，`crtn.o` 提供 epilogue），另一种是直接遍历 `.init_array` 的函数指针数组。我们选择了后者，因为它只需要 5 行代码，不需要提供自定义的 `crti.o` 和 `crtn.o`。

然后是 `operator new/delete`：

```cpp
}  // extern "C" ends here

void operator delete(void* ptr) noexcept {
    (void)ptr;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这些操作符的 halt 实现看起来很激进——调用 `new` 或 `delete` 直接停机。但在当前阶段这是合理的：内核还没有物理内存管理器（PMM），没有任何动态内存分配的基础设施。如果代码试图 `new` 一个对象，那一定是个 bug，停机是最好的选择——比起返回 `nullptr` 然后在某个遥远的地方触发空指针解引用，直接 halt 能让你立刻定位问题所在。

注意 `new/delete` 必须放在 `extern "C"` 块之外——它们需要 C++ 的 name mangling，因为 `operator new` 和 `operator delete` 是通过 C++ 的重载解析机制来匹配的。如果你把它们包在 `extern "C"` 里，链接器会找不到正确的 mangled 名称，报 missing symbol 错误。

### 与 Linux 内核的 initcall 机制对比

Linux 内核也使用类似的机制来调用初始化函数，但远比我们复杂。Linux 定义了多个 level 的 initcall：`pure_initcall`、`core_initcall`、`postcore_initcall`、`arch_initcall`、`subsys_initcall`、`fs_initcall`、`device_initcall`、`late_initcall`——每个 level 对应不同的初始化优先级，内核启动时按 level 顺序依次调用。这些 level 的底层实现和 Cinux 一样是通过链接器脚本中的 section 排列来保证顺序的（`__initcall0_start`、`__initcall1_start` 等），只不过 Linux 把一个 `.init_array` 拆成了多个子区间。

Linux 之所以需要这么复杂的分级，是因为它有数百个初始化函数，而且有些初始化之间存在严格的依赖关系——比如中断控制器必须在设备驱动之前初始化，内存管理必须在 slab 分配器之前就绪。Cinux 作为一个教学操作系统，目前只有一两个全局构造函数，一个扁平的 `.init_array` 就够了。但如果后续内核规模增长，Linux 的多级 initcall 方案是一个很好的参考。

### 与 xv6 的对比

xv6 的入口代码（`entry.S`）比 Cinux 简单得多，原因是它省略了两个步骤。第一，xv6 不需要手动清 BSS——它依赖 GRUB（Multiboot 协议）加载 ELF 内核，Multiboot 规范要求 bootloader 在加载 ELF 时清零 BSS 段。第二，xv6 不需要调用全局构造函数——它是纯 C 写的，没有 `.init_array`，没有全局对象。

xv6 的入口序列只有四步：开启 4MB 大页（CR4.PSE）、设置页目录（CR3）、开启分页（CR0.PG）、设栈，然后直接 `jmp *%eax` 跳到 `main()`。Cinux 的入口序列多了 SSE 初始化、BSS 清除和全局构造函数调用——每一步都是因为 C++ 和自写 bootloader 这两个设计选择带来的额外复杂度。这些复杂度不是没有回报的：C++ 的类、虚函数、RAII 等特性让内核代码更安全和更有表达力，而自写 bootloader 让我们从头到尾理解了 x86 启动的每一个环节。

## 内核主函数：C++ 特性验证

`kernel/mini/main.cpp` 是内核的 C++ 主函数。在当前 tag 中，它的主要职责不是做什么有用的工作，而是验证 C++ 的各种特性在 freestanding 环境下是否正常工作。

```cpp
extern "C" {
extern uint64_t __boot_info_ptr;
}

static void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}
```

`__boot_info_ptr` 声明为 `uint64_t`（不是指针类型），因为它是 `boot.S` 中 `.data` 段的一个 8 字节变量，里面存的是 BootInfo 的地址值。把它声明为 `extern uint64_t` 然后强制转换为 `BootInfo*` 是最直接的做法。

第一个测试是简单的类构造和析构，验证最基本的 C++ 特性——成员初始化列表、构造函数调用、成员函数调用都能正常工作。构造时输出 `'C'` 加上 value 的数字（比如 `C1`）。

第二个测试是虚函数和多态——`Base` 是一个抽象类（有两个纯虚函数），`Derived` 继承并实现了它们。通过基类指针调用虚函数需要 vtable 的间接寻址。如果链接器没有正确生成 vtable（比如缺少 `__cxa_pure_virtual` 的定义），或者 `.rodata` 段的地址映射有问题，这里就会崩溃。输出 `'V'` 表示 Derived 的构造函数被调用。

第三个测试是全局对象构造——`global_counter` 是一个全局对象，它的构造函数不是由 `mini_kernel_main` 中的代码显式调用的，而是由 `_init_global_ctors` 遍历 `.init_array` 时间接调用的。GCC 会生成一个 `_GLOBAL__sub_I_global_counter` 函数放在 `.init_array` 中，这个函数调用 `GlobalCounter::GlobalCounter()`。这就是我们在符号冲突故事中提到的那个全局对象——正是它的 `global_construction_count = 42` 导致了 `__boot_info_ptr` 被覆盖为 `0x2a00000000`。

最后看 `mini_kernel_main` 的主体：

```cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('C');
    debugcon_putc('P');
    debugcon_putc('P');

    SimpleClass obj1(1);
    if (obj1.getValue() == 1 && obj1.getMarker() == 'S') {
        debugcon_putc('1');
    }

    Derived derived(5);
    Base* base = &derived;
    if (base->getName() == 'D' && base->compute() == 10) {
        debugcon_putc('2');
    }

    if (global_counter.getCount() == 42) {
        debugcon_putc('3');
    }

    if (boot_info->entry_point == 0xFFFFFFFF80020000
        && boot_info->kernel_phys_base == 0x20000) {
        debugcon_putc('B');
    }

    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('E');
    debugcon_putc('N');
    debugcon_putc('D');

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

这里有一个值得注意的设计选择：`mini_kernel_main` 接收 `boot_info_addr` 参数（来自 `%rdi`），但实际使用的是 `__boot_info_ptr`（来自 `.data` 段）。`boot_info_addr` 被 `(void)` 显式忽略。这样做的原因是使用 `.data` 段中保存的值更可靠——它不受 System V ABI 调用约定的 volatile 寄存器规则影响，也不会被 BSS 清除或符号冲突破坏（修复之后）。

BootInfo 验证检查了两个字段：`entry_point` 应该是 `0xFFFFFFFF80020000`，`kernel_phys_base` 应该是 `0x20000`。如果这两个值正确，说明 BootInfo 从 bootloader 到内核的传递链条是完整的——bootloader 在 `stage2.S` 中填充、通过 `%rdi` 传递、`boot.S` 保存到 `.data` 段、`main.cpp` 读回来验证。

## 构建配置

`kernel/mini/CMakeLists.txt` 定义了小内核的构建规则，编译选项每一个都有存在的理由：

```cmake
target_compile_options(mini_kernel PRIVATE
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)
```

`-ffreestanding` 告诉编译器这是 freestanding 环境（没有标准库），禁止隐式引用标准库函数。`-fno-exceptions` 和 `-fno-rtti` 禁用异常和运行时类型信息——这两者在内核中既不必要也有性能开销。`-fno-pie` 禁用位置无关可执行文件——内核加载地址是固定的，不需要 PIC 重定位。`-mcmodel=large` 允许代码访问任意虚拟地址——默认的 `small` 内存模型假设所有符号都在低 2GB 地址空间内，但我们的内核在高半核地址，超出这个范围。`-mno-red-zone` 禁用 x86-64 的 Red Zone 优化——内核代码随时可能被中断打断，Red Zone（栈指针以下 128 字节不可触碰区域）在中断上下文中不安全。

链接后用 `objcopy -O binary` 把 ELF 转成扁平二进制——这个命令剥离所有 ELF header 和 section header，只保留各段的原始内容，按 LMA 排列输出。bootloader 直接把 flat binary 加载到物理地址 0x20000 后，代码的相对偏移就是正确的。

## 验证

构建并运行 QEMU：

```bash
cd build && cmake .. && make -j$(nproc)
make run
```

然后检查 `debug.log` 文件。如果一切正常，你会看到完整的输出序列：

```
OPLJ123G4===CPPGC1V123B===END
```

逐字符解读：

- `OPLJ`：bootloader 阶段完成
- `1`：内核 `_start` 到达
- `2`：栈设置完成
- `3`：BSS 清除完成
- `G`：全局构造函数执行（`GlobalCounter` 构造函数输出）
- `4`：全局构造完成
- `===CPP`：C++ 特性测试开始
- `C1`：简单类构造验证通过
- `V`：虚函数类构造
- `2`：虚函数调用验证通过
- `3`：全局对象验证通过
- `B`：BootInfo 验证通过
- `===END`：全部测试通过

如果序列在某处中断，根据最后一个字符就能定位问题所在。比如停在 `OPLJ12` 说明 BSS 清除出了问题（没看到 `3`），停在 `123G` 说明全局构造之后的恢复出了问题（没看到 `4`），停在 `===CPPC1V` 说明虚函数之后的某个验证失败了。

## 收尾

回过头看，这一章我们做了内核侧的完整启动流程：从 `_start` 入口到 C++ `main` 函数，经历关中断、设栈、保存 BootInfo 指针、清 BSS、调用全局构造函数这些步骤。过程中遇到了两个非常经典的 bug——`rep stosb` 破坏 `%rdi` 是理解 x86 字符串指令语义的好教材，而符号地址冲突（`0x2a00000000 = 42 << 32`）则是理解链接器行为和段布局的绝佳案例。

设计上有几个值得记住的决策：`__boot_info_ptr` 放在 `.data` 段而不是 `.bss` 段，是为了避免 BSS 清除和符号冲突的双重打击；`new`/`delete` 的 halt 实现是在没有 PMM 阶段最安全的做法；直接遍历 `.init_array` 比使用 `crt*.o` 级联更简单可靠。

与 xv6 和 Linux 的对比揭示了"自写 bootloader + C++ 内核"这个组合带来的额外复杂度——xv6 依赖 GRUB 清 BSS 所以不需要这步，Linux 用 `%r15` 保存参数所以不需要内存存储。但正是这些额外的步骤让我们对 x86 启动的底层机制有了更深入的理解。

到这里，tag 004C 的全部工作就完成了。从 Bootloader 到内核的整条链路——E820 内存探测（004A）、ELF 加载（004B）、BootInfo 握手与高半核跳转（004C-1）、内核初始化与 C++ 运行时（004C-2）——已经全部打通。接下来的 tag 将会让内核开始做真正有用的事情：建立中断处理、实现物理内存管理、加载真正的内核。到那时，我们今天搭好的 C++ 运行时基础就会派上真正的用场了。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 2A, "STOS/STOSB/STOSD/STOSQ -- Store String": `REP STOSB` 的完整语义——`%rdi` 为目标地址，`%rax` 为填充值，`%rcx` 为计数器，每写一字节自动递增 `%rdi`。方向标志 DF（由 `cld`/`std` 控制）决定递增还是递减。这是理解 BSS 清除破坏 `%rdi` 这一 bug 的关键。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 3.4.3 -- System V AMD64 ABI Calling Convention: 第一个整数参数在 `%rdi`，callee-saved 寄存器为 `%rbx, %rbp, %r12~%r15`。`%rdi` 是 caller-saved（易失的），函数调用后可能被修改。Cinux 把 BootInfo 指针存在 `.data` 段而不是依赖寄存器保存，规避了这个易失性问题。

- OSDev Wiki -- Calling Global Constructors: `.init_array` 段的遍历方法详解，包括 ARM BPABI 风格（直接遍历函数指针数组）和传统 GNU 风格（`crti.o`/`crtbegin.o` 级联）。Cinux 使用前者。
  https://wiki.osdev.org/Calling_Global_Constructors

- OSDev Wiki -- C++ Bare Bones: freestanding 环境下必须提供的运行时函数清单（`__cxa_pure_virtual`、`__cxa_atexit`、`operator new/delete`），以及 `-ffreestanding`、`-fno-exceptions`、`-fno-rtti` 编译选项的说明。
  https://wiki.osdev.org/C%2B%2B_Bare_Bones

- OSDev Wiki -- Multiboot Specification: GRUB 为 ELF 内核清除 BSS 的保证。Cinux 不使用 GRUB，所以必须手动清零——这也是触发 BSS 清除 bug 的根本原因。
  https://wiki.osdev.org/Multiboot

- Linux Kernel `arch/x86/kernel/head_64.S`: Linux 的 `startup_64` 入口序列，对比 Cinux 的 `boot.S`。Linux 把 boot_params 保存到 `%r15`（callee-saved），依赖 decompressor 清零 BSS，使用 `__START_KERNEL_map` 作为高半核基址。
  https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S

- xv6 `entry.S` (MIT): xv6 的入口代码——比 Cinux 简单得多，因为 xv6 依赖 GRUB（Multiboot）完成 BSS 清零，且是纯 C（没有 `.init_array` 和全局构造函数）。
  https://github.com/mit-pdos/xv6-public/blob/master/entry.S
