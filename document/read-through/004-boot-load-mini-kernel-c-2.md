# 004 通读版 · 内核启动与 C++ 运行时

## 概览

在上一篇文章里，我们看着 bootloader 在 Long Mode 中把 BootInfo 填好，往 debugcon 敲了一个 `'J'`，然后 `jmp *%rax` 头也不回地跳进了 0xFFFFFFFF80020000。从那一刻起，CPU 执行的就是我们自己写的内核代码了。但内核要真正"活过来"，还得经历一系列初始化步骤：关中断、开 SSE、设栈、清 BSS、调用全局构造函数，最后才能进入 C++ 的 `main` 函数。

本文覆盖内核侧的全部代码——`kernel/mini/arch/x86_64/boot.S`（内核入口汇编序列）、`kernel/mini/arch/x86_64/crt_stub.cpp`（C++ 运行时支撑函数）、`kernel/mini/linker.ld`（链接器脚本和内存布局）、`kernel/mini/main.cpp`（内核主函数及 C++ 特性验证），以及 `kernel/mini/CMakeLists.txt`（构建配置）。

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
    .text : AT(KERNEL_PHYS_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
```

`.text` 段放代码和只读数据。`AT(KERNEL_PHYS_BASE)` 指定 LMA（Load Memory Address）为 0x20000——这是 bootloader 把内核 flat binary 放到内存中的实际物理位置。VMA 是 0xFFFFFFFF80020000，这是 CPU 执行时看到的虚拟地址。VMA 和 LMA 的分离是高半核设计的核心：代码在物理内存的 0x20000 处，但 CPU 通过页表映射从 0xFFFFFFFF80020000 访问它。

```
    .data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) {
        *(.data .data.*)
        __init_array_start = .;
        *(.init_array .init_array.*)
        __init_array_end = .;
    }
```

`.data` 段紧接着 `.text` 段，存放有初始值的全局变量。LMA 设为 `KERNEL_PHYS_BASE + SIZEOF(.text)`，意思是数据段的物理位置紧贴在代码段之后——这在扁平二进制中是自然的布局（代码在前，数据在后，顺序排列）。

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

### 全局构造函数调用——C++ 的隐藏基础设施

BSS 清除完成后，内核的内存状态终于干净了——所有未初始化的全局变量都被清零。接下来要做的是调用全局对象的构造函数。

```asm
    call _init_global_ctors

    movb $0x34, %al
    outb %al, $0xE9
