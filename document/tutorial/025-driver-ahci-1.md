---
title: 025-driver-ahci-1 · AHCI 驱动
---

# 从 PCI 到 AHCI：发现你的 SATA 控制器

> 标签：PCI, AHCI, 配置空间, BAR, 设备枚举
> 前置：[024 shell 教程](024-shell-1.md)

## 前言

说实话，做到 tag 024 的时候我们的内核已经像模像样了——能跑用户态程序、能响应键盘中断、甚至有个能 echo 的 shell。但有一个问题始终悬在头上：我们的内核从来没主动从磁盘上读过哪怕一个字节。所有数据要么是编译时嵌进去的，要么是在内存里临时构造的。这不是一个真正的操作系统，更像一个精巧的嵌入式 demo。

tag 025 要打破这个天花板。我们要让内核自己找到 SATA 控制器，自己配置它，然后从磁盘上读出第一个扇区。这一步是后面文件系统、持久化存储、甚至用户态文件操作的根基。

为什么从 PCI 开始而不是直接跳到 AHCI？因为 AHCI 控制器本身就是一个 PCI 设备——你不知道它在 PCI 总线上的哪个位置，就没法找到它。PCI 枚举是所有外设驱动的起点。

## 环境说明

我们运行的依然是 QEMU，但这次 CMake 配置里多加了几行参数：一个 AHCI 设备和一个 1MB 的测试磁盘镜像。构建系统会在编译时自动调用脚本 `create_ahci_test_disk.sh` 生成一个带有 MBR 引导签名（0x55AA）的空磁盘。工具链不变：GCC/G++ 14 + CMake + GNU AS，目标 x86_64，无标准库。

## PCI 配置空间：0xCF8 和 0xCFC 的魔术

PCI 规范定义了一种非常优雅的配置空间访问机制。系统保留了两个 32 位 I/O 端口——0xCF8 叫 CONFIG_ADDRESS，0xCFC 叫 CONFIG_DATA。软件先往 0xCF8 写一个精心构造的 32 位地址字，然后再读写 0xCFC，就完成了对某个 PCI 设备某个配置寄存器的访问。

这个地址字的布局是这样的：

```
Bit 31      : Enable (必须置 1)
Bits 30-24  : Reserved
Bits 23-16  : Bus Number
Bits 15-11  : Device Number (Slot)
Bits 10-8   : Function Number
Bits 7-2    : Register Offset
Bits 1-0    : 必须为 0 (DWORD 对齐)
```

我们的 pci_read 函数就是这么实现的——把 bus/slot/func/offset 按位拼起来，加上 bit 31 的使能位，写到 0xCF8，然后从 0xCFC 读回 32 位结果：

```cpp
uint32_t PCI::pci_read(uint8_t bus, uint8_t slot, uint8_t func,
                       uint8_t offset) {
    uint32_t address = (1U << 31)
                     | (static_cast<uint32_t>(bus) << 16)
                     | (static_cast<uint32_t>(slot) << 11)
                     | (static_cast<uint32_t>(func) << 8)
                     | (offset & 0xFC);
    io_outl(PciPort::CONFIG_ADDRESS, address);
    return io_inl(PciPort::CONFIG_DATA);
}
```

pci_write 完全一样，只是最后一步从读变成写。这两个函数是整个 PCI 子系统的地基——后面所有的设备发现、BAR 读取、命令寄存器配置，全部建立在它们之上。

## 暴力扫描：挨个问过去

有了读写能力，下一步就是枚举。PCI 的设计允许最多 256 条总线，每条总线 32 个设备，每个设备 8 个功能——理论上需要检查 65536 个位置。但实际上 QEMU 环境下几乎不会超过 bus 0，所以我们把 MAX_BUS 设为 32，扫描次数降低到 8192 次。

扫描逻辑很简单：三层嵌套循环遍历 bus/slot/func，对每个位置读 Vendor ID（配置空间偏移 0x00 的低 16 位）。如果读到 0xFFFF，说明这个位置没有设备——如果 func 0 就为空，那整个 slot 都为空，直接 break 内层循环跳到下一个 slot。

找到设备后，我们读出它的 Device ID、Class Code、Subclass、Prog IF、Header Type 等字段，存入 PCIDevice 结构体并打印：

```cpp
cinux::lib::kprintf("[PCI] %02x:%02x.%x %04x:%04x "
                    "class=%02x sub=%02x\n",
                    bus, slot, func,
                    dev.vendor_id, dev.device_id,
                    dev.class_code, dev.subclass);
```

这种暴力扫描方式在 QEMU 里足够了。xv6 也用类似的暴力扫描策略（虽然它找的是 IDE 控制器而非 AHCI）。Linux 内核则使用更聪明的递归扫描——遇到 PCI-to-PCI 桥就递归进入 secondary bus，这样可以正确处理多级桥接拓扑。不过对于只有一个 PCI 总线的 QEMU 虚拟机，暴力扫描和递归扫描的结果完全一样。

