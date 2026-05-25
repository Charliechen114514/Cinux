---
title: 004-boot-load-mini-kernel-b-1 · 内核加载 (B)
---

# 004 通读版 · Real Mode 完整加载小内核

到 tag 004A 为止，我们的 bootloader 已经能在 Real Mode 下完成 E820 内存探测和 ELF header 的初次读取（4KB），但离"把完整小内核加载到内存"这个目标还有一段距离。004A 留下的状态是：物理地址 0x10000 处放着 4KB 的 ELF header，Stage2 还不知道内核到底有多大、需要读多少扇区、加载到哪里。现在我们要把这件事彻底做完。

tag 004B 做的事情可以用一句话概括：**在 Real Mode 下把完整的小内核 flat binary 一次性读到目标地址，然后 Protected Mode 什么都不做，直接穿过进入 Long Mode**。听起来简单，但这一句话背后牵出了好几个设计决策和至少一个让我血压拉满的踩坑——栈冲突。我们新增了 BootInfo 结构体定义（bootloader 和内核的交接契约），把磁盘读取从 8 扇区扩展到 832 扇区（分块循环），把小内核从 ELF 格式切换到 flat binary，把加载地址从 0x10000 改到 0x20000（避开 Real Mode 栈），还给小内核加上了真正的入口代码和 BSS 清零逻辑。

这一章的内容虽然涉及 18 个文件的变更，但主题非常内聚——所有东西都围绕"怎么把小内核完整地搬进内存"这个核心问题展开。

## 架构图

先看全局启动流程，搞清楚 004B 在链条中做了什么、跳过了什么：

```
                    Cinux Tag 004B 启动流程
                    =======================

  Power On
     |
     v
  [MBR @ 0x7C00]  ---  加载 Stage2 到 0x8000，跳转
     |
     v
  [Stage2 @ 0x8000]
     ├── enable_a20
     ├── VESA 初始化 (0x6000 / 0x6200 / 0x6400)
     |
     ├── query_memory_map()         (004A, E820 -> 0x5000)
     ├── load_kernel_from_disk()    <-- CHANGED: 832 扇区 -> 0x20000
     |
     ├── 切换保护模式 (CR0.PE = 1)
     ├── [pm_entry]                 <-- 004B: 无操作，直接穿过
     ├── setup_page_tables()        (0x1000 ~ 0x3FFF)
     ├── enter_long_mode()
     |
     └── [Long Mode]
          └── hlt (004B 仍不跳转; 004C 填 BootInfo 并跳转)
```

内存布局视角——注意 004B 新增的加载区域和 BootInfo 结构体约定位置：

```
  物理地址          内容                       所属 Tag
  ──────────────────────────────────────────────────────
  0x0000~0x0400     IVT (中断向量表)            BIOS
  0x5000            E820 内存映射表              004A
  0x6000            VBE Controller Info         002
  0x6200            VBE Mode Info               002
  0x6400            Framebuffer Info            002
  0x7000            BootInfo 结构体              004B (约定位置，004C 填充)
  0x7B00            DAP (磁盘读取参数包)         004A
  0x9000            Real Mode 栈 (SS=0x0900)     001
  ──────────────────────────────────────────────────────
  0x20000           Mini Kernel (flat binary)   004B <-- 最大 416KB
  ~0x88000          Mini Kernel 结束             004B
  ── 32KB gap ──
  0x90000           Protected/Long Mode 栈      002
```

BootInfo 结构体的字段布局：

```
  BootInfo @ 0x7000
  ┌─────────────────────────────────────────┐
  │ uint64_t entry_point            (8B)    │  高半核虚拟入口地址
  │ uint64_t kernel_phys_base       (8B)    │  物理加载基址 (0x20000)
  │ uint64_t kernel_size            (8B)    │  内核大小
  ├─────────────────────────────────────────┤
  │ uint64_t fb_addr                (8B)    │  帧缓冲物理地址
  │ uint32_t fb_width               (4B)    │  宽度 (px)
  │ uint32_t fb_height              (4B)    │  高度 (px)
  │ uint32_t fb_pitch               (4B)    │  扫描线字节数
  │ uint32_t fb_bpp                 (4B)    │  位深 (通常 32)
  ├─────────────────────────────────────────┤
  │ uint32_t mmap_count             (4B)    │  内存映射条目数
  │ uint32_t _pad                   (4B)    │  对齐填充
  │ MemoryMapEntry mmap[32]   (32*24=768B)  │  E820 条目数组
  └─────────────────────────────────────────┘
  总大小 ≈ 820 字节
```

