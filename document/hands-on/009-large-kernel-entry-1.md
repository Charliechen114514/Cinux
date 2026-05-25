---
title: 009-large-kernel-entry-1 · 大内核入口
---

# 009-1 Higher-Half 大内核启动 — 链接脚本 + 启动汇编 + C++ 运行时

## 章节导语

上一章（008）我们把 ATA 磁盘驱动和 ELF 加载器搞定了，mini kernel 已经有能力从磁盘读取一个大内核 ELF 文件、解析它的 PT_LOAD 段、把它们搬运到正确的物理地址。但问题在于——跳过去之后第一件事做什么？大内核的代码链接在什么样的地址上？这些问题在 008 里一笔带过了，因为那时候大内核本身还不存在。

这一章我们要做的事情就是让大内核真正"站起来"。具体来说，我们需要编写大内核自己的链接脚本（linker script），把代码链接到 higher-half 虚拟地址 `0xFFFFFFFF80000000`；编写一段启动汇编（boot.S），负责在 mini kernel 跳过来之后的第一时间设置栈、清零 BSS、运行全局构造器、最后跳进 C++ 世界；再写一组 C++ 运行时桩函数（crt_stub），把编译器在 freestanding 环境下期望的那些符号全部补齐。完成本章后，你会看到 QEMU 串口输出 `[BIG] Big kernel running @ 0x1000000`——这意味着大内核的第一行代码已经成功执行了。

本章的前置知识是上一章的磁盘驱动和 ELF 加载器，因为大内核是被 mini kernel 从磁盘加载并跳转过来的。

---

## 概念精讲

### 为什么内核要住在 higher-half？

我们的 mini kernel 不是一直在物理地址的 identity mapping 下跑得好好的吗？为什么大内核非得搬到 `0xFFFFFFFF80000000` 这么远的地址上去？

原因有好几个。最直接的一个是**用户空间的地址空间规划**。在现代操作系统中，内核和用户进程共享同一个虚拟地址空间，如果我们把内核放在低地址，用户进程的代码就没法用这些地址了。而把内核放到虚拟地址空间的最高端（x86-64 的 canonical high 区域），低地址就完全留给用户进程——每个进程都能拥有一整块连续的地址空间，内核映射在所有进程的页表里始终存在，系统调用的时候不需要切换页表，只需要修改 CS:RIP 跳到高地址就行了。

另一个原因是**安全性**。Higher-half 的布局天然地把内核地址和用户地址分开了，只要在页表项里把 U/S bit 设对，用户态的代码根本不可能碰触到内核空间。所以几乎所有现代 OS（Linux、Windows、macOS）都采用 higher-half 内核的设计。我们选择的 `0xFFFFFFFF80000000` 是一个常见的基址选择——Linux 内核也用这个地址作为 `__START_KERNEL_map`。

```
x86-64 虚拟地址空间布局 (48-bit canonical):

0x0000000000000000 ┌──────────────────┐
                  │   用户空间 (128 TB) │  低半部分，每个进程独立
0x00007FFFFFFFFFFF ├──────────────────┤ ← canonical boundary
                  │  不可访问 (hole)   │  非 canonical 地址，访问触发 #GP
0xFFFF800000000000 ├──────────────────┤
                  │   内核空间 (128 TB) │  高半部分，所有进程共享
0xFFFFFFFF80000000 │  ← KERNEL_VMA     │  大内核从这里开始
0xFFFFFFFFFFFFFFFF └──────────────────┘
```

### VMA 和 LMA 到底是什么？

在链接脚本里你会频繁看到两个概念：VMA（Virtual Memory Address）和 LMA（Load Memory Address），理解这两个概念是搞懂 higher-half 内核链接过程的关键。

VMA 是"链接器认为这个代码/数据在运行时应该出现在哪个虚拟地址"，编译器生成相对跳转、绝对地址引用、函数指针这些的时候，用的都是 VMA。LMA 是"这个代码/数据在加载时实际应该被放到哪个物理地址"。我们的 ELF 加载器在加载大内核的时候，页表还是 mini kernel 设置的 identity mapping，CPU 还不认识 `0xFFFFFFFF80000000` 这个虚拟地址——所以段数据必须被加载到一个物理地址（也就是 LMA），代码才能在 identity mapping 下正常执行。

