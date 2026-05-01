# 008-2 解析 ELF，加载大内核：从磁盘到跳转的最后一步

> 标签：ELF64, PT_LOAD, BSS, higher-half, 内核加载, 程序头表
> 前置：[008-1 ATA PIO 驱动](008-load-large-kernel-1.md)

## 前言——为什么我们需要理解 ELF

上一章我们把 ATA PIO 磁盘驱动搞定了，mini kernel 现在能从磁盘读取任意扇区的数据。但说实话，光能读数据还不够——你从磁盘上读到了一堆字节，但你怎么知道这些字节代表什么？代码从哪里开始执行？数据段放在内存的什么位置？全局变量占多大空间？这些问题都不是靠"读数据"能回答的——你需要一个"格式解析器"来理解磁盘上的数据结构。

这个格式就是 ELF（Executable and Linkable Format），Linux/Unix 系统的标准可执行文件格式。我们的"大内核"以 ELF64 格式存储在磁盘上，mini kernel 需要解析 ELF header 找到入口点、遍历程序头表（Program Header Table）找到所有需要加载的段、把每个段从 staging buffer 搬运到正确的物理地址、清零 BSS 区域。这一连串操作在用户态的 Linux 系统里是 `ld.so` 的工作，但在内核启动阶段——没有操作系统、没有标准库、连 `malloc` 都没有——我们必须自己写。

## ELF 格式——你真的了解你编译出来的二进制文件吗？

如果你之前只用过 `gcc` 编译出可执行文件然后直接运行，你可能从来没关心过 ELF 格式。但当你开始写操作系统的时候，理解 ELF 就变成了必修课——因为你的内核就是一个 ELF 文件，而你必须自己写代码来解析它。

ELF 文件的开头是一个 64 字节的 header（Elf64_Ehdr），包含了文件的基本信息。其中最重要的字段包括：前 4 字节是 magic number `\x7FELF`（用于验证"这确实是个 ELF 文件"）、e_type 标识文件类型（2 = 可执行文件）、e_machine 标识目标架构（0x3E = 62 = x86-64）、e_entry 是程序的入口点虚拟地址、e_phoff 是程序头表在文件中的偏移、e_phnum 是程序头的数量。

程序头表是一个数组，每个元素（Elf64_Phdr，56 字节）描述一个"段"（segment）。我们只关心 `p_type == PT_LOAD`（type=1）的段——这些是需要加载到内存的段。每个 PT_LOAD 段有几个关键字段：p_offset（段数据在文件中的起始偏移）、p_vaddr（目标虚拟地址）、p_paddr（目标物理地址）、p_filesz（文件中的大小）、p_memsz（内存中的大小）。这里有一个非常重要的点：p_memsz 可以大于 p_filesz，多出来的部分就是 BSS——需要清零但不需要从文件读取。C/C++ 的语义要求全局变量初始化为零，这个"清零"工作在用户态由操作系统加载器完成，但在我们的场景里需要自己做。

```
磁盘上的 ELF 文件:
+------------------+
|  Elf64_Ehdr      |  magic: \x7FELF, e_entry = 0xFFFFFFFF80001000
|  (64 bytes)      |  e_phoff -> 程序头表偏移
+------------------+
|  Elf64_Phdr[0]   |  PT_LOAD: p_paddr=0x100000, p_filesz=8K, p_memsz=12K
|  Elf64_Phdr[1]   |  PT_LOAD: p_paddr=0x200000, p_filesz=4K, p_memsz=4K
+------------------+
|  段 0 数据        |  代码段 (8K)
|  段 1 数据        |  数据段 (4K)
+------------------+

加载后的内存:
物理地址 0x100000:  [代码段 8K 字节][BSS 清零 4K 字节]
物理地址 0x200000:  [数据段 4K 字节]

物理入口 = 0xFFFFFFFF80001000 - 0xFFFFFFFF80000000 = 0x1000
```

## 环境说明

和上一章完全一致：C++ 内核（`-ffreestanding -nostdlib`），QEMU 模拟 x86_64 环境，磁盘镜像中 LBA 848 位置预留了大内核空间。ELF 解析器在 mini kernel 的身份映射环境下运行，使用物理地址进行段搬运。

