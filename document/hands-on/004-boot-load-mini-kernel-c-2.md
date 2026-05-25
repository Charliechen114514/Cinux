---
title: 004-boot-load-mini-kernel-c-2 · 内核加载 (C)
---

# 004-C 动手篇（下）：内核入口与 C++ 运行时

> 本章完成后的可见效果：debugcon 日志输出 `OPLJ123G4===CPPGC1V123B===END`，全部 C++ 特性测试通过
>
> 前置要求：已完成本系列上篇（BootInfo 填充与高半核跳转），Bootloader 能成功 `jmp` 到 Mini Kernel 入口

---

## 导语

上一篇的末尾，Bootloader 输出了 `J` 然后一跳了之，把控制权交给了物理地址 0x20000（虚拟地址 0xFFFFFFFF80020000）处的 Mini Kernel。但跳过去之后呢？CPU 面对的景象是这样的：SSE 指令还没启用（Long Mode 下 SSE 默认不开，得手动置 CR4 的位），栈指针不知道指向哪里，BSS 段里全是垃圾数据（没人帮你清零），全局对象的构造函数一个都没调用——而 `%rdi` 里辛辛苦苦传过来的 BootInfo 指针随时可能被下一步操作毁掉。

这一章我们要做的，就是在内核入口 `boot.S` 中把上面这些问题逐一解决，然后搭建一个最小的 C++ freestanding 运行时（crt_stub），让内核的 `mini_kernel_main` 可以安心地用类、虚函数、全局对象这些现代 C++ 特性。最后我们用一组测试来验证所有特性工作正常。

说句实话，这一章踩的坑比写的功能还多。BSS 清除指令 `rep stosb` 会破坏 `%rdi` 里的 BootInfo 指针——这个我们提前知道了还好办。但真正让人血压拉满的是第二个坑：`__boot_info_ptr` 这个保存指针的变量原本放在 `.bss` 段，结果链接器把它和一个 C++ 全局对象分配到了同一个地址，全局构造函数一跑，`42` 这个测试常量就覆盖了 BootInfo 指针，产生了 `0x2a00000000` 这种让人怀疑人生的神秘值。整个排查过程在 `document/notes/004-C/` 里有详细记录，我会在下面的实现步骤中穿插讲解。

---

## 概念精讲

### BSS 段与 rep stosb：一把双刃剑

BSS（Block Started by Symbol）段是 C/C++ 程序中存放"零初始化全局变量"的区域。编译器不会在 ELF 文件里为 BSS 分配实际空间——它只记录"这个段有多大"，运行时由启动代码负责把整块区域清零。在普通的应用程序里，C 运行时（crt0）或操作系统的 ELF 加载器帮你做了这件事，你完全感知不到。但写内核的时候没有人帮你，你得自己来。

`rep stosb` 是 x86 的重复存储指令：它把 `%al`（或 `%rax` 的低字节）的值重复写入 `%rdi` 指向的内存地址，每次写入后 `%rdi` 自动递增（如果方向标志 DF=0），`%rcx` 自动递减，直到 `%rcx` 变成 0。这相当于一个超高效的 `memset`。我们用它来清除 BSS——把 `%rdi` 设为 `__bss_start`，`%rcx` 设为 `__bss_end - __bss_start`，`%rax` 清零，然后 `rep stosb` 一发搞定。

但问题就出在这里：`rep stosb` 会修改 `%rdi`——执行完后 `%rdi` 不再指向 BootInfo 了，而是指向 BSS 末尾的某个位置。更糟糕的是，如果你把用来保存 BootInfo 指针的变量 `__boot_info_ptr` 放在了 `.bss` 段，那 `rep stosb` 会把这个变量本身也清零——你存的 0x7000 被清成了 0x0，等于白存。所以保存 `%rdi` 的时机和位置都大有讲究，稍后实现步骤里我们会详细展开。

