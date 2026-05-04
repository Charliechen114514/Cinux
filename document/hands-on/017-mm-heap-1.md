# 017-1 BlockHeader 数据结构与堆初始化

## 导语

到 tag 016 为止，PMM 能分配物理页，VMM 能把虚拟地址映射到物理页——我们的内存管理"基础设施建设"已经完工了。但如果你现在想在内核里分配一个 100 字节的小结构体，唯一的办法就是从 PMM 拿一整页 4096 字节，然后只用了 100 字节，剩下的 3996 字节全部浪费。内核里到处都是这种零碎的小分配——字符串缓冲区、链表节点、设备描述符——每一样都消耗一整页的话，内存利用率会惨不忍睹。

这一章我们开始搭建堆分配器（Heap Allocator），它坐在 PMM 和 VMM 之上，把整页的虚拟内存切成任意大小的小块，按需分配给内核使用。这是内存管理层级中的第三层：PMM 管物理页、VMM 管虚拟映射、Heap 管子页粒度的分配。本章聚焦于最基础的部分——每个内存块的元数据头部（BlockHeader）和堆区域的初始化流程。

知识前置：你需要熟悉 tag 016 中 VMM 的 `map` 接口（将虚拟地址映射到物理页）以及 PMM 的 `alloc_page` 接口（分配一个物理页）。堆初始化的核心工作就是"从 VMM 那里借一段虚拟地址空间，映射好物理页，然后把整段空间标记为一个大空闲块"。

## 概念精讲

### 为什么需要堆分配器？

你可以把 PMM 想象成一个只批发不零售的建材供应商——你要买砖头，最少也得买一整托盘（4096 块），不接受拆零。VMM 则是运输公司，帮你把托盘送到指定的仓库位置。但实际装修的时候，你需要的是"给我 3 块砖""给我半袋水泥"这种小份量的分配。堆分配器就是零售商——它从批发商那里整托盘地进货，然后按客户需求拆开零售。

OSDev Wiki 上有一篇很好的 Memory Allocation 概述（https://wiki.osdev.org/Memory_Allocation），里面描述了经典的内存管理层级架构，和 Cinux 的设计如出一辙：PMM 在最底层分配整页物理内存，VMM 负责虚拟地址映射，堆分配器在最上层提供任意大小的分配接口。每一层只和直接相邻的层打交道——堆分配器不直接碰物理内存，它只向 VMM 请求更多的虚拟地址空间。

### BlockHeader：每个块的"身份证"

堆分配器管理的内存被划分成一个个连续的块（block），每个块前面紧贴着一段固定大小的元数据——BlockHeader。这个头部包含以下信息：一个 32 位的 magic 常量（固定值 0xDEADBEEF），用于运行时检测内存损坏和 double-free；一个 32 位的 size 字段，记录紧跟在头部后面的 payload 有多少字节；一个 32 位的 free 标志，值为 1 表示空闲、0 表示已分配；一个 12 字节的填充字段，把头部撑到合适的尺寸；以及一个 next 指针，把空闲块串成一个链表。

整个 BlockHeader 的尺寸被硬性规定为 32 字节，通过 `static_assert` 在编译期强制检查。这个数字不是随便选的——32 字节正好满足 16 字节对齐要求的整数倍，而且足够容纳上述所有字段。填充字段的存在就是为了把头部对齐到这个尺寸，以后如果你需要往头部里加新字段，可以缩减填充而不需要改常量。结构体加上 `[[gnu::packed]]` 属性，告诉编译器不要插入额外的对齐填充——我们需要精确控制每个字节的位置。

你可能觉得 magic 常量有点"多余"——它不是硬件要求，纯粹是软件层面的防御性编程。但在内核开发中，内存损坏的 bug 往往是最难排查的。如果一个指针错误导致某块内存被意外覆写，magic 常量会在 free 的时候立刻暴露问题，而不是让错误悄悄传播到系统崩溃才被发现。FreeRTOS 的 heap_4 实现就没有 magic 校验，在生产环境中可能无所谓，但在教学 OS 里这种安全网特别有价值。

### 空闲链表：堆分配器的核心数据结构

