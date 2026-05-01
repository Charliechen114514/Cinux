# 008-2 Read-through: ELF64 解析器与大内核加载器

## 概览

本文是 tag 008 代码讲解的第二部分，聚焦于 ELF64 解析器（`elf_loader.hpp/cpp`）、大内核加载器（`big_kernel_loader.hpp/cpp`）和 `main.cpp` 中的集成代码。ELF 解析器负责验证 ELF 头、计算内核内存占用、解析并加载 PT_LOAD 段；大内核加载器编排整个流程——先通过 ATA 驱动读磁盘，然后调用 ELF 解析器完成加载。这两个模块和上一篇文章中的 ATA 驱动、freestanding 工具函数一起，构成了完整的"从磁盘加载大内核"管线。

关键设计决策一览：ELF 解析器使用 p_paddr 而非 p_vaddr 作为段加载目标（适配身份映射环境）；higher-half 地址转换在最后一步完成（入口点虚拟→物理）；staging buffer bounds checking 防止读到截断数据。

---

## 架构图

```
大内核加载流程:

load_big_kernel(disk_lba)
  │
  ├── ATA::read(lba, 512 sectors, 0x1000000)  ← 上一篇
  │     将整个 ELF 文件读到 staging buffer
  │
  ├── 快速验证: magic[0..3] == "\x7FELF"
  │
  └── load_elf(0x1000000, staging_size)
        │
        ├── parse_elf_header()
        │     验证 magic/class/machine/type
        │
        ├── 遍历 Program Headers
        │     ├── get_phdr(ehdr, i)
        │     ├── bounds checking: p_offset + p_filesz <= staging_size
        │     ├── memcpy(dest, src + p_offset, p_filesz)  ← lib/string
        │     └── memset(dest + p_filesz, 0, bss_size)    ← lib/string
        │
        └── higher-half 地址转换
              entry = e_entry - 0xFFFFFFFF80000000
              return entry (物理地址)

磁盘布局:
LBA 0-15:     MBR + Stage2
LBA 16-847:   Mini kernel (832 sectors = 416KB)
LBA 848+:     Big kernel ELF (最多 512 sectors = 256KB)

内存布局:
0x20000:      Mini kernel (运行中)
0x1000000:    Staging buffer (大内核 ELF 临时存放处)
p_paddr 处:   各 PT_LOAD 段的最终位置
```

---

## 代码精讲

### ELF64 解析器

#### elf_loader.hpp — ELF64 结构体定义

```cpp
namespace cinux::mini::elf_loader {

constexpr uint32_t ELF_MAGIC = 0x464C457F;
constexpr uint8_t  ELF_CLASS_64 = 2;
constexpr uint8_t  ELF_DATA_LSB = 1;
constexpr uint8_t  ELF_OSABI_SYSV = 0;
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t EM_X86_64 = 62;
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PF_X = 1;
constexpr uint32_t PF_W = 2;
constexpr uint32_t PF_R = 4;

struct Elf64_Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

bool parse_elf_header(const void* elf);
size_t calculate_kernel_size(const Elf64_Ehdr* ehdr);
uint64_t load_elf(void* elf_src, uint64_t staging_size);
```

ELF64 的结构体定义严格按照规范，使用 `__attribute__((packed))` 防止编译器在字段之间插入填充字节。这里有一个非常容易踩的坑——ELF64 的程序头和 ELF32 的字段顺序不同。在 ELF32 中，`p_flags` 是最后一个字段；但在 ELF64 中，`p_flags` 被移到了第二个位置（紧跟 `p_type` 之后）。如果你照着 ELF32 的结构体定义来写，后面的 `p_offset`、`p_vaddr` 等字段全部会读到错误的偏移位置，解析出来的值全是垃圾。这个坑不会在编译时暴露，只在运行时才会发现数据完全对不上。

常量定义涵盖了 ELF header 验证需要的所有值：magic number（0x464C457F 在小端系统上就是 `\x7FELF`）、64-bit class（2）、little-endian encoding（1）、executable type（2）、x86-64 machine（62 = 0x3E）、PT_LOAD 段类型（1）以及权限标志 PF_X/PF_W/PF_R。

#### elf_loader.cpp — parse_elf_header()