## 第一步——定义 ELF64 结构体

ELF64 的结构体定义必须严格按照规范，字段顺序和类型都不能错。这里有一个非常容易踩的坑——ELF64 的程序头和 ELF32 的字段顺序不同。在 ELF32 中，`p_flags` 是最后一个字段；但在 ELF64 中，`p_flags` 被移到了第二个位置（紧跟 `p_type` 之后）。如果你照着 ELF32 的资料来写 ELF64 的结构体，后面的 `p_offset`、`p_vaddr` 等字段全部会读到错误的偏移位置，解析出来的值全是垃圾。这个坑不会在编译时暴露，只在运行时才会发现数据完全对不上，笔者在这里血压拉满。

```cpp
// kernel/mini/elf_loader.hpp — 关键结构体定义
struct Elf64_Ehdr {
    uint8_t  e_ident[16];    // ELF identification (magic, class, endian, ...)
    uint16_t e_type;         // Object type (2 = executable)
    uint16_t e_machine;      // Architecture (0x3E = x86-64)
    uint32_t e_version;
    uint64_t e_entry;        // Virtual entry point
    uint64_t e_phoff;        // Program header table offset
    uint64_t e_shoff;        // Section header table offset
    uint32_t e_flags;
    uint16_t e_ehsize;       // ELF header size (64)
    uint16_t e_phentsize;    // Program header entry size (56)
    uint16_t e_phnum;        // Number of program headers
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct Elf64_Phdr {
    uint32_t p_type;     // Segment type (1 = PT_LOAD)
    uint32_t p_flags;    // 注意：ELF64 里 p_flags 在第二个位置！
    uint64_t p_offset;   // File offset
    uint64_t p_vaddr;    // Virtual address
    uint64_t p_paddr;    // Physical address
    uint64_t p_filesz;   // Size in file
    uint64_t p_memsz;    // Size in memory (>= filesz, delta = BSS)
    uint64_t p_align;    // Alignment
} __attribute__((packed));
```

`__attribute__((packed))` 防止编译器在字段之间插入填充字节——ELF 文件里的二进制布局是固定的，任何额外的填充都会导致读取错误。

## 第二步——验证 ELF header

`parse_elf_header` 的验证逻辑是层层递进的，每一步失败都会打印详细的错误信息。这个函数故意不打印成功信息——因为调用者可能在"探测"多个位置，不是每个位置都一定有 ELF 文件。验证顺序经过精心安排：magic 最便宜（只读 4 字节），如果连 magic 都不对，后面的检查没有意义。class 和 endianness 检查在 machine 之前，因为如果字节序都不对，读出来的 e_machine 值可能是乱序的。

```cpp
// kernel/mini/elf_loader.cpp — parse_elf_header (简化)
bool parse_elf_header(const void* elf) {
    if (elf == nullptr) return false;
    const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf);

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L'  || ehdr->e_ident[3] != 'F') {
        kprintf("[ELF] ERROR: invalid magic\n");
        return false;
    }
    if (ehdr->e_ident[4] != ELF_CLASS_64)  { /* error */ return false; }
    if (ehdr->e_ident[5] != ELF_DATA_LSB)  { /* error */ return false; }
    if (ehdr->e_machine != EM_X86_64)      { /* error */ return false; }
    if (ehdr->e_type != ET_EXEC)           { /* error */ return false; }
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) { /* error */ return false; }

    return true;
}
```

## 第三步——加载 ELF 段

`load_elf` 是 ELF 解析器的核心函数，承担了最关键的工作：遍历所有程序头，对每个 PT_LOAD 段做 bounds checking、拷贝文件数据、清零 BSS。

其中 bounds checking 是一个重要的安全措施。我们事先不知道 ELF 文件有多大，所以读了一个"保守上限"（512 个扇区 = 256KB）到 staging buffer。如果 ELF 文件比这个上限大，段数据就会被截断。bounds checking 能在这种情况下提前发现错误——检查 `p_offset + p_filesz` 是否超出 staging_size——而不是让垃圾数据静默地被拷贝到目标地址。这种 bug 在小内核上不容易发现，但内核变大后就会暴露，而且非常难定位。

