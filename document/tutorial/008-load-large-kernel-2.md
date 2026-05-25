---
title: 008-load-large-kernel-2 · 大内核加载
---

# 008-2 ATA 初始化与扇区读取：让 mini kernel 真正能从磁盘拿数据

> 标签：ATA PIO, 软件复位, LBA28/LBA48, 扇区读取, x86_64
> 前置：[008-1 ATA PIO 驱动框架](008-load-large-kernel-1.md)

## 前言

上一章我们搭建了 ATA PIO 驱动的常量定义和辅助函数框架，定义了 I/O 端口、状态位掩码和命令码，也手写了 freestanding 环境下的 memset/memcpy/memmove。这一章我们继续完成驱动的两个核心函数——`init()` 和 `read()`——然后在 main.cpp 中实际运行，验证磁盘读取管线是否通了。

## 环境说明

和上一章完全一致：C++ 内核（`-ffreestanding -nostdlib`），QEMU 模拟 x86_64 环境。磁盘镜像中 LBA 848 位置预留了大内核空间。ATA 驱动在 mini kernel 的身份映射环境下运行，使用物理地址进行操作。

## 第四步——ATA 初始化：软件复位

ATA 控制器的初始化遵循一个标准流程：软件复位 → 等待就绪 → 选择驱动器 → 验证存在性。软件复位通过控制端口 0x3F6 操作——先写入 0x04（bit 1 = SRST 复位，bit 2 = nIEN 禁用中断），等 400ns，再写入 0x00 取消复位。nIEN 位持续禁用中断，因为我们用轮询模式，不需要磁盘控制器发送 IRQ。

```cpp
bool init() {
    kprintf("[INIT] Initializing ATA controller...\n");

    io::outb(ATA_PRIMARY_CTRL, 0x04);   // SRST=1, nIEN=1
    delay_400ns();
    io::outb(ATA_PRIMARY_CTRL, 0x00);   // Clear reset
    delay_400ns();

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: drive did not come out of reset\n");
        return false;
    }

    write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER);  // 选主盘 + LBA
    delay_400ns();

    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: master drive not ready\n");
        return false;
    }

    uint8_t status = read_reg(ATA_REG_STATUS);
    if (status == 0xFF) {
        kprintf("[ATA] ERROR: no drive detected (floating bus)\n");
        return false;
    }
    if ((status & ATA_STATUS_RDY) == 0) {
        kprintf("[ATA] ERROR: drive not ready, status=0x%02x\n", status);
        return false;
    }

    s_initialized = true;
    kprintf("[INIT] ATA controller initialized successfully (status=0x%02x).\n", status);
    return true;
}
```

验证驱动器存在性的方法很巧妙：如果总线上没有设备，所有端口读出来的值都是 0xFF——这是浮空总线的高阻态上拉特征。所以 `status == 0xFF` 就是"没检测到设备"的标志。正常情况下 QEMU 的 ATA 设备初始化后 status 应该包含 RDY 位（bit 6 置 1），通常是 0x50。

## 第五步——扇区读取：LBA28 与 LBA48

`read()` 函数是 ATA PIO 驱动的核心，参数校验非常全面——检查驱动器是否初始化、扇区数是否为零、缓冲区是否为空、LBA 是否超出 48 位范围。然后根据 LBA 地址和扇区数自动选择寻址模式：LBA 超过 28 位范围（0x10000000）或扇区数超过 256 时使用 LBA48，否则使用 LBA28。

LBA28 把 24 位 LBA 分散到三个端口，高 4 位塞进驱动器选择寄存器的低 4 位。LBA48 的命令序列更长——先发送高 16 位（扇区计数高字节、LBA[24:47]），再发送低 16 位。这种"先高后低"的顺序是 ATA 规范规定的，目的是让控制器在收到低字节时就能开始处理。

