---
title: 004-boot-load-mini-kernel-b-1 · 内核加载 (B)
---

# 004 加载小内核（BootInfo 结构定义篇） -- 定义 bootloader 和内核的"交接契约"

> 本章完成后的可见效果：头文件编译通过，`static_assert` 在 32 位和 64 位编译下均通过
>
> 前置要求：已完成 `004_boot_load_mini_kernel_A`，理解 E820 内存探测和磁盘读取基础

---

## 导语

上一章我们用 E820 画好了内存地图，用 INT 13h 把 Mini Kernel 的前缀读到了 0x10000——但说实话，4KB 的 ELF header 只够让 Bootloader "确认文件在磁盘上"，离真正把内核跑起来还差十万八千里。从这一章开始，我们要逐步完成"加载完整小内核并跳转"这个目标。

这一篇聚焦于第一件事：定义 BootInfo 结构体。它是 bootloader 和内核之间的"交接契约"——bootloader 把所有关键信息打包进去，内核启动时拿到这个结构体就知道世界长什么样。这个设计不是我们凭空发明的，Linux 有 `struct boot_params`、Multiboot 规范有 boot information 结构体，核心思路都是一样的：用一块固定地址的结构体作为 bootloader 和内核之间的正式契约。BootInfo 的设计借鉴了它们的经验，但做了适合教学内核的简化——没有版本协商机制（我们只有一个 bootloader 实现），没有 tag 链表（固定布局更简单），没有命令行参数支持（教学内核不需要 `root=/dev/sda1`）。

更重要的是，这个结构体会被 bootloader（16/32 位汇编）和内核（64 位 C++）共同使用，所以它的布局必须在两种编译模式下完全一致。这是本章最关键的设计约束——如果布局不一致，内核读到的就是垃圾数据，而且这种 bug 不会在编译期报错，只有运行时才会暴露。

**这一步之后，我们就能：**
- 用一个统一的数据结构在 bootloader 和内核之间传递信息
- 确保 32 位和 64 位编译模式下数据布局一致
- 为后续完整的内核跳转铺好路

---

## 一、概念精讲

### 1.1 BootInfo 结构体：为什么"散落的数据"不够用

你可能会想：上一章不是已经把 E820 数据放到 0x5000、framebuffer 信息放到 0x6400 了吗？为什么还要再搞一个 BootInfo？这是因为"数据散落在固定地址"这个方案在小项目里勉强能用，但随着内核功能增长，你需要的信息会越来越多——内核入口地址、内核物理基址、内核大小、内存地图、framebuffer 参数……如果这些信息各自蹲在不同的固定地址上，内核启动时就得一个个去"寻宝"，地址改了一个就全部乱套。更麻烦的是缺乏自描述性——内核怎么知道 E820 有几条记录？framebuffer 在哪？这些信息如果没有一个统一的结构来管理，每加一项功能就得在 bootloader 和内核两边同步修改地址常量。

BootInfo 的思路是：定义一个统一的结构体，Bootloader 在跳转内核之前把所有信息打包进去，然后把结构体的指针通过寄存器（System V AMD64 ABI 用 RDI）传给内核。内核拿到指针后，一个结构体就能访问所有启动信息。这种方式的好处是集中管理——改地址只需要改 BootInfo 的填充代码和读取代码，不需要满世界搜索 magic number。

BootInfo 里的字段可以分成三类。第一类是内核自身的信息：入口点的虚拟地址（entry_point）、内核被加载到的物理基址（kernel_phys_base）、内核的实际大小（kernel_size）。第二类是显示相关的信息：framebuffer 的物理地址、宽度、高度、每行字节数（pitch）、色深（bpp）——这些来自 VESA 初始化。第三类是内存地图：一个计数字段（mmap_count）加上最多 32 条 E820 记录的数组，每条记录 24 字节。整个 BootInfo 大约 824 字节，放在物理地址 0x7000。

### 1.2 跨编译环境一致性：为什么必须 packed + static_assert

这里有一个非常重要的设计约束：BootInfo 的头文件会被 Bootloader（-m32 编译）和内核（-m64 编译）同时包含。32 位编译和 64 位编译下，结构体布局可能因为默认对齐规则不同而出现差异——比如 64 位编译器可能在某些字段之间插入填充字节。为了消除这种不确定性，所有字段都使用显式大小的类型（uint32_t、uint64_t），结构体整体加上 `__attribute__((packed))` 禁止编译器插入任何填充，并且用 `static_assert` 在编译时验证结构体大小。

