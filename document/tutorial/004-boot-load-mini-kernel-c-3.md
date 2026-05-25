---
title: 004-boot-load-mini-kernel-c-3 · 内核加载 (C)
---

# 内核的第一次呼吸（下）：构建系统深度剖析与跨 OS 对比

> 标签：build system, CMake, mcmodel, linker script, objcopy, debugcon, OS comparison
> 前置：[004C-2 内核的第一次呼吸](004-boot-load-mini-kernel-c-2.md)

## 写在前面

前两篇我们把 004C tag 的核心代码都讲完了——从 BootInfo 填充和高半核跳转，到 boot.S 的入口序列、BSS 清除陷阱、符号冲突侦探故事，再到 crt_stub.cpp 的 C++ 运行时支撑和 mini_kernel_main 的特性验证。这一篇换一个角度，从构建系统和验证的角度重新审视这些代码，同时横向对比 Linux、xv6 和 SerenityOS 在相同问题上的不同选择。

为什么要专门花一篇来讲构建系统？因为在内核开发中，编译选项和链接脚本不是"配置文件"那么简单——它们是代码的一部分。一个 `-mcmodel` 选错了，代码编译和链接都不报错，运行时直接暴毙。一个链接脚本段的顺序搞反了，BSS 清除会误伤 `.data` 段的变量。这些问题的根因都不在逻辑层面，而在构建配置层面，排查起来特别耗时间。

## 环境说明

构建工具链版本：GCC 13+（需要支持 C++23 特性，虽然当前 tag 只用了 C++17 的基础特性）、CMake 3.20+、GNU Binutils（提供 objcopy 和 ld）。目标三元组是 x86_64-elf（freestanding，不依赖任何系统头文件和库）。

## CMakeLists.txt：每一个选项都是血的教训

Mini Kernel 的构建系统定义在 `kernel/mini/CMakeLists.txt` 中。这个文件看起来很简单——三个源文件、几个编译选项、一个链接脚本、一个 objcopy 后处理——但每一个选项背后都有一个"不选就会出事"的故事。

### 源文件列表

```cmake
add_library(mini_kernel_common OBJECT
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    arch/x86_64/gdt.cpp
    arch/x86_64/idt.cpp
    ...
    main.cpp
)
```

源文件用 OBJECT 库组织，这样测试目标（CMakeLists.txt 在 test/ 子目录下）和生产目标可以共享同一份源文件编译结果，避免重复编译。`boot.S` 的 `.S` 后缀会被 CMake 自动识别为需要 C 预处理的汇编文件——和 `.s`（小写）不同，后者不经过预处理。这意味着 `boot.S` 里可以用 `#include` 引入头文件、用 `#define` 定义常量。

### 编译选项逐一解读

```cmake
set(MINI_KERNEL_COMMON_COMPILE_OPTIONS
    -ffreestanding
    -fno-exceptions
    -fno-rtti
    -fno-pie
    -mcmodel=large
    -mno-red-zone
    -Wall
)
```

**`-ffreestanding`** 告诉编译器这是 freestanding 环境。在 hosted 环境下（普通应用程序），编译器可以假定标准库函数存在并生成隐式调用。比如 `memcpy` 可能被编译器内联展开为 `rep movsb`，但如果你在 freestanding 环境下没有提供 `memcpy` 的实现，链接就会失败。加上这个选项后，编译器不再做这种假设。它同时意味着 `main` 函数不再有特殊语义——我们的入口函数叫 `_start` 而不是 `main`，链接器通过 `ENTRY(_start)` 来确定入口点。

**`-fno-exceptions -fno-rtti`** 这两个选项禁用 C++ 的异常和运行时类型信息。异常需要 `.eh_frame` 段和栈展开（stack unwinding）基础设施——内核中我们没有这些。RTTI 需要 `typeid` 的额外数据，增加代码体积。SerenityOS 也使用相同的选项组合——内核中的错误处理用 `ErrorOr<T>` 返回值而不是异常。值得注意的是，`-fno-exceptions` 会让 `throw` 语句变成编译错误，但 `catch` 块在某些编译器版本下可能仍然被接受（只是永远不会执行到）——所以如果你发现某些 C++ 代码在内核中编译报错，首先检查是不是用了异常相关的特性。