在链接脚本里，关键公式是 `LMA = VMA - KERNEL_VMA`。每个 section 用 `AT()` 伪操作显式指定了 LMA。这样一来，ELF 文件的程序头里 `p_vaddr` 是高地址（供将来开启分页后使用），`p_paddr` 是低地址（供当前 identity mapping 使用），两边都不耽误。

```
链接脚本地址关系:

VMA (链接器看到的地址):        LMA (加载器放到内存的地址):
0xFFFFFFFF80000000 ─┐          0x01000000 ─┐
  .text.start       │ -0xFFFFFFFF80000000   │
  .text             │  ──────────────────→   │ 物理内存中的实际位置
  .rodata           │  AT() 做的减法         │
  .data             │                        │
  .init_array       │                        │
  .bss              │                        │
  stack             │                        │
0xFFFFFFFF80XXXXXX ─┘          0x01XXXXXX ─┘
```

### .init_array 和全局构造器

如果你写的是 C 内核，启动汇编只需要设置栈、清 BSS、调用 main 就行了。但 C++ 有一套额外的机制：全局对象的构造函数必须在 main 之前被调用。编译器处理全局对象的方式是生成一个构造器函数指针，把它放进 `.init_array` section 里。启动代码需要遍历这个数组，逐个调用这些构造器。如果遗漏了这一步，所有带非平凡构造函数的全局对象都会处于未初始化状态——虚函数表指针是垃圾数据，调用任何方法都是未定义行为。

---

## 动手实现

### Step 1: 编写大内核链接脚本

**目标**：定义大内核的内存布局，把代码链接到 higher-half 虚拟地址空间，同时确保 ELF 的程序头包含正确的物理地址信息供 mini kernel 的 ELF 加载器使用。

**设计思路**：

我们需要创建一个链接脚本 `kernel/linker.ld`。开头声明输出格式为 `elf64-x86-64`、目标架构为 `i386:x86-64`、入口点为 `_start`（定义在 boot.S 里）。然后定义两个地址常量：`KERNEL_VMA = 0xFFFFFFFF80000000`（higher-half 虚拟基地址）和 `KERNEL_LMA = 0x1000000`（16MB 物理加载地址）。这个 16MB 要和 mini kernel 的 `BIG_KERNEL_LOAD_ADDR` 常量一致。

SECTIONS 块里最关键的一行是把位置计数器设为 `KERNEL_VMA + KERNEL_LMA`。之后所有 section 的 VMA 会从 `0xFFFFFFFF80100000` 开始递增。每个 section 都用 `AT(ADDR(.section) - KERNEL_VMA)` 显式指定了 LMA——把 VMA 减去 `0xFFFFFFFF80000000` 得到纯物理偏移量。

Section 排列顺序是有讲究的：`.text.start` 必须放在 `.text` 最前面（确保 `_start` 出现在 ELF 文件最开头）；`.text` 和 `.rodata` 合并；`.data` 单独一个段；`.init_array` 用 `KEEP()` 防止被 `--gc-sections` 删掉；`.bss` 不需要 `AT()` 因为它不占文件空间；最后分配 16KB 的 `.stack (NOLOAD)` 段。

**踩坑预警**：`KERNEL_LMA` 的值必须和 mini kernel 的 `BIG_KERNEL_LOAD_ADDR` 一致，否则加载器搬过去的段对不上链接器期望的物理地址，整个内核就是一团乱。

**验证**：完成链接脚本后，配合后续的 boot.S 和 CMakeLists.txt 一起构建，用 `readelf -l` 检查生成的 ELF 的 program header，确认 `p_vaddr` 是 `0xFFFFFFFF80100000` 起始的高地址，`p_paddr` 是 `0x1000000` 起始的物理地址。

```bash
# 构建完成后检查 ELF headers
readelf -l build/kernel/big/big_kernel
# 预期：看到 PT_LOAD 段，p_paddr 从 0x1000000 开始，p_vaddr 从 0xFFFFFFFF80100000 开始
```

### Step 2: 编写大内核启动汇编 boot.S

**目标**：实现 `_start` 函数——大内核执行的第一段代码。它需要依次完成六步操作：禁用中断、设置栈、清零 BSS、运行全局构造器、调用 kernel_main、halt 循环。

