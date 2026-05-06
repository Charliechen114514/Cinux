# 008-2 Hands-on: ATA PIO 扇区读取与磁盘验证

## 导语

上一节我们定义了 ATA PIO 驱动的常量和辅助函数，完成了控制器的初始化流程。这一节我们继续实现核心的 `read()` 函数——从磁盘读取任意数量的扇区，然后在 main.cpp 中验证整个管线。这是整个磁盘驱动最关键的部分，需要处理 LBA28 和 LBA48 两种寻址模式的自动切换，以及逐扇区的数据轮询读取。

完成本节后，我们可以在 main.cpp 中测试读取 MBR（LBA 0）并验证引导签名 0xAA55，确认磁盘读取管线完全通了。

---

## 概念精讲

### LBA28 模式详解

LBA28 使用 28 位地址空间。命令序列中，LBA 的低 24 位分别写入三个端口（LBA_LOW/MID/HIGH），最高 4 位（bit 24-27）被编码进驱动器选择寄存器的低 4 位。扇区计数寄存器为 8 位，值 0 表示 256 个扇区。

LBA28 的完整命令序列如下：先写入驱动器选择寄存器（0xE0 | LBA 高 4 位），等待 400ns；然后依次写入扇区计数、LBA 低字节、LBA 中字节、LBA 高字节；最后写入命令码 0x20（READ SECTORS）。这个顺序不能乱——ATA 控制器是按照固定的寄存器写入顺序来解读命令的。

### LBA48 模式详解

LBA48 使用 48 位地址空间。它的特点是寄存器要写两次——先发高 16 位，再发低 16 位。这种"先高后低"的顺序是 ATA 规范规定的 HOB（High Order Byte）机制：第二次写入某个寄存器时，第一次写入的值自动变成高字节。LBA48 的命令码是 `0x24`（READ SECTORS EXT）。

LBA48 的完整命令序列更长：写入驱动器选择寄存器（0xE0 | 0x40 表示 LBA48 模式），等待 400ns；先写入扇区计数高字节、LBA[24:31]、LBA[32:39]、LBA[40:47]（高位部分）；再写入扇区计数低字节、LBA[0:7]、LBA[8:15]、LBA[16:23]（低位部分）；最后写入命令码 0x24。这种两遍发送的机制让 LBA48 命令可以向后兼容 LBA28 的硬件。

### 缓冲区对齐与类型转换

ATA 数据端口是 16 位宽的，所以读取缓冲区需要当作 `uint16_t*` 来使用。指针算术中 `buf += 256` 实际前进 512 字节（256 x sizeof(uint16_t)），而不是 256 字节——这个细节搞错了读取的数据就会错位。缓冲区建议 16 字节对齐，虽然 x86 上不对齐也不会崩溃，但对齐访问更快。

---

## 动手实现

### Step 5: 实现 ATA PIO 驱动——read() 扇区读取

**目标**：实现对 ATA 主控制器的扇区读取功能，支持 LBA28 和 LBA48 两种寻址模式。

**设计思路**：读取流程分为参数校验、地址模式选择、命令发送、逐扇区数据读取四步。首先校验驱动器已初始化、扇区数不为零、缓冲区非空、LBA 在 48 位范围内。然后等待 BSY 清零。

地址模式选择：如果 LBA 超过 28 位范围或者扇区数超过 256，使用 LBA48 模式；否则使用 LBA28 模式。LBA48 模式的特点是先发送高字节（扇区计数高字节、LBA[24:31]、LBA[32:39]、LBA[40:47]），再发送低字节（扇区计数低字节、LBA[0:7]、LBA[8:15]、LBA[16:23]）。LBA28 模式把 LBA[24:27] 编码进驱动器选择寄存器的低 4 位。

命令发送后进入逐扇区读取循环：每个扇区先等 400ns 延迟，然后调用 `wait_data_ready` 等待数据就绪，接着从数据端口连续读取 256 个 16 位字（256 x 2 = 512 字节 = 1 个扇区）。缓冲区指针每次前进 256 个 word（512 字节）。

**实现约束**：函数签名是 `bool read(uint64_t lba, uint16_t count, void* buffer)`。返回 true 表示全部扇区读取成功，false 表示出错。缓冲区必须至少有 `count x 512` 字节的空间。缓冲区被当作 `uint16_t*` 使用以配合 16 位 `inw` 读取，所以需要适当的对齐（16 字节对齐比较安全）。

**踩坑预警**：LBA28 模式下，扇区计数寄存器值为 0 时实际表示 256 个扇区（不是 0 个！）。LBA48 模式下，0 表示 65536 个扇区。另外缓冲区类型转换时要小心——用 `static_cast<uint16_t*>` 转换 buffer，每次读 256 个 word 后指针加 256，而不是加 512（因为指针算术会自动乘以 sizeof(uint16_t) = 2）。

**验证**：编译运行 mini kernel，在 main.cpp 中添加测试代码——读取 LBA 0（MBR 扇区），检查最后两个字节是否为 0xAA55（引导签名）。串口输出应显示：

```
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xAA55 (VALID)
```

---

## 构建与运行

完整的构建和运行命令：

```bash
# 构建
cd build && cmake --build . --target mini_kernel

# 创建磁盘镜像（确保 MBR 和 Stage2 在正确的 LBA 位置）
# 具体命令取决于项目的 build_image.sh 脚本

# QEMU 启动
qemu-system-x86_64 -hda disk.img -serial stdio -display none
```

QEMU 参数说明：
- `-hda disk.img`：使用 disk.img 作为第一块硬盘（ATA 主通道主盘）
- `-serial stdio`：串口输出重定向到终端
- `-display none`：无图形界面（纯串口调试）

如果你想用 QEMU monitor 调试，可以加上 `-monitor telnet:localhost:4444,server,nowait`，然后另开终端 `telnet localhost 4444` 连接。

---

## 调试技巧

### 常见 Bug 1: 卡在轮询死循环里

如果程序卡在 `wait_not_busy` 或 `wait_data_ready` 里出不来，通常是因为命令发送不正确，或者磁盘控制器状态异常。用 GDB 或 QEMU monitor 检查：在 `wait_not_busy` 的循环里打断点，看 status 寄存器的值是否在变化。

QEMU monitor 调试命令：
```
# 查看 I/O 端口状态
info mice

# 单步执行
singlestep on

# 查看寄存器
info registers
```

GDB 调试方式：
```bash
qemu-system-x86_64 -hda disk.img -serial stdio -s -S
# 另一终端
gdb
(gdb) target remote :1234
(gdb) break *0x20000  # mini kernel 入口
(gdb) continue
```

### 常见 Bug 2: 读到的数据全是 0xFF 或 0x00

这说明你读错了端口，或者 LBA 地址不对。0xFF 通常是浮空总线的特征（没有设备响应），0x00 可能是读到了未写入的磁盘区域。检查：数据端口是否是 0x1F0（不是 0x1F7！），LBA 地址是否正确，扇区计数是否合理。

---

## 本章小结

| 概念 | 关键值/要点 |
|------|-------------|
| LBA28 最大扇区 | 256（count=0 时） |
| LBA28 地址范围 | 28 位 (0 ~ 0x0FFFFFFF) |
| LBA48 地址范围 | 48 位 |
| LBA28 命令码 | 0x20 |
| LBA48 命令码 | 0x24 |
| 模式自动切换条件 | LBA >= 0x10000000 或 count > 256 |
| LBA48 发送顺序 | 先高字节，后低字节 |
