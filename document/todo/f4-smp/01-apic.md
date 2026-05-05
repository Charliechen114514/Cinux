# M2: Local APIC + I/O APIC

> 配置 APIC 替代 8259 PIC。Local APIC 处理 CPU 本地中断，
> I/O APIC 处理外部设备中断路由。

## 依赖

- M1 ACPI 解析（APIC 地址 + CPU 列表）

## 目标

1. Local APIC 初始化（每个 CPU）
2. Local APIC Timer（替代 PIT）
3. I/O APIC 初始化（替代 8259 PIC）
4. IPI 发送能力（用于 AP 启动和调度通知）

## 任务清单

### T1: Local APIC 驱动

**文件**: `kernel/drivers/apic/local_apic.hpp`, `kernel/drivers/apic/local_apic.cpp`

Local APIC 通过 MMIO 访问（从 MADT 获取基址）：

```cpp
namespace cinux::drivers::apic {

class LocalAPIC {
public:
    void init(uint64_t mmio_base);

    // 基本操作
    uint32_t read(uint32_t reg);
    void write(uint32_t reg, uint32_t value);

    // ID 管理
    uint32_t id();
    uint32_t version();

    // 中断控制
    void enable();       // 设置 Spurious Interrupt Vector + 软件使能
    void disable();

    // EOI（End of Interrupt）
    void eoi();

    // IPI（Inter-Processor Interrupt）
    void send_ipi(uint32_t target_apic_id, uint32_t vector);
    void send_init(uint32_t target_apic_id);   // INIT IPI（AP 启动用）
    void send_sipi(uint32_t target_apic_id, uint8_t page); // SIPI（AP 启动用）

    // Timer
    void timer_init(uint32_t vector, bool periodic);
    void timer_set(uint32_t count);
    void timer_stop();
    uint32_t timer_current();

    // 错误处理
    uint32_t error_status();
    void clear_error();

private:
    volatile uint32_t* base_;  // MMIO 基地址
};

extern LocalAPIC g_lapic;

} // namespace cinux::drivers::apic
```

**关键 MMIO 寄存器**:
| 偏移 | 名称 | 用途 |
|------|------|------|
| 0x020 | LAPIC ID | 本地 APIC ID |
| 0x030 | LAPIC Version | 版本信息 |
| 0x080 | TPR | 任务优先级 |
| 0x0B0 | EOI | 中断结束 |
| 0x0F0 | Spurious Vector | 伪中断向量 + 使能位 |
| 0x100-0x170 | ISR/IRR/TMR | 中断状态 |
| 0x280 | Error Status | 错误状态 |
| 0x300 | ICR Low | IPI 命令低 32 位 |
| 0x310 | ICR High | IPI 命令高 32 位（目标 APIC ID） |
| 0x320 | LVT Timer | 定时器配置 |
| 0x380 | Timer Initial Count | 定时器初值 |
| 0x390 | Timer Current Count | 定时器当前值 |
| 0x3E0 | Timer Divide | 定时器分频 |

- [ ] MMIO 映射（使用 VMM map + FLAG_PCD）
- [ ] read/write 寄存器操作
- [ ] enable() — Spurious Vector Register bit 8
- [ ] eoi() — 写 0 到 EOI 寄存器
- [ ] send_ipi() — ICR 写入（等待 delivery 完成）
- [ ] send_init() / send_sipi() — AP 启动专用 IPI

### T2: Local APIC Timer

替代 PIT 作为系统时钟源：

```cpp
void LocalAPIC::timer_init(uint32_t vector, bool periodic) {
    // 配置 LVT Timer Entry:
    // - Vector = timer_vector (如 IRQ0 = 32)
    // - Mode = periodic (bit 17 = 1) or one-shot
    // - Mask = 0 (unmask)
    write(0x320, vector | (periodic ? (1 << 17) : 0));

    // 分频配置 = 1
    write(0x3E0, 0x0B);  // divide by 1

    // 设置初始计数值
    // 需要校准：与 PIT 对比测量 APIC timer 频率
}
```

**校准流程**:
1. 设置 PIT 在已知时间后中断（如 10ms）
2. 同时启动 APIC timer（one-shot，最大值）
3. PIT 中断时读取 APIC timer 当前值
4. 计算每 tick 的 APIC 时钟周期数

- [ ] APIC timer 校准（使用 PIT 参考）
- [ ] periodic 模式设置
- [ ] 替代 PIT 作为调度 tick 源
- [ ] 保留 PIT 作为 fallback

### T3: I/O APIC 驱动

**文件**: `kernel/drivers/apic/io_apic.hpp`, `kernel/drivers/apic/io_apic.cpp`

```cpp
class IOAPIC {
public:
    void init(uint64_t mmio_base, uint32_t gsi_base);

    uint32_t read(uint32_t reg);
    void write(uint32_t reg, uint32_t value);

    // 重定向表（将 GSI 路由到特定 CPU 的 Local APIC）
    void set_redirect(uint32_t gsi, uint8_t vector, uint8_t dest_apic_id,
                      bool active_low = false, bool level_triggered = false);

    // 屏蔽/解除屏蔽
    void mask(uint32_t gsi);
    void unmask(uint32_t gsi);

    // 信息
    uint32_t version();
    uint32_t max_redirect();  // 最大重定向条目数

private:
    volatile uint32_t* base_;
    uint32_t gsi_base_;
};

extern IOAPIC g_ioapic;
```

**I/O APIC 寄存器**:
| 偏移 | 名称 |
|------|------|
| 0x00 | IOREGSEL — 寄存器选择 |
| 0x10 | IOWIN — 数据窗口 |

**重定向条目格式**（64-bit，通过两次 32-bit 读/写）:
- Bit 0-7: Vector
- Bit 8-10: Delivery mode (Fixed/Lowest Priority/SMI/NMI/INIT/ExtINT)
- Bit 11: Destination mode (Physical/Logical)
- Bit 12: Delivery status
- Bit 13: Polarity (Active High/Low)
- Bit 14: Remote IRR
- Bit 15: Trigger mode (Edge/Level)
- Bit 16: Mask
- Bit 56-63: Destination APIC ID

- [ ] I/O APIC MMIO 映射
- [ ] set_redirect() — 配置中断路由
- [ ] mask/unmask 操作
- [ ] 从 MADT Interrupt Source Override 配置非标准 IRQ 映射

### T4: PIC → APIC 切换

**文件**: `kernel/arch/x86_64/pic.cpp`

- [ ] 启动时仍用 PIC（早期初始化）
- [ ] APIC 初始化完成后：
  - 屏蔽所有 PIC 中断（0xFF 写入 IMR）
  - 启用 Local APIC（Spurious Vector）
  - 配置 I/O APIC 重定向（覆盖 IRQ 0-15 → vector 32-47）
- [ ] 保留 PIC 代码作为 fallback

### T5: 单元测试

- [ ] Local APIC 初始化 + ID 读取
- [ ] APIC timer 校准 + 周期中断
- [ ] I/O APIC 重定向配置
- [ ] QEMU `-smp 2` 双核 APIC 初始化

## 产出物

- [ ] `kernel/drivers/apic/local_apic.hpp` / `.cpp`
- [ ] `kernel/drivers/apic/io_apic.hpp` / `.cpp`
- [ ] `kernel/drivers/apic/CMakeLists.txt`
- [ ] APIC timer 校准
- [ ] PIC → APIC 切换流程
- [ ] QEMU 双核验证
