# 009-1 Read-through: Higher-Half 大内核启动 — 链接脚本 + boot.S + crt_stub + kernel_main

## 概览

本文是 tag 009 的第一篇 read-through，聚焦于大内核从 mini kernel 跳转过来之后、进入 C++ main 函数之前的所有工作。在整个 tag 中，这部分代码是启动链条的最后一环——BIOS 把控制权交给 MBR，MBR 交给 Stage2，Stage2 交给 mini kernel，mini kernel 加载大内核 ELF 并跳转，然后就是本文讲解的 boot.S 接管了。关键设计决策集中在 VMA/LMA 分离策略、C++ 运行时桩函数的选择、以及 BootInfo 传递的取舍上。

## 架构图

```
启动链全貌:

BIOS → MBR (512B) → Stage2 (实模式→保护模式→长模式)
                                    ↓
                              Mini Kernel
                          (identity mapping)
                         ATA 读盘 + ELF 加载
                                    ↓
                          jmp *entry (物理地址)
                                    ↓
┌─────────────── Big Kernel 从这里开始 ───────────────┐
│                                                      │
│  boot.S: _start                                      │
│    cli        禁中断                                  │
│    mov RSP    设栈 (linker script 的 __kernel_stack_top)│
│    xor RBP    清基址指针                               │
│    rep stosb  清零 BSS                                │
│    call       _init_global_ctors                      │
│    call       kernel_main                             │
│    halt       死循环                                   │
│                                                      │
│  crt_stub.cpp:                                       │
│    __cxa_pure_virtual  → halt (debugcon 'V')         │
│    __stack_chk_fail    → halt (debugcon 'S')         │
│    __cxa_atexit        → return 0                    │
│    _init_global_ctors  → walk .init_array            │
│    operator new/delete → halt (no heap)              │
│                                                      │
│  main.cpp:                                           │
│    kernel_main() → kprintf_init() + kprintf(...)     │
│                                                      │
└──────────────────────────────────────────────────────┘
```

## 代码精讲

### 链接脚本 kernel/linker.ld

先来看整个大内核的"地基"——链接脚本。它定义了内核在虚拟地址空间和物理地址空间中的布局。

```ld
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_VMA   = 0xFFFFFFFF80000000;   /* higher-half virtual base */
KERNEL_LMA   = 0x1000000;            /* physical load address (16 MB) */

SECTIONS
{
    . = KERNEL_VMA + KERNEL_LMA;
```

开头的伪操作告诉链接器我们要生成 x86-64 的 ELF 文件，入口点是 `_start`。两个地址常量是整个设计的关键：`KERNEL_VMA = 0xFFFFFFFF80000000` 是 higher-half 的虚拟基地址，和 Linux 的 `__START_KERNEL_map` 完全一致；`KERNEL_LMA = 0x1000000`（16MB）是物理加载地址，必须和 mini kernel 的 `BIG_KERNEL_LOAD_ADDR` 匹配。

`. = KERNEL_VMA + KERNEL_LMA` 这行把位置计数器设为 `0xFFFFFFFF80100000`——之后所有 section 的 VMA 从这个值开始递增。但这只是 VMA，我们还需要为每个 section 指定独立的 LMA：

```ld
    .text : AT(ADDR(.text) - KERNEL_VMA) ALIGN(4096) {
        *(.text.start)         /* _start MUST be first */
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

每个 section 的 `AT()` 伪操作里做了减法 `ADDR(.section) - KERNEL_VMA`，把 VMA 减去 `0xFFFFFFFF80000000` 得到纯物理偏移。这样 ELF 的 program header 中 `p_vaddr` 是高地址，`p_paddr` 是低地址，两边各取所需。

`.text.start` 被强制放在 `.text` 段的最前面——这保证了 `_start` 函数出现在 ELF 文件的第一个字节，mini kernel 跳转到入口点时执行的第一条指令就是我们的启动代码。`.rodata` 合并到 `.text` 段是因为它们都是只读的，放在同一个 PT_LOAD 段里更节省空间。

`.init_array` 用了 `KEEP()` 防止被 `--gc-sections` 删掉——这些函数指针是被 `_init_global_ctors` 通过 linker script 符号间接引用的，链接器的静态分析发现不了依赖关系。`__init_array_start` 和 `__init_array_end` 两个边界符号标记了数组的起止位置。

```ld
    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    __kernel_end = .;
    PROVIDE(__kernel_size = __kernel_end - (KERNEL_VMA + KERNEL_LMA));

    .stack (NOLOAD) : ALIGN(4096) {
        . = . + 0x4000;          /* 16 KB stack */
        __kernel_stack_top = .;
    }

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
```

`.bss` 不需要 `AT()` 因为它不占文件空间——启动汇编负责清零。`__kernel_end` 和 `__kernel_size` 是给物理内存管理器（PMM）预留的标记。`.stack (NOLOAD)` 在 VMA 空间里预留了 16KB 但不产生文件内容，`__kernel_stack_top` 指向栈顶。最后 `/DISCARD/` 扔掉了不需要的 `.comment`、`.note`、`.eh_frame` 段。

### 启动汇编 kernel/arch/x86_64/boot.S

这是大内核的第一段代码，mini kernel 通过间接跳转来到这里。

```asm
.section .text.start, "ax"
.code64

