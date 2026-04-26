# 串口驱动

> 里程碑: `012_driver_serial`

## 功能概述

COM1 串口 I/O 驱动，内核最早的输出通道。kprintf 默认注册 serial 为第一个后端。

## API (`kernel/drivers/serial/serial.hpp/cpp`)
- `Serial::init(port, baud)` — 初始化串口
- `Serial::putc(char c)` — 输出单字节
- `Serial::puts(const char* s)` — 输出字符串
- `Serial::is_ready()` — 检查发送就绪

## 与 kprintf 的关系
- `kprintf_init()` 默认注册 serial sink
- 通过 `OutputSink` 回调接口接入 kprintf 多后端架构

## 源码位置
- `kernel/drivers/serial/serial.hpp/cpp`
