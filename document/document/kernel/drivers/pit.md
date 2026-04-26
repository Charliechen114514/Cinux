# PIT 定时器

> 里程碑: `011_big_kernel_pic_irq`

## 功能概述

8254 PIT Channel 0 可编程定时器，为调度器提供时钟节拍。默认 100 Hz。

## API (`kernel/drivers/pit/pit.hpp/cpp`)
- `PIT::init(freq_hz=100)` — 写 CMD `0x43=0x36`，写 divisor=`1193182/freq_hz` 低/高字节到 `0x40`
- `PIT::get_ticks()` — 获取累计 tick 数
- `PIT::get_uptime_ms()` — 获取运行时间 (毫秒)
- IRQ0 handler — 递增 `tick_count`，每秒输出 `[TICK] uptime`

## 线程安全
- `tick_count_` 使用原子操作 (`kernel/lib/atomic.hpp`)

## 集成
- IRQ0 → `PIT::irq0_handler` → `Scheduler::tick()` → 时间片调度

## 源码位置
- `kernel/drivers/pit/pit.hpp/cpp`
