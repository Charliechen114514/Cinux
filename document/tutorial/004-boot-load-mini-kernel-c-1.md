# 从 Bootloader 到内核的交接：BootInfo 握手协议与高半核跳转

> 标签：bootloader, BootInfo, higher-half kernel, 页表映射, System V ABI, 参数传递
> 前置：[004B 从 ELF 解析到完整内核加载](004-boot-load-mini-kernel-b-1.md)

## 写在前面

到 tag 004B 为止，我们的 Stage2 bootloader 已经能在 Real Mode 下把小内核的扁平二进制完整读到物理地址 0x20000，然后一路穿过保护模式、建好页表、开启 Long Mode，最后优雅地 `hlt` 在那里。说实话，能走到这一步已经很不容易了，但一个 halt 住的 bootloader 没有任何意义——它该做的事情是把控制权交接给内核，让内核开始真正工作。这就好比搭了一舞台，灯光音响都就位了，演员就是不上场。

tag 004C 要做的事情就是把这条"最后一公里"打通。具体来说，bootloader 进入 Long Mode 之后需要做三件事：把 BootInfo 结构填好（告诉内核你被加载到哪里、内存长什么样、显存在哪里），在页表里加上高半核映射（让内核运行在高地址而不是低地址），然后跳过去。这三件事每件都不复杂，但组合在一起就涉及到一个完整的握手协议——bootloader 和内核必须就数据格式、地址约定、参数传递方式达成一致，否则内核一启动就会拿到垃圾数据然后当场暴毙。

我们这一篇覆盖 bootloader 侧的全部代码：BootInfo 的 C 语言定义（`boot/boot_info.h`）、高半核页表映射（`boot/common/long_mode.S`）、BootInfo 填充与跳转逻辑（`boot/stage2.S`），以及一个小但关键的构建配置变更（`cmake/qemu.cmake`）。下一篇我们再进入内核侧，看内核拿到 BootInfo 之后怎么完成自己的初始化。

## 环境 & 背景

先快速回顾一下当前的低 1MB 内存布局，因为 BootInfo 放在哪里、数据从哪里来，完全取决于这个布局：

```
物理地址        内容                      来源 Tag
─────────────────────────────────────────────────
0x0000~0x0400   IVT                       BIOS
0x1000~0x3FFF   页表 (PML4/PDPT/PD)       003
0x5000          E820 内存映射表             004A
0x6000          VBE Controller Info        002
0x6200          VBE Mode Info              002
0x6400          Framebuffer Info           002
0x7000          BootInfo 结构              004C <-- 本篇新增
0x7B00          DAP (磁盘读取参数)          004A
0x7C00          MBR                        000
0x8000          Stage2                     001~004
─────────────────────────────────────────────────
0x20000         Mini Kernel (flat binary)  004B
0x90000         Protected/Long Mode 栈     002
```

你会发现从 0x5000 到 0x7C00 之间的低 1MB 区域已经相当拥挤了。每一块数据都有它被放那个位置的理由——E820 需要一个 BIOS 能写的连续缓冲区、VESA 信息需要在保护模式切换前保存、DAP 是 MBR 留下来的结构可以复用。现在我们要在这个夹缝里再塞一个 824 字节的 BootInfo，选位置的时候必须避开所有已占用的区域。

工具链方面没什么变化，还是 GNU AS（AT&T 语法）+ GCC + CMake 那一套。BootInfo 的头文件有一个特殊之处：它同时被 bootloader（编译为 32 位 C）和内核（编译为 64 位 C++）include，所以需要处理 C/C++ 兼容性。这个小细节后面会展开讲。

## BootInfo 结构：bootloader 和内核的握手契约

### 为什么需要 BootInfo

先别急着看代码，我们先想清楚一个问题：bootloader 跳转到内核的时候，内核需要知道什么？

内核需要知道自己被加载到物理内存的哪个位置、入口点在哪里、帧缓冲区的地址和分辨率是多少、物理内存的布局是什么样的。这些信息内核自己是不知道的——它刚从磁盘被搬进内存，两眼一抹黑。如果没有这些信息，内核连自己代码在哪里都找不到，更别说初始化内存管理或者画个像素点了。

这就是 BootInfo 存在的意义：它是 bootloader 和内核之间的"握手契约"，bootloader 负责把所有启动时需要的信息填进去，内核负责从中读取。这个契约必须在两边都定义清楚——字段类型、字节对齐、结构体大小，任何不一致都会导致数据错位。

