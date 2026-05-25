---
title: 004-boot-load-mini-kernel-b-1 · 内核加载 (B)
---

# Bootloader 开发日记 004B（上）：BootInfo 的诞生——bootloader 和内核的握手契约

> 标签：x86, 保护模式, bootloader, BootInfo, flat binary, 内存布局
> 前置：[004A E820 + INT 13h 基础](004-boot-load-mini-kernel-a-1.md)

## 写在前面

到 tag 004A 为止，我们的 bootloader 已经能在 Real Mode 下用 E820 探测物理内存布局，用 INT 13h 从磁盘读取小内核的 ELF header（4KB，8 个扇区）。但说实话，一个只有 ELF header、连实际代码都没加载的系统，离"把内核跑起来"这个目标还有一大段路。004A 留下的状态是：物理地址 0x10000 处放着 4KB 的 ELF header，Stage2 不知道内核到底有多大、该读多少扇区、该加载到哪里。

这一章我们要把这件事彻底做完。具体来说就是三件事：第一，定义一个叫 BootInfo 的结构体，作为 bootloader 和内核之间的"交接契约"——bootloader 把 E820 内存映射、VESA 帧缓冲区信息、内核的入口地址和物理基址全部打包进去，内核启动时拿这个结构体就知道世界长什么样。第二，把磁盘读取从 8 扇区扩展到 832 扇区（416KB 完整小内核），而且因为 BIOS INT 13h 每次最多只能读 127 个扇区，所以必须用循环分块读取，每次动态计算 buffer 地址和 LBA 偏移。第三，把小内核的格式从 ELF 切换为 flat binary，把加载地址从 0x10000 改到 0x20000（避开 Real Mode 栈），并给小内核加上了真正的入口代码。

在这三件事之外，还有一个让我血压拉满的踩坑：栈冲突。小内核最初设计加载到物理地址 0x10000，但 Real Mode 的栈恰好占据 0x9000~0x19000，磁盘写入会直接覆盖栈上保存的返回地址，然后 `ret` 指令跳飞到不知名的地方。解决方案是把加载地址从 0x10000 提高到 0x20000，留出 28KB 的安全间隙。这个改动牵一发而动全身：boot.S 的常量要改、链接脚本的物理地址要改、build_image.sh 的大小上限要改，全章所有地址计算都围绕 0x20000 这个锚点展开。

还有一件事值得提前说：在这一章里，保护模式（Protected Mode）什么都不做。这不是偷懒，而是架构层面的刻意选择。所有数据搬运（E820 探测 + 832 扇区磁盘读取）全部在 Real Mode 下完成，因为只有 Real Mode 才能用 BIOS 中断。保护模式只是一个必经的过渡状态——设置段寄存器、建页表、切 Long Mode——不做任何跟内核加载相关的工作。

## 环境说明

和 004A 保持一致：x86_64 架构，QEMU 虚拟机，GNU AS 汇编器（AT&T 语法），GCC/G++ 编译器，CMake 构建系统。bootloader 代码以 16 位 Real Mode 汇编为主，小内核以 64 位 Long Mode C++ 编写。调试手段是 QEMU 的 debugcon 端口（0xE9），配合 `-debugcon file:debug.log` 参数把字符输出到日志文件。本章结束后，如果一切正常，你会在 debugcon 日志里看到 `OPL` 三个字符——'O' 代表磁盘读取成功（Real Mode），'P' 代表进入保护模式，'L' 代表进入 Long Mode。

工具链的特殊约束依然存在：bootloader 以 `-m32` 编译（实际上是 16 位实模式代码嵌入在 32 位目标文件里），内核以 `-m64` 编译。两者共享的 `boot/boot_info.h` 头文件必须确保在两种编译环境下结构体布局完全一致——这一点后面讲到 BootInfo 的时候会展开。

## 为什么需要 BootInfo

在 004A 之前，bootloader 和内核之间没有任何正式的数据交换机制。bootloader 做完自己的事情（E820 探测、VESA 初始化、磁盘读取），然后内核从某个固定地址开始执行，自己跑去 0x5000 读 E820 数据、去 0x6400 读帧缓冲区信息。这种"隐式约定"的做法在小项目里能凑合，但它有三个根本性的问题。