段的目标地址使用 `p_paddr`（物理地址）而不是 `p_vaddr`（虚拟地址），因为 mini kernel 运行在身份映射环境下——物理地址就是可以直接通过指针访问的地址。

BSS 清零是一个容易被忽略但极其重要的步骤。如果不清零 BSS，内核里的全局变量会包含随机垃圾值，导致"有时候能跑有时候不能跑"的随机性行为，调试起来非常折磨。清零逻辑就是计算 `p_memsz - p_filesz` 得到 BSS 大小，然后调用前面实现的 `memset` 填零。

```cpp
// kernel/mini/elf_loader.cpp — load_elf 核心循环 (简化)
for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
    const Elf64_Phdr* phdr = get_phdr(ehdr, i);
    if (phdr->p_type != PT_LOAD) continue;

    // Bounds checking — 防止读到截断数据
    if (phdr->p_offset + phdr->p_filesz > staging_size) {
        kprintf("[ELF] ERROR: segment exceeds staging buffer\n");
        return 0;
    }

    uint64_t dest_addr = phdr->p_paddr;
    const void* src = (const uint8_t*)elf_src + phdr->p_offset;

    // 拷贝文件数据
    if (phdr->p_filesz > 0)
        memcpy((void*)dest_addr, src, phdr->p_filesz);

    // 清零 BSS
    if (phdr->p_memsz > phdr->p_filesz) {
        uint64_t bss_start = dest_addr + phdr->p_filesz;
        size_t bss_size = phdr->p_memsz - phdr->p_filesz;
        memset((void*)bss_start, 0, bss_size);
    }
}
```

加载完成后，入口点的处理需要做 higher-half 地址转换。Cinux 的大内核采用 higher-half 设计——内核代码链接在 0xFFFFFFFF80000000 以上的虚拟地址空间。但 mini kernel 运行时只有物理地址的身份映射，没有建立内核的虚拟地址映射。所以如果 `e_entry` 是虚拟地址，需要减去 0xFFFFFFFF80000000 得到物理入口；如果入口点在低地址，直接使用即可。

```cpp
constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
uint64_t entry = ehdr->e_entry;
if (entry >= HIGHER_HALF_BASE) {
    entry = entry - HIGHER_HALF_BASE;
}
return entry;
```

### xv6、Linux 和 SerenityOS 怎么做 ELF 加载？

现在回头看 ELF 加载这个话题，不同 OS 的设计选择差异非常大。xv6 的 `bootmain()` 把整个 ELF 加载塞在一个函数里——先读 ELF header 到 0x10000，验证 magic，然后遍历 program headers，每个段直接从磁盘读到 paddr。xv6 不需要 staging buffer，也不需要 bounds checking——因为它每次只读一个 program header 指定的数据量。和 Cinux 的设计相比，xv6 更紧凑但能力也更有限：它只支持 ELF32，不做 bounds checking，没有 higher-half 地址转换的概念。

Linux 的 ELF 加载分两层。内核自身的加载由 GRUB 完成——GRUB 解析 Multiboot header，把内核加载到 1MB 以上的物理内存，然后跳转到入口点。用户态程序的 ELF 加载则由内核的 `binfmt_elf` 模块完成——这是一个非常复杂的加载器，处理动态链接、共享库加载、ASLR（地址空间布局随机化）、段权限设置等。Cinux 的情况更像 Linux 的内核加载——我们加载的是内核本身，不是用户态程序。但 Cinux 自己实现 ELF 解析而不是依赖 GRUB，这又回到了"自己动手"和"依赖第三方"的权衡。

SerenityOS 和 Linux 一样依赖 GRUB 做内核加载。它的内核代码因此不需要在启动早期实现 ELF 解析器——GRUB 处理了所有底层细节。这种设计让 SerenityOS 的内核入口代码更简洁，但也意味着 SerenityOS 的开发者对启动链条的理解可能不如自己实现过 ELF 加载的开发者深刻。从教学角度来看，Cinux 选择自己实现 ELF 解析是更有价值的——你不仅理解了 ELF 格式本身，还理解了 higher-half 内核的地址转换、BSS 清零的重要性、以及 staging buffer 和 bounds checking 的设计考量。