你会发现整个设计的核心思路是：在 Real Mode 里把所有需要 BIOS 中断的事情全部做完（E820 探测 + 完整磁盘读取），然后保护模式只是过渡，什么额外工作都不做，直接穿过去进 Long Mode。这种设计意味着保护模式阶段的代码极度简洁，但也意味着 Real Mode 阶段的磁盘读取必须一次性完成——不能偷懒，不能分段，416KB 的数据必须一口气搬进来。

## 代码精讲

### BootInfo 结构体（boot/boot_info.h）

我们先从 BootInfo 结构体开始看，因为它是 bootloader 和内核之间的"交接契约"——后续所有 tag 都依赖这个结构来传递启动信息。

```c
typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi;
} __attribute__((packed)) MemoryMapEntry;

static_assert(sizeof(MemoryMapEntry) == 24, "MemoryMapEntry must be 24 bytes");
```

`MemoryMapEntry` 直接对应 E820 BIOS 返回的内存映射条目格式：base 是物理基地址，length 是区域长度，type 标识内存类型（1=可用 RAM，2=保留，3=ACPI 可回收等），acpi 是 ACPI 扩展属性（通常为 0）。24 字节的大小来自 ACPI 3.0 的扩展定义——老的 BIOS 只返回 20 字节（没有 acpi 字段），但我们统一按 24 字节接收。`__attribute__((packed))` 禁止编译器插入填充字节，`static_assert` 在编译时验证大小——如果因为 ABI 差异导致结构体大小不是 24 字节，编译直接报错。

这里有一个容易忽略的关键点：这个头文件同时被两套编译环境使用。bootloader 的代码以 `-m32` 编译（32 位），内核代码以 `-m64` 编译（64 位）。如果不加 `packed`，32 位和 64 位编译器可能对结构体做不同的对齐填充，导致两边对同一块内存的解读不一致。所有字段使用显式大小的类型（uint32_t、uint64_t），配合 packed 属性，确保两个编译环境看到的内存布局完全相同。

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

`BootInfo` 结构体包含三大块信息。前三个字段是内核信息：entry_point 是高半核虚拟入口地址（0xFFFFFFFF80020000），kernel_phys_base 是物理加载基址，kernel_size 是实际大小。中间四个字段是帧缓冲信息，来自 VESA BIOS 调用保存在 0x6400 的数据。最后是 E820 内存映射数组，最多 32 条。

注意 `_pad` 字段——它在 mmap_count 之后、mmap 数组之前，把 mmap 数组对齐到 8 字节边界。虽然 packed 属性会禁止编译器自动填充，但显式写一个 padding 字段有两个好处：第一，代码的意图一目了然（读者知道这里有个对齐间隙）；第二，如果我们将来去掉 packed 属性，结构体布局不会因为这个间隙而改变。

头文件里的注释还列出了完整的地址约定表——BootInfo 结构体放在物理 0x7000，E820 缓冲区在 0x5000，帧缓冲信息在 0x6400。这些固定地址在调试时非常有用：你可以在 QEMU monitor 或者 GDB 里直接 `x/8x 0x7000` 查看 BootInfo 的内容，不用猜测数据在哪里。

### 分块磁盘读取（boot/common/boot.S — load_kernel_from_disk）

接下来是本章最核心的代码变更：`load_kernel_from_disk` 从 004A 的"读 8 个扇区"升级为"读 832 个扇区（416KB）"。这个函数的工作量暴增了两个数量级，也引入了好几个新坑。

先看常量定义的变化：

