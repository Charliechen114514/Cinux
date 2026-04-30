# Bootloader 开发日记 004B：保护模式的"静默"、栈冲突惊魂记，以及 BootInfo 的诞生

> 标签：x86, 保护模式, Long Mode, bootloader, 内存布局, BootInfo, flat binary
> 前置：[004A E820 + INT 13h 基础](004-boot-load-mini-kernel-a-1.md)

## 写在前面

到 tag 004A 为止，我们的 bootloader 已经能在 Real Mode 下用 E820 探测物理内存布局，用 INT 13h 从磁盘读取小内核的 ELF header（4KB，8 个扇区）。但说实话，一个只有 ELF header、连实际代码都没加载的系统，离"把内核跑起来"这个目标还有一大段路。004A 留下的状态是：物理地址 0x10000 处放着 4KB 的 ELF header，Stage2 不知道内核到底有多大、该读多少扇区、该加载到哪里。

这一章我们要把这件事彻底做完。具体来说就是三件事：第一，定义一个叫 BootInfo 的结构体，作为 bootloader 和内核之间的"交接契约"——bootloader 把 E820 内存映射、VESA 帧缓冲区信息、内核的入口地址和物理基址全部打包进去，内核启动时拿这个结构体就知道世界长什么样。第二，把磁盘读取从 8 扇区扩展到 832 扇区（416KB 完整小内核），而且因为 BIOS INT 13h 每次最多只能读 127 个扇区，所以必须用循环分块读取，每次动态计算 buffer 地址和 LBA 偏移。第三，给小内核加上真正的入口代码——不再是 004A 那个 `cli; hlt` 死循环，而是一个完整的 boot.S 汇编入口，负责设置栈、清零 BSS 段、保存 BootInfo 指针，然后跳转到 C++ 的 `mini_kernel_main`。

在这三件事之外，还有一个让我血压拉满的踩坑：栈冲突。小内核最初设计加载到物理地址 0x10000，但 Real Mode 的栈恰好占据 0x9000~0x19000，磁盘写入会直接覆盖栈上保存的返回地址，然后 `ret` 指令跳飞到不知名的地方。这个 bug 花了我大半天才发现——症状是 `load_kernel_from_disk` 返回后系统直接 triple fault，用 GDB 看返回地址已经被磁盘数据覆盖成了乱码。解决方案是把加载地址从 0x10000 提高到 0x20000，留出 28KB 的安全间隙。这个改动牵一发而动全身：boot.S 的常量要改、链接脚本的物理地址要改、build_image.sh 的大小上限要改，全章所有地址计算都围绕 0x20000 这个锚点展开。

还有一件事值得提前说：在这一章里，保护模式（Protected Mode）什么都不做。这不是偷懒，而是架构层面的刻意选择。所有数据搬运（E820 探测 + 832 扇区磁盘读取）全部在 Real Mode 下完成，因为只有 Real Mode 才能用 BIOS 中断。保护模式只是一个必经的过渡状态——设置段寄存器、建页表、切 Long Mode——不做任何跟内核加载相关的工作。这意味着保护模式阶段的代码和 004A 相比完全没变，只是多了一块注释说明"这里什么也不需要做"。

## 环境说明

和 004A 保持一致：x86_64 架构，QEMU 虚拟机，GNU AS 汇编器（AT&T 语法），GCC/G++ 编译器，CMake 构建系统。bootloader 代码以 16 位 Real Mode 汇编为主，小内核以 64 位 Long Mode C++ 编写。调试手段是 QEMU 的 debugcon 端口（0xE9），配合 `-debugcon file:debug.log` 参数把字符输出到日志文件。本章结束后，如果一切正常，你会在 debugcon 日志里看到 `OPL` 三个字符——'O' 代表磁盘读取成功（Real Mode），'P' 代表进入保护模式，'L' 代表进入 Long Mode。

工具链的特殊约束依然存在：bootloader 以 `-m32` 编译（实际上是 16 位实模式代码嵌入在 32 位目标文件里），内核以 `-m64` 编译。两者共享的 `boot/boot_info.h` 头文件必须确保在两种编译环境下结构体布局完全一致——这一点后面讲到 BootInfo 的时候会展开。

## 第一阶段——BootInfo：bootloader 和内核的"握手协议"

### 为什么需要 BootInfo

在 004A 之前，bootloader 和内核之间没有任何正式的数据交换机制。bootloader 做完自己的事情（E820 探测、VESA 初始化、磁盘读取），然后内核从某个固定地址开始执行，自己跑去 0x5000 读 E820 数据、去 0x6400 读帧缓冲区信息。这种"隐式约定"的做法在小项目里能凑合，但它有三个根本性的问题。

第一，地址硬编码意味着每次改动都可能牵扯多个文件。如果你把 E820 缓冲区从 0x5000 挪到 0x4000，你得同时改 boot.S 的常量定义、内核的地址引用、可能还有 linker script 里的某些符号——任何一处漏改都是一个微妙的 bug。第二，没有版本信息。如果未来 BootInfo 的字段顺序变了或者新增了字段，内核没法知道它读到的是新版本还是旧版本的结构体，只能祈祷两边恰好同步。第三，没有集中的文档入口。所有地址约定散落在各个文件的注释里，新人看代码只能靠 grep 搜索 0x5000、0x6400 这些 magic number 来拼凑全貌。

BootInfo 结构体把所有这些信息集中到一个地方：bootloader 负责填充它，内核负责读取它，地址只有一个——0x7000。这个设计跟 Linux 的 `struct boot_params`、Multiboot 规范的 boot information 结构体在理念上完全一致：用一块固定地址的结构体作为 bootloader 和内核之间的正式契约。

### MemoryMapEntry：24 字节的 E820 条目

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

这个结构体直接对应 E820 BIOS 调用返回的 24 字节内存映射条目格式。base 是物理基地址（64 位，因为物理地址可以超过 4GB），length 是区域长度，type 标识内存类型——1 是可用 RAM，2 是保留区，3 是 ACPI 可回收，4 是 ACPI NVS（Non-Volatile Storage），5 是坏内存。acpi 字段是 ACPI 3.0 引入的扩展属性，老 BIOS 返回 0，新 BIOS 可能在这里标明条目是否支持 NUMA 亲和性之类的信息。Intel SDM Vol.3A §15.3.1 对这些字段有完整的定义。