## 找到 AHCI：Class 0x01, Subclass 0x06

扫描完所有设备后，我们需要从中找到 AHCI 控制器。PCI 规范为每种设备类型定义了 Class Code 和 Subclass。大容量存储设备的 Class Code 是 0x01，其中 AHCI（串行 ATA）的 Subclass 是 0x06。我们的 find_ahci 函数再次扫描所有 bus/slot/func，找到第一个匹配的设备：

```cpp
if (dev.class_code == PciClass::MASS_STORAGE &&     // 0x01
    dev.subclass == PciClass::AHCI_SUBCLASS) {       // 0x06
    read_bars(dev);
    out = dev;
    return true;
}
```

这里和 xv6 形成了鲜明对比。xv6 完全不做 PCI 枚举——它假设 IDE 控制器永远在固定的 I/O 端口地址（0x1F0-0x1F7）。这在 QEMU 的默认配置下恰好是对的，但在真机上不一定成立。现代 x86 系统的 SATA 控制器可能挂在不同的 PCI slot 上，甚至可能有多个控制器。通过 PCI 枚举动态发现设备是更鲁棒的做法，这也是 Linux 和所有生产级 OS 采用的策略。

## BAR 解码：设备告诉你它的寄存器在哪

找到 AHCI 控制器后，最关键的一步是读 BAR5（Base Address Register 5，配置空间偏移 0x24）。AHCI 规范规定 BAR5 指向 HBA 的 MMIO 寄存器区域——这个物理地址是后面所有操作的基础。

PCI 的 BAR 机制比很多人想象的要复杂。一个设备最多有 6 个 BAR，每个 BAR 可以是 I/O 空间或内存空间，内存空间 BAR 又可以是 32 位或 64 位的。判断方法看原始值的低位：bit 0 = 1 是 I/O 空间，bit 0 = 0 是内存空间。内存空间 BAR 的 bits 2:1 指示类型——0x0 是 32 位，0x2 是 64 位。64 位 BAR 会消耗下一个 BAR 寄存器作为高 32 位地址。

我们的 read_bars 函数处理了所有这些情况：

```cpp
if ((raw & BAR_IO_SPACE) != 0) {
    dev.bar[i] = raw & 0xFFFFFFFC;    // I/O BAR
} else {
    dev.bar[i] = raw & BAR_ADDR_MASK_32;  // 内存 BAR
    if ((raw & BAR_TYPE_MASK) == BAR_TYPE_64 && (i + 1) < BAR_COUNT) {
        uint32_t high = pci_read(dev.bus, dev.slot, dev.func,
                                 bar_offsets[i + 1]);
        dev.bar[i] = (static_cast<uint64_t>(high) << 32)
                   | (raw & BAR_ADDR_MASK_32);
        dev.bar[i + 1] = 0;
        ++i;  // 跳过被消耗的高 32 位 BAR
    }
}
```

对于 AHCI 控制器，BAR5 通常是 32 位内存 BAR（QEMU 的 ICH9 AHCI 控制器就是这样），指向一段 4KB 的 MMIO 区域。这段区域包含了 HBA 的所有控制寄存器——通用主机控制、端口控制、命令下发、中断状态等。下一章我们就来映射并使用它。

## 收尾

到这里我们已经完成了 PCI 子系统的全部工作：配置空间读写、设备枚举、BAR 解码、AHCI 控制器发现。串口输出应该能看到类似这样的信息：

```
[PCI] Scanning PCI bus...
[PCI] 00:00.0 8086:1237 class=06 sub=00
[PCI] 00:01.0 8086:7000 class=06 sub=01
...
[PCI] 00:1f.2 8086:2922 class=01 sub=06
[PCI] Found 18 PCI devices.
[PCI] AHCI found: 00:1f.2 BAR5=0x...
```

下一章我们要把 BAR5 的物理地址映射到内核的虚拟地址空间，按 AHCI 规范初始化 HBA 控制器，为每个活跃端口配置命令列表和 FIS 缓冲区。真正的磁盘操作已经近在咫尺了。

## 参考资料

- OSDev Wiki: [PCI](https://wiki.osdev.org/PCI) — Configuration Mechanism #1、设备结构、BAR 布局
- OSDev Wiki: [AHCI](https://wiki.osdev.org/AHCI) — AHCI 控制器的 PCI 标识
- xv6 源码: ide.c — xv6 的 IDE 驱动（固定端口，无 PCI 枚举）
- Linux 源码: drivers/ata/ahci.c — Linux 的 AHCI PCI 驱动绑定
