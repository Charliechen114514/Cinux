# 009-1 Tutorial: Higher-Half 大内核启动 — 从链接脚本到 C++ 入口

> 标签：x86-64, higher-half kernel, linker script, boot.S, VMA/LMA, ELF, CRT stub
> 前置：[008 大内核加载](./008-load-large-kernel-3.md)

## 前言

说实话，上一章（008）做完之后我心里一直不太踏实——mini kernel 已经能把大内核 ELF 从磁盘读出来、解析 PT_LOAD 段、搬运到内存里了，但大内核本身还不存在。我们一直在对着一个空壳 ELF 测试加载器，跳转过去的后果就是 triple fault——因为没有代码在那里等着。这一章我们要做的事情就是让大内核真正"站起来"，从 `0xFFFFFFFF80000000` 这个虚拟地址开始呼吸。

你要问为什么非得用这么高的地址？Mini kernel 在物理地址上跑得好好的，干嘛要折腾？原因在于现代操作系统的地址空间规划——内核住在虚拟地址空间的最高端，用户进程住在低端，两边互不干扰。这不仅是 Linux 的做法，也是 Windows、macOS、所有"正经"OS 的做法。我们选择的 `0xFFFFFFFF80000000` 这个地址恰好是 Linux 的 `__START_KERNEL_map`，这么做不是巧合——我们想在地址空间布局上和 Linux 保持一致，这样后续讲解用户空间切换、系统调用的时候，读者脑中的模型就和对 Linux 的理解是对得上的。

## 环境说明

和之前一样，我们的工具链是 GNU AS（AT&T 语法）+ GCC/G++ + CMake。大内核用 `-ffreestanding -nostdlib -nostartfiles` 编译，没有标准库、没有异常、没有 RTTI。运行环境是 QEMU（`-serial stdio -m 8G -accel kvm`）。大内核被 mini kernel 从磁盘加载到物理地址 `0x1000000`（16MB），在 identity mapping 下执行。

## 第一步——编写链接脚本：在虚拟和物理之间搭桥

我们现在要做的是定义大内核的内存布局。这个链接脚本要解决一个看似矛盾的问题：代码的"家"在虚拟地址 `0xFFFFFFFF80000000`，但 CPU 现在不认识这个地址——它还在 mini kernel 的 identity mapping 下运行。所以我们需要在 ELF 文件里同时记录两套地址：虚拟地址（VMA）给链接器用，物理地址（LMA）给加载器用。

```ld
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA   = 0xFFFFFFFF80000000;
KERNEL_LMA   = 0x1000000;

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;
```

开头的伪操作声明了输出格式和架构。`ENTRY(_start)` 指定入口点——这个符号在 boot.S 里定义。两个地址常量是关键：`KERNEL_VMA` 是 higher-half 虚拟基地址，`KERNEL_LMA` 是 16MB 物理地址（要和 mini kernel 的 `BIG_KERNEL_LOAD_ADDR` 一致，不然加载器搬过去的段对不上）。

`. = KERNEL_VMA + KERNEL_LMA` 这行把位置计数器设为 `0xFFFFFFFF80100000`。之后所有 section 的 VMA 从这个值开始递增。但光设 VMA 还不够——如果 LMA 也是这个天文数字，ELF 加载器就要往 `0xFFFFFFFF80100000` 写数据，CPU 根本不认这个地址。所以每个 section 都用 `AT()` 指定了独立的 LMA：

```ld
    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) {
        *(.text.start)
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA) ALIGN(4096) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_VMA) ALIGN(8) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end   = .;
    }
```

`AT(ADDR(.text) - KERNEL_VMA)` 做了一件事：把 VMA 减去 `0xFFFFFFFF80000000` 得到纯物理偏移。比如 `.text` 的 VMA 是 `0xFFFFFFFF80101000`，那 LMA 就是 `0x101000`。ELF 的 program header 中 `p_vaddr` 是高地址（将来开分页后用），`p_paddr` 是低地址（现在 identity mapping 下用），两边都不耽误。

`.text.start` 必须放在 `.text` 最前面——确保 `_start` 出现在 ELF 文件第一个字节。`.init_array` 用了 `KEEP()` 防止被链接器的 `--gc-sections` 当垃圾回收——这些函数指针是被 `_init_global_ctors` 通过 linker script 符号间接引用的，链接器发现不了这个依赖。

