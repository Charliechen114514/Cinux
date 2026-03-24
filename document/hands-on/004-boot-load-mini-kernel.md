# 004_boot_load_mini_kernel_C：long mode 内填 BootInfo 并跳转

## 章节导语

欢迎来到第四章，这可能是整个启动过程中最令人兴奋的一节——我们终于要真正跳转到自己的内核代码了！

在前面几章中，我们完成了从 MBR 加载 stage2，进入了保护模式，再开启了 long mode。但到现在为止，CPU 运行的还都是 bootloader 的代码。本章的使命是让 bootloader 把控制权交接给我们自己写的 C++ 内核。

听起来很简单，对吧？但这里有几个隐藏的坑等你踩：BootInfo 结构怎么填、C++ 运行时怎么初始化、全局对象的构造函数什么时候调用——还有最让人血压拉高的 BSS 清除和符号冲突问题。

不过先别慌，我们一步步来。完成本章后，你会在 QEMU 的串口输出中看到 `OPLJ123G4===CPPGC1V123B===END`，这意味着你的 C++ 内核已经成功运行，并且验证了类构造、虚函数、全局对象等高级特性都正常工作。

本章基于上一章 `003_boot_long_mode` 的内容，请确保你已经完成了 long mode 的开启。

---

## 概念精讲

### BootInfo：bootloader 和内核的握手协议

你可以把 BootInfo 理解为 bootloader 写给内核的一封信。当 bootloader 完成使命准备"退休"时，它把所有重要信息都打包到这个结构体里，然后把信的地址作为参数传给内核。

这封信里有什么呢？

```
BootInfo 结构布局（位于物理地址 0x7000）
├── entry_point      → 内核入口点的虚拟地址（高半核地址）
├── kernel_phys_base → 内核在物理内存中的加载地址
├── kernel_size      → 内核实际大小
├── fb_addr/pitch/width/height/bpp → 显存信息（VESA 获取）
└── mmap_count + mmap[] → 内存映射表（E820 查询结果）
```

为什么要用固定的物理地址 `0x7000`？因为在进入 long mode 后，虚拟地址映射还没完全建立，用固定的物理地址最简单可靠。

### 高半核映射（Higher-Half Kernel）

你可能注意到了，`entry_point` 的值是 `0xFFFFFFFF80020000`，这是一个非常大的地址。这就是所谓的"高半核"映射。

```
虚拟地址布局（x86-64，canonical address）
│
├─────────────────────────────────────────┤ 0xFFFFFFFFFFFFFFFF
│           用户空间（高半部分）            │
├─────────────────────────────────────────┤ 0x0000800000000000
│           非规范地址 hole（不可用）        │
├─────────────────────────────────────────┤ 0x0000800000000000
│           用户空间（低半部分）            │
├─────────────────────────────────────────┤ 0x0000000000400000
│           内核空间（低地址映射）           │
├─────────────────────────────────────────┤ 0x0000000000020000
│           内核空间（高半核映射） ← 我们用这个 │
├─────────────────────────────────────────┤ 0xFFFFFFFF80000000
│           内核代码段（只读）               │
├─────────────────────────────────────────┤ 0xFFFFFFFF80020000
```

高半核的好处是内核代码位于高地址，不会和用户程序的低地址冲突。同时，通过页表映射，高地址 `0xFFFFFFFF80020000` 和低地址 `0x20000` 实际指向同一块物理内存——这是一种identity mapping和higher-half mapping的双重映射。

### AT&T 汇编语法速查

项目使用 GNU AS，语法是 AT&T 风格（和 NASM 的 Intel 风格不同）。关键区别：

```
操作数顺序：  AT&T:   指令 源操作数, 目的操作数
            Intel:  指令 目的操作数, 源操作数

寄存器前缀： AT&T:   %rax, %rbx, %rdi
            Intel:  rax, rbx, rdi

立即数前缀： AT&T:   $0x7000
            Intel:  0x7000

内存寻址：   AT&T:   displacement(base, index, scale)
            Intel:  [base + index*scale + displacement]

示例：
AT&T:   movl $0x7000, %edi      # 把立即数 0x7000 移到 %edi
Intel:  mov edi, 0x7000         # 把 0x7000 移到 edi

AT&T:   movl (%rsi), %eax       # 把 %rsi 指向的内存内容读到 %eax
Intel:  mov eax, [rsi]          # 把 [rsi] 的内容读到 eax
```

