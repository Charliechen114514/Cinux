# 004 通读版 · BootInfo 填充与高半核跳转

## 概览

到 tag 004B 为止，我们的 Stage2 bootloader 已经能在 Real Mode 下把小内核的扁平二进制完整读到物理地址 0x20000，然后一路穿过保护模式、建好页表、开启 Long Mode，最后优雅地 `hlt` 在那里。但一个 halt 住的 bootloader 没有任何意义——它该做的事情是把控制权交接给内核，让内核开始真正工作。

tag 004C 要做的事情就是把这条"最后一公里"打通。具体来说，bootloader 进入 Long Mode 之后需要做三件事：把 BootInfo 结构填好（告诉内核你被加载到哪里、内存长什么样、显存在哪里），在页表里加上高半核映射（让内核运行在高地址而不是低地址），最后跳过去。本文覆盖 bootloader 侧的全部代码——BootInfo 的 C 语言定义（`boot/boot_info.h`）、高半核页表映射（`boot/common/long_mode.S`）、BootInfo 填充与跳转逻辑（`boot/stage2.S`），以及一个小但关键的构建配置变更（`cmake/qemu.cmake`）。

关键设计决策一览：BootInfo 放在固定物理地址 0x7000，bootloader 和内核通过 System V AMD64 ABI 的 `%rdi` 寄存器传递指针；高半核映射复用了同一套 PD 条目，identity mapping 和 higher-half mapping 指向同一块物理内存。

## 架构图

先看完整的启动流程，搞清楚 004C 新增的部分在整个链条里处于什么位置：

```
                    Cinux Tag 004C 启动流程
                    ========================

  Power On
     |
     v
  [MBR @ 0x7C00]  ---  加载 Stage2 到 0x8000，跳转
     |
     v
  [Stage2 @ 0x8000]  (Real Mode)
     ├── enable_a20
     ├── VESA 初始化
     ├── E820 内存探测 -> 0x5000
     ├── load_kernel_from_disk -> 0x20000 (完整小内核)
     ├── 切换保护模式
     ├── setup_page_tables (0x1000~0x3FFF)
     │    └── NEW: PML4[511] -> PDPT, PDPT[510] -> PD  (高半核映射)
     ├── enter_long_mode
     |
     v
  [Long Mode Entry]
     ├── 填充 BootInfo @ 0x7000       <-- NEW
     │    ├── entry_point = 0xFFFFFFFF80020000
     │    ├── kernel_phys_base = 0x20000
     │    ├── fb_addr/width/height/pitch/bpp (from 0x6400)
     │    └── mmap_count + mmap[] (from 0x5000)
     ├── outb 'J' to debugcon          <-- NEW
     ├── movq $0x7000, %rdi            <-- NEW
     ├── jmp *0xFFFFFFFF80020000       <-- NEW (跳入小内核)
     |
     v
  [Mini Kernel @ 0xFFFFFFFF80020000]   <-- NEW (004C 文章 2 覆盖)
```

内存布局视角——低 1MB 中的所有关键数据结构：

```
  物理地址        内容                      来源 Tag
  ─────────────────────────────────────────────────
  0x0000~0x0400   IVT                       BIOS
  0x1000~0x3FFF   页表 (PML4/PDPT/PD)       003
  0x5000          E820 内存映射表             004A
  0x6000          VBE Controller Info        002
  0x6200          VBE Mode Info              002
  0x6400          Framebuffer Info           002
  0x7000          BootInfo 结构              004C <-- NEW
  0x7B00          DAP (磁盘读取参数)          004A
  0x7C00          MBR                        000
  0x8000          Stage2                     001~004
  ─────────────────────────────────────────────────
  0x20000         Mini Kernel (flat binary)  004B
  0x90000         Protected/Long Mode 栈     002
```

高半核映射的虚拟地址视角：

```
  虚拟地址                              物理地址         说明
  ──────────────────────────────────────────────────────────────
  0x0000000000000000 ~ 0x00000000007FFFFF  0~8MB     Identity mapping
  0xFFFFFFFF80000000 + 0x20000            0x20000   Higher-half kernel entry
                   = 0xFFFFFFFF80020000
```

你会发现整个设计的主线非常清晰：bootloader 在 Real Mode 和 Long Mode 阶段分别负责不同的数据准备工作，所有需要 BIOS 中断的操作在 Real Mode 一次性完成，进入 Long Mode 之后只做纯数据搬运和结构体填充，最后用一个 `jmp *%rax` 完成交接。

## 代码精讲

