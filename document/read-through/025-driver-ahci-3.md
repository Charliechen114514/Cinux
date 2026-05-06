# 025-3 通读篇：DMA 命令构建与扇区读写

## 概览

本文是 AHCI 驱动三篇通读教程的最后一篇，聚焦 DMA 命令的构建和执行——从 build_cfis 构造 FIS，到 execute_command 下发命令并轮询等待完成，再到 read/write 公开接口的封装。同时涵盖测试代码（test_ahci.cpp）、内核主函数集成、QEMU 测试磁盘创建脚本，以及单元测试框架。

关键设计决策：使用轮询（polling）而非中断来等待命令完成；单 PRDT 条目（不支持 scatter-gather）；单命令槽（slot 0）；物理地址直传（不经过 block 层抽象）。这些简化在 QEMU 环境下完全够用，真机性能也不差（单次读写延迟主要在磁盘寻道而非软件开销）。

## 架构图

```
ahci.read(port, lba, count, buf_phys)
    │
    └── execute_command(port, slot=0, write=false, lba, count, buf_phys)
          │
          ├── 清零 Command Table
          ├── build_cfis()           ← 构造 RegH2D FIS
          ├── 配置 PRDT[0]           ← buf_phys, dbc=count*512-1
          ├── 配置 Command Header    ← CFL=5, PRDTL=1, CTBA
          ├── port.is = ~0           ← 清除中断状态
          ├── port.ci = (1 << slot)  ← 下发命令
          │
          └── 轮询: ci bit 清零?
                ├── 是 → 检查 TFD.ERR
                │         ├── 0 → return true
                │         └── 1 → return false (错误)
                └── 超时 → return false
```

## 代码精讲

### build_cfis — 构造命令 FIS

这个静态方法负责填充命令表中 cfis 区域的前 20 字节，把它解释为 RegH2DFIS 结构体：

```cpp
void AHCI::build_cfis(HBACommandTable* cmd_tbl, bool write_cmd,
                      uint64_t lba, uint16_t count) {
    auto* fis = reinterpret_cast<RegH2DFIS*>(cmd_tbl->cfis);
    fis->fis_type = FisType::REG_H2D;   // 0x27
    fis->flags    = 0x80;               // command bit
    fis->command  = write_cmd ? AtaCmd::WRITE_DMA_EXT  // 0x35
                              : AtaCmd::READ_DMA_EXT;   // 0x25
    fis->feature  = 0;

    // 48-bit LBA 拆分成 6 个字节
    fis->lba0 = static_cast<uint8_t>(lba & 0xFF);
    fis->lba1 = static_cast<uint8_t>((lba >> 8) & 0xFF);
    fis->lba2 = static_cast<uint8_t>((lba >> 16) & 0xFF);
    fis->device = 0x40;                 // LBA 模式 (bit 6)
    fis->lba3 = static_cast<uint8_t>((lba >> 24) & 0xFF);
    fis->lba4 = static_cast<uint8_t>((lba >> 32) & 0xFF);
    fis->lba5 = static_cast<uint8_t>((lba >> 40) & 0xFF);
    fis->feature_exp = 0;

    fis->count0 = static_cast<uint8_t>(count & 0xFF);
    fis->count1 = static_cast<uint8_t>((count >> 8) & 0xFF);
    fis->control = 0;
}
```

fis_type = 0x27 标识这是 Register Host-to-Device FIS。flags = 0x80 的 bit 6 置位表示"这是一个命令"（而非纯控制操作）。device = 0x40 的 bit 6 置位选择 LBA 寻址模式（而非 CHS 模式）。48 位 LBA 地址被拆分成低 24 位（lba0-lba2）和高 24 位（lba3-lba5）。扇区数量 16 位拆成低字节和高字节。

### execute_command — 命令下发与轮询

这是 DMA 读写的核心引擎。它负责把所有数据结构串联起来并触发硬件操作。

首先获取命令列表和命令表的虚拟地址。命令表的物理地址计算为 `cmd_list_phys + CMD_SLOTS * sizeof(HBACommandHeader)`，即 32 个 32 字节的 Command Header 之后的位置（偏移 0x400）。清零整个命令表区域（CMD_TABLE_TOTAL = 0x80 + 8*16 = 256 字节）。