```

`_init_global_ctors` 定义在 `crt_stub.cpp` 中。在解释它的实现之前，我们先理解为什么需要手动调用全局构造函数。

在有标准 C++ 运行时的环境中（比如 Linux 用户态），`crt0.o`（C Runtime Startup）会在调用 `main` 之前自动遍历 `.init_array` 段，调用所有注册的初始化函数。但我们用的是 `-nostdlib -ffreestanding`，没有任何 C 运行时代码——`_start` 是程序真正的入口，没有人帮我们做这件事。如果不去调用全局构造函数，`GlobalCounter global_counter` 这个全局对象的构造函数永远不会执行，`global_construction_count` 永远是 0，所有依赖全局初始化的代码都会出错。

### crt_stub.cpp——freestanding 环境下的 C++ 运行时支撑

`kernel/mini/arch/x86_64/crt_stub.cpp` 提供了 C++ 在 freestanding 环境下必须存在的一组运行时函数。这些函数在有 libstdc++ 的环境中由标准库提供，但在内核开发中必须手动实现。

```cpp
extern "C" {

[[noreturn]] void __cxa_pure_virtual() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`__cxa_pure_virtual` 在通过虚函数表调用纯虚函数时被调用——这永远是一个编程错误。在用户态程序中，它通常会打印一条错误信息然后调用 `abort()`。在内核中我们只能停机。实际上，如果你设计得当，这个函数永远不会被触发，但链接器要求它必须存在——即使没有任何代码直接调用它，vtable 的生成也会隐式引用它。

```cpp
[[noreturn]] void __stack_chk_fail() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}
```

`__stack_chk_fail` 是栈保护（Stack Canary）检测到溢出时的处理函数。如果编译时启用了 `-fstack-protector`，编译器会在函数栈帧中插入一个随机 canary 值，函数返回前检查 canary 是否被改写。改写意味着发生了栈溢出（通常是缓冲区越界写入），此时 `__stack_chk_fail` 被调用。

`__cxa_atexit` 是 `atexit` 的底层实现，用于注册程序退出时的清理函数。在内核中我们不支持"程序退出"这个概念——内核是一个永不终止的循环，所以这个函数直接返回 0（假装注册成功）。有些编译器生成的析构代码会引用它，即使析构函数永远不会被调用，链接器也需要这个符号存在。

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

遍历方式很直接：从 `__init_array_start` 开始，逐个调用函数指针，直到 `__init_array_end`。这是 OSDev Wiki 上推荐的 "ARM BPABI method"——简单、可靠、不依赖 `.init` 段的级联调用链。Linux 内核也使用几乎相同的机制（`do_initcalls`），只不过它分了多个 level（`__initcall_start` 到 `__initcall_end`，按优先级排列）。

然后是 `operator new/delete`：

```cpp
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

这些操作符的 halt 实现看起来很激进——调用 `new` 或 `delete` 直接停机。但在当前阶段这是合理的：内核还没有物理内存管理器（PMM），没有任何动态内存分配的基础设施。如果代码试图 `new` 一个对象，那一定是个 bug，停机是最好的选择。

`operator delete` 和 `operator delete(void*, unsigned long)` 两个重载都需要提供——C++14 引入了 sized deallocation，如果只提供一个，链接器会报 missing symbol 错误。同样，`operator new[]`（数组 new）也需要提供，否则使用数组的代码会链接失败。

注意 `new/delete` 必须放在 `extern "C"` 块之外——它们需要 C++ 的 name mangling，因为 `operator new` 和 `operator delete` 是通过 C++ 的重载解析机制来匹配的。如果你把它们包在 `extern "C"` 里，链接器会找不到正确的 mangled 名称。

### 内核主函数——C++ 特性验证

`kernel/mini/main.cpp` 是内核的 C++ 主函数。在当前 tag 中，它的主要职责不是做什么有用的工作，而是验证 C++ 的各种特性在 freestanding 环境下是否正常工作。

```cpp
extern "C" {
extern uint64_t __boot_info_ptr;
}

static void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}
```

`__boot_info_ptr` 声明为 `uint64_t`（不是指针类型），因为它是 `boot.S` 中 `.data` 段的一个 8 字节变量，里面存的是 BootInfo 的地址值。把它声明为 `extern uint64_t` 然后强制转换为 `BootInfo*` 是最直接的做法。`debugcon_putc` 是一个内联汇编辅助函数，往 debugcon 端口（0xE9）输出一个字符。

第一个测试——简单的类构造和析构：

```cpp
class SimpleClass {
private:
    int value;
    char marker;

public:
    SimpleClass(int v) : value(v), marker('S') {
        debugcon_putc('C');
        debugcon_putc('0' + v);
    }

    ~SimpleClass() {
        debugcon_putc('D');
        debugcon_putc('0' + value);
    }

    int getValue() const { return value; }
    char getMarker() const { return marker; }
};
```

`SimpleClass` 有两个成员变量和一个初始化列表构造函数。构造时输出 `'C'` 加上 value 的数字（比如 `C1`），析构时输出 `'D'` 加上 value。这个类验证了最基本的 C++ 特性：成员初始化列表、构造函数调用、成员函数调用。如果 SSE 没有正确启用（boot.S 中有一段 SSE 初始化代码——实际上在当前版本中 SSE 启用被放在了后续 tag 中，但这里的代码碰巧不依赖 SSE），或者栈没有对齐，构造函数就会在访问成员变量时触发 Page Fault 或 General Protection Fault。

第二个测试——虚函数和多态：

```cpp
class Base {
public:
    virtual char getName() = 0;
    virtual int compute() = 0;
    virtual ~Base() {}
};

class Derived : public Base {
private:
    int multiplier;

public:
    Derived(int m) : multiplier(m) {
        debugcon_putc('V');
    }

    virtual char getName() override { return 'D'; }
    virtual int compute() override { return multiplier * 2; }
    virtual ~Derived() override { debugcon_putc('d'); }
};
```

虚函数测试验证了 vtable 机制在 freestanding 环境下是否正常。`Base` 是一个抽象类（有两个纯虚函数），`Derived` 继承并实现了它们。关键是 `Base* base = &derived; base->getName()` 这行——通过基类指针调用虚函数，需要 vtable 的间接寻址。如果链接器没有正确生成 vtable（比如缺少 `__cxa_pure_virtual` 的定义），或者 `.rodata` 段的地址映射有问题，这里就会崩溃。输出 `'V'` 表示 Derived 的构造函数被调用。

第三个测试——全局对象构造：

```cpp
static int global_construction_count = 0;