### 结构定义

`boot/boot_info.h` 是这个契约的具体形式。它是一个同时被 bootloader 和内核 include 的头文件，职责只有一个：定义一个两边都能理解的数据结构。

先看内存映射条目的定义：

```c
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed)) MemoryMapEntry;
```

`MemoryMapEntry` 直接对应 BIOS E820 调用返回的 24 字节条目格式。`__attribute__((packed))` 禁止编译器在字段之间插入填充字节——这一点在跨编译单元共享结构体时至关重要。如果你漏掉了 packed，编译器可能会在 `type` 和 `acpi` 之间塞进 4 字节的对齐填充，结构体变成 28 字节，然后 bootloader 写的偏移量和内核读的偏移量就对不上了。你会收获一个非常漂亮的数据错位 bug，而且不会触发任何编译错误，只能靠运行时的诡异行为来排查——这种 bug 调试起来真的能让人怀疑人生。

为了在编译期捕获这类问题，我们加了 `static_assert`。但这里有一个小细节值得展开：这个头文件同时被 C 和 C++ 编译器处理，而 C11 的 `_Static_assert` 和 C++11 的 `static_assert` 是不同的关键字。004C 的改动就是处理了这个兼容性：

```c
#if defined(__cplusplus)
static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#else
_Static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#endif
```

接下来是 BootInfo 的主体：

```c
typedef struct {
    uint64_t entry_point;
    uint64_t kernel_phys_base;
    uint64_t kernel_size;

    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;

    uint32_t mmap_count;
    uint32_t _pad;
    MemoryMapEntry mmap[32];
} __attribute__((packed)) BootInfo;
```

前三个字段是内核的自身信息：入口点的虚拟地址（高半核地址 `0xFFFFFFFF80020000`）、物理加载地址（`0x20000`）、大小。接下来五个字段是帧缓冲区信息——显存物理地址、宽度、高度、行间距、像素深度，这些数据来自 tag 002 的 VESA 初始化，存储在 0x6400。最后是 E820 内存映射表，最多 32 条记录，来自 tag 004A 的 E820 探测，存储在 0x5000。

为什么是 824 字节？3 个 uint64_t = 24 字节，加上 fb_addr = 8 字节，5 个 uint32_t = 20 字节（fb_width/height/pitch/bpp + mmap_count），加上 `_pad` = 4 字节，加上 32 x 24 = 768 字节的 mmap 数组，总计 24 + 8 + 20 + 4 + 768 = 824。静态断言也修正了——004C 改成了直接写明 824 字节这个数字，同样处理了 C/C++ 兼容的条件编译：

```c
#if defined(__cplusplus)
static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");
#else
_Static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");
#endif
```

这个数字不太可能变化——如果 mmap 的最大条目数从 32 改了，`static_assert` 会在编译时拦住你，绝不会让你带着一个大小不对的结构体上线。

### 为什么选 0x7000

BootInfo 放在固定物理地址 0x7000，这个位置是经过仔细挑选的。它不能和 E820 缓冲区（0x5000，占到大约 0x5000 + 4 + 32*24 = 0x51C4）冲突，不能和 VESA 帧缓冲信息（0x6400，约 20 字节，到 0x6414）冲突，不能和 DAP（0x7B00，16 字节）冲突，也不能和 MBR（0x7C00）或 Stage2（0x8000）冲突。0x7000 到 0x7AFF 之间有一块空地，而 BootInfo 是 824 字节（约 0x338），放到 0x7000 之后结束于 0x7338，距离 0x7B00 的 DAP 还有 0x7C8（1992 字节）的空间，绰绰有余。

你可能会问：为什么不动态分配一个地址？答案是调试。固定地址在调试时有巨大的优势——你可以在 QEMU monitor 中直接 `xp /824bx 0x7000` 查看完整的 BootInfo 内容，不需要先找指针再间接寻址。如果用了动态地址，每次调试都要先确认指针的值是什么，增加了不必要的认知负担。而且 Cinux 的 bootloader 是自写的，bootloader 和内核之间不存在"多个 bootloader 实现"的兼容性问题——如果是 GRUB 加载内核，那就得用 Multiboot 规范的动态地址方案了。

### 与 Linux boot_params 的对比

