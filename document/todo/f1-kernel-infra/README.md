# F1: 内核基础设施

> Phase 0 完成后的第一项任务。为所有后续 Feature 域（驱动、文件系统、内存管理）提供基础设施。

## 实现决策

| 决策 | 选择 |
|------|------|
| 日志系统 | Ring Buffer 作为可复用 lib，增强 kprintf |
| 块设备抽象 | 极简 IBlockDevice 接口（同步 read/write，不含请求队列） |
| DMA 框架 | DMA Buffer Pool + PRDT 工具库 |
| Buffer Cache | 推到 F2 |

## Milestone 依赖

```
M1 Ring Buffer ──→ M2 Kernel Logging
     ↓
M3 DMA Infra ──→ M4 Block Device Abstraction
```

M1 和 M3 无互相依赖，可并行。M4 依赖 M3（AHCI 适配器需要 DMA 工具）。

## 文件清单

| 文件 | Milestone | 说明 |
|------|-----------|------|
| [00-ring-buffer.md](00-ring-buffer.md) | M1 | 通用环形缓冲区库 |
| [01-kernel-logging.md](01-kernel-logging.md) | M2 | 内核日志增强 + dmesg |
| [02-dma-infra.md](02-dma-infra.md) | M3 | DMA Buffer Pool + PRDT 工具库 |
| [03-block-device.md](03-block-device.md) | M4 | IBlockDevice 抽象 + ext2 解耦 |

## 关键代码位置

| 模块 | 文件 |
|------|------|
| kprintf | `kernel/lib/kprintf.hpp`, `kernel/lib/kprintf.cpp` |
| 格式化引擎 | `kernel/lib/private/vkprintf_impl.hpp` |
| AHCI 驱动 | `kernel/drivers/ahci/ahci.hpp`, `kernel/drivers/ahci/ahci.cpp` |
| AHCI 寄存器定义 | `kernel/drivers/ahci/ahci_config.hpp` |
| ext2（AHCI 耦合） | `kernel/fs/ext2.hpp`, `kernel/fs/ext2.cpp` |
| VFS 抽象 | `kernel/fs/vfs_filesystem.hpp` |
| PMM | `kernel/mm/pmm.hpp` |
| VMM | `kernel/mm/vmm.hpp` |
| 内存布局 | `kernel/arch/x86_64/memory_layout.hpp` |
| Mini kernel DMA 参考 | `kernel/mini/driver/ata.hpp`, `kernel/mini/driver/ata.cpp` |
| Spinlock | `kernel/proc/sync.hpp` |

## 验收标准

每个 Milestone：
1. `cmake --build build/` 编译通过
2. QEMU 启动运行正常
3. 新增代码有单元测试
4. 每文件 ≤ 500 行
