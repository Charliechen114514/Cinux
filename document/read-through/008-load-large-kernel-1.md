# 008-1 Read-through: ATA PIO 磁盘驱动与 Freestanding 工具函数

## 概览

本文是 tag 008（load-large-kernel）代码讲解的第一部分，聚焦于两个基础模块：freestanding 内存工具函数（`lib/string.h/cpp`）和 ATA PIO 磁盘驱动（`driver/ata.hpp/cpp`）。这两个模块是大内核加载管线的基础设施——前者提供内存操作原语，后者提供磁盘读取能力。在下一篇文章中，我们会基于这两个模块实现 ELF 解析器和大内核加载器。

关键设计决策一览：ATA 驱动采用轮询式 PIO 而非 DMA 或中断驱动模式，追求简单可靠；freestanding 工具函数用逐字节实现而非汇编优化，追求代码清晰度；LBA28/LBA48 双模式自动切换，兼顾兼容性和扩展性。

---

## 架构图

```
mini kernel 启动流程 (tag 008):

main.cpp
  │
  ├── GDT + IDT + PMM 初始化 (前面 tag 已实现)
  │
  ├── driver/ata::init()          ← 本文重点
  │     ├── 软件复位 (控制端口 0x3F6)
  │     ├── 等待 BSY 清零
  │     ├── 选择主盘 (0xE0)
  │     └── 验证 RDY 状态
  │
  ├── driver/ata::read(0, 1, buf) ← 本文重点
  │     ├── LBA28/LBA48 模式选择
  │     ├── 发送 READ SECTORS 命令
  │     └── 轮询 DRQ → 读 256 words × count 扇区
  │
  └── elf_loader::parse_elf_header(buf) ← 下一篇重点

依赖关系:
┌─────────────┐     ┌──────────────┐
│ lib/string  │────→│  elf_loader  │  (memcpy/memset)
│ (memset,    │     │  (下一篇)     │
│  memcpy,    │     └──────────────┘
│  memmove)   │
└─────────────┘
┌─────────────┐     ┌──────────────────┐
│ driver/ata  │────→│ big_kernel_loader│  (read sectors)
│ (init, read)│     │  (下一篇)         │
└─────────────┘     └──────────────────┘
```

---

## 代码精讲

### Freestanding 内存工具函数

#### lib/string.h — 头文件

```cpp
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Fill memory region with a byte value
void* memset(void* dest, int val, size_t count);

/// Copy memory region (source and dest must not overlap)
void* memcpy(void* __restrict__ dest, const void* __restrict__ src, size_t count);

/// Copy memory region (source and dest may overlap)
void* memmove(void* dest, const void* src, size_t count);

#ifdef __cplusplus
}
#endif
```

这三个函数是 freestanding 环境下最基本的内存操作原语。头文件用 `#ifdef __cplusplus` + `extern "C"` 包裹，确保在 C++ 代码中调用时使用 C 语言的链接约定（不做 name mangling）。`__restrict__` 关键字告诉编译器 `dest` 和 `src` 不会指向同一块内存区域，编译器可以据此做更激进的优化——虽然在我们逐字节拷贝的实现里效果有限，但这是语义上的正确声明。

这三个函数的声明看似简单，但在 freestanding 环境下是不可或缺的。ELF 加载器在拷贝段数据和清零 BSS 时需要 `memcpy` 和 `memset`，如果编译器因为 `-ffreestanding -nostdlib` 找不到这些符号，链接阶段就会报 undefined reference。

#### lib/string.cpp — 实现

```cpp
#include "string.h"

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
        for (size_t i = 0; i < count; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = count; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dest;
}
```