**`-mcmodel=large`** 这是高半核内核的硬性要求。x86-64 的默认代码模型是 `small`，它假设所有符号都在低 2GB 地址空间内（0 到 0x7FFFFFFF），编译器用 32 位 RIP-relative 寻址来访问全局变量——指令编码只有 7 字节（REX 前缀 + 操作码 + 32 位位移）。但我们的内核 VMA 是 `0xFFFFFFFF80000000`，远超 2GB 范围。`large` 模型下，编译器对每个全局变量访问都用 `movabsq $64bit_addr, %rax` + 间接寻址，指令编码变成 10 字节。代价是代码体积增大（每条全局变量访问多 3-5 字节），但安全。

如果你忘了加这个选项会怎样？编译和链接都正常通过，没有任何警告。但运行时，所有高半核地址的引用都会被截断成 32 位值，变成完全错误的地址。而且崩溃的位置和根因完全不相关——你可能花了半天才发现问题出在编译选项上。

**`-mno-red-zone`** 禁用 x86-64 的 Red Zone 优化。Red Zone 是 ABI 定义的栈指针以下 128 字节保留区域，用户态函数可以用它存储临时数据而不调整 `%rsp`。但在内核代码中，硬件中断可能在任何指令处打断执行——中断处理程序直接使用当前栈，会覆盖 Red Zone 的内容。如果编译器把某个关键变量存在了 Red Zone 里，中断到来时数据就被破坏了。这个问题的特征是"某些时候正常、某些时候崩溃"——因为中断的时机是随机的，只有在 Red Zone 里的数据恰好被中断覆盖时才会出问题。排查这种 bug 的难度极高，所以一定要从一开始就禁用 Red Zone。

