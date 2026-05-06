# M3: AP 启动 + Per-CPU 数据

> 启动 Application Processor（非 BSP 的其他核心）。
> 建立 Per-CPU 数据机制，每个核心独立管理自己的数据。

## 依赖

- M1 ACPI（CPU 列表）
- M2 APIC（IPI 发送能力）

## 任务清单

### T1: AP 启动协议

**文件**: `kernel/arch/x86_64/ap_boot.S`（新增）

x86_64 AP 启动序列（Intel SDM 规定的 INIT-SIPI-SIPI 协议）：

```
BSP 执行:
1. 为每个 AP 准备启动代码（放置在物理地址 0x8000，16-bit 实模式）
2. 发送 INIT IPI → AP 开始执行 BIOS 初始化
3. 等待 10ms
4. 发送 SIPI (Startup IPI) → vector = 0x08（即物理地址 0x8000）
5. 等待 200us
6. 发送第二个 SIPI（重试）
7. 等待 AP 报告就绪

AP 执行:
1. 在 0x8000 开始执行（16-bit 实模式）
2. 切换到保护模式
3. 切换到长模式
4. 加载临时 GDT/IDT
5. 跳转到 C 函数 ap_main()
6. 设置 Per-CPU 数据
7. 初始化 Local APIC
8. 报告就绪（设置 smp_init_barrier）
9. 进入调度器
```

**AP 启动代码**（16-bit 实模式 → 64-bit 长模式）：
```asm
; ap_trampoline.S — 放置在 0x8000
[bits 16]
ap_trampoline:
    cli
    ; 启用 A20 line
    ; 加载临时 GDT
    ; 切换保护模式 (CR0.PE = 1)
    ; far jump 到 32-bit 代码
[bits 32]
ap_pm:
    ; 启用 PAE (CR4.PAE = 1)
    ; 加载 PML4 页表 (CR3)
    ; 启用长模式 (EFER.LME = 1)
    ; 启用分页 (CR0.PG = 1)
    ; far jump 到 64-bit 代码
[bits 64]
ap_lm:
    ; 加载 64-bit GDT/IDT
    ; 设置栈
    ; 跳转 ap_main()
```

- [ ] AP trampoline 汇编（16→32→64 模式切换）
- [ ] INIT-SIPI-SIPI 发送逻辑
- [ ] 启动等待和超时处理
- [ ] AP 就绪同步屏障

### T2: AP C 入口

**文件**: `kernel/arch/x86_64/ap_main.cpp`

```cpp
extern "C" void ap_main(uint8_t apic_id) {
    // 1. 设置 Per-CPU 数据
    percpu_init(apic_id);

    // 2. 设置 AP 自己的栈
    // 3. 初始化 AP 自己的 Local APIC
    // 4. 加载完整的 GDT/IDT
    // 5. 启用 APIC timer
    // 6. 标记 AP 就绪
    atomic_fetch_add(&smp_ready_count, 1);

    // 7. 进入调度器（idle loop）
    scheduler_ap_enter();
}
```

- [ ] ap_main() C 入口
- [ ] Per-CPU 初始化
- [ ] AP 本地 Local APIC 初始化
- [ ] 就绪计数同步

### T3: Per-CPU 数据机制

**文件**: `kernel/arch/x86_64/percpu.hpp`, `kernel/arch/x86_64/percpu.cpp`

利用 GS 段寄存器基址指向 Per-CPU 数据块：

```cpp
struct PerCpu {
    uint32_t cpu_id;         // CPU 编号 (0, 1, 2, ...)
    uint32_t apic_id;        // Local APIC ID
    Task*    current_task;   // 当前运行的 task
    uint64_t kernel_stack;   // 内核栈顶
    uint32_t lapic_timer_hz; // 本 CPU 的 APIC timer 频率

    // 调度
    SchedulingClass* sched;  // 本 CPU 的调度器实例
    void*            run_queue; // 本 CPU 的 run queue

    // 统计
    uint64_t interrupt_count;
    uint64_t context_switch_count;
};

// 获取当前 CPU 的 PerCpu（通过 GS 基址）
PerCpu* percpu();
uint32_t cpu_id();            // 当前 CPU 编号
uint32_t cpu_count();         // 总 CPU 数

// 初始化 PerCpu 数据块
void percpu_init(uint32_t apic_id);
```

**实现**:
- 启动时为每个 CPU 分配一个 PerCpu 结构（4KB 对齐）
- 通过 `wrmsr(MSR_GS_BASE, &percpu_block[cpu_id])` 设置 GS 基址
- `percpu()` 直接读 GS:0 获取

- [ ] PerCpu 结构体定义
- [ ] percpu() 通过 GS 基址快速访问
- [ ] BSP 初始化时设置自己的 PerCpu
- [ ] 每个 AP 启动时设置自己的 PerCpu

### T4: 全局 CPU 管理器

**文件**: `kernel/proc/cpu.hpp`

```cpp
namespace cinux::proc {

struct CPUInfo {
    uint32_t  cpu_id;
    uint32_t  apic_id;
    bool      online;       // 是否在线
    PerCpu*   percpu;
};

class CPUManager {
public:
    void init(const acpi::ACPIInfo& acpi);
    uint32_t count() const;
    const CPUInfo& info(uint32_t cpu_id) const;
    void mark_online(uint32_t cpu_id);

private:
    CPUInfo cpus_[MAX_CPUS];  // MAX_CPUS = 16
    uint32_t count_;
};

extern CPUManager g_cpu_mgr;

} // namespace cinux::proc
```

- [ ] CPUManager 从 ACPIInfo 初始化 CPU 列表
- [ ] mark_online() AP 就绪后标记
- [ ] 当前仅支持 QEMU `-smp N`（N ≤ 16）

### T5: 多核 GDT/IDT

- [ ] AP 启动时加载与 BSP 相同的 GDT 和 IDT
- [ ] IDT 中 APIC timer 中断和 IPI 中断注册
- [ ] APIC spurious interrupt handler

### T6: 单元测试

- [ ] QEMU `-smp 1/2/4` 启动验证
- [ ] Per-CPU 数据独立（每个 CPU 读到不同的 cpu_id）
- [ ] AP 就绪同步（所有 AP 报告后 BSP 继续）

## 产出物

- [ ] `kernel/arch/x86_64/ap_boot.S` — AP trampoline
- [ ] `kernel/arch/x86_64/ap_main.cpp` — AP C 入口
- [ ] `kernel/arch/x86_64/percpu.hpp` / `.cpp` — Per-CPU 机制
- [ ] `kernel/proc/cpu.hpp` — CPU 管理器
- [ ] QEMU 多核启动验证
