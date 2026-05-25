---
title: 025-driver-ahci-1 · AHCI 驱动
---

# 025-1 通读篇：PCI 子系统全代码解析

## 概览

本文是 tag 025（AHCI 驱动）三篇通读教程的第一篇，聚焦 PCI 子系统的完整实现。我们将逐文件、逐函数地走过 `pci_config.hpp`、`pci.hpp`、`pci.cpp` 的每一行代码，理解 PCI 配置空间访问的每一个细节。PCI 是 AHCI 驱动的地基——不知道控制器在哪，后续一切免谈。

关键设计决策：Cinux 采用 Mechanism #1（0xCF8/0xCFC 端口对）访问 PCI 配置空间，使用暴力扫描（brute-force enumeration）策略遍历所有总线/设备/功能组合，不做递归桥接扫描。这在 QEMU 和绝大多数现代硬件上完全够用。

## 架构图

```
kernel_main()
    │
    ├── PCI::init()          ← 全总线扫描，打印设备列表
    │     └── scan_function()  ← 读单个 func 的身份信息
    │
    ├── PCI::find_ahci()     ← 再次扫描，按 class/subclass 匹配
    │     └── scan_function()
    │     └── read_bars()      ← 解码 BAR0-BAR5
    │
    └── [传递 PCIDevice 给 AHCI::init()]
```

## 代码精讲

### pci_config.hpp — PCI 硬件常量定义

这个文件纯粹是常量和类型定义，不包含任何逻辑代码。所有常量用 constexpr 定义，分属不同的命名空间。

PciPort 命名空间定义了两个 I/O 端口地址：

```cpp
namespace PciPort {
constexpr uint16_t CONFIG_ADDRESS = 0xCF8;
constexpr uint16_t CONFIG_DATA    = 0xCFC;
}
```

PciReg 命名空间定义了 PCI 配置空间头部的寄存器偏移量。这些偏移是规范里固定不变的——Vendor ID 永远在 0x00，Command 永远在 0x04，BAR0-BAR5 从 0x10 到 0x24，每个间隔 4 字节。

```cpp
namespace PciReg {
constexpr uint8_t VENDOR_ID   = 0x00;
constexpr uint8_t DEVICE_ID   = 0x02;
constexpr uint8_t COMMAND     = 0x04;
constexpr uint8_t STATUS      = 0x06;
constexpr uint8_t REVID       = 0x08;
constexpr uint8_t PROG_IF     = 0x09;
constexpr uint8_t SUBCLASS    = 0x0A;
constexpr uint8_t CLASS_CODE  = 0x0B;
constexpr uint8_t HEADER_TYPE = 0x0E;
constexpr uint8_t BAR0        = 0x10;
// ... BAR1-BAR5 各间隔 4 字节
constexpr uint8_t BAR5        = 0x24;
}
```

PciClass 命名空间定义了我们要查找的设备类码。大容量存储设备 class = 0x01，AHCI 子类 = 0x06。

BAR 相关的掩码也在这里：`BAR_IO_SPACE`（0x01）区分 I/O 和内存空间；`BAR_TYPE_MASK`（0x06）和 `BAR_TYPE_64`（0x04）检测 64 位内存 BAR；`BAR_ADDR_MASK_32`（0xFFFFFFF0）提取 32 位地址。

扫描范围上限：MAX_BUS = 32（暴力扫描不需要 256 条总线，QEMU 环境下足够）、MAX_SLOT = 32、MAX_FUNC = 8、BAR_COUNT = 6。

### pci.hpp — PCIDevice 结构体和 PCI 类声明

PCIDevice 是一个 POD 结构体，把一个 PCI 功能的所有身份信息和 BAR 值打包在一起：

```cpp
struct PCIDevice {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
    uint32_t bar[BAR_COUNT];
};
```

bus/slot/func 三元组唯一标识一个 PCI 功能。vendor_id 和 device_id 是厂商和设备标识。class_code/subclass/prog_if 描述设备类型。bar[6] 存放解码后的 BAR 值（64 位 BAR 的高 32 位被拼接到低 32 位上，对应的下一个 slot 填 0）。

PCI 类提供了四个公开方法：init() 做全总线扫描，find_ahci() 查找 AHCI 控制器，pci_read() 和 pci_write() 是静态的配置空间读写。私有方法 scan_function() 扫描单个功能，read_bars() 读取所有 BAR。

### pci.cpp — PCI 子系统实现

**配置空间读写**是一切的基石。pci_read 的实现：