第一，地址硬编码意味着每次改动都可能牵扯多个文件。如果你把 E820 缓冲区从 0x5000 挪到 0x4000，你得同时改 boot.S 的常量定义、内核的地址引用、可能还有 linker script 里的某些符号——任何一处漏改都是一个微妙的 bug。第二，没有版本信息。如果未来 BootInfo 的字段顺序变了或者新增了字段，内核没法知道它读到的是新版本还是旧版本的结构体，只能祈祷两边恰好同步。第三，没有集中的文档入口。所有地址约定散落在各个文件的注释里，新人看代码只能靠 grep 搜索 0x5000、0x6400 这些 magic number 来拼凑全貌。

BootInfo 结构体把所有这些信息集中到一个地方：bootloader 负责填充它，内核负责读取它，地址只有一个——0x7000。这个设计跟 Linux 的 `struct boot_params`、Multiboot 规范的 boot information 结构体在理念上完全一致：用一块固定地址的结构体作为 bootloader 和内核之间的正式契约。

## MemoryMapEntry：24 字节的 E820 条目

我们先看 BootInfo 里用到的第一个子结构体，`MemoryMapEntry`：

```c
typedef struct {
    uint64_t base;    // Physical base address of the region
    uint64_t length;  // Region length in bytes
    uint32_t type;    // Memory type (1=usable, 2=reserved, etc.)
    uint32_t acpi;    // ACPI extended attributes (usually 0)
} __attribute__((packed)) MemoryMapEntry;

#if defined(__cplusplus)
static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#else
_Static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#endif
```

这个结构体直接对应 E820 BIOS 调用返回的 24 字节内存映射条目格式。base 是物理基地址（64 位，因为物理地址可以超过 4GB），length 是区域长度，type 标识内存类型——1 是可用 RAM，2 是保留区，3 是 ACPI 可回收，4 是 ACPI NVS（Non-Volatile Storage），5 是坏内存。acpi 字段是 ACPI 3.0 引入的扩展属性，老 BIOS 返回 0，新 BIOS 可能在这里标明条目是否支持 NUMA 亲和性之类的信息。Intel SDM Vol.3A Section 15.3.1 对这些字段有完整的定义。

这里有一个非常容易忽略的关键细节：这个头文件同时被两套编译环境使用。bootloader 的代码以 `-m32` 编译，内核代码以 `-m64` 编译。两套编译器的 ABI 不同——32 位模式下 `uint64_t` 可能需要 8 字节对齐，而 packed 结构体里不做对齐；64 位模式下指针大小是 8 字节，结构体的默认对齐规则也完全不同。如果不用 `__attribute__((packed))`，两边编译出来的结构体布局很可能不一致，bootloader 填充的数据内核读取时字段偏移全错，然后你花一整天调试一个看起来完全不合理的数据损坏。实际测试中我发现，如果不加 packed，64 位 GCC 会在 `acpi` 字段后面插入 4 字节填充来对齐整个结构体到 8 字节边界——结构体从 24 字节变成 28 字节，E820 数据的读取全部错位。这个 bug 的诡异之处在于：你单独编译 bootloader 或内核都不会报错，只有合在一起运行时才会出问题。

`static_assert` 是我们的安全网。如果因为某种原因——编译器版本差异、ABI 变化、头文件包含顺序导致 `uint64_t` 定义不同——导致结构体大小不是 24 字节，编译直接报错。注意这里用了条件编译：C++11 用 `static_assert`，C11 用 `_Static_assert`。这是因为 bootloader 里如果将来引入 C 代码（比如 ELF loader，虽然我们现在用了 flat binary 方案不需要了），也得能编译通过。

## BootInfo：三大类启动信息的容器

接下来是主角——`BootInfo` 结构体本身：

```c
typedef struct {
    // Kernel information
    uint64_t entry_point;       // Virtual entry point address
    uint64_t kernel_phys_base;  // Physical base address where kernel was loaded
    uint64_t kernel_size;       // Actual kernel size in bytes

    // Framebuffer information (from VESA BIOS calls)
    uint64_t fb_addr;    // Physical framebuffer base address
    uint32_t fb_width;   // Framebuffer width in pixels
    uint32_t fb_height;  // Framebuffer height in pixels
    uint32_t fb_pitch;   // Bytes per scan line
    uint32_t fb_bpp;     // Bits per pixel (usually 32)

    // Memory map (from E820 BIOS call)
    uint32_t       mmap_count;  // Number of valid entries
    uint32_t       _pad;        // Explicit padding for alignment
    MemoryMapEntry mmap[32];    // Memory map entries (max 32)
} __attribute__((packed)) BootInfo;
```

结构体分成三个逻辑区块，我们一个一个说。