**设计思路**：

把这段代码放在 `.section .text.start, "ax"` 里，确保它被链接到 ELF 文件的最开头。标记为 `.code64` 因为 mini kernel 在 long mode 下跳转过来。

整个流程是六个步骤的严格序列，每一步都依赖前一步的正确完成。

**实现约束**：

Step 1 只需要一条 `cli` 指令禁用中断——因为大内核还没有设置自己的 IDT，如果中断开启，任何硬件中断都会触发 CPU 去查 mini kernel 的 IDT，大概率崩溃。

Step 2 设置栈：把链接脚本里定义的 `__kernel_stack_top` 的地址加载到 RSP，然后把 RBP 清零标记调用链底部。x86-64 的栈是向下增长的，所以"栈顶"是这片内存的最高地址。

Step 3 清零 BSS 是非常关键的一步，把 `__bss_start` 的地址放进 `%rdi`、`__bss_end` 放进 `%rcx`，算出字节数后用 `rep stosb` 指令填充零。我们不在 ELF 加载器中依赖 BSS 清零，而是在启动汇编里无条件做一遍，保证万无一失。

**踩坑预警**：`rep stosb` 会破坏 `%rdi`、`%rcx`、`%rax` 的值！这意味着如果 mini kernel 在跳转时通过 `%rdi` 传递了 BootInfo 指针（System V AMD64 ABI 的第一个参数寄存器），经过 BSS 清零之后这个值就丢了。目前 milestone 009 不需要 BootInfo，所以 Step 5 直接传 NULL。如果后续需要 BootInfo，必须在 BSS 清零之前把 `%rdi` 保存起来（比如压栈或者存到另一个寄存器），清零之后再恢复。

Step 4 调用 `_init_global_ctors()`，遍历 `.init_array` section 中的函数指针数组。当前内核还没有带非平凡构造函数的全局对象，所以 `.init_array` 是空的，这个调用什么也不做——但基础设施必须提前搭好。

Step 5 调用 `kernel_main()`，进入 C++ 世界。因为 BSS 清零破坏了 `%rdi`，这里显式清零后传 NULL 给 kernel_main。

Step 6 是一个三指令死循环：`cli; hlt; jmp .halt`。为什么三行而不是一行 `hlt`？因为 `hlt` 在收到 NMI 时会被唤醒，没有 `jmp` 的话 CPU 会继续往下执行垃圾指令，导致 triple fault。

**验证**：

```bash
# 配合后续步骤构建完整内核后，查看反汇编确认 _start 位于文件开头
objdump -d build/kernel/big/big_kernel | head -30
# 预期：第一条指令是 cli (0xFA)
```

### Step 3: 实现 C++ 运行时桩函数 crt_stub.cpp

**目标**：提供 freestanding C++ 环境下编译器期望的运行时函数，包括纯虚函数调用处理、栈保护失败处理、atexit 注册、全局构造器初始化、operator new/delete 的 stub。

**设计思路**：

这个文件里的函数分为两类：一类代表"不应该发生但编译器需要链接"的编程错误处理（`__cxa_pure_virtual`、`__stack_chk_fail`），另一类是内核环境的必要适配（`__cxa_atexit`、`_init_global_ctors`、operator new/delete）。

**实现约束**：

编程错误处理函数应该在 QEMU 的 debug console（I/O 端口 0xE9）输出一个标识字符（比如 'V' 代表纯虚函数、'S' 代表栈保护），然后死循环。这样在 `debug.log` 里看到一个字符就知道出了什么问题。

`__cxa_atexit` 直接返回 0（成功），内核永不退出，不需要析构注册。

`_init_global_ctors` 获取链接脚本里定义的 `__init_array_start` 和 `__init_array_end` 两个边界符号，遍历其间的函数指针数组逐个调用。注意要检查 NULL 指针，因为对齐填充可能导致空隙。

operator new/delete 必须放在 `extern "C"` 块的外面（需要 C++ name mangling），实现为死循环而非返回 nullptr——如果有人不小心用了 new，CPU 会立刻卡住，在 GDB 里很容易定位。