堆分配器用一个单链表来追踪所有空闲块——free_list_ 指向链表头，每个空闲块的 BlockHeader 中的 next 指针指向下一个空闲块。已分配的块不在链表中——它们的 next 指针为空，分配器不需要追踪已分配的块，因为 `free` 操作可以通过指针减去 HEADER_SIZE 直接定位到 BlockHeader。

这种设计有一个显而易见的限制：遍历空闲链表是 O(n) 的操作，n 是空闲块的数量。如果堆里碎片化严重（大量小空闲块），分配操作会变慢。但在内核的典型工作负载下，空闲块数量通常不会太多——几十个到几百个的水平——线性扫描完全够用。Linux 内核的 SLOB 分配器用的也是类似的 first-fit 线性扫描策略，在嵌入式场景中表现良好。

### 堆的虚拟地址选址

Cinux 把堆放在虚拟地址 0xFFFF800000000000——这是一个 higher-half canonical address，位于内核空间的"下半区"。这个地址的选择有几个考量：它远离内核代码段（0xFFFFFFFF80000000 附近），不会和内核映像、页表、MMIO 映射冲突；它足够低，将来可以向上扩展很大的范围；而且它是一个 4KB 对齐的地址，满足分页映射的要求。初始大小是 64 KB（16 个物理页），足够内核启动阶段的分配需求。