```cpp
uint32_t PCI::pci_read(uint8_t bus, uint8_t slot, uint8_t func,
                       uint8_t offset) {
    uint32_t address = (1U << 31)
                     | (static_cast<uint32_t>(bus) << 16)
                     | (static_cast<uint32_t>(slot) << 11)
                     | (static_cast<uint32_t>(func) << 8)
                     | (offset & 0xFC);
    io_outl(PciPort::CONFIG_ADDRESS, address);
    uint32_t value = io_inl(PciPort::CONFIG_DATA);
    return value;
}
```

地址字按 PCI 规范构造：bit 31 使能，bus 移到 bits 23:16，slot 移到 bits 15:11，func 移到 bits 10:8，offset 的低 2 位被 & 0xFC 屏蔽掉（保持 dword 对齐）。写入 0xCF8 后从 0xCFC 读取 32 位结果。pci_write 的地址构造完全相同，只是最后一步从读 0xCFC 变成了写 0xCFC。

**scan_function** 读取单个功能的身份信息。先读 offset 0x00（一个 dword 同时包含 Vendor ID 低 16 位和 Device ID 高 16 位），Vendor ID 为 0xFFFF 则返回 false。再读 offset 0x08（一个 dword 包含 Revision ID、Prog IF、Subclass、Class Code），以及 offset 0x0C 取出 Header Type。所有字段填入 PCIDevice 结构体。

**read_bars** 逐个读取 BAR0-BAR5。对每个 BAR，先读出原始 32 位值。如果 bit 0 为 1（I/O 空间），地址掩码用 0xFFFFFFFC。如果 bit 0 为 0（内存空间），检查 bits 2:1 是否为 0x04（64 位 BAR），如果是就读下一个 BAR 寄存器拼成完整的 64 位地址，并跳过下一个索引。

```cpp
if ((raw & BAR_TYPE_MASK) == BAR_TYPE_64 && (i + 1) < BAR_COUNT) {
    uint32_t high = pci_read(dev.bus, dev.slot, dev.func,
                             bar_offsets[i + 1]);
    dev.bar[i] = (static_cast<uint64_t>(high) << 32)
               | (raw & BAR_ADDR_MASK_32);
    dev.bar[i + 1] = 0;
    ++i;  // 跳过被消耗的高 32 位 BAR
}
```

**init()** 做暴力全扫描：三层嵌套循环遍历 bus/slot/func，func 0 为空时 break 内层循环（slot 空则跳过后续 func）。

**find_ahci()** 几乎和 init() 一样的扫描逻辑，但在找到 class_code == 0x01 && subclass == 0x06 的设备后调用 read_bars() 读取所有 BAR，保存到 out 引用并返回 true。打印 AHCI 发现信息，包含 BAR5 的值。

## 设计决策

### 决策：暴力扫描 vs 递归桥接扫描

**问题**: 如何遍历 PCI 总线拓扑？

**本项目的做法**: 暴力扫描 bus 0-31、slot 0-31、func 0-7。简单直接，不处理 PCI-to-PCI 桥。

**备选方案**: 递归扫描——从 bus 0 开始，遇到 PCI-to-PCI 桥就递归进入 secondary bus。这样可以正确处理多级桥接拓扑。

**为什么不选备选方案**: QEMU 默认只有一个 PCI 总线（bus 0），所有设备都在 bus 0 上。暴力扫描在 QEMU 环境下完全够用，而且代码复杂度大幅降低。等将来需要在真机上运行时，再升级为递归扫描也不迟。MAX_BUS 设为 32 而非 256 是一种折中——减少扫描时间，同时覆盖 QEMU 可能用到的所有总线。

**如果要扩展**: 将 scan_function 中检测 PCI-to-PCI 桥（class=0x06, subclass=0x04）的逻辑加入，读取 secondary bus number 后递归扫描。同时将 MAX_BUS 提升到 256。

## 扩展方向

- **PCIe ECAM 支持**: 使用 MMIO 方式访问扩展配置空间（4KB per function），支持 PCIe 设备的扩展能力链表。
- **多功能设备正确处理**: 检查 Header Type bit 7 来判断是否需要扫描 func 1-7，避免单功能设备的重复报告。
- **设备驱动匹配框架**: 建立基于 class code/subclass 的驱动注册表，自动为发现的 PCI 设备绑定驱动。
- **中断路由**: 读取设备的 Interrupt Line/Pin 字段，配合 ACPI 或 MP Table 解析中断路由。

## 参考资料

- OSDev Wiki: [PCI](https://wiki.osdev.org/PCI) — 配置空间访问机制 #1、BAR 布局、类码定义
- OSDev Wiki: [AHCI](https://wiki.osdev.org/AHCI) — AHCI 控制器的 PCI 标识（class=0x01, subclass=0x06）
- PCI Local Bus Specification rev 3.0 — 官方规范，配置空间头部的完整定义