**重要提示**：AT&T 语法中，源操作数在前，目标操作数在后。这和 Intel 语法正好相反！

### C++ 运行时支持：freestanding 环境

我们的内核是"freestanding"环境，没有标准库支持。这意味着：

1. **没有 `new`/`delete`**：动态内存分配需要自己实现
2. **没有异常**：`-fno-exceptions` 编译选项禁用了异常
3. **没有 RTTI**：`-fno-rtti` 禁用了运行时类型信息
4. **需要手动调用全局构造函数**：`.init_array` 段中的函数指针需要我们手动遍历调用

但是！类、虚函数、vtable 这些特性仍然可用，它们是编译期特性，不依赖运行时库。

```
C++ 对象模型（freestanding）
├── 构造/析构函数 → 编译器生成，可直接调用
├── 虚函数表 (vtable) → 编译器生成，放在 .rodata 段
├── 全局对象构造函数 → 放在 .init_array 段，需手动调用
└── new/delete → 需要自己提供实现（或 stub 函数）
```

---

## 动手实现

### Step 1：定义 BootInfo 结构

**目标**：创建一个 bootloader 和内核都能识别的数据结构定义。

这是 bootloader 和内核之间的"契约"，两边必须用同样的定义，否则数据会解析错误。

**代码**（文件路径：`boot/boot_info.h`）：

```c
/**
 * @file boot/boot_info.h
 * @brief Bootloader-to-kernel handoff structure definition
 *
 * IMPORTANT: This header is included by BOTH bootloader AND kernel.
 * Must use identical layout and field sizes!
 */

#ifndef BOOT_BOOT_INFO_H
#define BOOT_BOOT_INFO_H

#include <stdint.h>

// Memory Map Entry (from E820 BIOS call)
typedef struct {
    uint64_t base;          // Physical base address
    uint64_t length;        // Region length in bytes
    uint32_t type;          // 1=usable, 2=reserved, etc.
    uint32_t acpi;          // ACPI attributes (usually 0)
} __attribute__((packed)) MemoryMapEntry;

// Compile-time size check (must be 24 bytes to match E820 format)
static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");

// Boot Information Structure
typedef struct {
    // Kernel information
    uint64_t entry_point;       // Virtual entry point (higher-half)
    uint64_t kernel_phys_base;  // Physical load address
    uint64_t kernel_size;       // ELF file size

    // Framebuffer information
    uint64_t fb_addr;           // Physical framebuffer address
    uint32_t fb_width;          // Width in pixels
    uint32_t fb_height;         // Height in pixels
    uint32_t fb_pitch;          // Bytes per scan line
    uint32_t fb_bpp;            // Bits per pixel (usually 32)

    // Memory map
    uint32_t mmap_count;        // Number of valid entries
    uint32_t _pad;              // Padding for alignment
    MemoryMapEntry mmap[32];    // Max 32 entries

} __attribute__((packed)) BootInfo;

// Size check: 4*8 + 6*4 + 32*24 = 32 + 24 + 768 = 824 bytes
static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");

#endif
```

**解释**：

`__attribute__((packed))` 是关键——它告诉编译器不要插入任何填充字节，确保结构体的内存布局严格按照我们写的顺序。如果没有这个属性，编译器可能会在字段之间插入填充以对齐访问，导致结构体大小和预期不符。

`static_assert` 是编译期断言，确保结构体大小是我们期望的值。这样如果有人不小心修改了结构体，编译会直接失败，而不是运行时出现莫名其妙的 bug。

为什么 `fb_pitch` 和 `fb_bpp` 分开？因为 VESA 返回的信息就是这样的布局：pitch 是每行字节数，可能大于 `width * bpp / 8`，因为可能有行间填充。

**验证**：编译 bootloader 和内核都不会报错，说明结构体定义一致。

---

### Step 2：在 long mode 入口填充 BootInfo

**目标**：进入 long mode 后，从之前保存的地址读取 VESA 和内存映射信息，填充到 BootInfo 结构。

这里我们已经在 long mode（64位模式），可以用完整的 64 位寄存器了。

**代码**（文件路径：`boot/stage2.S`，在 `long_mode_entry` 标签后）：