> 参考：[OSDev Wiki - Memory Allocation](https://wiki.osdev.org/Memory_Allocation)
> 参考：[dreamportdev Osdev Notes - Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md)

## 动手实现

### Step 1: 定义 BlockHeader 结构体

**目标**: 创建一个精确 32 字节的块头部结构体，包含 magic、size、free、填充和 next 指针。

**设计思路**: 结构体需要用 `[[gnu::packed]]` 属性确保编译器不会插入额外的对齐填充。字段依次是：magic（uint32_t，固定 0xDEADBEEF）、size（uint32_t，payload 大小）、free（uint32_t，空闲标志）、_pad（uint8_t[12]，填充到 32 字节）、next（BlockHeader*，链表指针）。加上 packed 属性后，这几个字段的总大小需要恰好是 32 字节——magic 4 + size 4 + free 4 + _pad 12 + next 8 = 32。用 `static_assert(sizeof(BlockHeader) == 32)` 做编译期检查。

**实现约束**: 结构体定义在 `kernel/mm/heap.hpp` 中，命名空间 `cinux::mm`。magic 常量不在结构体里定义，而是作为 `heap.cpp` 中的 `constexpr uint32_t` 常量（因为它是实现细节，外部不需要知道具体的 magic 值）。

**踩坑预警**: 如果你将来需要给 BlockHeader 添加新字段，必须同步修改 _pad 的大小，确保总大小仍然是 32 字节。如果你忘了改 static_assert，编译器会立刻报错提醒你——但如果你的修改恰好保持了 32 字节（比如把 _pad 缩了 4 字节又加了一个 uint32_t 字段），assert 不会触发，你需要自己确认新布局的合理性。另外，`[[gnu::packed]]` 意味着 next 指针可能不是自然对齐的——在某些架构上访问未对齐的指针会导致性能下降甚至 crash，但 x86-64 对未对齐访问有硬件支持，所以不用担心。

**验证**: 结构体定义完成后，编译即可验证 static_assert 通过。如果大小不是 32 字节，编译错误信息会告诉你实际的 sizeof 值，检查你的字段声明是否遗漏了什么。

### Step 2: 声明 Heap 类和全局实例

**目标**: 定义堆分配器的公开接口和私有状态。

**设计思路**: Heap 类的公开接口包括四个方法：`init(virt_base, initial_size)` 初始化堆区域；`alloc(size, align)` 分配一块指定大小和对齐的内存，返回 payload 指针；`free(ptr)` 释放之前分配的内存；`dump_stats()` 打印堆的统计信息到内核日志。私有部分包括：`expand(min_bytes)` 在堆空间不足时扩展；`coalesce(block)` 合并相邻空闲块；以及成员变量 base_（堆的虚拟基地址）、size_（总大小）、used_（已使用大小）、free_list_（空闲链表头指针）。

全局实例 `g_heap` 声明为 `extern Heap g_heap`，在 `heap.cpp` 中定义。整个内核通过 `cinux::mm::g_heap` 访问唯一的堆实例。

**实现约束**: 类定义在 `kernel/mm/heap.hpp`，命名空间 `cinux::mm`。成员变量使用类内初始化（`base_{}`、`size_{}` 等），默认构造的对象所有状态为零/空指针。alloc 的 align 参数有默认值 16。

**踩坑预警**: alloc 的返回类型是 `void*`——当请求大小为 0 时必须返回 `nullptr`，而不是一个有效的指针。这是 C 标准库 malloc 的语义，也是 C++ operator new 的要求。如果你返回了一个指向零字节 payload 的指针，调用者可能会试图往里面写数据，虽然没分配任何空间，但可能意外覆写相邻块的头部。

**验证**: 头文件编写完成后，在 `kernel/main.cpp` 中 include `kernel/mm/heap.hpp`，尝试引用 `cinux::mm::g_heap`——编译通过即可确认接口定义正确。

### Step 3: 实现 Heap::init 初始化函数

**目标**: 将指定虚拟地址范围的物理页映射好，创建初始空闲块。

**设计思路**: init 做五件事。第一，把 initial_size 向上对齐到页面大小（4096 字节）——如果调用者传入 60000，对齐后就是 65536（16 页）。第二，逐页分配物理内存并映射：循环从 PMM 调用 alloc_page 获取物理页，然后用 VMM::map 把它映射到堆区域的对应虚拟地址。映射标志位是 present + writable（0x03）。第三，把整个映射好的区域清零——这既是安全考虑（防止泄漏旧数据），也为了后续创建 BlockHeader 时不需要逐字段清零未使用的部分。第四，在堆区域的起始位置创建初始空闲块——这个块覆盖整个堆区域，它的 size 等于总大小减去 HEADER_SIZE（因为头部本身占 32 字节），free 标志为 1，next 为 nullptr。第五，保存堆的元数据（基地址、总大小、已使用量为零、空闲链表指向初始块）。

**实现约束**: 函数定义在 `kernel/mm/heap.cpp` 中。需要 include `kernel/mm/pmm.hpp`、`kernel/mm/vmm.hpp`、`kernel/arch/x86_64/paging_config.hpp`（获取 PAGE_SIZE）。常量 HEADER_SIZE 定义为 `sizeof(BlockHeader)`，PAGE_FLAGS 定义为 0x03。

**踩坑预警**: init 过程中如果 PMM 返回 0（内存不足），必须立即停止并报告错误——不能继续映射一个空物理页，否则后续对堆内存的读写会触发 page fault。另外，memzero 清零操作必须在映射完成之后、创建 BlockHeader 之前——如果顺序搞反了，清零会把刚写好的 BlockHeader 也清掉。

**验证**: 在 `kernel/main.cpp` 的启动序列中，在 VMM 初始化之后、framebuffer 初始化之前，调用 `g_heap.init(0xFFFF800000000000ULL, 64 * 1024)`。串口输出应该显示 `[HEAP] Initialised at 0x...000, size 64 KB`。

### Step 4: 实现 dump_stats 调试输出

**目标**: 遍历空闲链表，统计空闲空间总量和块数量，打印汇总信息。

**设计思路**: dump_stats 从 free_list_ 开始遍历链表，对每个块累加它的 size（只统计 free=1 的块），同时计数总块数。最终通过 kprintf 输出四个数字：总堆大小（KB）、已使用量（KB）、空闲链表中的空闲空间（KB）、空闲块数量。这个函数在调试时非常有用——你可以随时调用它来检查堆的健康状况。

**实现约束**: 函数是 const 方法——它只读不写。输出格式要统一，方便 grep 或者肉眼比对。

**踩坑预警**: dump_stats 遍历的是空闲链表，不是堆中的所有块。已分配的块不在链表中，所以"已使用量"不是从链表中统计出来的，而是从 used_ 成员变量读取的。如果你发现 free_total + used 和 total 对不上，可能是 coalesce 或 expand 的逻辑有问题，导致某些空间既没有被计入 used_ 也没有出现在空闲链表中。

**验证**: 在 init 之后立即调用 `g_heap.dump_stats()`。预期输出是：total=64 KB，used=0 KB，free_list≈63 KB（减去一个 HEADER_SIZE 的开销），blocks=1。

### Step 5: 将堆初始化插入内核启动序列

**目标**: 在 main.cpp 的启动流程中，在正确位置调用堆初始化。

**设计思路**: 堆必须初始化在 VMM 之后——因为 init 需要调用 VMM::map 来映射物理页。同时应该尽量早——因为后续的 C++ 全局对象构造、framebuffer 初始化、设备驱动初始化都可能用到动态内存分配。所以 Cinux 把堆初始化放在启动序列的第 9 步，紧跟 VMM（第 8 步）之后，在 framebuffer（第 10 步）之前。

需要定义两个常量：`HEAP_VIRT_BASE = 0xFFFF800000000000ULL` 和 `HEAP_INITIAL_SIZE = 64 * 1024`。这两个值目前硬编码在 main.cpp 中，将来如果需要更灵活的配置可以提取到配置头文件。

**踩坑预警**: 堆的虚拟基地址不能和内核映像、页表、MMIO 区域重叠。0xFFFF800000000000 在 Cinux 的地址空间布局中是安全的，但如果你修改了链接脚本或者增加了新的 MMIO 映射，需要确认没有冲突。另一个要注意的是：如果在堆初始化之前的代码中不小心调用了 operator new（比如某个全局对象的构造函数），它会触发旧的 halt-on-use stub——内核直接死锁。确保堆初始化在所有需要动态内存的操作之前完成。

**验证**: 编译并运行内核：

```bash
cmake --build build && ./build/run_qemu.sh
```

串口输出中应该看到 `[HEAP] Initialised at 0xFFFF800000000000, size 64 KB`，后面跟着后续初始化步骤的正常输出。如果堆初始化失败（PMM 内存不足），会看到 `[HEAP] OOM during init` 的错误消息。

## 构建与运行

本章涉及两个新文件：`kernel/mm/heap.hpp`（头文件，BlockHeader + Heap 类声明）和 `kernel/mm/heap.cpp`（init 和 dump_stats 的实现）。需要在 `kernel/CMakeLists.txt` 的 `big_kernel_common` 库中添加 `mm/heap.cpp`。

构建后运行 QEMU，串口输出应包含：

```
[BIG] VMM initialised, kernel PML4 at phys ...
[HEAP] Initialised at 0xFFFF800000000000, size 64 KB
[HEAP] Stats: total=64 KB, used=0 KB, free_list=63 KB, blocks=1
[BIG] Framebuffer initialised: ...
```

## 调试技巧

**init 时 OOM**: 如果看到 `[HEAP] OOM during init`，说明 PMM 没有足够的物理页来映射 64 KB 的堆区域。检查 PMM 的 free_page_count 输出——64 KB 需要 16 个物理页。在 QEMU 默认配置（128MB 内存）下这不应该发生，但如果你的 PMM 初始化有 bug 导致可用页数不足，就会在这里暴露。

**BlockHeader 大小不对**: 如果 static_assert 触发，检查你的字段类型和数量是否和上面描述的一致。常见的错误是 _pad 的大小写错了（比如写成 `_pad[10]`），或者忘记加 `[[gnu::packed]]` 属性导致编译器在 next 指针前插入了 6 字节的对齐填充。

**dump_stats 的 free_total 和 total 对不上**: 初始状态下，free_total 应该等于 total - 32（一个 HEADER_SIZE 的开销）。如果差得更多，可能是初始空闲块的 size 计算有误——正确值是 `aligned_size - HEADER_SIZE`，不是 `aligned_size`。

## 本章小结

| 概念 | 要点 |
|------|------|
| BlockHeader | 32 字节 packed 结构体：magic(0xDEADBEEF) + size + free + _pad[12] + next |
| static_assert | 编译期保证 sizeof(BlockHeader) == 32 |
| Heap::init | 对齐大小 -> 逐页映射 -> 清零 -> 创建初始空闲块 -> 保存元数据 |
| 空闲链表 | free_list_ 指向链表头，空闲块通过 next 串联 |
| 堆地址 | 0xFFFF800000000000，64 KB 初始大小，在 VMM 之后初始化 |
| dump_stats | 遍历空闲链表统计 free_total 和 block_count，配合 used_ 输出四项指标 |