说到启动信息传递，我们不妨看看 Linux 是怎么做的。Linux 的 `boot_params` 结构定义在 `arch/x86/include/uapi/asm/bootparam.h` 中，大小是 4096 字节——比我们 824 字节的 BootInfo 大了近 5 倍。这倒不是 Linux 更"铺张"，而是它的 `boot_params` 承载了远比我们多的信息：EDD（Enhanced Disk Drive）数据、APM（Advanced Power Management）信息、EFI 系统表指针、VESA 显示模式细节、ISA 插槽信息……几乎所有 BIOS 能告诉你的硬件信息，Linux 都想收集起来。

设计思路的差异在于：Cinux 的 BootInfo 只传递内核启动的最小必要信息（入口点、物理地址、帧缓冲、内存映射），Linux 的 `boot_params` 则试图在启动阶段就把所有硬件信息一次性收集完备。Linux 这样做是因为它需要支持海量的硬件配置——不同的 BIOS 实现、不同的固件接口（BIOS、UEFI）、不同的启动方式（MBR、EFI stub、PXE），每种情况可能提供不同的硬件信息子集，所以它需要一个足够大的"容器"来容纳所有可能的信息。

数据结构的选择也有差异。Linux 的 `boot_params` 中嵌入了 `struct e820entry` 数组（128 条）和一个指向 `e820_table` 的间接指针，而 Cinux 直接把 E820 条目作为 `MemoryMapEntry mmap[32]` 嵌入 BootInfo。Linux 之所以用间接指针，是因为它的 E820 表可能在启动过程中动态扩展——内核会做排序、合并、冲突检测，这些操作可能增加条目数。Cinux 在 bootloader 阶段不做任何处理，只忠实地记录 BIOS 返回的原始数据。

参数传递机制也不同。Linux 的 boot protocol 规定 bootloader 把 `boot_params` 的物理地址放在 `%esi` 中（32 位入口）或 `%rsi` 中（64 位入口），而 Cinux 用的是 System V AMD64 ABI 的 `%rdi`。这是因为 Linux 的 32 位入口点 `startup_32` 使用的是不同于 System V ABI 的自定义寄存器约定（历史原因），而 Cinux 直接采用了标准的调用约定——对于教学项目来说，遵循标准比兼容历史包袱更重要。

## 高半核映射：让内核运行在高地址

### 为什么需要 Higher-Half Kernel

到 tag 003 为止，`setup_page_tables` 只建立了 identity mapping：PML4[0] 指向 PDPT，PDPT[0] 指向 PD，PD 的前 4 个条目用 2MB 大页映射了物理地址 0~8MB。这意味着虚拟地址 0x20000 直接等于物理地址 0x20000。identity mapping 在启动初期很好用——地址转换是 1:1 的，不用操心虚拟地址和物理地址的对应关系。但有一个问题：内核运行在低地址区域（0~8MB），和用户程序的地址空间会混在一起。

高半核（Higher-Half Kernel）的设计思路是让内核运行在虚拟地址空间的高端，典型地址是 `0xFFFFFFFF80000000` 往上。这样做的好处是用户程序可以独占整个低半地址空间（0 到 0x00007FFFFFFFFFFF，共 128TB），内核不需要为每一段代码和数据做特殊的地址范围处理。当内核需要运行用户程序时，只需要在页表中把高半核的映射保持不变、低半核的映射换成用户程序的映射就行了——内核和用户程序的地址空间天然隔离。

这不仅是 Cinux 的选择，也是几乎所有现代操作系统的标准做法。Linux 用 `__START_KERNEL_map = 0xFFFFFFFF80000000`，SerenityOS 用 `0xFFFFFFFF80000000`，Windows 用 `0xFFFFF80000000000`。甚至 xv6 虽然在教学环境下用 identity mapping，但它的改进版 xv6-riscv 也把内核放在了 `0x80000000`（RISC-V 的高地址区域）。这不是巧合——高半核是在地址空间层面隔离内核和用户态的最干净方案。

### PML4/PDPT 索引计算

现在我们来看 004C 在 `long_mode.S` 的 `setup_page_tables` 末尾加的代码。只有四条 `mov` 指令，但做的事情非常精妙：