```cpp
bool parse_elf_header(const void* elf) {
    if (elf == nullptr) return false;

    const auto* ehdr = static_cast<const Elf64_Ehdr*>(elf);

    // Verify ELF magic: 0x7F 'E' 'L' 'F'
    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        kprintf("[ELF] ERROR: invalid magic: %02x %02x %02x %02x\n", ...);
        return false;
    }

    if (ehdr->e_ident[4] != ELF_CLASS_64) {
        kprintf("[ELF] ERROR: not 64-bit ELF (class=%u)\n", ehdr->e_ident[4]);
        return false;
    }

    if (ehdr->e_ident[5] != ELF_DATA_LSB) {
        kprintf("[ELF] ERROR: not little-endian (encoding=%u)\n", ehdr->e_ident[5]);
        return false;
    }

    if (ehdr->e_machine != EM_X86_64) {
        kprintf("[ELF] ERROR: not x86-64 (machine=%u)\n", ehdr->e_machine);
        return false;
    }

    if (ehdr->e_type != ET_EXEC) {
        kprintf("[ELF] ERROR: not executable (type=%u)\n", ehdr->e_type);
        return false;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        kprintf("[ELF] ERROR: no program headers\n");
        return false;
    }

    return true;
}
```

`parse_elf_header` 的验证逻辑是层层递进的：先检查 magic number（前 4 字节必须是 0x7F, 'E', 'L', 'F'），然后检查 class（必须是 64-bit）、字节序（必须是 little-endian）、目标架构（必须是 x86-64）、文件类型（必须是可执行文件），最后检查程序头表是否存在。每一步失败都会打印详细的错误信息——包含实际读到的值，这样调试时能立刻知道是哪个字段不匹配。

这个验证顺序是精心安排的：magic number 验证最便宜（只读 4 字节），如果连 magic 都不对，后面的检查没有意义。class 和 endianness 检查在 machine 之前，因为如果字节序都不对，读出来的 e_machine 值可能是乱序的。program headers 的存在性检查放在最后，因为即使没有 program headers，前面的字段正确也说明"这是一个 ELF64 x86-64 可执行文件，只是缺少段信息"。

值得注意的是，这个函数故意不打印成功信息。原因是调用者可能在"探测"——比如尝试读取某个 LBA 位置看是否有 ELF 文件，不是每个位置都一定有。如果每次成功都打印一行，那正常流程的串口输出会被大量冗余信息淹没。

#### elf_loader.cpp — calculate_kernel_size()

```cpp
size_t calculate_kernel_size(const Elf64_Ehdr* ehdr) {
    uint64_t lowest_addr = UINT64_MAX;
    uint64_t highest_addr = 0;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* phdr = get_phdr(ehdr, i);
        if (phdr == nullptr) continue;
        if (phdr->p_type != PT_LOAD) continue;

        if (phdr->p_paddr < lowest_addr) {
            lowest_addr = phdr->p_paddr;
        }
        uint64_t seg_end = phdr->p_paddr + phdr->p_memsz;
        if (seg_end > highest_addr) {
            highest_addr = seg_end;
        }
    }

    if (lowest_addr == UINT64_MAX) return 0;
    return static_cast<size_t>(highest_addr - lowest_addr);
}
```

`calculate_kernel_size` 通过遍历所有 PT_LOAD 段，找到最低物理地址和最高物理地址+大小，计算总内存占用。注意这里用的是 `p_memsz` 而不是 `p_filesz`——因为 BSS 虽然不在文件里，但在内存中确实占空间。如果只算 filesz，会低估内核的实际内存需求。

这个函数目前在大内核加载流程中并没有被调用（它更像是一个工具函数），但在后续 milestone 中可能会用于预检查——在加载之前先确认目标内存区域是否有足够空间。`get_phdr` 辅助函数通过 `e_phoff + index * e_phentsize` 计算每个程序头的位置，这是标准做法。

#### elf_loader.cpp — load_elf()