class GlobalCounter {
public:
    GlobalCounter() {
        global_construction_count = 42;
        debugcon_putc('G');
    }

    int getCount() const { return global_construction_count; }
};

GlobalCounter global_counter;
```

`global_counter` 是一个全局对象——它的构造函数不是由 `mini_kernel_main` 中的代码显式调用的，而是由 `_init_global_ctors` 遍历 `.init_array` 时间接调用的。GCC 会生成一个 `_GLOBAL__sub_I_global_counter` 函数放在 `.init_array` 中，这个函数调用 `GlobalCounter::GlobalCounter()`。如果 `.init_array` 的起止符号（`__init_array_start`/`__init_array_end`）没有在链接器脚本中正确定义，或者 `KEEP()` 没有防止被 strip，全局构造就不会执行。

这个全局对象也和前面讲到的符号冲突 bug 直接相关——正是 `global_construction_count = 42` 的写入导致了 `__boot_info_ptr` 被覆盖为 `0x2a00000000`（42 << 32）。修复之后，`global_construction_count` 在 `.bss` 段有了自己独立的地址，`__boot_info_ptr` 在 `.data` 段安然无恙。

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

这里有一个值得注意的设计选择：`mini_kernel_main` 接收 `boot_info_addr` 参数（来自 `%rdi`），但实际使用的是 `__boot_info_ptr`（来自 `.data` 段）。`boot_info_addr` 被 `(void)` 显式忽略。这样做的原因是 `boot_info_addr` 经过了 BSS 清除和全局构造函数调用两个阶段，虽然理论上 `%rdi` 的值在进入 `mini_kernel_main` 时应该已经被正确设置（boot.S 在调用前从 `__boot_info_ptr` 恢复了 `%rdi`），但使用 `.data` 段中保存的值更可靠——它不受 System V ABI 调用约定的 volatile 寄存器规则影响。

BootInfo 验证检查了两个字段：`entry_point` 应该是 `0xFFFFFFFF80020000`（高半核入口地址），`kernel_phys_base` 应该是 `0x20000`。如果这两个值正确，说明 BootInfo 从 bootloader 到内核的传递链条是完整的——bootloader 在 stage2.S 中填充、通过 `%rdi` 传递、boot.S 保存到 `.data` 段、main.cpp 读回来验证。

### 构建配置

`kernel/mini/CMakeLists.txt` 定义了小内核的构建规则：

```cmake
add_executable(mini_kernel
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    main.cpp
)
```

源文件列表包含了三个文件：`boot.S`（内核入口汇编）、`crt_stub.cpp`（C++ 运行时支撑）、`main.cpp`（内核主函数）。CMake 会自动识别 `.S` 文件需要用 GCC 的汇编模式编译（带 C 预处理），`.cpp` 文件用 C++ 编译。

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

这些编译选项每一个都有存在的理由。`-ffreestanding` 告诉编译器这是 freestanding 环境（没有标准库），禁止隐式引用标准库函数。`-fno-exceptions` 和 `-fno-rtti` 禁用异常和运行时类型信息——这两者在内核中既不必要也有性能开销（异常需要 `.eh_frame` 段和栈展开表，RTTI 需要 `typeid` 的额外数据）。

`-fno-pie` 禁用位置无关可执行文件——内核加载地址是固定的（0x20000），不需要 PIC 重定位。`-mcmodel=large` 允许代码访问任意虚拟地址——默认的 `small` 内存模型假设所有符号都在低 2GB 地址空间内，但我们的内核在高半核地址（0xFFFFFFFF80000000），超出这个范围。`-mno-red-zone` 禁用 x86-64 的 Red Zone 优化——内核代码随时可能被中断打断，Red Zone（栈指针以下 128 字节不可触碰区域）在中断上下文中不安全。

```cmake
target_link_options(mini_kernel PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -no-pie
)

add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel>
        $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)