```asm
// ============================================================
// Long Mode Entry Point
// ============================================================
.code64                          // Now in 64-bit long mode
.global long_mode_entry
long_mode_entry:
    // Set up data segment registers for 64-bit mode
    movw $GDT_DATA64, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    // Set up 64-bit stack
    movabsq $0x90000, %rsp

    // Verify long mode entry with debugcon output
    movb $CHAR_LONG_MODE, %al    // 'L' = 0x4C
    outb %al, $DEBUGCON_PORT     // Output 'L' to debugcon

    // ============================================================
    // 004_boot_load_mini_kernel_C: Fill BootInfo and Jump to Mini Kernel
    // ============================================================

    // BootInfo is placed at physical 0x7000
    // Layout matches boot/boot_info.h
    movq $0x7000, %rdi           // BootInfo destination address

    // 1. entry_point: higher-half kernel virtual address
    //    VMA = 0xFFFFFFFF80000000 + 0x20000 = 0xFFFFFFFF80020000
    movq $0xFFFFFFFF80020000, %rax
    movq %rax, (%rdi)            // [0x7000] = entry_point

    // 2. kernel_phys_base: physical load address
    movq $0x20000, %rax
    movq %rax, 8(%rdi)           // [0x7008] = kernel_phys_base

    // 3. kernel_size: actual size (416KB max)
    movq $0x68000, %rax          // 416KB = 0x68000 bytes
    movq %rax, 16(%rdi)          // [0x7010] = kernel_size

    // 4. Framebuffer info: copy from 0x6400 (VESA saved it there)
    //    0x6400: fb_addr (8B) + fb_pitch (4B) + fb_width (2B) + fb_height (2B)
    movq $0x6400, %rsi           // Source: VESA framebuffer info
    movq (%rsi), %rax            // Read fb_addr
    movq %rax, 24(%rdi)          // [0x7018] = fb_addr

    movl 8(%rsi), %eax           // Read fb_pitch
    movl %eax, 40(%rdi)          // [0x7028] = fb_pitch

    movzwq 12(%rsi), %rax        // Read fb_width (zero-extend 16-bit)
    movl %eax, 32(%rdi)          // [0x7020] = fb_width

    movzwq 14(%rsi), %rax        // Read fb_height (zero-extend 16-bit)
    movl %eax, 36(%rdi)          // [0x7024] = fb_height

    // fb_bpp: assume 32 (standard for VESA mode 0x118)
    movl $32, %eax
    movl %eax, 44(%rdi)          // [0x702C] = fb_bpp

    // 5. Memory map: copy from 0x5000 (E820 saved it there)
    //    0x5000: mmap_count (4B) + mmap entries (24B each)
    movq $0x5000, %rsi           // Source: E820 memory map
    movl (%rsi), %eax            // Read mmap_count
    movl %eax, 48(%rdi)          // [0x7030] = mmap_count

    // Copy mmap entries (max 32 entries * 24 bytes = 768 bytes)
    movq $56, %rdx               // Destination offset in BootInfo
    movq $4, %rcx                // Source offset (skip count)
    movq $768, %r8               // Bytes to copy
1:
    movb (%rsi, %rcx), %al
    movb %al, (%rdi, %rdx)
    incq %rcx
    incq %rdx
    decq %r8
    jnz 1b
```

**解释**：

这里有几处 AT&T 语法需要注意。首先是内存寻址 `(%rdi)` 和 `8(%rdi)`——前者等价于 `[%rdi]`，后者等价于 `[%rdi + 8]`。

`movzwq` 指令是把 16 位值零扩展到 64 位。VESA 信息中的宽度和高度是 16 位的，但我们的寄存器和结构体字段是 32/64 位的，所以需要扩展。

内存拷贝那段没有用 `rep movsb`，而是手动循环，是因为源地址和目标地址的偏移量不同（源要跳过 count，目标从 mmap 数组开始）。

**验证**：暂时无法验证，需要完成内核部分才能看到效果。

---

### Step 3：配置链接器脚本

**目标**：确保内核被加载到正确的物理地址，并且虚拟地址映射正确。

链接器脚本决定程序段的内存布局。对于高半核内核，这是最关键的部分。

**代码**（文件路径：`kernel/mini/linker.ld`）：

