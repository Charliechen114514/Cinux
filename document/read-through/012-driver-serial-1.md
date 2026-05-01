# UART 16550 串口驱动完整代码走读

## 概览

这篇文章我们来逐行拆解 Cinux 大内核的 UART 16550 串口驱动——`kernel/drivers/serial.hpp` 和 `kernel/drivers/serial.cpp`。这个驱动采用轮询式（polling）I/O 模型，通过 x86 端口映射 I/O（PMIO）与 UART 硬件交互，为 `kprintf` 提供字符输出能力。整个设计非常直截了当——没有中断驱动的收发，没有 DMA，没有环形缓冲区，就是最朴素的"死等 THR 空然后写一个字节"的模式。在我们这个阶段，这种设计足够好用，而且调试起来非常直观。

## 架构图

```
                    kernel_main()
                        |
                  kprintf_init()
                        |
                   Serial::init()
                        |
            +-----------+-----------+
            |                       |
        kprintf()               kpanic()
            |                       |
     vkprintf_impl()         vkprintf_impl()
            |                       |
     Serial::putc() * N      Serial::putc() * N
            |                       |
     poll LSR.TX_READY         poll LSR.TX_READY
            |                       |
        io_outb(THR)            io_outb(THR)
            |                       |
        COM1 (0x3F8)            COM1 (0x3F8)
            |                       |
          QEMU                    QEMU
```

硬件寄存器的访问路径是这样的：CPU 通过 `outb` 指令往 `base_port + offset` 写数据，UART 芯片根据 offset 映射到不同的寄存器（THR、IER、FCR、LCR、MCR、LSR 等），QEMU 的虚拟 UART 收到数据后输出到终端。

## 代码精讲

### 端口地址常量与寄存器偏移定义

```cpp
// kernel/drivers/serial.hpp

namespace cinux::drivers {

// ============================================================
// Serial Port Base Addresses (x86 standard)
// ============================================================
constexpr uint16_t SERIAL_COM1 = 0x03F8;
constexpr uint16_t SERIAL_COM2 = 0x02F8;
constexpr uint16_t SERIAL_COM3 = 0x03E8;
constexpr uint16_t SERIAL_COM4 = 0x2E8;
```

PC 兼容机上的 COM 端口地址是历史遗留的标准分配。COM1 固定在 `0x3F8`，COM2 在 `0x2F8`，COM3 在 `0x3E8`，COM4 在 `0x2E8`。这些地址最早可以追溯到 IBM PC/AT 时代，那时候的硬件就是这么硬编码的，直到今天 QEMU 和绝大多数硬件模拟器依然沿用这个映射。我们目前只使用 COM1——毕竟内核调试只需要一个输出通道就足够了。这些常量被定义为 `constexpr`，编译期就确定了值，不会有任何运行时开销。

接下来是 UART 寄存器的偏移定义：

```cpp
namespace SerialReg {
constexpr uint8_t RBR = 0;  ///< Receive Buffer Register  (read)
constexpr uint8_t THR = 0;  ///< Transmit Holding Register (write)
constexpr uint8_t IER = 1;  ///< Interrupt Enable Register
constexpr uint8_t FCR = 2;  ///< FIFO Control Register
constexpr uint8_t LCR = 3;  ///< Line Control Register
constexpr uint8_t MCR = 4;  ///< Modem Control Register
constexpr uint8_t LSR = 5;  ///< Line Status Register
constexpr uint8_t MSR = 6;  ///< Modem Status Register
constexpr uint8_t SCR = 7;  ///< Scratch Register
}  // namespace SerialReg
```

