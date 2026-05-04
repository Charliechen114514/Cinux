# 让 HBA 跑起来：AHCI 端口初始化实战

> 标签：AHCI, MMIO, HBA, DMA, 命令列表, FIS
> 前置：[025-1 PCI 教程](025-driver-ahci-1.md)

## 前言

上一章我们通过 PCI 枚举找到了 AHCI 控制器，拿到了 BAR5 的物理地址。但这个物理地址还不能直接用——我们内核跑在长模式下的虚拟地址空间里，CPU 访问任何物理地址都得先通过页表映射。这一章我们要把 BAR5 映射到虚拟空间，然后按 AHCI 规范的步骤一步步把 HBA（Host Bus Adapter）初始化起来：复位控制器、探测端口、为每个活跃端口分配命令列表和 FIS 缓冲区。

说实话，AHCI 的初始化流程比我想象的繁琐得多。IDE PIO 时代你只需要往几个固定端口写命令就行了，但 AHCI 要求你在内存里摆好一整套数据结构——命令列表、命令表、FIS 缓冲区——然后告诉硬件"去读这个内存地址"。好处是 DMA 传输不需要 CPU 介入，坏处是初始化链路确实长。不过这正是现代硬件的趋势：用初始化复杂度换取运行时性能。

## 环境说明

和上一章一样，QEMU + AHCI 设备 + 1MB 测试磁盘。内核已启用分页（4KB 页），PMM（物理内存管理器）和 VMM（虚拟内存管理器）均已初始化。PMM 提供整页（4KB）的物理内存分配，VMM 提供虚拟-物理页映射。这两个是端口初始化的前置依赖。

## 第一步——映射 BAR5 到虚拟空间

BAR5 的物理地址拿到手后，第一件事是把它映射到内核的虚拟地址空间。我们选了 0xFFFF800000100000 作为 MMIO 基地址，在内核的高位 canonical 区域内，不会和堆、用户态区域冲突。

映射时有一个非常重要的细节：MMIO 寄存器必须映射为非缓存的。普通 RAM 可以安全地被 CPU 缓存，因为 RAM 的内容不会自己变。但硬件寄存器不一样——HBA 的中断状态寄存器可能在任何时候被硬件修改，如果 CPU 缓存了旧值，你就永远读不到最新的中断状态。所以在映射标志里必须加上 PCD（Page Cache Disable）位。

```cpp
constexpr uint64_t mmio_flags = cinux::arch::FLAG_PRESENT
                              | cinux::arch::FLAG_WRITABLE
                              | cinux::arch::FLAG_PCD;
```

我们映射了 2 页（8KB），覆盖最多 8 个端口。每个端口占 0x80 字节的寄存器空间，从 offset 0x100 开始，所以 8 个端口需要 0x100 + 8*0x80 = 0x500 字节，一页就够。但留点余量映射两页更安全。

映射完成后，虚拟地址被 reinterpret_cast 成 HBAMem* 指针。从此以后所有 AHCI 寄存器的访问都通过这个指针完成——读 hba_mem_->pi 拿端口位图，读 hba_mem_->cap 拿能力信息，写 hba_mem_->ghc 控制全局状态。

这一点上 Linux 的做法更完善：它用 ioremap() 做映射，自动处理缓存属性，还通过 devm 框架做资源自动回收。Cinux 直接硬编码虚拟地址，简单粗暴但对教学内核来说够了。SerenityOS 介于两者之间——它有独立的 MMIO 区域管理器，但不做自动回收。

## 第二步——HBA 复位和 AHCI 模式使能

映射完成后，按规范流程初始化 HBA：

1. 设置 GHC.AE（bit 31）使能 AHCI 模式
2. 设置 GHC.HR（bit 0）触发 HBA 复位
3. 轮询等待 GHC.HR 自动清零
4. 重新设置 GHC.AE（复位会清除此位）
5. 设置 GHC.IE（bit 1）使能全局中断

步骤 4 是一个经典陷阱——很多人不知道复位会清除 AE 位。如果你忘了重新使能 AHCI 模式，HBA 会回到 IDE 兼容模式，后面所有端口操作的行为都不符合 AHCI 规范。这种 bug 在 QEMU 上可能不显现（有些 QEMU 版本的 HBA 在复位后 AE 默认就是 1），但在真机上一定会炸。我第一次写的时候就在这里卡了半天，串口输出显示端口存在但命令下发后完全没反应，最后发现是忘了重新使能 AE。

```cpp
hba_mem_->ghc |= GhcBits::AE;      // 使能 AHCI
reset_hba();                         // GHC.HR=1, 轮询等待
hba_mem_->ghc |= GhcBits::AE;      // 复位后重新使能
hba_mem_->ghc |= GhcBits::INT_ENABLE; // 使能中断
```

