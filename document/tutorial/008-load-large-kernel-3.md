# 008-3 解析 ELF，加载大内核：从磁盘到跳转的最后一步

> 标签：ELF64, PT_LOAD, BSS, higher-half, 内核加载, 程序头表
> 前置：[008-2 ATA PIO 驱动](008-load-large-kernel-2.md)

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

## 第六步——定义 ELF64 结构体

ELF64 的结构体定义必须严格按照规范，字段顺序和类型都不能错。这里有一个非常容易踩的坑——ELF64 的程序头和 ELF32 的字段顺序不同。在 ELF32 中，`p_flags` 是最后一个字段；但在 ELF64 中，`p_flags` 被移到了第二个位置（紧跟 `p_type` 之后）。如果你照着 ELF32 的资料来写 ELF64 的结构体，后面的 `p_offset`、`p_vaddr` 等字段全部会读到错误的偏移位置，解析出来的值全是垃圾。这个坑不会在编译时暴露，只在运行时才会发现数据完全对不上。

```cpp
// kernel/mini/elf_loader.hpp -- 关键结构体定义
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

## 第七步——验证 ELF header 和加载段

`parse_elf_header` 的验证逻辑是层层递进的，每一步失败都会打印详细的错误信息。这个函数故意不打印成功信息——因为调用者可能在"探测"多个位置，不是每个位置都一定有 ELF 文件。

核心加载函数是 `load_elf()`，它接受两个参数：一个指向内存中 ELF 数据的指针，以及一个暂存区大小（用于边界检查）。函数遍历所有程序头，跳过非 PT_LOAD 的段，对每个 PT_LOAD 段执行以下操作：

先做边界检查——确认段数据（`p_offset + p_filesz`）没有超出暂存区的实际大小。这一步特别重要，因为我们从磁盘读的扇区数是固定的（BIG_KERNEL_MAX_SECTORS = 512），如果大内核的 ELF 比暂存区还大，不检查就会读到未初始化的内存垃圾。

然后是关键的搬运和清零操作：

```cpp
uint64_t dest_addr = phdr->p_paddr;
const void* src = reinterpret_cast<const uint8_t*>(elf_src) + phdr->p_offset;

if (phdr->p_filesz > 0) {
    memcpy(reinterpret_cast<void*>(dest_addr), src, phdr->p_filesz);
}

if (phdr->p_memsz > phdr->p_filesz) {
    uint64_t bss_start = dest_addr + phdr->p_filesz;
    size_t bss_size = static_cast<size_t>(phdr->p_memsz - phdr->p_filesz);
    memset(reinterpret_cast<void*>(bss_start), 0, bss_size);
}
```

加载完成后，函数从 ELF 头中读取入口地址 `e_entry`。这里有一个小细节需要处理：我们的大内核是 higher-half 设计的，入口地址是一个类似 `0xFFFFFFFF80000000` 的虚拟地址，但 mini kernel 当前运行在 identity mapping 下，没法直接用这个虚拟地址跳转。所以我们需要把它转换成物理地址——如果入口地址大于 `0xFFFFFFFF80000000`（higher-half 基址），就减去这个基址得到物理地址，否则直接使用：

```cpp
constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
uint64_t entry = ehdr->e_entry;
if (entry >= HIGHER_HALF_BASE) {
    entry = entry - HIGHER_HALF_BASE;
}
return entry;
```

## 第八步——串起完整加载管线

现在我们有了磁盘驱动和 ELF 解析器，还需要一个模块把两者串起来，形成从"磁盘上的原始字节"到"内存中可跳转的内核"的完整管线。这就是 `big_kernel_loader` 的工作。

加载策略是这样的：大内核的 ELF 二进制存放在磁盘的 LBA 848 开始的位置，我们用 ATA 驱动一次读取 512 个扇区（256KB）到 0x1000000 的暂存区，然后调用 ELF 解析器处理暂存区中的数据。512 个扇区是一个保守的上限，如果实际的大内核更小，ELF 解析器只会处理它找到的段，多读的磁盘数据不会被使用。

```cpp
uint64_t load_big_kernel(uint64_t disk_lba) {
    constexpr uint32_t staging_bytes =
        static_cast<uint32_t>(BIG_KERNEL_MAX_SECTORS) * driver::ata::ATA_SECTOR_SIZE;

    if (!driver::ata::read(disk_lba, BIG_KERNEL_MAX_SECTORS,
                           reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR))) {
        kprintf("[LOADER] ERROR: Failed to read big kernel from disk!\n");
        return 0;
    }

    const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
    if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        kprintf("[LOADER] ERROR: No ELF magic at staging buffer!\n");
        return 0;
    }

    uint64_t entry = elf_loader::load_elf(
        reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR), staging_bytes);

    kprintf("[LOADER] Big kernel loaded successfully.\n");
    kprintf("[LOADER] Entry point: 0x%p\n", entry);
    return entry;
}
```

你会发现我们在调用 ELF 解析器之前先做了一个快速魔法数检查——读取暂存区的前四个字节看是不是 0x7F 'E' 'L' 'F'。这不是多余的，因为 `load_elf` 内部虽然也有头验证，但如果磁盘上的数据根本不是 ELF 文件（比如大内核还没放上去，读到的全零），快速检查可以避免进入更复杂的解析逻辑，也能给出更明确的错误信息。

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

- OSDev Wiki: [ELF](https://wiki.osdev.org/ELF) -- ELF64 格式结构定义和加载步骤
  - Source: `helpers/web_search/osdev/elf_format.md`
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) -- ATA PIO 磁盘读取
  - Source: `helpers/web_search/osdev/ata_pio_mode.md`
- xv6 bootmain.c: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) -- 经典的 bootloader 级 ELF 加载，先读 ELF header 再逐段读磁盘
  - Source: `helpers/web_search/os_compare/xv6_bootmain.md`
- Linux boot process: [linux-insides](https://0xax.gitbook.io/linux-insides/summary/booting/linux-bootstrap-1) -- GRUB 解析 Multiboot header 加载内核
  - Source: `helpers/web_search/os_compare/ata_disk_load_compare.md`
- SerenityOS: [GitHub](https://github.com/SerenityOS/serenity) -- 使用 GRUB + Multiboot 启动，内核不需要自己实现 ELF 加载
- Intel SDM: Vol. 3A -- 物理地址空间布局和处理器初始化
