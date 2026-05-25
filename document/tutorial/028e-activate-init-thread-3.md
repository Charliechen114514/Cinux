---
title: 028e-activate-init-thread-3 · Init 线程
---

# 当栈覆盖了 MMIO——内存布局冲突的排查实录

> 标签：MMIO, virtual address collision, memory layout, debugging, page table
> 前置：[028e-2 实现 kernel_init_thread](028e-activate-init-thread-2.md)

## 前言

上一篇的末尾我留了一个悬念——如果你按步骤实现了 init 线程，大概率会在串口输出中看到一个令人困惑的错误：`[AHCI] Port 1: command timeout`。AHCI 初始化阶段一切正常，Port 1 检测到了 SATA 设备，SSTS 寄存器读出 `0x113`（设备在线且活跃），但 init 线程尝试挂载 ext2 时，同一个端口的命令却超时了。更诡异的是，如果你在 init 线程中手动读一下 Port 1 的 SSTS，它变成了 `0x0`——设备凭空消失了。

说实话这个 bug 真的坑了我半天。一开始我以为是调度器抢占导致的——AHCI DMA 轮询被打断，浪费了超时时间。于是我在 ext2 挂载外加了一层 InterruptGuard 关中断，问题依旧。然后我开始怀疑是 AHCI 驱动的线程安全问题——但驱动里的轮询循环是纯本地的，不涉及任何共享状态。直到我灵机一动，在 init 线程中直接打印 Port 1 的 SSTS 寄存器值，看到了 `0x0`，才意识到真正的问题：不是 AHCI 出了问题，而是 MMIO 的页表映射被覆盖了。

## 环境说明

排查过程在 QEMU 中完成，使用 `-serial stdio` 捕获串口输出。问题涉及的文件包括 `kernel/arch/x86_64/memory_layout.hpp`（新建）、`kernel/drivers/ahci/ahci.cpp`、`kernel/proc/process.cpp`、`kernel/fs/ext2.cpp` 和 `kernel/main.cpp`。核心概念是 x86_64 的 4-level paging——MMIO 寄存器通过页表映射到虚拟地址空间，如果页表项被改写，MMIO 映射就失效了。

## 第一步——症状确认：SSTS 从 0x113 变成 0x0

排查的第一步是确认问题的边界。AHCI 初始化在 `kernel_main()` 中执行，此时串口输出显示：

```
[AHCI] Port 1: SSTS=0x113 DET=3 SIG=0xffffffff
```

`SSTS=0x113` 意味着设备在线且通信正常（DET=3 表示 device present and communicating）。但进入 init 线程后，同样是 Port 1 的 SSTS 寄存器：

```
[INIT] Port 1 SSTS=0x0 before mount
```

一个硬件寄存器的值从 `0x113` 变成了 `0x0`——这在物理上是不可能的，除非这个寄存器已经不是原来的那个寄存器了。而 MMIO 寄存器通过页表映射访问，如果页表项被改写了，虚拟地址就不再指向物理寄存器，而是指向了某个普通的物理页。读出全零，说明新映射的物理页恰好是被清零过的。

## 第二步——定位：谁覆盖了 MMIO 映射

知道了是页表映射被覆盖，下一步就是找到是谁干的。我们在 AHCI 驱动和进程管理器中找到了两个关键地址：

```cpp
// ahci.cpp
static constexpr uint64_t MMIO_VIRT_BASE = 0xFFFF800000100000ULL;

// process.cpp
std::atomic<uint64_t> next_stack_vaddr{0xFFFF800000100000ULL};
```

两个地址完全相同——`0xFFFF800000100000`。AHCI 驱动把 BAR5 映射到这个虚拟地址，进程管理器从这个虚拟地址开始分配内核栈。当 `Scheduler::init()` 创建 idle task 时，`TaskBuilder::build()` 为它分配 4 页内核栈，调用 `g_vmm.map()` 把物理页映射到 `0xFFFF800000100000` 起始的虚拟地址——直接覆盖了 AHCI BAR5 的 MMIO 页表项。

然后创建 kernel_init 线程，栈映射到 `0xFFFF800000104000`——覆盖了 AHCI 命令列表的映射。再创建 boot task，栈映射到 `0xFFFF800000108000`——覆盖了 FIS 接收缓冲区的映射。三层覆盖，AHCI 彻底失联。

这个 bug 之前不暴露的原因也很清楚了：重构前 `Scheduler::init()` 在 `run_concurrent_stress()` 中调用，此时 AHCI init 和 ext2 mount 已经在 `kernel_main()` 中完成了。没有内核栈映射，MMIO 区域完好无损。重构后，启动顺序变成了 AHCI init → Scheduler::init() → 创建 Task，栈映射在 MMIO 映射之后才发生——冲突暴露。

## 第三步——修复：统一的内存布局头文件

修复方案是创建 `kernel/arch/x86_64/memory_layout.hpp`，集中定义所有内核虚拟地址区域。每个区域用一组 (base, size) 常量描述，区域按顺序排列，后续区域的 base 等于前一个区域的 base + size。