Intel SDM Vol. 2A 对 `STOS`/`STOSB` 指令有完整的描述，包括 DF 标志位对 `%rdi` 递增方向的影响——正常情况下我们之前应该已经用 `cld` 清过方向标志了，但如果你不确定，最好在 `rep stosb` 之前加一条 `cld` 保平安。

### .init_array：C++ 全局构造函数的秘密入口

在普通 C++ 程序里，全局对象会在 `main()` 之前自动调用构造函数。这件事是谁做的？答案藏在 `.init_array` 段里。GCC 编译器在编译每个含全局对象的翻译单元时，会生成一个特殊的函数（名字类似 `_GLOBAL__sub_I_xxx`），这个函数的功能就是调用该翻译单元里所有全局对象的构造函数。编译器把这个函数的指针放进 `.init_array` 段。链接时，所有翻译单元的 `.init_array` 段被合并成一个函数指针数组。C 运行时在启动阶段遍历这个数组，依次调用每个指针指向的构造函数。

在有标准库的环境下，crt0 里的 `_start` 最终会调用 `__libc_csu_init` 来做这件事。但在我们的 freestanding 内核里，没有 crt0，没有 libc，所以必须自己写一个 `_init_global_ctors` 函数来遍历 `.init_array`。链接器脚本里用 `__init_array_start` 和 `__init_array_end` 两个符号标记这个数组的起止边界——遍历逻辑就是从这个起始地址走到结束地址，每 8 字节取一个函数指针然后调用它。

有一点很关键：链接器脚本的 `.init_array` 段必须用 `KEEP()` 包裹，否则如果开启了 section garbage collection（`-ffunction-sections -fdata-sections` 加上 `--gc-sections`），链接器可能认为这些函数指针"没有被引用"而把整个段砍掉——那你的全局对象就永远得不到构造了。OSDev Wiki 的 [Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors) 页面对这个流程有非常详细的讲解，Cinux 采用的就是该页面提到的"ARM BPABI 方式"——直接遍历 `.init_array`，不依赖 `.init` 段的链式调用。

### C++ freestanding 环境：哪些能用，哪些得自己造

内核编译时加了 `-ffreestanding -fno-exceptions -fno-rtti -nostdlib`，这意味着标准库不可用、异常不可用、RTTI 不可用。但 C++ 语言本身的大部分特性仍然可用：类、构造/析构函数、继承、虚函数、vtable——这些都是编译期特性，编译器直接生成代码，不依赖运行时库。

然而，有几样东西是运行时必须提供的。编译器在生成代码时会"假定"某些函数存在：调用纯虚函数时跳转到 `__cxa_pure_virtual`（应该永远不会触发，但你必须提供实现，否则链接报错）、栈保护失败时调用 `__stack_chk_fail`（如果你开了 `-fstack-protector`）、`atexit` 注册函数 `__cxa_atexit`（即使你不支持进程退出也得给个空实现链接通过）。另外，`operator new` 和 `operator delete` 也必须提供——即使我们暂时不想实现真正的堆分配器，也得给一个"调用就停机"的 stub，否则如果有任何代码路径触发了 new/delete，链接就会失败。