```ld
/* ==============================================================
 * Cinux Mini Kernel - Minimal Linker Script
 * ============================================================== */

OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

/* Kernel physical load address and virtual base offset */
KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    /* Higher-half kernel virtual base address with physical offset */
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    /* All read-only sections combined */
    .text : AT(KERNEL_PHYS_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    /* All read-write sections combined */
    .data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) {
        *(.data .data.*)
        __init_array_start = .;
        *(.init_array .init_array.*)
        __init_array_end = .;
    }

    /* .bss section: no LMA needed (cleared at runtime) */
    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Kernel end markers */
    __mini_kernel_end = .;

    /DISCARD/ : {
        *(.comment*)
        *(.note*)
        *(.eh_frame*)
    }
}
```

**解释**：

这个链接器脚本有几个关键点：

1. **`AT(KERNEL_PHYS_BASE)`**：指定了段的加载地址（LMA），即物理内存中的位置。`.text` 段从 `0x20000` 开始。

2. **`.` 地址设置**：当前虚拟地址（VMA）设为 `0xFFFFFFFF80000000 + 0x20000 = 0xFFFFFFFF80020000`。这是代码执行时的虚拟地址。

3. **`__init_array_start/end`**：标记了全局构造函数指针数组的起止位置。C++ 编译器会把所有全局对象的构造函数指针放在这个段里。

4. **`.bss` 段没有 `AT`**：BSS 段不占用文件空间，运行时清零即可，所以不需要指定加载地址。

**⚠️ 注意**：`.data` 段必须在 `.bss` 段之前！后面的踩坑笔记会解释为什么。

**验证**：编译后用 `objdump -f` 查看内核的起始地址：

```bash
$ objdump -f build/kernel/mini/mini_kernel
build/kernel/mini/mini_kernel:     file format elf64-x86-64
architecture: i386:x86-64, flags 0x0000012:
EXEC_P, HAS_SYMS
start address 0xffffffff80020000  # 正确！
```

---

### Step 4：实现内核启动汇编

**目标**：编写内核的入口点 `_start`，负责初始化运行时环境后跳转到 C++ main。

这是内核真正开始执行的第一行代码。我们需要设置栈、清除 BSS、调用全局构造函数，然后才跳转到 C++ 代码。

**代码**（文件路径：`kernel/mini/arch/x86_64/boot.S`）：

```asm
/* ==============================================================
 * Cinux Mini Kernel - Bootstrap Entry Point
 * ============================================================== */

.section .text
.code64

.global _start
.type _start, @function

_start:
    /* Disable interrupts */
    cli

    /* Output '1' to debugcon - _start reached */
    movb $0x31, %al              /* '1' */
    outb %al, $0xE9

    /* Setup stack - 8KB stack at end of kernel */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Output '2' to debugcon - stack ready */
    movb $0x32, %al              /* '2' */
    outb %al, $0xE9

    /* Save BootInfo pointer BEFORE clearing BSS */
    movq %rdi, __boot_info_ptr

    /* Clear BSS section */
    movq $__bss_start, %rdi      /* destination */
    movq $__bss_end, %rcx        /* end */
    subq %rdi, %rcx              /* count = end - start */
    xorq %rax, %rax              /* value = 0 */
    rep stosb                    /* clear */

    /* Output '3' to debugcon - BSS cleared */
    movb $0x33, %al              /* '3' */
    outb %al, $0xE9

    /* Call global constructors (C++ runtime) */
    call _init_global_ctors

    /* Output '4' to debugcon - ctors done */
    movb $0x34, %al              /* '4' */
    outb %al, $0xE9

    /* Call C++ main */
    movq __boot_info_ptr, %rdi   /* first argument: BootInfo* */
    call mini_kernel_main

    /* Halt if kernel returns */
.halt:
    cli
    hlt
    jmp .halt

.size _start, .-_start

/* ==============================================================
 * BootInfo Pointer Storage (.data section - NOT .bss!)
 * ============================================================== */
.section .data
.global __boot_info_ptr
.align 8
__boot_info_ptr:
    .quad 0

/* ==============================================================
 * Stack Section
 * ============================================================== */
.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

__mini_stack:
    .skip MINI_STACK_SIZE
__mini_stack_top:
```

**解释**：

这段代码的执行流程是：

1. `cli` 禁用中断——我们还没设置中断处理，这时候来中断会崩溃
2. 设置栈指针到栈顶（注意栈是向下增长的）
3. **保存 BootInfo 指针**——这一步必须在 BSS 清除之前！原因后面会讲
4. 清除 BSS 段——用 `rep stosb` 指令快速清零
5. 调用全局构造函数——遍历 `.init_array` 中的函数指针
6. 恢复 BootInfo 指针到 `%rdi`，调用 C++ main