这里有一个非常容易忽略的关键细节：这个头文件同时被两套编译环境使用。bootloader 的代码以 `-m32` 编译，内核代码以 `-m64` 编译。两套编译器的 ABI 不同——32 位模式下 `uint64_t` 可能需要 8 字节对齐，而 packed 结构体里不做对齐；64 位模式下指针大小是 8 字节，结构体的默认对齐规则也完全不同。如果不用 `__attribute__((packed))`，两边编译出来的结构体布局很可能不一致，bootloader 填充的数据内核读取时字段偏移全错，然后你花一整天调试一个看起来完全不合理的数据损坏。

`static_assert` 是我们的安全网。如果因为某种原因——编译器版本差异、ABI 变化、头文件包含顺序导致 `uint64_t` 定义不同——导致结构体大小不是 24 字节，编译直接报错。注意这里用了条件编译：C++11 用 `static_assert`，C11 用 `_Static_assert`。这是因为 bootloader 里如果将来引入 C 代码（比如 ELF loader，虽然我们现在用了 flat binary 方案不需要了），也得能编译通过。

### BootInfo：三大类启动信息的容器

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

`static_assert(sizeof(BootInfo) == 824, ...)` 确保总大小是 824 字节——4 个 uint64_t（32 字节）+ 6 个 uint32_t（24 字节）+ 32 个 MemoryMapEntry（768 字节）。

### 和 Linux boot_params、Multiboot 的设计对比

BootInfo 这个概念不是我们发明的，几乎所有操作系统都有自己的 bootloader-kernel handoff 机制。我们来深入看看几个知名项目是怎么做的，以及 Cinux 为什么做了不同的选择。