```cpp
uint64_t load_elf(void* elf_src, uint64_t staging_size) {
    if (!parse_elf_header(elf_src)) {
        kprintf("[ELF] ERROR: ELF header validation failed!\n");
        return 0;
    }

    auto* ehdr = static_cast<Elf64_Ehdr*>(elf_src);
    kprintf("[ELF] Entry point: 0x%p\n", ehdr->e_entry);
    kprintf("[ELF] Program headers: %u at offset 0x%p\n", ehdr->e_phnum, ehdr->e_phoff);

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* phdr = get_phdr(ehdr, i);
        if (phdr == nullptr) { /* warning */ continue; }
        if (phdr->p_type != PT_LOAD) continue;

        kprintf("[ELF] PT_LOAD[%u]: vaddr=0x%p paddr=0x%p filesz=0x%p memsz=0x%p\n", ...);

        // Bounds checking
        if (phdr->p_offset + phdr->p_filesz > staging_size) {
            kprintf("[ELF] ERROR: segment %u data exceeds staging buffer\n", ...);
            return 0;
        }

        uint64_t dest_addr = phdr->p_paddr;
        const void* src = reinterpret_cast<const uint8_t*>(elf_src) + phdr->p_offset;

        // Copy file data
        if (phdr->p_filesz > 0) {
            memcpy(reinterpret_cast<void*>(dest_addr), src, phdr->p_filesz);
        }

        // Zero-fill BSS
        if (phdr->p_memsz > phdr->p_filesz) {
            uint64_t bss_start = dest_addr + phdr->p_filesz;
            size_t bss_size = static_cast<size_t>(phdr->p_memsz - phdr->p_filesz);
            memset(reinterpret_cast<void*>(bss_start), 0, bss_size);
        }

        kprintf("[ELF] Loaded segment %u: 0x%p -> 0x%p (%u bytes, BSS %u bytes)\n", ...);
    }

    kprintf("[ELF] All PT_LOAD segments loaded.\n");

    // Higher-half address conversion
    constexpr uint64_t HIGHER_HALF_BASE = 0xFFFFFFFF80000000ULL;
    uint64_t entry = ehdr->e_entry;
    if (entry >= HIGHER_HALF_BASE) {
        entry = entry - HIGHER_HALF_BASE;
    }

    return entry;
}
```

`load_elf` 是 ELF 解析器的核心函数，承担了最关键的工作。整个流程可以分成四个阶段：验证、遍历、加载、地址转换。

验证阶段调用 `parse_elf_header` 确认 ELF header 合法，然后将 void 指针转换为 `Elf64_Ehdr*` 以便访问各个字段。这里之所以能安全地做 `static_cast`（而不是 `const_cast`），是因为 `elf_src` 参数是 `void*` 非 const——load_elf 需要修改目标内存区域的内容（段拷贝和 BSS 清零），所以 staging buffer 必须是可写的。

遍历阶段逐个检查程序头，跳过非 PT_LOAD 段。对每个 PT_LOAD 段，先做 bounds checking——确认段数据（从 `p_offset` 开始，长 `p_filesz` 字节）不会超出 staging buffer 的范围。这是一个重要的安全检查：我们事先不知道 ELF 文件有多大，所以读了一个"保守上限"的扇区数。如果 ELF 文件比这个上限大，段数据就会被截断，读到的东西是垃圾。bounds checking 能在这种情况下提前发现错误，而不是让垃圾数据静默地被拷贝到目标地址。

加载阶段的核心就是两步：先用 `memcpy` 把文件数据从 staging buffer 拷贝到 `p_paddr` 指定的物理地址，再用 `memset` 把 BSS 部分（`p_memsz - p_filesz`）清零。段的目标地址直接使用 `p_paddr`（物理地址），因为 mini kernel 运行在身份映射环境下——物理地址就是可以直接通过指针访问的地址。`p_vaddr`（虚拟地址）在这里不使用，因为 mini kernel 没有建立大内核的虚拟地址映射。

BSS 清零是一个容易被忽略但极其重要的步骤。C/C++ 的语义要求全局变量和静态变量初始化为零，这个"清零"工作在用户态由操作系统加载器完成，但在内核加载阶段需要我们自己来做。如果不清零 BSS，内核里的全局变量会包含随机垃圾值，导致不可预测的行为——而且是那种"有时候能跑有时候不能跑"的随机性，调试起来非常折磨。

