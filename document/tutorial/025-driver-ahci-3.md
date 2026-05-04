# 第一次 DMA 读写：从零构造 FIS 并验证 MBR

> 标签：AHCI, DMA, FIS, PRDT, ATA, MBR
> 前置：[025-2 AHCI 端口初始化教程](025-driver-ahci-2.md)

## 前言

前两章我们铺好了所有铁轨——PCI 枚举找到了 AHCI 控制器，BAR5 映射让内核能访问 HBA 寄存器，端口初始化分配了命令列表和 FIS 缓冲区。铁轨已经铺好，现在我们要让第一列火车跑起来：构造一个 ATA READ DMA EXT 命令，把它封装成 FIS，填好 PRDT，按下"发送"按钮，然后等待 HBA 把磁盘的第一个扇区 DMA 到我们的内存缓冲区。

如果一切正确，我们会在串口看到 `[AHCI] Read sector 0: 55 AA`。这两个字节是 MBR 引导签名——0x55 和 0xAA 位于磁盘第一个扇区的最后两个字节（偏移 510 和 511），是 BIOS 判断磁盘是否可引导的标志。能读出这个签名，意味着整个 PCI 枚举 -> BAR5 映射 -> HBA 初始化 -> 端口配置 -> FIS 构造 -> DMA 传输的完整链路都走通了。

## 环境说明

QEMU + AHCI + 1MB 测试磁盘。测试磁盘由构建脚本创建：1MB 全零数据 + 偏移 510/511 处写入 0x55/0xAA。内核的 PMM 和 VMM 已初始化，可以分配物理页并映射到虚拟地址。AHCI 驱动已完成 init()，端口 0 处于活跃状态。

## FIS：SATA 的"信封"

SATA 用 FIS（Frame Information Structure）来封装所有主机和设备之间的通信。FIS 有很多种类型——Register H2D（主机发给设备的寄存器命令）、Register D2H（设备返回给主机的状态）、DMA Setup、Data、PIO Setup 等。我们只需要关心一种：Register Host-to-Device FIS（类型 0x27），用于向设备发送 ATA 命令。

这个 FIS 的布局直接继承了 IDE 时代的"任务文件寄存器"——command 字节放 ATA 命令码，lba0-lba5 放 48 位 LBA 地址，count0/count1 放扇区数量，device 字节的 bit 6 置 1 表示 LBA 寻址模式。flags 字段的 bit 6 是 "command" 位，必须置 1 表示"这是一个命令而非纯控制操作"。

我们的 build_cfis 函数就是这么做的——把命令表的 cfis 区域解释为 RegH2DFIS 结构体，逐字段填充：

```cpp
auto* fis = reinterpret_cast<RegH2DFIS*>(cmd_tbl->cfis);
fis->fis_type = FisType::REG_H2D;       // 0x27
fis->flags    = 0x80;                    // command bit (bit 6)
fis->command  = write_cmd ? AtaCmd::WRITE_DMA_EXT    // 0x35
                          : AtaCmd::READ_DMA_EXT;     // 0x25
fis->device   = 0x40;                    // LBA mode (bit 6)

fis->lba0 = static_cast<uint8_t>(lba & 0xFF);
fis->lba1 = static_cast<uint8_t>((lba >> 8) & 0xFF);
// ... lba2-lba5 依次类推 ...

fis->count0 = static_cast<uint8_t>(count & 0xFF);
fis->count1 = static_cast<uint8_t>((count >> 8) & 0xFF);
```

48 位 LBA 的拆分顺序需要特别小心。lba0 是最低字节（bits 7:0），lba5 是最高字节（bits 47:40）。字节序搞反了读到的就是错误的扇区。好在我们先用 LBA 0 测试——所有 LBA 字节都是 0，不会暴露字节序问题。

## PRDT：告诉 HBA "数据放这里"

FIS 构造好后，还要告诉 HBA 数据应该 DMA 到哪个内存地址。这就是 PRDT（Physical Region Descriptor Table）的工作。每个 PRDT 条目描述一个连续的物理缓冲区，包含 64 位基地址、字节计数（22 位，最大 4MB - 1）和一个中断标志。

对于简单的单扇区读取，只需要一个 PRDT 条目。字节计数字段是 (实际字节数 - 1)，所以读 1 个扇区（512 字节）时 dbc = 511：

```cpp
uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
cmd_tbl->prdt[0].dba  = static_cast<uint32_t>(buf_phys & 0xFFFFFFFF);
cmd_tbl->prdt[0].dbau = static_cast<uint32_t>(buf_phys >> 32);
cmd_tbl->prdt[0].dbc  = byte_count & 0x3FFFFF;  // 22-bit max
cmd_tbl->prdt[0].i    = 1;  // 完成时中断
```

这里有一个非常重要的概念区分：PRDT 里写的是物理地址，不是虚拟地址。HBA 的 DMA 引擎直接操作物理内存，它不知道 CPU 的页表长什么样。而你的内核代码访问同一个缓冲区时必须用虚拟地址。所以在 kernel_main 里你会看到两个地址同时存在——buf_phys 给 DMA 引擎，buf_virt 给 CPU：

```cpp
uint64_t buf_phys = cinux::mm::g_pmm.alloc_page();
constexpr uint64_t buf_virt = 0xFFFF800000300000ULL;
cinux::mm::g_vmm.map(buf_virt, buf_phys, flags);

ahci.read(0, 0, 1, buf_phys);  // DMA 引擎写 buf_phys
auto* buf = reinterpret_cast<uint8_t*>(buf_virt);
printf("[AHCI] Read sector 0: %02x %02x\n", buf[510], buf[511]);  // CPU 读 buf_virt
```