**Linux 的 `struct boot_params`** 是最经典的例子。Linux x86 Boot Protocol 定义了一个庞大的结构体（`struct boot_params`，大约 4096 字节），放在物理地址 0x7000 附近。bootloader（GRUB、systemd-boot 等）负责填充这个结构体，然后通过 `int 0x10/0x13/0x15` 收集硬件信息，最后跳转到内核入口。boot_params 里的字段非常全面：E820 内存映射、VESA 帧缓冲区参数、命令行参数（`cmd_line_ptr`）、initrd 地址和大小、APM/EDD 信息、甚至还有 EFI 系统表指针。Linux 的设计哲学是"bootloader 尽可能多地收集信息，内核只管用"。你可以在 [Linux x86 Boot Protocol](https://www.kernel.org/doc/html/latest/x86/boot.html) 文档里看到完整的字段列表和版本协商机制。

Cinux 的 BootInfo 和 Linux 的 boot_params 在设计理念上一致——固定地址 + 结构体 + 寄存器传指针。但有三个关键区别。第一，Linux 有版本协商：boot_params 的 `hdr.version` 字段让内核知道 bootloader 是哪个版本的协议，字段是否存在取决于版本号。Cinux 目前没有版本信息，因为我们只有一个 bootloader 实现，不存在多版本兼容的需求。第二，Linux 的结构体是按缓存行对齐的（64 字节），有些字段组有专门的偏移约定。Cinux 用 packed 结构体，没有对齐优化——820 字节的数据一次性搬完，对齐对性能的影响在这个阶段可以忽略。第三，Linux 的 cmdline 参数是通过指针引用的（bootloader 把命令行字符串放在内存某个位置，boot_params 里只存指针），而 Cinux 目前没有命令行参数的需求——教学内核不需要 `root=/dev/sda1` 这种东西。

**Multiboot 规范（GRUB）** 走的是另一条路。Multiboot 1 使用 EBX 传递 boot info 指针、EAX 传递魔数 0x2BADB002，boot info 结构体是一个固定头部 + 变长 flag 数组。Multiboot 2 更进一步，使用 tag 链表结构——每个 tag 有 type 和 size 字段，后面跟具体数据，tag 链表以一个 type=0 的终止 tag 结尾。这种设计的好处是可扩展——新增功能只需要定义新的 tag type，旧的 bootloader 和内核可以忽略不认识的 tag。OSDev Wiki 的 [Multiboot](https://wiki.osdev.org/Multiboot) 页面对两种版本都有详细描述。

Cinux 为什么没选 Multiboot 式的 tag 链表？原因很简单：我们不需要可扩展性。Cinux 只有一个 bootloader 实现，BootInfo 的字段在可预见的未来都不会大幅变动。tag 链表带来的解析代码（遍历链表、按 type 查找 tag）对教学项目来说是过度设计。固定布局的结构体更简单——内核直接按偏移读字段就行，不用遍历、不用匹配 type。如果将来 Cinux 要支持 GRUB 引导，我们可以实现一个 Multiboot 到 BootInfo 的转换层，在 GRUB 的 boot info 和我们的 BootInfo 之间做翻译。

**SerenityOS** 的做法又不太一样。SerenityOS 使用自己的 bootloader（也支持 GRUB），通过 Multiboot 协议传递信息。它的内核入口接收 Multiboot 的 boot info 指针，然后自己解析 E820 内存映射和帧缓冲区信息。和 Cinux 的区别在于 SerenityOS 没有定义自己的 handoff 结构体——它直接使用 Multiboot 定义的结构。这种方式更标准化（GRUB 可以直接引导），但也意味着对 Multiboot 规范的依赖——如果你想换一个不支持 Multiboot 的 bootloader，内核的入口代码就得改。

你会发现这些项目在 bootloader-kernel 交接这个问题上做了不同的取舍，但核心思路是一样的：bootloader 在 Real Mode 下收集硬件信息，然后通过某种约定好的数据结构传给内核。固定地址结构体（Cinux/Linux）、寄存器+魔数（Multiboot）、tag 链表（Multiboot 2）只是具体的实现形式不同，本质都是解决同一个问题——bootloader 和内核之间怎么可靠地传递信息。

## 第二阶段——从 4KB 到 416KB：分块磁盘读取

### 从 ELF 到 flat binary 的策略转变

在讲分块读取之前，我们需要先说清楚一个重要的设计变更：004B 把小内核的格式从 ELF 切换成了 flat binary（扁平二进制）。在 004A 里，我们读取的是 ELF header，计划在后续阶段解析 Program Header、遍历 PT_LOAD 段、做重定位和 BSS 清零。但仔细想想，Real Mode 的 16 位环境做不了 64 位算术——ELF64 里的地址字段都是 64 位的，你要在 16 位实模式下用多个寄存器拼接才能读写一个地址。虽然可以在 Protected Mode（32 位）下用 C 写一个 ELF loader，但那意味着 Stage2 里要链接一个 32 位 C 编译的目标文件，构建系统的复杂度一下子就上去了。

flat binary 的思路简单粗暴：用 `objcopy -O binary` 把编译好的 ELF 文件剥掉所有 header、section table、符号表，只保留段的原始二进制内容。输出的第一个字节就是 `.text` 段的第一个字节，也就是 `_start` 的第一条指令。bootloader 只需要把这个二进制文件原封不动地读到目标物理地址，然后在 Long Mode 里跳过去就行了——不需要解析任何格式，不需要做重定位，加载地址和链接地址必须完全一致。

代价是明显的：内核必须链接在固定的物理地址（0x20000），不能做 ASLR，也不能利用 ELF 的段对齐特性。但对于一个教学内核来说，这些限制完全可以接受。而且这个选择只影响 bootloader 到 mini kernel 的加载——将来 mini kernel 加载 big kernel（tag 008）的时候，mini kernel 已经有完整的 C++ 运行环境和内存管理，用 ELF 格式加载 big kernel 就是正确的选择了。bootloader 到 mini kernel 用 flat binary 是"简单"，mini kernel 到 big kernel 用 ELF 是"正确"，两者各取所长。

### BIOS 127 扇区限制和分块策略

004A 读了 8 个扇区（4KB），一次 INT 13h 调用就搞定了。现在我们要读 832 个扇区（416KB），一次读不完——OSDev Wiki 的 [Disk access using the BIOS (INT 13h)](https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)) 页面明确指出，INT 13h AH=42h 扩展读取的单次上限是 127 个扇区（某些 BIOS 实现甚至更低）。这不是 INT 13h 规范本身的硬限制，而是 BIOS 实现中 DMA 传输缓冲区的约束——BIOS 内部需要把 DMA 传输限制在 64KB 边界内，127 个扇区 = 65024 字节，恰好不超过这个限制。

所以我们必须分块读取。832 个扇区按每次 127 个算，需要 7 次循环：6 次读满 127 个扇区（762 个扇区），最后一次读 70 个扇区（832 - 762 = 70）。循环变量 BX 追踪已经读取的扇区数，每次计算 min(127, 剩余扇区数)，然后动态构建 DAP（Disk Address Packet），更新 buffer 地址和 LBA 偏移，调一次 INT 13h，然后 BX 加上本次读取量，继续下一轮。

先看常量定义，这里每一条背后都有故事：

```asm
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set MINI_KERNEL_SECTORS,     832         // Total sectors (416KB)

.set MINI_KERNEL_LOAD_PHYS,   0x20000     // Physical address where kernel is loaded
.set MINI_KERNEL_LOAD_SEG,    0x2000      // Segment: 0x2000 (0x2000 << 4 = 0x20000)
.set MINI_KERNEL_LOAD_OFF,    0x0000      // Offset

.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
.set DISK_MAX_SECTORS_PER_CALL, 127       // Max sectors per BIOS call
```

和 004A 相比最大的变化是加载地址从 0x10000 变成了 0x20000。这个改动背后的故事就是那个让我血压拉满的栈冲突 bug——我们待会儿单独说。扇区数从 8 变成了 832，新增了 `DISK_MAX_SECTORS_PER_CALL = 127`。LBA 还是 16——小内核在磁盘镜像上从第 16 个扇区开始，紧接在 Stage2（扇区 1~15）之后。

### 栈冲突惊魂记：0x10000 到 0x20000

在讲循环逻辑之前，我们得先说清楚为什么加载地址是 0x20000 而不是 0x10000。这不是一开始就定好的——最初的设计就是 0x10000，和 xv6 的 `bootmain.c` 以及 Linux 0.01 的 boot sector 一样。0x10000 是经典的"临时加载区"，几乎所有 x86 bootloader 教程都把数据加载到这里。

但问题是，我们忘了算 Real Mode 栈的位置。

Stage2 在启动时把栈设在 `SS=0x0900, SP=0xFFFE`，这意味着 Real Mode 栈占据的物理地址范围是 0x9000（SS<<4 = 0x9000）到 0x19000（0x9000 + 0xFFFE ≈ 0x19000）。栈是向下生长的，所以从 0x19000 往下压栈，push 的数据写在高地址，新数据往低地址走。当小内核从 0x10000 开始加载时，832 个扇区的数据会从 0x10000 写到 0x10000 + 832*512 = 0x10000 + 0x68000 = 0x78000。看起来 0x78000 远大于栈顶 0x19000，应该没问题？

问题在于 `load_kernel_from_disk` 是一个函数调用。调用前，调用者把返回地址 push 到栈上，栈指针从 0xFFFE 往下移了 2 个字节变成 0xFFFC（物理 0x18FFC）。函数入口又 `pusha` 保存了 8 个通用寄存器（16 字节），再加 `pushw %es` 和 `pushw %ds`（4 字节），栈上现在有 22 字节的数据，物理范围大约在 0x18FEA ~ 0x18FFF。这些数据中最重要的就是返回地址——`call load_kernel_from_disk` 时 CPU 自动 push 的 IP 值，函数末尾 `ret` 要靠它跳回调用者。

现在 INT 13h 开始往 0x10000 写数据了。写啊写啊，写到偏移 0x9000 的时候（也就是第 36 个扇区左右），数据刚好覆盖了 0x19000 附近的栈区域。返回地址被磁盘数据覆盖了。等 `load_kernel_from_disk` 执行到 `ret` 的时候，pop 出来的 IP 是磁盘上某个随机的字节，CPU 跳到一个莫名其妙的地址，大概率触发 triple fault 然后重启。

这个 bug 的症状非常诡异：在 QEMU 里它不一定每次都触发（取决于栈的精确位置和磁盘数据恰好覆盖了哪里），在 Bochs 里更容易触发。用 GDB 单步跟踪时你会发现 `ret` 之后 RIP 变成了一个完全意料之外的值，然后 CPU 就 triple fault 了。说实话，这个问题真的坑了我半天——直到用 QEMU monitor 手动 `xp /4bx 0x18FFC` 检查才发现栈顶的数据已经被覆盖成了磁盘上的 0x7F（ELF magic 的第一个字节），才意识到是加载地址和栈冲突了。

解决方案是把加载地址提高到 0x20000。关键不等式是：`MINI_KERNEL_LOAD_PHYS > (SS << 4 + SP)` → `0x20000 > 0x19000`。安全间隙 = 0x20000 - 0x19000 = 0x7000 = 28KB。这个间隙足够覆盖任何栈操作可能触及的范围——即使函数调用嵌套了好几层，28KB 的空间也绑绰绰有余。

接下来问题来了：Protected Mode 的栈在 0x90000（Stage2 里 `movl $0x90000, %esp` 设置的），小内核最大 416KB，加载到 0x20000~0x88000，离 Protected Mode 栈只有 0x90000 - 0x88000 = 0x8000 = 32KB 的间隙。这个间隙虽然够用，但我们要在 build_image.sh 里加上大小限制检查——如果小内核超过 416KB，构建直接失败，而不是等到运行时才发现数据写到了栈区。

### 分块循环读取的完整逻辑

现在我们来看 `load_kernel_from_disk` 的循环本体。先说入口：

```asm
load_kernel_from_disk:
    pusha                                // save all 16-bit registers
    pushw %es                            // save ES
    pushw %ds                            // save DS

    xorw %bx, %bx                        // BX = 0 (no sectors read yet)
    xorw %si, %si                        // SI = 0 (stays 0, used for DAP offset)

    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %es                        // ES points to DAP area
```

函数入口保存所有寄存器。BX 是我们的核心循环变量——"已经读了多少扇区"，初始化为 0。SI 固定为 0，后面每次循环都用它指向 DAP 的偏移地址。ES 指向 DAP 所在的段 0x07B0（物理 0x7B00）。

然后进入循环：

```asm
.read_loop:
    cmpw $MINI_KERNEL_SECTORS, %bx       // Compare with total
    jae .read_done                       // Done if BX >= 832

    movw $MINI_KERNEL_SECTORS, %ax       // AX = 832 (total)
    subw %bx, %ax                        // AX = remaining sectors
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax // Compare with 127
    jbe .read_count_ok                   // Use AX if <= 127
    movw $DISK_MAX_SECTORS_PER_CALL, %ax // Cap at 127

.read_count_ok:
    movw %ax, %bp                        // BP = sectors to read this round
```

每次循环开始先检查是否读完——BX >= 832 则退出。然后计算本次读取量：`min(127, remaining)`。前几次循环 BX 还小，remaining 远大于 127，所以每次读 127 个扇区。最后几轮 remaining 小于 127，按实际剩余量读取。BP 暂存本次读取量。

接下来构建 DAP——每次循环都要重新填写，因为 buffer 地址和 LBA 都在变：

```asm
    // CRITICAL: Re-set ES before building DAP (BIOS may clobber it)
    movw $DAP_SEGMENT, %dx
    movw %dx, %es                        // ES = 0x07B0 (DAP segment)

    movw $DAP_OFFSET, %di                // DI = 0x0000

    movb $16, %es:(%di)                  // DAP size = 16
    movb $0, %es:1(%di)                  // DAP reserved = 0
    movw %bp, %es:2(%di)                 // DAP count = BP (sectors this round)
```

这里有一个很容易翻车的细节——每次循环都必须重新设置 ES 为 DAP 段地址。为什么？因为 INT 13h 调用可能破坏 ES 的值。BIOS 内部实现不保证保留调用者的段寄存器，如果你偷懒只设置一次 ES 然后循环复用，第二轮开始 ES 已经不是 0x07B0 了，你往 `es:(%di)` 写的 DAP 字段全部写到了错误的位置。这个 bug 在 QEMU 里可能不触发（SeaBIOS 实现比较规矩），但在真实硬件或者 Bochs 里几乎必现。

另外一个细节是 `movb $0, %es:1(%di)` 显式清零 reserved 字段。004A 没有这行，因为当时只构建一次 DAP、内存碰巧是干净的。现在我们循环复用同一个 DAP 位置，必须确保每次迭代都从干净状态开始——不然上一轮的残留数据可能让某些 BIOS 实现（特别是那些严格检查 reserved 字段的）返回错误。

### buffer 地址的动态计算

真正的坑在后面——buffer 地址的计算。每一轮循环的目标物理地址都在变：第 0 轮写 0x20000，第 1 轮写 0x20000 + 127*512 = 0x2FE00，第 2 轮写 0x20000 + 254*512 = 0x3FC00，以此类推。但 Real Mode 用的是 segment:offset 格式，不是线性地址。我们需要把"第 BX 个扇区对应的物理地址"转换成段地址格式：

```asm
    // Target physical address = 0x20000 + BX * 512 = 0x20000 + (BX << 9)
    // Segment = physical >> 4 = (0x20000 + (BX << 9)) >> 4
    //         = 0x2000 + (BX << 5) = 0x2000 + BX * 32
    movw %bx, %ax                        // AX = sector offset
    shlw $5, %ax                         // AX *= 32
    addw $MINI_KERNEL_LOAD_SEG, %ax      // AX = 0x2000 + BX*32 = target segment
    movw %ax, %es:6(%di)                 // buffer segment
    movw $MINI_KERNEL_LOAD_OFF, %ax      // AX = 0 (offset always 0)
    movw %ax, %es:4(%di)                 // buffer offset
```

这段代码的核心是那个位移计算。Real Mode 的地址公式是 `物理地址 = segment << 4 + offset`。我们的策略是让 offset 始终为 0，把所有偏移编码到 segment 里。推导过程是这样的：目标物理地址 = 0x20000 + BX * 512。除以 16（右移 4 位）得到 segment = 0x2000 + BX * 512 / 16 = 0x2000 + BX * 32。所以代码里 BX 左移 5 位（相当于乘 32）再加上基础段 0x2000，一步到位。

你会发现这个计算有一个隐含的假设：BX * 32 的结果不超过 16 位。BX 最大值是 832（832 * 32 = 26624 = 0x6800），确实在 16 位范围内。但如果将来内核大小超过这个范围，这个计算就会溢出——不过 build_image.sh 的大小限制检查会在编译期就拦住。

然后是 LBA 的填写：

```asm
    movw $MINI_KERNEL_LBA, %ax           // AX = 16 (base LBA)
    addw %bx, %ax                        // AX = 16 + BX = current LBA
    movw %ax, %es:8(%di)                 // LBA low 16 bits
    xorw %ax, %ax                        // AX = 0
    movw %ax, %es:10(%di)                // LBA bits 16-31
    movw %ax, %es:12(%di)                // LBA bits 32-47
    movw %ax, %es:14(%di)                // LBA bits 48-63 (must be zero!)
```

当前 LBA = 基础 LBA（16）+ 已读扇区数（BX）。LBA 是 DAP 里的 64 位字段（8 字节），虽然我们的 LBA 值很小（最大 848），但高 48 位必须显式清零。这是 004A 里就踩过的坑——DAP 的高位 LBA 如果有残留的垃圾值，BIOS 会尝试读取一个巨大 LBA，直接返回错误。每轮循环我们只更新低 16 位，高 48 位全部写零，确保万无一失。

### BX/BP 寄存器保护和 BIOS 调用

接下来是整个函数里最需要注意的部分——BIOS 调用前后的寄存器保护：

```asm
    // CRITICAL: Save BX and BP across BIOS call
    pushw %bx                            // save sector counter
    pushw %bp                            // save sector count

    // INT 13h AH=42h requires DS:SI to point to DAP, NOT ES:SI!
    movw $DAP_SEGMENT, %dx
    movw %dx, %ds                        // DS = 0x07B0
    movw $DAP_OFFSET, %si                // SI = 0x0000
    movb $0x80, %dl                      // DL = 0x80 (first hard disk)
    movb $DISK_READ_CMD, %ah             // AH = 0x42

    int $0x13
```

这里有三件事情，每一件都够你调试一个下午。

第一，BX 和 BP 必须在 BIOS 调用前压栈保存。INT 13h 的实现不保证保留这两个寄存器——很多 BIOS 实现会把 BX 用作内部循环计数器，把 BP 用作临时存储。如果我们不保存，调用返回后 BX 和 BP 里的值已经不知道变成什么了，循环计数直接乱套，要么死循环要么少读一段数据。说实话这个坑在 QEMU 里不一定触发（SeaBIOS 可能恰好没碰 BX 和 BP），但在真实硬件上几乎必现。

第二，INT 13h AH=42h 要求 DS:SI 指向 DAP，不是 ES:SI。这跟大多数 BIOS 调用的 ES:DI 约定不同——E820 用 ES:DI，INT 10h（VESA）也用 ES:DI，但 INT 13h 扩展读取偏偏用 DS:SI。搞反了 BIOS 不会报错但会从错误的位置读取 DAP，然后要么读到垃圾数据导致读取失败，要么读到恰好看起来合法的数据然后读取到错误的地址。代码里 `movw %dx, %ds` 把 DS 设为 0x07B0，然后 SI 设为 0。

第三，DL 必须设为 0x80（第一块硬盘）。如果你忘了设（或者 DL 里恰好还是上一次调用的残留值），BIOS 会尝试从不存在的驱动器读取，返回 AH=0x01（Invalid Function）。004A 里详细讲过这个坑——我们曾在 E820 调用后直接调 INT 13h，DL 里还是 E820 留下的残留值，结果 BIOS 返回错误但数据碰巧是对的，折腾了半天才发现问题。

调用后的处理分两条路径：

```asm
    jc .disk_error_restore_bp            // CF=1 means failure
    cmpb $0, %ah                         // AH should be 0
    jne .disk_error_restore_bp           // AH!=0 means error

    // Success path: restore BP and BX
    popw %bp                             // restore sector count
    popw %bx                             // restore sector counter
    addw %bp, %bx                        // BX += sectors read
    jmp .read_loop                       // Continue reading

.disk_error_restore_bp:
    popw %bp                             // restore (stack: BP then BX)
.disk_error_restore:
    popw %bx
    jmp disk_read_failed                 // handle error
```

成功时弹出 BP 和 BX，把 BP（本次读取量）加到 BX 上，继续循环。错误时也必须先弹出 BP 和 BX——栈上有两个 word（先压 BX 后压 BP，所以栈顶是 BP），如果不恢复栈指针就直接跳走，后续的 DS/ES 恢复会从错误的位置弹出，寄存器全部乱掉。注意错误路径分成了两步 `.disk_error_restore_bp` 和 `.disk_error_restore`，这不是代码冗余——先弹 BP 再弹 BX，保证栈平衡。

循环正常退出后：

```asm
.read_done:
    popw %ds                             // restore DS
    popw %es                             // restore ES
    popa                                 // restore general registers

    movb $'O', %al                       // 'O' for disk OK
    outb %al, $0xe9                      // debugcon output

    movw $MINI_KERNEL_SECTORS, %ax       // return 832
    ret
```

恢复所有寄存器后，输出字符 'O' 到 debugcon——这是我们的调试约定，'O' 代表"磁盘读取 OK"。AX 返回 832（读取的总扇区数），作为返回值交给调用者。

错误处理也加上了 debugcon 输出以区分不同的失败类型：

```asm
disk_read_failed:
    movb $'F', %al                       // 'F' for Failed
    outb %al, $0xe9
    popw %ds
    popw %es
    popa
    movw $(msg_disk_read_failed), %si
    jmp panic

disk_too_large:
    movb $'T', %al                       // 'T' for Too large
    outb %al, $0xe9
    popw %ds
    popw %es
    popa
    movw $(msg_disk_too_large), %si
    jmp panic
```

'F' 代表磁盘读取失败，'T' 代表内核太大。这三个 debugcon 字符（'O'/'F'/'T'）配合 Stage2 中的 'P'（Protected Mode）和 'L'（Long Mode），构成了一条完整的启动状态链。

### 和 xv6 磁盘读取策略的深度对比

到这里代码讲完了，我们退后一步看看 Cinux 的分块读取和其他操作系统的磁盘读取策略有什么本质区别。这个对比不止是"谁用了什么函数"——而是为什么不同的项目做了不同的选择。

**xv6** 的 `bootmain.c` 走的是完全不同的路线。xv6 在 32 位 Protected Mode 下用 IDE PIO 直接操作端口 0x1F0~0x1F7 读取磁盘，不依赖任何 BIOS 中断。它的 `readseg()` 函数调用 `waitdisk()` 等待磁盘就绪（轮询 0x1F7 端口的 BSY 和 DRDY 位），然后往端口 0x1F2~0x1F7 写入读取命令（LBA 地址、扇区数、读取命令 0x20），最后用 `insl` 从数据端口 0x1F0 读入数据。整个流程不依赖 BIOS，进入保护模式后照样能用，而且完全可控。

这种方案的好处是灵活——你在保护模式下可以做 32 位算术，可以解析 ELF header，可以遍历 Program Headers 逐段加载到任意地址。xv6 的 `bootmain()` 就是这么做的：先读 ELF header 到 0x10000，验证 magic `\x7FELF`，然后遍历 Program Headers，对每个 PT_LOAD 段调用 `readseg()` 加载到 `ph->paddr`，最后 BSS 清零（`stosb`）并跳转到 `elf->entry`。整个流程在 C 里完成，代码清晰易读。

但代价也很大——你得自己实现磁盘驱动。虽然 IDE PIO 相对简单（就是往几个端口读写数据），但它只支持 IDE/SATA 硬盘，遇到 NVMe 或者 USB 启动就不灵了。xv6 作为教学操作系统只在 QEMU 里跑，QEMU 模拟的是 IDE 磁盘，所以没问题。但如果你想在一个现代笔记本上从 U 盘启动 xv6，IDE PIO 方案直接废了。

Cinux 在 Real Mode 下用 INT 13h 的选择是另一端的权衡。好处是 BIOS 帮你屏蔽了所有磁盘控制器的差异——不管底层是 IDE、SATA、NVMe 还是 USB，INT 13h 的调用方式都一样。代价是你只能在 Real Mode 下用这个接口，而且 BIOS 的实现质量参差不齐（127 扇区限制就是其中之一），分块循环读取增加了代码复杂度。但总体来说，对于 Real Mode 阶段必须完成的磁盘读取工作，INT 13h 是最省事的方案——几行汇编填 DAP 调一次中断，比写一个完整的 IDE PIO 驱动简单得多。

**Linux** 早期（0.01）用的方案和 Cinux 更像——也是在 Real Mode 下用 INT 13h 读取内核。但 Linux 用的是 INT 13h AH=0x02（CHS 模式），不是 Cinux 用的 AH=0x42（LBA 模式）。CHS 需要你自己算柱面/磁头/扇区，最大只支持 8GB 磁盘。现代 Linux 的 Bootloader（GRUB/systemd-boot）当然早就不用 CHS 了，它们在 Real Mode 下用 INT 13h AH=42h 读取内核到内存，然后切换保护模式跳转——和 Cinux 的策略非常相似。

你会发现一个有趣的趋势：简单的教学项目（Cinux、早期 Linux）倾向于在 Real Mode 下用 BIOS 做磁盘读取，而更复杂的项目（xv6、现代 Linux bootloader）要么在保护模式下自己实现磁盘驱动，要么用 Multiboot 协议让 GRUB 代劳。这种选择不是偶然的——BIOS INT 13h 在 Real Mode 下是"够用"的，但一旦你需要更大的灵活性（读取超过 1MB 的数据、在保护模式下读取、解析 ELF），就不得不抛弃 BIOS 自己动手了。

## 第三阶段——Protected Mode：优雅的"无操作"

我们回头看 Stage2 里 `load_kernel_from_disk` 的调用位置：

```asm
    call query_memory_map           // [->0x5000] E820 memory map
    call load_kernel_from_disk      // [->0x20000] Load mini kernel (416KB, 832 sectors)
    cli                             // disable interrupts
```

调用位置和 004A 完全相同——在 VESA 之后、保护模式切换之前。`cli` 紧跟其后，因为接下来的 GDT 加载和模式切换是临界区。832 个扇区（416KB）的读取全部在这里完成，一口气读完。

然后是 Protected Mode 入口：

```asm
.code32
pm_entry:
    movw $0x10, %ax               // Data selector
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    movw %ax, %ss

    movl $0x90000, %esp           // New stack in protected mode

    movb $0x50, %al               // 'P'
    outb %al, $0xE9               // Debugcon output

    // ============================================================
    // 004_boot_load_mini_kernel_B: No operation needed
    // ============================================================
    // Mini kernel bin was already loaded to 0x20000 in real mode.
    // Nothing to do in protected mode, proceed to long mode.

    call setup_page_tables
    call enter_long_mode
```

你会发现 Protected Mode 入口和 004A 相比只多了一块注释——"No operation needed"。段寄存器设置、栈初始化、debugcon 输出、页表建立、Long Mode 切换——这些全都是之前就有的代码。004B 没有在 Protected Mode 里新增任何一条指令。

这个"无操作"设计背后的原因是多方面的。首先是架构纯粹性——Intel SDM Vol.3A §9.1 定义的处理器状态转换流程是 Real Mode → Protected Mode → Long Mode，每一层都有明确的职责。Real Mode 负责 BIOS 调用和硬件初始化，Protected Mode 负责设置 32 位环境（段寄存器、栈），Long Mode 负责建立 64 位执行环境。如果我们在 Protected Mode 里混入磁盘读取或数据搬运的操作，就打破了这种清晰的分层。

其次是内存布局一致性。小内核已经通过 flat binary 方式加载到了 0x20000——它在磁盘镜像上的位置和在内存里的位置完全对应，不需要任何重定位或搬移。如果 Protected Mode 里要做 ELF 解析，那确实需要在这个阶段做额外工作（读 Program Header、搬移段、清零 BSS）。但我们选了 flat binary，这些工作全部省掉了。

最后是 BIOS 中断的限制。保护模式下 IVT 被 IDT 替换，INT 13h 不再可用。如果要在保护模式下读磁盘，就得像 xv6 那样自己实现 IDE PIO 驱动。对于一个教学 bootloader 来说，在 Real Mode 下用 BIOS 一次性读完所有数据，然后在保护模式和 Long Mode 里不做任何磁盘操作，是最简单的方案。

## 第四阶段——Mini Kernel 重生

### boot.S：从死循环到真正的内核入口

004A 的小内核是一个 `cli; hlt` 死循环，唯一的意义是让构建系统有一个可编译的 ELF 文件。004B 给它加上了真正的入口代码，让它成为一个能做事情的程序。

先看汇编入口 `kernel/mini/arch/x86_64/boot.S`：

```asm
.section .text.start, "ax"
.code64

.global _start
.type _start, @function

_start:
    cli

    /* Enable SSE */
    movq %cr4, %rax
    orq $(1 << 9), %rax          /* OSFXSR */
    orq $(1 << 10), %rax         /* OSXMMEXCPT */
    movq %rax, %cr4
    clts                          /* Clear CR0.TS */

    movb $0x31, %al              /* '1' */
    outb %al, $0xE9
```

入口第一件事是关中断——这是内核入口的标准操作，因为在跳转到内核之前 bootloader 的中断状态是不确定的。然后启用 SSE——设置 CR4 的 OSFXSR（bit 9）和 OSXMMEXCPT（bit 10）位，并清零 CR0 的 TS（Task Switched）位。Intel SDM Vol.3A §9.1 明确指出，操作系统在进入 Long Mode 后如果想要使用 SSE/AVX 指令，必须先设置这些控制位。如果不设置，任何 SSE 指令都会触发 #UD（Undefined Opcode）异常。这一步很容易被忽略——C++ 编译器可能在任何地方生成 SSE 指令（比如 `rep stosb` 在某些情况下会被优化成 SSE 版本的 memset），如果你的内核入口没有启用 SSE，你会在一个看起来完全无关的地方 triple fault。

然后是栈和 BSS 清零：

```asm
    /* Setup stack */
    movq $__mini_stack_top, %rsp
    xorq %rbp, %rbp

    /* Save BootInfo pointer BEFORE clearing BSS */
    movq %rdi, __boot_info_ptr

    /* Clear BSS section */
    movq $__bss_start, %rdi
    movq $__bss_end, %rcx
    subq %rdi, %rcx
    xorq %rax, %rax
    rep stosb
```

这里有一个非常重要的操作顺序：**先保存 BootInfo 指针，再清零 BSS**。BootInfo 指针是通过 RDI 寄存器传入的（System V AMD64 ABI 的第一个参数），但 BSS 清零用的是 `rep stosb`，这个指令会把 RDI 当作目标地址——清零完成后 RDI 已经指向了 BSS 段末尾，原来的 BootInfo 指针就丢了。所以必须在 `rep stosb` 之前把 RDI 的值保存到一个全局变量 `__boot_info_ptr` 里。这个全局变量放在 `.data` 段而不是 `.bss` 段——因为 BSS 清零会把 `.bss` 段的变量清零，BootInfo 指针就又丢了。

BSS 清零本身是 C/C++ 标准的要求——未初始化的全局变量和静态变量必须初始化为 0。ELF 格式的可执行文件在加载时由加载器负责 BSS 清零，但 flat binary 不包含这个信息，所以我们必须手动做。`__bss_start` 和 `__bss_end` 是链接脚本导出的符号，标记 BSS 段的起止位置。

```asm
    /* Call global constructors (C++ runtime) */
    call _init_global_ctors

    /* Call C++ main */
    movq __boot_info_ptr, %rdi   /* first argument: BootInfo* */
    call mini_kernel_main

.halt:
    cli
    hlt
    jmp .halt
```

清零 BSS 之后调用 `_init_global_ctors` 执行 C++ 全局构造函数——如果你的内核有全局对象（比如 `static SerialPort serial;`），它们的构造函数必须在这里被调用。然后从 `__boot_info_ptr` 读回 BootInfo 指针，作为第一个参数传给 `mini_kernel_main`。如果 `mini_kernel_main` 返回了（理论上不应该），就进入 `cli; hlt` 死循环。

栈和 BootInfo 指针的存储位置：

```asm
.section .data
.global __boot_info_ptr
.align 8
__boot_info_ptr:
    .quad 0

.section .bss
.align 16
.global __mini_stack
.global __mini_stack_top

.set MINI_STACK_SIZE, 0x2000    /* 8KB */

__mini_stack:
    .skip MINI_STACK_SIZE
__mini_stack_top:
```

8KB 的栈空间对当前的极简内核绑绰有余。`__mini_stack_top` 指向栈顶（x86 栈向下增长），`__boot_info_ptr` 放在 `.data` 段确保不会被 BSS 清零覆盖。这两个符号都是全局的，供 C++ 代码通过 `extern` 引用。

### linker.ld：物理地址从 0x200000 到 0x20000

链接脚本从 004A 的 0x200000（2MB）改到了 0x20000（128KB）：

```
KERNEL_PHYS_BASE = 0x20000;
KERNEL_Virt_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text.start)        /* _start must be first! */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_Virt_BASE) {
        *(.data .data.*)
    }

    .init_array : AT(ADDR(.init_array) - KERNEL_Virt_BASE) {
        __init_array_start = .;
        KEEP(*(.init_array .init_array.*))
        __init_array_end = .;
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
}
```

物理起始地址 0x20000 是本章所有改动的锚点。链接地址和加载地址必须完全一致——如果链接脚本写 0x20000 但 bootloader 把数据加载到了 0x30000，所有绝对地址引用（函数调用、全局变量访问）全部指向错误的内存位置。`.text.start` 段排在最前面，确保 `_start` 是输出文件的第一个符号——flat binary 的第一个字节就是入口点，没有 ELF header 帮你找 entry point。

VMA（虚拟地址）设为 `0xFFFFFFFF80020000`（高半核），LMA（加载地址）设为 `0x20000`。`AT(ADDR(.text) - KERNEL_Virt_BASE)` 这个表达式告诉链接器：虽然代码的虚拟地址在 0xFFFFFFFF80000000 以上，但它应该被加载到物理地址 0x20000。Long Mode 的页表会同时映射 identity mapping（0x20000 → 0x20000）和 higher-half mapping（0xFFFFFFFF80020000 → 0x20000），所以 CPU 用哪个地址都能访问到同一块物理内存。

`.init_array` 段保留了 C++ 全局构造函数的指针数组。`__init_array_start` 和 `__init_array_end` 标记数组的起止位置，`_init_global_ctors` 函数会遍历这个数组依次调用每个构造函数。BSS 段导出了 `__bss_start` 和 `__bss_end` 供 boot.S 使用。

### flat binary 生成：objcopy -O binary

构建流程的最后一步是把 ELF 可执行文件转成 flat binary。这一步在 CMakeLists.txt 的 POST_BUILD 命令里完成：

```cmake
add_custom_command(TARGET mini_kernel
    POST_BUILD
    COMMAND objcopy -O binary $<TARGET_FILE:mini_kernel>
        $<TARGET_FILE_DIR:mini_kernel>/mini_kernel.bin
    COMMENT "Converting mini kernel to flat binary"
    VERBATIM
)
```

`objcopy -O binary` 做的事情非常直接：剥离所有 ELF header、section header table、符号表、重定位信息，只保留 LOAD 段的原始二进制内容。输出文件的第一个字节就是 `.text.start` 段的第一个字节——`_start` 的第一条指令 `cli`（0xFA）。这个文件会被 build_image.sh 写到磁盘镜像的 LBA 16 位置，然后 boot loader 的 `load_kernel_from_disk` 把它原封不动地读到物理 0x20000。

值得注意的是，链接器本身仍然需要 ELF 格式来工作——它需要解析符号、处理重定位、计算地址。flat binary 是最终产物，不是中间产物。所以编译流程是：源代码 → ELF 可执行文件（链接器输出）→ flat binary（objcopy 转换）→ 磁盘镜像（dd 写入）。

### 构建脚本更新

`build_image.sh` 的关键变更是使用 flat binary 替代 ELF 文件，并加上了严格的大小限制检查：

```bash
MINI_BIN=${3:-${BUILD_DIR}/kernel/mini/mini_kernel.bin}

MINI_KERNEL_MAX_BYTES=$((416 * 1024))    # 425984 bytes
MINI_KERNEL_MAX_SECTORS=$((MINI_KERNEL_MAX_BYTES / 512))  # 832 sectors

if [ $MINI_SIZE -gt $MINI_KERNEL_MAX_BYTES ]; then
    log_error "Mini kernel too large!"
    log_error "  Actual:   $MINI_SIZE bytes ($MINI_SECTORS sectors)"
    log_error "  Maximum:  $MINI_KERNEL_MAX_BYTES bytes (416KB, 832 sectors)"
    echo ""
    log_error "Memory layout constraints:"
    log_error "  - Real mode stack:       0x9000 ~ 0x19000"
    log_error "  - Kernel load area:      0x20000 ~ 0x88000 (416KB max)"
    log_error "  - Protected mode stack:  0x90000"
    exit 1
fi
```

416KB 这个上限不是拍脑袋定的，而是来自内存布局的硬约束。加载区域是 0x20000~0x88000（0x20000 + 416KB），再往上就是 Protected Mode 栈（0x90000）减去 32KB 安全间隙的上限。如果小内核超过这个大小，构建直接失败并打印详细的错误信息和内存布局约束说明——这种 fail-fast 的设计比运行时才发现数据写到了栈区要好得多。

磁盘镜像的组装和 004A 类似，依次写入 MBR（扇区 0）、Stage2（扇区 1~15）、小内核 flat binary（扇区 16 开始）。`conv=notrunc` 确保 dd 不会截断输出文件。

## 收尾

来验证一下本章的成果。构建并运行 QEMU，检查 debugcon 日志文件。如果一切正常，你应该看到三个字符：`OPL`。'O' 是 `load_kernel_from_disk` 在 Real Mode 下成功读完 832 个扇区后输出的，'P' 是 Stage2 进入 Protected Mode 后输出的，'L' 是进入 Long Mode 后输出的。这三个字符构成了一条完整的启动状态链——任何一个字符缺失都意味着对应的阶段出了问题。

如果你想更深入地验证小内核确实被正确加载到了 0x20000，可以在 QEMU 里用 GDB 连接后执行 `x/10i 0x20000`——你应该能看到 boot.S 的前几条指令（`cli`、CR4 操作等），而不是 004A 时代的 ELF magic `0x7F 0x45 0x4C 0x46`。如果你看到的还是 ELF magic，说明 build_image.sh 写入的是 ELF 文件而不是 flat binary——检查 CMakeLists.txt 的 POST_BUILD 步骤是否正确生成了 mini_kernel.bin。

回过头看，这一章我们做了四件事。第一，定义了 BootInfo 结构体作为 bootloader 和内核的正式交接契约——它把 E820 内存映射、VESA 帧缓冲区信息、内核入口和物理基址打包成一个 824 字节的 packed 结构体，放在物理 0x7000。第二，把磁盘读取从 8 扇区扩展到 832 扇区，用循环分块读取绕过 BIOS 的 127 扇区限制，每次动态计算 buffer 段地址和 LBA 偏移。第三，解决了栈冲突 bug——加载地址从 0x10000 提高到 0x20000，避开 Real Mode 栈，留出 28KB 安全间隙。第四，给小内核加上了完整的入口代码——boot.S 设置栈、启用 SSE、保存 BootInfo 指针、清零 BSS、调用全局构造函数、跳转到 C++ main 函数。

踩的坑主要是三个：栈冲突（0x10000 覆盖 Real Mode 栈导致 ret 跳飞）、BX/BP 寄存器被 BIOS 破坏（循环计数乱套）、以及 ES 段地址在循环中必须每次重设（BIOS 不保证保留调用者的段寄存器）。每一个都是那种"知道了觉得理所当然、不知道时能调一整天"的坑。

下一篇（tag 004C）我们将完成最后的拼图——在 Long Mode 里填充 BootInfo 结构体（把 E820 数据从 0x5000、VESA 数据从 0x6400 搬到 0x7000 的 BootInfo 里），然后跳转到小内核的入口地址，让那个 'M' 字符真正出现在 debugcon 日志里。到那一刻，bootloader 的工作才算真正完成——从 MBR 到 Long Mode 再到内核入口，整条启动链全部打通。

## 参考资料

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 9.1 — Processor Management and Initialization。定义了 Real Mode → Protected Mode → Long Mode 的状态转换流程，以及各模式下控制寄存器的设置要求（包括 CR4.OSFXSR 和 CR4.OSXMMEXCPT 用于启用 SSE）。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- Intel 64 and IA-32 Architectures Software Developer's Manual, Vol.3A, Section 15.3.1 — INT 15h, EAX=E820h: Query System Address Map。定义了 E820 返回的内存映射条目格式（base 8B + length 8B + type 4B + acpi 4B = 24B），以及 type 值的含义（1=可用 RAM，2=保留，3=ACPI 可回收等）。
  https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

- OSDev Wiki — Disk access using the BIOS (INT 13h): INT 13h AH=42h 扩展读取的完整规范。DAP 16 字节结构定义、DS:SI 指向 DAP 的要求、每次最多 127 扇区的限制、DL=0x80 硬盘约定。
  https://wiki.osdev.org/Disk_access_using_the_BIOS_(INT_13h)

- OSDev Wiki — Rolling Your Own Bootloader: 自写 bootloader 的综合指南。关于选择加载地址（避开栈区和 BIOS 数据区）、flat binary vs ELF 的取舍讨论、Real Mode 下完成所有 BIOS 依赖操作的建议。
  https://wiki.osdev.org/Rolling_Your_Own_Bootloader

- OSDev Wiki — Multiboot: Multiboot/Multiboot2 规范定义的 bootloader-kernel 交接设计。Multiboot 1 使用固定头部+flag 数组，Multiboot 2 使用 tag 链表（更灵活可扩展）。Cinux 的 BootInfo 借鉴了其思路但简化为固定布局。
  https://wiki.osdev.org/Multiboot

- Linux x86 Boot Protocol: Linux 内核的 bootloader-kernel 交接协议。`struct boot_params` 放在物理 0x7000 附近，包含 E820 内存映射、VESA 帧缓冲区参数、命令行参数、initrd 地址等字段。设计理念与 Cinux BootInfo 相同——固定地址 + 结构体 + 寄存器传指针，但 Linux 有版本协商机制和更完善的字段定义。
  https://www.kernel.org/doc/html/latest/x86/boot.html

- xv6 bootmain.c (MIT): xv6 在 32 位保护模式下用 IDE PIO 直接端口 I/O（0x1F0~0x1F7）读取磁盘的参考实现。ELF header 读到 0x10000，遍历 Program Headers 逐段加载到 `ph->paddr`，BSS 清零然后跳转 entry。与 Cinux 的 Real Mode + BIOS INT 13h 方案形成对比——xv6 自实现磁盘驱动但可在保护模式运行，Cinux 依赖 BIOS 但仅限 Real Mode。
  https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c
