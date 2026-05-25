---
title: 008-load-large-kernel-1 · 大内核加载
---

# 008-1 从磁盘读数据：ATA PIO 驱动与 Freestanding 内存工具

> 标签：ATA PIO, LBA, I/O 端口, freestanding, 磁盘驱动, x86_64
> 前置：[007 中断系统](007-mini-kernel-intr-1.md)

## 前言——从"一次性的内核"到"加载器中的加载器"

说实话，折腾到 tag 007 为止，我们的 mini kernel 还是个很尴尬的存在——Bootloader 把它从磁盘加载到内存里，它打印一堆信息、测一下中断，然后就永远停在 `cli; hlt` 的死循环里了。你能说它在"运行"吗？倒也能，但什么正事都干不了。

问题出在哪呢？Mini kernel 受限于 Bootloader 的加载能力，只能被放在 0x20000 开始的那块区域里，最大 416KB。这个空间对于一个真正的、能跑文件系统、能跑进程调度的内核来说，根本不够看。我们需要的不是一个"一次性内核"，而是一个"加载器中的加载器"——让 mini kernel 自己充当一个迷你的 bootloader，从磁盘读取一个更大的 ELF 格式内核，把它搬到正确的内存位置，然后跳过去执行。

但这里有一个根本性的障碍：mini kernel 运行在 64 位 long mode 下，在这个模式里，实模式的 BIOS 中断服务已经完全不可用了。你在 Bootloader 阶段可以愉快地调 `INT 13h` 读磁盘，但一旦进入了保护模式再进入长模式，那条路就彻底断了。我们只能直接和硬件对话。

所以这一章我们要做的是：让 mini kernel 学会直接操作 ATA 硬盘控制器，通过 I/O 端口发送命令、轮询状态、读取扇区数据。同时还需要实现几个最基本的内存操作函数（memset/memcpy/memmove），因为 freestanding 环境下没有标准库，而后续的 ELF 加载器必须用它们来拷贝段数据和清零 BSS。

## 环境说明

这次的开发环境和前面几个 tag 一致：工具链是 GNU AS (AT&T 语法) + GCC/G++ + CMake，内核用 C++ 编写（`-ffreestanding -nostdlib -fno-exceptions -fno-rtti`），在 QEMU 里通过 `-hda disk.img -serial stdio` 运行。QEMU 模拟的硬盘是一个标准的 ATA 设备，连接在主通道（Primary Channel）上作为主盘（Master Drive），使用传统的 I/O 端口 0x1F0-0x1F7 进行通信。

## 第一步——手写 memset、memcpy 和 memmove

你可能觉得奇怪，这三个函数不是标准库里最基础的吗？但别忘了我们的内核编译时带了 `-ffreestanding -nostdlib`，编译器不会链接任何标准库实现。这几个函数看起来不起眼，但 ELF 加载器在后面会大量使用它们——拷贝段数据需要 `memcpy`，清零 BSS 区域需要 `memset`——所以必须先把这个基础设施搞定。

我们先来看头文件 `kernel/mini/lib/string.h`。三个函数的声明被 `extern "C"` 块包裹，这是为了使用 C 语言的链接约定。如果不用 `extern "C"`，C++ 编译器会做 name mangling——把 `memset` 变成 `_Z6memsetPvim` 这样的乱码，链接器就找不到它了。

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

实现文件 `kernel/mini/lib/string.cpp` 的内容非常直白——逐字节操作。`memset` 把值截断为 `uint8_t` 然后逐字节填充，`memcpy` 逐字节拷贝。`memmove` 比 `memcpy` 多了一点讲究：它需要处理源地址和目标地址重叠的情况。如果目标在源前面，从前到后拷贝是安全的；如果目标在源后面，必须从后向前拷贝，否则会覆盖还没读的数据。

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
// kernel/mini/driver/ata.hpp -- 关键常量
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

`ATA_DRIVE_MASTER = 0xE0` 这个值看起来有点随意——其实不是。它的 bit 7 和 bit 5 必须置 1（ATA 规范要求的），bit 6 是 LBA 模式使能位。所以 `0xE0 = 1110_0000b` 就是"选主盘 + LBA 模式"。如果你手滑把 LBA 位忘了，控制器会按 CHS 模式寻址，读出来的数据完全不对。

## 第三步——实现 ATA PIO 驱动的辅助函数

驱动实现文件 `kernel/mini/driver/ata.cpp` 里的辅助函数都放在匿名命名空间中（相当于 `static`），对外不可见。核心辅助函数包括：`read_reg`/`write_reg`（封装 `inb`/`outb` 加上基址偏移）、`read_data`（用 `inw` 读 16 位数据端口）、`wait_not_busy`（轮询直到 BSY 清零）、`wait_data_ready`（轮询直到 DRQ 置位且 BSY 清零，同时检查错误）、`delay_400ns`（读控制端口 4 次）。

`read_data()` 使用 `inw` 指令而不是 `inb`，因为 ATA 数据端口是 16 位宽的——每次读一个 word（2 字节），256 次 word 读取刚好是一个 512 字节的扇区。

```cpp
inline uint16_t read_data() {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(ATA_PRIMARY_BASE));
    return value;
}
```

`delay_400ns` 函数看似简单——读控制端口 4 次——但背后的原理值得了解。ATA 规范要求发送命令后至少等待 400ns 才能读状态寄存器。关键是我们读的是控制端口（0x3F6）而不是状态端口（0x1F7）——读状态端口有清除中断标志位的副作用。

轮询函数 `wait_not_busy` 和 `wait_data_ready` 都有超时机制（10,000,000 次循环），并且循环体里有 `__asm__ volatile("pause")` 指令。`wait_data_ready` 还在轮询时先检查 ERR 和 DF 位——这个顺序很关键。

## 收尾——下一步

到这里，freestanding 工具函数和 ATA 驱动的框架都搭建完毕了。下一章里，我们会实现 ATA 初始化和扇区读取的核心函数，验证整个磁盘读取管线。

## 参考资料

- Intel SDM: Vol. 2A -- IN/OUT 指令（inw/outb 的编码和操作数约束）
  - Source: `helpers/web_search/intel_sdm/vol2a_io_port_instructions.md`
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) -- PIO 模式完整说明、端口布局、400ns 延迟要求
- OSDev Wiki: [ATA read/write sectors](https://wiki.osdev.org/ATA_read/write_sectors) -- LBA28/LBA48 代码示例
