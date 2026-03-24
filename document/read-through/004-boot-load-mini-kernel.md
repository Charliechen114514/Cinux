# 004 通读版 · Long Mode 内填 BootInfo 并跳转小内核

## 章节概览

上一章我们成功跳转到了小内核，但那只是一个输出 'M' 然后死循环的空壳。本章要完成的是真正意义上的"内核接管"——在 Long Mode 下填充 BootInfo 结构体，跳转到高半内核地址，验证 C++ 运行时支持（构造函数、虚函数、全局对象），并修复两个诡异的 bug：BootInfo 参数被破坏和 BSS/.bss 符号冲突。听上去像是"填个结构体跳转就行了"，但实际上这一步涉及到 x86-64 内存管理的核心设计：高半内核映射、System V AMD64 ABI 参数传递、C++ 运行时最小实现，以及链接器脚本和段布局的微妙问题。

在整个 Cinux OS 的启动链条中，本章是 bootloader 完成历史使命、内核正式接管的里程碑时刻。上一章我们只是跳到了物理地址 0x20000，而本章我们要跳转到高半内核地址 0xFFFFFFFF80020000，这意味着内核可以运行在统一的、不受物理内存布局影响的虚拟地址空间。BootInfo 结构体承载了内存布局、framebuffer 信息等关键数据，是 bootloader 向内核传递信息的唯一通道。C++ 运行时支持则确保我们可以用现代 C++ 编写内核代码，而不是被限制在 C 语言的子集。

本章的核心设计决策包括：使用高半内核地址（0xFFFFFFFF80000000 + 0x20000）而非纯物理地址；在 Long Mode 的 `long_mode_entry` 中填充 BootInfo（而非在 Protected Mode）；通过 RDI 寄存器传递 BootInfo 指针（符合 System V AMD64 ABI）；实现最小 C++ 运行时（`__cxa_pure_virtual`、`__stack_chk_fail`、`_init_global_ctors`）；将 `__boot_info_ptr` 放在 `.data` 段而非 `.bss` 段避免被清零；使用通配符 `*(.text.*)` 捕获所有代码 subsection。与上一章相比，本章从"能跳转"升级到"能运行 C++ 代码"，是内核功能完整性的基础。

### 关键设计决策一览

* **高半内核映射**：虚拟地址 0xFFFFFFFF80020000 映射到物理 0x20000，PML4[511]->PDPT[510]->PD[1]
* **BootInfo 填充位置**：在 `long_mode_entry`（64 位模式）填充，而非 Protected Mode
* **参数传递约定**：通过 RDI 传递 BootInfo 指针（System V AMD64 ABI 第一个参数）
* **C++ 运行时最小实现**：`__cxa_pure_virtual`、`__stack_chk_fail`、`_init_global_ctors`、operator new/delete
* **段布局修复**：`__boot_info_ptr` 放在 `.data` 段，避免 BSS 清除破坏
* **链接脚本通配符**：`*(.text.*)` 捕获所有编译器生成的 subsection
* **验证输出**：`OPLJ123G4===CPPGC1V123B===END` 完整字符串确认启动链路

---

## 架构图

下面是本章的内存布局、页表结构和启动流程图：

