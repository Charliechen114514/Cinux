# memory_layout.hpp 与地址冲突修复

> 标签：memory layout, MMIO, virtual address collision, page table, debugging
> 前置：[028e-2 init.cpp 与 usermode.cpp](028e-activate-init-thread-2.md)

## 概览

本文是 028e Read-through 系列的最后一篇，聚焦于这个 tag 中最「惊心动魄」的部分——MMIO 地址冲突的排查与修复。我们将完整拆解 `kernel/arch/x86_64/memory_layout.hpp` 的设计，看看一个 35 行的头文件是如何解决一个让 AHCI 设备凭空「消失」的诡异 bug 的。同时我们会走一遍所有消费者文件（ahci.cpp、process.cpp、ext2.cpp、main.cpp）的修改，理解魔法地址是如何被统一管理的。

关键设计决策一览：
- 集中管理所有内核虚拟地址区域的 (base, size) 常量
- 链式布局：每个区域的 base = 前一个区域 base + size
- MMIO 区域必须排在 Stack 区域之前，避免栈映射覆盖 MMIO
- 各消费者文件通过 `cinux::arch::KMEM_*` 常量引用地址

## 内存布局图

```
虚拟地址                        区域            大小
─────────────────────────────────────────────────────────────
0xFFFF8000_00000000 ─┐
                     │ KMEM_HEAP     1 MB
0xFFFF8000_00100000 ─┘
                     │ KMEM_MMIO     256 KB
0xFFFF8000_00140000 ─┘
                     │ KMEM_STACK    (按需增长，~1 MB 预留)
0xFFFF8000_00240000 ─┘
                     │ KMEM_DMA      1 MB
0xFFFF8000_00340000 ─┘
                     │ KMEM_EXT2_DMA 1 MB
0xFFFF8000_00440000 ─┘

... 0xFFFFFFFF_80000000 = Kernel code + data (higher-half direct map)
```

注意：当前 main 分支的 heap 已扩展到 128 MB，并新增了 Framebuffer 区域，但 028e tag 的初始版本如上所示。架构设计不变。

## 代码精讲

### memory_layout.hpp——完整实现

这个头文件只有 35 行（diff 版本），但它的设计简洁而有效。

```cpp
#pragma once

#include <stdint.h>

namespace cinux::arch {

// ============================================================
// Kernel virtual memory layout (0xFFFF8000_00000000+)
// ============================================================
// Regions are defined as (base, size) pairs.  Each subsequent
// region starts at the previous region's base + size.
// To add a new region, insert it here and bump the ones below.

constexpr uint64_t KMEM_BASE = 0xFFFF800000000000ULL;
```

注释清楚地说明了设计原则：所有区域定义为一组 (base, size) 对，每个后续区域的基址等于前一个区域的基址加大小。新增区域只需要插入一行并调整后续基址。`KMEM_BASE` 是整个内核虚拟内存区域的起始地址——`0xFFFF800000000000` 是 x86_64 canonical address space 中高半部分（sign-extended from bit 47）的第一个可用地址。

然后是各区域的定义。以 diff 版本为例（当前代码略有扩展但结构相同）：

```cpp
// Heap: kernel heap allocator
constexpr uint64_t KMEM_HEAP_SIZE  = 0x100000ULL;       // 1 MB
constexpr uint64_t KMEM_HEAP_BASE  = KMEM_BASE;

// MMIO: memory-mapped I/O (AHCI BAR5, etc.)
constexpr uint64_t KMEM_MMIO_SIZE  = 0x40000ULL;       // 256 KB
constexpr uint64_t KMEM_MMIO_BASE  = KMEM_HEAP_BASE + KMEM_HEAP_SIZE;

// Stacks: per-task kernel stacks (allocated upward)
constexpr uint64_t KMEM_STACK_BASE = KMEM_MMIO_BASE + KMEM_MMIO_SIZE;
```

注意 Stack 区域只有 BASE 没有 SIZE——因为栈是按需增长的，通过 `alloc_stack_vaddr()` 的 `fetch_add` 逐个分配。它紧跟 MMIO 区域之后，这是解决地址冲突的关键：在旧代码中，栈的起始地址和 MMIO 的起始地址相同（都是 `0xFFFF800000100000`），现在栈从 MMIO 区域之后开始，互不重叠。

```cpp
// DMA: ad-hoc DMA buffers (sector reads, etc.)
constexpr uint64_t KMEM_DMA_SIZE   = 0x100000ULL;       // 1 MB
constexpr uint64_t KMEM_DMA_BASE   = KMEM_STACK_BASE + 0x100000ULL;

// ext2 DMA: ext2 filesystem block cache / DMA buffers
constexpr uint64_t KMEM_EXT2_DMA_SIZE = 0x100000ULL;    // 1 MB
constexpr uint64_t KMEM_EXT2_DMA_BASE = KMEM_DMA_BASE + KMEM_DMA_SIZE;
```

DMA 和 ext2 DMA 各 1 MB，用于 AHCI 读写操作的缓冲区和 ext2 文件系统的块缓存。它们之间通过硬编码的偏移量（`0x100000`）隔开——这个偏移量实际上是为 Stack 区域预留的空间（1 MB 大约可以容纳 64 个 Task 各 4 页内核栈）。

### ahci.cpp——MMIO 虚拟基址的统一引用