```asm
    // PML4[511] -> PDPT (same as PML4[0])
    movl $PDPT_PHYS_ADDR, %eax
    orl $0x03, %eax
    movl %eax, PML4_PHYS_ADDR + (511 * 8)

    // PDPT[510] -> PD (same as PDPT[0])
    movl $PD_PHYS_ADDR, %eax
    orl $0x03, %eax
    movl %eax, PDPT_PHYS_ADDR + (510 * 8)
```

让我们拆解虚拟地址 `0xFFFFFFFF80020000` 在四级页表中的索引路径。x86-64 的 4-level paging 把 48 位有效虚拟地址分成四个 9 位的索引（每个索引对应一级页表），加上 12 位的页偏移。对于 `0xFFFFFFFF80020000`：

```
虚拟地址: 0xFFFFFFFF80020000
二进制:   1111...1111 11111110 00000000 00100000 00000000 00000000

PML4 索引 (bits 47:39):  111111111 = 511 = 0x1FF
PDPT 索引 (bits 38:30):  111111110 = 510 = 0x1FE
PD   索引 (bits 29:21):  000000000 = 0   = 0x000
页偏移   (bits 20:00):   0x20000 (within 2MB page)
```

PML4[511] 指向 PDPT——和 PML4[0] 指向的是同一个 PDPT。PDPT[510] 指向 PD——和 PDPT[0] 指向的是同一个 PD。这意味着 identity mapping 和 higher-half mapping 共享了同一套 PD 条目。PD[0] 映射了物理地址 0~2MB 的 2MB 大页，而 0x20000 落在 PD[0] 的范围内。所以不管是通过 identity mapping 访问 0x20000 还是通过 higher-half mapping 访问 0xFFFFFFFF80020000，CPU 穿过页表之后都会到达同一块物理内存。

这个设计的巧妙之处在于：只增加了两条页表项（PML4[511] 和 PDPT[510]），就建立起了一个完整的虚拟地址映射，不需要额外分配任何物理页来存放新的页表。代价是 PML4[511] 和 PDPT[510] 把整个 1GB 区域（PDPT[510] 映射的 `0xFFFFFFFF80000000`~`0xFFFFFFFFBFFFFFFF`）全部映射到了和 identity mapping 相同的物理内存上——这在启动阶段完全没问题，后续建立正式页表时会替换掉这些映射。

这里有一个 x86-64 的规范细节需要注意：虚拟地址必须是 canonical address，即 bits 63:48 必须是 bit 47 的符号扩展（Intel SDM Vol. 3A Section 3.3.7 对此有详细说明）。`0xFFFFFFFF80020000` 的 bit 47 = 1，所以 bits 63:48 全部是 1——这是合法的 canonical address。如果你尝试访问 `0x0000800000000000`（bit 47 = 0，bit 48 = 1，非 canonical），CPU 会直接触发 General Protection Fault，连页表查询都不会做。这个约束来自 x86-64 架构的虚拟地址设计——48 位是当前 4-level paging 的有效宽度，未来的 5-level paging 会扩展到 57 位，但 canonical address 的规则不变。

### 与 Linux 高半核映射的对比

Linux 同样使用 `__START_KERNEL_map = 0xFFFFFFFF80000000` 作为高半核基址，地址上和 Cinux 一模一样。但映射的实现方式差异很大。

Linux 的 `startup_64`（`arch/x86/kernel/head_64.S`）不使用复用策略——它有独立的内核页表。Linux 在启动阶段建立了独立的内核 PML4 条目（`init_top_pgt`），内核区域有自己专属的 PDPT 和 PD 页面。这样做的好处是 identity mapping 可以在内核启动后移除，减少攻击面。Linux 甚至支持 5-level paging（PML4 -> P4D -> PDPT -> PD -> Page），页表层级比 Cinux 多了一级。而且还支持 KASLR（Kernel Address Space Layout Randomization）——每次启动时内核的虚拟地址会随机偏移，让攻击者无法预测内核代码和数据的位置。

Cinux 选择复用策略的原因很简单：bootloader 阶段分配的固定页表内存（0x1000~0x3FFF）已经捉襟见肘——总共只有 3 个 4KB 页，分别给了 PML4、PDPT 和 PD。如果要建立独立的 PDPT 和 PD，还需要额外的 8KB 物理内存。更重要的是，bootloader 的页表只是临时的，内核启动后会建立自己的正式页表，启动阶段的简洁性比安全性更重要。当内核的物理内存管理器上线后，可以在内核初始化阶段分配新的页表页面，把高半核映射独立出来，然后取消 identity mapping——这是标准的安全加固步骤，很多生产级内核都会在启动完成后移除低地址的 identity mapping。