```
+---------------------------------------------------------------------+
|                    页表映射结构（高半内核）                           |
+---------------------------------------------------------------------+
|                                                                     |
|   虚拟地址: 0xFFFFFFFF80020000                                       |
|       |                                                              |
|       v  [PML4 索引 511]                                             |
|   +------------------+                                             |
|   | PML4[511]        | -> PDPT (0x2000)                             |
|   +------------------+       |                                      |
|                             v  [PDPT 索引 510]                       |
|                        +------------------+                          |
|                        | PDPT[510]        | -> PD (0x3000)           |
|                        +------------------+       |                   |
|                                                       v  [PD 索引 1]  |
|                                                  +-----------+        |
|   物理地址: 0x20000  <----------------------------| PD[1]     |        |
|                                                  +-----------+        |
|   (2MB 页面: 0x20000-0x3FFFF)                                        |
|                                                                     |
|   同时映射:                                                         |
|   虚拟地址: 0x0000000000020000  -> 物理地址: 0x20000 (identity)      |
|   虚拟地址: 0xFFFFFFFF80020000 -> 物理地址: 0x20000 (higher-half)    |
|                                                                     |
+---------------------------------------------------------------------+
|                    高半内核地址计算                                  |
+---------------------------------------------------------------------+
|                                                                     |
|   KERNEL_Virt_BASE = 0xFFFFFFFF80000000                             |
|   KERNEL_PHYS_BASE = 0x20000                                        |
|                                                                     |
|   VMA = KERNEL_Virt_BASE + KERNEL_PHYS_BASE                        |
|       = 0xFFFFFFFF80000000 + 0x20000                               |
|       = 0xFFFFFFFF80020000                                         |
|                                                                     |
|   链接脚本 (linker.ld):                                             |
|       . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;                     |
|                                                                     |
+---------------------------------------------------------------------+
|                    低内存布局（BootInfo 填充后）                      |
+---------------------------------------------------------------------+
|  0x00005000  +----------------------------------------------+       |
|              |  E820 Buffer                                  |       |
|  0x00007000  +----------------------------------------------+       |
|              |  BootInfo Structure (已填充)                   |       |
|              |  [0x00] entry_point       = 0xFFFF...80020000 |       |
|              |  [0x08] kernel_phys_base  = 0x20000           |       |
|              |  [0x10] kernel_size       = 0x68000 (416KB)   |       |
|              |  [0x18] fb_addr           = (从 0x6400 复制)  |       |
|              |  [0x20] fb_width/height/pitch/bpp             |       |
|              |  [0x38] mmap_count        = (从 0x5000 复制)  |       |
|              |  [0x3C] mmap[32]          = (从 0x5000 复制)  |       |
|  0x00007B00  +----------------------------------------------+       |
|                                                                     |
+---------------------------------------------------------------------+
|                    高半内核布局（ELF64）                              |
+---------------------------------------------------------------------+
|                                                                     |
|   虚拟地址              内容              物理地址                   |
|   ─────────────────────────────────────────────────────────        |
|   0xFFFFFFFF80020000  .text             0x20000                    |
|   0xFFFFFFFF80022000  .rodata/.data     0x22000                    |
|   0xFFFFFFFF800226E8  __boot_info_ptr   0x226E8 (.data!)           |
|   0xFFFFFFFF800226F0  .bss (C++ 全局)   0x226F0                    |
|   0xFFFFFFFF80022180  __mini_stack_top  0x22180 (8KB 栈)           |
|                                                                     |
+---------------------------------------------------------------------+
|                    启动流程（本章重点）                              |
+---------------------------------------------------------------------+
|                                                                     |
|  Real Mode -> Protected Mode -> Long Mode (003)                    |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | long_mode_entry          |  64-bit 模式                          |
|  |   [设置 DS/ES/FS/GS/SS]  |                                       |
|  |   [设置 RSP = 0x90000]   |                                       |
|  |   [输出 'L']             |  验证进入 Long Mode                  |
|  +--------------------------+                                       |
|       |                                                              |
|       v  [本章新增]                                                   |
|  +--------------------------+                                       |
|  | 填充 BootInfo             |  物理地址 0x7000                    |
|  |   [0x00] = 0xFFFF...80020000|  entry_point (高半地址)            |
|  |   [0x08] = 0x20000        |  kernel_phys_base                   |
|  |   [0x18-44] = fb_info     |  从 0x6400 复制 VESA 信息           |
|  |   [0x38-776] = mmap       |  从 0x5000 复制 E820 信息           |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | 输出 'J'                  |  即将跳转                            |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | mov $0x7000, %rdi         |  第一个参数: BootInfo*              |
|  | mov $0xFFFF...80020000, %rax|  跳转目标: 高半内核地址            |
|  | jmp *%rax                 |  间接跳转                            |
|  +--------------------------+                                       |
|       |                                                              |
|       v  [跳转成功]                                                   |
|  Mini Kernel _start (0xFFFFFFFF80020000)                             |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | [输出 '1']                |  _start 到达                         |
|  | [设置栈]                  |  RSP = __mini_stack_top             |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | mov %rdi, __boot_info_ptr|  保存参数到 .data 段                 |
|  | [清零 BSS]                |  rep stosb                           |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call _init_global_ctors |  调用 C++ 全局构造函数               |
|  | [输出 '4']                |  构造完成                            |
|  +--------------------------+                                       |
|       |                                                              |
|       v                                                              |
|  +--------------------------+                                       |
|  | call mini_kernel_main    |  C++ 主函数                          |
|  |   [输出 '===CPP']        |  C++ 代码开始                        |
|  |   [创建 SimpleClass]     |  测试普通类                          |
|  |   [创建 Derived]         |  测试虚函数                          |
|  |   [验证 global_counter]  |  测试全局构造                        |
|  |   [验证 BootInfo]        |  测试参数传递                        |
|  |   [输出 '===END']        |  测试完成                            |
|  |   cli; hlt               |  停机                                |
|  +--------------------------+                                       |
|                                                                     |
+---------------------------------------------------------------------+
```

---

## 关键代码精讲

本章涉及的文件相当多，我们按照逻辑顺序逐个拆解。首先是最关键的页表高半映射设置，然后是 Long Mode 的 BootInfo 填充和跳转，接着是小内核的汇编入口和 C++ 运行时支持，最后是链接脚本和调试笔记中两个 bug 的修复过程。

### 页表高半映射：通往高半内核的桥梁

`boot/common/long_mode.S` 的 `setup_page_tables` 函数在本章新增了高半内核映射。上一章我们只实现了 identity mapping（0x0000000000000000 -> 0x00000000），现在要添加 higher-half mapping（0xFFFFFFFF80000000 -> 0x00000000）：