**⚠️ 重要**：`__boot_info_ptr` 必须放在 `.data` 段，不能放在 `.bss` 段！因为 BSS 清除会把它清零，而且后面 C++ 全局对象的初始化可能和它冲突。这是一个真实踩过的坑，详见后面的调试笔记。

`rep stosb` 是一个重复存储指令：
- `%rdi` 是目标地址
- `%rax` 是要存储的值（这里是 0）
- `%rcx` 是重复次数
- 每次存储后 `%rdi` 增加，`%rcx` 减少

这相当于 C 语言的 `memset(__bss_start, 0, __bss_end - __bss_start)`。

**验证**：暂时无法验证，需要完成 C++ 运行时支持。

---

### Step 5：实现 C++ 运行时支持

**目标**：提供 C++ 运行时需要的最小支持函数。

freestanding 环境没有标准库，但 C++ 编译器生成的代码会调用一些运行时函数。我们需要提供这些函数的实现。

**代码**（文件路径：`kernel/mini/arch/x86_64/crt_stub.cpp`）：

```cpp
/* ==============================================================
 * Cinux Mini Kernel - C++ Runtime Support
 * ============================================================== */

extern "C" {

// ============================================================
// Pure Virtual Function Call Handler
// ============================================================
// Called when a pure virtual function is called (should never happen)
[[noreturn]] void __cxa_pure_virtual() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// ============================================================
// Stack Smashing Protector Failure Handler
// ============================================================
[[noreturn]] void __stack_chk_fail() {
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// ============================================================
// Atexit Handler (Minimal Implementation)
// ============================================================
// We don't support process termination, so this is a no-op
int __cxa_atexit(void (*)(void*), void*, void*) {
    return 0;
}

// ============================================================
// Global Constructors Initialization
// ============================================================
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
    for (void (**func)() = __init_array_start; func != __init_array_end; func++) {
        (*func)();
    }
}

}  // extern "C"

// ============================================================
// Operator new/delete (Minimal Implementation)
// ============================================================
// NOTE: These must be outside extern "C" as they require C++ linkage.

void operator delete(void* ptr) noexcept {
    (void)ptr;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void operator delete(void* ptr, unsigned long size) noexcept {
    (void)ptr;
    (void)size;
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

void* operator new[](unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

**解释**：

这些函数都是 C++ 编译器可能调用的运行时函数：

1. `__cxa_pure_virtual`：当纯虚函数被调用时进入这里（这是编程错误，不该发生）
2. `__stack_chk_fail`：栈保护检测到破坏时调用
3. `__cxa_atexit`：注册退出回调，我们不支持进程退出，所以空实现
4. `_init_global_ctors`：遍历 `.init_array` 段，调用所有全局构造函数
5. `operator new/delete`：动态内存分配，我们暂时不支持，所以这些函数被调用时会停机

`[[noreturn]]` 是 C++11 属性，告诉编译器这个函数永不返回，允许编译器优化代码生成。

`extern "C"` 确保 C 链接，这样汇编代码可以调用这些函数。但 `operator new/delete` 必须用 C++ 链接，所以放在 `extern "C"` 块外面。

**验证**：编译不报错，链接能找到所有符号。

---

### Step 6：实现 C++ 内核 main

**目标**：编写内核的 C++ 主函数，验证 C++ 特性正常工作。

这是真正的内核代码！我们在这里测试类、虚函数、全局对象等 C++ 特性。

**代码**（文件路径：`kernel/mini/main.cpp`）：

```cpp
/* ==============================================================
 * Cinux Mini Kernel - Main Entry Point
 * ============================================================== */

extern "C" {
#include <stdint.h>
}

#include "../../boot/boot_info.h"

extern "C" {
extern uint64_t __boot_info_ptr;
}

// Simple inline function for debugcon output
static void debugcon_putc(char c) {
    __asm__ volatile("outb %0, $0xE9" : : "a"(c));
}

// ============================================================
// Test 1: Simple Class with Constructor/Destructor
// ============================================================
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

// ============================================================
// Test 2: Virtual Functions (vtable test)
// ============================================================
class Base {
public:
    virtual char getName() = 0;   // Pure virtual
    virtual int compute() = 0;
    virtual ~Base() {}
};

class Derived : public Base {
private:
    int multiplier;

public:
    Derived(int m) : multiplier(m) {
        debugcon_putc('V');       // 'V' for Virtual
    }