## BootInfo 填充：在 Long Mode 中搬运数据

### 填充过程

进入 Long Mode 之后，`stage2.S` 中的 `long_mode_entry` 开始执行。GDT 和段寄存器设置完成后，紧接着就是 004C 新增的 BootInfo 填充代码。先设定目标地址：

```asm
    movq $0x7000, %rdi              // BootInfo destination address
```

接下来逐字段填充。先是内核自身的三个字段：

```asm
    // 1. entry_point: higher-half kernel virtual address
    movq $0xFFFFFFFF80020000, %rax
    movq %rax, (%rdi)               // [0x7000] = entry_point

    // 2. kernel_phys_base: physical load address
    movq $0x20000, %rax
    movq %rax, 8(%rdi)              // [0x7008] = kernel_phys_base

    // 3. kernel_size: actual size (will be read from ELF later)
    movq $0x68000, %rax             // 416KB = 0x68000 bytes (max)
    movq %rax, 16(%rdi)             // [0x7010] = kernel_size
```

`entry_point` 写死为 `0xFFFFFFFF80020000`——这就是高半核的入口地址，对应 linker script 中 `_start` 的虚拟地址。`kernel_phys_base` 是小内核被加载到的物理地址。`kernel_size` 目前写死为 0x68000（416KB），这是预分配的最大值。理想情况下这个值应该从 ELF header 中动态读取，但当前 tag 为了简化实现选择了写死，后续 tag 会改进。

然后是帧缓冲区信息，从 0x6400 的 VESA 保存区域搬运过来：

```asm
    movq $0x6400, %rsi              // Source: VESA framebuffer info
    movq (%rsi), %rax               // Read fb_addr
    movq %rax, 24(%rdi)             // [0x7018] = fb_addr

    movl 8(%rsi), %eax              // Read fb_pitch
    movl %eax, 40(%rdi)             // [0x7028] = fb_pitch

    movzwq 12(%rsi), %rax           // Read fb_width (zero-extend 16-bit)
    movl %eax, 32(%rdi)             // [0x7020] = fb_width

    movzwq 14(%rsi), %rax           // Read fb_height (zero-extend 16-bit)
    movl %eax, 36(%rdi)             // [0x7024] = fb_height

    movl $32, %eax
    movl %eax, 44(%rdi)             // [0x702C] = fb_bpp
```

这里有一个容易忽略的类型宽度问题：0x6400 处存储的 fb_width 和 fb_height 是 16 位值（VESA Mode Info 返回的 XResolution 和 YResolution 是 uint16），但 BootInfo 结构中 fb_width 和 fb_height 是 uint32。所以必须用 `movzwq`（Move Word to Quadword with Zero-Extend）做零扩展：从 16 位读到 64 位寄存器，高 48 位自动填零，然后用 `movl` 写入 32 位目标字段。如果你直接用 `movl` 读 16 位值，高 16 位会带上源地址后面的垃圾数据——这种隐式 bug 在 QEMU 里不一定暴露，但在真机上可能让你的分辨率变成一个荒谬的值。

fb_bpp 写死为 32——这是 VESA mode 0x118（1024x768x32bpp）的标准值。

最后是 E820 内存映射表的搬运：

```asm
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

0x5000 处的前 4 字节是条目计数，直接写入 BootInfo 偏移 48。从偏移 4 开始是连续的 24 字节条目数组，需要复制到 BootInfo 的偏移 56 处。这里的复制循环比较朴素——逐字节搬运 768 字节。在 Long Mode 下完全可以优化为用 `rep movsq` 一次搬完，但考虑到这段代码只执行一次（启动时），而且 768 字节并不算多，用最简单的方式实现反而减少了出错的可能。启动代码的可靠性远比性能重要。

### 参数传递：System V AMD64 ABI 的 %rdi 约定

BootInfo 填充完毕后，就是整个 bootloader 最激动人心的时刻——跳转到内核。

```asm
    movb $0x4A, %al                 // 'J'
    outb %al, $DEBUGCON_PORT        // Output 'J' to debugcon

    movq $0x7000, %rdi              // First argument: BootInfo*
    movq $0xFFFFFFFF80020000, %rax  // Entry point: _start in higher-half

    jmp *%rax
