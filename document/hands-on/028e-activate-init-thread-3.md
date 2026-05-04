# MMIO 地址冲突排查与内存布局规范

> 标签：memory layout, MMIO, virtual address collision, debug, page table
> 前置：[028e-2 init 线程实现与用户态启动](028e-activate-init-thread-2.md)

## 导语

如果你按前两章的步骤实现了 init 线程并启动了 QEMU，大概率会在串口输出中看到一个令人困惑的现象：AHCI 初始化阶段一切正常，Port 1 检测到了 SATA 设备（`SSTS=0x113`），但 init 线程尝试挂载 ext2 时，却报出了 `Port 1: command timeout`。更诡异的是，如果你在 init 线程中手动读一下 Port 1 的 SSTS 寄存器，会发现它变成了 `0x0`——设备凭空「消失」了。

这一章我们要完整拆解这个 bug 的排查过程，从现象到假设到验证，最终找到根因：内核栈的虚拟地址和 AHCI MMIO 的虚拟地址完全重合，导致创建调度器任务时，栈映射覆盖了 MMIO 页表项。修复方案是创建一个统一的内存布局头文件 `memory_layout.hpp`，集中管理所有内核虚拟地址区域。这个经历告诉我们一个非常重要的教训：在内核开发中，散落在各处的魔法地址数字就是定时炸弹，迟早会炸。

## 概念精讲

### MMIO 与页表映射的关系

AHCI 控制器的寄存器不是通过 I/O 端口访问的，而是通过 Memory-Mapped I/O——PCI 配置空间中的 BAR5 寄存器保存了 MMIO 区域的物理基地址，内核需要把这个物理地址映射到虚拟地址空间中才能访问。在 Cinux 中，`AHCI::map_bar5()` 函数负责把 BAR5 的物理页映射到 `MMIO_VIRT_BASE` 开始的虚拟地址。

关键的理解是：MMIO 映射是通过页表来实现的。当你往 VMM 的 map 函数传入一个虚拟地址和一个物理地址时，VMM 会在当前页表的 PML4 → PDPT → PD → PT 四级结构中创建对应的页表项。如果你后来又往同一个虚拟地址映射了不同的物理页，那原来的映射就被覆盖了——页表项被改写，指向了新的物理页。这就是 MMIO「消失」的原因。

### 为什么重构前不会触发这个 Bug

在重构前，`Scheduler::init()` 和 Task 创建都在 `run_concurrent_stress()` 内部，而这个函数是在 AHCI 初始化和 ext2 挂载之后才被调用的。也就是说，AHCI MMIO 映射完成时，还没有任何内核栈被映射到虚拟地址空间中。MMIO 区域完好无损。

重构后，启动顺序变成了：AHCI 初始化 → `Scheduler::init()` → 创建 idle/kernel_init/boot 三个 Task → `run_first()` 启动调度。当 `TaskBuilder::build()` 为这三个 Task 分配内核栈时，它调用 `alloc_stack_vaddr()` 获取虚拟地址——而这个函数的起始地址恰好和 AHCI MMIO 的虚拟基地址一模一样。第一个 Task 的栈映射就覆盖了 BAR5 的页表项，AHCI 寄存器从此消失。

### 虚拟地址区域管理的最佳实践

OSDev Wiki 的 Higher Half Kernel 页面给出了明确的建议：内核的虚拟地址空间应该划分为明确的、互不重叠的区域，每个区域的 (base, size) 在一个集中的地方定义。这和 Linux 的 `vmalloc` 区域、SerenityOS 的 `MemoryLayout` 是同一思路——你需要在头文件里定义所有区域的边界，然后在各个驱动和子系统中引用这些常量。

这样做的好处是显而易见的：第一，当你需要新增一个区域时（比如添加一个新的 DMA 缓冲区），只需要在布局头文件中插入一行定义并调整后续区域的基址就行，不需要去各个驱动文件中搜索和修改魔法数字；第二，地址冲突在编译期就能发现——如果你定义的两个区域基址和大小有重叠，注释中就能看出来；第三，代码审查时，任何人都可以一眼看出整个内核的虚拟地址布局。

## 动手实现

### Step 1: 复现问题

**目标**: 在修复前，先理解如何确认 MMIO 地址冲突。