    virtual char getName() override {
        return 'D';               // 'D' for Derived
    }

    virtual int compute() override { return multiplier * 2; }

    virtual ~Derived() override {
        debugcon_putc('d');
    }
};

// ============================================================
// Test 3: Global Object (global constructor test)
// ============================================================
static int global_construction_count = 0;

class GlobalCounter {
public:
    GlobalCounter() {
        global_construction_count = 42;
        debugcon_putc('G');       // 'G' for Global
    }

    int getCount() const { return global_construction_count; }
};

GlobalCounter global_counter;     // Global object

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    // Start marker: "===CPP"
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('C');
    debugcon_putc('P');
    debugcon_putc('P');

    // Test 1: SimpleClass
    SimpleClass obj1(1);
    if (obj1.getValue() == 1 && obj1.getMarker() == 'S') {
        debugcon_putc('1');
    }

    // Test 2: Virtual Functions
    Derived derived(5);
    Base* base = &derived;
    if (base->getName() == 'D' && base->compute() == 10) {
        debugcon_putc('2');
    }

    // Test 3: Global Object
    if (global_counter.getCount() == 42) {
        debugcon_putc('3');
    }

    // Verify BootInfo
    if (boot_info->entry_point == 0xFFFFFFFF80020000 &&
        boot_info->kernel_phys_base == 0x20000) {
        debugcon_putc('B');
    }

    // End marker: "===END"
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('=');
    debugcon_putc('E');
    debugcon_putc('N');
    debugcon_putc('D');

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

**解释**：

这段代码设计了几个测试，每个测试通过输出一个特定字符来验证成功：

1. `SimpleClass obj1(1)`：输出 `C1`，验证构造函数调用
2. `Derived derived(5)`：输出 `V`，验证虚函数表的构造
3. `global_counter.getCount()`：输出 `G`（在全局构造时），验证全局对象构造
4. `boot_info` 验证：输出 `B`，验证 BootInfo 传递正确

最终的期望输出是 `===CPPGC1V123B===END`，让我们拆解一下：

```
===CPP    → 开始标记
G         → global_counter 构造
C1        → obj1 构造
V         → derived 构造
1         → SimpleClass 测试通过
2         → 虚函数测试通过
3         → 全局对象测试通过
B         → BootInfo 验证通过
===END    → 结束标记
```

注意析构函数不会输出，因为内核在 main 返回前就 `hlt` 停机了。

**验证**：需要完整构建后运行 QEMU。

---

### Step 7：从 Bootloader 跳转到内核

**目标**：在 Bootloader 填充完 BootInfo 后，跳转到内核入口。

这是交接的最后一棒——bootloader 完成使命，把控制权交给内核。

**代码**（文件路径：`boot/stage2.S`，在填充 BootInfo 后）：

```asm
    // ============================================================
    // Jump to Mini Kernel
    // ============================================================

    // Output 'J' to debugcon to indicate jump is about to happen
    movb $0x4A, %al               // 'J' = 0x4A
    outb %al, $DEBUGCON_PORT      // Output 'J' to debugcon

    // Prepare jump arguments
    movq $0x7000, %rdi            // First argument: BootInfo*
    movq $0xFFFFFFFF80020000, %rax // Entry point: _start in higher-half

    // Jump to mini kernel (never returns)
    jmp *%rax

    // Should never reach here
    cli
.lm_halt:
    hlt
    jmp .lm_halt
```

**解释**：

`jmp *%rax` 是间接跳转——跳转到 `%rax` 寄存器指向的地址。这里 `%rax` 存的是内核入口点的虚拟地址 `0xFFFFFFFF80020000`。

按照 System V AMD64 ABI，第一个参数通过 `%rdi` 传递。所以我们把 BootInfo 的地址 `0x7000` 放到 `%rdi`。

输出 `J` 是为了在调试时明确知道跳转即将发生。如果 QEMU 在这里之后崩溃，说明内核入口有问题。

**验证**：完整运行 QEMU，看到 `J` 后紧接着输出 `1234`（内核启动代码的调试输出）。

---

### Step 8：配置 CMake 构建

**目标**：配置 CMake 构建系统，正确编译和链接内核。

**代码**（文件路径：`kernel/mini/CMakeLists.txt`）：