调用 build_cfis 构造 FIS 后，设置 PRDT 条目 0：

```cpp
uint32_t byte_count = static_cast<uint32_t>(count) * SECTOR_SIZE - 1;
cmd_tbl->prdt[0].dba  = static_cast<uint32_t>(buf_phys & 0xFFFFFFFF);
cmd_tbl->prdt[0].dbau = static_cast<uint32_t>(buf_phys >> 32);
cmd_tbl->prdt[0].dbc  = byte_count & 0x3FFFFF;  // 22-bit max
cmd_tbl->prdt[0].i    = 1;  // 完成时中断
```

dbc 字段是 (实际字节数 - 1)，截断到 22 位（最大 4MB - 1）。i = 1 表示传输完成后产生中断（虽然我们用轮询，但设置这个位没有坏处）。

配置 Command Header：

```cpp
headers[slot].cfl    = sizeof(RegH2DFIS) / 4;  // 20 / 4 = 5 DW
headers[slot].prdtl  = 1;
headers[slot].write  = write_cmd ? 1 : 0;
headers[slot].ctba   = static_cast<uint32_t>(cmd_tbl_phys & 0xFFFFFFFF);
headers[slot].ctbau  = static_cast<uint32_t>(cmd_tbl_phys >> 32);
headers[slot].prdbc  = 0;
```

CFL（Command FIS Length）以 DWORD 为单位，RegH2DFIS 是 20 字节 = 5 个 DWORD。PRDTL = 1（一个 PRDT 条目）。CTBA 指向命令表物理地址。PRDBC 清零（HBA 完成传输后会写入实际传输字节数）。

清除中断状态、下发命令、轮询等待：

```cpp
port->is = static_cast<uint32_t>(~0U);   // 写 1 清除所有中断位
port->ci = (1U << slot);                  // 设置对应位下发命令

for (uint32_t i = 0; i < POLL_TIMEOUT; ++i) {
    if ((port->ci & (1U << slot)) == 0) {
        uint32_t tfd = port->tfd;
        if ((tfd & 0x01) != 0) {
            cinux::lib::kprintf("[AHCI] Port %u: command error TFD=0x%x\n",
                                port_index, tfd);
            return false;
        }
        return true;
    }
    __asm__ volatile("pause");
}
```

写 IS = 0xFFFFFFFF 清除所有中断位（写 1 清除，write-1-to-clear）。写 CI 的对应位下发命令。轮询直到 CI 位清零（HBA 完成命令后自动清除）。完成后检查 TFD 的 bit 0（ERR 标志）。

### read/write — 公开接口

两个方法都是 execute_command 的简单包装，区别只在 write_cmd 参数：

```cpp
bool AHCI::read(uint8_t port_index, uint64_t lba, uint16_t count,
                uint64_t buf) {
    if (hba_mem_ == nullptr || port_index >= MAX_PORTS) {
        return false;
    }
    if (cmd_list_phys_[port_index] == 0) {
        cinux::lib::kprintf("[AHCI] Port %u not initialised.\n", port_index);
        return false;
    }
    return execute_command(port_index, 0, false, lba, count, buf);
}
```

参数校验确保 HBA 已初始化且目标端口已配置。未配置的端口会打印警告而非静默失败。slot 硬编码为 0（单命令操作）。buf 是物理地址——调用者负责确保缓冲区物理连续且已映射。

### kernel/main.cpp 集成

内核主函数中的 AHCI 测试代码展示了完整的使用流程：