你会发现这种 VMA/LMA 分离的手法在教学 OS 里很常见。xv6 的做法更简单粗暴——它根本不做 higher-half，整个内核在物理地址上跑，链接脚本就是一句 `. = 0x100000`（x86 版）或者 `. = 0x80000000`（RISC-V 版，QEMU 的默认加载地址）。SerenityOS 则走了另一条路——它的链接脚本不做 VMA/LMA 分离（`AT(ADDR(.text))` 即 VMA == LMA），页表映射由 Prekernel 代码在运行时建立。Cinux 的方案介于两者之间：在链接脚本中显式分离，但不需要 Prekernel 这样的额外启动阶段。

```ld
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;

    .stack (NOLOAD) : ALIGN(4096) {
        . = . + 0x4000;
        __kernel_stack_top = .;
    }
```

`.bss` 不需要 `AT()` 因为它不占文件空间——启动汇编负责清零。栈分配了 16KB（`0x4000`），`NOLOAD` 表示不产生文件内容。Linux 的 head_64.S 也用类似的方式定义栈，但 Linux 的栈来自 per-CPU 的 `current_task` 结构，远比我们的固定 16KB 复杂。

## 第二步——编写 boot.S：六步启动序列

链接脚本搭好了"舞台"，现在该让演员上场了。boot.S 是大内核执行的第一段代码——mini kernel 通过 `jmp *entry` 跳转过来，CPU 从这里开始执行。

```asm
.section .text.start, "ax"
.code64

.global _start
.type   _start, @function

_start:
    cli                           # disable interrupts
```

放在 `.text.start` section 确保出现在 ELF 文件最开头。`.code64` 因为 mini kernel 在 64 位长模式下跳过来。第一条指令 `cli` 禁用中断——大内核还没有设置自己的 IDT，任何硬件中断都会触发 CPU 查 mini kernel 的 IDT，执行旧的回调大概率崩溃。Linux 的 startup_64 也做了同样的事情，但 Linux 的处理更复杂——它在跳到 C 代码之前还要设置 GDT、加载 IDT、配置 CR4、处理 SME 加密。

```asm
    movq  $__kernel_stack_top, %rsp
    xorq  %rbp, %rbp
```

设置栈。`$__kernel_stack_top` 取链接脚本定义的栈顶地址（16KB 栈空间的最高地址，x86-64 栈向下增长）。`xorq %rbp, %rbp` 清零基址指针标记调用链底部。这个模式和 Linux 几乎一模一样——Linux 也是先设置栈再往下走，只不过 Linux 的栈来源不同（从 per-CPU 数据结构获取）。

```asm
    movq  $__bss_start, %rdi
    movq  $__bss_end, %rcx
    subq  %rdi, %rcx
    xorq  %rax, %rax
    rep stosb
```

清零 BSS。`rep stosb` 是 x86 的字符串操作指令，把 `%al`（这里是 0）写到 `%rdi` 指向的位置，重复 `%rcx` 次。Intel SDM Vol.3A §4.3 描述了这条指令在 64 位模式下的行为。我们不在 ELF 加载器中依赖 BSS 清零，无条件做一遍万无一失。

不过这里有个坑——`rep stosb` 破坏了 `%rdi`、`%rcx`、`%rax`。如果 mini kernel 通过 `%rdi` 传了 BootInfo 指针（System V AMD64 ABI 的第一个参数寄存器），经过 BSS 清零之后这个值就丢了。Linux 的 head_64.S 用一种优雅的方式解决了这个问题：`mov %rsi, %r15`——在动任何寄存器之前先把 boot_params 存到 `%r15`（callee-saved 寄存器），等所有初始化做完后再 `movq %r15, %rdi` 恢复。我们的代码里有个 TODO 标记了这个问题，等后续需要 BootInfo 时再补上。

```asm
    call  _init_global_ctors

    xorq  %rdi, %rdi
    call  kernel_main

.halt:
    cli
    hlt
    jmp   .halt
```

调用全局构造器后清零 `%rdi` 传 NULL 给 `kernel_main`，然后调 C++ 入口。halt 循环三指令兜底——`cli; hlt; jmp` 保证即使被 NMI 唤醒也会重新停机。xv6 的 entry.S 也有类似的模式，但 xv6 是 C 内核不需要全局构造器，也不做 BSS 清零——它依赖 GRUB 加载器清零。