确保一致性的规则有三条。第一，使用固定大小类型：`uint32_t`、`uint64_t`，不要用 `int` 或 `long`——这些类型在 32 位和 64 位下的宽度不同。第二，显式对齐：用 `__attribute__((packed))` 禁止编译器插入填充字节。第三，显式 padding：如果需要对齐，用明确的 `_pad` 字段，而不是依赖编译器的自动填充。

这里需要注意 `static_assert` 的跨语言兼容性。C++11 用 `static_assert`，C11 用 `_Static_assert`。头文件里需要用 `#if defined(__cplusplus)` 来区分两种编译环境，分别使用对应的语法。两边的断言条件一样：MemoryMapEntry 必须是 24 字节，BootInfo 必须是 824 字节。

### 1.3 Flat Binary vs ELF：简化到极致

上一章我们讨论了 ELF 格式的 Program Header，当时的设计是 Bootloader 解析 ELF header 后加载各个段。但到了真正动手实现的时候，你会发现一个尴尬的问题：在 Real Mode 下解析 ELF header 需要做 64 位算术运算（读取 8 字节的地址字段），而 Real Mode 的寄存器只有 16 位。虽然技术上可以通过多个寄存器拼接来实现，但代码会非常繁琐且容易出错。更关键的是，Mini Kernel 现在的链接地址是固定的（0x20000），不存在需要重定位的段——所有的段都会被加载到连续的地址空间里。在这种情况下，解析 ELF header 纯粹是在给自己找麻烦。

Flat binary 就是"纯二进制"——`objcopy -O binary` 把 ELF 可执行文件中的所有可加载段按地址顺序提取出来，去掉 ELF header、Program Header、Section Header 等所有元数据，生成一个纯粹的字节流。Bootloader 不需要任何解析，直接把这个字节流原封不动地读到目标物理地址就行了——因为链接脚本里指定的物理地址和 Bootloader 的加载地址是同一个值（0x20000），字节流被放到正确的位置后，代码里的地址引用天然就是对的。

当然 flat binary 也有代价：链接地址必须和加载地址完全匹配，不能做任何重定位。但对于我们 Mini Kernel 现阶段的需求来说，这个限制完全可以接受。等以后要加载"大内核"的时候，Mini Kernel 已经在 Long Mode 下运行了，有完整的 C++ 环境和 64 位算术能力，到时候再解析 ELF 也不迟。

### 1.4 内存布局约定

bootloader 和内核需要约定好数据的物理位置。到 004B 结束时，低 1MB 内存的布局如下：

| 地址 | 用途 | 大小 |
|------|------|------|
| `0x5000` | E820 条目数量 + 内存地图 | 4 + 32*24 B |
| `0x6400` | VESA framebuffer 信息 | 16 B |
| `0x7000` | `BootInfo` 结构体（约定位置，004C 填充） | ~824 B |
| `0x7B00` | DAP (磁盘读取参数包) | 16 B |
| `0x8000` | Stage2 Bootloader 代码 | ~4KB |
| `0x9000~0x19000` | Real Mode 栈 (SS=0x0900, SP=0xFFFE) | ~64KB |
| `0x20000~0x88000` | Mini Kernel 镜像 (flat binary) | 最大 416KB |
| `0x90000` | Protected/Long Mode 栈 | - |

这些地址是"硬编码契约"，双方都要遵守。`BootInfo` 本身放在 0x7000，但其中的 `mmap` 数组指向 0x5000 的数据，`fb_addr` 等字段来自 0x6400 的 VESA 信息。注意 BootInfo 放在 0x7000 到 0x7B00 之间有 2816 字节的空间，824 字节的 BootInfo 绰绰有余。而且 0x7000 是页对齐的（4KB 边界），方便将来做页表映射。

---

## 二、动手实现

### Step 1: 创建 BootInfo 头文件

**目标**：创建 `boot/boot_info.h`，定义 Bootloader 和内核共用的数据结构。

**设计思路**：这个头文件是 Bootloader 和内核之间的"契约"——两边必须对数据布局有完全一致的理解。我们用显式大小的类型（stdint.h 的 uint32_t 和 uint64_t）而不是 int、long 这种平台相关的类型，加上 `__attribute__((packed))` 确保编译器不会偷偷插入填充字节。`static_assert` 在编译时验证结构体大小，如果有人不小心改了字段导致大小变化，编译直接报错而不是运行时数据错乱。