三个函数都是最朴素的逐字节实现。`memset` 把 `val` 截断为 `uint8_t` 然后逐字节填充，`memcpy` 逐字节从源拷贝到目标。`memmove` 是这里面最有趣的——它处理了源和目标重叠的情况。当目标地址在源地址之前时（`d < s`），从前向后拷贝是安全的；当目标地址在源地址之后时（`d > s`），必须从后向前拷贝，否则前面的数据会被覆盖，后面的拷贝就会读到错误的数据。当 `d == s` 时什么都不做，因为源和目标完全重合。

说实话这三个函数用逐字节实现确实不是最优的——在现代 x86 上，用 SSE/AVX 指令一次可以拷贝 16 甚至 32 字节，速度快得多。但对于教学目的来说，逐字节实现最清楚、最不容易出错，而且我们拷贝的数据量也不大（内核 ELF 的段数据通常不超过几百 KB），性能不是瓶颈。

---

### ATA PIO 驱动

#### driver/ata.hpp — 头文件与常量定义

```cpp
namespace cinux::mini::driver::ata {

constexpr uint16_t ATA_PRIMARY_BASE = 0x1F0;
constexpr uint16_t ATA_PRIMARY_CTRL = 0x3F6;

constexpr uint16_t ATA_REG_DATA       = 0;
constexpr uint16_t ATA_REG_ERROR      = 1;
constexpr uint16_t ATA_REG_FEATURES   = 1;
constexpr uint16_t ATA_REG_SECTOR_CNT = 2;
constexpr uint16_t ATA_REG_LBA_LOW    = 3;
constexpr uint16_t ATA_REG_LBA_MID    = 4;
constexpr uint16_t ATA_REG_LBA_HIGH   = 5;
constexpr uint16_t ATA_REG_DRIVE      = 6;
constexpr uint16_t ATA_REG_STATUS     = 7;
constexpr uint16_t ATA_REG_COMMAND    = 7;

constexpr uint8_t ATA_STATUS_ERR  = 0x01;
constexpr uint8_t ATA_STATUS_DRQ  = 0x08;
constexpr uint8_t ATA_STATUS_DF   = 0x20;
constexpr uint8_t ATA_STATUS_RDY  = 0x40;
constexpr uint8_t ATA_STATUS_BSY  = 0x80;

constexpr uint8_t ATA_CMD_READ_PIO        = 0x20;
constexpr uint8_t ATA_CMD_READ_PIO_EXT    = 0x24;
constexpr uint8_t ATA_CMD_IDENTIFY        = 0xEC;

constexpr uint8_t ATA_DRIVE_MASTER = 0xE0;
constexpr uint8_t ATA_DRIVE_LBA48  = 0x40;

constexpr uint16_t ATA_SECTOR_SIZE = 512;

bool init();
bool read(uint64_t lba, uint16_t count, void* buffer);
}
```

头文件把所有 ATA 相关的常量定义为 `constexpr`，这比用 `#define` 宏好得多——`constexpr` 有类型安全、有作用域、在调试器里可见。注意 `ATA_REG_ERROR` 和 `ATA_REG_FEATURES` 共用偏移 1——读操作时是错误寄存器，写操作时是特性寄存器，和状态/命令寄存器共用端口的原理一样。

`ATA_DRIVE_MASTER = 0xE0` 的含义需要拆开来看：bit 7 和 bit 5 必须置 1（ATA 规范要求），bit 6 是 LBA 模式使能位，低 4 位在 LBA28 模式下承载 LBA 的最高 4 位。所以 `0xE0 = 1110_0000b` 就是"选主盘 + LBA 模式 + LBA[24:27] = 0"。

公开接口只有两个函数：`init()` 初始化控制器，`read()` 读取扇区。精简的接口反映了 ATA PIO 驱动在 mini kernel 中的角色——一个一次性的、只读的、轮询式的磁盘读取工具。

#### driver/ata.cpp — 内部辅助函数

