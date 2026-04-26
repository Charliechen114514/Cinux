# PCI 总线 & AHCI SATA 驱动

> 里程碑: `025_driver_ahci`

## 功能概述

PCI 配置空间枚举 + AHCI SATA 控制器驱动。通过 BAR5 MMIO 操作 HBA 端口，支持 DMA 读写。

## PCI 总线 (`kernel/drivers/pci/pci.hpp/cpp`)

### 数据结构
- `PCIDevice {bus, slot, func, vendor_id, device_id, class_code, subclass, prog_if, bar[6]}`

### API
- `pci_read(bus, slot, func, reg)` — 写 `0xCF8`，读 `0xCFC`
- `pci_write(bus, slot, func, reg, val)` — 写 `0xCF8` + `0xCFC`
- `pci_find_ahci(PCIDevice& out)` — 枚举 class=0x01 subclass=0x06

## AHCI 控制器 (`kernel/drivers/ahci/ahci.hpp/cpp`)

### 数据结构
- `HBAmem [[gnu::packed]]` — cap/ghc/is/pi 等 HBA 寄存器
- `HBAport [[gnu::packed]]` — clb/fb/is/ie/cmd 等端口寄存器

### 初始化 (`ahci_init()`)
1. 映射 BAR5 MMIO (`VMM::map`)
2. 检测 `pi` 位图确定活跃端口
3. 为每端口分配 Command List (32x32B) + FIS Buffer (256B)，物理连续且对齐

### 读写 API
- `ahci_read(port, lba, count, buf)` — 构造 CFIS (ATA READ DMA EXT=0x25) + PRDT，写 `port.ci`，轮询 `port.is`
- `ahci_write(port, lba, count, buf)` — 同上，命令为 WRITE DMA EXT=0x35

### 单例模式
- `AHCI::instance()` / `AHCI::set_instance()` — 替代裸全局变量

## 源码位置
- `kernel/drivers/pci/pci.hpp/cpp` — PCI 枚举
- `kernel/drivers/pci/pci_config.hpp` — PCI 常量
- `kernel/drivers/ahci/ahci.hpp/cpp` — AHCI 驱动
- `kernel/drivers/ahci/ahci_config.hpp` — AHCI 配置