搞混这两个地址是新手最容易犯的错误之一。如果你把虚拟地址传给了 DMA 引擎，HBA 会把数据写到页表项指向的物理地址——如果那个映射恰好不存在，你就什么都读不到，甚至可能写入到随机物理地址。这种 bug 在 QEMU 上可能不炸（QEMU 对物理地址访问比较宽容），但在真机上会直接导致数据损坏或 page fault。

## 命令下发：写 CI 寄存器

所有数据结构就位后，配置 Command Header（CFL = FIS 长度 / 4 = 5、PRDTL = 1、CTBA 指向命令表物理地址），清除中断状态，写 port.ci 的 bit 0 下发命令。HBA 收到命令后自动读取命令列表、解析 FIS、发送 ATA 命令给设备、等设备返回数据、通过 DMA 把数据写入 PRDT 指定的缓冲区。完成后 HBA 清除 CI 位并设置中断状态。

我们用轮询等待命令完成——不断检查 CI 的 bit 0 是否清零。完成后检查 TFD（Task File Data）的 bit 0（ERR 标志），非零表示命令失败：

```cpp
port->ci = (1U << slot);   // 下发命令

for (uint32_t i = 0; i < POLL_TIMEOUT; ++i) {
    if ((port->ci & (1U << slot)) == 0) {
        uint32_t tfd = port->tfd;
        if ((tfd & 0x01) != 0) {
            kprintf("[AHCI] Port %u: command error TFD=0x%x\n",
                    port_index, tfd);
            return false;
        }
        return true;
    }
    __asm__ volatile("pause");
}
```

Linux 的做法完全不同——它注册中断处理器，在 HBA 完成命令后通过硬件中断通知，然后唤醒等待的 I/O 线程。这避免了轮询的 CPU 浪费，但需要完整的中断框架和线程调度支持。SerenityOS 也使用中断驱动的 AHCI，配合 block device request queue 做异步 I/O 调度。Cinux 选择轮询是合理的简化——在 QEMU 里磁盘延迟几乎为零，轮询的开销可以忽略。

## 验证结果

如果一切正确，串口会打印：

```
[AHCI] Read sector 0: 55 AA
```

这就是我们想要的——0x55 和 0xAA，磁盘第一个扇区最后两个字节的值。如果你看到的是 `00 00`，可能的原因有：测试磁盘镜像没正确创建（用 `xxd -s 510 -l 2 build/ahci_test.img` 检查）、PRDT 的地址写错了、或者 DMA 传输到了错误的物理地址。

如果你看到 `[AHCI] Port 0: command error TFD=0x...`，说明 HBA 收到了命令但执行失败了。常见原因包括：Command Header 的 CFL 字段不对（必须是 FIS 长度 / 4 = 5）、命令表没有正确清零、CTBA 指向了错误地址。

## 对比：Cinux vs Linux vs SerenityOS

| 维度 | Cinux | Linux | SerenityOS |
|------|-------|-------|------------|
| 命令等待 | 轮询 CI | 中断驱动 | 中断驱动 |
| PRDT | 单条目 | 多条目 scatter-gather | 多条目 |
| 命令槽 | slot 0 | NCQ 32 槽 | NCQ 支持 |
| 错误恢复 | 报错返回 | EH 状态机 | 端口复位+重试 |
| Block 层 | 无 | request_queue | BlockDevice |
| DMA 内存 | PMM alloc_page | dma_alloc_coherent | kmalloc + DMA 映射 |

Cinux 的实现是"最小可用"——证明 DMA 读写链路跑通了，但没有性能优化和错误恢复。Linux 代表了生产级的完整实现，每一点优化都经过大规模测试。SerenityOS 介于两者之间——有完整的 block 设备抽象和中断支持，但代码复杂度比 Linux 低一个数量级。

对我们来说，最重要的是理解 AHCI DMA 的核心概念：在内存里摆好数据结构，告诉硬件去做，等它做完。这个模式不仅适用于磁盘驱动，几乎所有 DMA 外设（网卡、GPU、USB 控制器）都是同样的思路。

## 收尾

到这里 tag 025 的全部工作就完成了。我们从 PCI 枚举开始，找到了 AHCI 控制器，映射了 BAR5，初始化了 HBA，配置了端口，构造了 FIS，完成了第一次 DMA 读写。串口输出 `[AHCI] Read sector 0: 55 AA` 这一行字标志着我们的内核第一次从磁盘读取了数据。

接下来 tag 026 会在这个基础上构建 ramdisk——把磁盘数据加载到内存，提供简单的文件列表接口。再往后就是文件系统的实现了。我们终于踏入了存储的领域。

## 参考资料

- OSDev Wiki: [AHCI](https://wiki.osdev.org/AHCI) — FIS 构造、PRDT 配置、命令下发示例
- Intel AHCI Specification rev 1.3 — Figure 6-5 (Command Table)、Section 4.2 (Command Issue)
- ATA8-ACS — READ DMA EXT (0x25) 和 WRITE DMA EXT (0x35) 命令定义
- Linux 源码: [drivers/ata/libahci.c](https://github.com/torvalds/linux/blob/master/drivers/ata/libahci.c) — ahci_qc_issue、ahci_fill_cmd_slot
