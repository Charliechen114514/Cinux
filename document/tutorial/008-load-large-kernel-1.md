# 008-1 从磁盘读数据：ATA PIO 驱动与 Freestanding 内存工具

> 标签：ATA PIO, LBA, I/O 端口, freestanding, 磁盘驱动, x86_64
> 前置：[007 中断系统](007-mini-kernel-intr-1.md)

## 前言——从"一次性的内核"到"加载器中的加载器"

说实话，折腾到 tag 007 为止，我们的 mini kernel 还是个很尴尬的存在——Bootloader 把它从磁盘加载到内存里，它打印一堆信息、测一下中断，然后就永远停在 `cli; hlt` 的死循环里了。你能说它在"运行"吗？倒也能，但什么正事都干不了。

问题出在哪呢？Mini kernel 受限于 Bootloader 的加载能力，只能被放在 0x20000 开始的那块区域里，最大 416KB。这个空间对于一个真正的、能跑文件系统、能跑进程调度的内核来说，根本不够看。我们需要的不是一个"一次性内核"，而是一个"加载器中的加载器"——让 mini kernel 自己充当一个迷你的 bootloader，从磁盘读取一个更大的 ELF 格式内核，把它搬到正确的内存位置，然后跳过去执行。

但这里有一个根本性的障碍：mini kernel 运行在 64 位 long mode 下，在这个模式里，实模式的 BIOS 中断服务已经完全不可用了。你在 Bootloader 阶段可以愉快地调 `INT 13h` 读磁盘，但一旦进入了保护模式再进入长模式，那条路就彻底断了。我们只能直接和硬件对话。

所以这一章我们要做的是：让 mini kernel 学会直接操作 ATA 硬盘控制器，通过 I/O 端口发送命令、轮询状态、读取扇区数据。同时还需要实现几个最基本的内存操作函数（memset/memcpy/memmove），因为 freestanding 环境下没有标准库，而后续的 ELF 加载器必须用它们来拷贝段数据和清零 BSS。

## 环境说明

这次的开发环境和前面几个 tag 一致：工具链是 GNU AS (AT&T 语法) + GCC/G++ + CMake，内核用 C++ 编写（`-ffreestanding -nostdlib -fno-exceptions -fno-rtti`），在 QEMU 里通过 `-hda disk.img -serial stdio` 运行。QEMU 模拟的硬盘是一个标准的 ATA 设备，连接在主通道（Primary Channel）上作为主盘（Master Drive），使用传统的 I/O 端口 0x1F0-0x1F7 进行通信。这些端口号是 ATA 规范固定的，不是 QEMU 自己定义的——真实硬件上也是这几个端口。

## 第一步——手写 memset、memcpy 和 memmove

你可能觉得奇怪，这三个函数不是标准库里最基础的吗？但别忘了我们的内核编译时带了 `-ffreestanding -nostdlib`，编译器不会链接任何标准库实现。这几个函数看起来不起眼，但 ELF 加载器在后面会大量使用它们——拷贝段数据需要 `memcpy`，清零 BSS 区域需要 `memset`——所以必须先把这个基础设施搞定。

我们先来看头文件 `kernel/mini/lib/string.h`。三个函数的声明被 `extern "C"` 块包裹，这是为了使用 C 语言的链接约定。如果不用 `extern "C"`，C++ 编译器会做 name mangling——把 `memset` 变成 `_Z6memsetPvim` 这样的乱码，链接器就找不到它了。笔者在这里确实踩过坑：当时忘了加 `extern "C"`，链接阶段报了一堆 undefined reference，排查了半天才意识到是 name mangling 的问题。`memcpy` 的参数还用了 `__restrict__` 关键字，告诉编译器源和目标不会指向同一块内存，编译器可以据此做更激进的优化——不过在我们逐字节拷贝的实现里，这个提示更多是语义上的声明。