```cpp
namespace cinux::mini::driver::ata {

static bool s_initialized = false;

namespace {

inline uint8_t read_reg(uint16_t reg) {
    return io::inb(ATA_PRIMARY_BASE + reg);
}

inline void write_reg(uint16_t reg, uint8_t value) {
    io::outb(ATA_PRIMARY_BASE + reg, value);
}

inline uint16_t read_data() {
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(ATA_PRIMARY_BASE));
    return value;
}

bool wait_not_busy() {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = read_reg(ATA_REG_STATUS);
        if ((status & ATA_STATUS_BSY) == 0) {
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

bool wait_data_ready() {
    for (uint32_t i = 0; i < 10000000; i++) {
        uint8_t status = read_reg(ATA_REG_STATUS);

        if (status & ATA_STATUS_ERR) {
            kprintf("[ATA] ERROR: drive error, status=0x%02x, error=0x%02x\n",
                    status, read_reg(ATA_REG_ERROR));
            return false;
        }
        if (status & ATA_STATUS_DF) {
            kprintf("[ATA] ERROR: drive fault, status=0x%02x\n", status);
            return false;
        }

        if ((status & ATA_STATUS_BSY) == 0 && (status & ATA_STATUS_DRQ)) {
            return true;
        }

        __asm__ volatile("pause");
    }
    kprintf("[ATA] ERROR: timeout waiting for data ready\n");
    return false;
}

void delay_400ns() {
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
    io::inb(ATA_PRIMARY_CTRL);
}

}  // anonymous namespace
```

辅助函数都放在匿名命名空间里，相当于 `static`，对外不可见。`read_reg` 和 `write_reg` 是对 `io::inb`/`io::outb` 的简单封装，加上基址偏移。`read_data` 则不同——它使用 `inw` 指令读取 16 位值，因为 ATA 数据端口是 16 位宽的。这里必须用内联汇编而不是调用 io 库函数，因为我们的 io 库只提供了 8 位的 `inb`/`outb`。

`wait_not_busy` 和 `wait_data_ready` 是两个轮询函数，都有一个 10,000,000 次的超时上限。在 QEMU 里，ATA 命令几乎是瞬时完成的，但在真实硬件上可能需要等更长时间。轮询循环里的 `__asm__ volatile("pause")` 是 x86 的 hint 指令，告诉 CPU 当前在等待某个条件，可以降低功耗并减少总线争用——这在长时间轮询时能改善系统行为。

`wait_data_ready` 比 `wait_not_busy` 多了错误检查——它在轮询时先检查 ERR 和 DF 位。这个顺序很重要：如果先检查 DRQ，可能刚好在错误发生的时候 DRQ 还是 1（因为错误状态也可能置位 DRQ），导致读到错误数据。先检查错误可以避免这种边界情况。

`delay_400ns` 读取控制端口（0x3F6）4 次。我们读控制端口而不是状态端口（0x1F7）是有原因的——读状态端口有清除中断标志位的副作用。虽然我们用的是轮询模式不依赖中断，但养成读控制端口做延迟的好习惯，将来切换到中断驱动模式时就不会踩坑。

#### driver/ata.cpp — init() 初始化

