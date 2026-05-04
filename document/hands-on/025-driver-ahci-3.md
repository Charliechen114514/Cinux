# 025-3 动手篇：DMA 扇区读写与验证

## 导语

前两章我们完成了 PCI 枚举、BAR5 映射、HBA 复位、端口配置——所有基础设施已经就位。现在到了最激动人心的部分：真正让磁盘转起来。我们要构造 ATA 命令、设置 DMA 传输、读取磁盘的第一个扇区（LBA 0），验证 MBR 引导签名（0x55AA）。

完成本章后，你会在串口看到 `[AHCI] Read sector 0: 55 AA`——这 4 个十六进制字符标志着内核第一次成功从磁盘读取了数据。

前置知识：前两章的 PCI 和 AHCI 初始化结果；理解 DMA（Direct Memory Access）的基本概念——硬件自主完成数据搬运，不需要 CPU 逐字节干预。

## 概念精讲

### DMA 和 AHCI 命令流程

AHCI 的 DMA 工作流程可以用一句话概括：驱动在内存里摆好"任务单"（命令表 + PRDT），告诉 HBA "去做"，HBA 完成后通知驱动。

具体来说，"任务单"包含三层结构。最外层是命令列表里的 Command Header，它记录了命令 FIS 的长度、读写方向、PRDT 条目数量、以及命令表的物理地址。中间层是命令表（Command Table），前 64 字节存放命令 FIS（Frame Information Structure），后面是 PRDT 数组。最里层的 PRDT 条目描述了一个连续物理缓冲区的地址和大小。

命令下发的过程是：清零命令表、构造命令 FIS、填写 PRDT、配置 Command Header、清除中断状态、写 port.ci 的对应位触发命令、轮询等待 ci 位清零。

### Register H2D FIS

AHCI 用 FIS 来封装 ATA 命令。Register Host-to-Device FIS（类型 0x27）是最常用的 FIS 类型，用于向设备发送 ATA 命令。它的布局直接继承了传统 IDE 的任务文件寄存器：command 字节放 ATA 命令码，LBA 字段放目标扇区地址（48 位，分成 6 个字节），count 字段放扇区数量（16 位），device 字节的 bit 6 置 1 表示 LBA 模式。

FIS 的 flags 字段 bit 6 是 "command" 位——置 1 时表示这是一个命令（而不是纯控制操作），必须设置。48 位 LBA 的编码方式是把 48 位地址拆成低 24 位（lba0-lba2）和高 24 位（lba3-lba5）两部分。

### ATA 命令码

我们用到的两个核心命令：READ DMA EXT（0x25）和 WRITE DMA EXT（0x35）。"EXT" 表示使用 48 位 LBA，"DMA" 表示使用 DMA 传输（而不是 PIO）。这两个命令配合 LBA48 寻址模式可以访问超大容量磁盘（2^48 个扇区 = 128 PB）。

还有一个常用命令 IDENTIFY DEVICE（0xEC），用于读取设备的身份信息（容量、序列号、支持的特性等）。Cinux 在当前 tag 里没有使用它，但了解即可。

### PRDT 和数据缓冲区

PRDT（Physical Region Descriptor Table）实现了 scatter-gather 功能——一次 DMA 传输可以把数据分散到多个不连续的物理内存区域。每个 PRDT 条目描述一个连续的物理缓冲区，包括基地址（64 位）、字节计数（22 位，最大 4MB 减 1）、以及一个中断标志位。

对于简单的单扇区读写，只需要一个 PRDT 条目。字节计数字段是 (实际字节数 - 1)，所以读 1 个扇区（512 字节）时 dbc = 511。数据缓冲区必须物理连续，所以用 PMM 分配一整页来保证。

### MBR 引导签名

磁盘的第一个扇区（LBA 0）在 MBR 分区方案中有特殊意义：它的最后两个字节（偏移 510 和 511）必须是 0x55 和 0xAA，这是 BIOS 用来判断磁盘是否可引导的标志。QEMU 的测试磁盘镜像在创建时就被写入了这个签名，所以读取 LBA 0 并检查这两个字节是验证整个读写链路是否正常的绝佳方式。