BootInfo 的字段布局你需要设计成这样：首先是内核相关的三个 uint64_t（entry_point、kernel_phys_base、kernel_size，共 24 字节），然后是 framebuffer 相关的字段（fb_addr 是 uint64_t，fb_width、fb_height、fb_pitch、fb_bpp 各是 uint32_t，共 24 字节），最后是内存地图（mmap_count 是 uint32_t，紧跟一个 uint32_t 的显式填充 _pad，然后是 mmap 数组——最多 32 个 MemoryMapEntry，每个 24 字节，共 768 字节）。MemoryMapEntry 的字段和 E820 返回格式完全一致：base（uint64_t）、length（uint64_t）、type（uint32_t）、acpi（uint32_t），共 24 字节，同样 packed。

头文件的注释还应该列出完整的地址约定表——BootInfo 放在物理 0x7000，E820 缓冲区在 0x5000，帧缓冲信息在 0x6400。这些固定地址在调试时非常有用：你可以在 QEMU monitor 或者 GDB 里直接 `x/8xg 0x7000` 查看 BootInfo 的内容，不用猜测数据在哪里。

**踩坑预警**：如果你忘了加 `__attribute__((packed))`，64 位编译器可能会在 mmap_count 后面插入 4 字节填充来对齐后面的 mmap 数组（因为 mmap 数组的元素包含 uint64_t 字段，编译器想让数组起始地址 8 字节对齐）。这时候 BootInfo 的实际大小就会变成 828 字节而不是 824 字节——bootloader 按旧偏移写的字段，内核按新偏移读，所有数据全部错位。这种 bug 非常阴险，因为两边单独编译都不会报错，只有合在一起运行时才会出问题。所以 `static_assert` 是你最后的安全网。

关于 `static_assert` 的写法需要注意：这个头文件会被 C 文件（bootloader）和 C++ 文件（内核）同时包含。C11 用 `_Static_assert`，C++11 用 `static_assert`。你需要用 `#if defined(__cplusplus)` 来区分两种编译环境，分别使用对应的语法。两边的断言条件一样：MemoryMapEntry 必须是 24 字节，BootInfo 必须是 824 字节。

**验证**：头文件创建后不需要单独构建验证，但可以确认文件路径为 `boot/boot_info.h`，并且 `static_assert` 的条件在 32 位和 64 位编译下都成立。后续 Step 5 会统一验证。

---

### Step 2: 确认内存布局约定

**目标**：确认并记录所有与 BootInfo 相关的固定地址约定，确保整个代码库对地址理解一致。

**设计思路**：BootInfo 结构体放在物理 0x7000。这个位置不是随意选的——低 1MB 的内存里已经塞了不少东西：0x5000 是 E820 缓冲区、0x6000~0x6400 是 VESA 信息、0x7B00 是 DAP、0x8000 是 Stage2 代码。0x7000 到 0x7B00 之间有 0xB00 = 2816 字节的空闲空间，放一个 824 字节的 BootInfo 绰绰有余。而且 0x7000 是页对齐的（4KB 边界），方便将来做页表映射。

你需要在头文件的注释中记录完整的地址约定表，这样任何开发者看到这个头文件就能理解整个内存布局。同时确认 linker script（`kernel/mini/linker.ld`）中的 `KERNEL_PHYS_BASE = 0x20000` 与 bootloader 的加载地址一致。

**踩坑预警**：如果你只改了 boot.S 的加载地址但忘了改 linker.ld 的 KERNEL_PHYS_BASE，会出现一个非常诡异的 bug：Bootloader 把内核正确加载到了 0x20000，但内核代码里的地址引用全部按 0x10000（旧值）计算——函数调用跳到错误的地址，全局变量读到错误的数据。更阴险的是，如果旧值和新值之间恰好没有严重冲突（比如旧值附近也是可写内存），内核可能"看起来"能跑一段但数据完全错乱。

**验证**：检查 `boot/boot_info.h` 注释中的地址表是否和实际代码一致。E820 在 0x5000、VESA 在 0x6400、BootInfo 在 0x7000、DAP 在 0x7B00、Stage2 在 0x8000、内核加载到 0x20000。同时确认 `kernel/mini/linker.ld` 中的 `KERNEL_PHYS_BASE = 0x20000`。