```cpp
bool init() {
    kprintf("[INIT] Initializing ATA controller...\n");

    // Step 1: Software reset
    io::outb(ATA_PRIMARY_CTRL, 0x04);  // SRST=1, nIEN=1
    delay_400ns();
    io::outb(ATA_PRIMARY_CTRL, 0x00);  // Clear reset
    delay_400ns();

    // Step 2: Wait for drive to come out of reset
    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: drive did not come out of reset (BSY timeout)\n");
        return false;
    }

    // Step 3: Select master drive with LBA mode
    write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER);
    delay_400ns();

    // Step 4: Verify drive is present and ready
    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: master drive not ready after selection\n");
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

初始化过程遵循 ATA 规范的标准流程。软件复位通过控制端口操作：写入 0x04（bit 1 = SRST 软件复位，bit 2 = nIEN 禁用中断），等 400ns，再写入 0x00 取消复位。nIEN 位保持禁用中断状态——因为我们用轮询模式，不需要磁盘控制器发送中断。

选择主盘时写入 `ATA_DRIVE_MASTER`（0xE0），这个值的 bit 6 置位启用 LBA 寻址模式。选择驱动器之后又等了 400ns——ATA 规范要求在驱动器切换后等待足够时间让新选中的驱动器响应。

验证驱动器存在性的方法很巧妙：如果总线上没有设备，所有端口读出来的值都是 0xFF（浮空总线的高阻态上拉）。所以 `status == 0xFF` 就是"没检测到设备"的标志。正常情况下，QEMU 的 ATA 设备初始化后 status 应该是 0x50（RDY=1，DSC=1，其余为 0）或者类似值。

#### driver/ata.cpp — read() 扇区读取

```cpp
bool read(uint64_t lba, uint16_t count, void* buffer) {
    // Parameter validation
    if (!s_initialized) { /* error */ return false; }
    if (count == 0) { /* error */ return false; }
    if (buffer == nullptr) { /* error */ return false; }
    if (lba >= (1ULL << 48)) { /* error */ return false; }

    if (!wait_not_busy()) { /* error */ return false; }

    // Determine addressing mode
    bool use_lba48 = (lba >= 0x10000000ULL) || (count > 256);

    if (use_lba48) {
        write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER | 0x40);
        delay_400ns();

        // High order bytes first
        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>((count >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>((lba >> 24) & 0xFF));
        write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 32) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 40) & 0xFF));

        // Low order bytes second
        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
        write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>(lba & 0xFF));
        write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 16) & 0xFF));

        write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
    } else {
        // LBA28
        write_reg(ATA_REG_DRIVE,
                  ATA_DRIVE_MASTER | static_cast<uint8_t>((lba >> 24) & 0x0F));
        delay_400ns();

        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
        write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>(lba & 0xFF));
        write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 16) & 0xFF));

        write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    }

    // Read sectors
    auto* buf = static_cast<uint16_t*>(buffer);
    for (uint16_t sector = 0; sector < count; sector++) {
        delay_400ns();
        if (!wait_data_ready()) { /* error */ return false; }

        for (int word = 0; word < 256; word++) {
            buf[word] = read_data();
        }
        buf += 256;
    }
    return true;
}
```

`read()` 函数是 ATA PIO 驱动的核心。它的参数校验很全面——检查驱动器是否初始化、扇区数是否为零、缓冲区是否为空、LBA 是否超出 48 位范围。这些防御性检查在 bootloader 级别的代码里特别重要，因为这里的 bug 通常表现为"静默地读到垃圾数据"，排查起来非常痛苦。

LBA28 和 LBA48 的命令序列有显著差异。LBA28 模式把 LBA 的高 4 位（bit 24-27）编码到驱动器选择寄存器的低 4 位，扇区计数只发一次（8 位，最多 256 个扇区），LBA 的低 24 位分别写入三个 LBA 端口。LBA48 模式则发送两次——第一次发送高 16 位（LBA[24:47] 和扇区计数高字节），第二次发送低 16 位（LBA[0:15] 和扇区计数低字节）。这种"先高后低"的顺序是 ATA 规范规定的，目的是让控制器在收到低字节时就能开始处理，而不必等所有字节到齐。

数据读取的核心循环很直观：对每个扇区，先等 400ns，然后等数据就绪（DRQ=1, BSY=0），接着连续读 256 个 16 位 word。缓冲区指针 `buf` 是 `uint16_t*` 类型，每次读一个 word 后指针自动前进 2 字节——所以读完一个扇区后 `buf += 256` 刚好前进 512 字节。

有一个值得注意的细节：LBA28 模式下 `count` 参数是 `uint16_t`，但写入扇区计数寄存器时做了 `& 0xFF` 截断为 8 位。在 LBA28 模式下，扇区计数寄存器为 0 表示 256 个扇区——所以当 count 恰好是 256 时，写入 0 是正确的。这也是为什么切换到 LBA48 的条件包含 `count > 256`——超过 256 就必须用 LBA48 了。

---

## 设计决策

### 决策：PIO 轮询 vs DMA vs 中断驱动

**问题**: 磁盘读取有三种基本策略——PIO 轮询、PIO + 中断、DMA。选择哪种？

**本项目的做法**: PIO 轮询——CPU 发命令后主动轮询状态寄存器，等数据就绪后逐字读取。

**备选方案 1**: PIO + 中断——发命令后 CPU 做别的事，磁盘数据就绪时触发 IRQ14/IRQ15 中断。
**备选方案 2**: DMA——配置 DMA 引擎，让磁盘控制器直接把数据写到内存，完成后中断通知。

**为什么不选备选方案**: 在 bootloader/mini kernel 阶段，没有其他进程在运行，轮询的"CPU 浪费"不是问题。DMA 需要配置 PCI/ISA 总线 master、处理物理地址映射、管理 buffer descriptor，实现复杂度远高于 PIO。中断驱动 PIO 虽然不复杂，但需要先设置好 IDT 中的 IRQ handler——虽然我们已经有了 IDT（tag 007），但为了保持启动流程的简洁性，还是用轮询。如果将来需要支持并发磁盘 I/O，再切换到中断驱动。

**如果要扩展/改进**: 可以用 SIMD 指令（movdqa）加速数据端口的读取——`rep insw` 指令一次只读 16 位，但理论上可以用更大的寄存器宽度。另外可以实现 IDENTIFY DEVICE 命令来检测磁盘容量和特性，动态选择最优传输模式。

### 决策：LBA28/LBA48 双模式自动切换

**问题**: 支持 LBA28 就够了（QEMU 默认磁盘很小），还是需要实现 LBA48？

**本项目的做法**: 两者都实现，根据 LBA 地址和扇区数自动选择。

**备选方案**: 只实现 LBA28——代码更简单，QEMU 默认配置下够用。

**为什么不选备选方案**: LBA48 的实现代码量不大（约 20 行），而且将来对接更大的虚拟磁盘或真实硬件时不用改驱动代码。自动切换的逻辑也很直观——检查 LBA 和 count 的范围即可。教学价值方面，LBA48 的"先高后低"两次发送机制本身就是一个有趣的知识点。

---

## 扩展方向

1. **IDENTIFY DEVICE 命令** — 实现 `ATA_CMD_IDENTIFY`（0xEC），获取磁盘容量、型号、序列号等信息（难度：⭐）
2. **ATA 写入支持** — 在 read 基础上添加 write 函数，命令码 0x30（LBA28）/ 0x34（LBA48），数据方向改为 outsw（难度：⭐⭐）
3. **多扇区流水线读取** — 利用 ATA 控制器的内部缓冲区，在读取当前扇区时命令控制器准备下一个扇区，减少轮询等待时间（难度：⭐⭐）
4. **IDE IDENTIFY 检测总线浮动** — 在 init() 中发送 IDENTIFY 命令前先检测设备是否真的存在，避免向不存在的设备发送命令（难度：⭐）
5. **SIMD 优化的数据传输** — 使用 SSE 的 movdqa 指令替代 inw 循环，一次读取 128 位数据（难度：⭐⭐⭐）

---

## 参考资料

- Intel SDM: Vol. 2A — IN/OUT 指令参考（inw/outb 的编码和约束）
  - Source: `helpers/web_search/intel_sdm/vol2a_io_port_instructions.md`
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) — PIO 模式的完整说明、端口布局、命令序列
- OSDev Wiki: [ATA read/write sectors](https://wiki.osdev.org/ATA_read/write_sectors) — LBA28/LBA48 汇编代码示例
- OSDev Wiki: [ELF](https://wiki.osdev.org/ELF) — ELF64 格式结构参考