```asm
// ============================================================
// Setup Higher-Half Kernel Mapping (004_boot_load_mini_kernel_C)
// ============================================================
// Map 0xFFFFFFFF80000000 -> 0x00000000 using the same PD entries
//
// Virtual address: 0xFFFFFFFF80020000
//   - PML4[511] (bits 47:39 = 0x1FF = 511)
//   - PDPT[510]  (bits 38:30 = 0x1FE = 510)
//   - PD[1]      (bits 29:21 = 0x001 = 1)  -> maps to 0x20000 physical

// PML4[511] -> PDPT (same as PML4[0])
movl $PDPT_PHYS_ADDR, %eax       // PDPT physical address
orl $0x03, %eax                   // Add present+writable flags
movl %eax, PML4_PHYS_ADDR + (511 * 8)  // PML4[511] = PDPT|flags

// PDPT[510] -> PD (same as PDPT[0])
movl $PD_PHYS_ADDR, %eax         // PD physical address
orl $0x03, %eax                   // Add present+writable flags
movl %eax, PDPT_PHYS_ADDR + (510 * 8)  // PDPT[510] = PD|flags
```

这里需要理解 x86-64 的页表结构。48 位虚拟地址被分为 5 个部分：PML4 索引（9 位）、PDPT 索引（9 位）、PD 索引（9 位）、PT 索引（9 位）、页内偏移（12 位）。对于 0xFFFFFFFF80020000，我们手动计算：

```
0xFFFFFFFF80020000 = 1111111111111111111111111111111110000000000000001000000000000000 (二进制)
                      ^^^^^^^^^^^^ ^^^^^^^^^^^ ^^^^^^^^^ ^^^^^^^^^ ^^^^^^^^^^^^^^^^
                      PML4(511)   PDPT(510)   PD(1)     PT(0)     offset(0x20000)
```

所以我们设置 `PML4[511] -> PDPT` 和 `PDPT[510] -> PD`，然后复用已经存在的 `PD[1]`（指向物理 0x20000 的 2MB 页）。这样，0xFFFFFFFF80020000 和 0x0000000000020000 都映射到同一个物理页，但前者是高半内核地址。

### Long Mode 填充 BootInfo：bootloader 的最后一件事

`boot/stage2.S` 的 `long_mode_entry` 在本章新增了 BootInfo 填充逻辑。这是 bootloader 在跳转前必须完成的"数据交接"：

```asm
// BootInfo is placed at physical 0x7000
movq $0x7000, %rdi              // BootInfo destination address

// 1. entry_point: higher-half kernel virtual address
//    VMA = 0xFFFFFFFF80000000 + 0x20000 = 0xFFFFFFFF80020000
movq $0xFFFFFFFF80020000, %rax
movq %rax, (%rdi)               // [0x7000] = entry_point

// 2. kernel_phys_base: physical load address
movq $0x20000, %rax
movq %rax, 8(%rdi)              // [0x7008] = kernel_phys_base

// 3. kernel_size: actual size (will be read from ELF later)
movq $0x68000, %rax             // 416KB = 0x68000 bytes (max)
movq %rax, 16(%rdi)             // [0x7010] = kernel_size
```

这里有个关键点：`entry_point` 必须是虚拟地址，不是物理地址。因为内核在高半地址运行，跳转目标必须是 0xFFFFFFFF80020000，而不是 0x20000。Bootloader 知道这个地址是因为它是硬编码约定的（linker.ld 中的 VMA 计算）。`kernel_phys_base` 是物理地址 0x20000，内核可以用它来做物理内存管理。`kernel_size` 这里硬编码为 0x68000（416KB），实际应该是从 ELF header 读取，但简化处理先用固定值。

```asm
// 4. Framebuffer info: copy from 0x6400
movq $0x6400, %rsi              // Source: VESA framebuffer info
movq (%rsi), %rax               // Read fb_addr
movq %rax, 24(%rdi)             // [0x7018] = fb_addr

movl 8(%rsi), %eax              // Read fb_pitch
movl %eax, 40(%rdi)             // [0x7028] = fb_pitch

movzwq 12(%rsi), %rax           // Read fb_width (zero-extend 16-bit)
movl %eax, 32(%rdi)             // [0x7020] = fb_width

movzwq 14(%rsi), %rax           // Read fb_height (zero-extend 16-bit)
movl %eax, 36(%rdi)             // [0x7024] = fb_height

// fb_bpp: assume 32 (standard for VESA mode 0x118)
movl $32, %eax
movl %eax, 44(%rdi)             // [0x702C] = fb_bpp
```

Framebuffer 信息来自 VESA BIOS 调用的结果，存储在 0x6400。我们用 `movq (%rsi), %rax` 读取 64 位的 fb_addr，用 `movl` 读取 32 位的 fb_pitch，用 `movzwq`（move zero-extend word to quad）读取 16 位的 fb_width/fb_height 并零扩展到 64 位。`movzwq` 是关键——如果用普通的 `movq` 读取 16 位值，高位会有垃圾数据。`fb_bpp` 我们硬编码为 32，因为 VESA mode 0x118 的标准 bpp 就是 32。

```asm
// 5. Memory map: copy from 0x5000
movq $0x5000, %rsi              // Source: E820 memory map
movl (%rsi), %eax               // Read mmap_count
movl %eax, 48(%rdi)             // [0x7030] = mmap_count

// Copy mmap entries (max 32 entries * 24 bytes = 768 bytes)
movq $56, %rdx                  // Destination offset in BootInfo
movq $4, %rcx                   // Source offset (skip count)
movq $768, %r8                  // Bytes to copy
1:
movb (%rsi, %rcx), %al
movb %al, (%rdi, %rdx)
incq %rcx
incq %rdx
decq %r8
jnz 1b
```

