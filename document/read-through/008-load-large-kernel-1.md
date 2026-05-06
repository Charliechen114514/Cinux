# 008-1 Read-through: Freestanding 工具函数与 ATA PIO 驱动框架

## 概览

本文是 tag 008（load-large-kernel）代码讲解的第一部分，聚焦于两个基础模块：freestanding 内存工具函数（`lib/string.h/cpp`）和 ATA PIO 驱动的头文件与辅助函数（`driver/ata.hpp` 中的常量定义和 `ata.cpp` 中的内部辅助函数）。这两个模块是大内核加载管线的基础设施——前者提供内存操作原语，后者提供磁盘读取常量和底层轮询能力。

关键设计决策一览：freestanding 工具函数用逐字节实现而非汇编优化，追求代码清晰度；ATA 常量全部使用 `constexpr` 而非宏定义，确保类型安全；轮询函数有超时上限并使用 `pause` 指令，兼顾正确性和性能。

---

## 架构图

```
mini kernel 启动流程 (tag 008):

main.cpp
  |
  +-- GDT + IDT + PMM 初始化 (前面 tag 已实现)
  |
  +-- driver/ata::init()          <-- 本篇重点 (常量 + 辅助函数)
  |     +-- 软件复位 (控制端口 0x3F6)
  |     +-- 等待 BSY 清零
  |     +-- 选择主盘 (0xE0)
  |     +-- 验证 RDY 状态
  |
  +-- driver/ata::read(0, 1, buf) <-- 下一篇重点
  |     +-- LBA28/LBA48 模式选择
  |     +-- 发送 READ SECTORS 命令
  |     +-- 轮询 DRQ -> 读 256 words x count 扇区
  |
  +-- elf_loader::parse_elf_header(buf) <-- 下一篇重点

依赖关系:
+-------------+     +---------------+
| lib/string  |---->|  elf_loader   |  (memcpy/memset)
| (memset,    |     |  (下一篇)      |
|  memcpy,    |     +---------------+
|  memmove)   |
+-------------+
+-------------+     +-------------------+
| driver/ata  |---->| big_kernel_loader |  (read sectors)
| (init, read)|     |  (下一篇)          |
+-------------+     +-------------------+
```

---

## 代码精讲

### Freestanding 内存工具函数

#### lib/string.h -- 头文件

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

#### lib/string.cpp -- 实现

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

### ATA PIO 驱动 -- 头文件与常量定义

#### driver/ata.hpp -- 常量与接口

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

头文件把所有 ATA 相关的常量定义为 `constexpr`，这比用 `#define` 宏好得多——`constexpr` 有类型安全、有作用域、在调试器里可见。注意 `ATA_REG_ERROR` 和 `ATA_REG_FEATURES` 共用偏移 1——读操作时是错误寄存器，写操作时是特性寄存器，和状态/命令寄存器共用端口的原理一样。`ATA_REG_STATUS` 和 `ATA_REG_COMMAND` 同理，共用偏移 7。

`ATA_DRIVE_MASTER = 0xE0` 的含义需要拆开来看：bit 7 和 bit 5 必须置 1（ATA 规范要求），bit 6 是 LBA 模式使能位，低 4 位在 LBA28 模式下承载 LBA 的最高 4 位。所以 `0xE0 = 1110_0000b` 就是"选主盘 + LBA 模式 + LBA[24:27] = 0"。

公开接口只有两个函数：`init()` 初始化控制器，`read()` 读取扇区。精简的接口反映了 ATA PIO 驱动在 mini kernel 中的角色——一个一次性的、只读的、轮询式的磁盘读取工具。

---

### ATA PIO 驱动 -- 内部辅助函数

#### driver/ata.cpp -- 辅助函数

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

辅助函数都放在匿名命名空间里，相当于 `static`，对外不可见。`read_reg` 和 `write_reg` 是对 `io::inb`/`io::outb` 的简单封装，加上基址偏移。

`wait_not_busy` 和 `wait_data_ready` 是两个轮询函数，都有一个 10,000,000 次的超时上限。在 QEMU 里，ATA 命令几乎是瞬时完成的，但在真实硬件上可能需要等更长时间。轮询循环里的 `__asm__ volatile("pause")` 是 x86 的 hint 指令，告诉 CPU 当前在等待某个条件，可以降低功耗并减少总线争用——这在长时间轮询时能改善系统行为。

`wait_data_ready` 比 `wait_not_busy` 多了错误检查——它在轮询时先检查 ERR 和 DF 位。这个顺序很重要：如果先检查 DRQ，可能刚好在错误发生的时候 DRQ 还是 1（因为错误状态也可能置位 DRQ），导致读到错误数据。先检查错误可以避免这种边界情况。

`delay_400ns` 读取控制端口（0x3F6）4 次。我们读控制端口而不是状态端口（0x1F7）是有原因的——读状态端口有清除中断标志位的副作用。虽然我们用的是轮询模式不依赖中断，但养成读控制端口做延迟的好习惯，将来切换到中断驱动模式时就不会踩坑。

---

## 设计决策

### 决策：constexpr 常量而非 #define 宏

**问题**: ATA 驱动有大量固定的常量值（端口号、状态位掩码、命令码），怎么定义？

**本项目的做法**: 全部使用命名空间内的 `constexpr` 常量。

**备选方案**: 使用 `#define` 宏（如 `#define ATA_STATUS_BSY 0x80`）。

**为什么不选备选方案**: `constexpr` 有类型信息（`uint8_t`/`uint16_t` 等），编译器可以在类型不匹配时发出警告；`#define` 只是文本替换，没有类型检查。`constexpr` 遵循作用域规则，放在命名空间内不会污染全局；`#define` 从定义点到文件末尾都有效，容易和其他宏冲突。在调试器中 `constexpr` 变量可见且显示值，`#define` 在预处理阶段就消失了。这些优势在教学代码中尤其重要——读者更容易理解每个常量的类型和用途。

---

## 扩展方向

1. **SSE 优化的 memcpy/memset** -- 使用 `movdqa` 指令一次拷贝 16 字节，大幅提升大块内存操作性能（难度：star-star）
2. **IDENTIFY DEVICE 命令** -- 实现 `ATA_CMD_IDENTIFY`（0xEC），获取磁盘容量、型号、序列号等信息（难度：star）
3. **多扇区流水线读取** -- 利用 ATA 控制器的内部缓冲区，在读取当前扇区时命令控制器准备下一个扇区（难度：star-star）

---

## 参考资料

- Intel SDM: Vol. 2A -- IN/OUT 指令参考（inw/outb 的编码和约束）
  - Source: `helpers/web_search/intel_sdm/vol2a_io_port_instructions.md`
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) -- PIO 模式的完整说明、端口布局、命令序列
