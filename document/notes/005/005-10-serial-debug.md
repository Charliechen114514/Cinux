---
title: 串口调试
---

# 005-10: 串口调试详解

## 概述

串口是裸机开发中最常用的调试输出方式。Cinux 使用 NS16550A/8250 兼容的 UART 驱动，将调试信息通过 COM1 端口（0x3F8）发送，由 QEMU 重定向到标准输出。

---

## 1. 硬件基础：NS16550A UART

### 寄存器映射

| 偏移 | 读寄存器 | 写寄存器 | 简写 | 说明 |
|------|----------|----------|------|------|
| +0 | RBR | THR | - | 接收/发送缓冲区 |
| +1 | - | IER | IER | 中断使能 |
| +2 | - | FCR | FCR | FIFO 控制 |
| +3 | - | LCR | LCR | 线路控制 |
| +4 | - | MCR | MCR | Modem 控制 |
| +5 | LSR | - | LSR | 线路状态 |
| +6 | MSR | - | MSR | Modem 状态 |
| +7 | SCR | SCR | SCR | 暂存器 |

### 标准 COM 端口基址

| 端口 | 基地址 |
|------|--------|
| COM1 | 0x3F8 |
| COM2 | 0x2F8 |
| COM3 | 0x3E8 |
| COM4 | 0x2E8 |

### LSR (线路状态寄存器) 位定义

| 位 | 名称 | 说明 |
|----|------|------|
| 0 | DR | 数据就绪（RBR 有数据） |
| 5 | THRE | 发送保持寄存器为空（可发送） |

---

## 2. 串口驱动实现

### 头文件

文件：[`kernel/mini/driver/serial.h`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/driver/serial.h)

```cpp
class Serial {
private:
    uint16_t base_port;  // COM1 = 0x3F8

    bool is_tx_ready() const {
        return (io::inb(base_port + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
    }

public:
    explicit Serial(uint16_t port = SERIAL_COM1);
    void putc(char c);     // 发送单个字符（阻塞）
    char getc();           // 接收单个字符（阻塞）
    bool has_data() const;
    void puts(const char* s);
    void init();           // 初始化为 115200 8N1
};

Serial& get_initial_serial();
```

### 初始化代码