**踩坑预警**：`__init_array_start` 和 `__init_array_end` 在 C++ 里的声明方式是 `extern void (*__init_array_start[])()`——把它们声明为函数指针数组。链接器分配给这两个符号的"值"就是它们的地址本身，声明成数组后 start 和 end 就分别指向数组首元素和尾后元素。

**验证**：

```bash
# 构建 + 链接，确认没有 undefined reference 报错
cmake --build build --target big_kernel
# 预期：链接成功，没有 __cxa_pure_virtual、__stack_chk_fail 等符号的未定义错误
```

### Step 4: 编写 kernel_main 入口

**目标**：实现 `kernel_main()` 函数——大内核的 C++ 入口点，初始化串口并打印里程碑消息。

**设计思路**：

这个函数做的事情很直白：先调用 `kprintf_init()` 初始化串口（COM1，115200 8N1），然后打印一行里程碑消息 `[BIG] Big kernel running @ 0x1000000`，最后进入死循环。大内核使用的是 `kernel/lib/kprintf.hpp`（命名空间 `cinux::lib`），和 mini kernel 的 kprintf 是两套独立的实现。之所以要重新初始化串口，是因为不能假设 mini kernel 留下的硬件状态。

函数签名是 `extern "C" void kernel_main()`，不返回。boot.S 的 `.halt` 标签也有死循环兜底。

**验证**：

```bash
# 完整构建并运行（需要所有文件就绪）
cmake --build build && make run
# 预期 QEMU 串口输出：
# [MINI] ... (mini kernel 的加载日志)
# [BIG] Big kernel running @ 0x1000000
```

---

## 构建与运行

完整的构建需要配合 CMakeLists.txt 中定义的 `big_kernel` target。构建系统会编译大内核的所有源文件（boot.S、crt_stub.cpp、serial.cpp、kprintf.cpp、main.cpp），用 `kernel/linker.ld` 链接，生成 ELF 文件。`build_image.sh` 脚本会把 MBR + Stage2 + mini kernel + big kernel ELF 写入磁盘镜像。

```bash
# 完整构建
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 运行
cd build && make run
# 预期：QEMU 启动，串口输出包含 [BIG] Big kernel running @ 0x1000000
```

---

## 调试技巧

### 常见 Bug 1: 大内核不输出任何信息

如果 QEMU 串口没有任何 `[BIG]` 输出，说明大内核的 `_start` 可能没有被执行到。排查步骤：先用 GDB 在 `_start` 设断点，看 mini kernel 是否成功跳转过来。如果断点没命中，检查 mini kernel 的 ELF 加载器是否正确解析了入口点地址（`e_entry - KERNEL_VMA`）。

### 常见 Bug 2: 串口输出乱码

通常是串口初始化问题。确认 `kprintf_init()` 被调用了，确认 QEMU 启动参数带了 `-serial stdio`。如果串口配置不正确（比如 LCR 设错了），输出的波特率/数据位不匹配会导致乱码。

### 常见 Bug 3: kernel_main 之后 triple fault

检查 BSS 清零是否越界（`__bss_start` 和 `__bss_end` 的值是否合理），检查栈指针 `__kernel_stack_top` 是否指向有效内存。用 GDB 的 `info registers` 查看 RSP 的值是否在合理范围内。

### GDB 调试命令

```bash
# 在另一个终端启动 GDB
gdb build/kernel/big/big_kernel
(gdb) target remote :1234
(gdb) break _start
(gdb) break kernel_main
(gdb) continue
```

---

## 本章小结

| 概念 | 要点 |
|------|------|
| KERNEL_VMA | `0xFFFFFFFF80000000`，higher-half 虚拟基地址 |
| KERNEL_LMA | `0x1000000`（16MB），物理加载地址 |
| VMA/LMA 分离 | 链接脚本用 `AT(ADDR(.section) - KERNEL_VMA)` |
| boot.S 流程 | cli → 设栈 → 清 BSS → 全局构造器 → kernel_main → halt |
| rep stosb | 清零 BSS，破坏 %rdi/%rcx/%rax |
| .init_array | 全局构造器指针数组，KEEP() 防止被删 |
| crt_stub | __cxa_pure_virtual、__stack_chk_fail、__cxa_atexit、new/delete |
| operator new/delete | 死循环（无堆分配器），放在 extern "C" 外 |