OSDev Wiki 的 [C++ Bare Bones](https://wiki.osdev.org/C++_Bare_Bones) 页面列出了 freestanding 环境下需要提供的完整运行时函数清单。Cinux 的 `crt_stub.cpp` 就是对这个清单的实现。

---

## 动手实现

### Step 1: 编写内核入口 boot.S——SSE 启用、栈设置、BSS 清除、全局构造函数调用

**目标**：在 `kernel/mini/arch/x86_64/boot.S` 中实现 `_start` 入口函数，依次完成：关闭中断、启用 SSE、设置栈指针、保存 BootInfo 指针、清除 BSS、调用全局构造函数、恢复 BootInfo 指针到 `%rdi`、跳转 `mini_kernel_main`。

**设计思路**：内核入口的第一件事是 `cli` 关中断——此时 IDT 还没设置，任何中断都会触发 Triple Fault。接下来启用 SSE：Long Mode 下要使用 SSE 指令（比如浮点运算、某些编译器生成的 SIMD 优化），必须在 CR4 寄存器中置位 OSFXSR（bit 9，表示操作系统支持 SSE）和 OSXMMEXCPT（bit 10，表示操作系统支持 SIMD 浮点异常）。设置方法是把 CR4 读到 `%rax`，和这两个位的掩码做 OR，再写回 CR4。

栈的设置很直接：把 `%rsp` 设为 `__mini_stack_top` 的值（这是一个在 `.bss` 段定义的符号，指向 8KB 栈区域的顶端，因为栈是向下增长的），同时把 `%rbp` 清零以标记栈帧的底部。

接下来是最关键的步骤——保存 `%rdi`。Bootloader 通过 `%rdi` 把 BootInfo 的物理地址 0x7000 传了过来，但这个寄存器是 caller-saved 的，任何后续操作都可能修改它。特别是下一步 BSS 清除要用 `rep stosb`，这条指令会直接把 `%rdi` 改成 BSS 的起始地址。所以我们必须在 BSS 清除之前，把 `%rdi` 的值保存到 `__boot_info_ptr` 这个内存变量里。而 `__boot_info_ptr` 本身必须放在 `.data` 段——不能放在 `.bss` 段，否则 BSS 清除会把它也清零。

BSS 清除的逻辑是：把 `__bss_start` 的地址加载到 `%rdi`，`__bss_end` 减去 `__bss_start` 算出字节总数放入 `%rcx`，`%rax` 清零，然后 `rep stosb` 一口气全部清零。执行完后 `%rdi` 已经变了，`%rcx` 是 0，`%rax` 是 0——三个寄存器全废了，这就是为什么前面必须先保存 BootInfo 指针。

BSS 清除之后、调用 C++ 代码之前，我们要先调用 `_init_global_ctors` 来初始化所有全局对象。这个函数在 crt_stub.cpp 中实现（下一步会讲），它的作用是遍历 `.init_array` 段中的函数指针数组，依次调用每个全局构造函数。

最后一步：把 `__boot_info_ptr` 里的值重新加载到 `%rdi`（因为之前的 BSS 清除已经毁了 `%rdi`），然后 `call mini_kernel_main` 正式进入 C++ 世界。如果 `mini_kernel_main` 意外返回了（理论上不会，因为它声明了 `[[noreturn]]`），后面跟一个 `cli; hlt; jmp .-2` 的死循环兜底。

在每个关键步骤之间输出一个 debugcon 字符（`1` 表示到达入口、`2` 表示栈就绪、`3` 表示 BSS 清除完成、`4` 表示全局构造函数调用完毕），方便在日志中定位问题出现在哪一步。

**踩坑预警**：这一步有两个大坑，都是我们实打实踩过的。第一个坑前面已经反复强调了——`rep stosb` 会破坏 `%rdi`，所以保存 BootInfo 指针必须在 BSS 清除之前。如果你先 `rep stosb` 再 `movq %rdi, __boot_info_ptr`，那你存的就是 BSS 的结束地址而不是 0x7000 了。第二个坑更阴险：即使你在 BSS 清除之前就存了值到 `__boot_info_ptr`，如果这个变量本身在 `.bss` 段里，那 `rep stosb` 一跑它就变回 0 了。详细排查过程见 `document/notes/004-C/boot_info_param_corruption.md`。

**验证**：每个子步骤都有对应的 debugcon 输出。运行后如果日志里出现 `1234` 这四个字符，说明入口序列全部正确执行。如果卡在某一步之后（比如有 `12` 但没有 `3`），说明 BSS 清除那里出了问题——可能是 `__bss_start` 和 `__bss_end` 的值不对（检查链接器脚本），或者 `rep stosb` 的方向标志有问题。

### Step 2: 将 __boot_info_ptr 放在 .data 段（不是 .bss！）

**目标**：在 `boot.S` 中把 `__boot_info_ptr` 的定义放在 `.data` 段，用 `.quad 0` 显式初始化为 8 字节的零值，确保它不会被 `rep stosb` 清除。

**设计思路**：`.data` 段存放有初始值的全局变量，它的内容会被加载器从 ELF 文件复制到内存中。`.bss` 段存放零初始化的全局变量，它在 ELF 文件里不占空间，运行时由启动代码清零。两者的关键区别是：BSS 清除只影响 `__bss_start` 到 `__bss_end` 之间的地址范围。`__boot_info_ptr` 如果在 `.data` 段，它的地址就不在这个范围内，所以 `rep stosb` 不会碰它。

在汇编文件中定义的方式是：先切换到 `.data` 段（`.section .data`），声明全局符号（`.global __boot_info_ptr`），8 字节对齐（`.align 8`），然后放一个 8 字节的零值（`.quad 0`）。这个 `.quad 0` 很重要——它让链接器知道这是一个"有初始值"的变量，放在 `.data` 段而不是 `.bss` 段。如果你写成 `.skip 8`（分配 8 字节但不给初始值），链接器可能还是会把它放进 `.bss` 段。

**踩坑预警**：这里隐藏着本章最大的坑——`.bss` 段符号地址冲突。把 `__boot_info_ptr` 放在 `.bss` 段时，我们最初用的是 `.skip 8` 后跟标签的写法。这种写法在汇编层面创建了一个没有"大小"信息的符号（大小为 0），而 C++ 编译器生成的 `global_counter` 变量是有大小信息的（1 字节）。链接器在分配 `.bss` 段时，把这两个符号放在了同一个地址 `0xffffffff800226e8`——因为链接器认为 `__boot_info_ptr` 大小为 0，不占空间，下一个变量可以直接从这里开始。结果就是：全局构造函数执行 `global_construction_count = 42` 时，这个值通过某种内存布局重叠写入了 `__boot_info_ptr` 的位置，产生了 `0x2a00000000`（42 左移 32 位）这个让人怀疑人生的神秘值。排查过程用到了 `objdump -t` 检查符号表，发现两个符号地址完全相同——那一刻的恍然大悟感，不亚于修了一个三天的 bug。完整调试记录在 `document/notes/004-C/bss_data_symbol_conflict.md`。

**验证**：编译后用 `objdump -t build/kernel/mini/mini_kernel | grep boot_info` 检查 `__boot_info_ptr` 所在的段——应该显示 `.data` 而不是 `.bss`。地址应该和其他 `.bss` 符号不重叠。

### Step 3: 编写 crt_stub.cpp——C++ freestanding 运行时支持

**目标**：在 `kernel/mini/arch/x86_64/crt_stub.cpp` 中提供 C++ 编译器期望的所有运行时函数。

**设计思路**：freestanding 环境下，以下函数必须由我们手动提供。`__cxa_pure_virtual` 在有人调用了纯虚函数时被调用——这是编程错误，理论上不应该发生，但链接器需要它的符号存在。实现就是一个 `cli; hlt` 的死循环，用 `[[noreturn]]` 标记告诉编译器这个函数永不返回。`__stack_chk_fail` 在栈保护机制检测到 canary 被篡改时调用——同样的死循环处理。`__cxa_atexit` 用于注册进程退出时的回调函数——内核不"退出"，所以空实现返回 0 即可。

`_init_global_ctors` 是本章的核心运行时函数。它需要声明两个外部符号 `__init_array_start` 和 `__init_array_end`（在链接器脚本中定义的函数指针数组边界），然后用一个循环从 start 走到 end，每次取一个函数指针并调用。注意这些外部符号的类型是"函数指针数组"——声明方式是用 `extern` 加上函数指针类型。循环中的每次调用都可能触发某个全局对象的构造函数，所以如果有多个全局对象，它们会按链接顺序依次构造。

`operator new` 和 `operator delete` 也需要提供——暂时用死循环 stub 实现，因为我们还没有堆分配器。注意这些运算符函数必须用 C++ 链接（不能放在 `extern "C"` 块里），因为 C++ 的 name mangling 是 new/delete 重载所必需的。但上面那几个 `__cxa_*` 函数必须用 `extern "C"` 包裹，因为它们是被编译器按 C 链接约定调用的。

**实现约束**：所有 `__cxa_*` 和 `__stack_chk_fail` 函数需要 `extern "C"` 包裹。`operator new/delete` 不需要。`_init_global_ctors` 需要 `extern "C"`，因为它在汇编中被 `call` 调用。

**验证**：编译内核不报链接错误即可。如果链接器报 "undefined reference to `__cxa_pure_virtual`" 之类的错误，说明你漏了某个 stub 函数。

### Step 4: 设计链接器脚本——高半核 VMA、AT() LMA、.init_array

**目标**：编写 `kernel/mini/linker.ld`，定义内核的内存布局：代码段的虚拟地址（VMA）从高半核起始，加载地址（LMA）指向物理内存 0x20000，`.data` 在 `.bss` 之前，`.init_array` 用 `KEEP()` 保护。

**设计思路**：链接器脚本的核心是区分 VMA 和 LMA。VMA（Virtual Memory Address）是代码运行时"看到"的地址——对于高半核内核，这个地址从 0xFFFFFFFF80000000 + 0x20000 = 0xFFFFFFFF80020000 开始。LMA（Load Memory Address）是 Bootloader 把二进制数据实际放到物理内存的地址——这里是 0x20000。`AT()` 指令就是用来设置 LMA 的：`.text` 段的 AT 写的是 `KERNEL_PHYS_BASE`（0x20000），`.data` 段的 AT 写的是 `KERNEL_PHYS_BASE + SIZEOF(.text)`（紧跟在代码段后面的物理地址）。

段的排列顺序是 `.text`（代码+只读数据）-> `.data`（可读写数据+`.init_array`）-> `.bss`（零初始化数据）。`.data` 必须在 `.bss` 前面——这不是可选项，而是因为 `__boot_info_ptr` 等关键变量在 `.data` 段，BSS 清除范围由 `__bss_start` 和 `__bss_end` 界定，如果 `.data` 和 `.bss` 的顺序搞反了或者混在一起，BSS 清除可能会误伤 `.data` 段的变量。

`.init_array` 段用 `KEEP(*(.init_array .init_array.*))` 包裹——`KEEP` 告诉链接器不要因为 section garbage collection 而丢弃这个段。两个边界符号 `__init_array_start` 和 `__init_array_end` 分别标记在 `.init_array` 的前后，供 crt_stub.cpp 中的 `_init_global_ctors` 遍历使用。

`.bss` 段不需要 `AT()`，因为 BSS 在 ELF 文件里不占空间，运行时由 `rep stosb` 清零。但需要定义 `__bss_start` 和 `__bss_end` 两个边界符号。

入口点设为 `_start`，输出格式是 `elf64-x86-64`。`/DISCARD/` 段用来扔掉 `.comment`、`.note`、`.eh_frame` 等不需要的段。

**踩坑预警**：`*(.text .text.*)` 的写法会匹配所有以 `.text` 开头的段——但如果你希望 `_start` 函数始终在输出文件的最前面，应该用 `*(.text.start)` 放在 `*(.text .text.*)` 之前，并且在 boot.S 中把 `_start` 放进 `.text.start` 段。如果不用这种优先排列，链接器可能把别的函数放在文件开头，导致 Bootloader 跳转到的不是 `_start` 而是某个无关函数。

**验证**：编译后用 `objdump -f build/kernel/mini/mini_kernel` 查看 start address，应该是 `0xffffffff80020000`。用 `objdump -h build/kernel/mini/mini_kernel` 查看段的 VMA 和 LMA 是否正确分配。`readelf -S build/kernel/mini/mini_kernel` 也能看到段的详细属性。

### Step 5: 在内核 CMakeLists.txt 中加入 crt_stub.cpp 和正确的编译选项

**目标**：更新 `kernel/mini/CMakeLists.txt`，将 `crt_stub.cpp` 加入编译源文件列表，并确保所有编译和链接选项正确。

**设计思路**：源文件列表应该包含三个文件：`arch/x86_64/boot.S`（内核入口汇编）、`arch/x86_64/crt_stub.cpp`（C++ 运行时）、`main.cpp`（内核主函数）。编译选项的关键几项：`-ffreestanding`（不依赖标准库）、`-fno-exceptions`（禁用异常）、`-fno-rtti`（禁用 RTTI）、`-fno-pie`（不做位置无关可执行文件）、`-mcmodel=large`（大代码模型，允许访问任意 64 位地址——高半核地址远超默认 small 模型的 2GB 限制）、`-mno-red-zone`（禁用 red zone——内核代码不能依赖 red zone，因为中断可能在任何时刻打断执行，而中断处理不会帮你保存 red zone 的内容）。

链接选项需要 `-T linker.ld` 指定链接脚本、`-nostdlib` 不链接标准库、`-no-pie` 不做位置无关。Post-build 步骤用 `objcopy -O binary` 把 ELF 转成纯二进制（`mini_kernel.bin`），因为 Bootloader 不知道怎么解析 ELF 格式——它只是把磁盘上的二进制数据原样读入内存然后跳转。objcopy 的 `-O binary` 选项会把所有可加载段按地址顺序提取出来，去掉所有 ELF 元数据。

include 路径需要包含 `boot/` 目录，因为 `main.cpp` 要引用 `boot_info.h`。

**踩坑预警**：`-mcmodel=large` 是不能省的。如果你用了默认的 `-mcmodel=small`，编译器生成的代码会假设所有符号都在低 2GB 地址空间内，用 32 位相对地址来引用全局变量。但我们的内核 VMA 是 0xFFFFFFFF80000000，远超 2GB——运行时这些地址引用会被截断成错误的值，产生各种匪夷所思的崩溃。这个坑的特点是：编译和链接都不会报错，只有运行时才炸，而且崩溃的位置和原因完全不相关，排查难度极高。

**验证**：编译后确认 `build/kernel/mini/mini_kernel.bin` 文件存在且大小合理。用 `file build/kernel/mini/mini_kernel` 确认它是 ELF 64-bit x86-64 格式。

### Step 6: 编写 mini_kernel_main——验证 C++ 特性和 BootInfo 传递

**目标**：在 `kernel/mini/main.cpp` 中编写内核主函数，通过 debugcon 输出验证以下 C++ 特性正常工作：类的构造/析构、虚函数多态、全局对象构造，以及 BootInfo 指针传递正确。

**设计思路**：验证策略是用一系列精心设计的测试类，每个测试通过 debugcon 输出特定字符来表示通过。所有测试被包裹在 `===CPP` 开始标记和 `===END` 结束标记之间，方便在日志中一眼定位。

首先是类构造函数测试——定义一个简单的类，包含一个 int 成员和一个 char 成员，构造函数在 debugcon 输出 `C` 加上构造参数值。实例化时如果看到 `C1`，说明构造函数正确执行了。

接下来是虚函数测试——定义一个含纯虚函数的基类和一个派生类，派生类实现这些虚函数。通过基类指针调用派生类的虚函数（这是多态的经典用法），如果返回值正确，输出 `2`。如果虚函数表（vtable）有问题，要么调用到了错误的方法，要么直接跳到了 `__cxa_pure_virtual` 进入死循环。

全局对象测试是最有意义的——定义一个全局对象（在函数外实例化），它的构造函数会把一个全局变量设为 42 并输出 `G`。这个构造函数的调用时机不是在 `mini_kernel_main` 里，而是在 `_init_global_ctors` 遍历 `.init_array` 时。如果在主函数里检查到这个全局变量确实变成了 42，输出 `3`——这就证明了全局构造函数机制完整运作。

最后是 BootInfo 验证——从 `__boot_info_ptr`（现在在 `.data` 段，安全可靠）读出 BootInfo 指针，检查 entry_point 是否为 0xFFFFFFFF80020000、kernel_phys_base 是否为 0x20000。如果都对，输出 `B`。

期望的完整输出序列是 `===CPPGC1V123B===END`。拆解一下：`===CPP` 是开始标记；`G` 是全局对象在 `_init_global_ctors` 中构造时输出的；`C1` 是局部对象的构造函数输出；`V` 是虚函数测试对象构造时的输出；`1` 表示类测试通过，`2` 表示虚函数测试通过，`3` 表示全局对象测试通过，`B` 表示 BootInfo 验证通过；`===END` 是结束标记。

**踩坑预警**：`mini_kernel_main` 需要用 `extern "C"` 和 `[[noreturn]]` 标记——前者防止 C++ name mangling（汇编中用 `call mini_kernel_main` 调用需要精确匹配符号名），后者告诉编译器这个函数不会返回，避免编译器生成不必要的返回代码。函数的第一个参数是 `uint64_t boot_info_addr`（由 `%rdi` 传入），但我们在函数内部是通过 `__boot_info_ptr` 全局变量来获取 BootInfo 的——这两种方式获取到的应该是同一个值（0x7000）。如果你用参数而不是全局变量，也是可以的，但要注意参数只在入口时有效，如果后续有别的函数也要访问 BootInfo，还是全局变量更方便。

**验证**：这是整个 tag 004C 的终极验证。完整构建运行后，debugcon 日志应该包含完整的 `OPLJ123G4===CPPGC1V123B===END`。让我们逐段解读：`OPL` 来自 Bootloader（MBR/保护模式/Long Mode），`J` 来自 Bootloader 跳转前，`1234` 来自内核入口序列（到达/栈就绪/BSS 清除/构造函数），`===CPPGC1V123B===END` 来自 C++ 测试。如果这个完整序列出现了，恭喜你——从按下 QEMU 电源按钮到 C++ 内核成功运行，整条启动链路已经全部打通了。

## 构建与运行

在项目根目录执行完整构建并运行：

构建命令和前面章节一致：CMake 配置 -> 编译 -> `make run`。

QEMU 启动后，debugcon 日志的预期输出：

```
OPLJ123G4===CPPGC1V123B===END
```

逐字符解读——`OPL` 是 Bootloader 的启动标记（003 章就开始输出的那几个），`J` 表示 Bootloader 正在跳转到内核，`1` 表示内核入口 `_start` 到达，`2` 表示栈设置完成，`3` 表示 BSS 清除完成，`4` 表示全局构造函数调用完成，`G` 是全局对象构造时的输出，`===CPP` 到 `===END` 是 C++ 特性测试的完整标记。

如果一切正常，QEMU 在输出 `END` 后会进入 `cli; hlt` 的停机状态。此时 VGA 屏幕上应该能看到 VESA 模式的图形界面（虽然我们的内核还没有往帧缓冲写任何东西，所以可能就是黑屏），debugcon 日志到此为止。

## 调试技巧

### Bug：`__boot_info_ptr` 的值变成了 0x2a00000000

这是我们踩过的一个经典 bug。症状是 BootInfo 验证失败——内核读到的 `__boot_info_ptr` 不是 0x7000 而是一串看起来像垃圾的数据 `0x2a00000000`。

排查方法是这样的：首先确认 `%rdi` 在入口时是正确的。在 `_start` 入口处立即把 `%rdi` 的低 16 位通过 hex-to-ASCII 转换输出到 debugcon（比如输出 `R7000` 表示 `%rdi = 0x7000`）。如果入口时正确但后面不对，那就追踪哪里被改了。

下一步检查 `__boot_info_ptr` 里的值。在 BSS 清除之后、调用全局构造函数之前输出 `__boot_info_ptr` 的值——如果它是 0x0，说明 `__boot_info_ptr` 在 `.bss` 段被 `rep stosb` 清掉了。如果它是一个奇怪的值比如 `0x2a00000000`，那说明有别的代码在之后覆盖了它。

这个 `0x2a00000000 = 42 << 32` 的值给了我们一个巨大的线索——42 是 `global_construction_count` 的测试值。用 `objdump -t build/kernel/mini/mini_kernel | grep boot_info` 检查符号表，发现 `__boot_info_ptr` 和 `global_counter` 被分配到了同一个地址 `0xffffffff800226e8`！根因是链接器在处理汇编定义的 `.bss` 符号时，把它和一个 C++ 全局对象放在了同一位置——因为汇编的 `.skip 8` 后跟标签的写法，让链接器误以为这个符号大小为 0。

修复方法是把 `__boot_info_ptr` 移到 `.data` 段。这同时解决了两个问题：BSS 清除不再影响它，而且 `.data` 段和 `.bss` 段的符号完全隔离，不会再出现地址冲突。

### 使用 objdump 检查符号表

当你怀疑某个变量的值不对时，检查符号表是最直接的诊断手段：

```bash
objdump -t build/kernel/mini/mini_kernel | sort -k 1
```

按地址排序输出所有符号。查看你关心的变量是否和其他变量共享了同一个地址。特别关注 `.bss` 段的符号——这个段的分配机制最容易出问题。

如果只想看特定段：

```bash
objdump -t build/kernel/mini/mini_kernel | grep "\.bss"
objdump -t build/kernel/mini/mini_kernel | grep "\.data"
```

### 用 GDB 单步跟踪入口序列

内核入口是整个启动链路中最脆弱的一段——任何一步出错都会导致 Triple Fault。用 GDB 可以在每一步之后检查寄存器和内存状态：

```
(gdb) break _start
(gdb) continue
(gdb) info registers rdi rsp
(gdb) si
(gdb) si
...
```

在 `_start` 入口处 `%rdi` 应该是 0x7000。在 `rep stosb` 执行之前，`__boot_info_ptr` 应该已经被写入 0x7000。在 `rep stosb` 执行之后，`__boot_info_ptr` 应该仍然是 0x7000（因为它在 `.data` 段）。在 `_init_global_ctors` 返回之后，`__boot_info_ptr` 仍然应该是 0x7000。如果任何一步出了问题，GDB 能帮你精确定位。

## 本章小结

| 概念 | 要点 |
|------|------|
| SSE 启用 | CR4 置位 OSFXSR(bit 9) + OSXMMEXCPT(bit 10)，Long Mode 下 SSE 非默认开启 |
| 栈设置 | `%rsp` = `__mini_stack_top`（.bss 段顶端），`%rbp` = 0 |
| BSS 清除 | `rep stosb`：`%rdi`=start, `%rcx`=count, `%rax`=0，清除后 `%rdi` 被破坏 |
| `__boot_info_ptr` | 必须在 `.data` 段（`.quad 0` 显式初始化），不能在 `.bss` 段 |
| .bss 符号冲突 | 汇编 `.skip` + 标签写法让链接器误判符号大小，可能与 C++ 全局变量共享地址 |
| `.init_array` | 全局构造函数指针数组，用 `KEEP()` 防止被链接器丢弃 |
| `_init_global_ctors` | 遍历 `__init_array_start` 到 `__init_array_end`，调用每个函数指针 |
| crt_stub 运行时函数 | `__cxa_pure_virtual`、`__stack_chk_fail`、`__cxa_atexit`、`new/delete` |
| 链接器脚本 | `.data` 在 `.bss` 前，`.text` 的 AT() 指向物理地址，入口 `_start` |
| 编译选项 | `-ffreestanding -fno-exceptions -fno-rtti -mcmodel=large -mno-red-zone` |
| 最终验证输出 | `OPLJ123G4===CPPGC1V123B===END` |