```

先往 debugcon 输出一个 `'J'`（0x4A），作为"即将跳转"的信号。这个字符在调试时非常有用——如果你在 `debug.log` 里看到 `OPLJ` 序列（O = Stage2 OK, P = Protected Mode, L = Long Mode, J = Jump），就说明 bootloader 全程顺利，控制权已经交给内核。如果停在 `OPL` 没有 `J`，说明跳转前的代码出了问题。

然后按 System V AMD64 ABI 约定，把 BootInfo 指针（0x7000）放入 `%rdi` 作为第一个参数。这是 64 位 Linux 系统调用的标准调用约定——第一个整数参数在 `%rdi`，第二个在 `%rsi`，依此类推。Intel SDM Vol. 3A 对此没有直接规定（ABI 规范由 OS 和工具链共同约定，不属于 ISA 层面），但 System V AMD64 ABI 文档的 "3.2 Function Calling Sequence" 一节明确定义了这个约定。内核的 `_start` 函数会从 `%rdi` 拿到这个值。

最后 `jmp *%rax` 是一个间接跳转——CPU 从 `%rax` 中读取目标地址然后无条件跳过去。跳转之后 bootloader 的代码就永远不会回来了。值得一提的是，这里用的是 `jmp` 而不是 `call`——bootloader 不需要内核返回，因为 bootloader 的使命已经结束了。用 `call` 的话返回地址会压栈白白浪费 8 字节，用 `jmp` 在语义上更准确。

这个跳转跨越了虚拟地址空间：从 identity mapping 区域（bootloader 的代码还在低地址执行）跳到 higher-half 区域（内核的 `_start` 在 `0xFFFFFFFF80020000`）。因为两条映射指向同一套物理页表条目，CPU 的 TLB 能够正确地翻译这个地址，跳转不会触发 Page Fault。

### 与 Linux 参数传递机制的对比

Linux 的参数传递路径和 Cinux 类似但更复杂。Linux 的 `startup_64` 接收 boot_params 指针的方式是：bootloader（GRUB 或 EFI stub）把 `boot_params` 的地址放在 `%rsi` 中，然后 Linux 在 `startup_64` 里把它保存到 `%r15`——一个 callee-saved 寄存器，在整个启动过程中不会被函数调用破坏。等到需要传给 C 代码时，再把 `%r15` 恢复到 `%rdi`：`movq %r15, %rdi`。

Cinux 的做法是把 `%rdi` 保存到 `.data` 段的 `__boot_info_ptr` 变量中。两种方式各有优劣：Linux 用寄存器保存，好处是不占内存，坏处是如果启动过程中有汇编代码不小心用了 `%r15`（比如忘了 callee-saved 约定），值就被破坏了。Cinux 用内存保存，好处是 `.data` 段的内容不会被任何正常操作破坏（除非有代码显式修改它），坏处是多了一次内存访问。对于教学项目来说，内存保存更安全——你不用担心寄存器约定在复杂的启动序列中被意外违反。

## debugcon 配置变更

最后说一个小但关键的配置变更。`cmake/qemu.cmake` 把 debugcon 相关的 QEMU 参数从 `QEMU_DEBUG_FLAGS` 移到了 `QEMU_COMMON_FLAGS`：

```cmake
set(QEMU_COMMON_FLAGS
    -m 512M
    -serial stdio
    -no-reboot
    -no-shutdown
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
)