.global _start
.type   _start, @function

_start:
    cli                           # disable interrupts
```

放在 `.text.start` section 里确保它出现在 ELF 文件最开头。`.code64` 因为 mini kernel 在 64 位长模式下跳转过来。上来第一条指令就是 `cli` 禁用中断——因为大内核还没有设置自己的 IDT，任何硬件中断都会触发 CPU 查 mini kernel 的 IDT，大概率崩溃。

```asm
    movq  $__kernel_stack_top, %rsp
    xorq  %rbp, %rbp
```

设置栈。`$__kernel_stack_top` 是 AT&T 语法取链接脚本符号的值——栈空间的最高地址。x86-64 栈向下增长，所以"栈顶"是高地址。`xorq %rbp, %rbp` 清零基址指针，标记调用链底部——stack unwinding 工具看到 RBP=0 就知道到顶了。

```asm
    movq  $__bss_start, %rdi
    movq  $__bss_end, %rcx
    subq  %rdi, %rcx
    xorq  %rax, %rax
    rep stosb
```

清零 BSS。`rep stosb` 是 x86 的字符串操作指令——把 `%al`（这里是 0）写到 `%rdi` 指向的地址，`%rdi` 自增，`%rcx` 自减，循环 `%rcx` 次。这是一条非常高效的内存填充指令。我们不在 ELF 加载器中依赖 BSS 清零，而是在启动汇编里无条件做一遍，万无一失。

不过这里有个副作用：`rep stosb` 破坏了 `%rdi`、`%rcx`、`%rax`。如果 mini kernel 通过 `%rdi` 传了 BootInfo 指针过来，清零 BSS 之后这个值就丢了。

```asm
    call  _init_global_ctors

    xorq  %rdi, %rdi
    call  kernel_main

.halt:
    cli
    hlt
    jmp   .halt

.size _start, . - _start
```

调用全局构造器后，显式把 `%rdi` 清零传 NULL 给 `kernel_main`（因为原始的 BootInfo 指针已经被 BSS 清零破坏了），然后调用 C++ 入口点。如果 `kernel_main` 不知怎么返回了，三指令死循环兜底——`cli; hlt; jmp` 保证即使被 NMI 唤醒也会重新停机。

### C++ 运行时桩函数 kernel/arch/x86_64/crt_stub.cpp

这个文件提供 freestanding C++ 环境下编译器期望的所有运行时符号。

```cpp
extern "C" {

[[noreturn]] void __cxa_pure_virtual() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'V'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`__cxa_pure_virtual` 是 C++ ABI 要求的符号——纯虚函数被调用时触发。最常见场景是在基类构造函数里调了虚函数：C++ 对象构造时 vtable 指针是逐级设置的，基类构造期间如果基类有纯虚函数，调用它就会走进这里。我们在 QEMU 的 debug console（端口 0xE9）输出字符 'V'，然后在 `debug.log` 里看到一个 'V' 就知道是纯虚函数被调了。

```cpp
[[noreturn]] void __stack_chk_fail() {
    __asm__ volatile("cli");
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)'S'), "Nd"((uint16_t)0xE9));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}
```

`__stack_chk_fail` 处理栈保护失败。虽然我们的编译选项有 `-fno-stack-protector`，但保留它以防万一。`__cxa_atexit` 直接返回 0——内核永不退出，析构函数永远不会被调用，对于内核来说完全合理。

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

}  // extern "C"
```

这是全局构造器初始化的核心逻辑。`__init_array_start` 和 `__init_array_end` 是链接脚本定义的符号，声明为函数指针数组是 C/C++ 里处理 linker script 符号的标准手法——链接器给符号分配的"值"就是地址本身，声明成数组后 start 和 end 就分别指向首元素和尾后元素。循环里的 `nullptr` 检查是对齐填充的安全防护。

```cpp
void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

