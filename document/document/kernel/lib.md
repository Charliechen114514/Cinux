# 内核库

> 里程碑: `009_big_kernel_entry` `012_driver_serial`

## 功能概述

内核自包含的基础库，提供格式化输出（多后端 kprintf）和 freestanding 字符串/内存操作。

## kprintf 多后端架构 (`kernel/lib/kprintf.hpp/cpp`)

### 核心机制
- `using OutputSink = void(*)(char, void* ctx)` — 输出后端回调类型
- 最多注册 8 个后端 (serial, console 等)
- `kprintf/kvprintf` 遍历所有 enabled sink 调用

### API
- `kprintf_init()` — 默认注册 serial sink
- `kprintf_register_sink(OutputSink, void* ctx)` — 注册新后端
- `kprintf(fmt, ...)` / `kvprintf(fmt, va_list)` — 格式化输出
- `kpanic(fmt, ...) [[noreturn]]` — 格式化输出后 halt

### 格式化能力
- `%d %u %x %X %s %p %c %%`
- `%08x` / `%-10s` 等宽度和补零修饰
- `%p` 输出 `0x` + 16 位十六进制

### 内部实现 (`kernel/lib/private/vkprintf_impl.hpp`)
- `fmt_uint(val, base, width, pad, upper, buf, len)` — 前补零支持

## 字符串/内存操作 (`kernel/lib/string.hpp/cpp`)
- `memset` / `memcpy` / `memmove` / `memcmp`
- `strcmp` / `strncmp` / `strlen`

## 原子操作 (`kernel/lib/atomic.hpp`)
- 自实现的原子操作，替代 `std::atomic` 依赖

## 源码位置
- `kernel/lib/kprintf.hpp/cpp`
- `kernel/lib/private/vkprintf_impl.hpp`
- `kernel/lib/string.hpp/cpp`
- `kernel/lib/atomic.hpp`