```

链接选项中，`-T linker.ld` 指定链接脚本，`-nostdlib` 不链接标准库和启动文件。构建完成后用 `objcopy -O binary` 把 ELF 转成扁平二进制——这个命令剥离所有 ELF header 和 section header，只保留各段的原始内容，按 VMA 排列输出。因为链接脚本中 `.text` 的 LMA 是 0x20000，`objcopy -O binary` 输出的文件从 LMA=0 开始排列（flat binary 没有 LMA 概念），所以 bootloader 直接把 flat binary 加载到物理地址 0x20000 后，代码的相对偏移就是正确的。

## 设计决策

### 决策：`__boot_info_ptr` 放在 `.data` 还是 `.bss`

**问题**：bootloader 通过 `%rdi` 传递的 BootInfo 指针需要在 BSS 清除之后、进入 C++ main 之前被保存和恢复。这个指针应该存放在哪个段？

**本项目的做法**：`.data` 段，用 `.quad 0` 显式初始化。BSS 清除只影响 `.bss` 段，`.data` 段的数据不受影响。

**备选方案**：Linux 内核的做法是把 boot_params 指针保存到 `%r15`（callee-saved 寄存器），不涉及内存存储。`%r15` 在函数调用过程中不会被破坏，可以一直持有到需要的时候。

**为什么不选备选方案**：使用寄存器保存的问题在于，一旦有任何函数调用，你就必须考虑调用约定——虽然 `%r15` 是 callee-saved（被调用者保存），但如果你在 `_init_global_ctors` 的调用链中不小心使用了一个不遵守约定的汇编代码段（这在内核开发中很常见），寄存器值就可能被破坏。`.data` 段的内存存储不受函数调用的影响，只要没有代码显式修改它，值就是安全的。而且在 `mini_kernel_main` 中直接用全局变量 `__boot_info_ptr` 比传递寄存器值更方便——C++ 代码不需要依赖特定的寄存器约定。

**如果要扩展/改进，应该怎么做**：当内核的正式启动流程建立后（有 MMU、有正式的栈帧管理），可以考虑用 Linux 风格的寄存器传递，并在 C++ 入口处立即把 BootInfo 拷贝到一个 C++ 对象中（不再是裸指针），避免全局变量的生命周期管理问题。

### 决策：手动遍历 `.init_array` vs 使用 `.init` 段级联

**问题**：全局对象的构造函数应该通过什么机制调用？

**本项目的做法**：直接遍历 `.init_array` 段的函数指针数组，从 `__init_array_start` 到 `__init_array_end`，逐个调用。这是 OSDev Wiki 上称为 "ARM BPABI method" 的方式。

**备选方案**：传统的 GNU 工具链使用 `.init` 段的级联调用：`crti.o` 提供 `.init` 段的 prologue，`crtbegin.o` 把 `.init_array` 的调用代码插入 `.init` 段，`crtend.o` 提供调用代码的尾部，`crtn.o` 提供 epilogue。链接时这些片段按顺序合并，形成完整的初始化函数。

**为什么不选备选方案**：级联调用需要提供 `crti.o` 和 `crtn.o` 的自定义版本（标准的来自 glibc，不能直接用），增加了构建系统的复杂度。直接遍历 `.init_array` 只需要 5 行代码，没有任何外部依赖。两种方式的效果完全相同——都是按顺序调用 `.init_array` 中的函数指针。

**如果要扩展/改进，应该怎么做**：如果后续需要支持构造优先级（比如某些全局对象必须在其他对象之前初始化），可以像 Linux 内核一样定义多个 level（`initcall0`、`initcall1` 等），每个 level 是 `.init_array` 的一个子区间，按 level 顺序调用。

### 决策：`new`/`delete` halt vs 返回错误

**问题**：在还没有内存管理器的阶段，`operator new` 应该怎么实现？

**本项目的做法**：halt（死循环）。调用 `new` 就意味着遇到了编程错误，直接停机。

**备选方案**：返回 `nullptr`。调用者应该检查返回值并做错误处理——但 C++ 的 `new` 默认不应该返回 `nullptr`（那是 `nothrow new` 的行为），返回 `nullptr` 会导致未定义行为。

**为什么不选备选方案**：在内核环境中，如果代码试图动态分配内存但分配器还没准备好，这是一个设计错误而不是运行时错误——你不应该在还没有 PMM 的代码路径中调用 `new`。停机可以立即暴露问题，而返回 `nullptr` 可能导致后续的空指针解引用，报错位置和实际出错位置相差很远，更难调试。

**如果要扩展/改进，应该怎么做**：当 PMM 和堆分配器上线后，替换这些 stub 为真正的内存分配实现。`operator new` 调用内核的 `kmalloc()`，`operator delete` 调用 `kfree()`。可以保留 halt 版本作为 fallback，用 `#ifdef` 在分配器就绪后切换。