文件：[`kernel/mini/driver/serial.cpp`](https://github.com/CinuxOS/Cinux/blob/main/kernel/mini/driver/serial.cpp#L20-L33)

```cpp
void Serial::init() {
    // 1. 禁用中断
    io::outb(base_port + SerialReg::IER, 0x00);

    // 2. 设置 8N1（8 位数据，无奇偶校验，1 位停止位）
    // LCR = 0x03
    io::outb(base_port + SerialReg::LCR, 0x03);

    // 3. 启用 FIFO，清除缓冲区，14 字节阈值
    io::outb(base_port + SerialReg::FCR, 0xC7);

    // 4. 设置 RTS + DTR（Modem 控制）
    io::outb(base_port + SerialReg::MCR, 0x03);
}
```

### 发送字符

```cpp
void Serial::putc(char c) {
    // 等待发送缓冲区就绪
    while (!is_tx_ready()) {
        __asm__ volatile("pause");  // 降低 CPU 占用
    }

    // 写入发送保持寄存器
    io::outb(base_port + SerialReg::THR, static_cast<uint8_t>(c));
}
```

### 发送字符串

```cpp
void Serial::puts(const char* s) {
    if (s == nullptr) return;

    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');  // 换行前发送回车
        }
        putc(*s);
        s++;
    }
}
```

---

## 3. QEMU 串口配置

### 命令行参数

来源：[`cmake/qemu.cmake`](https://github.com/CinuxOS/Cinux/blob/main/cmake/qemu.cmake#L8-L14)

```cmake
set(QEMU_COMMON_FLAGS
    -m 512M
    -serial stdio           # 串口重定向到标准 I/O
    -no-reboot
    -debugcon file:debug.log
    -global isa-debugcon.iobase=0xe9
)
```

### -serial 选项

| 值 | 说明 |
|---|------|
| `-serial stdio` | 重定向到标准输入输出 |
| `-serial file:serial.log` | 输出到文件 |
| `-serial pty` | 伪终端（Linux） |
| `-serial mon:stdio` | 与 QEMU monitor 混合 |
| `-serial none` | 禁用串口 |

---

## 4. 串口输出流程

```
┌─────────────────────────────────────────────────────────────┐
│                    串口输出流程                              │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  kprintf("Value: %d\n", 42)                                │
│       │                                                     │
│       ▼                                                     │
│  vkprintf_impl(lambda{serial.putc)})                       │
│       │                                                     │
│       ▼                                                     │
│  format_decimal(42, buffer, ...)                            │
│       │                                                     │
│       ▼                                                     │
│  buffer = "42", len = 2                                     │
│       │                                                     │
│       ▼                                                     │
│  serial.putc('4') ───► 等待 THRE ──► outb(0x3F8, '4')      │
│       │                                                     │
│       ▼                                                     │
│  serial.putc('2') ───► 等待 THRE ──► outb(0x3F8, '2')      │
│       │                                                     │
│       ▼                                                     │
│  serial.putc('\n') ──► 等待 THRE ──► outb(0x3F8, '\n')     │
│       │                                                     │
│       ▼                                                     │
│  QEMU 捕获 0x3F8 端口写入                                   │
│       │                                                     │
│       ▼                                                     │
│  重定向到 stdout（stdio）                                   │
│       │                                                     │
│       ▼                                                     │
│  终端显示: "42"                                            │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

## 5. 宿主机串口工具

### picocom（推荐）

```bash
# 安装
sudo apt install picocom

# 连接到串口
picocom -b 115200 /dev/ttyUSB0
picocom -b 115200 /dev/ttyS0

# 退出: Ctrl+A → Ctrl+X
```

### minicom

```bash
# 安装
sudo apt install minicom

# 配置
sudo minicom -s
# 设置: Serial Device: /dev/ttyUSB0
#       Baud Rate: 115200 8N1

# 运行
sudo minicom

# 退出: Ctrl+A → Q
```

### screen

```bash
# 连接
screen /dev/ttyUSB0 115200

# 退出: Ctrl+A → K → Y
```

### 工具对比

| 工具 | 优点 | 缺点 |
|------|------|------|
| picocom | 轻量、简单 | 功能少 |
| minicom | 功能丰富、可配置 | 配置复杂 |
| screen | 无需额外安装 | 需记住退出序列 |

---

## 6. 串口 vs DebugConsole

| 特性 | 串口 (COM1) | DebugConsole (0xE9) |
|------|-------------|---------------------|
| 端口 | 0x3F8 | 0xE9 |
| QEMU 参数 | `-serial stdio` | `-debugcon stdio/file:xxx` |
| 硬件标准 | NS16550A UART | QEMU 私有 |
| 实际硬件 | ✓ 支持 | ✗ 不支持 |
| 缓冲 | FIFO (14字节) | 无缓冲 |
| 速度 | 可配置 | 极快 |
| 用途 | 兼容输出 | 快速调试 |

### 何时使用哪个？

- **串口**：正式输出、兼容真实硬件
- **DebugConsole**：快速调试、早期启动

---

## 7. 真实硬件串口

### 硬件连接

```
PC (USB) ──► USB转TTL ──► UART_RX
                        ├─► UART_TX
                        ├─► GND
                        └─► VCC (可选)
```

### 波特率配置

| 波特率 | 分配器 | 使用场景 |
|--------|--------|----------|
| 9600 | 默认 | 传统设备 |
| 115200 | QEMU 默认 | 现代开发 |
| 921600 | 高速 | 大量日志 |

---

## 8. 故障排查

### 问题：无输出

**检查清单**：
1. QEMU 是否使用 `-serial stdio`
2. Serial::init() 是否被调用
3. 端口地址是否正确 (0x3F8)
4. 等待 THRE 的循环是否退出

### 问题：输出乱码

**原因**：波特率不匹配

**解决**：
```bash
# 确认 QEMU 默认 115200
# 驱动无需配置（QEMU 默认已是 115200 8N1）
```

### 问题：输出卡顿

**原因**：轮询等待

**优化**：
```cpp
// 添加 pause 指令降低 CPU 占用
while (!is_tx_ready()) {
    __asm__ volatile("pause");
}
```

---

## 9. 参考资源

### 参考文档
- [ArchWiki: Working with Serial Console](https://wiki.archlinux.org/title/Working_with_the_serial_console) - Linux 串口控制台
- [OpenWrt Wiki: Serial Console](https://openwrt.org/docs/techref/hardware/port.serial) - picocom 使用
- [NS16550A Datasheet](https://pdf.datasheetcatalog.com/datasheet2/national/447461.pdf) - 官方数据手册

### 相关链接
- [005-06: kprintf 格式化实现](005-06-kprintf-format.md) - 串口输出接口
- [005-11: 调试工作流总结](005-11-debug-workflows.md) - 完整调试流程