## 第三步——C++ 运行时桩函数：freestanding 环境的必需品

如果你写 C 内核，boot.S 里设栈、清 BSS、调 main 就够了。但 C++ 有一套额外的运行时需求——编译器在 freestanding 环境下期望某些符号存在，缺了就链接不过。

```cpp
extern "C" {

[[noreturn]] void __cxa_pure_virtual() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'V'), "Nd"((uint16_t)0xE9));
    while (1) { __asm__ volatile("cli; hlt"); }
}

[[noreturn]] void __stack_chk_fail() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'S'), "Nd"((uint16_t)0xE9));
    while (1) { __asm__ volatile("cli; hlt"); }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}
```

`__cxa_pure_virtual` 处理纯虚函数调用（在 QEMU debug console 输出 'V'），`__stack_chk_fail` 处理栈保护失败（输出 'S'）。两者都是 halt，因为它们代表编程错误。`__cxa_atexit` 返回 0——内核永不退出，析构函数永远不会被调用。xv6 不需要这些，因为它是 C 内核。Linux 有完整的一套，但策略类似——出错就 Oops + panic。

```cpp
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    void (**start)() = __init_array_start;
    void (**end)()   = __init_array_end;

    for (void (**func)() = start; func != end; func++) {
        void (*ctor)() = *func;
        if (ctor != nullptr) {
            ctor();
        }
    }
}
```

遍历 `.init_array` section 调用全局构造器。OSDev Wiki 的 "Calling Global Constructors" 页面详细介绍了这个机制——System V ABI 用 crt*.o 链来处理，但内核用 `-nostdlib` 编译时没有这些启动文件，所以自己遍历 `.init_array`。SerenityOS 的做法类似——在链接脚本的 `.rodata` 段里收集 `*(.ctors) *(.init_array)`。

```cpp
}  // extern "C"

void* operator new(unsigned long size) {
    (void)size;
    while (1) { __asm__ volatile("cli; hlt"); }
}

void operator delete(void* ptr) noexcept {
    (void)ptr;
    while (1) { __asm__ volatile("cli; hlt"); }
}
```

`operator new/delete` 必须放在 `extern "C"` 外面（需要 C++ mangling）。实现为死循环而非返回 nullptr——如果有人用了 new，CPU 立刻卡住，GDB 一看就知道哪里出问题了。Linux 有完整的 slub 分配器，SerenityOS 有 kmalloc，但教学内核在 milestone 009 阶段用 halt 是完全合理的。

## 第四步——kernel_main：C++ 世界的第一行输出

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

大内核的 C++ 入口点。先初始化串口（不能假设 mini kernel 留下的硬件状态），打印里程碑消息，然后死循环。大内核的 kprintf 使用 `cinux::lib` 命名空间，和 mini kernel 的 `cinux::mini::lib` 是两套独立实现——这样大内核可以独立演进，不受 mini kernel 的限制。

## 收尾

到这里我们已经完成了大内核的启动基础设施——从链接脚本定义虚拟/物理地址映射，到 boot.S 的六步启动序列，到 C++ 运行时桩函数补齐编译器需求，最后在 kernel_main 里打出了 `[BIG] Big kernel running @ 0x1000000`。整个启动链条至此完整：BIOS -> MBR -> Stage2 -> mini kernel -> big kernel，一条线串下来。

但大内核现在还是个哑巴——只有一行 printf 输出。下一章我们要给它装上更完整的输出基础设施：I/O 端口原语、串口驱动、以及一个完整的 kprintf 格式化引擎。

## 参考资料

- Intel SDM: Vol.3A §4.3 — 64-Bit Mode 下 rep stosb 的行为
- Intel SDM: Vol.3A §5.3 — 48 位 canonical 地址空间布局
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
- OSDev Wiki: [Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors)
- Linux: [head_64.S](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S) — boot_params 保存到 %r15
- xv6: [entry.S](https://github.com/mit-pdos/xv6-public/blob/master/entry.S) — 简化的 C 内核启动
- xv6-riscv: [kernel.ld](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kernel.ld) — 简单 `. = 0x80000000` 布局
- SerenityOS: [linker.ld](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/linker.ld) — PHDRS 显式定义