```asm
.set MINI_KERNEL_LBA,         16          // Mini kernel start LBA (sector 16)
.set MINI_KERNEL_SECTORS,     832         // Total sectors (416KB)

.set MINI_KERNEL_LOAD_PHYS,   0x20000     // Physical address where kernel is loaded
.set MINI_KERNEL_LOAD_SEG,    0x2000      // Segment: 0x2000 (0x2000 << 4 = 0x20000)
.set MINI_KERNEL_LOAD_OFF,    0x0000      // Offset

.set DISK_READ_CMD,           0x42        // INT 0x13 AH=0x42 extended read
.set DISK_MAX_SECTORS_PER_CALL, 127       // Max sectors per BIOS call (int13 limit)
```

和 004A 相比有几个关键变化。加载地址从 0x10000 变成了 0x20000——这个改动背后的故事等一下展开讲，它涉及一个很恶心的栈冲突 bug。扇区数从 8 变成了 832（416KB），这意味着不能一次读完，必须分块循环。新增了 `DISK_MAX_SECTORS_PER_CALL = 127`，这是 INT 13h 扩展读取的单次上限——有些 BIOS 实现甚至只支持更少的扇区，127 是一个经过实测的安全值。

现在看函数主体。整个函数的核心结构是一个 `.read_loop` 循环：

```asm
load_kernel_from_disk:
    pusha                                // save all 16-bit registers
    pushw %es                            // save ES
    pushw %ds                            // save DS

    xorw %bx, %bx                        // BX = 0 (no sectors read yet)
    xorw %si, %si                        // SI = 0 (buffer offset, stays 0)

    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %es
```

函数入口保存所有寄存器——注意这里从 004A 的 `pushaw` 改成了 `pusha`，两者在 16 位模式下效果一样，`pusha` 是更简洁的写法。BX 寄存器被用作"已读扇区计数器"，初始化为 0。SI 保持为 0——它在后面会被用来指向 DAP 的偏移地址。

接下来是循环本体：

```asm
.read_loop:
    cmpw $MINI_KERNEL_SECTORS, %bx       // Compare with total
    jae .read_done                       // Done if BX >= total

    movw $MINI_KERNEL_SECTORS, %ax       // AX = total sectors
    subw %bx, %ax                        // AX = remaining sectors
    cmpw $DISK_MAX_SECTORS_PER_CALL, %ax // Compare with max
    jbe .read_count_ok                   // Use AX if <= 127
    movw $DISK_MAX_SECTORS_PER_CALL, %ax // Cap at 127

.read_count_ok:
    movw %ax, %bp                        // BP = sectors to read
```

循环开头先检查是否读完了——BX（已读扇区数）大于等于 832 则退出。然后计算本次应该读多少扇区：取 min(127, 剩余扇区数)。这个计算逻辑虽然只有几条指令，但它处理了两种情况——最后一次迭代可能不满 127 扇区，这时候应该精确读取剩余量。BP 寄存器暂存本次读取量，后面更新 BX 时要用到。

接下来是 DAP（Disk Address Packet）的构建——这部分每次循环都要重新填写，因为 buffer 地址和 LBA 都在变化：

```asm
    // CRITICAL: Re-set ES before building DAP (BIOS may clobber it)
    movw $DAP_SEGMENT, %dx               // DX = 0x07B0
    movw %dx, %es                        // ES = DAP segment

    movw $DAP_OFFSET, %di                // DI = 0x0000

    movb $16, %es:(%di)                  // DAP size = 16
    movb $0, %es:1(%di)                  // DAP reserved = 0

    movw %bp, %es:2(%di)                 // DAP count = BP (sectors this round)
```

这里有一个很容易翻车的细节：每次循环迭代都必须重新设置 ES 为 DAP 段地址（0x07B0）。为什么？因为 INT 13h 调用可能破坏 ES 的值——BIOS 内部实现不保证保留调用者的段寄存器。如果你偷懒只设置一次 ES 然后循环复用，第二轮开始 ES 已经不是 0x07B0 了，你往 `es:(%di)` 写的 DAP 字段全部写到了错误的位置。这个 bug 在 QEMU 里可能不触发（QEMU 的 SeaBIOS 实现比较规矩），但在真实硬件或者 Bochs 里几乎必现。