## 动手实现

### Step 1: 实现命令 FIS 构造

**目标**: 封装一个函数，根据读写方向、LBA 地址和扇区数量，构造完整的 Register H2D FIS。

**设计思路**: 把命令表的 cfis 区域 reinterpret_cast 成 RegH2DFIS 结构体指针，然后逐字段填充。fis_type = 0x27（Reg H2D），flags = 0x80（command bit），command 根据读写方向选 READ_DMA_EXT(0x25) 或 WRITE_DMA_EXT(0x35)。48 位 LBA 拆成 6 个字节（lba0 到 lba5），device = 0x40（LBA 模式），扇区数量拆成 count0（低字节）和 count1（高字节）。

**实现约束**: 作为 AHCI 类的静态私有方法。FIS 中所有保留字段必须清零（调用前整个命令表已经被清零了）。feature 和 feature_exp 字段设为 0。

**踩坑预警**: LBA 的字节拆分顺序很容易搞混。lba0 是最低字节（bits 7:0），lba5 是最高字节（bits 47:40）。如果字节序搞反了，读到的扇区会完全不对。建议先用 LBA 0 测试，这样所有 LBA 字节都是 0，不会暴露字节序问题——等你验证基本链路通了再试其他 LBA。

**验证**: 构造 READ DMA EXT 命令后，检查 FIS 的 command 字节是 0x25，fis_type 是 0x27，device 是 0x40。

### Step 2: 实现命令执行函数

**目标**: 封装完整的命令下发流程：清零命令表、构造 FIS、设置 PRDT、配置 Command Header、触发命令、轮询等待完成。

**设计思路**: 函数接受端口号、slot 号、读写标志、LBA、扇区数、数据缓冲区物理地址。先清零命令表。调用 build_cfis 构造 FIS。设置 PRDT 条目 0 的地址（从 buf_phys 拆分低 32 和高 32 位）、字节计数（扇区数 * 512 - 1，截断到 22 位）、中断标志（i=1）。配置 Command Header 的 CFL（FIS 长度，以 DWORD 为单位，RegH2DFIS 是 20 字节 = 5 个 DWORD）、PRDTL（PRDT 条目数 = 1）、write 标志、CTBA（命令表物理地址）。清除端口中断状态。写 port.ci 的对应位触发命令。轮询等待 ci 位清零，检查 TFD 的错误位。

**实现约束**: 作为 AHCI 类的私有方法，返回 bool 表示成功或失败。TFD（Task File Data）的 bit 0 是 ERR 标志，非零表示命令出错。轮询超时设为 1 亿次循环。

**踩坑预警**: PRDT 的字节计数是 (实际字节数 - 1)，不是实际字节数。如果你写 512 而不是 511，HBA 可能只传输 511 字节或者直接报错。另外，命令表的物理地址必须正确——如果 CTBA 指向了错误的地址，HBA 会读到垃圾数据，轻则命令失败，重则 HBA 挂死。命令表放在命令列表页的后半部分（紧跟 32 个 Command Header 之后），这个偏移量计算一定要仔细。

**验证**: 执行一次 LBA 0 的读命令，返回 true 表示成功。如果返回 false，检查串口的 TFD 错误码和超时信息。

### Step 3: 实现 read/write 公开接口

**目标**: 封装面向用户的读写方法，接受端口号、LBA、扇区数、缓冲区物理地址。

**设计思路**: 先做参数校验（hba_mem_ 非空、端口号合法、该端口已初始化）。然后调用 execute_command，read 传 false 作为写标志，write 传 true。这是一个简单的包装层。

**实现约束**: 缓冲区地址是物理地址，调用者负责分配物理连续的内存。扇区数是 uint16_t（最大 65535）。