```cpp
// LBA28 命令序列
write_reg(ATA_REG_DRIVE,
          ATA_DRIVE_MASTER | static_cast<uint8_t>((lba >> 24) & 0x0F));
delay_400ns();
write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>(lba & 0xFF));
write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 8) & 0xFF));
write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 16) & 0xFF));
write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO);   // 0x20
```

命令发送后进入逐扇区读取循环：每个扇区先等 400ns 延迟，然后等数据就绪，接着从数据端口连续读取 256 个 16 位 word。缓冲区指针每次前进 256 个 word（因为是 `uint16_t*`，所以 `buf += 256` 实际前进 512 字节）。有一个容易忽略的细节：LBA28 模式下扇区计数寄存器值为 0 实际表示 256 个扇区（不是 0 个！），LBA48 的 0 则表示 65536 个扇区。

## 其他 OS 怎么做磁盘读取？

说到这里，我们可以横向对比一下其他 OS 的做法。xv6 的 `bootmain()` 是 OS 开发领域最经典的磁盘读取代码之一——它在 32 位保护模式下直接操作 IDE 端口（0x1F0-0x1F7）读取内核 ELF，和我们做的事情完全一样，只不过 xv6 是 32 位系统而我们是在 64 位 long mode 下操作。xv6 把整个磁盘读取+ELF 加载的代码塞在 `bootmain.c` 这一个文件里，大约 100 行 C 代码，风格非常紧凑。

Linux 走了一条完全不同的路——它把 bootloader 级别的磁盘读取外包给 GRUB。GRUB 是一个功能完善的 bootloader，支持 ext2/3/4、FAT 等多种文件系统，能直接从 `/boot` 目录读取 `vmlinuz`。Linux 内核自身不需要实现 ELF 加载器——GRUB 负责解析 ELF/Multiboot 头、加载内核到正确位置、传递启动信息。Cinux 和 xv6 选择了"自己动手"的道路，虽然更累但更有教学价值。

SerenityOS 和 Linux 一样使用 GRUB + Multiboot 规范启动。GRUB 负责所有底层磁盘 I/O，SerenityOS 的内核代码因此更简洁——不需要在启动早期实现 ATA 驱动。

## 收尾——验证与运行

在 `main.cpp` 中，我们通过两个 demo 测试验证整个管线。第一个读取 MBR（LBA 0）验证引导签名 0xAA55——这是最经典的"第一个磁盘读取测试"，因为 MBR 的引导签名是固定的、已知的值，如果 ATA 驱动工作正常就一定能读到。第二个读取 LBA 16 尝试 ELF 解析。

```
[INIT] Initializing ATA controller...
[INIT] ATA controller initialized successfully (status=0x50).
[DEMO] Reading MBR (LBA 0)...
[DEMO] MBR boot signature: 0xAA55 (VALID)
[DEMO] Reading mini kernel header (LBA 16)...
```

到这里，mini kernel 已经具备了从磁盘读取任意扇区数据的能力。接下来的第三章里，我们会基于这个 ATA 驱动实现 ELF 解析器和大内核加载器，完成从"读磁盘"到"加载内核"的最后一步。

## 参考资料

- Intel SDM: Vol. 2A -- IN/OUT 指令（inw/outb 的编码和操作数约束）
  - Source: `helpers/web_search/intel_sdm/vol2a_io_port_instructions.md`
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) -- PIO 模式完整说明、端口布局、400ns 延迟要求
- OSDev Wiki: [ATA read/write sectors](https://wiki.osdev.org/ATA_read/write_sectors) -- LBA28/LBA48 代码示例
- xv6 bootmain.c: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/bootmain.c) -- 经典 bootloader 级 IDE PIO 磁盘读取
- Linux boot process: [linux-insides](https://0xax.gitbook.io/linux-insides/summary/booting/linux-bootstrap-1) -- GRUB 加载内核的完整流程
- SerenityOS: [GitHub](https://github.com/SerenityOS/serenity) -- GRUB + Multiboot 启动方式