E820 内存映射数据存储在 0x5000，格式是 `[count(4B)] + [entries(24B each)]`。我们先读取 4 字节的 `mmap_count`，然后用字节循环复制 768 字节的 entry 数组（最多 32 条 * 24 字节）。为什么用字节循环而不是 `rep movsb`？因为我们在学习阶段，显式的循环更容易理解和调试。如果用 `rep movsb`，需要先设置好 RSI/RDI/RCX，这里的循环虽然慢一点，但逻辑清晰。

### 跳转到高半内核：真正的交接

BootInfo 填充完成后，准备跳转：

```asm
// ============================================================
// Jump to Mini Kernel
// ============================================================

// Output 'J' to debugcon to indicate jump is about to happen
movb $0x4A, %al                 // 'J'
outb %al, $DEBUGCON_PORT        // Output 'J' to debugcon

// Prepare jump arguments
movq $0x7000, %rdi              // First argument: BootInfo*
movq $0xFFFFFFFF80020000, %rax  // Entry point: _start in higher-half

// Jump to mini kernel (never returns)
jmp *%rax

// Should never reach here
cli
.lm_halt:
    hlt
    jmp .lm_halt
```

这里有两个关键点。第一，`movq $0x7000, %rdi` 把 BootInfo 指针作为第一个参数传递。根据 System V AMD64 ABI，第一个整数参数放在 RDI，第二个在 RSI，第三个在 RDX，依此类推。小内核的 `mini_kernel_main(uint64_t boot_info_addr)` 会从 RDI 读取这个值。第二，`jmp *%rax` 是间接跳转。为什么不用 `jmp $0xFFFFFFFF80020000`？因为 64 位模式下，直接绝对跳转的编码有限制（只能跳转到 32 位相对偏移），而间接跳转通过寄存器可以跳转到任意 64 位绝对地址。

'J' 字符的输出是关键的验证点。如果看到 debugcon 输出 'J'，说明 bootloader 成功到达跳转点。如果随后看到 '1'（小内核 _start 的第一个输出），说明跳转成功，内核接管了 CPU。

### 小内核汇编入口：设置运行环境

`kernel/mini/arch/x86_64/boot.S` 是小内核的入口点，负责设置 C/C++ 运行时环境：

```asm
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
```

首先关中断（`cli`），因为内核还没设置中断处理程序，中断来了会崩溃。然后输出 '1' 确认到达 _start。接着设置栈：`__mini_stack_top` 是在 linker.ld 中定义的符号，指向 BSS 段之后的栈顶。清零 RBP 是为了调试时的栈回溯干净。输出 '2' 确认栈设置完成。

```asm
    /* Save BootInfo pointer BEFORE clearing BSS (BSS clear uses %rdi) */
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
```

这里是第一个关键 bug 修复点。BootInfo 指针通过 RDI 传递，我们必须在清零 BSS 之前保存它，因为 `rep stosb` 会使用 RDI 作为目标地址寄存器，破坏原值。保存的目标是 `__boot_info_ptr`，但这个变量不能放在 `.bss` 段，否则会被 `rep stosb` 清零。因此我们把它放在 `.data` 段（后面详细解释）。BSS 清除是 C/C++ 运行时的要求：未初始化的全局变量必须为零。`rep stosb` 是"重复存储字节"指令，每次把 RAX 的值写入 RDI 指向的内存，RCX 次。这里 RAX=0，所以就是清零。

```asm
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
```

调用全局构造函数是 C++ 运行时的关键一步。C++ 全局对象（如 `global_counter`）的构造函数需要在 main 之前执行，这些函数指针被编译器放在 `.init_array` 段。`_init_global_ctors` 遍历这个数组，逐个调用。输出 '4' 确认构造完成。然后从 `__boot_info_ptr` 恢复 BootInfo 指针到 RDI，调用 `mini_kernel_main`。如果 main 返回了（虽然它标记为 `[[noreturn]]`），进入死循环 halt。

