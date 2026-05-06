# F4: SMP 多核支持

> 为 Cinux 引入多核处理器支持。ACPI 解析、APIC 配置、AP 启动、多核调度。
> 解锁多核性能和现代驱动（NVMe、xHCI 等需要 APIC 中断）。

## 实现决策

| 决策 | 选择 |
|------|------|
| ACPI 范围 | 最小：RSDP → XSDT → MADT + FADT + HPET |
| 多核调度 | Per-CPU run queue + work stealing |
| 同步原语 | ticket lock + Per-CPU 变量机制 |
| AML 解释器 | 不做 |

## Milestone 依赖

```
M1 ACPI 解析 ──→ M2 APIC 配置 ──→ M3 AP 启动 + Per-CPU
                                          ↓
                                    M4 多核调度
                                          ↓
                                    M5 多核同步
```

严格串行：每个 Milestone 依赖前一个。

## 文件清单

| 文件 | Milestone | 说明 |
|------|-----------|------|
| [00-acpi.md](00-acpi.md) | M1 | ACPI 表格解析 |
| [01-apic.md](01-apic.md) | M2 | Local APIC + I/O APIC |
| [02-ap-boot.md](02-ap-boot.md) | M3 | AP 启动 + Per-CPU 数据 |
| [03-smp-sched.md](03-smp-sched.md) | M4 | Per-CPU run queue + 负载均衡 |
| [04-smp-sync.md](04-smp-sync.md) | M5 | ticket lock + 原子操作库 |

## 关键代码位置

| 模块 | 文件 |
|------|------|
| 中断框架 | `kernel/arch/x86_64/idt.hpp` — IDT 设置 |
| PIC | `kernel/arch/x86_64/pic.hpp` — 8259 PIC（将被 APIC 替代） |
| PIT 定时器 | `kernel/drivers/pit/pit.hpp` — 100Hz tick |
| 调度器 | `kernel/proc/scheduler.hpp` — RoundRobin |
| 上下文切换 | `kernel/arch/x86_64/context_switch.S` |
| Spinlock | `kernel/proc/sync.hpp` — 当前 test-and-set |
| 启动流程 | `kernel/arch/x86_64/boot.S` — BSP 启动 |
| 内存映射 | `kernel/arch/x86_64/memory_layout.hpp` — MMIO 区域 |
| PCI | `kernel/drivers/pci/pci.hpp` — MMIO BAR 映射参考 |

## 验收标准

每个 Milestone：
1. `cmake --build build/` 编译通过
2. QEMU `-smp 2` 双核启动测试
3. 所有核心正确初始化并进入调度
4. 现有单核功能不回归