```cpp
// 原来：
// static constexpr uint64_t MMIO_VIRT_BASE = 0xFFFF800000100000ULL;

// 现在：
static constexpr uint64_t MMIO_VIRT_BASE = cinux::arch::KMEM_MMIO_BASE;
```

这是一行简单但意义重大的替换。原来的硬编码地址 `0xFFFF800000100000` 恰好和 `process.cpp` 中内核栈的起始地址完全相同——这就是冲突的根源。替换为 `KMEM_MMIO_BASE` 后，MMIO 基址由布局头文件统一管理，保证了它和 Stack 区域不会重叠。

AHCI 驱动的其他部分不需要修改——`MMIO_VIRT_BASE` 只在 `map_bar5()`、`setup_port()` 等内部函数中使用，用于计算 BAR5 映射、命令列表和 FIS 缓冲区的虚拟地址。引用方式从硬编码变成了布局常量，运行时行为完全不变。

### process.cpp——内核栈分配器的统一引用

```cpp
// 原来：
// std::atomic<uint64_t> next_stack_vaddr{0xFFFF800000100000ULL};

// 现在：
std::atomic<uint64_t> next_stack_vaddr{cinux::arch::KMEM_STACK_BASE};
```

`next_stack_vaddr` 是一个原子变量，记录下一个可用的内核栈虚拟地址。每次 `TaskBuilder::build()` 调用 `alloc_stack_vaddr()` 时，它通过 `fetch_add` 原子地分配 `(STACK_PAGES + 1) * PAGE_SIZE` 字节的虚拟地址空间（多出的一页是 guard page）。

原来初始值是 `0xFFFF800000100000`，和 AHCI MMIO 基址相同。替换为 `KMEM_STACK_BASE` 后，栈从 MMIO 区域之后开始分配——这就是修复地址冲突的直接原因。

### ext2.cpp——DMA 缓冲区的统一引用

```cpp
// 原来：
// static constexpr uint64_t EXT2_DMA_VIRT_BASE = 0xFFFF800000400000ULL;

// 现在：
static constexpr uint64_t EXT2_DMA_VIRT_BASE = cinux::arch::KMEM_EXT2_DMA_BASE;
```

ext2 文件系统驱动使用独立的虚拟地址区域作为 DMA 缓冲区——用于从磁盘读取的数据暂存。原来的硬编码地址虽然不和其他区域冲突（在旧布局中它排在 MMIO 之后很远），但替换为布局常量后，地址的可追溯性和一致性都得到了保证。

### main.cpp——Heap 和 DMA 的统一引用

```cpp
// 原来：
// constexpr uint64_t HEAP_VIRT_BASE = 0xFFFF800000000000ULL;
constexpr uint64_t HEAP_VIRT_BASE = cinux::arch::KMEM_HEAP_BASE;

// 原来：
// constexpr uint64_t buf_virt = 0xFFFF800000300000ULL;
constexpr uint64_t buf_virt = cinux::arch::KMEM_DMA_BASE;
```

`kernel_main()` 中有两处地址引用被替换。Heap 基址是内核堆分配器的起始虚拟地址，DMA 基址用于 AHCI MBR 读取的临时缓冲区。两处替换都不改变运行时行为，只是让地址来源可追溯。

## 设计决策

### 决策：集中式布局头文件 vs 各模块自行管理

**问题**: 内核虚拟地址区域应该如何管理？

**本项目的做法**: 创建 `memory_layout.hpp`，集中定义所有区域的 (base, size) 常量，各模块通过命名空间常量引用。

**备选方案**: 使用一个运行时的虚拟地址分配器（类似 `vmalloc`），动态分配虚拟地址区域。

**为什么不选备选方案**: 运行时分配器增加了复杂度和潜在的运行时错误（分配失败、碎片化）。对于我们的内核来说，各区域的数量和大小在编译期就是已知的——MMIO 需要 2 MB，栈从某个基址开始增长，DMA 需要 1 MB。编译期常量更简单、更可靠、更容易审计。Linux 也在早期启动阶段使用类似的编译期布局定义（`KERNEL_DS`、`PAGE_OFFSET` 等），运行时分配器只在后期用于动态映射。

**如果要扩展**: 当前实现的一个限制是 Stack 区域没有显式的 size 边界检查。如果创建的 Task 太多，栈区域可能溢出到 DMA 区域。改进方案是给 Stack 区域添加一个 size 上限，在 `alloc_stack_vaddr()` 中检查溢出。

## 扩展方向

- 添加编译期静态断言，检查各区域没有重叠
- 给 Stack 区域添加运行时溢出检测，超过预留空间时打印警告
- 支持动态 MMIO 映射——在 PCI 枚举发现新设备时，自动从 MMIO 区域分配虚拟地址
- 添加一个 `kmem_layout_dump()` 调试函数，打印当前所有区域的地址范围和使用情况
- 考虑使用 2 MB 大页映射 Framebuffer 区域，减少 TLB 压力

## 参考资料

- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 内核虚拟地址空间布局设计
- OSDev Wiki: [Memory Mapped Registers in C/C++](https://wiki.osdev.org/Memory_mapped_registers_in_C/C%2B%2B) — MMIO 映射注意事项（cache disable、虚拟地址预留）
- Intel SDM: Vol.3A Section 4.5 — 4-Level Paging, canonical address space (0xFFFF8000_00000000+)
- Linux: `include/asm-generic/pgtable.h` — 内核虚拟地址区域宏定义