```cpp
namespace cinux::arch {

constexpr uint64_t KMEM_BASE = 0xFFFF800000000000ULL;

// Heap
constexpr uint64_t KMEM_HEAP_SIZE = 0x100000ULL;    // 1 MB
constexpr uint64_t KMEM_HEAP_BASE = KMEM_BASE;

// MMIO
constexpr uint64_t KMEM_MMIO_SIZE = 0x40000ULL;     // 256 KB
constexpr uint64_t KMEM_MMIO_BASE = KMEM_HEAP_BASE + KMEM_HEAP_SIZE;

// Stack (follows MMIO — key fix: stacks come AFTER MMIO)
constexpr uint64_t KMEM_STACK_BASE = KMEM_MMIO_BASE + KMEM_MMIO_SIZE;

// DMA
constexpr uint64_t KMEM_DMA_SIZE   = 0x100000ULL;    // 1 MB
constexpr uint64_t KMEM_DMA_BASE   = KMEM_STACK_BASE + 0x100000ULL;

// ext2 DMA
constexpr uint64_t KMEM_EXT2_DMA_SIZE = 0x100000ULL; // 1 MB
constexpr uint64_t KMEM_EXT2_DMA_BASE = KMEM_DMA_BASE + KMEM_DMA_SIZE;

}  // namespace cinux::arch
```

修复的核心在 `KMEM_STACK_BASE = KMEM_MMIO_BASE + KMEM_MMIO_SIZE`——栈从 MMIO 之后开始。在旧代码中，栈的起始地址和 MMIO 相同（都是 `0xFFFF800000100000`）；现在栈在 MMIO 之后 256 KB 的位置，永远不会重叠。

各个消费者文件的修改很直接——把硬编码的魔法地址替换为布局常量：

```cpp
// ahci.cpp
static constexpr uint64_t MMIO_VIRT_BASE = cinux::arch::KMEM_MMIO_BASE;

// process.cpp
std::atomic<uint64_t> next_stack_vaddr{cinux::arch::KMEM_STACK_BASE};

// ext2.cpp
static constexpr uint64_t EXT2_DMA_VIRT_BASE = cinux::arch::KMEM_EXT2_DMA_BASE;

// main.cpp
constexpr uint64_t HEAP_VIRT_BASE = cinux::arch::KMEM_HEAP_BASE;
constexpr uint64_t buf_virt = cinux::arch::KMEM_DMA_BASE;
```

每一行替换都不改变运行时行为（地址值是相同的），但它们消除了魔法数字，让地址的来源可追溯。更重要的是，将来如果需要新增一个区域，只需要在布局头文件中插入一行并调整后续基址就行——不需要在各个驱动文件中搜索和修改。

## 与其他 OS 的对比

### Linux 的内核虚拟地址布局

Linux 使用编译期宏定义来管理内核虚拟地址布局。在 x86_64 上，`PAGE_OFFSET`（通常是 `0xffff888000000000`）是直接映射区的起始，`VMALLOC_START` 是 vmalloc 区的起始，`VMEMMAP_START` 是 struct page 数组的起始。这些宏定义在 `include/asm/pgtable_64_types.h` 中，和我们 `memory_layout.hpp` 的设计完全一致——集中定义，各模块引用。

Linux 还有一个 Cinux 没有的机制：`vmalloc()` 运行时虚拟地址分配器。它从 `VMALLOC_START` 到 `VMALLOC_END` 的范围内动态分配虚拟地址区域，用于大块内存映射（比如模块加载、大缓冲区）。Cinux 目前不需要这种动态分配——我们的所有区域大小在编译期就确定了。但如果将来内核变得更大（比如支持动态设备驱动加载），添加一个类似的运行时分配器是合理的。

### SerenityOS 的 MemoryLayout

SerenityOS 有一个专门的 `MemoryLayout` 类，定义在 `Kernel/MemoryLayout.h` 中。它比 Cinux 的 `memory_layout.hpp` 更完善——不仅定义了各区域的基址和大小，还提供了运行时查询接口（比如 `is_user_address(vaddr)` 判断地址是否属于用户空间）。这种设计在安全审计中非常有用——当内核访问一个虚拟地址时，可以快速检查它是否在合法的区域内。

Cinux 目前使用 `constexpr` 常量就足够了，不需要运行时查询。但如果将来需要添加地址合法性检查（比如在页表操作中验证虚拟地址范围），可以考虑升级为类似的 MemoryLayout 类。

## 收尾

构建并运行修复后的内核，你应该能看到完整的、无错误的启动序列——AHCI init 正常、调度器启动、init 线程挂载 ext2、VFS 注册、shell 启动。从 `[AHCI] Port 1: SSTS=0x113` 到 `shell> _`，整个链条没有任何超时或崩溃。

这个 bug 给我们的教训非常深刻：在内核开发中，散落在各处的魔法地址数字就是定时炸弹。你永远不知道什么时候两个子系统会碰巧选了同一个虚拟地址。`memory_layout.hpp` 的引入不仅修复了当前的冲突，更重要的是建立了一个架构规范——所有内核虚拟地址区域在一个地方定义，任何新增区域都必须通过这个头文件注册。

到这里，028e tag 的所有工作就完成了。我们的内核现在有了正规的 init 线程、干净的 shell_task 修复、统一的内存布局管理。从 `kernel_main()` 的三段式架构到 ext2 挂载到 shell 启动，每一步都在调度器的管理下进行，没有 hack，没有伪造，一切都干干净净。

## 参考资料

- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) — 虚拟地址空间布局设计指南
- OSDev Wiki: [Memory Mapped Registers in C/C++](https://wiki.osdev.org/Memory_mapped_registers_in_C/C%2B%2B) — MMIO 映射注意事项（cache disable、地址预留）
- OSDev Wiki: [Memory Map (x86)](https://wiki.osdev.org/Memory_Map_(x86)) — x86 物理内存布局
- Intel SDM: Vol.3A Section 4.5 — 4-Level Paging, canonical address space
- Linux: `arch/x86/include/asm/pgtable_64_types.h` — 内核虚拟地址布局宏定义