```cpp
// Step 20: PCI enumeration
cinux::drivers::pci::PCI pci;
pci.init();

// Step 21: Find AHCI controller and initialise
cinux::drivers::ahci::AHCI ahci;
cinux::drivers::pci::PCIDevice    ahci_dev;
if (pci.find_ahci(ahci_dev)) {
    ahci.init(ahci_dev);

    // Step 22: Read sector 0 (MBR) and check boot signature
    uint64_t buf_phys = cinux::mm::g_pmm.alloc_page();
    if (buf_phys != 0) {
        constexpr uint64_t buf_virt  = 0xFFFF800000300000ULL;
        constexpr uint64_t buf_flags = cinux::arch::FLAG_PRESENT
                                     | cinux::arch::FLAG_WRITABLE;
        cinux::mm::g_vmm.map(buf_virt, buf_phys, buf_flags);

        auto* buf = reinterpret_cast<uint8_t*>(buf_virt);
        for (uint32_t i = 0; i < 512; ++i) {
            buf[i] = 0;
        }

        if (ahci.read(0, 0, 1, buf_phys)) {
            cinux::lib::kprintf("[AHCI] Read sector 0: %02x %02x\n",
                                buf[510], buf[511]);
        } else {
            cinux::lib::kprintf("[AHCI] Failed to read sector 0.\n");
        }
    }
} else {
    cinux::lib::kprintf("[AHCI] No AHCI controller found.\n");
}
```

注意几个要点。buf_virt 使用硬编码的地址 0xFFFF800000300000（内核 DMA 缓冲区区域），而不是随意挑选的地址。buf_phys（传给 DMA 引擎的物理地址）和 buf_virt（CPU 访问数据的虚拟地址）是两个不同的值——HBA 只知道物理地址，CPU 只知道虚拟地址。

### QEMU 测试磁盘脚本

`scripts/create_ahci_test_disk.sh` 创建一个 1MB 的全零磁盘镜像，在偏移 510 和 511 处写入 0x55 和 0xAA（MBR 引导签名）。

### test_ahci.cpp — 内核态集成测试

内核测试文件 test_ahci.cpp 包含三个测试用例：(1) PCI 枚举找到 AHCI 控制器；(2) AHCI init 映射 BAR5 成功；(3) 读取扇区 0 并验证 MBR 签名为 0x55/0xAA。每个测试用独立的 PCI 和 AHCI 实例，测试 3 还额外分配和映射 DMA 缓冲区。

### test/unit/test_ahci.cpp — 主机端单元测试

约 820 行的主机端单元测试覆盖了所有寄存器结构体大小（static_assert）、常量值、FIS 构造逻辑、LBA 编码、扇区计数编码、PCI 地址字构造、BAR 类型检测等纯算术逻辑。不链接内核代码，纯 C++ 测试。

## 设计决策

### 决策：轮询 vs 中断

**问题**: 如何等待 DMA 命令完成？

**本项目的做法**: 轮询 CI 寄存器直到对应位清零。

**备选方案**: 注册中断处理器，在 HBA 完成命令后触发中断，驱动在中断处理器中唤醒等待线程。

**为什么不选备选方案**: Cinux 目前还没有中断驱动的线程唤醒机制（只有键盘中断的简单处理）。轮询在 QEMU 环境下延迟极低（磁盘是内存模拟的），不需要中断的开销。等将来做异步 I/O 和 block 层时再升级为中断驱动。

### 决策：单命令槽

**问题**: 每个端口支持 32 个命令槽，如何利用？

**本项目的做法**: 只使用 slot 0，一次只发一个命令。

**备选方案**: 实现命令队列，同时下发多个命令，利用 NCQ 提高吞吐。

**为什么不选备选方案**: NCQ 需要设备支持（QEMU 的 ide-hd 不一定支持 NCQ），且队列管理增加了很多复杂度。对于教学内核，单命令轮询已经足够演示 DMA 的核心概念。

## 扩展方向

- **多扇区读写**: 支持一次读写多个连续扇区（修改 count 参数即可）。
- **IDENTIFY DEVICE**: 发送 0xEC 命令读取设备信息（总扇区数、扇区大小、序列号）。
- **中断驱动**: 注册 IRQ 处理器，在命令完成后通过中断通知，避免轮询浪费 CPU。
- **Block 设备抽象**: 将 AHCI 读写封装为通用的 block_device 接口，为文件系统层做准备。
- **AHCI 写入验证**: 写入自定义模式到扇区，再读回来比对，验证写入链路的正确性。

## 参考资料

- OSDev Wiki: [AHCI](https://wiki.osdev.org/AHCI) — FIS 构造示例、命令下发流程、PRDT 配置
- Intel AHCI Specification rev 1.3 — Figure 6-5 (Command Table)、Section 4.2 (Command Issue)
- ATA8-ACS — READ DMA EXT (0x25) 和 WRITE DMA EXT (0x35) 命令定义