```cmake
add_executable(mini_kernel
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    main.cpp
)

set_target_properties(mini_kernel PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/kernel/mini
    OUTPUT_NAME "mini_kernel"
)

target_compile_options(mini_kernel PRIVATE
    -ffreestanding           # No standard library
    -fno-exceptions         # Disable exceptions
    -fno-rtti               # Disable RTTI
    -fno-pie                # No position-independent executable
    -mcmodel=large          # Large code model (for higher-half kernel)
    -mno-red-zone           # Disable red zone (kernel code)
    -Wall
)

target_link_options(mini_kernel PRIVATE
    -T ${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
    -nostdlib
    -no-pie
)

# Post-build: Convert ELF to flat binary
add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel> $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary: mini_kernel.bin"
    VERBATIM
)
```

**解释**：

编译选项很关键：

- `-ffreestanding`：告诉编译器这是 freestanding 环境，没有标准库
- `-fno-exceptions` 和 `-fno-rtti`：禁用异常和 RTTI，我们没提供对应的运行时支持
- `-mcmodel=large`：大代码模型，允许代码访问任意地址（高半核需要）
- `-mno-red-zone`：禁用红区。红区是 x86-64 ABI 的一项优化，在栈指针下方留 128 字节不需要信号保护的区域。但内核代码会在中断时切换栈，红区会出问题

`objcopy -O binary` 把 ELF 文件转换成纯二进制，去掉所有 ELF 头和段信息。这是 bootloader 需要的格式。

**验证**：运行 `make` 应该成功编译，生成 `build/kernel/mini/mini_kernel.bin`。

---

## 构建与运行

现在让我们来验证整个系统是否能正常工作。

### 完整构建流程

```bash
# 确保从正确的 tag 开始
git checkout 004_boot_load_mini_kernel_B

# 创建构建目录
mkdir -p build
cd build

# 配置 CMake
cmake -DCMAKE_BUILD_TYPE=Debug -B build -S ..

# 编译
make -j$(nproc)

# 运行 QEMU
make run
```

### QEMU 启动参数

```bash
qemu-system-x86_64 \
    -drive format=raw,file=cinux.img \
    -debugcon stdio \
    -no-reboot \
    -no-shutdown \
    -device VGA,edid=on,xres=1024,yres=768 \
    -serial none \
    -monitor none
```

参数解释：

- `-drive`：挂载磁盘镜像
- `-debugcon stdio`：把 debugcon 输出到 stdout，这是我们的调试输出
- `-no-reboot` 和 `-no-shutdown`：内核 halt 后 QEMU 不自动重启或退出
- `-device VGA`：配置 VESA 显卡参数
- `-serial none` 和 `-monitor none`：关闭其他输出，保持 debugcon 纯净

### 期望输出

```
OPLJ123G4===CPPGC1V123B===END
```

让我们逐个字符解析这个神秘的字符串：

```
O        → MBR 输出：成功加载 stage2
P        → Protected mode entered
L        → Long mode entered
J        → About to jump to kernel
1        → kernel _start reached
2        → Stack ready
3        → BSS cleared
4        → Global constructors called
G        → global_counter constructed
=        → Begin marker
=        → Begin marker
=        → Begin marker
C        → CPP marker
P        → CPP marker
P        → CPP marker
1        → SimpleClass test passed
2        → Virtual function test passed
3        → Global object test passed
B        → BootInfo valid
=        → End marker
=        → End marker
=        → End marker
E        → END marker
N        → END marker
D        → END marker
```

如果你看到这个输出，恭喜！你的 C++ 内核已经成功运行了！

---

## 调试技巧

这一章踩过的坑不少，这里分享两个最经典的问题和排查方法。

### Bug 1：BootInfo 参数被破坏

**症状**：内核中读取 `boot_info` 指针时，值是错误的。

**排查方法**：

在 `boot.S` 的 `_start` 入口处立即打印 `%rdi` 的值：

```asm
/* Debug: Output %rdi value */
movq %rdi, %r11               // Save to safe register
// ... print %r11 in hex ...
```

如果 `%rdi` 在入口时正确，但在 `mini_kernel_main` 中错误，说明中间某处被破坏了。

**根本原因**：BSS 清除使用 `%rdi` 作为目标地址，覆盖了参数寄存器。而且 `__boot_info_ptr` 如果在 `.bss` 段，也会被清零。

**解决方案**：
1. 在 BSS 清除前立即保存 `%rdi` 到安全位置
2. 把 `__boot_info_ptr` 放在 `.data` 段，不是 `.bss` 段

