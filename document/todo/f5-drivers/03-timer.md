# M4: HPET + RTC 定时器

> HPET 高精度定时器 + RTC 实时时钟。
> HPET 作为高精度时间源补充 APIC Timer，RTC 提供日期时间。

## 任务清单

### T1: HPET 驱动

**文件**: `kernel/drivers/hpet/hpet.hpp`, `kernel/drivers/hpet/hpet.cpp`

从 ACPI HPET 表获取 MMIO 地址：

```cpp
namespace cinux::drivers {

class HPET {
public:
    void init(uint64_t mmio_base);

    // 计数器
    uint64_t counter();              // 当前计数值
    uint64_t frequency();            // Hz
    uint64_t nanoseconds_per_tick();

    // 定时器通道（0-2 可用，3+ 可选）
    void timer_set(uint32_t channel, uint64_t ns, bool periodic, uint32_t irq_vector);
    void timer_stop(uint32_t channel);
    void timer_enable(uint32_t channel);
    void timer_disable(uint32_t channel);

    // 时间转换
    uint64_t ticks_to_ns(uint64_t ticks);
    uint64_t ns_to_ticks(uint64_t ns);

private:
    volatile struct HPETRegs* regs_;
    uint64_t period_;  // femtoseconds per tick
    uint64_t freq_hz_;
};

extern HPET g_hpet;

} // namespace cinux::drivers
```

**HPET 寄存器布局**:
| 偏移 | 名称 | 用途 |
|------|------|------|
| 0x000 | General Capabilities | period, vendor, count size |
| 0x010 | General Configuration | ENABLE, LEGACY_RT_ROUTE |
| 0x020 | General Interrupt Status | 中断状态 |
| 0x0F0 | Main Counter Value | 64-bit 自由运行计数器 |
| 0x100+0x20*n | Timer n Config | 定时器通道配置 |
| 0x108+0x20*n | Timer n Comparator | 比较值 |

- [ ] HPET MMIO 映射（从 ACPI HPET 表获取地址）
- [ ] counter() 读取主计数器
- [ ] 频率计算（从 period 字段，单位 femtoseconds）
- [ ] 定时器通道配置（one-shot + periodic）
- [ ] 中断集成（配置路由到 APIC）
- [ ] 纳秒级时间 API

### T2: 系统时间 API

**文件**: `kernel/lib/time.hpp`

```cpp
namespace cinux::lib {

// 获取系统时间（纳秒精度）
uint64_t time_ns();

// 获取系统时间（微秒精度）
uint64_t time_us();

// 获取系统时间（毫秒精度）
uint64_t time_ms();

// 获取 Unix 时间戳（秒）
uint64_t time_epoch();

} // namespace cinux::lib
```

- [ ] 基于 HPET counter 实现 time_ns/us/ms
- [ ] 启动时从 RTC 读取 epoch 偏移
- [ ] 与现有 PIT get_uptime_ms() 对齐

### T3: RTC 驱动

**文件**: `kernel/drivers/rtc/rtc.hpp`, `kernel/drivers/rtc/rtc.cpp`

CMOS RTC 通过端口 0x70/0x71 访问：

```cpp
struct DateTime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

class RTC {
public:
    void init();
    DateTime read();
    uint64_t epoch_seconds();  // 转为 Unix 时间戳
};

extern RTC g_rtc;
```

**CMOS 寄存器**:
| 偏移 | 内容 |
|------|------|
| 0x00 | 秒（BCD） |
| 0x02 | 分钟（BCD） |
| 0x04 | 小时（BCD） |
| 0x06 | 星期 |
| 0x07 | 日（BCD） |
| 0x08 | 月（BCD） |
| 0x09 | 年（BCD） |
| 0x0B | 状态 B（24h 模式、BCD 格式） |

- [ ] CMOS 端口读取
- [ ] BCD → 二进制转换
- [ ] 24h/12h 模式检测
- [ ] 世纪处理（2000+）
- [ ] epoch_seconds() 转换
- [ ] RTC 更新完成检测（UIP bit）

### T4: Syscall 集成

- [ ] clock_gettime(CLOCK_REALTIME) — 基于 HPET + RTC epoch
- [ ] clock_gettime(CLOCK_MONOTONIC) — 基于 HPET counter
- [ ] gettimeofday() — clock_gettime 封装
- [ ] nanosleep() — 基于 HPET 定时器通道

### T5: 单元测试

- [ ] HPET counter 单调递增
- [ ] HPET 定时器周期中断
- [ ] RTC 读取合理日期
- [ ] time_ns() 精度验证
- [ ] nanosleep 睡眠精度

## 产出物

- [ ] `kernel/drivers/hpet/hpet.hpp` / `.cpp`
- [ ] `kernel/drivers/rtc/rtc.hpp` / `.cpp`
- [ ] `kernel/lib/time.hpp` — 系统时间 API
- [ ] clock_gettime / gettimeofday / nanosleep syscall
- [ ] CMakeLists.txt 更新
