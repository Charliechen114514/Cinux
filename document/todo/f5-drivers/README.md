# F5: 设备驱动

> 尽可能多适配驱动。利用 F1 的 DMA/PRDT 工具库和 IBlockDevice 抽象。
> 覆盖存储、网络、USB、定时器四大类。

## 实现决策

| 类别 | 驱动 |
|------|------|
| 存储 | AHCI DMA 升级 + VirtIO Block + NVMe |
| USB | xHCI 最小化（仅 HID 键盘/鼠标） |
| 网卡 | Intel E1000 + VirtIO Net |
| 定时器 | HPET + RTC |

## Milestone 依赖

```
M1 AHCI DMA ──→ M3 NVMe
       ↓
M2 VirtIO 框架 ──→ M2b VirtIO Block
       ↓
M5 xHCI 最小化
       ↓
M6 E1000 ──→ F7 网络栈
       ↓
M7 VirtIO Net

M4 HPET + RTC（独立）
```

M1、M2、M4 可并行。M2b 依赖 M2。M6/M7 为 F7 前置。

## 文件清单

| 文件 | Milestone |
|------|-----------|
| [00-ahci-dma.md](00-ahci-dma.md) | M1: AHCI DMA 升级 |
| [01-virtio.md](01-virtio.md) | M2: VirtIO 框架 + VirtIO Block |
| [02-nvme.md](02-nvme.md) | M3: NVMe 驱动 |
| [03-timer.md](03-timer.md) | M4: HPET + RTC |
| [04-xhci.md](04-xhci.md) | M5: xHCI 最小化 |
| [05-e1000.md](05-e1000.md) | M6: Intel E1000 网卡 |
| [06-virtio-net.md](06-virtio-net.md) | M7: VirtIO Net 网卡 |

## 关键代码位置

| 模块 | 文件 |
|------|------|
| AHCI | `kernel/drivers/ahci/ahci.hpp`, `.cpp` |
| PCI | `kernel/drivers/pci/pci.hpp`, `.cpp` |
| IBlockDevice | `kernel/drivers/block_device.hpp`（F1 产出） |
| DMA Pool | `kernel/drivers/dma/dma_pool.hpp`（F1 产出） |
| PRDT | `kernel/drivers/dma/prdt.hpp`（F1 产出） |
| PIT | `kernel/drivers/pit/pit.hpp` |
| Keyboard | `kernel/drivers/keyboard/keyboard.hpp` |
| Mouse | `kernel/drivers/mouse/mouse.hpp` |
| VMM/MMIO | `kernel/mm/vmm.hpp` — FLAG_PCD |
| Memory layout | `kernel/arch/x86_64/memory_layout.hpp` |

## 验收标准

每个 Milestone：
1. QEMU 对应设备功能验证
2. IBlockDevice 实现可通过 ext2 挂载测试
3. 中断驱动正常（无轮询阻塞）
4. 每文件 ≤ 500 行