前三个字段是内核信息。`entry_point` 是高半核虚拟入口地址——我们的内核运行在虚拟地址 0xFFFFFFFF80000000 以上的空间，所以入口点不是物理地址 0x20000，而是 `0xFFFFFFFF80020000`。`kernel_phys_base` 是物理加载基址，这一章的值是 0x20000。`kernel_size` 是内核的实际大小（字节）。这三个字段让内核在启动时就能知道"我从哪里来、我有多大、我的入口在哪里"——虽然入口点在当前阶段还是硬编码的，但把这些信息放在 BootInfo 里意味着将来做 ELF 加载或者地址随机化时，只需要改 bootloader 的填充逻辑，内核侧不用动。

中间四个字段是帧缓冲区信息，来自 VESA BIOS 调用保存在 0x6400 的数据。VESA 初始化在 tag 002 就完成了，它把帧缓冲区的物理地址、分辨率（宽 x 高）、扫描线字节数（pitch）和色深（bpp）保存在一块固定内存里。BootInfo 把这些信息"搬一次家"——从 0x6400 的临时位置搬到 BootInfo 结构体的 fb_ 字段里。这样做的好处是内核只需要知道 BootInfo 的地址，不用再去记住 VESA 缓冲区在 0x6400 这个隐式约定。

最后是 E820 内存映射数组。`mmap_count` 是有效条目数，`mmap[32]` 是条目数组，最多 32 条——这对 QEMU 的 5~7 条和大多数真实硬件的 10~20 条来说绑绰有余。`_pad` 字段乍一看有点多余，既然已经加了 `__attribute__((packed))` 禁止填充了，为什么还要手动写一个 padding？原因有两个：第一，让代码的意图一目了然——读者看到 `_pad` 就知道这里有一个对齐间隙，不用去数字节。第二，如果将来某天我们去掉了 packed 属性（比如为了性能优化），这个显式 padding 确保结构体布局不会因为缺少它而改变。

`static_assert(sizeof(BootInfo) == 824, ...)` 确保总大小是 824 字节——4 个 uint64_t（32 字节）+ 6 个 uint32_t（24 字节）+ 32 个 MemoryMapEntry（768 字节）。我们可以用一张表来验证每个字段的偏移和大小：

| 偏移 | 字段名 | 类型 | 大小 | 说明 |
|------|--------|------|------|------|
| 0x00 | entry_point | uint64_t | 8 | 高半核虚拟入口 `0xFFFFFFFF80020000` |
| 0x08 | kernel_phys_base | uint64_t | 8 | 物理加载基址 `0x20000` |
| 0x10 | kernel_size | uint64_t | 8 | 内核实际大小 |
| 0x18 | fb_addr | uint64_t | 8 | 帧缓冲区物理地址 |
| 0x20 | fb_width | uint32_t | 4 | 宽度（像素） |
| 0x24 | fb_height | uint32_t | 4 | 高度（像素） |
| 0x28 | fb_pitch | uint32_t | 4 | 扫描线字节数 |
| 0x2C | fb_bpp | uint32_t | 4 | 色深（通常 32） |
| 0x30 | mmap_count | uint32_t | 4 | E820 有效条目数 |
| 0x34 | _pad | uint32_t | 4 | 对齐填充 |
| 0x38 | mmap[32] | MemoryMapEntry[32] | 768 | E820 条目数组 |

总计 = 0x38 + 768 = 56 + 768 = 824 字节。这个表在调试时非常有用——你可以用 GDB 的 `x/8xg 0x7000` 直接查看 BootInfo 的前几个字段，按偏移对照这张表就能知道每个值对不对。

BootInfo 结构体在内存中的位置也有讲究。我们选了物理地址 0x7000，这个位置不是随便定的。低 1MB 的内存里已经塞了不少东西：0x5000 是 E820 缓冲区、0x6000~0x6400 是 VESA 信息、0x7B00 是 DAP、0x8000 是 Stage2 代码。0x7000 到 0x7B00 之间有 0xB00 = 2816 字节的空闲空间，放一个 824 字节的 BootInfo 绑绰有余。而且 0x7000 是页对齐的（4KB 边界），方便将来做页表映射。

有一点值得一提：BootInfo 结构体虽然在本章定义，但实际的填充工作是在 tag 004C 完成的——004C 的代码在 Long Mode 入口处把 E820 数据从 0x5000、VESA 数据从 0x6400 复制到 BootInfo 的对应字段中。004B 只定义结构、约定地址，不做填充。这个分阶段的设计是有意的：定义和填充分开，让每个 tag 的职责更清晰——004B 定义数据格式和加载策略，004C 执行数据搬运和跳转。

