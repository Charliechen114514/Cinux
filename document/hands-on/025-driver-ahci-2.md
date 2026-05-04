# 025-2 动手篇：AHCI HBA 初始化与端口配置

## 导语

上一章我们成功通过 PCI 枚举找到了 AHCI 控制器，读出了 BAR5 的物理地址。但那个物理地址本身还不能直接用——我们需要把它映射到内核的虚拟地址空间，然后按 AHCI 规范的步骤初始化 HBA（Host Bus Adapter），配置每个活跃端口的命令列表和 FIS 缓冲区。

完成本章后，串口输出会显示类似 `[AHCI] Port 0 set up: cmdlist=0x... fis=0x...` 和 `[AHCI] 1 active ports initialised.`，意味着 AHCI 控制器已经就绪，可以接受读写命令了。

前置知识：上一章的 PCI 枚举结果（PCIDevice 结构体，特别是 BAR5 地址）；内核的 PMM（物理内存分配）和 VMM（虚拟内存映射）接口。

## 概念精讲

### AHCI 的整体架构

AHCI 把自己抽象成一个"数据搬运引擎"——它站在系统内存和 SATA 设备之间，驱动程序只需要在内存里摆好数据结构（命令列表、命令表、FIS 缓冲区），告诉 HBA "去干活"，HBA 就会自动完成 DMA 传输。驱动不需要像 IDE PIO 那样一个字节一个字节地搬运数据。

AHCI 的 MMIO 寄存器区域（通过 BAR5 访问）分为两部分：通用主机控制寄存器（offset 0x00-0xFF）和端口寄存器（从 offset 0x100 开始，每个端口占 0x80 字节）。通用区域里的关键寄存器有 CAP（能力）、GHC（全局控制）、PI（端口实现位图）、VS（版本号）。每个端口有自己的一套 CLB/FB/CMD/CI 等寄存器。

### GHC 寄存器和 HBA 复位

GHC（Global HBA Control）是整个控制器的总开关。它有几个关键位：AE（AHCI Enable，bit 31）置位后 HBA 工作在 AHCI 模式（否则是 IDE 兼容模式）；HR（HBA Reset，bit 0）写 1 触发复位，HBA 完成复位后自动清零这位；IE（Interrupt Enable，bit 1）控制全局中断使能。

初始化流程通常是：先使能 AHCI 模式，然后复位 HBA，复位完成后重新使能 AHCI 模式（因为复位会清除 AE 位），最后使能中断。每一步之间可能需要轮询等待。

### PI 位图和端口探测

PI（Port Implemented）是一个 32 位寄存器，每一位对应一个端口。如果 bit N 为 1，说明端口 N 被实现了（但不一定有设备连接）。要确认是否有设备，还需要看端口的 SSTS（SATA Status）寄存器的 DET 字段——DET == 0x03 表示设备存在且通信已建立。

### 命令列表和 FIS 缓冲区

每个端口需要两个关键的内存结构：

命令列表（Command List）是一个数组，最多 32 项，每项 32 字节（叫 Command Header）。每个 Command Header 描述一个命令，包括 FIS 长度、PRDT 数量、读写方向、以及命令表（Command Table）的物理地址。命令列表必须 1K 字节对齐。

FIS 接收缓冲区（FIS Receive Buffer）是 256 字节，用于接收设备发回来的 FIS（比如 Device to Host Register FIS）。必须 256 字节对齐。

命令表（Command Table）是每个命令槽位独立的，包含命令 FIS（最多 64 字节）、ATAPI 命令缓冲区（16 字节），以及 PRDT（Physical Region Descriptor Table）数组。PRDT 告诉 HBA 数据应该 DMA 到哪个物理地址、传输多少字节。

### 端口引擎的启停

在修改端口的 CLB/FB 寄存器之前，必须先停止端口的命令引擎。停止步骤是：先清除 ST（Start）位，等待 CR（Command Running）位清零；然后清除 FRE（FIS Receive Enable）位，等待 FR（FIS Receive Running）位清零。配置完成后，先设置 FRE，再设置 ST 来启动引擎。顺序不能反——FRE 必须在 ST 之前使能。

## 动手实现

### Step 1: 定义 AHCI 寄存器结构和常量