```cpp
// kernel/mini/lib/string.h
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memset(void* dest, int val, size_t count);
void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count);
void* memmove(void* dest, const void* src, size_t count);

#ifdef __cplusplus
}
#endif
```

实现文件 `kernel/mini/lib/string.cpp` 的内容非常直白——逐字节操作。`memset` 把值截断为 `uint8_t` 然后逐字节填充，`memcpy` 逐字节拷贝。`memmove` 比 `memcpy` 多了一点讲究：它需要处理源地址和目标地址重叠的情况。如果目标在源前面，从前到后拷贝是安全的；如果目标在源后面，必须从后向前拷贝，否则会覆盖还没读的数据。虽然在我们当前的加载场景里不太会遇到重叠，但作为一个通用工具函数，做对了总是好的。

```cpp
// kernel/mini/lib/string.cpp
void* memset(void* dest, int val, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    uint8_t v = static_cast<uint8_t>(val);
    for (size_t i = 0; i < count; i++) {
        d[i] = v;
    }
    return dest;
}

void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

void* memmove(void* dest, const void* src, size_t count) {
    uint8_t* d = static_cast<uint8_t*>(dest);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    if (d < s) {
        for (size_t i = 0; i < count; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = count; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}
```

说实话这三个函数用逐字节实现确实不是最优的——在现代 x86 上，用 SSE/AVX 指令一次可以操作 16 甚至 32 字节。但对于教学目的来说，逐字节最清楚、最不容易出错，而且我们拷贝的数据量也不大（内核 ELF 的段数据），性能完全不是瓶颈。

## 第二步——认识 ATA 控制器和 I/O 端口

现在我们进入正题。ATA（Advanced Technology Attachment）是 x86 平台上最经典的硬盘接口协议，即使现在的主流 SATA 硬盘在软件层面也保持了 ATA 兼容性。QEMU 模拟的硬盘就是一个标准的 ATA 设备。PIO（Programmed I/O）是 ATA 传输方式中最简单的一种——CPU 主动轮询数据端口，一个字一个字地把数据从磁盘控制器读进来。没有 DMA，没有中断通知，就是纯粹的"发了命令就等着，数据准备好了就读"。效率当然不高，但在 boot 阶段没有其他进程在运行，CPU 全部时间都是我们的，简单可靠才是第一位的。

x86 平台上，主 ATA 控制器使用 `0x1F0` 到 `0x1F7` 这组 I/O 端口，外加一个控制端口 `0x3F6`。每个端口的功能是 ATA 规范固定的，不能改也不能猜。我们先来看看这些端口的布局：

- `0x1F0`：数据端口（Data Register），16 位宽度。读取扇区数据就是从这里面一个字一个字地取——每个扇区 512 字节需要 256 次 `inw` 读取
- `0x1F2`：扇区计数寄存器，告诉控制器要读几个扇区
- `0x1F3/4/5`：LBA 地址的低/中/高字节，定位要读的扇区位置
- `0x1F6`：驱动器选择寄存器，选择主/从盘以及 LBA 模式
- `0x1F7`：状态寄存器（读）/ 命令寄存器（写），这是最重要的端口

其中 LBA（Logical Block Addressing）是现代磁盘的寻址方式，你可以把它理解为"扇区编号"——LBA 0 就是第一个扇区（MBR），LBA 1 是第二个扇区，以此类推。相比古老的 CHS（柱面-磁头-扇区）寻址，LBA 就是一个简单的线性编号，对软件来说友好得多。状态寄存器的关键位包括：BSY（bit 7，驱动器忙）、DRQ（bit 3，数据就绪）、ERR（bit 0，出错）、RDY（bit 6，驱动器就绪）。

这些常量都在 `kernel/mini/driver/ata.hpp` 中定义为 `constexpr`：