### BootInfo 结构定义——bootloader 和内核的握手契约

`boot/boot_info.h` 是一个同时被 bootloader（编译为 32 位 C）和内核（编译为 64 位 C++）include 的头文件。它的职责只有一个：定义一个两边都能理解的数据结构，让 bootloader 有地方写、内核有地方读。

```c
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed)) MemoryMapEntry;
```

`MemoryMapEntry` 直接对应 BIOS E820 调用返回的 24 字节条目格式。`__attribute__((packed))` 禁止编译器在字段之间插入填充字节——这一点在跨编译单元共享结构体时至关重要。如果你漏掉了 packed，编译器可能会在 `type` 和 `acpi` 之间塞进 4 字节的对齐填充，结构体变成 28 字节，然后 bootloader 写的偏移量和内核读的偏移量就对不上了，你会收获一个非常漂亮的数据错位 bug，而且不会触发任何编译错误。

`static_assert` 用来在编译期捕获这类问题。但这里有一个小细节值得展开：这个头文件同时被 C 和 C++ 编译器处理，而 C11 的 `_Static_assert` 和 C++11 的 `static_assert` 是不同的关键字。004C 的改动就是处理了这个兼容性：

```c
#if defined(__cplusplus)
static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#else
_Static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
#endif
```

BootInfo 主体结构长这样：

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

前三个字段是内核的自身信息：入口点的虚拟地址（高半核地址 `0xFFFFFFFF80020000`）、物理加载地址（`0x20000`）、大小。接下来六个字段是帧缓冲区信息——显存物理地址、分辨率、行间距、像素深度，这些数据来自 tag 002 的 VESA 初始化，存储在 0x6400。最后是 E820 内存映射表，最多 32 条记录，来自 tag 004A 的 E820 探测，存储在 0x5000。

静态断言也修正了——之前用的是一个数学表达式做 size check，004C 改成了直接写明 824 字节这个数字，并添加了 C/C++ 兼容的条件编译：

```c
#if defined(__cplusplus)
static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");
#else
_Static_assert(sizeof(BootInfo) == 824, "BootInfo size mismatch");
#endif
```

为什么是 824？3 个 uint64_t = 24 字节（entry_point + kernel_phys_base + kernel_size），加上 fb_addr = 8 字节，6 个 uint32_t = 24 字节（fb_width/height/pitch/bpp + mmap_count + _pad），加上 32 x 24 = 768 字节的 mmap 数组，总计 24 + 8 + 24 + 768 = 824。这个数字不太可能变化——如果 mmap 的最大条目数从 32 改了，`static_assert` 会在编译时拦住你。

### 高半核页表映射——让内核运行在高地址

到 tag 003 为止，`setup_page_tables` 只建立了 identity mapping：PML4[0] 指向 PDPT，PDPT[0] 指向 PD，PD 的前 4 个条目（PD[0]~PD[3]）用 2MB 大页映射了物理地址 0~8MB。这意味着虚拟地址 0x20000 直接等于物理地址 0x20000，identity mapping 在启动初期很好用，但有一个问题：内核运行在低地址区域，和用户程序的地址空间会混在一起。

高半核（Higher-Half Kernel）的设计思路是让内核运行在虚拟地址空间的高端，典型地址是 `0xFFFFFFFF80000000` 往上。这样做的好处是用户程序可以独占整个低半地址空间（0 到 0x00007FFFFFFFFFFF），内核不需要为每一段代码和数据做特殊的地址范围处理。

现在我们来看 004C 在 `long_mode.S` 的 `setup_page_tables` 末尾加的代码：

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

只有四条 `mov` 指令，但做的事情非常精妙。让我们拆解虚拟地址 `0xFFFFFFFF80020000` 在四级页表中的索引路径：

```
  虚拟地址: 0xFFFFFFFF80020000
  二进制:   1111...1111 10000000 00000010 00000000 00000000

  PML4 索引 (bits 47:39):  111111111 = 511 = 0x1FF
  PDPT 索引 (bits 38:30):  100000000 = 510 = 0x1FE
  PD   索引 (bits 29:21):  000000001 = 1   = 0x001
  页偏移   (bits 20:00):   00000000 00000000 00000000 = 0x00000 (within 2MB page)
```