### Bug 2：符号地址冲突

**症状**：`__boot_info_ptr` 的值变成奇怪的 `0x2a00000000`（42 << 32）。

**排查方法**：

用 `objdump` 检查符号表：

```bash
objdump -t build/kernel/mini/mini_kernel | grep boot_info
```

如果看到 `__boot_info_ptr` 和某个 C++ 全局变量在同一地址，那就是符号冲突。

**根本原因**：汇编定义的符号和 C++ 全局变量被链接器分配到了同一地址。`.bss` 段的符号分配有问题。

**解决方案**：把 `__boot_info_ptr` 移到 `.data` 段，显式初始化为 0。

### 使用串口输出辅助调试

`outb %al, $0xE9` 是 QEMU 的 debugcon 输出，非常方便：

```asm
/* 输出单字符 */
movb $'X', %al
outb %al, $0xE9

/* 输出十六进制数字（4位） */
movl %eax, %ebx
shrl $12, %ebx        // 取高4位
andl $0xF, %ebx
addb $'0', %bl
cmpb $'9', %bl
jna 1f
addb $'A'-'0'-10, %bl // 10-15 变成 A-F
1:
outb %bl, $0xE9
```

### QEMU Monitor 命令

QEMU 的 monitor 可以查看内存和寄存器：

```
(qemu) xp /16xg 0x7000    // 查看 BootInfo 内容
(qemu) info registers     // 查看寄存器
(qemu) x/10i $rip         // 反汇编当前指令
```

---

## 本章小结

### 新增关键函数/结构/寄存器总结

| 名称 | 类型 | 用途 |
|------|------|------|
| `BootInfo` | 结构体 | bootloader 到内核的信息传递 |
| `MemoryMapEntry` | 结构体 | E820 内存映射条目 |
| `_start` | 函数 | 内核入口点（汇编） |
| `mini_kernel_main` | 函数 | C++ 内核主函数 |
| `_init_global_ctors` | 函数 | 调用全局构造函数 |
| `__cxa_pure_virtual` | 函数 | 纯虚函数调用错误处理 |
| `__boot_info_ptr` | 变量 | BootInfo 指针存储（.data 段） |
| `%rdi` | 寄存器 | System V AMD64 ABI 第一个参数 |
| `rep stosb` | 指令 | 重复存储字节（用于清除 BSS） |

### 关键配置

| 配置项 | 值 | 说明 |
|--------|-----|------|
| `KERNEL_PHYS_BASE` | `0x20000` | 内核物理加载地址 |
| `KERNEL_Virt_BASE` | `0xFFFFFFFF80000000` | 高半核虚拟基址 |
| BootInfo 地址 | `0x7000` | 物理地址，固定约定 |
| 栈大小 | `0x2000` (8KB) | 内核栈 |

### 下一章预告

本章我们已经成功跳转到 C++ 内核，并验证了基本的 C++ 特性。下一章 `005_mini_kernel_entry` 将深入内核初始化：设置 GDT、IDT、中断处理，以及更高级的内存管理。

到时候，我们的内核将能够响应中断、处理键盘输入、显示更复杂的图形界面。让我们一起继续这段从零开始的 OS 开发之旅！

---

## 附录：完整的内存布局

```
物理地址                    内容
─────────────────────────────────────────────────────────────
0x0000 - 0x04FF            IVT（中断向量表，实模式使用）
0x0500 - 0x6FFF            E820 内存映射（0x5000）
0x6400 - 0x67FF            VESA 显存信息
0x7000 - 0x73FF            BootInfo 结构
0x8000 - 0x9FFF            Stage2 bootloader
0x10000 - 0x1FFFF          ELF 加载缓冲区（备用）
0x20000 - 0x87FFF          Mini kernel (416KB)

虚拟地址（高半核映射）      内容
─────────────────────────────────────────────────────────────
0xFFFFFFFF80020000         _start（内核入口）
0xFFFFFFFF800226E8         __boot_info_ptr (.data)
0xFFFFFFFF800226F0         global_construction_count (.bss)
0xFFFFFFFF800226F4         global_counter (.bss)
```

---

> 如果你在实践中遇到了其他问题，欢迎查看 `document/notes/006/` 目录下的详细调试笔记，那里记录了本章踩坑的完整心路历程。祝你内核开发愉快！
