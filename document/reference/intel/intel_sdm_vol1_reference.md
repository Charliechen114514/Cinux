# Intel SDM — 官方手册索引与参考

> Fetched: 2026-04-27
> Source URL: https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
> 备注: 以下内容来自 Intel 官网实际抓取

## 手册结构（从 Intel 官网获取）

Intel SDM 提供三种下载格式（内容完全相同）：
- **Combined Volume Set**: 合并为一个 PDF
- **Four-Volume Set**: 分 4 卷
- **Ten-Volume Set**: 分 10 卷（适合慢速网络）

当前版本: **091**

### Volume 1: Basic Architecture
> 描述支持 IA-32 和 Intel 64 架构的处理器的架构和编程环境。

**与 Cinux tag 000 相关的内容：**
- Chapter 3: Basic Execution Environment — 描述寄存器组、内存组织、实模式操作
- 实模式（Real-Address Mode）是 CPU 上电后的初始状态：16 位，1MB 寻址空间
- MBR 代码运行在实模式下，BIOS 将第一个 512 字节扇区加载到物理地址 0x7C00

### Volume 2 (2A/2B/2C/2D): Instruction Set Reference, A-Z
> 完整的指令集参考。

### Volume 3 (3A/3B/3C/3D): System Programming Guide
> 操作系统级的编程环境，包括内存管理、保护、任务管理、中断和异常处理等。

**后续 tag 相关内容：**
- Volume 3A Chapter 3: Protected-Mode Memory Management — GDT/IDT（tag 002-006）
- Volume 3A Chapter 4: Paging — 页表（tag 003, 012）
- Volume 3A Chapter 6: Interrupt and Exception Handling — 中断处理（tag 006-007）

### Volume 4: Model-Specific Registers (MSRs)
> 描述 MSR 寄存器。

## 下载方式

从 Intel 官网下载：
- https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html

也可购买纸质版: http://www.lulu.com/spotlight/IntelSDM

## 补充参考

Intel 还提供 GitHub 仓库：
- https://github.com/intel/SDM-Processor-Topology-Enumeration — 处理器拓扑枚举代码示例
- https://intelxed.github.io — X86 Encoder Decoder (XED) 库