## 扩展方向

1. **添加 SSE/AVX 初始化**（难度：中等）——当前 `boot.S` 没有启用 SSE。如果 C++ 代码使用了浮点运算或 SIMD 指令（比如编译器自动向量化），会触发 `#NM`（Device Not Available）异常。需要在 `_start` 中设置 CR4.OSFXSR（bit 9）和 CR4.OSXMMEXCPT（bit 10），然后执行 `fninit` 初始化 x87 FPU 状态。

2. **实现简单的堆分配器**（难度：中等）——替换 `operator new` 和 `operator delete` 的 halt 实现为基于 BSS 末尾的 bump allocator（只分配不释放）。这样可以验证 `new`/`delete` 在 freestanding 环境下的完整工作流。

3. **添加析构函数测试**（难度：简单）——当前 `mini_kernel_main` 在 `while(1) hlt` 中结束，局部对象的析构函数永远不会执行。可以在 halt 前手动调用析构函数（或者用 `placement new` 在指定地址构造对象），验证析构函数链的正确性。

4. **内核命令行解析**（难度：中等）——在 BootInfo 中添加 `cmdline` 字段，bootloader 从 Multiboot 或固定位置读取内核命令行字符串，内核解析后控制行为（比如 `debug=verbose` 开启详细日志）。

5. **使用 `__attribute__((constructor))` 指定优先级**（难度：简单）——在 `crt_stub.cpp` 中添加一个带 `__attribute__((constructor(101)))` 的函数，验证 `.init_array` 的优先级排序是否按预期工作（数值小的先执行）。GCC 会把不同优先级的构造函数放在 `.init_array` 的不同子段中。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 2A, "STOS/STOSB/STOSD/STOSQ -- Store String": `REP STOSB` 的完整语义——`%rdi` 为目标地址，`%rax` 为填充值，`%rcx` 为计数器，每写一字节自动递增 `%rdi`。方向标志 DF（由 `cld`/`std` 控制）决定递增还是递减。这是理解 BSS 清除破坏 `%rdi` 这一 bug 的关键。

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 3.4.3 -- System V AMD64 ABI Calling Convention: 第一个整数参数在 `%rdi`，callee-saved 寄存器为 `%rbx, %rbp, %r12~%r15`。`%rdi` 是 caller-saved（易失的），函数调用后可能被修改。Cinux 把 BootInfo 指针存在 `.data` 段而不是依赖寄存器保存，规避了这个易失性问题。

- OSDev Wiki -- Calling Global Constructors: `.init_array` 段的遍历方法详解，包括 ARM BPABI 风格（直接遍历函数指针数组）和传统 GNU 风格（`crti.o`/`crtbegin.o` 级联）。Cinux 使用前者。
  https://wiki.osdev.org/Calling_Global_Constructors

- OSDev Wiki -- C++ Bare Bones: freestanding 环境下必须提供的运行时函数清单（`__cxa_pure_virtual`、`__cxa_atexit`、`operator new/delete`），以及 `-ffreestanding`、`-fno-exceptions`、`-fno-rtti` 编译选项的说明。
  https://wiki.osdev.org/C%2B%2B_Bare_Bones

- OSDev Wiki -- Multiboot Specification: GRUB 为 ELF 内核清除 BSS 的保证（Multiboot 规范要求 bootloader 在加载 ELF 时清零 BSS 段）。Cinux 不使用 GRUB，所以必须手动清零——这也是触发 BSS 清除 bug 的根本原因。
  https://wiki.osdev.org/Multiboot

- Linux Kernel `arch/x86/kernel/head_64.S`: Linux 的 `startup_64` 入口序列，对比 Cinux 的 `boot.S`。Linux 把 boot_params 保存到 `%r15`（callee-saved），依赖 decompressor 清零 BSS，使用 `__START_KERNEL_map` 作为高半核基址（和 Cinux 的 `0xFFFFFFFF80000000` 相同）。
  https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S

- xv6 `entry.S` (MIT): xv6 的入口代码——比 Cinux 简单得多，因为 xv6 依赖 GRUB（Multiboot）完成 BSS 清零，且是纯 C（没有 `.init_array` 和全局构造函数）。xv6 使用 `V2P_WO(entry)` 计算物理入口地址。
  https://github.com/mit-pdos/xv6-public/blob/master/entry.S