**目标**: 把 AHCI 规范里所有的寄存器结构、位域定义、常量都集中到一个配置头文件里。

**设计思路**: 按照规范精确还原 HBAMem（HBA 控制器寄存器）、HBAPort（端口寄存器）、HBACommandHeader（命令头）、HBACommandTable（命令表）、HBAPrdtEntry（PRDT 条目）、RegH2DFIS（Register Host-to-Device FIS）这几个结构体。所有结构体用 `[[gnu::packed]]` 修饰，端口寄存器字段用 volatile。同时用命名空间组织各种位常量：GhcBits、PxCmd、PxIs、PxSsts、AtaCmd、FisType 等。

**实现约束**: 文件名 ahci_config.hpp，放在 kernel/drivers/ahci/ 目录下。用 static_assert 验证每个结构体的大小（HBAPort 必须 0x80 字节，HBAMem 的通用区域部分必须 0x100 字节，HBACommandHeader 必须 32 字节等）。端口数组在 HBAMem 中声明为 ports[1]（变长数组技巧，实际访问时用指针偏移）。

**踩坑预警**: HBAPort 结构体的 volatile 限定符不能遗漏——这些是硬件寄存器，编译器优化可能导致读不到最新的值。另外，HBAMem 的 reserved 区域大小必须精确计算，确保端口数组从 offset 0x100 开始。如果你算错了哪怕一个字节，后面所有端口寄存器的偏移都会错位。

**验证**: 编译时 static_assert 全部通过，说明结构体布局正确。

### Step 2: 实现 BAR5 MMIO 映射

**目标**: 把 BAR5 的物理地址映射到内核虚拟地址空间，返回一个可以访问 HBAMem 结构体的指针。

**设计思路**: 在内核的高位虚拟地址区域（0xFFFF800000100000 附近）预留一块空间给 MMIO。映射 2 个 4KB 页（覆盖最多 8 个端口），映射标志位包括 Present、Writable 和 PCD（Page Cache Disable）——MMIO 必须是非缓存的，因为 CPU 缓存和硬件寄存器的语义完全不同。

**实现约束**: 使用 VMM::map() 逐页映射。映射成功后把虚拟地址 reinterpret_cast 成 HBAMem*。如果映射失败返回 nullptr。

**踩坑预警**: PCD 标志位绝对不能忘。如果映射成普通缓存内存，CPU 可能会缓存寄存器的值，导致你读到的不是硬件最新写入的数据。这种 bug 在 QEMU 上可能不显现（QEMU 的内存模拟比较宽松），但在真机上一定会炸。另外，确保映射的虚拟地址范围不和其他 MMIO 区域重叠。

**验证**: 映射成功后读取 hba_mem->pi、hba_mem->cap、hba_mem->vs 寄存器，打印出来。PI 应该是一个非零的位图（至少 bit 0 置位），CAP 和 VER 也应该有合理值。

### Step 3: 实现 HBA 复位和 AHCI 模式使能

**目标**: 按规范顺序复位 HBA 控制器，然后重新使能 AHCI 模式和全局中断。

**设计思路**: 先设置 GHC.AE 位使能 AHCI 模式。然后设置 GHC.HR 位触发复位，轮询等待 HR 位自动清零（最多等约 1 秒）。复位完成后重新设置 GHC.AE（复位会清除此位）。最后设置 GHC.IE 使能全局中断。

**实现约束**: 轮询循环中插入 `pause` 指令降低功耗。超时上限设为 1 亿次循环（在 ~1 GHz 频率下约 1 秒）。超时后打印错误信息但不 panic（优雅降级）。

**踩坑预警**: 复位后必须重新使能 AE。如果忘了这一步，HBA 会留在 IDE 兼容模式，后面所有的端口操作都不会按 AHCI 规范行为。这个坑非常隐蔽，因为有些 HBA 在复位后 AE 默认就是 1，有些不是——QEMU 的行为和真机可能不同。

**验证**: 复位完成后打印 `[AHCI] HBA reset complete.`。如果看到 `[AHCI] HBA reset timeout!`，说明 HBA 没有正常响应复位，可能需要检查 BAR5 映射是否正确。

### Step 4: 实现端口启停和配置

