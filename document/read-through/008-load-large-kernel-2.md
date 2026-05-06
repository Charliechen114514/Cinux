# 008-2 Read-through: ATA PIO 扇区读取——init() 与 read()

## 概览

本文是 tag 008 代码讲解的第二部分，聚焦于 ATA PIO 驱动的核心功能实现：`init()` 控制器初始化和 `read()` 扇区读取。上一篇我们讲解了常量定义和辅助函数，本篇将它们组合成完整的初始化和读取流程。下一篇将转向 ELF 解析器和大内核加载器。

---

## 代码精讲

### driver/ata.cpp -- init() 初始化

```cpp
bool init() {
    kprintf("[INIT] Initializing ATA controller...\n");

    // Step 1: Software reset
    io::outb(ATA_PRIMARY_CTRL, 0x04);  // SRST=1, nIEN=1
    delay_400ns();
    io::outb(ATA_PRIMARY_CTRL, 0x00);  // Clear reset
    delay_400ns();

    // Step 2: Wait for drive to come out of reset
    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: drive did not come out of reset (BSY timeout)\n");
        return false;
    }

    // Step 3: Select master drive with LBA mode
    write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER);
    delay_400ns();

    // Step 4: Verify drive is present and ready
    if (!wait_not_busy()) {
        kprintf("[ATA] ERROR: master drive not ready after selection\n");
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

初始化过程遵循 ATA 规范的标准流程。软件复位通过控制端口操作：写入 0x04（bit 1 = SRST 软件复位，bit 2 = nIEN 禁用中断），等 400ns，再写入 0x00 取消复位。nIEN 位保持禁用中断状态——因为我们用轮询模式，不需要磁盘控制器发送中断。

选择主盘时写入 `ATA_DRIVE_MASTER`（0xE0），这个值的 bit 6 置位启用 LBA 寻址模式。选择驱动器之后又等了 400ns——ATA 规范要求在驱动器切换后等待足够时间让新选中的驱动器响应。

验证驱动器存在性的方法很巧妙：如果总线上没有设备，所有端口读出来的值都是 0xFF（浮空总线的高阻态上拉）。所以 `status == 0xFF` 就是"没检测到设备"的标志。正常情况下，QEMU 的 ATA 设备初始化后 status 应该是 0x50（RDY=1，DSC=1，其余为 0）或者类似值。

---

### driver/ata.cpp -- read() 扇区读取

```cpp
bool read(uint64_t lba, uint16_t count, void* buffer) {
    // Parameter validation
    if (!s_initialized) { /* error */ return false; }
    if (count == 0) { /* error */ return false; }
    if (buffer == nullptr) { /* error */ return false; }
    if (lba >= (1ULL << 48)) { /* error */ return false; }

    if (!wait_not_busy()) { /* error */ return false; }

    // Determine addressing mode
    bool use_lba48 = (lba >= 0x10000000ULL) || (count > 256);

    if (use_lba48) {
        write_reg(ATA_REG_DRIVE, ATA_DRIVE_MASTER | 0x40);
        delay_400ns();

        // High order bytes first
        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>((count >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>((lba >> 24) & 0xFF));
        write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 32) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 40) & 0xFF));

        // Low order bytes second
        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
        write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>(lba & 0xFF));
        write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 16) & 0xFF));

        write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO_EXT);
    } else {
        // LBA28
        write_reg(ATA_REG_DRIVE,
                  ATA_DRIVE_MASTER | static_cast<uint8_t>((lba >> 24) & 0x0F));
        delay_400ns();

        write_reg(ATA_REG_SECTOR_CNT, static_cast<uint8_t>(count & 0xFF));
        write_reg(ATA_REG_LBA_LOW,    static_cast<uint8_t>(lba & 0xFF));
        write_reg(ATA_REG_LBA_MID,    static_cast<uint8_t>((lba >> 8) & 0xFF));
        write_reg(ATA_REG_LBA_HIGH,   static_cast<uint8_t>((lba >> 16) & 0xFF));

        write_reg(ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    }

    // Read sectors
    auto* buf = static_cast<uint16_t*>(buffer);
    for (uint16_t sector = 0; sector < count; sector++) {
        delay_400ns();
        if (!wait_data_ready()) { /* error */ return false; }

        for (int word = 0; word < 256; word++) {
            buf[word] = read_data();
        }
        buf += 256;
    }
    return true;
}
```

`read()` 函数是 ATA PIO 驱动的核心。它的参数校验很全面——检查驱动器是否初始化、扇区数是否为零、缓冲区是否为空、LBA 是否超出 48 位范围。这些防御性检查在 bootloader 级别的代码里特别重要，因为这里的 bug 通常表现为"静默地读到垃圾数据"，排查起来非常痛苦。

LBA28 和 LBA48 的命令序列有显著差异。LBA28 模式把 LBA 的高 4 位（bit 24-27）编码到驱动器选择寄存器的低 4 位，扇区计数只发一次（8 位，最多 256 个扇区），LBA 的低 24 位分别写入三个 LBA 端口。LBA48 模式则发送两次——第一次发送高 16 位（LBA[24:47] 和扇区计数高字节），第二次发送低 16 位（LBA[0:15] 和扇区计数低字节）。这种"先高后低"的顺序是 ATA 规范规定的，目的是让控制器在收到低字节时就能开始处理，而不必等所有字节到齐。

数据读取的核心循环很直观：对每个扇区，先等 400ns，然后等数据就绪（DRQ=1, BSY=0），接着连续读 256 个 16 位 word。缓冲区指针 `buf` 是 `uint16_t*` 类型，每次读一个 word 后指针自动前进 2 字节——所以读完一个扇区后 `buf += 256` 刚好前进 512 字节。

有一个值得注意的细节：LBA28 模式下 `count` 参数是 `uint16_t`，但写入扇区计数寄存器时做了 `& 0xFF` 截断为 8 位。在 LBA28 模式下，扇区计数寄存器为 0 表示 256 个扇区——所以当 count 恰好是 256 时，写入 0 是正确的。这也是为什么切换到 LBA48 的条件包含 `count > 256`——超过 256 就必须用 LBA48 了。

---

### main.cpp -- ATA 集成与 MBR Demo

```cpp
static uint8_t g_sector_buf[512] __attribute__((aligned(16)));