`operator new/delete` 全部实现为死循环——没有堆分配器，任何动态内存分配都是编程错误。死循环而非返回 `nullptr` 的好处是问题立刻暴露：如果有人用了 `new`，CPU 卡住，GDB 一看就知道。返回 `nullptr` 的话问题要等到后面解引用空指针才暴露。这些函数必须放在 `extern "C"` 块外面，因为需要 C++ 的 name mangling。

### C++ 入口点 kernel/main.cpp

```cpp
extern "C" void kernel_main() {
    cinux::lib::kprintf_init();
    cinux::lib::kprintf("[BIG] Big kernel running @ 0x1000000\n");

    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

大内核的 C++ 入口点。先调用 `kprintf_init()` 初始化串口——不能假设 mini kernel 留下的硬件状态，重新初始化一次最安全。然后打印里程碑消息确认到达了这里。函数签名没有 `[[noreturn]]`，但内部死循环保证不返回，boot.S 的 `.halt` 也兜底。大内核使用 `cinux::lib` 命名空间的 kprintf，和 mini kernel 的 `cinux::mini::lib` 是两套独立实现。

## 设计决策

### 决策：BootInfo 指针传递方式

**问题**：mini kernel 通过 `%rdi` 传递 BootInfo 指针给大内核，但 BSS 清零操作破坏了 `%rdi`。

**本项目的做法**：目前 milestone 009 不需要 BootInfo，直接传 NULL。

**备选方案**：在 BSS 清零之前把 `%rdi` 保存到 `%r12`（callee-saved 寄存器），清零之后恢复。Linux 采用类似方案，把 boot_params 从 `%rsi` 存到 `%r15`。

**为什么不选备选方案**：milestone 009 确实不需要 BootInfo，提前引入保存/恢复逻辑会增加代码复杂度且无法验证正确性。等后续 milestone 真正需要 BootInfo 时再加。

**如果要扩展/改进**：在 boot.S 的 Step 2 和 Step 3 之间插入 `movq %rdi, %r12`，在 Step 5 把 `xorq %rdi, %rdi` 替换为 `movq %r12, %rdi`。

### 决策：operator new/delete 直接 halt

**问题**：没有堆分配器时如何处理动态内存分配请求？

**本项目的做法**：死循环 halt。

**备选方案**：返回 nullptr，让调用者自己处理。

**为什么不选备选方案**：返回 nullptr 会延迟问题暴露——调用者可能不检查返回值，到后续解引用空指针时才崩溃，排查路径更长。halt 是 fail-fast 策略，立刻定位问题。

**如果要扩展/改进**：实现一个简单的 bump allocator 或 slab allocator 后，替换 operator new/delete 的实现。

## 扩展方向

- 在 boot.S 中保存/恢复 BootInfo 指针（难度 ⭐）
- 添加 .ctors/.dtors 段支持（兼容旧 GCC，难度 ⭐）
- 实现简单的 bump allocator 替换 operator new 的死循环（难度 ⭐⭐）
- 在 boot.S 中添加启动阶段检测（判断是从 mini kernel 跳转还是从 multiboot bootloader 启动）（难度 ⭐⭐）
- 实现栈保护 canary（修改编译选项启用 -fstack-protector，难度 ⭐⭐）

## 参考资料

- Intel SDM: Vol.3A §4.3 — 64-Bit Mode 指令行为（rep stosb, cli, hlt）
- Intel SDM: Vol.3A §5.3 — 48 位 canonical 地址空间
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
- OSDev Wiki: [Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors)
- OSDev Wiki: [Linker Scripts](https://wiki.osdev.org/Linker_Scripts)
- Linux: [head_64.S](https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S) — 保存 boot_params 到 %r15
- xv6: [entry.S](https://github.com/mit-pdos/xv6-public/blob/master/entry.S) — C 内核的简化启动流程
- SerenityOS: [linker.ld](https://github.com/SerenityOS/serenity/blob/master/Kernel/Arch/x86_64/linker.ld) — PHDRS 显式定义