地址转换阶段处理 higher-half 内核的入口点。ELF header 里的 `e_entry` 是虚拟地址（比如 0xFFFFFFFF80001000），但 mini kernel 运行在物理地址身份映射模式下，需要把虚拟入口转换为物理入口。方法很直接：如果入口点地址大于等于 0xFFFFFFFF80000000（higher-half 基址），就减去这个基址。如果入口点在低地址（低于 higher-half 基址），说明内核不是 higher-half 设计的，直接用原始地址即可。

### 大内核加载器

#### big_kernel_loader.hpp — 常量定义

```cpp
namespace cinux::mini::loader {

constexpr uint64_t MINI_KERNEL_LOAD_ADDR = 0x20000;
constexpr uint64_t BIG_KERNEL_LOAD_ADDR  = 0x1000000;  // 16MB
constexpr uint64_t BIG_KERNEL_LBA = 848;
constexpr uint16_t BIG_KERNEL_MAX_SECTORS = 512;

uint64_t load_big_kernel(uint64_t disk_lba);
}
```

四个常量定义了整个加载过程的物理布局。`MINI_KERNEL_LOAD_ADDR` 是 mini kernel 被加载到的地址，`BIG_KERNEL_LOAD_ADDR` 是大内核 ELF 的 staging buffer 地址。0x1000000（16MB）这个位置选得很有讲究——它远离 mini kernel（0x20000）和 bootloader 结构（0x500-0x7BFF），同时也低于通常的内核加载地址（Linux 传统上在 0x100000 = 1MB），确保不会和任何已有的数据结构冲突。

`BIG_KERNEL_LBA = 848` 是大内核在磁盘上的起始扇区。这个值是这样算出来的：前 16 个扇区给 MBR（1 个扇区）+ Stage2（约 15 个扇区），然后 832 个扇区给 mini kernel（416KB = 832 × 512）。这个值必须和构建脚本中磁盘镜像的布局一致——如果构建脚本调整了 mini kernel 的大小或者 Stage2 的大小，这个 LBA 值也需要相应调整。

`BIG_KERNEL_MAX_SECTORS = 512`（256KB）是一个保守的上限。这个数字应该远大于大内核的实际大小——如果大内核超过 256KB，加载器会读到截断的数据，但 bounds checking 会捕获这个错误并报告。将来如果内核变大，只需要增大这个常量和 staging buffer 的可用空间即可。

#### big_kernel_loader.cpp — load_big_kernel()

```cpp
uint64_t load_big_kernel(uint64_t disk_lba) {
    kprintf("[LOADER] Loading big kernel from disk LBA 0x%x...\n", disk_lba);

    constexpr uint32_t staging_bytes =
        static_cast<uint32_t>(BIG_KERNEL_MAX_SECTORS) * driver::ata::ATA_SECTOR_SIZE;
    kprintf("[LOADER] Staging at physical address 0x%p (%u KB buffer)\n",
            BIG_KERNEL_LOAD_ADDR, staging_bytes / 1024);

    // Step 1: Read from disk
    if (!driver::ata::read(disk_lba, BIG_KERNEL_MAX_SECTORS,
                           reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR))) {
        kprintf("[LOADER] ERROR: Failed to read big kernel from disk!\n");
        return 0;
    }
    kprintf("[LOADER] Read %u sectors (%u KB) from disk.\n",
            BIG_KERNEL_MAX_SECTORS, staging_bytes / 1024);

    // Step 2: Quick magic check
    const auto* magic = reinterpret_cast<const uint8_t*>(BIG_KERNEL_LOAD_ADDR);
    if (magic[0] != 0x7F || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        kprintf("[LOADER] ERROR: No ELF magic at staging buffer! Got: %02x %02x %02x %02x\n",
                magic[0], magic[1], magic[2], magic[3]);
        return 0;
    }
    kprintf("[LOADER] ELF magic verified at staging buffer.\n");

    // Step 3: Load ELF
    uint64_t entry = elf_loader::load_elf(
        reinterpret_cast<void*>(BIG_KERNEL_LOAD_ADDR), staging_bytes);
    if (entry == 0) {
        kprintf("[LOADER] ERROR: ELF loading failed!\n");
        return 0;
    }

    kprintf("[LOADER] Big kernel loaded successfully.\n");
    kprintf("[LOADER] Entry point: 0x%p\n", entry);
    return entry;
}
```