### 链接选项和后处理

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
    VERBATIM
)
```

`-nostdlib` 不链接标准库和启动文件（crt0.o、crti.o 等）。这很重要——如果忘了加这个选项，链接器会尝试链接 glibc 的启动代码，然后因为找不到 `__libc_start_main` 而报错。即使链接成功了，glibc 的初始化代码也会试图调用我们没实现的系统调用，导致内核直接崩溃。`-no-pie` 禁用位置无关可执行文件——内核地址在链接时确定，不需要运行时重定位。

objcopy 后处理把 ELF 转成 flat binary。这一步有几个需要注意的点：ELF 的 section header table、program header table、符号表、重定位信息全部被剥离；输出只包含可加载段（`.text`、`.data`、`.init_array`）的原始二进制内容；`.bss` 段不包含在输出中（它在 ELF 里不占空间）。Bootloader 不知道 ELF 是什么——它只是把磁盘上的二进制数据原封不动读到物理地址 0x20000 然后跳转。

### 链接脚本中的 .text.start 段排列技巧

这里有一个很多教程不会讲到的细节：_start 必须是 flat binary 的第一个字节。如果不做特殊处理，链接器可能把其他函数（比如 __cxa_pure_virtual 或某个类的成员函数）放在输出文件的开头——链接器的段合并顺序并不总是和源文件列表顺序一致。

Cinux 的解决方案是使用 .text.start 子段。在 boot.S 中，_start 被放在 .section .text.start, "ax" 段（注意 "ax" 表示 allocatable + executable）。在链接脚本中，.text 段的第一行就是 *(.text.start)——通配符 *(.text .text.*) 放在后面。这保证了链接器首先排放 .text.start 段的内容，也就是 _start 的代码。这是 flat binary 内核开发中的标准技巧——Linux 使用类似的 __HEAD .section .head.text 来确保 startup_64 在文件最前面。

## debugcon：从调试工具到核心验证基础设施

`cmake/qemu.cmake` 的一个小变更体现了 004C tag 的设计理念转变：debugcon 从"调试工具"升级为"核心验证基础设施"。

之前 debugcon 只在 `QEMU_DEBUG_FLAGS` 中启用（`make run-debug`），正常 `make run` 不启用。这在 003 章没问题——debugcon 输出只是辅助调试。但从 004C 开始，内核的 `boot.S` 和 `main.cpp` 用 debugcon 输出字符序列来验证每一步执行是否正确。这个序列（`OPLJ123G4===CPPGC1V123B===END`）不是"调试信息"，而是"功能验证"——如果某个字符缺失，就说明对应的功能有 bug。

把它移到 `QEMU_COMMON_FLAGS` 后，所有运行模式都能看到输出。QEMU 参数 `-debugcon file:debug.log` 指定输出到文件，`-global isa-debugcon.iobase=0xe9` 显式绑定端口。这种"从调试工具到验证基础设施"的演进在内核开发中很常见——Linux 内核的 `pr_debug`、`printk` 也是类似的发展路径，最初只是为了调试，后来变成了生产环境可观测性的基础。

debugcon 的 I/O 端口 0xE9 是 Bochs 模拟器最早引入的调试端口，后来 QEMU 也支持了。往这个端口写一个字节，QEMU 会把它输出到 debugcon 设备。`outb %al, $0xE9` 这条 2 字节的指令就是所有的硬件开销——比串口 UART 简单得多（UART 需要初始化波特率、检查发送缓冲区是否空闲等），这也是我们选择 debugcon 而不是串口作为早期验证输出的原因。

## 跨 OS 对比：同一问题的不同解法

### 内核入口序列

| 步骤 | Cinux (004C) | Linux (head_64.S) | xv6 (entry.S) |
|------|-------------|-------------------|----------------|
| 关中断 | `cli` | `cli`（decompressor 已关） | 无（依赖 GRUB） |
| SSE 启用 | CR4 设置 OSFXSR+OSXMMEXCPT | 内核启动后由 `fpu__init` 完成 | 不需要（纯 C） |
| 设栈 | `__mini_stack_top`（.bss 末尾） | `initial_stack`（定义在 head_64.S） | `stack`（定义在 entry.S） |
| BSS 清除 | `rep stosb`（自写） | decompressor 清零 + `__bss_start/__bss_end` | GRUB 保证清零 |
| 全局构造函数 | `_init_global_ctors`（遍历 .init_array） | `do_initcalls`（多级 initcall） | 不需要（纯 C） |
| BootInfo 传递 | `%rdi` -> `.data` 段 | `%rsi` -> `%r15` 寄存器 | 不需要 |

### 高半核设计

| OS | 高半核基址 | 页表策略 | 5-level paging |
|----|-----------|---------|---------------|
| Cinux | `0xFFFFFFFF80000000` | 复用 identity mapping 的 PD | 不支持 |
| Linux | `0xFFFFFFFF80000000` | 独立内核页表（init_top_pgt） | 支持（KASLR） |
| SerenityOS | `0xFFFFFFFF80000000` | 独立内核页表 | 不支持 |
| xv6 | 无（identity mapping） | 无 | 不适用 |

### C++ 运行时策略

| 特性 | Cinux | SerenityOS |
|------|-------|------------|
| 异常 | 禁用 (`-fno-exceptions`) | 禁用，使用 `ErrorOr<T>` |
| RTTI | 禁用 (`-fno-rtti`) | 禁用 |
| 全局构造函数 | `.init_array` 直接遍历 | 类似机制 |
| new/delete | halt stub | 真正的 kmalloc/kfree |
| 构建系统 | CMake | CMake + Serenity's MetaData |

## objcopy 深度剖析：从 ELF 到 Flat Binary 发生了什么

理解 objcopy 的工作方式对排查构建问题至关重要。让我们用实际命令来看看转换前后发生了什么变化。

首先是 ELF 文件的段布局：

```bash
readelf -S build/kernel/mini/mini_kernel
```

输出会显示大约 10-15 个段，包括 `.text.start`、`.text`、`.rodata`、`.data`、`.init_array`、`.bss` 等。每个段都有 `Addr`（VMA）、`Offset`（文件内偏移）和 `Size`。注意 `.bss` 段的 `Size` 不为零但 `Offset` 指向文件末尾之后——这意味着它不占磁盘空间。

执行 `objcopy -O binary` 后，所有这些段的原始二进制内容被提取出来，按 LMA（物理地址）排序，拼接成一个连续的文件。section header、program header、符号表、字符串表、重定位表全部丢弃。输出文件的第一字节对应 ELF 中最低 LMA 的段的第一个字节。

有一个细节值得注意：objcopy 在计算 flat binary 的布局时使用的是 LMA（`AT()` 指定的物理地址），而不是 VMA。在我们的链接脚本中，所有段的 LMA 都用 `ADDR(.xxx) - KERNEL_Virt_BASE` 计算，这意味着 `.text` 从 LMA 0x20000 开始，`.data` 从 LMA `0x20000 + SIZEOF(.text)` 开始，`.init_array` 紧随其后。由于 flat binary 的第一个字节对应最低 LMA，而我们的最低 LMA 是 0x20000，objcopy 实际上输出的文件从"相对偏移 0"开始——也就是说，flat binary 的第 0 字节对应物理地址 0x20000，第 N 字节对应物理地址 0x20000 + N。Bootloader 把整个文件加载到物理地址 0x20000 后，每个字节就自动到了正确的物理位置。

如果 objcopy 的输出看起来太大或者太小，可以用 `readelf -l` 检查 ELF 的 program header 中的 `FileSiz` 和 `MemSiz` 列——flat binary 的大小应该等于所有可加载段 FileSiz 的总和（加上段之间的对齐填充）。

## debugcon 输出序列的完整解读

`OPLJ123G4===CPPGC1V123B===END` 这个 30 字符序列是 004C tag 的完整验证标准。让我们完整拆解每个字符的来源和含义。

`O` 来自 `boot/stage2.S`，在 MBR 把 Stage2 加载到 0x8000 并跳转后输出。`P` 来自 `boot/stage2.S` 的 `pm_entry` 标签，保护模式切换完成后输出。`L` 来自 `boot/common/long_mode.S` 的 `long_mode_entry` 标签，Long Mode 进入后输出。`J` 来自 `boot/stage2.S` 的 BootInfo 填充代码末尾，跳转到内核前输出。

`1` 来自 `kernel/mini/arch/x86_64/boot.S` 的 `_start` 入口，SSE 启用后输出。`2` 同样来自 boot.S，栈设置完成后输出。`3` 来自 boot.S，BSS 清除（`rep stosb`）完成后输出。`G` 来自 `kernel/mini/main.cpp` 的 `GlobalCounter` 构造函数，在 `_init_global_ctors` 遍历 `.init_array` 时输出。`4` 来自 boot.S，全局构造函数调用完毕后输出。

`===CPP` 是 `mini_kernel_main` 的开始标记。`C1` 是 `SimpleClass` 构造函数输出。`V` 是 `Derived` 构造函数输出。`2` 是虚函数测试通过。`3` 是全局对象验证通过。`B` 是 BootInfo 验证通过。`===END` 是结束标记。

如果序列在某处中断，定位方法很简单：找到最后一个出现的字符，它的下一步就是出错的位置。

## 验证清单

完整构建后，按以下清单逐项验证：

1. `build/kernel/mini/mini_kernel` 是 ELF 64-bit x86-64（`file` 命令确认）
2. 入口地址是 `0xffffffff80020000`（`readelf -h` 确认）
3. `.init_array` 段存在且不为空（`readelf -S` 确认）
4. `__boot_info_ptr` 在 `.data` 段，不在 `.bss` 段（`objdump -t` 确认）
5. `mini_kernel.bin` 存在且大小合理（几十 KB）
6. `debug.log` 包含完整序列 `OPLJ123G4===CPPGC1V123B===END`

每一步的失败模式都不一样：第 3 步失败说明 `KEEP()` 没写对——链接器的 section garbage collection 把 `.init_array` 砍掉了，全局构造函数永远不会被调用；第 4 步失败说明 `__boot_info_ptr` 的段定义有误——如果它在 `.bss` 段，BSS 清除会把它清零或者和其他符号冲突；第 6 步失败需要根据最后一个出现的字符来定位具体是哪个阶段出了问题。

如果所有检查都通过但 debug.log 里还是没有完整序列，最后检查一下磁盘镜像是否正确生成了——`build_image.sh` 脚本需要把 MBR 写入 LBA 0、Stage2 写入 LBA 1 开始的扇区、Mini Kernel flat binary 写入 LBA 16 开始的扇区。任何一步写错了位置，Bootloader 都会跳到错误的地址。

## 收尾

004C 这个 tag 把从 Bootloader 到 C++ 内核的整条启动链路全部打通了。构建系统虽然看起来只是"配置文件"，但 `-mcmodel=large` 和 `-mno-red-zone` 这种选项漏了任何一个都会导致运行时崩溃，而且编译链接阶段不会有任何报错。debugcon 从调试工具升级为验证基础设施，体现了"内核开发中每一步都要可观测"的原则。

与 Linux、xv6、SerenityOS 的横向对比揭示了一个有趣的规律：所有高半核内核都选了 `0xFFFFFFFF80000000` 这个地址，但页表策略、BSS 清除方式、全局构造函数调用机制各有不同。这些差异不是随机的——它们反映的是项目规模、目标硬件、开发语言选择之间的权衡。Cinux 的"复用 PD + 自清 BSS + .init_array 遍历"组合，是一个教学内核在资源受限和概念简洁性之间的最优解。

当你后面回过头来重构这段代码时——比如把 bootloader 的页表替换为内核自己管理的页表，或者把 halt 版的 new/delete 替换为真正的堆分配器——你会感激当初选择了这种清晰的分层设计。每一层只做一件事，每一层都有独立的验证手段（debugcon 的字符输出），这使得后续的重构和扩展可以在可控的范围内进行。

从 004A 到 004C，我们完成了整个 Bootloader 到内核的启动链路。004A 在 Real Mode 下做 E820 探测和磁盘读取，004B 定义 BootInfo 结构体并把磁盘读取从 4KB 扩展到 416KB，004C 在 Long Mode 下填充 BootInfo、建立高半核映射、跳转到内核、完成 C++ 运行时初始化。每一步都有清晰的 tag 边界和验证标准。接下来的 tag 将会让内核开始做真正有用的事情——建立中断处理、实现物理内存管理、加载真正的内核。到那时，我们今天搭好的 C++ 运行时和 BootInfo 传递机制就会派上真正的用场了。到那时回头看这几十行汇编代码，你会发现它们虽然简单，却承载了整个系统启动过程中最关键的一环。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 4.3 -- 4-Level Paging: 4 级页表结构和 2MB 大页设置。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- System V Application Binary Interface, AMD64 Architecture Processor Supplement: 函数调用约定和 Red Zone 定义。
  https://gitlab.com/x86-psABIs/x86-64-ABI

- OSDev Wiki -- C++ Bare Bones: freestanding 环境下的 C++ 内核设置。
  https://wiki.osdev.org/C%2B%2B_Bare_Bones

- Linux Kernel `arch/x86/kernel/head_64.S`: Linux 的 `startup_64` 入口序列。
  https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S

- SerenityOS Kernel: 另一个使用 C++ 的开源操作系统内核，同样使用 `-ffreestanding -fno-exceptions -fno-rtti`。
  https://github.com/SerenityOS/serenity

- xv6 `entry.S` (MIT): 纯 C 内核的简化入口，对比 C++ 内核的额外复杂度。
  https://github.com/mit-pdos/xv6-public/blob/master/entry.S