set(QEMU_DEBUG_FLAGS
    -s
    -S
)
```

之前 debugcon 只在 debug 模式（`make run-debug`）下启用，正常模式（`make run`）下是不启用的。但随着 tag 004C 的完成，内核的 `boot.S` 和 `main.cpp` 都依赖 debugcon 输出来验证执行状态——`OPLJ123G4===CPPGC1V123B===END` 这串字符全部通过 `outb` 写到 0xE9 端口。如果正常模式下不启用 debugcon，`outb %al, $0xE9` 会变成一个空操作，你什么都看不到。把它移到 COMMON 后，不管以什么模式运行，debugcon 都会把输出写到 `debug.log` 文件中。

`-global isa-debugcon.iobase=0xe9` 把 debugcon 设备的 I/O 端口绑定到 0xE9。QEMU 的 isa-debugcon 设备默认端口就是 0xE9，但显式指定可以避免未来的默认值变更导致问题。

## 验证

来验证一下本章的成果。构建并运行 QEMU：

```bash
cd build && cmake .. && make -j$(nproc)
make run
```

然后检查 `debug.log` 文件。如果你看到 `OPLJ` 序列，说明 bootloader 已经成功完成 BootInfo 填充和高半核跳转。其中 `J` 是最后一步"跳转到内核"的标志——如果 `J` 出现了但后面没有内核侧的输出（`1234...`），那说明内核侧的代码有问题，那是下一篇的内容。如果连 `J` 都没出现，那说明 BootInfo 填充或跳转本身出了问题，可以用 QEMU 的 `-d int` 选项查看是否有异常中断。

如果你想更深入地验证 BootInfo 的内容，可以在 QEMU monitor 里用 `xp /824bx 0x7000` 查看 BootInfo 的原始数据——前 8 字节应该是 entry_point（`0xFFFFFFFF80020000` 的小端序表示），接下来 8 字节是 kernel_phys_base（`0x20000` 的小端序），以此类推。

## 收尾

回过头看，这一章我们做了 bootloader 侧的最后一组工作：定义了 BootInfo 握手契约（824 字节的结构体，packed 对齐，C/C++ 兼容的 static_assert），在页表里加了两条映射建立高半核（PML4[511] 和 PDPT[510]），在 Long Mode 中把所有启动信息从各自的临时缓冲区搬运到 BootInfo 里，最后用一个 `jmp *%rax` 把控制权交给了内核。

设计上最值得记住的点有两个：一是固定地址 BootInfo 的选择——牺牲了灵活性但换来了调试的便利性；二是高半核映射的复用策略——用两条页表项就建起了完整的虚拟地址映射，零额外内存分配。这两个决策都是在 bootloader 资源极度受限的环境下做出的合理权衡。

下一篇我们进入内核侧，看内核怎么从 `_start` 入口开始，经历关中断、开 SSE、设栈、清 BSS、调用全局构造函数这些步骤，最终进入 C++ 的 `main` 函数。那里有两个非常经典的 bug 等着我们——`rep stosb` 破坏 `%rdi` 的陷阱和链接器的符号地址冲突，都是那种"知道了觉得理所当然、不知道时能调一整天"的坑。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 3.3.7 -- Canonical Address: 定义了 x86-64 虚拟地址的 canonical 约束（bits 63:48 必须是 bit 47 的符号扩展）。`0xFFFFFFFF80020000` 的 bit 47 = 1，高位全 1，符合 canonical 要求。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 4.3 -- 4-Level Paging: 4 级页表结构（PML4 -> PDPT -> PD -> Page），2MB 大页的 PS bit 设置，以及 PML4/PDPT 条目格式。Cinux 的高半核映射利用 PD[0]~PD[3] 的 2MB 大页条目被 PML4[511]/PDPT[510] 复用。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- System V Application Binary Interface, AMD64 Architecture Processor Supplement, Section 3.2: 函数调用序列定义——第一个整数参数在 `%rdi`，callee-saved 寄存器为 `%rbx, %rbp, %r12~%r15`。Cinux 的 BootInfo 指针通过 `%rdi` 传递。
  https://gitlab.com/x86-psABIs/x86-64-ABI

- Linux Kernel `arch/x86/kernel/head_64.S`: Linux 的 `startup_64` 入口点，使用 `__START_KERNEL_map`（`0xFFFFFFFF80000000`）作为高半核基址。Linux 把 boot_params 保存到 `%r15`（callee-saved），依赖 decompressor 清零 BSS，页表结构远比 Cinux 复杂（5-level paging、KASLR、SMEP/SMAP 等）。
  https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S

- xv6 `entry.S` (MIT): xv6 的入口代码——比 Cinux 简单得多，因为 xv6 依赖 GRUB（Multiboot）完成 BSS 清零，且是纯 C（没有 `.init_array` 和全局构造函数）。xv6 使用 `V2P_WO(entry)` 计算物理入口地址。
  https://github.com/mit-pdos/xv6-public/blob/master/entry.S

- OSDev Wiki -- Calling Global Constructors: `.init_array` 段的遍历方法详解，包括 ARM BPABI 风格（直接遍历函数指针数组）和传统 GNU 风格（`crti.o`/`crtbegin.o` 级联）。Cinux 使用前者。
  https://wiki.osdev.org/Calling_Global_Constructors