## 第三步——端口探测

HBA 初始化完成后，读 PI（Port Implemented）寄存器。这是一个 32 位位图，bit N = 1 表示端口 N 被实现。但"被实现"不等于"有设备连接"——你还需要检查端口的 SSTS（SATA Status）寄存器的 DET 字段。DET = 0x03 表示设备存在且通信已建立。

```cpp
uint32_t ssts = port->ssts;
uint32_t det = ssts & PxSsts::DET_MASK;
if (det != PxSsts::DET_ACTIVE) {
    // 这个端口没有设备，跳过
    continue;
}
```

在 QEMU 里，只有挂了磁盘的端口才会显示 DET = 3。我们的 CMake 配置在端口 0 上挂了一个 ide-hd 设备，所以只有端口 0 的 DET 会是 3。

## 第四步——端口初始化的核心

找到活跃端口后，需要为每个端口分配两种内存结构：

命令列表（Command List）是一个 32 项的数组，每项 32 字节（HBACommandHeader），总共 1KB，必须 1K 字节对齐。每个 Command Header 描述一个待执行的命令——命令 FIS 的长度、读写方向、PRDT 条目数、命令表的物理地址等。

FIS 接收缓冲区（FIS Receive Buffer）是 256 字节，必须 256 字节对齐。HBA 会把设备发回的 FIS（比如 Device to Host Register FIS）写入这个缓冲区。

因为 PMM 分配的页面是 4KB 对齐的，天然满足 1K 和 256 字节的对齐要求。每个端口分配两个物理页：一个给命令列表，一个给 FIS 缓冲区。

在写入 CLB/FB 寄存器之前，必须先停止端口的命令引擎。这是另一个容易踩的坑——如果你在引擎还在运行的时候改 CLB 或 FB，HBA 可能会写入到部分更新的地址，导致内存损坏。停止步骤是：清 ST -> 等 CR 清零 -> 清 FRE -> 等 FR 清零。启动步骤反过来：设 FRE -> 设 ST。FRE 必须在 ST 之前使能。

```cpp
// 停止
port->cmd &= ~PxCmd::ST;
// ... 等待 CR 清零 ...
port->cmd &= ~PxCmd::FRE;
// ... 等待 FR 清零 ...

// 写入新地址
port->clb  = cmd_list_phys & 0xFFFFFFFF;
port->clbu = cmd_list_phys >> 32;
port->fb   = fis_buf_phys & 0xFFFFFFFF;
port->fbu  = fis_buf_phys >> 32;

// 启动
port->cmd |= PxCmd::FRE;
port->cmd |= PxCmd::ST;
```

Linux 的 libahci.c 做的事情和我们在概念上完全一样——ahci_port_start() 分配命令列表和命令表，ahci_stop_engine() 和 ahci_start_engine() 控制引擎启停。区别在于 Linux 用 dma_alloc_coherent() 来分配物理连续的 DMA 内存（还处理了 IOMMU 映射），而我们直接用 PMM 的 alloc_page()。另外 Linux 支持所有 32 个命令槽的命令表，而我们只给 slot 0 分配了一个。

## 收尾

完成端口初始化后，串口输出应该是这样的：

```
[AHCI] BAR5 mapped, PI=0x1 CAP=0x... VER=0x...
[AHCI] HBA reset complete.
[AHCI] Port 0: SSTS=0x113 DET=3 SIG=0x101
[AHCI] Port 0 set up: cmdlist=0x... fis=0x...
[AHCI] 1 active ports initialised.
```

PI=0x1 说明只有端口 0 被实现（bit 0 置位）。SSTS=0x113 中 DET=3 表示设备存在，IPM=1 表示活跃电源管理状态。SIG=0x101 是 SATA 硬盘的标准签名。

到这里 HBA 已经完全就绪，端口 0 的命令引擎正在运行，等待接收命令。下一章我们来构造第一个 ATA 命令——读取磁盘的 LBA 0 扇区，验证 MBR 引导签名 0x55AA。这将是内核第一次从磁盘读取数据。

## 参考资料

- OSDev Wiki: [AHCI](https://wiki.osdev.org/AHCI) — 端口初始化流程、命令列表/FIS 配置
- Intel AHCI Specification rev 1.3 — Section 3.3 (GHC)、Section 3.3.1 (PxCMD)、Section 4.2 (Command List)
- Linux 源码: [drivers/ata/libahci.c](https://github.com/torvalds/linux/blob/master/drivers/ata/libahci.c) — ahci_port_start/stop_engine
- SerenityOS 源码: Kernel/BusControllers/AHCI/ — AHCI 控制器实现