PML4[511] 指向 PDPT（和 PML4[0] 指向的是同一个 PDPT），PDPT[510] 指向 PD（和 PDPT[0] 指向的是同一个 PD）。这意味着 identity mapping 和 higher-half mapping 共享了同一套 PD 条目。PD[1] 映射了物理地址 0x200000~0x400000 的 2MB 大页，而 `0x20000` 落在 PD[0] 的范围内（PD[0] 映射 0~2MB），所以不管是通过 identity mapping 访问 0x20000 还是通过 higher-half mapping 访问 0xFFFFFFFF80020000，CPU 穿过页表之后都会到达同一块物理内存。

这个设计的巧妙之处在于：只增加了两条页表项（PML4[511] 和 PDPT[510]），就建立起了一个完整的虚拟地址映射，不需要额外分配任何物理页来存放新的页表。代价是 PML4[511] 和 PDPT[510] 把整个 1GB 区域（PDPT[510] 映射的 0xFFFFFFFF80000000~0xFFFFFFFFBFFFFFFF）全部映射到了和 identity mapping 相同的物理内存上——这在启动阶段完全没问题，后续建立正式页表时会替换掉这些映射。

这里有一个值得注意的 x86-64 规范细节：虚拟地址必须是 canonical address，即 bits 63:48 必须是 bit 47 的符号扩展。`0xFFFFFFFF80020000` 的 bit 47 = 1，所以 bits 63:48 全部是 1——这是合法的 canonical address。如果你尝试访问 `0x0000800000000000`（bit 47 = 0，bit 48 = 1，非 canonical），CPU 会直接触发 General Protection Fault，连页表查询都不会做。Intel SDM Vol. 3A Section 3.3.7 对此有详细说明。

### BootInfo 填充——在 Long Mode 中搬运数据

进入 Long Mode 之后，`stage2.S` 中的 `long_mode_entry` 开始执行。GDT 和段寄存器设置完成后，紧接着就是 004C 新增的 BootInfo 填充代码。

先设定目标地址：

```asm
    movq $0x7000, %rdi              // BootInfo destination address
```

0x7000 这个位置经过了仔细的选择。它不能和 E820 缓冲区（0x5000）冲突，不能和 VESA 帧缓冲信息（0x6400）冲突，不能和 DAP（0x7B00）冲突，也不能和 MBR（0x7C00）或 Stage2（0x8000）冲突。0x7000 到 0x7AFF 之间有一块空地，而 BootInfo 是 824 字节（约 0x338），放到 0x7000 之后结束于 0x7338，距离 0x7B00 的 DAP 还有足够的空间。

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

这里有一个容易忽略的类型宽度问题：0x6400 处存储的 fb_width 和 fb_height 是 16 位值（VESA Mode Info 返回的 XResolution 和 YResolution 是 uint16），但 BootInfo 结构中 fb_width 和 fb_height 是 uint32。所以必须用 `movzwq`（Move Word to Quadword with Zero-Extend）做零扩展：从 16 位读到 64 位寄存器，高 48 位自动填零，然后用 `movl` 写入 32 位目标字段。如果你直接用 `movl` 读 16 位值，高 16 位会带上源地址后面的垃圾数据。

fb_bpp 写死为 32——这是 VESA mode 0x118（1024x768x32bpp）的标准值。严格来说应该从 VESA Mode Info 中读取 BitsPerPixel 字段，但 0x118 的 bpp 就是 32，写死简化了代码。

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

0x5000 处的前 4 字节是条目计数（mmap_count），直接写入 BootInfo 偏移 48。从偏移 4 开始是连续的 24 字节条目数组，需要复制到 BootInfo 的偏移 56 处（前 56 字节是 entry_point + kernel_phys_base + kernel_size + fb_addr + fb_width + fb_height + fb_pitch + fb_bpp + mmap_count + _pad）。

这里的复制循环比较朴素——逐字节搬运 768 字节。在 Long Mode 下完全可以优化为用 `rep movsq`（8 字节为单位）一次搬完，但考虑到这段代码只执行一次（启动时），而且 768 字节并不算多，用最简单的方式实现反而减少了出错的可能。启动代码的可靠性远比性能重要。

### 跳转到小内核——bootloader 的最终使命

BootInfo 填充完毕后，就是整个 bootloader 最激动人心的时刻——跳转。

```asm
    movb $0x4A, %al                 // 'J'
    outb %al, $DEBUGCON_PORT        // Output 'J' to debugcon

    movq $0x7000, %rdi              // First argument: BootInfo*
    movq $0xFFFFFFFF80020000, %rax  // Entry point: _start in higher-half

    jmp *%rax
```