```cpp
// kernel/mini/driver/ata.hpp — 关键常量
namespace cinux::mini::driver::ata {

constexpr uint16_t ATA_PRIMARY_BASE = 0x1F0;
constexpr uint16_t ATA_PRIMARY_CTRL = 0x3F6;

constexpr uint16_t ATA_REG_DATA       = 0;   // 数据端口 (16-bit)
constexpr uint16_t ATA_REG_SECTOR_CNT = 2;   // 扇区计数
constexpr uint16_t ATA_REG_LBA_LOW    = 3;   // LBA [0:7]
constexpr uint16_t ATA_REG_LBA_MID    = 4;   // LBA [8:15]
constexpr uint16_t ATA_REG_LBA_HIGH   = 5;   // LBA [16:23]
constexpr uint16_t ATA_REG_DRIVE      = 6;   // 驱动器选择
constexpr uint16_t ATA_REG_STATUS     = 7;   // 状态（读）/ 命令（写）

constexpr uint8_t ATA_STATUS_ERR  = 0x01;   // 错误
constexpr uint8_t ATA_STATUS_DRQ  = 0x08;   // 数据就绪
constexpr uint8_t ATA_STATUS_RDY  = 0x40;   // 驱动器就绪
constexpr uint8_t ATA_STATUS_BSY  = 0x80;   // 驱动器忙

constexpr uint8_t ATA_CMD_READ_PIO     = 0x20;  // LBA28 读
constexpr uint8_t ATA_CMD_READ_PIO_EXT = 0x24;  // LBA48 读
constexpr uint8_t ATA_DRIVE_MASTER     = 0xE0;  // 主盘 + LBA 模式

constexpr uint16_t ATA_SECTOR_SIZE = 512;

bool init();
bool read(uint64_t lba, uint16_t count, void* buffer);
}
```

你可能注意到了 `ATA_DRIVE_MASTER = 0xE0` 这个值看起来有点随意——其实不是。它的 bit 7 和 bit 5 必须置 1（ATA 规范要求的），bit 6 是 LBA 模式使能位。所以 `0xE0 = 1110_0000b` 就是"选主盘 + LBA 模式"。如果你手滑把 LBA 位忘了，控制器会按 CHS 模式寻址，读出来的数据完全不对——这种 bug 排查起来能让你怀疑人生，因为代码逻辑看起来完全没问题。

### 其他 OS 怎么做磁盘读取？

说到这里，我们可以横向对比一下其他 OS 的做法。xv6 的 `bootmain()` 是 OS 开发领域最经典的磁盘读取代码之一——它在 32 位保护模式下直接操作 IDE 端口（0x1F0-0x1F7）读取内核 ELF，和我们做的事情完全一样，只不过 xv6 是 32 位系统而我们是在 64 位 long mode 下操作。xv6 把整个磁盘读取+ELF 加载的代码塞在 `bootmain.c` 这一个文件里，大约 100 行 C 代码，风格非常紧凑——先读 ELF header 到 0x10000，验证 magic，然后遍历 program headers 逐段从磁盘读到目标地址。和 Cinux 的设计相比，xv6 更"原始"——它在 bootloader 里完成所有工作，而 Cinux 在 mini kernel（有完整 C++ 环境、有 kprintf、有 PMM）里做，能力更强但代码量也更大。

Linux 走了一条完全不同的路——它把 bootloader 级别的磁盘读取外包给 GRUB。GRUB 是一个功能完善的 bootloader，支持 ext2/3/4、FAT 等多种文件系统，能直接从 `/boot` 目录读取 `vmlinuz`（压缩内核）和 `initramfs`。Linux 内核自身不需要实现 ELF 加载器——GRUB 负责解析 ELF/Multiboot 头、加载内核到正确位置、传递启动信息。这种方式对用户最友好（你不需要关心磁盘几何结构这些底层细节），但代价是你失去了对启动流程的完全控制。Cinux 和 xv6 选择了"自己动手"的道路，虽然更累但更有教学价值——你能理解从硬件信号到软件逻辑的每一步。