**踩坑预警**: 调用者必须确保缓冲区物理地址是有效的、物理连续的、已映射到虚拟空间的。如果传入一个未映射的物理地址，虽然 HBA 可以完成 DMA，但内核后续访问缓冲区时会 page fault。

**验证**: 调用 read(0, 0, 1, buf_phys) 读取端口 0 的 LBA 0，返回 true。

### Step 4: 在 kernel_main 中集成测试

**目标**: 在内核主函数中分配缓冲区、执行读操作、检查 MBR 签名。

**设计思路**: 在 PCI 初始化和 AHCI 初始化之后，用 PMM 分配一页物理内存作为读缓冲区。映射到内核虚拟空间并清零。调用 ahci.read(0, 0, 1, buf_phys) 读取扇区 0。通过虚拟地址访问缓冲区的第 510 和 511 字节，检查是否为 0x55 和 0xAA。打印结果。

**实现约束**: 缓冲区的虚拟地址需要选一个不和现有映射冲突的区域（比如 0xFFFF800000300000）。映射标志用 Present + Writable 即可（数据缓冲区不需要 PCD）。

**踩坑预警**: 传给 ahci.read 的必须是物理地址，不是虚拟地址。HBA 做 DMA 时只知道物理地址。同时，你自己访问数据要用虚拟地址。千万别搞混了。另外，确保 QEMU 的测试磁盘镜像在构建时已经创建（CMake 的 add_custom_command 会调用 create_ahci_test_disk.sh 脚本）。

**验证**: 串口输出 `[AHCI] Read sector 0: 55 AA`。如果输出的是 `00 00`，说明读到的数据全是零，可能磁盘镜像没正确创建、或者 DMA 传输到了错误的地址。

## 构建与运行

```bash
cd build && cmake .. && make -j$(nproc) && make run
```

测试磁盘镜像会在构建时自动创建。你也可以手动检查：

```bash
xxd -s 510 -l 2 build/ahci_test.img
# 应该输出：000001fe: 55aa
```

运行内核测试（自动化 QEMU 测试）：

```bash
cd build && make run-kernel-test
```

这会运行 test_ahci.cpp 中定义的三个内核态测试：PCI 找到 AHCI、BAR5 映射成功、读取 MBR 签名正确。

## 调试技巧

**问题：命令发出后一直超时**
排查：检查端口是否正确启动（ST 和 FRE 是否置位）。检查 Command Header 的 CFL 字段是否正确（RegH2DFIS 的长度除以 4 = 5）。检查 CTBA 指向的命令表是否在物理上可访问。用 QEMU monitor 的 `info ahci` 命令查看 HBA 状态。

**问题：MBR 签名读出来是 00 00**
排查：先用 `xxd` 检查磁盘镜像文件本身是否有签名。如果文件有但读出来没有，说明 DMA 传输到了错误的地方——检查传给 read() 的 buf_phys 是否就是你映射到虚拟空间的那个物理页。另一个可能：PRDT 的字节计数写错了（比如写了 0 而不是 511）。

**问题：读命令返回 true 但数据缓冲区全是 0**
排查：HBA 完成了命令但数据可能 DMA 到了别的地址。检查 PRDT 的 DBA 字段是不是正确的物理地址。确认缓冲区物理页确实是通过 PMM 分配的（不是某个已经被别人用过的地址）。

## 本章小结

| 概念 | 关键要点 |
|------|----------|
| Register H2D FIS | 类型 0x27，封装 ATA 命令、LBA、扇区数量 |
| READ DMA EXT | 命令码 0x25，48 位 LBA + DMA 传输 |
| WRITE DMA EXT | 命令码 0x35，同上但方向相反 |
| PRDT | 描述 DMA 缓冲区的物理地址和大小，dbc = 字节数 - 1 |
| 命令下发 | 配置 Command Header -> 清 IS -> 写 CI -> 轮询等待 |
| TFD 错误检查 | bit 0 = ERR，非零表示命令失败 |
| MBR 签名 | 偏移 510-511 处的 0x55 0xAA，验证读写链路的最佳手段 |
