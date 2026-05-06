# 025-2 通读篇：AHCI 寄存器定义与端口初始化

## 概览

本文是 AHCI 驱动三篇通读教程的第二篇，聚焦 AHCI 硬件寄存器的精确定义和端口初始化流程。我们将走过 `ahci_config.hpp` 中所有寄存器结构体和常量定义，以及 `ahci.hpp` 中的 AHCI 类声明和 `ahci.cpp` 中的初始化相关方法（init、reset_hba、stop_port、start_port、setup_port、map_bar5）。

关键设计决策：所有寄存器结构体用 `[[gnu::packed]]` 修饰并配合 static_assert 验证大小；端口数组声明为变长（ports[1]）；MMIO 映射使用 PCD 标志确保非缓存访问。

## 架构图

```
AHCI::init(PCIDevice)
    │
    ├── map_bar5()           ← BAR5 物理地址 → 虚拟地址（HBAMem*）
    │     └── VMM::map() × 2 页（PCD + Present + Writable）
    │
    ├── GHC.AE = 1           ← 使能 AHCI 模式
    ├── reset_hba()          ← GHC.HR = 1, 轮询等待清零
    ├── GHC.AE = 1           ← 复位后重新使能
    ├── GHC.IE = 1           ← 全局中断使能
    │
    └── for each bit in PI:
          └── if SSTS.DET == 3:
                └── setup_port(i)
                      ├── stop_port()
                      │     ├── CMD.ST = 0 → wait CR=0
                      │     └── CMD.FRE = 0 → wait FR=0
                      ├── PMM 分配 cmdlist + fis buf
                      ├── 写 CLB/CLBU/FB/FBU
                      ├── 配置 Command Header[0].CTBA
                      └── start_port()
                            ├── CMD.FRE = 1
                            └── CMD.ST = 1
```

## 代码精讲

### ahci_config.hpp — 寄存器结构和硬件常量

这个文件是整个 AHCI 驱动的"数据字典"，定义了约 300 行的硬件常量和寄存器布局。

**硬件常量**部分定义了关键尺寸：MAX_PORTS = 32（HBA 最多支持 32 个端口），CMD_SLOTS = 32（每个端口 32 个命令槽），CMD_TBL_HDR_SIZE = 0x80（命令表头部 128 字节），FIS_BUF_SIZE = 256（FIS 接收缓冲区 256 字节），MAX_PRDT_ENTRIES = 8（最多 8 个 PRDT 条目），SECTOR_SIZE = 512。

**位常量**用命名空间分组。PxCmd 包含端口命令寄存器的位定义（ST=bit0 启动、FRE=bit4 FIS 接收使能、FR=bit14 FIS 接收运行中、CR=bit15 命令引擎运行中）。GhcBits 包含全局控制位（HBA_RESET=bit0、INT_ENABLE=bit1、AE=bit31）。AtaCmd 包含 ATA 命令操作码（READ_DMA_EXT=0x25、WRITE_DMA_EXT=0x35、IDENTIFY=0xEC）。PxIs 包含端口中断状态位。PxSsts 包含 SATA 状态的 DET 字段（DET_ACTIVE=0x03 表示设备存在）。

**HBAPort** 结构体精确还原了每个端口的 0x80 字节寄存器布局：

```cpp
struct [[gnu::packed]] HBAPort {
    volatile uint32_t clb;      // 0x00: 命令列表基址低32位
    volatile uint32_t clbu;     // 0x04: 命令列表基址高32位
    volatile uint32_t fb;       // 0x08: FIS基址低32位
    volatile uint32_t fbu;      // 0x0C: FIS基址高32位
    volatile uint32_t is;       // 0x10: 中断状态
    volatile uint32_t ie;       // 0x14: 中断使能
    volatile uint32_t cmd;      // 0x18: 命令和状态
    volatile uint32_t rsv0;     // 0x1C: 保留
    volatile uint32_t tfd;      // 0x20: 任务文件数据
    volatile uint32_t sig;      // 0x24: 签名
    volatile uint32_t ssts;     // 0x28: SATA状态
    volatile uint32_t sctl;     // 0x2C: SATA控制
    volatile uint32_t serr;     // 0x30: SATA错误
    volatile uint32_t sact;     // 0x34: SATA活跃
    volatile uint32_t ci;       // 0x38: 命令下发
    volatile uint32_t sntf;     // 0x3C: SATA通知
    volatile uint32_t fbs;      // 0x40: FIS交换控制
    volatile uint32_t rsv1[11]; // 0x44-0x6F: 保留
    volatile uint32_t vendor[4];// 0x70-0x7F: 厂商自定义
};
```

所有字段都是 volatile 的——硬件可能随时修改这些寄存器的值，编译器不能假设"上一次读到的值还适用"。static_assert 确认 sizeof(HBAPort) == 0x80。

**HBAMem** 结构体从 offset 0x00 到 0xFF 是通用主机控制区域，然后端口数组从 0x100 开始：