## 第四步——大内核加载器的流程编排

`load_big_kernel()` 是整个加载过程的编排者，把 ATA 驱动和 ELF 解析器串联起来。流程很直接：先读磁盘到 staging buffer（0x1000000 = 16MB 物理地址），验证 ELF magic，然后调用 ELF 解析器完成段加载。staging buffer 放在 16MB 位置是有讲究的——远离 mini kernel（0x20000）和 bootloader 结构（0x500-0x7BFF），不会产生冲突。

```cpp
// kernel/mini/big_kernel_loader.cpp
uint64_t load_big_kernel(uint64_t disk_lba) {
    constexpr uint32_t staging_bytes = BIG_KERNEL_MAX_SECTORS * 512;

    // 读磁盘到 staging buffer
    if (!driver::ata::read(disk_lba, BIG_KERNEL_MAX_SECTORS,
                           (void*)BIG_KERNEL_LOAD_ADDR)) {
        return 0;
    }

    // 快速验证 ELF magic
    const auto* magic = (const uint8_t*)BIG_KERNEL_LOAD_ADDR;
    if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        return 0;
    }

    // ELF 加载
    return elf_loader::load_elf((void*)BIG_KERNEL_LOAD_ADDR, staging_bytes);
}
```

大内核在磁盘上的起始 LBA 是 848——前面 16 个扇区给 MBR + Stage2，然后 832 个扇区给 mini kernel（416KB）。这个值必须和构建脚本中磁盘镜像的布局一致。BIG_KERNEL_MAX_SECTORS = 512（最多读 256KB）是一个保守的上限——如果将来内核变大，只需要增大这个常量。

快速 magic 检查是一个"快速失败"策略。与其让 ELF 解析器一步步验证，不如先花一微秒检查前四个字节——如果不是 ELF 文件就直接返回，省得浪费时间。而且逐字节比较避免了将任意内存位置强制转换为 uint32_t 可能导致的对齐问题。

## 收尾——验证管线可用

在 `main.cpp` 中，我们通过两个 demo 测试验证整个管线。第一个读取 MBR（LBA 0）验证引导签名 0xAA55，第二个读取 LBA 16（mini kernel 在磁盘上的位置）检查是否有 ELF header。虽然真正的大内核还不存在，但加载管线已经完全就位——ATA 驱动、ELF 解析器、大内核加载器三剑客全部到位，只差一个真正的大内核 ELF 文件放上去就能跑。

```
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xAA55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
[DEMO] ELF header detected at disk LBA 16 (mini kernel)
[MINI] Milestone 008 complete. Waiting for big kernel (009+)...
```

到这里就大功告成了——mini kernel 现在具备了完整的磁盘读取和 ELF 加载能力。下一个 milestone 我们会真正编译一个大内核 ELF 放到磁盘上，让 mini kernel 加载它并跳转过去执行。那才是真正的"大内核启动"时刻。

## 参考资料

- OSDev Wiki: [ELF](https://wiki.osdev.org/ELF) — ELF64 格式结构定义和加载步骤
  - Source: `helpers/web_search/osdev/elf_format.md`
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) — ATA PIO 磁盘读取
  - Source: `helpers/web_search/osdev/ata_pio_mode.md`
- xv6 bootmain.c: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) — 经典的 bootloader 级 ELF 加载，先读 ELF header 再逐段读磁盘
  - Source: `helpers/web_search/os_compare/xv6_bootmain.md`
- Linux boot process: [linux-insides](https://0xax.gitbook.io/linux-insides/summary/booting/linux-bootstrap-1) — GRUB 解析 Multiboot header 加载内核
  - Source: `helpers/web_search/os_compare/ata_disk_load_compare.md`
- SerenityOS: [GitHub](https://github.com/SerenityOS/serenity) — 使用 GRUB + Multiboot 启动，内核不需要自己实现 ELF 加载
- Intel SDM: Vol. 3A — 物理地址空间布局和处理器初始化