// In mini_kernel_main():
// ATA initialization
if (!cinux::mini::driver::ata::init()) {
    kprintf("[INIT] ERROR: ATA initialization failed!\n");
    while (1) __asm__ volatile("cli; hlt");
}

// Demo: Read MBR and verify boot signature
kprintf("[DEMO] Reading MBR (LBA 0)...\n");
if (cinux::mini::driver::ata::read(0, 1, g_sector_buf)) {
    uint16_t sig = static_cast<uint16_t>(g_sector_buf[510]) |
                   (static_cast<uint16_t>(g_sector_buf[511]) << 8);
    kprintf("[DEMO] MBR boot signature: 0x%04x %s\n", sig,
            sig == 0xAA55 ? "(VALID)" : "(INVALID)");
}

// Demo: Read mini kernel header and check for ELF
kprintf("[DEMO] Reading mini kernel header (LBA 16)...\n");
if (cinux::mini::driver::ata::read(16, 1, g_sector_buf)) {
    if (cinux::mini::elf_loader::parse_elf_header(g_sector_buf)) {
        kprintf("[DEMO] ELF header detected at disk LBA 16 (mini kernel)\n");
    } else {
        kprintf("[DEMO] No valid ELF header at LBA 16 (expected for flat binary)\n");
    }
}
```

main.cpp 中的 demo 代码设计得很巧妙——它不尝试加载大内核（因为大内核还不存在），而是通过两个小测试验证整个管线的可用性。

第一个 demo 读取 MBR（LBA 0）并验证引导签名 0xAA55。这个测试的意义在于：如果 ATA 驱动工作正常，读出来的第 510-511 字节应该是 0x55 和 0xAA（小端序下组合为 0xAA55）。MBR 的引导签名是固定的、已知的值，非常适合作为"第一个读取测试"的验证数据。

`g_sector_buf` 声明为 16 字节对齐，这对于 ATA PIO 的 `inw` 操作是最优的对齐方式（虽然 x86 上不对齐也不会崩溃，但对齐访问更快）。

---

## 设计决策

### 决策：PIO 轮询 vs DMA vs 中断驱动

**问题**: 磁盘读取有三种基本策略——PIO 轮询、PIO + 中断、DMA。选择哪种？

**本项目的做法**: PIO 轮询——CPU 发命令后主动轮询状态寄存器，等数据就绪后逐字读取。

**备选方案 1**: PIO + 中断——发命令后 CPU 做别的事，磁盘数据就绪时触发 IRQ14/IRQ15 中断。
**备选方案 2**: DMA——配置 DMA 引擎，让磁盘控制器直接把数据写到内存，完成后中断通知。

**为什么不选备选方案**: 在 bootloader/mini kernel 阶段，没有其他进程在运行，轮询的"CPU 浪费"不是问题。DMA 需要配置 PCI/ISA 总线 master、处理物理地址映射、管理 buffer descriptor，实现复杂度远高于 PIO。中断驱动 PIO 虽然不复杂，但需要先设置好 IDT 中的 IRQ handler——虽然我们已经有了 IDT（tag 007），但为了保持启动流程的简洁性，还是用轮询。

### 决策：LBA28/LBA48 双模式自动切换

**问题**: 支持 LBA28 就够了（QEMU 默认磁盘很小），还是需要实现 LBA48？

**本项目的做法**: 两者都实现，根据 LBA 地址和扇区数自动选择。

**备选方案**: 只实现 LBA28——代码更简单，QEMU 默认配置下够用。

**为什么不选备选方案**: LBA48 的实现代码量不大（约 20 行），而且将来对接更大的虚拟磁盘或真实硬件时不用改驱动代码。自动切换的逻辑也很直观——检查 LBA 和 count 的范围即可。教学价值方面，LBA48 的"先高后低"两次发送机制本身就是一个有趣的知识点。

---

## 扩展方向

1. **ATA 写入支持** -- 在 read 基础上添加 write 函数，命令码 0x30（LBA28）/ 0x34（LBA48），数据方向改为 outsw（难度：star-star）
2. **多扇区流水线读取** -- 利用 ATA 控制器的内部缓冲区，减少轮询等待时间（难度：star-star）

---

## 参考资料

- OSDev Wiki: [ATA read/write sectors](https://wiki.osdev.org/ATA_read/write_sectors) -- LBA28/LBA48 汇编代码示例
- OSDev Wiki: [ATA PIO Mode](https://wiki.osdev.org/ATA_PIO_Mode) -- PIO 模式的完整说明