SerenityOS 和 Linux 一样使用 GRUB + Multiboot 规范启动，内核编译为 ELF 格式并在头部包含 Multiboot magic（0x1BADB002）。GRUB 负责所有底层磁盘 I/O，SerenityOS 的内核代码因此更简洁——不需要在启动早期实现 ATA 驱动。这种设计的选择本质上是一个权衡：你是想要简洁的内核代码（依赖 GRUB），还是想要理解完整的启动链条（自己实现）。从教学角度来看，Cinux/xv6 的方式显然更有价值。

## 第三步——实现 ATA PIO 驱动的辅助函数

驱动实现文件 `kernel/mini/driver/ata.cpp` 里的辅助函数都放在匿名命名空间中（相当于 `static`），对外不可见。核心辅助函数包括：`read_reg`/`write_reg`（封装 `inb`/`outb` 加上基址偏移）、`read_data`（用 `inw` 读 16 位数据端口）、`wait_not_busy`（轮询直到 BSY 清零）、`wait_data_ready`（轮询直到 DRQ 置位且 BSY 清零，同时检查错误）、`delay_400ns`（读控制端口 4 次）。

这里有一个值得细说的细节。`read_data()` 使用 `inw` 指令而不是 `inb`，因为 ATA 数据端口是 16 位宽的——每次读一个 word（2 字节），256 次 word 读取刚好是一个 512 字节的扇区。我们的 io 库只提供了 8 位的 `inb`/`outb`，所以 `read_data` 必须用内联汇编实现。

```cpp
inline uint16_t read_data() {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(ATA_PRIMARY_BASE));
    return value;
}
```

`delay_400ns` 函数看似简单——读控制端口 4 次——但背后的原理值得了解。ATA 规范要求发送命令后至少等待 400ns 才能读状态寄存器，原因是磁盘控制器在收到命令后需要一点时间来设置内部状态。在真实硬件上读一次 I/O 端口大约需要 100ns（经历 PCI/ISA 总线延迟），所以 4 次读取刚好满足要求。关键是我们读的是控制端口（0x3F6）而不是状态端口（0x1F7）——读状态端口有清除中断标志位的副作用。虽然我们用的是轮询模式不依赖中断，但养成读控制端口做延迟的好习惯很重要，将来切换到中断驱动模式时就不会踩坑。

轮询函数 `wait_not_busy` 和 `wait_data_ready` 都有超时机制（10,000,000 次循环），并且循环体里有 `__asm__ volatile("pause")` 指令——这是 x86 的 hint 指令，告诉 CPU 当前在等待某个条件，可以降低功耗并减少总线争用。`wait_data_ready` 还在轮询时先检查 ERR 和 DF 位——这个顺序很关键，如果先检查 DRQ，可能刚好在错误发生时 DRQ 还是 1，导致读到错误数据。

## 第四步——ATA 初始化：软件复位

ATA 控制器的初始化遵循一个标准流程：软件复位 → 等待就绪 → 选择驱动器 → 验证存在性。软件复位通过控制端口 0x3F6 操作——先写入 0x04（bit 1 = SRST 复位，bit 2 = nIEN 禁用中断），等 400ns，再写入 0x00 取消复位。nIEN 位持续禁用中断，因为我们用轮询模式，不需要磁盘控制器发送 IRQ。

```cpp
bool init() {
    kprintf("[INIT] Initializing ATA controller...\n");

    io::outb(ATA_PRIMARY_CTRL, 0x04);   // SRST=1, nIEN=1
    delay_400ns();
    io::outb(ATA_PRIMARY_CTRL, 0x00);   // Clear reset
    delay_400ns();

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: drive did not come out of reset\n");
        return false;
    }

    write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER);  // 选主盘 + LBA
    delay_400ns();

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: master drive not ready\n");
        return false;
    }

    uint8_t status = read_reg(ATA_REG_STATUS);
    if (status == 0xFF) {
        kprintf("[ATA] ERROR: no drive detected (floating bus)\n");
        return false;
    }
    if ((status & ATA_STATUS_RDY) == 0) {
        kprintf("[ATA] ERROR: drive not ready, status=0x%02x\n", status);
        return false;
    }

    s_initialized = true;
    kprintf("[INIT] ATA controller initialized successfully (status=0x%02x).\n", status);
    return true;
}
```