```asm
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

这里是第二个关键 bug 修复点。`__boot_info_ptr` 必须放在 `.data` 段，而不是 `.bss` 段。`.data` 段的数据在 ELF 文件中有初始值，加载时直接写入内存，不会被运行时清零。`.bss` 段的数据没有初始值，程序启动时由运行时清零。如果 `__boot_info_ptr` 在 `.bss` 段，`rep stosb` 会把刚保存的 BootInfo 指针清零，后续从 `__boot_info_ptr` 恢复时得到的就是 0，导致内核无法访问 BootInfo。栈空间放在 `.bss` 段是合理的，因为栈不需要初始值，只需要预留空间。

### C++ 运行时支持：最小的 libc++ 替代品

`kernel/mini/arch/x86_64/crt_stub.cpp` 提供了 C++ 运行时的最小实现：

```cpp
extern "C" {

// Pure Virtual Function Call Handler
[[noreturn]] void __cxa_pure_virtual() {
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
```

`__cxa_pure_virtual` 是当纯虚函数被调用时的处理程序。纯虚函数不应该被直接调用（只能通过派生类的覆盖版本），如果发生了，说明程序有 bug。我们的处理方式是死循环 halt，因为没法抛异常（`-fno-exceptions`）。

```cpp
// Stack Smashing Protector Failure Handler
[[noreturn]] void __stack_chk_fail() {
	while (1) {
		__asm__ volatile("cli; hlt");
	}
}
```

`__stack_chk_fail` 是栈保护检测到破坏时的处理程序。虽然我们编译时用了 `-fno-stack-protector`，但某些库或编译器版本可能仍然插入栈保护检查。提供这个函数可以避免链接错误。

```cpp
// Atexit Handler (Minimal Implementation)
int __cxa_atexit(void (*)(void*), void*, void*) {
	return 0;  // Success (but we don't actually register anything)
}
```

`__cxa_atexit` 是注册程序退出时调用的回调函数。我们的内核不会"退出"（没有进程概念），所以这是一个 no-op，直接返回成功。

```cpp
// Global Constructors Initialization
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void _init_global_ctors() {
	for (void (**func)() = __init_array_start; func != __init_array_end; func++) {
		(*func)();
	}
}

}  // extern "C"
```

`_init_global_ctors` 是关键函数。`__init_array_start` 和 `__init_array_end` 是 linker.ld 中定义的符号，标记 `.init_array` 段的起止地址。这个数组包含全局对象的构造函数指针（由编译器自动生成）。我们遍历这个数组，逐个调用。这是 C++ 全局对象构造的标准实现方式。

```cpp
// Operator new/delete (Minimal Implementation)
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

operator new/delete 是 C++ 动态内存分配的操作符。我们的内核还没有实现堆（heap），所以这些操作符如果被调用，说明程序尝试动态分配内存，这是不支持的。我们用 halt 让开发者知道这个限制。如果编译器生成了对 new/delete 的调用（比如某些类的虚析构函数），这些 stub 可以避免链接错误。

### 小内核主函数：C++ 测试代码

`kernel/mini/main.cpp` 是小内核的主函数，包含一系列 C++ 特性测试：

```cpp
// Simple inline function for debugcon output
static void debugcon_putc(char c) {
	__asm__ volatile("outb %0, $0xE9" : : "a"(c));
}
```

`debugcon_putc` 是内联函数，用 `outb` 指令输出字符到 debugcon 端口（0xE9）。QEMU 的 `-debugcon` 选项可以捕获这个输出。

```cpp
// Test 1: Simple Class with Constructor/Destructor
class SimpleClass {
private:
	int	 value;
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

	int	 getValue() const { return value; }
	char getMarker() const { return marker; }
};
```

`SimpleClass` 是一个简单的类，测试构造函数、析构函数和成员函数。构造函数输出 'C' 加上 value 的数字，析构函数输出 'D' 加上 value。这个类用来验证 C++ 类的基本机制能正常工作。

```cpp
// Test 2: Virtual Functions (vtable test)
class Base {
public:
	virtual char getName() = 0;	 // Pure virtual
	virtual int	 compute() = 0;
	virtual ~Base() {}
};

class Derived : public Base {
private:
	int multiplier;

public:
	Derived(int m) : multiplier(m) {
		debugcon_putc('V');	 // 'V' for Virtual
	}

	virtual char getName() override {
		return 'D';	 // 'D' for Derived
	}

	virtual int compute() override { return multiplier * 2; }

	virtual ~Derived() override {
		debugcon_putc('d');	 // lowercase 'd' for Derived destructor
	}
};
```

`Base` 和 `Derived` 测试虚函数和 vtable。`Base` 有纯虚函数，是抽象类。`Derived` 覆盖了这些虚函数。构造函数输出 'V'（Virtual），析构函数输出 'd'（derived destructor）。这个测试验证虚函数调用机制（vtable dispatch）能正常工作，这对 C++ 多态至关重要。

```cpp
// Test 3: Global Object (global constructor test)
static int global_construction_count = 0;

class GlobalCounter {
public:
	GlobalCounter() {
		global_construction_count = 42;	 // Magic value to verify ctor ran
		debugcon_putc('G');				 // 'G' for Global
	}

	int getCount() const { return global_construction_count; }
};

// Global object - constructor should be called by _init_global_ctors
GlobalCounter global_counter;
```

`GlobalCounter` 是一个全局对象，测试全局构造函数。构造函数把 `global_construction_count` 设置为 42（魔数，容易识别），输出 'G'。这个对象是全局的，它的构造函数应该在 main 之前被 `_init_global_ctors` 调用。验证方法是检查 `global_construction_count` 是否等于 42。

```cpp
extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
	// Use the global __boot_info_ptr (now in .data section, no corruption)
	BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
	(void)boot_info_addr;  // Suppress unused warning

	// Start marker: "===CPP"
	debugcon_putc('=');
	debugcon_putc('=');
	debugcon_putc('=');
	debugcon_putc('C');
	debugcon_putc('P');
	debugcon_putc('P');
```

`mini_kernel_main` 是内核主函数。参数 `boot_info_addr` 是 RDI 传递的 BootInfo 指针，但我们不直接用它，而是从 `__boot_info_ptr` 读取（因为 RDI 可能被破坏）。输出 `===CPP` 作为 C++ 代码开始的标记。

```cpp
	// Test 1: Simple Class (stack allocation)
	SimpleClass obj1(1);

	// Verify object state
	if (obj1.getValue() == 1 && obj1.getMarker() == 'S') {
		debugcon_putc('1');  // '1' = SimpleClass test passed
	}

	// Test 2: Virtual Functions
	Derived derived(5);
	Base* base = &derived;

	// Test vtable dispatch
	if (base->getName() == 'D' && base->compute() == 10) {
		debugcon_putc('2');  // '2' = Virtual function test passed
	}

	// Test 3: Global Object (already constructed by _init_global_ctors)
	if (global_counter.getCount() == 42) {
		debugcon_putc('3');  // '3' = Global constructor test passed
	}

	// Verify BootInfo
	if (boot_info->entry_point == 0xFFFFFFFF80020000 && boot_info->kernel_phys_base == 0x20000) {
		debugcon_putc('B');  // 'B' = BootInfo valid
	}
```

测试 1：创建 `SimpleClass` 对象，构造函数输出 'C1'。验证 `value` 和 `marker` 是否正确，输出 '1' 表示测试通过。测试 2：创建 `Derived` 对象，构造函数输出 'V'。通过基类指针调用虚函数，验证 vtable dispatch 是否正确，输出 '2' 表示测试通过。测试 3：检查 `global_construction_count` 是否等于 42，验证全局构造函数是否执行，输出 '3' 表示测试通过。验证 BootInfo 的 `entry_point` 和 `kernel_phys_base` 是否正确，输出 'B' 表示测试通过。

```cpp
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

输出 `===END` 作为测试结束的标记，然后进入死循环 halt。注意这里的析构函数不会被调用，因为主函数永远不返回（`[[noreturn]]`）。

### 链接器脚本：高半内核的地址布局

`kernel/mini/linker.ld` 定义了内核的内存布局，是本章最关键的文件之一：

```ld
OUTPUT_FORMAT("elf64-x86-64")
ENTRY(_start)

/* Kernel physical load address and virtual base offset */
KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;
```

首先定义两个常量：`KERNEL_PHYS_BASE` 是物理加载地址（0x20000），`KERNEL_Virt_BASE` 是高半内核的虚拟基地址（0xFFFFFFFF80000000）。虚拟基地址的选择是有原因的：x86-64 的 canonical address 要求第 47 位到第 63 位必须相同（全 0 或全 1）。0xFFFFFFFF80000000 的第 47-63 位全是 1，满足 canonical 要求，而且为内核预留了巨大的地址空间（128 TB）。

```ld
SECTIONS
{
    /* Higher-half kernel virtual base address with physical offset */
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    /* All read-only sections combined - use wildcard to catch all .text subsections */
    .text : AT(KERNEL_PHYS_BASE) {
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
```

`. = KERNEL_Virt_BASE + KERNEL_PHYS_BASE` 设置当前地址计数器为 0xFFFFFFFF80020000。这是代码段的虚拟地址（VMA）。`AT(KERNEL_PHYS_BASE)` 指定代码段的物理加载地址（LMA）为 0x20000。这意味着 ELF 文件中代码段的内容会被加载到物理地址 0x20000，但代码中的符号引用使用虚拟地址 0xFFFFFFFF80020000。`*(.text .text.*)` 使用通配符捕获所有 `.text` 和 `.text.*` subsection。后者是编译器生成的各种 subsection，如 `.text.unlikely`、`.text.startup` 等。如果不加通配符，这些 subsection 会被丢弃，导致代码缺失。

```ld
    /* All read-write sections combined */
    .data : AT(KERNEL_PHYS_BASE + SIZEOF(.text)) {
        *(.data .data.*)
        __init_array_start = .;
        *(.init_array .init_array.*)
        __init_array_end = .;
    }
```

`.data` 段的 LMA 是物理地址 0x20000 加上 `.text` 段的大小（`SIZEOF(.text)`）。这意味着数据段紧跟在代码段之后。`__init_array_start` 和 `__init_array_end` 是两个符号，标记 `.init_array` 段的起止地址。`.init_array` 段包含全局构造函数指针，供 `_init_global_ctors` 遍历。

```ld
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

`.bss` 段不需要 LMA（没有 `AT(...)`），因为它的内容是零，不需要在 ELF 文件中占用空间。程序启动时由运行时清零（`rep stosb`）。`__bss_start` 和 `__bss_end` 标记 BSS 段的起止地址，供汇编代码使用。`/DISCARD/` 段丢弃不需要的 section，减少 ELF 文件大小。`.eh_frame` 是异常处理帧，我们用 `-fno-exceptions` 不需要它，丢弃可以避免链接器警告。

---

## 设计决策深度分析

### 决策 1：高半内核地址 vs 纯物理地址

**问题**：内核应该运行在高半虚拟地址（如 0xFFFFFFFF80000000）还是纯物理地址（如 0x20000）？

**本项目的做法**：我们采用高半内核地址。内核的虚拟地址是 0xFFFFFFFF80020000（KERNEL_Virt_BASE + KERNEL_PHYS_BASE），物理地址是 0x20000，页表同时提供 identity mapping（0x0000000000020000 -> 0x20000）和 higher-half mapping（0xFFFFFFFF80020000 -> 0x20000）。内核代码使用高半地址，但可以通过 identity mapping 访问物理内存。

**备选方案**：另一种设计是内核直接运行在物理地址上，不做任何虚拟地址映射。或者内核使用任意虚拟地址（如 0xC0000000），由 bootloader 动态配置页表映射。

**为什么不选备选方案**：纯物理地址有几个问题。第一，内核地址空间和用户地址空间混在一起，无法隔离。第二，内核代码中的指针是物理地址，移植性差（不同机器的物理内存布局不同）。第三，无法利用虚拟内存的优势（如延迟映射、按需分页）。任意虚拟地址需要 bootloader 解析内核 ELF header，提取 VMA 信息，动态配置页表，增加复杂度。高半内核地址是业界标准做法（Linux、xv6 都这么做），地址固定、约定俗成、便于理解和调试。

**如果要扩展/改进**：当前实现同时映射 identity 和 higher-half，这是为了方便早期开发（可以直接用物理地址访问硬件）。但 identity mapping 有安全风险，用户程序可能通过它访问内核内存。成熟的做法是只在启动时短暂保留 identity mapping，内核初始化完成后移除它。另一个改进是支持 KASLR（Kernel Address Space Layout Randomization），随机化内核的虚拟地址，增加安全性。

### 决策 2：BootInfo 在 Long Mode 填充 vs Protected Mode 填充

**问题**：BootInfo 结构体应该在哪个模式填充？Real Mode、Protected Mode 还是 Long Mode？

**本项目的做法**：我们在 Long Mode 的 `long_mode_entry` 中填充 BootInfo。此时已经进入 64 位模式，可以使用 64 位寄存器（RAX, RBX, ...），访问 64 位地址空间。填充的数据来自之前的 BIOS 调用结果（E820 在 0x5000，VESA 在 0x6400），用 64 位 mov 指令复制到 BootInfo（0x7000）。

**备选方案**：理论上可以在 Protected Mode 填充 BootInfo，因为那时已经有 32 位寄存器和线性地址。或者在 Real Mode 最后填充，用 16 位代码操作（但会很痛苦，因为 BootInfo 有 64 位字段）。

**为什么不选备选方案**：Protected Mode 填充也可以，但不如 Long Mode 方便。Long Mode 有 64 位寄存器，可以直接操作 64 位值（如 `entry_point`），不需要分两次 mov（低 32 位 + 高 32 位）。而且 Long Mode 已经是最终模式，填充后直接跳转，逻辑流畅。Real Mode 填充太痛苦，16 位代码操作 64 位字段需要多次 mov，而且 Real Mode 的 segment:offset 地址计算很烦。

**如果要扩展/改进**：当前实现是把数据从一个固定位置（0x5000/0x6400）复制到另一个固定位置（0x7000）。更灵活的设计是让这些数据结构通过指针链接，而不是硬编码地址。或者用结构化格式（如 Device Tree 或 ACPI 表）传递信息，而不是自定义的 BootInfo。另一个改进是压缩存储（如 E820 条目很多时可以只保留关键区域），节省内存。

### 决策 3：`.data` vs `.bss` 段的选择

**问题**：全局变量应该放在 `.data` 段还是 `.bss` 段？有什么区别？

**本项目的做法**：我们把 `__boot_info_ptr` 放在 `.data` 段，而不是 `.bss` 段。`.data` 段的变量在 ELF 文件中有初始值，加载时直接写入内存。`.bss` 段的变量没有初始值，程序启动时由运行时清零。因为 `__boot_info_ptr` 需要在 BSS 清除之前保存 BootInfo 指针，如果在 `.bss` 段会被 `rep stosb` 清零。

**备选方案**：另一种做法是调整 BSS 清除逻辑，不清除 `__boot_info_ptr` 所在的内存。或者把 BootInfo 指针保存在寄存器中，不存到内存。

**为什么不选备选方案**：调整 BSS 清除逻辑会增加复杂度，需要计算 `__boot_info_ptr` 的地址并在清除时跳过它，容易出错。保存在寄存器中不可行，因为寄存器有限，而且后续代码可能破坏寄存器。放在 `.data` 段是最简单的解决方案，`.data` 段的数据不会被运行时清零，自然避免了被破坏。

**如果要扩展/改进**：更严格的段划分是：`.data` 段放有初始值的读写数据，`.bss` 段放零初始化的读写数据，`.rodata` 段放只读数据。这样可以优化内存使用：`.rodata` 可以共享，`.bss` 可以不占 ELF 文件空间。链接器可以通过 `-Wl,--gc-sections` 丢弃未使用的 section，减少二进制大小。另一个改进是使用自定义段（如 `.bootdata`）专门存放启动相关的数据，与普通内核数据分离。

---

## 常见变体与扩展方向

下面列出几个你可以尝试的扩展实验：

1. **⭐ 添加更多 C++ 特性测试**：当前测试覆盖了构造/析构函数、虚函数、全局对象，你可以添加更多测试，如异常（需要 `-fexceptions`）、RTTI（需要 `-frtti`）、模板元编程、lambda 表达式等，验证 C++23 特性在 freestanding 环境中的支持程度。

2. **⭐ 实现 operator new/delete**：当前 new/delete 只是 halt，你可以实现一个简单的堆分配器，比如 bump allocator（指针线性增长）或 bitmap allocator（位图管理）。实现后可以测试 `std::vector` 等容器是否能正常工作。

3. **⭐⭐ 解析 BootInfo 并输出信息**：当前内核只是验证 BootInfo 是否有效，你可以扩展代码，遍历 `mmap[]` 数组，输出所有内存区域（base, length, type）；或者根据 fb_info 在 framebuffer 上绘制像素、显示文字。

4. **⭐⭐ 移除 Identity Mapping**：当前同时映射 identity 和 higher-half，你可以修改 `setup_page_tables`，移除 identity mapping（PML4[0]），只保留 higher-half mapping。这样内核只能通过虚拟地址访问内存，安全性更高，但需要确保所有代码都使用虚拟地址。

5. **⭐⭐⭐ 实现位置无关代码（PIC）内核**：用 `-fPIC` 编译内核，使内核可以加载到任意物理地址而不需要重定位。Bootloader 可以根据当前内存情况选择合适的加载位置，而不是硬编码 0x20000。这需要修改链接脚本，使用 `%rip` 相对寻址访问全局变量。

---

## 参考资料

### Intel/AMD 手册

* **Intel SDM Vol. 3A, Section 4.5**: 4-Level Paging and 5-Level Paging — x86-64 页表结构的详细说明
* **Intel SDM Vol. 2A, Chapter 2**: Instruction Format — MOV 指令的各种编码（MOVZX/MOVSX/MOVSXD）
* **Intel SDM Vol. 1, Section 3.4.1**: Canonical Address — Canonical 地址的要求和定义
* **AMD64 Architecture Programmer's Manual Vol. 2**, Section 5.6: Data Types — 32 位和 64 位下的数据类型和对齐

### OSDev Wiki

* [Higher Half](https://wiki.osdev.org/Higher_Half) — 高半内核的原理和实现
* [C++](https://wiki.osdev.org/C%2B%2B) — OS 开发中 C++ 的使用注意事项
* [Global Constructor](https://wiki.osdev.org/Using_Global_Constructors) — 全局构造函数的调用方法
* [Symbol Resolution](https://wiki.osdev.org/Using_Section_Attributes) — 链接器脚本和符号解析

### 其他资源

* [System V AMD64 ABI](https://gitlab.com/x86-psABIs/x86-64-ABI) — 函数调用约定，参数传递规则
* [ELF for the AMD64 Architecture](https://www.uclibc.org/docs/elf-64-gen.pdf) — ELF64 格式规范
* [GNU LD Documentation](https://sourceware.org/binutils/docs/ld/) — 链接器脚本的完整语法
* [C++ Runtime on OSDev](https://wiki.osdev.org/C%2B%2B_Runtime) — C++ 运行时的最小实现要求

---

到这里就大功告成了。本章我们完成了从"能跳转"到"能运行 C++ 代码"的关键跨越。虽然 mini kernel 只是做了一些测试然后 halt，但这个完整的测试链路代表着我们成功地让内核具备了现代 C++ 的运行环境。从 `OPL`（bootloader 标记）到 `J`（即将跳转）到 `1234`（启动阶段）到 `===CPPGC1V123B===END`（C++ 测试结果），每个字符都是启动链路的一个里程碑。

但事情到这里还没完，这些字符背后踩过的坑可能比你想的要多。第一个坑是 BootInfo 参数被破坏——`%rdi` 在 BSS 清除时被覆盖，导致内核收到错误的指针。解决方法是在清除前保存到 `.data` 段。第二个坑是 `.bss` 符号地址冲突——`__boot_info_ptr` 和 `global_counter` 被分配到同一个地址，导致指针被覆盖成 `0x2a00000000`（42 << 32）。解决方法是把 `__boot_info_ptr` 移到 `.data` 段，避免与 C++ 全局变量冲突。这两个 bug 的调试笔记（`document/notes/006/`）记录了完整的排查过程，值得一读。

下一章我们会在内核里做更多实际的事情：实现内存管理器、设置中断处理、编写键盘驱动、在 framebuffer 上显示文字。那时候，我们就能看到一个更像"操作系统"的东西了。但在此之前，好好品味一下这个启动过程：从 MBR 的 512 字节开始，到 Stage2，到 Real Mode 的 BIOS 调用，到 Protected Mode 和 Long Mode 的切换，到高半内核映射，到 BootInfo 填充，到 C++ 运行时初始化，最终跳转到我们自己的 C++ 代码。每一步都是精心设计的，每一步都有其历史和技术原因。理解了这些，你就理解了 x86-64 启动的精髓。