另外要注意，BootInfo 头文件里的注释引用了 `boot/elf_loader.c` 作为 bootloader 侧的使用者，但实际上这个文件在 flat binary 方案中并不存在——它是原始 ELF 加载方案遗留的注释。不过这个注释并不影响代码正确性，只是说明了一个历史设计决策：我们曾经计划在 Protected Mode 下用 C 写 ELF loader，后来改用 flat binary 方案后这个文件就不需要了。

## 和 Linux boot_params、Multiboot 的设计对比

BootInfo 这个概念不是我们发明的，几乎所有操作系统都有自己的 bootloader-kernel handoff 机制。我们来深入看看几个知名项目是怎么做的，以及 Cinux 为什么做了不同的选择。

**Linux 的 `struct boot_params`** 是最经典的例子。Linux x86 Boot Protocol 定义了一个庞大的结构体（`struct boot_params`，大约 4096 字节），放在物理地址 0x7000 附近。bootloader（GRUB、systemd-boot 等）负责填充这个结构体，然后通过 `int 0x10/0x13/0x15` 收集硬件信息，最后跳转到内核入口。boot_params 里的字段非常全面：E820 内存映射、VESA 帧缓冲区参数、命令行参数（`cmd_line_ptr`）、initrd 地址和大小、APM/EDD 信息、甚至还有 EFI 系统表指针。Linux 的设计哲学是"bootloader 尽可能多地收集信息，内核只管用"。你可以在 [Linux x86 Boot Protocol](https://www.kernel.org/doc/html/latest/arch/x86/boot.html) 文档里看到完整的字段列表和版本协商机制。

Cinux 的 BootInfo 和 Linux 的 boot_params 在设计理念上一致——固定地址 + 结构体 + 寄存器传指针。但有三个关键区别。第一，Linux 有版本协商：boot_params 的 `hdr.version` 字段让内核知道 bootloader 是哪个版本的协议，字段是否存在取决于版本号。Cinux 目前没有版本信息，因为我们只有一个 bootloader 实现，不存在多版本兼容的需求。第二，Linux 的结构体是按缓存行对齐的（64 字节），有些字段组有专门的偏移约定。Cinux 用 packed 结构体，没有对齐优化——824 字节的数据一次性搬完，对齐对性能的影响在这个阶段可以忽略。第三，Linux 的 cmdline 参数是通过指针引用的（bootloader 把命令行字符串放在内存某个位置，boot_params 里只存指针），而 Cinux 目前没有命令行参数的需求——教学内核不需要 `root=/dev/sda1` 这种东西。

**Multiboot 规范（GRUB）** 走的是另一条路。Multiboot 1 使用 EBX 传递 boot info 指针、EAX 传递魔数 0x2BADB002，boot info 结构体是一个固定头部 + 变长 flag 数组。Multiboot 2 更进一步，使用 tag 链表结构——每个 tag 有 type 和 size 字段，后面跟具体数据，tag 链表以一个 type=0 的终止 tag 结尾。这种设计的好处是可扩展——新增功能只需要定义新的 tag type，旧的 bootloader 和内核可以忽略不认识的 tag。OSDev Wiki 的 [Multiboot](https://wiki.osdev.org/Multiboot) 页面对两种版本都有详细描述。

Cinux 为什么没选 Multiboot 式的 tag 链表？原因很简单：我们不需要可扩展性。Cinux 只有一个 bootloader 实现，BootInfo 的字段在可预见的未来都不会大幅变动。tag 链表带来的解析代码（遍历链表、按 type 查找 tag）对教学项目来说是过度设计。固定布局的结构体更简单——内核直接按偏移读字段就行，不用遍历、不用匹配 type。如果将来 Cinux 要支持 GRUB 引导，我们可以实现一个 Multiboot 到 BootInfo 的转换层，在 GRUB 的 boot info 和我们的 BootInfo 之间做翻译。

**SerenityOS** 的做法又不太一样。SerenityOS 使用自己的 bootloader（也支持 GRUB），通过 Multiboot 协议传递信息。它的内核入口接收 Multiboot 的 boot info 指针，然后自己解析 E820 内存映射和帧缓冲区信息。和 Cinux 的区别在于 SerenityOS 没有定义自己的 handoff 结构体——它直接使用 Multiboot 定义的结构。这种方式更标准化（GRUB 可以直接引导），但也意味着对 Multiboot 规范的依赖——如果你想换一个不支持 Multiboot 的 bootloader，内核的入口代码就得改。

你会发现这些项目在 bootloader-kernel 交接这个问题上做了不同的取舍，但核心思路是一样的：bootloader 在 Real Mode 下收集硬件信息，然后通过某种约定好的数据结构传给内核。固定地址结构体（Cinux/Linux）、寄存器+魔数（Multiboot）、tag 链表（Multiboot 2）只是具体的实现形式不同，本质都是解决同一个问题——bootloader 和内核之间怎么可靠地传递信息。

## 内存布局总览

在深入代码之前，我们先搞清楚 004B 之后完整的低 1MB 内存布局。理解这个布局是理解后面所有地址计算的基础：

```
物理地址          内容                       所属 Tag
──────────────────────────────────────────────────────
0x0000~0x0400     IVT (中断向量表)            BIOS
0x1000~0x3FFF     页表 (PML4/PDPT/PD)         003
0x5000            E820 内存映射表              004A
0x6000            VBE Controller Info         002
0x6200            VBE Mode Info               002
0x6400            Framebuffer Info            002
0x7000            BootInfo 结构体              004B (约定位置，004C 填充)
0x7B00            DAP (磁盘读取参数包)         004A
0x9000~0x19000    Real Mode 栈 (SS=0x0900)     001
0x8000            Stage2 代码                  001
──────────────────────────────────────────────────────
0x20000           Mini Kernel (flat binary)   004B <-- 最大 416KB
~0x88000          Mini Kernel 结束             004B
── 32KB gap ──
0x90000           Protected/Long Mode 栈       002
```

注意几个关键设计点：E820 数据在 0x5000，这是低内存中一块相对"安静"的区域；BootInfo 在 0x7000，刚好在 VESA 信息和 DAP 之间有足够空间；小内核从 0x20000 开始加载，避开了 Real Mode 栈（0x9000~0x19000），留了 28KB 安全间隙。所有这些地址都不是随意选择的——每一个都经过计算，确保不会和其他数据区域重叠。

## 从 ELF 到 flat binary 的策略转变

在讲分块读取之前，我们需要先说清楚一个重要的设计变更：004B 把小内核的格式从 ELF 切换成了 flat binary（扁平二进制）。在 004A 里，我们读取的是 ELF header，计划在后续阶段解析 Program Header、遍历 PT_LOAD 段、做重定位和 BSS 清零。但仔细想想，Real Mode 的 16 位环境做不了 64 位算术——ELF64 里的地址字段都是 64 位的，你要在 16 位实模式下用多个寄存器拼接才能读写一个地址。虽然可以在 Protected Mode（32 位）下用 C 写一个 ELF loader，但那意味着 Stage2 里要链接一个 32 位 C 编译的目标文件，构建系统的复杂度一下子就上去了。

flat binary 的思路简单粗暴：用 `objcopy -O binary` 把编译好的 ELF 文件剥掉所有 header、section table、符号表，只保留段的原始二进制内容。输出的第一个字节就是 `.text` 段的第一个字节，也就是 `_start` 的第一条指令。bootloader 只需要把这个二进制文件原封不动地读到目标物理地址，然后在 Long Mode 里跳过去就行了——不需要解析任何格式，不需要做重定位，加载地址和链接地址必须完全一致。

代价是明显的：内核必须链接在固定的物理地址（0x20000），不能做 ASLR，也不能利用 ELF 的段对齐特性。但对于一个教学内核来说，这些限制完全可以接受。而且这个选择只影响 bootloader 到 mini kernel 的加载——将来 mini kernel 加载 big kernel（tag 008）的时候，mini kernel 已经有完整的 C++ 运行环境和内存管理，用 ELF 格式加载 big kernel 就是正确的选择了。bootloader 到 mini kernel 用 flat binary 是"简单"，mini kernel 到 big kernel 用 ELF 是"正确"，两者各取所长。

### flat binary 的生成流程

整个编译流程是这样的：源代码先由 GCC/G++ 编译成目标文件（.o），然后链接器根据 linker script 把所有目标文件合并成一个 ELF 可执行文件。这一步链接器需要 ELF 格式来工作——它需要解析符号、处理重定位、计算地址。所以 ELF 是中间产物，不是最终产物。最终产物由 `objcopy -O binary` 生成：这个工具剥离所有 ELF header、section header table、符号表、重定位信息，只保留 LOAD 段的原始二进制内容。输出文件的第一个字节就是 `.text.start` 段的第一个字节——`_start` 的第一条指令 `cli`（0xFA）。这个 flat binary 被 build_image.sh 写到磁盘镜像的 LBA 16 位置，然后 boot loader 的 `load_kernel_from_disk` 把它原封不动地读到物理 0x20000。

值得注意的是 `objcopy -O binary` 有一个容易踩的坑：如果你的 linker script 没有把 `.text.start` 放在最前面，flat binary 的第一个字节可能不是 `_start`，而是一段数据或者其他 section 的内容。所以链接脚本里必须有 `*(.text.start)` 放在最前面的约定，确保入口点的代码在 flat binary 的起始位置。

### 为什么不用 multiboot 让 GRUB 帮我们加载

你可能还会问：为什么不直接用 GRUB？GRUB 能解析 ELF、设置 VESA 模式、提供 multiboot info——为什么要自己写 bootloader 干这些脏活？答案有两层。第一，教学目的。写 bootloader 的过程本身就是在学习 x86 的启动机制——Real Mode/Protected Mode/Long Mode 的切换、段地址计算、BIOS 调用约定、页表映射。如果你用 GRUB，这些知识全部被一个黑盒屏蔽了。第二，控制力。GRUB 的 multiboot 协议有它的局限——它只提供 E820 内存映射和基本的帧缓冲区信息，不支持我们自定义的 BootInfo 格式。如果我们想让 BootInfo 包含更多字段（比如 ACPI 表地址、SMP 信息），要么扩展 multiboot 协议（需要修改 GRUB），要么在内核里做二次转换。自己写 bootloader 就没有这个限制——BootInfo 的字段完全由我们定义。

当然 GRUB 也有明显的优势：它支持多种文件系统、多种磁盘控制器、网络启动、命令行界面。如果 Cinux 的目标是一个生产级操作系统，用 GRUB 显然更合理。但作为一个教学项目，自己写 bootloader 带来的学习价值远大于省下的开发时间。

而且自己写 bootloader 有一个意外的好处：你可以完全控制启动流程的每一个细节。当你在 QEMU 里用 GDB 单步跟踪从 MBR 到 Stage2 到 Long Mode 到内核入口的整个过程时，你清楚地知道每一条指令在做什么、为什么这么做。这种理解深度是用 GRUB 一键启动完全无法获得的。在调试内核早期启动问题的时候，这种理解尤其重要——你不会因为"GRUB 内部做了什么操作"而困惑，因为 bootloader 的每一行代码都是你自己写的。

## 本章小结

在这一篇里，我们定义了 BootInfo 结构体作为 bootloader 和内核的正式交接契约——它把 E820 内存映射、VESA 帧缓冲区信息、内核入口和物理基址打包成一个 824 字节的 packed 结构体，放在物理 0x7000。我们对比了 Linux 的 boot_params、Multiboot 的 tag 链表、SerenityOS 的做法，解释了 Cinux 选择固定布局 packed 结构体的理由。同时我们说明了为什么选择 flat binary 而不是 ELF 作为 bootloader 到 mini kernel 的格式——Real Mode 下做 64 位 ELF 解析是不现实的，flat binary 把复杂度压到了最低。

下一篇我们将进入 004B 最硬核的部分：从 4KB 到 416KB 的分块磁盘读取，那个让我血压拉满的栈冲突 bug，以及保护模式为什么"什么都不做"。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 9.1 — Processor Management and Initialization。定义了 Real Mode / Protected Mode / Long Mode 的状态转换流程。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 15.3.1 — INT 15h, EAX=E820h: Query System Address Map。定义了 E820 返回的内存映射条目格式。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- Linux x86 Boot Protocol: Linux 内核的 bootloader-kernel 交接协议。`struct boot_params` 的设计理念和 Cinux BootInfo 相同。
  https://www.kernel.org/doc/html/latest/arch/x86/boot.html

- OSDev Wiki — Multiboot: Multiboot/Multiboot2 规范定义的 bootloader-kernel 交接设计。
  https://wiki.osdev.org/Multiboot

- OSDev Wiki — Rolling Your Own Bootloader: 自写 bootloader 的综合指南，flat binary vs ELF 的取舍讨论。
  https://wiki.osdev.org/Rolling_Your_Own_Bootloader

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O 读取磁盘的参考实现。与 Cinux 的 Real Mode + BIOS INT 13h 方案形成对比。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c