先往 debugcon 输出一个 `'J'`（0x4A），作为"即将跳转"的信号。这个字符在调试时非常有用——如果你看到 `OPLJ` 序列（O = Stage2 OK, P = Protected Mode, L = Long Mode, J = Jump），就说明 bootloader 全程顺利，控制权已经交给内核。如果停在 `OPL` 没有 `J`，说明跳转前的代码出了问题。

然后按 System V AMD64 ABI 约定，把 BootInfo 指针（0x7000）放入 `%rdi` 作为第一个参数。这是 64 位 Linux 系统调用的标准调用约定——第一个整数参数在 `%rdi`，第二个在 `%rsi`，依此类推。内核的 `_start` 函数会从 `%rdi` 拿到这个值。

最后 `jmp *%rax` 是一个间接跳转——CPU 从 `%rax` 中读取目标地址，然后无条件跳过去。跳转之后 bootloader 的代码就永远不会回来了。这个跳转跨越了虚拟地址空间：从 identity mapping 区域（bootloader 的代码还在低地址执行）跳到 higher-half 区域（内核的 `_start` 在 0xFFFFFFFF80020000）。因为两条映射指向同一套物理页表条目，CPU 的 TLB 能够正确地翻译这个地址，跳转不会触发 Page Fault。

值得一提的是，`jmp *%rax` 而不是 `call *%rax`——这是一个跳转，不是函数调用。bootloader 不需要内核返回，因为 bootloader 的使命已经结束了。如果你用了 `call`，返回地址会压栈，但内核永远不会 `ret`，那 8 字节的栈空间就白白浪费了——当然，这其实无关紧要，但用 `jmp` 在语义上更准确。

### debugcon 配置变更

`cmake/qemu.cmake` 做了一个小改动：把 debugcon 相关的 QEMU 参数从 `QEMU_DEBUG_FLAGS` 移到了 `QEMU_COMMON_FLAGS`。

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

之前 debugcon 只在 debug 模式（`make run-debug`）下启用，正常模式（`make run`）下是不启用的。但随着 tag 004C 的完成，内核的 `boot.S` 和 `main.cpp` 都依赖 debugcon 输出来验证执行状态（`OPLJ123G4===CPPGC1V123B===END` 这串字符全部通过 `outb` 写到 0xE9 端口）。如果正常模式下不启用 debugcon，`outb %al, $0xE9` 会变成一个空操作，你什么都看不到。把它移到 COMMON 后，不管以什么模式运行，debugcon 都会把输出写到 `debug.log` 文件中。

`-global isa-debugcon.iobase=0xe9` 把 debugcon 设备的 I/O 端口绑定到 0xE9。QEMU 的 isa-debugcon 设备默认端口就是 0xE9，但显式指定可以避免未来的默认值变更导致问题。

## 设计决策

### 决策：BootInfo 使用固定物理地址 vs 动态协商

**问题**：bootloader 和内核需要共享一块内存来传递启动信息，怎么确定这块内存的地址？

**本项目的做法**：硬编码物理地址 0x7000。bootloader 在 `stage2.S` 中往 0x7000 写 BootInfo，内核在 `boot.S` 中从 `%rdi` 拿到这个地址（%rdi = 0x7000），然后通过 `__boot_info_ptr` 保存到 `.data` 段供后续使用。

**备选方案**：Multiboot 规范的做法是 bootloader 把信息结构体的物理地址放在 `%ebx` 中传递，内核根据 `%ebx` 的值动态找到结构体。Linux 的 `boot_params` 类似，bootloader 把 `boot_params` 的地址放在 `%esi` 中。

**为什么不选备选方案**：Cinux 的 bootloader 是自写的，bootloader 和内核之间不存在"多个 bootloader 实现"的兼容性问题。固定地址在调试时有巨大的优势——你可以在 QEMU monitor 中直接 `xp /824bx 0x7000` 查看完整的 BootInfo 内容，不需要先找指针再间接寻址。如果用了动态地址，每次调试都要先确认 `%rdi` 的值是什么，增加了不必要的认知负担。

**如果要扩展/改进，应该怎么做**：当内核支持 Multiboot 或 Multiboot2 启动协议时，可以改为动态地址方案——bootloader 把 BootInfo 放在任意可用内存区域，通过约定寄存器传递地址。这样可以支持不同的 bootloader 实现加载 Cinux 内核。

### 决策：高半核映射复用 PD 条目 vs 独立页表

**问题**：高半核映射的页表是复用 identity mapping 的 PD 条目，还是分配独立的物理页建立完全分离的页表？

**本项目的做法**：复用。PML4[511] 指向和 PML4[0] 相同的 PDPT，PDPT[510] 指向和 PDPT[0] 相同的 PD。两条页表项，零额外内存分配。