**设计思路**: 在 init 线程中，ext2 挂载之前，直接读取 Port 1 的 SSTS 寄存器。如果 AHCI 初始化阶段看到的是 `SSTS=0x113`，但 init 线程中读到的是 `0x0`，就说明 MMIO 映射被覆盖了。

具体操作上，你需要获取 HBAMem 指针（通过 AHCI 实例的 `hba_mem()` 方法），然后访问对应端口的 `ssts` 字段。如果这个值从非零变成了零，映射一定被覆盖了。你还可以打印出 MMIO 区域的虚拟地址和内核栈分配器的当前地址，直接比较它们是否有重叠。

**验证**: 串口输出中应该能看到明显的 SSTS 值变化——从非零变为零。

### Step 2: 分析根因——地址重叠

**目标**: 确认具体的地址重叠情况。

**设计思路**: 找到 AHCI 驱动中 MMIO 虚拟基址的定义（在 ahci.cpp 中是一个 `static constexpr` 常量），再找到 Task 栈虚拟地址分配器的起始地址（在 process.cpp 中也是一个常量）。比较两者是否相同或重叠。

在我们的 bug 中，两者恰好都是 `0xFFFF800000100000`。这意味着：
- idle task 的栈（4 页）映射到 `0xFFFF800000100000 ~ 0xFFFF800000103FFF`，覆盖了 BAR5 MMIO
- kernel_init 的栈映射到 `0xFFFF800000104000 ~ 0xFFFF800000107FFF`，覆盖了 AHCI 命令列表和 FIS 缓冲区
- boot task 的栈映射到 `0xFFFF800000108000 ~ 0xFFFF80000010BFFF`，进一步覆盖了更多 MMIO 空间

**验证**: 打印两个地址值，确认它们相同。

### Step 3: 创建统一的内存布局头文件

**目标**: 新建 `kernel/arch/x86_64/memory_layout.hpp`，集中定义所有内核虚拟地址区域。

**设计思路**: 用一组 `constexpr` 常量定义每个区域的 base 和 size。区域按顺序排列，每个区域的 base 是前一个区域的 base + size。这种「链式」布局的好处是新增区域时只需要插入一行。

区域布局如下：
- **Heap**: 起始地址 `0xFFFF800000000000`，大小根据需求设定——堆分配器从低地址向高地址增长
- **MMIO**: 紧跟 Heap 之后，给 AHCI BAR5 和其他设备映射使用
- **Framebuffer**: 紧跟 MMIO 之后，用于线性帧缓冲区的 MMIO 映射
- **Stack**: 紧跟 Framebuffer 之后，每个 Task 分配若干页从这里开始——这是解决冲突的关键：栈区域必须排在 MMIO 之后
- **DMA**: 紧跟 Stack 之后（留出足够的栈空间），用于 AHCI DMA 读写缓冲区
- **ext2 DMA**: 紧跟 DMA 之后，用于 ext2 文件系统的块缓存

**实现约束**: 所有常量放在 `cinux::arch` 命名空间下。每个区域有 `KMEM_*_BASE` 和 `KMEM_*_SIZE` 两个常量。SIZE 使用十六进制字面量并标注注释说明大小。头文件顶部有清晰的注释说明布局规则。

**踩坑预警**: Stack 区域的 size 不需要显式定义——因为每个 Task 的栈是按需分配的（通过 atomic fetch_add），只需要定义起始地址就行。但要确保 Stack 和 DMA 之间留出足够的间隔，否则大量 Task 的栈会溢出到 DMA 区域。一个保守的做法是留出 1 MB 的栈空间（大约 64 个 Task 各占 4 页）。

**验证**: 头文件创建后，各个消费者文件应该引用这些常量而不是硬编码地址。编译通过即可。

### Step 4: 更新各消费者文件

**目标**: 将散落在 ahci.cpp、process.cpp、ext2.cpp、main.cpp 中的魔法地址替换为布局常量。

**设计思路**: 每个文件中原来的 `static constexpr uint64_t XXX_VIRT_BASE = 0xFFFF8000xxxxxxxx` 常量改为引用 `cinux::arch::KMEM_*_BASE`。具体对应关系：
- ahci.cpp 的 `MMIO_VIRT_BASE` → `cinux::arch::KMEM_MMIO_BASE`
- process.cpp 的 `next_stack_vaddr` 初始值 → `cinux::arch::KMEM_STACK_BASE`
- ext2.cpp 的 `EXT2_DMA_VIRT_BASE` → `cinux::arch::KMEM_EXT2_DMA_BASE`
- main.cpp 的 `HEAP_VIRT_BASE` 和 `buf_virt` → `cinux::arch::KMEM_HEAP_BASE` 和 `KMEM_DMA_BASE`