你会注意到 RBR 和 THR 共享偏移 0——这是 UART 16550 的标准设计，同一偏移在读操作时映射到接收缓冲寄存器（RBR），在写操作时映射到发送保持寄存器（THR）。IER 控制中断使能、FCR 控制 FIFO、LCR 控制线路参数（波特率、数据位、校验位、停止位）、MCR 控制 Modem 信号、LSR 提供线路状态信息（也是我们轮询的核心）、MSR 报告 Modem 状态、SCR 是一个通用的暂存寄存器。每个寄存器相对 `base_port` 的偏移量固定为 0-7，所以实际的 I/O 端口就是 `0x3F8+0` 到 `0x3F8+7`。

LSR 寄存器里我们关心的位只有两个：

```cpp
namespace SerialLSR {
constexpr uint8_t RX_READY  = 0x01;  ///< Data available in RBR
constexpr uint8_t TX_READY  = 0x20;  ///< THR empty, safe to write
}  // namespace SerialLSR
```

bit 0（RX_READY）表示 RBR 里有数据可读，bit 5（TX_READY）表示 THR 已空、可以写入新字节。这两个标志就是我们驱动工作的全部基础。

### Serial 类声明

```cpp
class Serial {
public:
    explicit Serial(uint16_t port = SERIAL_COM1);

    void init(uint16_t port = SERIAL_COM1, uint32_t baud = 115200);
    void putc(char c);
    void puts(const char* s);
    bool is_ready() const;

private:
    uint16_t base_port_;
    bool is_tx_ready() const;
};
```

这个类非常精简。构造函数接受一个端口号（默认 COM1），但不做硬件配置——真正的初始化延迟到 `init()` 调用时才发生。`putc()` 写一个字符（阻塞轮询），`puts()` 写一个字符串（自动转换 `\n` 为 `\r\n`），`is_ready()` 是一个公开的查询接口，内部调用私有的 `is_tx_ready()`。唯一的成员变量是 `base_port_`，保存构造时传入的端口基地址。

这种"构造不初始化"的设计是有意为之的。在内核启动阶段，全局对象的构造函数可能在我们完全控制硬件之前就被调用了，所以把硬件配置推迟到 `init()` 调用是一个更安全的策略。

### 构造函数

```cpp
// kernel/drivers/serial.cpp

Serial::Serial(uint16_t port)
    : base_port_(port) {
    // Caller calls init() explicitly after construction.
}
```

什么都没做，就是把端口地址存下来。这很对——我们需要在 `kernel_main()` 里显式调用 `kprintf_init()` 来触发串口的硬件初始化，而不是依赖 C++ 全局构造函数的执行顺序。

### init() —— UART 初始化序列

```cpp
void Serial::init(uint16_t /*port*/, uint32_t /*baud*/) {
    // Disable interrupts, set 8N1, enable FIFO, set MCR, verify LSR
    io_outb(base_port_ + SerialReg::IER, 0x00);
    io_outb(base_port_ + SerialReg::LCR, 0x03);
    io_outb(base_port_ + SerialReg::FCR, 0xC7);
    io_outb(base_port_ + SerialReg::MCR, 0x03);
    io_inb(base_port_ + SerialReg::LSR);
}
```

整个初始化序列只有五条 I/O 指令，但每一条都有它的目的。

第一条 `IER = 0x00` 禁用所有 UART 中断。我们采用的是轮询模式，不需要 UART 产生任何中断，所以干脆全部关掉。在更复杂的驱动里（比如 Linux 的 8250 驱动），IER 会被用来使能接收数据中断和发送空中断，以实现中断驱动的异步 I/O。

第二条 `LCR = 0x03` 设置 8 个数据位、无校验、1 个停止位——也就是经典的 8N1 配置。`0x03` 这个值的含义是 bit 0-1 = `11`（8 位数据），bit 2 = `0`（无校验），bit 3 = `0`（1 个停止位）。我们这里没有设置 DLAB（Divisor Latch Access Bit，bit 7），因为 QEMU 的虚拟 UART 默认就工作在 115200 波特率，不需要手动设置分频器。