`load_big_kernel` 是一个纯粹的流程编排函数——它不包含复杂的逻辑，只是按顺序调用三个子模块。第一步通过 ATA 驱动读取磁盘扇区到 staging buffer，第二步做 ELF magic 的快速检查（前四个字节是否为 `\x7FELF`），第三步调用 ELF 解析器完成完整的段加载。

快速 magic 检查是一个很好的"快速失败"策略。与其让 ELF 解析器一步步验证（magic → class → endian → machine → type → program headers），不如在编排层先做最简单的一次检查——如果连 ELF magic 都没有，说明磁盘上那个位置根本不是 ELF 文件，直接报错比浪费时间去 parse 整个 header 要好得多。而且这个检查用的是逐字节比较（magic[0], magic[1], ...），避免了直接将任意内存位置强制转换为 uint32_t 可能导致的对齐问题。

### main.cpp — 集成与 Demo

```cpp
static uint8_t g_sector_buf[512] __attribute__((aligned(16)));

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    kprintf("Cinux Mini Kernel v0.1.0\n");
    // ... boot info 打印、GDT/IDT/PMM 初始化 ...

    // #BP breakpoint exception test
    kprintf("\n[TEST] Triggering breakpoint exception (int $3)...\n");
    __asm__ volatile("int $3");
    kprintf("[TEST] Breakpoint test passed! Execution continued after #BP.\n\n");

    // ATA initialization
    if (!cinux::mini::driver::ata::init()) {
        kprintf("[INIT] ERROR: ATA initialization failed!\n");
        while (1) __asm__ volatile("cli; hlt");
    }

    // Demo: Read MBR and verify boot signature
    kprintf("[DEMO] Reading MBR (LBA 0)...\n");
    if (cinux::mini::driver::ata::read(0, 1, g_sector_buf)) {
        uint16_t sig = static_cast<uint16_t>(g_sector_buf[510]) |
                       (static_cast<uint16_t>(g_sector_buf[511]) << 8);
        kprintf("[DEMO] MBR boot signature: 0x%04x %s\n", sig,
                sig == 0xAA55 ? "(VALID)" : "(INVALID)");
    }

    // Demo: Read mini kernel header and check for ELF
    kprintf("[DEMO] Reading mini kernel header (LBA 16)...\n");
    if (cinux::mini::driver::ata::read(16, 1, g_sector_buf)) {
        if (cinux::mini::elf_loader::parse_elf_header(g_sector_buf)) {
            kprintf("[DEMO] ELF header detected at disk LBA 16 (mini kernel)\n");
        } else {
            kprintf("[DEMO] No valid ELF header at LBA 16 (expected for flat binary)\n");
        }
    }

    kprintf("\n[MINI] Milestone 008 complete. Waiting for big kernel (009+)...\n");
    while (1) __asm__ volatile("cli; hlt");
}
```

main.cpp 中的 demo 代码设计得很巧妙——它不尝试加载大内核（因为大内核还不存在），而是通过两个小测试验证整个管线的可用性。

第一个 demo 读取 MBR（LBA 0）并验证引导签名 0xAA55。这个测试的意义在于：如果 ATA 驱动工作正常，读出来的第 510-511 字节应该是 0x55 和 0xAA（小端序下组合为 0xAA55）。MBR 的引导签名是固定的、已知的值，非常适合作为"第一个读取测试"的验证数据。

第二个 demo 读取 LBA 16（mini kernel 在磁盘上的位置）的第一个扇区，检查是否有 ELF header。这个测试的结果取决于 mini kernel 是以 ELF 格式还是 flat binary 格式存储在磁盘镜像上——如果是 ELF 格式，`parse_elf_header` 会通过；如果是 flat binary（经过 objcopy 转换），则不会通过。无论哪种结果都是正确的——重点是验证 ATA 驱动和 ELF 解析器的集成没有问题。

`g_sector_buf` 声明为 16 字节对齐，这对于 ATA PIO 的 `inw` 操作是最优的对齐方式（虽然 x86 上不对齐也不会崩溃，但对齐访问更快）。

---

## 设计决策

### 决策：使用 p_paddr 而非 p_vaddr 作为段加载目标

**问题**: ELF 程序头里有 p_vaddr（虚拟地址）和 p_paddr（物理地址），加载段时用哪个？