**实现约束**: 每个文件需要 `#include "kernel/arch/x86_64/memory_layout.hpp"`。确保替换后地址值的含义没有改变——只是集中管理了，不是改变了地址。

**验证**: 编译通过。运行时确认：AHCI MMIO 映射在新的虚拟地址上，内核栈从 MMIO 之后开始分配，ext2 DMA 缓冲区在最末尾。

### Step 5: 端到端验证

**目标**: 确认修复后 ext2 挂载和 shell 启动正常。

**设计思路**: 构建并运行内核，检查完整的启动日志序列。关键检查点：
1. AHCI init 阶段：MMIO 映射到新的虚拟地址，Port 1 SSTS 正常
2. Scheduler init：idle task 栈映射到 MMIO 区域之后
3. Init 线程：ext2 挂载成功（不再超时），VFS 注册成功
4. Shell 启动：Ring 3 跳转正常，shell 提示符出现

**验证**: 完整的串口输出应该显示从调度器启动到 shell 提示符的完整链条，无任何超时或崩溃。

## 构建与运行

```
cd build && cmake .. && make -j$(nproc)
./run.sh
```

预期串口输出的关键片段：
```
[AHCI] BAR5 mapped, PI=0x3 CAP=0x...
[AHCI] Port 1: SSTS=0x113 DET=3 SIG=0x...
[AHCI] 2 active ports initialised.
[BIG] ===== Scheduler & Init Thread =====
[SCHED] Idle task created tid=1
[PROC] Created task tid=2 name='kernel_init' stack=0x...
[INIT] kernel_init started tid=2
[INIT] ===== Milestone 028: ext2 Filesystem =====
[EXT2] Superblock: magic=0xef53 ...
[INIT] ===== Milestone 027: VFS =====
[VFS] ext2 mounted at /
[USER] Jumping to Ring 3
shell> _
```

## 调试技巧

**如何确认 MMIO 映射完好**: 在 init 线程中，ext2 挂载之前，通过 AHCI instance 获取 hba_mem 指针，读取 Port 1 的 SSTS。如果值是 0x113 就说明映射完好，如果值是 0x0 就说明被覆盖了。这是一种非常有效的诊断手段——直接检查硬件寄存器是否可读。

**如何排查虚拟地址冲突**: 打印 `alloc_stack_vaddr` 分配的地址和各个 MMIO 虚拟基址，直接比较数值范围。如果发现任何重叠，就是冲突点。在我们的案例中，这个问题被隐藏得非常深——它只在 init 线程模型下才暴露，因为只有那时内核栈映射才会和 MMIO 映射同时存在。

**防止未来再犯**: 每次新增一个虚拟地址区域的消费者时，都要检查 `memory_layout.hpp` 中是否有空闲空间。如果需要新增区域，在头文件中插入并调整后续基址。不要在任何地方硬编码 `0xFFFF8000xxxxxxxx` 这样的地址——全部通过布局常量引用。

## 本章小结

| 概念 | 说明 |
|------|------|
| MMIO 地址冲突 | 内核栈虚拟地址覆盖了 AHCI BAR5 映射，导致硬件寄存器不可读 |
| SSTS 寄存器诊断 | 从 0x113 变成 0x0 标志着页表项被覆盖 |
| memory_layout.hpp | 集中定义所有内核虚拟地址区域的 (base, size) |
| 链式布局 | 每个区域的 base = 前一个区域的 base + size |
| 魔法地址的危险 | 散落各处的硬编码地址迟早会导致冲突 |

到这里，028e 的全部三个核心主题都讲完了。我们完成了从 kernel_main 到 init 线程的架构重构，修复了 shell_task 伪造和 AddressSpace 链接问题，排查并解决了 MMIO 地址冲突。现在我们的内核启动流程干净、规范，init 线程在调度器的管理下运行，用户态从正规的 Task 上下文启动——这才是操作系统该有的样子。