第三条 `FCR = 0xC7` 启用 FIFO 并设置触发阈值。`0xC7` = `11000111b`：bit 0 = 1 启用 FIFO，bit 1 = 1 清空接收 FIFO，bit 2 = 1 清空发送 FIFO，bit 6-7 = `11` 设置接收 FIFO 触发阈值为 14 字节。这个 14 字节的阈值在轮询模式下不太重要（反正我们只做发送），但设置好 FIFO 可以让硬件在接收时有一定的缓冲能力。

第四条 `MCR = 0x03` 设置 RTS（Request To Send）和 DTR（Data Terminal Ready）信号。这是 Modem 控制信号，在虚拟环境下意义不大，但某些终端模拟器会检查这些信号来决定是否建立连接，所以还是设置一下比较稳妥。

最后一条 `io_inb(LSR)` 是一个"验证读"——读一次 LSR 确保它能正常响应，同时也顺便清除了可能存在的挂起状态。

你可能会问，为什么我们没有设置波特率？因为 QEMU 的虚拟 UART 默认就是 115200，而且我们的测试环境里没有真实的 Modem 需要协商，所以直接跳过了 DLAB 分频器的设置。如果哪天要跑在真实硬件上，这一步就必须补上了。

### putc() —— 轮询发送单个字符

```cpp
void Serial::putc(char c) {
    while (!is_tx_ready()) {
        __asm__ volatile("pause");
    }

    io_outb(base_port_ + SerialReg::THR, static_cast<uint8_t>(c));
}
```

这是驱动最核心的方法。进入一个 `while` 循环，反复读取 LSR 寄存器的 bit 5（TX_READY），直到 THR 为空。循环体里的 `__asm__ volatile("pause")` 是 x86 的 hint 指令，告诉 CPU "我在等一个资源，你可以降低功耗或者让出超线程资源给另一个逻辑核"。在 spin-wait 循环里使用 `pause` 是一个好习惯——虽然对我们的 QEMU 单核测试环境来说区别不大，但在真实的多核平台上能显著减少功耗和总线争用。

等 THR 空了之后，直接用 `io_outb` 把字符写到 THR 寄存器。UART 硬件会负责把 THR 里的字节串行发送出去。

### is_tx_ready() 和 is_ready()

```cpp
bool Serial::is_tx_ready() const {
    return (io_inb(base_port_ + SerialReg::LSR) & SerialLSR::TX_READY) != 0;
}

bool Serial::is_ready() const {
    return is_tx_ready();
}
```

私有方法 `is_tx_ready()` 读 LSR 寄存器，检查 bit 5 是否为 1。公开方法 `is_ready()` 目前只是转发调用，但保留了这个接口以便未来扩展——比如同时检查 TX 和 RX 的就绪状态。

### puts() —— 发送字符串并处理换行

```cpp
void Serial::puts(const char* s) {
    if (s == nullptr) {
        return;
    }

    while (*s != '\0') {
        if (*s == '\n') {
            putc('\r');
        }
        putc(*s);
        s++;
    }
}
```

`puts()` 遍历字符串中的每个字符，遇到 `\n` 时先发送一个 `\r`（回车），然后发送 `\n`（换行）。这是串口通信的标准约定——终端需要 `\r\n` 才能正确换行，而不是像 Unix 文件系统那样只用 `\n`。如果传入 `nullptr`，直接返回，避免空指针崩溃。

## 设计决策

### Decision: 轮询式 vs 中断驱动式 I/O

**问题**: 串口驱动应该采用轮询还是中断驱动的 I/O 模型？

**本项目的做法**: 纯轮询，`putc()` 里 `while (!is_tx_ready())` 死等。

**备选方案**: 中断驱动——使能 UART 的 TX 中断，当 THR 空时 UART 产生中断，ISR 从环形缓冲区取数据发送。Linux 8250 驱动就是这么做的，还带 spinlock 保护 SMP 并发。xv6 的 UART 驱动也有中断模式，配合 `sleep()`/`wakeup()` 实现异步输出。