**本项目的做法**: 使用 p_paddr 作为段的目标地址。

**备选方案**: 使用 p_vaddr，然后手动转换为物理地址（p_vaddr - 0xFFFFFFFF80000000）。

**为什么不选备选方案**: 在 higher-half 内核的链接脚本中，p_vaddr 和 p_paddr 通常都会设置——p_vaddr 是虚拟地址（0xFFFFFFFF80000000 以上），p_paddr 是对应的物理地址。直接使用 p_paddr 更简单直接，不需要额外的地址转换。而且，p_paddr 在 ELF 标准中就是"物理加载地址"的语义，正好是我们需要的目标地址。

**如果要扩展/改进**: 当 mini kernel 也建立了 higher-half 映射后，可以改为使用 p_vaddr，让 MMU 自动完成虚拟到物理的转换。但目前的身份映射环境下，p_paddr 是最自然的选择。

### 决策：先读到 staging buffer 再解析

**问题**: 是先读整个 ELF 到 staging buffer 再解析，还是边读边解析？

**本项目的做法**: 先通过 ATA 驱动一次性读取固定数量的扇区到 staging buffer（0x1000000），然后再用 ELF 解析器从 staging buffer 中读取和解析。

**备选方案**: 像 xv6 那样"两遍读取"——先读 ELF header 和 program headers，算出每个段的位置和大小，然后逐段从磁盘读到目标地址。

**为什么不选备选方案**: xv6 的两遍读取方案更节省内存——不需要额外的 staging buffer，因为每个段直接从磁盘读到最终位置。但它的代价是磁盘读取次数更多（每个段至少一次 seek + read），而且代码逻辑更复杂（需要维护读取位置）。Cinux 选择 staging buffer 方案是因为：1）内存空间充裕（mini kernel 以上有大量可用内存）；2）staging buffer 让 bounds checking 成为可能；3）代码更简洁——读磁盘和解析 ELF 完全解耦。

**如果要扩展/改进**: 如果内核变得很大（超过 staging buffer 的 256KB），可以改为动态计算 staging buffer 大小——先读 ELF header 确定文件大小，再分配足够的 staging 空间。或者像 xv6 那样用两遍读取方案。

### 决策：staging_size 参数用于 bounds checking

**问题**: ELF 解析器是否需要知道 staging buffer 的大小？

**本项目的做法**: `load_elf` 接受 `staging_size` 参数，用它做 bounds checking。

**备选方案**: 不传 staging_size，信任段数据不会超出 buffer。

**为什么不选备选方案**: 在 bootloader 级别的代码里，"信任"是最危险的策略。如果因为某种原因（内核变大、扇区数计算错误、磁盘镜像损坏），段数据超出了实际读取的范围，memcpy 会从 staging buffer 外面的内存读到垃圾数据，然后静默地写到目标地址。这种 bug 在小数据量下不明显，但内核变大后就会暴露，而且非常难定位。bounds checking 的代价只是一次整数比较，但能防止一类严重的错误。

---

## 扩展方向

1. **动态 staging buffer 大小** — 先读 ELF header，根据 e_shoff + e_shentsize * e_shnum 计算文件总大小，动态确定需要读取的扇区数（难度：⭐⭐）
2. **Section Header 解析** — 读取 section header table，获取符号表和字符串表，为将来的内核模块加载做准备（难度：⭐⭐⭐）
3. **ELF 重定位支持** — 处理 R_X86_64_RELATIVE 等重定位类型，支持内核加载到非固定地址（难度：⭐⭐⭐）
4. **校验和验证** — 在加载完成后对每个段计算 CRC32 校验和，检测磁盘数据损坏（难度：⭐⭐）
5. **多内核启动选择** — 在磁盘上存储多个内核 ELF，通过启动菜单或命令行参数选择加载哪一个（难度：⭐⭐）

---

## 参考资料

- OSDev Wiki: [ELF](https://wiki.osdev.org/ELF) — ELF64 格式结构定义和加载步骤
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) — ATA PIO 磁盘读取的完整说明
- xv6 bootmain.c: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) — 经典的 bootloader 级 ELF 加载实现
- Intel SDM: Vol. 3A — 处理器初始化和物理地址空间布局