```cpp
struct [[gnu::packed]] HBAMem {
    volatile uint32_t cap;       // 0x00: 能力
    volatile uint32_t ghc;       // 0x04: 全局控制
    volatile uint32_t is;        // 0x08: 中断状态
    volatile uint32_t pi;        // 0x0C: 端口实现位图
    volatile uint32_t vs;        // 0x10: 版本号
    // ... 其他通用寄存器 ...
    volatile uint8_t  rsv[116];  // 0x2C-0x9F: 保留
    volatile uint8_t  vendor[96];// 0xA0-0xFF: 厂商自定义
    HBAPort ports[1];            // 0x100+: 端口寄存器（变长）
};
```

ports[1] 是 C 语言中的变长数组技巧——数组在结构体末尾，实际元素数量由 PI 位图决定。static_assert 验证 `sizeof(HBAMem) - sizeof(HBAPort) == 0x100`，确保端口区域从正确的偏移开始。

**HBACommandHeader** 是命令列表中的条目，每个 32 字节。使用位域描述 bit 级别的字段（cfl:5, atapi:1, write:1, prefetch:1 等），prdtl 记录 PRDT 条目数，ctba/ctbau 指向命令表物理地址。

**HBAPrdtEntry** 是 scatter-gather 条目，每个 16 字节。dba/dbau 是数据缓冲区的 64 位物理地址，dbc:22 是字节计数减一，i:1 是中断标志。

**HBACommandTable** 是命令的完整描述：64 字节的 cfis 区域、16 字节的 ATAPI 命令区域、48 字节保留、然后是 PRDT 数组。

**RegH2DFIS** 是 Register Host-to-Device FIS 的布局，20 字节，精确还原了 ATA 任务文件寄存器的排列。

### ahci.hpp — AHCI 类声明

AHCI 类封装了整个驱动状态。公开接口只有 init（初始化）、read（读扇区）、write（写扇区）和 hba_mem（获取 MMIO 指针）。私有方法包括 map_bar5、reset_hba、setup_port、stop_port、start_port、execute_command、build_cfis。成员变量包括 HBAMem 指针和两个物理地址数组。

### ahci.cpp 初始化方法

**map_bar5** 将 BAR5 物理地址映射到内核 MMIO 区域的虚拟基地址（MMIO_VIRT_BASE = 0xFFFF800000100000，硬编码的高位 canonical 地址）。映射 2 页（8KB，覆盖最多 8 个端口），标志为 Present + Writable + PCD。PCD（Page Cache Disable）标志至关重要——MMIO 寄存器不能被 CPU 缓存。

**reset_hba** 设置 GHC.HR 位，然后轮询等待它清零。轮询中插入 `pause` 指令。超时打印警告但不 panic。

**stop_port** 分两步：先清 ST 等待 CR 清零，再清 FRE 等待 FR 清零。两步不能合并——必须先停命令引擎再停 FIS 接收。

**start_port** 先设 FRE 再设 ST。顺序不能反。

**setup_port** 是端口初始化的核心。调用 stop_port 停止引擎。用 PMM 分配命令列表页和 FIS 缓冲区页，清零后映射到虚拟空间。写入 CLB/CLBU/FB/FBU 寄存器。清除中断状态，设置中断使能。配置 Command Header[0] 的 CTBA 指向命令表（放在命令列表页内，偏移 = 32 * 32 = 0x400 字节处）。调用 start_port 启动引擎。

## 设计决策

### 决策：端口数组声明为 ports[1]

**问题**: AHCI 规范允许 1-32 个端口，如何在 HBAMem 中表示？

**本项目的做法**: 声明 `HBAPort ports[1]`，用指针偏移访问实际端口。

**备选方案**: 声明 `HBAPort ports[32]`，让 static_assert 验证起始偏移。

**为什么这样做**: 声明 32 个端口会使 HBAMem 结构体变得很大（超过 4KB），而且大部分系统只有 1-6 个端口。ports[1] 配合指针算术更灵活。代价是代码稍显不直观——`&hba_mem->ports[i]` 实际上是在做 `ports[0] + i` 的偏移。

## 扩展方向

- **BIOS/OS 握手**: 检查 BOHC 寄存器，在需要时完成 BIOS 到 OS 的控制权移交。
- **端口复位**: 对于 SSTS.DET 异常的端口，执行端口级复位（SCTL.DET = 1 然后清零）。
- **IDENTIFY DEVICE**: 对每个活跃端口发送 ATA IDENTIFY 命令，读取设备容量和特性。

## 参考资料

- OSDev Wiki: [AHCI](https://wiki.osdev.org/AHCI) — HBA 寄存器布局、端口初始化流程、命令列表/FIS 缓冲区配置
- Intel AHCI Specification rev 1.3 — 官方寄存器定义，图 3-1 到 3-4 的详细位域描述