验证驱动器存在性的方法很巧妙：如果总线上没有设备，所有端口读出来的值都是 0xFF——这是浮空总线的高阻态上拉特征。所以 `status == 0xFF` 就是"没检测到设备"的标志。正常情况下 QEMU 的 ATA 设备初始化后 status 应该包含 RDY 位（bit 6 置 1），通常是 0x50。

## 第五步——扇区读取：LBA28 与 LBA48

`read()` 函数是 ATA PIO 驱动的核心，参数校验非常全面——检查驱动器是否初始化、扇区数是否为零、缓冲区是否为空、LBA 是否超出 48 位范围。然后根据 LBA 地址和扇区数自动选择寻址模式：LBA 超过 28 位范围（0x10000000）或扇区数超过 256 时使用 LBA48，否则使用 LBA28。

```cpp
// LBA28 命令序列
write_reg(ATA_REG_DRIVE,
          ATA_DRIVE_MASTER | static_cast<uint8_t>((lba >> 24) & 0x0F));
delay_400ns();
write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>(lba & 0xFF));
write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 8) & 0xFF));
write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 16) & 0xFF));
write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO);   // 0x20
```

LBA28 把 24 位 LBA 分散到三个端口，高 4 位塞进驱动器选择寄存器的低 4 位。LBA48 的命令序列更长——先发送高 16 位（扇区计数高字节、LBA[24:47]），再发送低 16 位。这种"先高后低"的顺序是 ATA 规范规定的，目的是让控制器在收到低字节时就能开始处理。

命令发送后进入逐扇区读取循环：每个扇区先等 400ns 延迟，然后等数据就绪，接着从数据端口连续读取 256 个 16 位 word。缓冲区指针每次前进 256 个 word（因为是 `uint16_t*`，所以 `buf += 256` 实际前进 512 字节）。有一个容易忽略的细节：LBA28 模式下扇区计数寄存器值为 0 实际表示 256 个扇区（不是 0 个！），LBA48 的 0 则表示 65536 个扇区。

## 收尾——验证与运行

在 `main.cpp` 中，我们通过两个 demo 测试验证整个管线。第一个读取 MBR（LBA 0）验证引导签名 0xAA55——这是最经典的"第一个磁盘读取测试"，因为 MBR 的引导签名是固定的、已知的值，如果 ATA 驱动工作正常就一定能读到。

```
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xAA55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
```

到这里，mini kernel 已经具备了从磁盘读取任意扇区数据的能力。接下来的第二章里，我们会基于这个 ATA 驱动实现 ELF 解析器和大内核加载器，完成从"读磁盘"到"加载内核"的最后一步。

## 参考资料

- Intel SDM: Vol. 2A — IN/OUT 指令（inw/outb 的编码和操作数约束）
  - Source: `helpers/web_search/intel_sdm/vol2a_io_port_instructions.md`
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) — PIO 模式完整说明、端口布局、400ns 延迟要求
- OSDev Wiki: [ATA read/write sectors](https://wiki.osdev.org/ATA_read/write_sectors) — LBA28/LBA48 代码示例
- xv6 bootmain.c: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) — 经典 bootloader 级 IDE PIO 磁盘读取
- Linux boot process: [linux-insides](https://0xax.gitbook.io/linux-insides/summary/booting/linux-bootstrap-1) — GRUB 加载内核的完整流程
- SerenityOS: [GitHub](https://github.com/SerenityOS/serenity) — GRUB + Multiboot 启动方式