**为什么不选备选方案**: Cinux 现阶段是单核内核，`kprintf` 的调用场景全部是内核代码主动打印调试信息，不存在并发竞争。轮询式简单、可靠、不需要锁、不需要缓冲区、不需要中断处理——对于一个教育内核的调试输出来说，这就是最合适的选择。等以后有了多任务和用户进程，再考虑中断驱动也不迟。

### Decision: 硬编码波特率 vs DLAB 分频器编程

**问题**: 是否需要通过 DLAB 分频器显式设置波特率？

**本项目的做法**: 不设置波特率，依赖 QEMU 默认的 115200。

**备选方案**: 在 `init()` 中设置 DLAB 位，然后往 offset 0 和 offset 1 写入分频值。标准计算公式是 `divisor = 115200 / desired_baud`，比如 9600 baud 就写 12。

**为什么不选备选方案**: QEMU 的虚拟 UART 默认就工作在 115200，不管你设不设 DLAB。在真实硬件上这一步是必须的，但我们的目标平台就是 QEMU，没必要增加代码。如果以后要支持真机，需要在 `init()` 里加上 DLAB 编程步骤。

### Decision: init() 的参数被忽略

**问题**: `init(uint16_t port, uint32_t baud)` 的两个参数为什么被注释掉了？

**本项目的做法**: 参数声明保留在接口上但实现中忽略，用 `/*port*/` 标注。端口地址在构造函数中已经确定，波特率依赖 QEMU 默认值。

**备选方案**: 在 `init()` 中真正使用这些参数来重新配置端口和波特率。

**为什么不选备选方案**: 接口保留参数是为了未来兼容性——如果以后需要动态切换波特率或端口，调用方不需要改。但目前实现中不使用它们是为了保持简单，不做多余的工作。

## 扩展方向

- **实现 getc() 接收方法**: 轮询 LSR 的 RX_READY 位（bit 0），从 RBR 读取字节。这是实现内核交互式调试的基础。（难度：⭐）

- **添加 DLAB 波特率编程**: 在 `init()` 中通过设置 LCR bit 7 启用 DLAB，然后写入分频值。在 QEMU 里验证不同波特率（9600、19200、115200）的行为差异。（难度：⭐）

- **中断驱动发送**: 使能 IER 的 TX 中断，配合环形缓冲区和 ISR 实现异步输出。测量轮询和中断两种模式下的吞吐量差异。（难度：⭐⭐）

- **多端口支持**: 在 `kernel_main` 中构造第二个 `Serial` 实例（COM2），实现独立的调试输出通道，例如一个用于普通日志、一个用于 panic 输出。（难度：⭐）

- **与 SerenityOS 的 CharacterDevice 集成对比**: 研究 SerenityOS 如何将 `Serial16550` 包装成 VFS 中的 `/dev/ttyS*` 字符设备，理解从"裸硬件驱动"到"用户可访问设备文件"的演进路径。（难度：⭐⭐⭐）

## 参考资料

- Intel SDM: Vol. 2A Chapter 3 — IN/OUT 指令格式与操作数约束，8-bit 端口号用立即数，16-bit 端口号用 DX 寄存器
- OSDev Wiki: [Serial Ports](https://wiki.osdev.org/Serial_Ports) — UART 16550 完整寄存器映射、COM 端口地址、轮询收发、8N1 协议、波特率分频器计算
- Linux `drivers/tty/serial/8250/8250_early.c` — Linux 早期串口控制台实现，乐观写后轮询模式（先写 THR 再等 LSR），支持 PMIO/MMIO 多种 I/O 类型
- xv6 (MIT) `uart.c` — RISC-V 上的 MMIO UART 驱动，轮询 `uartputc_sync()` 与中断驱动 `uartputc()` 并存
- SerenityOS `Kernel/SerialIO/Serial16550.cpp` — 生产级串口驱动，`CharacterDevice` VFS 集成，spinlock SMP 保护，`IOWindow` PMIO/MMIO 抽象