**备选方案**：Linux 的做法是为内核区域分配独立的 PDPT 和 PD 页面，在高半核映射和 identity mapping 之间建立完全隔离的页表结构。这样 identity mapping 可以在内核启动后移除，减少攻击面。

**为什么不选备选方案**：Cinux 在 bootloader 阶段分配的固定页表内存（0x1000~0x3FFF）已经捉襟见肘——总共只有 3 个 4KB 页，分别给了 PML4、PDPT 和 PD。如果要建立独立的 PDPT 和 PD，还需要额外的 8KB 物理内存。更重要的是，bootloader 的页表只是临时的，内核启动后会建立自己的正式页表，启动阶段的简洁性比安全性更重要。

**如果要扩展/改进，应该怎么做**：当内核的物理内存管理器（PMM）上线后，可以在内核初始化阶段分配新的页表页面，把高半核映射独立出来，然后取消 identity mapping。这是标准的安全加固步骤——很多生产级内核（Linux、Windows）都会在启动完成后移除低地址的 identity mapping。

## 扩展方向

1. **动态读取内核大小**（难度：中等）——当前 `kernel_size` 写死为 0x68000。可以在 bootloader 读取 ELF header 后，从 Program Header 中提取实际的内核大小写入 BootInfo。这需要解析 ELF64 的 `p_filesz` 和 `p_memsz` 字段。

2. **从 VESA Mode Info 动态读取 bpp**（难度：简单）——当前 `fb_bpp` 写死为 32。改为从 0x6200（VBE Mode Info）中读取 `BitsPerPixel` 字段（偏移 0x19），这样可以支持不同的 VESA 模式。

3. **优化内存拷贝为 rep movsq**（难度：简单）——把 768 字节的逐字节 mmap 复制循环替换为 `rep movsq`（每次复制 8 字节，96 次迭代）。这纯粹是性能优化，对功能没有影响，但可以顺便练习 AT&T 语法的 `rep` 前缀指令。

4. **支持 Multiboot 启动协议**（难度：困难）——让 Cinux 内核可以被 GRUB 加载，需要实现 Multiboot 规范要求的 header magic、checksum 和 flag 字段。这意味着内核需要能在不同的 BootInfo 来源下工作（自写 bootloader 的固定地址 vs GRUB 的 Multiboot Info 结构）。

5. **加入帧缓冲区像素格式信息**（难度：中等）——当前 BootInfo 只记录了 bpp=32，但 32bpp 有多种像素格式（RGBX、BGRX、ARGB 等）。可以从 VESA Mode Info 中提取 `RedMaskSize`、`GreenMaskSize`、`BlueMaskSize` 等字段，放入 BootInfo 供内核的图形驱动使用。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 3.3.7 -- Canonical Address: 定义了 x86-64 虚拟地址的 canonical 约束（bits 63:48 必须是 bit 47 的符号扩展）。`0xFFFFFFFF80020000` 的 bit 47 = 1，高位全 1，符合 canonical 要求。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A, Section 4.3 -- 4-Level Paging: 4 级页表结构（PML4 -> PDPT -> PD -> Page），2MB 大页的 PS bit 设置，以及 PML4/PDPT 条目格式。Cinux 的高半核映射就是利用 PD[0]~PD[3] 的 2MB 大页条目被 PML4[511]/PDPT[510] 复用。

- OSDev Wiki -- Calling Global Constructors: `.init_array` 段的详细说明，GCC 如何为全局对象生成 `_GLOBAL__sub_I_XXX` 条目，以及 ARM BPABI 风格的直接迭代方法（Cinux 采用的方案）。
  https://wiki.osdev.org/Calling_Global_Constructors

- OSDev Wiki -- C++ Bare Bones: freestanding 环境下 C++ 内核的基本设置，包括 `-ffreestanding`、`-fno-exceptions`、`-fno-rtti` 编译选项，以及手动提供 `__cxa_pure_virtual` 等运行时函数的必要性。
  https://wiki.osdev.org/C%2B%2B_Bare_Bones

- Linux Kernel `arch/x86/kernel/head_64.S`: Linux 的 `startup_64` 入口点，使用 `__START_KERNEL_map`（0xFFFFFFFF80000000）作为高半核基址。Linux 同样使用 PML4 的高位索引来映射内核区域，但它的页表结构远比 Cinux 复杂（5-level paging、KASLR、SMEP/SMAP 等）。
  https://github.com/torvalds/linux/blob/master/arch/x86/kernel/head_64.S