**目标**: 对每个活跃端口（PI 位图中置位且 SSTS.DET == 0x03 的端口），停止引擎、分配命令列表和 FIS 缓冲区、写入端口寄存器、启动引擎。

**设计思路**: 先实现 stop_port：清除 ST，轮询等待 CR 清零；清除 FRE，轮询等待 FR 清零。然后实现 start_port：设置 FRE，再设置 ST。

端口的 setup 流程：调用 stop_port 停止引擎。用 PMM 分配一页物理内存作为命令列表（4KB 页远超 1KB 的需求，同时满足 1K 对齐）。用 PMM 再分配一页作为 FIS 缓冲区。把这两页都清零。把命令列表页和 FIS 缓冲区页映射到内核虚拟地址空间以便访问。写入端口的 CLB/CLBU/FB/FBU 寄存器（物理地址）。清除端口中断状态（IS 写 0xFFFFFFFF），设置中断使能位。在命令列表的第一个条目（slot 0）里设置命令表的物理地址（放在命令列表页的后半部分，紧跟 32 个 Command Header 之后）。最后调用 start_port 启动引擎。

**实现约束**: 每个端口独立的物理内存分配。命令列表和 FIS 缓冲区的虚拟地址映射区域要互相分开，不能重叠。使用偏移量 0x10000 和 0x20000 来区分不同端口的映射区域。端口的物理地址信息保存在类的成员数组 cmd_list_phys_[32] 和 fis_buf_phys_[32] 中。

**踩坑预警**: 清零命令列表和 FIS 缓冲区时，要使用物理地址直接映射（加 0xFFFFFFFF80000000 偏移）来访问，因为此时 VMM 映射可能还没建立。另外，在写 CLB/FB 之前一定要确保引擎已经完全停止（CR 和 FR 都清零了），否则行为未定义。这真的是一个会让人血压拉满的地方——你明明按照步骤做了，但忘了一个等待，整个端口就不响应了。

**验证**: 串口输出 `[AHCI] Port 0: SSTS=0x... DET=3 SIG=0x...`（DET=3 表示设备存在），然后 `[AHCI] Port 0 set up: cmdlist=0x... fis=0x...`。最后打印 `[AHCI] 1 active ports initialised.`。

## 构建与运行

```bash
cd build && cmake .. && make -j$(nproc) && make run
```

确保 QEMU 参数中包含 AHCI 设备。本 tag 的 CMake 已经配置好了，`make run` 会自动创建测试磁盘并挂载 AHCI 设备。

## 调试技巧

**问题：BAR5 映射后读取的值全是 0 或 0xFFFFFFFF**
排查：检查 BAR5 物理地址是否正确（从 PCI 配置空间读出来的值）。确认 VMM::map 调用成功。尝试用 QEMU monitor 的 `xp` 命令直接读取物理地址来验证硬件端是否正常。

**问题：HBA 复位超时**
排查：确认 BAR5 映射成功后再调用 reset_hba。检查 GHC 寄存器是否可写——有些 HBA 在 BIOS/OS 握手完成前不允许软件控制。如果 BOHC 寄存器相关位置位，可能需要先做 BIOS/OS 握手。

**问题：端口 SSTS.DET 始终不是 3**
排查：QEMU 的 AHCI 设备需要在启动时指定 `-device ide-hd,drive=ahci-disk,bus=ahci.0` 才会在端口 0 上出现设备。没有挂磁盘的端口 DET 当然不是 3。检查 QEMU 参数。

## 本章小结

| 概念 | 关键要点 |
|------|----------|
| BAR5 MMIO 映射 | 物理地址映射到虚拟空间，必须设置 PCD（非缓存）标志 |
| HBA 复位 | GHC.HR 置 1 触发，轮询等待自动清零 |
| AHCI 模式 | GHC.AE (bit 31)，复位后必须重新使能 |
| PI 位图 | bit N = 1 表示端口 N 被实现 |
| SSTS.DET | 0x03 = 设备存在且通信活跃 |
| 命令列表 | 32 x 32B = 1KB，1K 对齐，物理连续 |
| FIS 缓冲区 | 256B，256B 对齐，物理连续 |
| 端口启停 | 先停（清 ST→等 CR→清 FRE→等 FR），后启（设 FRE→设 ST） |
