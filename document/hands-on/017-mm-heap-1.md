# 017-1 BlockHeader 数据结构与堆初始化

## 导语

到 tag 016 为止，PMM 能分配物理页，VMM 能把虚拟地址映射到物理页——我们的内存管理"基础设施建设"已经完工了。但如果你现在想在内核里分配一个 100 字节的小结构体，唯一的办法就是从 PMM 拿一整页 4096 字节，然后只用了 100 字节，剩下的 3996 字节全部浪费。内核里到处都是这种零碎的小分配——字符串缓冲区、链表节点、设备描述符——每一样都消耗一整页的话，内存利用率会惨不忍睹。

这一章我们开始搭建堆分配器（Heap Allocator），它坐在 PMM 和 VMM 之上，把整页的虚拟内存切成任意大小的小块，按需分配给内核使用。这是内存管理层级中的第三层：PMM 管物理页、VMM 管虚拟映射、Heap 管子页粒度的分配。本章聚焦于最基础的部分——每个内存块的元数据头部（BlockHeader）和堆区域的初始化流程。

知识前置：你需要熟悉 tag 016 中 VMM 的 `map` 接口（将虚拟地址映射到物理页）以及 PMM 的 `alloc_page` 接口（分配一个物理页）。堆初始化的核心工作就是"从 VMM 那里借一段虚拟地址空间，映射好物理页，然后把整段空间标记为一个大空闲块"。

本章所有代码都在 `kernel/mm/` 目录下。新文件只有两个：`heap.hpp`（头文件，约 126 行）和 `heap.cpp`（实现，约 353 行）。编译时需要在 `kernel/CMakeLists.txt` 的 `big_kernel_common` 库中添加 `mm/heap.cpp`。

## 概念精讲

### 为什么需要堆分配器？

你可以把 PMM 想象成一个只批发不零售的建材供应商——你要买砖头，最少也得买一整托盘（4096 块），不接受拆零。VMM 则是运输公司，帮你把托盘送到指定的仓库位置。但实际装修的时候，你需要的是"给我 3 块砖""给我半袋水泥"这种小份量的分配。堆分配器就是零售商——它从批发商那里整托盘地进货，然后按客户需求拆开零售。

OSDev Wiki 上有一篇很好的 Memory Allocation 概述（https://wiki.osdev.org/Memory_Allocation），里面描述了经典的内存管理层级架构，和 Cinux 的设计如出一辙：PMM 在最底层分配整页物理内存，VMM 负责虚拟地址映射，堆分配器在最上层提供任意大小的分配接口。每一层只和直接相邻的层打交道——堆分配器不直接碰物理内存，它只向 VMM 请求更多的虚拟地址空间。

这个三层架构在工业界被广泛采用。Linux 内核的三层是 buddy（物理页管理）-> slab（固定大小对象缓存）-> kmalloc（通用分配接口）。FreeRTOS 虽然没有 VMM（它是单地址空间的 RTOS），但它的 heap_4 本质上也是坐在 PMM 之上的 sub-page 分配器。理解了这个分层模型，你就能看懂几乎所有 OS 的内存管理设计。

### BlockHeader：每个块的"身份证"

堆分配器管理的内存被划分成一个个连续的块（block），每个块前面紧贴着一段固定大小的元数据——BlockHeader。这个头部包含以下信息：一个 32 位的 magic 常量（固定值 0xDEADBEEF），用于运行时检测内存损坏和 double-free；一个 32 位的 size 字段，记录紧跟在头部后面的 payload 有多少字节；一个 32 位的 free 标志，值为 1 表示空闲、0 表示已分配；一个 12 字节的填充字段，把头部撑到合适的尺寸；以及一个 next 指针，把空闲块串成一个链表。

整个 BlockHeader 的尺寸被硬性规定为 32 字节，通过 `static_assert` 在编译期强制检查。这个数字不是随便选的——32 字节正好满足 16 字节对齐要求的整数倍，而且足够容纳上述所有字段。填充字段的存在就是为了把头部对齐到这个尺寸，以后如果你需要往头部里加新字段，可以缩减填充而不需要改常量。结构体加上 `[[gnu::packed]]` 属性，告诉编译器不要插入额外的对齐填充——我们需要精确控制每个字节的位置。

你可能觉得 magic 常量有点"多余"——它不是硬件要求，纯粹是软件层面的防御性编程。但在内核开发中，内存损坏的 bug 往往是最难排查的。如果一个指针错误导致某块内存被意外覆写，magic 常量会在 free 的时候立刻暴露问题，而不是让错误悄悄传播到系统崩溃才被发现。FreeRTOS 的 heap_4 实现就没有 magic 校验，在生产环境中可能无所谓，但在教学 OS 里这种安全网特别有价值。

关于 `[[gnu::packed]]` 在 x86-64 上的安全性：packed 属性意味着 BlockHeader 的 `next` 指针（偏移量 24）可能不是自然对齐的（8 字节指针从 24 + 12 = 24 的位置开始是 8 的倍数——实际上在这里恰好是）。但在其他场景下，如果 padding 更小，`next` 可能从奇数地址开始。x86-64 硬件支持未对齐访问（不像 ARM 那样会触发 alignment fault），所以不会 crash，但未对齐访问可能稍慢。在 Cinux 的场景下，这种性能差异可以忽略不计。

### 空闲链表：堆分配器的核心数据结构

堆分配器用一个单链表来追踪所有空闲块——free_list_ 指向链表头，每个空闲块的 BlockHeader 中的 next 指针指向下一个空闲块。已分配的块不在链表中——它们的 next 指针为空，分配器不需要追踪已分配的块，因为 `free` 操作可以通过指针减去 HEADER_SIZE 直接定位到 BlockHeader。

这种设计有一个显而易见的限制：遍历空闲链表是 O(n) 的操作，n 是空闲块的数量。如果堆里碎片化严重（大量小空闲块），分配操作会变慢。但在内核的典型工作负载下，空闲块数量通常不会太多——几十个到几百个的水平——线性扫描完全够用。Linux 内核的 SLOB 分配器用的也是类似的 first-fit 线性扫描策略，在嵌入式场景中表现良好。

为什么不用双向链表？单链表在删除节点时需要从头遍历找前驱，这是一个 O(n) 的操作——我们在 coalesce 中会大量遇到这个场景。但如果用双向链表，每个 BlockHeader 需要额外存一个 prev 指针，把头部从 32 字节膨胀到 40 字节，这会让每个块的元数据开销增加 25%。对于教学内核来说，32 字节这个"2 的幂"的尺寸带来的地址计算简便性更重要。

### 堆的虚拟地址选址

Cinux 把堆放在虚拟地址 0xFFFF800000000000——这是一个 higher-half canonical address，位于内核空间的"下半区"。这个地址的选择有几个考量：它远离内核代码段（0xFFFFFFFF80000000 附近），不会和内核映像、页表、MMIO 映射冲突；它足够低，将来可以向上扩展很大的范围；而且它是一个 4KB 对齐的地址，满足分页映射的要求。初始大小是 64 KB（16 个物理页），足够内核启动阶段的分配需求。

如果你好奇这个地址在页表中的位置：PML4 索引是 256（`(0xFFFF800000000000 >> 39) & 0x1FF`），恰好是 canonical 高半区的第一个 PML4 表项。从 PML4[256] 开始一直到 PML4[511]（内核代码段所在位置），中间有 256 个 PML4 表项可用，对应 256 * 512 GB = 128 TB 的虚拟地址空间。堆从 PML4[256] 的起始处开始，可以向上连续增长非常大的范围，不必担心和更高地址的内核数据结构冲突。

> 参考：[OSDev Wiki - Memory Allocation](https://wiki.osdev.org/Memory_Allocation)
> 参考：[dreamportdev Osdev Notes - Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md)

## 动手实现

### Step 1: 定义 BlockHeader 结构体

**目标**: 创建一个精确 32 字节的块头部结构体，包含 magic、size、free、填充和 next 指针。

**设计思路**: 结构体需要用 `[[gnu::packed]]` 属性确保编译器不会插入额外的对齐填充。字段依次是：magic（uint32_t，固定 0xDEADBEEF）、size（uint32_t，payload 大小）、free（uint32_t，空闲标志）、_pad（uint8_t[12]，填充到 32 字节）、next（BlockHeader*，链表指针）。加上 packed 属性后，这几个字段的总大小需要恰好是 32 字节——magic 4 + size 4 + free 4 + _pad 12 + next 8 = 32。用 `static_assert(sizeof(BlockHeader) == 32)` 做编译期检查。

**实现约束**: 结构体定义在 `kernel/mm/heap.hpp` 中，命名空间 `cinux::mm`。magic 常量不在结构体里定义，而是作为 `heap.cpp` 中的 `constexpr uint32_t` 常量（因为它是实现细节，外部不需要知道具体的 magic 值）。

**踩坑预警**: 如果你将来需要给 BlockHeader 添加新字段，必须同步修改 _pad 的大小，确保总大小仍然是 32 字节。如果你忘了改 static_assert，编译器会立刻报错提醒你——但如果你的修改恰好保持了 32 字节（比如把 _pad 缩了 4 字节又加了一个 uint32_t 字段），assert 不会触发，你需要自己确认新布局的合理性。另外，`[[gnu::packed]]` 意味着 next 指针可能不是自然对齐的——在某些架构上访问未对齐的指针会导致性能下降甚至 crash，但 x86-64 对未对齐访问有硬件支持，所以不用担心。

**验证**: 结构体定义完成后，编译即可验证 static_assert 通过（如果大小不是 32 字节，编译错误信息会告诉你实际的 sizeof 值，检查你的字段声明是否遗漏了什么）。

你可以用以下方式在编译时确认 BlockHeader 的布局：

你可以用以下方式在编译时看到 BlockHeader 的布局详情：

```bash
x86_64-elf-g++ -std=c++20 -dM -E - < /dev/null  # 检查编译器版本
```

如果编译器支持 `__builtin_dump_struct`（Clang），可以直接在运行时打印结构体的字段偏移和大小。GCC 目前没有这个特性，但你可以手写一个 `dump_block_header` 函数，用 `offsetof` 打印每个字段的偏移量，确认布局是否符合预期。

### Step 2: 声明 Heap 类和全局实例

**目标**: 定义堆分配器的公开接口和私有状态。

**设计思路**: Heap 类的公开接口包括四个方法：`init(virt_base, initial_size)` 初始化堆区域；`alloc(size, align)` 分配一块指定大小和对齐的内存，返回 payload 指针；`free(ptr)` 释放之前分配的内存；`dump_stats()` 打印堆的统计信息到内核日志。私有部分包括：`expand(min_bytes)` 在堆空间不足时扩展；`coalesce(block)` 合并相邻空闲块；以及成员变量 base_（堆的虚拟基地址）、size_（总大小）、used_（已使用大小）、free_list_（空闲链表头指针）。

全局实例 `g_heap` 声明为 `extern Heap g_heap`，在 `heap.cpp` 中定义。整个内核通过 `cinux::mm::g_heap` 访问唯一的堆实例。这种"单例"设计在内核中很常见——Linux 有全局的 `kmalloc` 接口，FreeRTOS 用一个静态数组 `ucHeap[configTOTAL_HEAP_SIZE]`。Cinux 的做法更灵活——堆区域不是静态数组，而是通过 VMM 动态映射的虚拟地址空间，可以在运行时扩展。

**实现约束**: 类定义在 `kernel/mm/heap.hpp`，命名空间 `cinux::mm`。成员变量使用类内初始化（`base_{}`、`size_{}` 等），默认构造的对象所有状态为零/空指针。alloc 的 align 参数有默认值 16。

**踩坑预警**: alloc 的返回类型是 `void*`——当请求大小为 0 时必须返回 `nullptr`，而不是一个有效的指针。这是 C 标准库 malloc 的语义，也是 C++ operator new 的要求。如果你返回了一个指向零字节 payload 的指针，调用者可能会试图往里面写数据，虽然没分配任何空间，但可能意外覆写相邻块的头部。

**验证**: 头文件编写完成后，在 `kernel/main.cpp` 中 include `kernel/mm/heap.hpp`，尝试引用 `cinux::mm::g_heap`——编译通过即可确认接口定义正确。

另一个值得验证的细节是成员变量的初始值。在堆初始化之前（`init` 调用之前），如果你意外调用了 `dump_stats` 或其他方法，所有成员都应该是零——`base_` 是 0、`size_` 是 0、`used_` 是 0、`free_list_` 是 `nullptr`。这意味着在未初始化状态下调用 `alloc` 会立即搜索 `nullptr`，循环直接退出，然后触发 `expand`——而 expand 会访问 `base_ + size_`（即地址 0），导致 page fault。这个"fail-fast"的行为虽然不够优雅，但至少不会静默地踩内存。

### Step 3: 实现 Heap::init 初始化函数

**目标**: 将指定虚拟地址范围的物理页映射好，创建初始空闲块。

**设计思路**: init 做五件事。第一，把 initial_size 向上对齐到页面大小（4096 字节）——如果调用者传入 60000，对齐后就是 65536（16 页）。第二，逐页分配物理内存并映射：循环从 PMM 调用 alloc_page 获取物理页，然后用 VMM::map 把它映射到堆区域的对应虚拟地址。映射标志位是 present + writable（0x03）。第三，把整个映射好的区域清零——这既是安全考虑（防止泄漏旧数据），也为了后续创建 BlockHeader 时不需要逐字段清零未使用的部分。第四，在堆区域的起始位置创建初始空闲块——这个块覆盖整个堆区域，它的 size 等于总大小减去 HEADER_SIZE（因为头部本身占 32 字节），free 标志为 1，next 为 nullptr。第五，保存堆的元数据（基地址、总大小、已使用量为零、空闲链表指向初始块）。

这五个步骤的顺序不能随意调换。映射必须在清零之前（否则清零操作会触发 page fault，因为虚拟地址还没有映射到物理页）。清零必须在创建 BlockHeader 之前（否则清零会把刚写好的 header 也擦掉）。BlockHeader 的创建必须在保存元数据之前（因为 free_list_ 需要指向已经创建好的 block）。如果你调换了映射和清零的顺序，运行时表现取决于 CPU 是否有 TLB 缓存——有时候"恰好能工作"（因为之前的映射残留），有时候 crash，这种不可靠的 bug 最难定位。

还有一个值得注意的细节：初始空闲块的 `first->size` 应该等于 `aligned_size - HEADER_SIZE`，而不是 `aligned_size`。前者意味着第一个 payload 从 header 结束处开始，大小等于总区域减去 32 字节的头部开销。如果错误地设为 `aligned_size`，第一个块的 payload 会延伸到堆区域之外 32 字节，下一次分配可能踩到未映射的内存。这个 off-by-one 错误在初始状态下不容易发现（因为 64 KB 的堆远大于初始分配），但在堆空间紧张时会触发 page fault。

**实现约束**: 函数定义在 `kernel/mm/heap.cpp` 中。需要 include `kernel/mm/pmm.hpp`、`kernel/mm/vmm.hpp`、`kernel/arch/x86_64/paging_config.hpp`（获取 PAGE_SIZE）。常量 HEADER_SIZE 定义为 `sizeof(BlockHeader)`，PAGE_FLAGS 定义为 0x03。

**踩坑预警**: init 过程中如果 PMM 返回 0（内存不足），必须立即停止并报告错误——不能继续映射一个空物理页，否则后续对堆内存的读写会触发 page fault。另外，memzero 清零操作必须在映射完成之后、创建 BlockHeader 之前——如果顺序搞反了，清零会把刚写好的 BlockHeader 也清掉。

一个容易忽略的边界情况：如果调用者传入的 `initial_size` 为 0 会发生什么？`align_up(0, PAGE_SIZE)` 返回 0，映射循环不会执行（`0 < 0` 为 false），memzero 也不会清零任何东西，然后创建 BlockHeader 时 `first->size = 0 - HEADER_SIZE` 会下溢为一个巨大的无符号数。这是一个需要防范的 bug——可以在 init 开头加入参数校验。当前的实现假设调用者（main.cpp）总是传入合理的值（64 KB），所以没有做这个检查，但防御性编程永远是值得的。

**验证**: 在 `kernel/main.cpp` 的启动序列中，在 VMM 初始化之后、framebuffer 初始化之前，调用 `g_heap.init(0xFFFF800000000000ULL, 64 * 1024)`。串口输出应该显示 `[HEAP] Initialised at 0x...000, size 64 KB`。

你可以通过以下检查进一步确认初始化的正确性。第一，在 init 之后立刻读取 `g_heap` 的 `base_` 值，应该是 `0xFFFF800000000000`。第二，`size_` 应该等于传入的 `initial_size`（64 KB），因为 64 KB 已经是页对齐的（64 KB / 4096 = 16 页）。第三，如果你传入一个非页对齐的大小（比如 60000），`size_` 应该被向上对齐到 65536（64 KB）。你可以用 `kprintf` 打印这些值做快速检查。

### Step 4: 实现 dump_stats 调试输出

**目标**: 遍历空闲链表，统计空闲空间总量和块数量，打印汇总信息。

**设计思路**: dump_stats 从 free_list_ 开始遍历链表，对每个块累加它的 size（只统计 free=1 的块），同时计数总块数。最终通过 kprintf 输出四个数字：总堆大小（KB）、已使用量（KB）、空闲链表中的空闲空间（KB）、空闲块数量。这个函数在调试时非常有用——你可以随时调用它来检查堆的健康状况。

注意 dump_stats 输出的 `free_list` 值和 `total - used` 不一定相等。差值来自两个来源：一是内部碎片（front padding 和 MIN_SPLIT 以下的 tail remainder），这些空间被分配但不在 payload 中；二是空闲链表中的块本身的 header 占用的空间（header 的 32 字节从"可用 payload"的角度看是"浪费"的，但从 `size_` 的角度看是"堆的一部分"）。理解这个差值对调试碎片化问题很重要——如果你发现 `total - used` 远大于 `free_list`，说明碎片化严重。

**实现约束**: 函数是 const 方法——它只读不写。输出格式要统一，方便 grep 或者肉眼比对。dump_stats 还可以用来发现链表损坏——如果遍历过程中遇到了 magic 不正确的块，程序可能会 crash。一个更健壮的版本可以在遍历时加入 magic 校验。

**踩坑预警**: dump_stats 遍历的是空闲链表，不是堆中的所有块。已分配的块不在链表中，所以"已使用量"不是从链表中统计出来的，而是从 used_ 成员变量读取的。如果你发现 free_total + used 和 total 对不上，可能是 coalesce 或 expand 的逻辑有问题，导致某些空间既没有被计入 used_ 也没有出现在空闲链表中。

**验证**: 在 init 之后立即调用 `g_heap.dump_stats()`。预期输出是：total=64 KB，used=0 KB，free_list≈63 KB（减去一个 HEADER_SIZE 的开销），blocks=1。

关于 memzero 的性能有一个值得注意的点：当前的实现是逐字节清零的，对于 64 KB 的堆区域来说需要循环 65536 次。这在内核启动阶段（只调用一次 init）是可以接受的，但如果你想优化，可以用 8 字节的 uint64_t 赋值代替逐字节操作，理论上速度提升 8 倍。不过实际瓶颈可能在内存访问延迟而非循环次数——现代 CPU 的 store buffer 可以吸收大量的顺序写入。在 Cinux 的当前阶段，逐字节清零的简单实现优先于微优化。

### Step 5: 将堆初始化插入内核启动序列

**目标**: 在 main.cpp 的启动流程中，在正确位置调用堆初始化。

**设计思路**: 堆必须初始化在 VMM 之后——因为 init 需要调用 VMM::map 来映射物理页。同时应该尽量早——因为后续的 C++ 全局对象构造、framebuffer 初始化、设备驱动初始化都可能用到动态内存分配。所以 Cinux 把堆初始化放在启动序列的第 9 步，紧跟 VMM（第 8 步）之后，在 framebuffer（第 10 步）之前。

需要定义两个常量：`HEAP_VIRT_BASE = 0xFFFF800000000000ULL` 和 `HEAP_INITIAL_SIZE = 64 * 1024`。这两个值目前硬编码在 main.cpp 中，将来如果需要更灵活的配置可以提取到配置头文件。

`HEAP_VIRT_BASE` 的地址选择需要考虑和链接脚本（linker script）中定义的其他虚拟地址段的冲突。Cinux 的链接脚本定义了 `KERNEL_VMA = 0xFFFFFFFF80000000` 作为内核代码段的起始地址。堆地址 `0xFFFF800000000000` 和内核代码段之间有约 128 TB 的距离（从 `0xFFFF800000000000` 到 `0xFFFFFFFF80000000`），这个空间足够堆增长到非常大的规模。如果你修改了链接脚本（比如把内核代码段移到了更低的地址），需要确认新的地址不会和堆区域重叠。

**踩坑预警**: 堆的虚拟基地址不能和内核映像、页表、MMIO 区域重叠。0xFFFF800000000000 在 Cinux 的地址空间布局中是安全的，但如果你修改了链接脚本或者增加了新的 MMIO 映射，需要确认没有冲突。另一个要注意的是：如果在堆初始化之前的代码中不小心调用了 operator new（比如某个全局对象的构造函数），它会触发旧的 halt-on-use stub——内核直接死锁。确保堆初始化在所有需要动态内存的操作之前完成。

**验证**: 编译并运行内核：

```bash
cmake --build build && ./build/run_qemu.sh
```

串口输出中应该看到 `[HEAP] Initialised at 0xFFFF800000000000, size 64 KB`，后面跟着后续初始化步骤的正常输出。如果堆初始化失败（PMM 内存不足），会看到 `[HEAP] OOM during init` 的错误消息。

你还可以在 QEMU 的 monitor 中使用 `info mem` 命令检查堆区域的页表映射是否正确。如果 `0xFFFF800000000000` 到 `0xFFFF80000000FFFF` 的地址范围在页表中没有对应的映射项，说明 init 的映射循环有问题——可能是 VMM::map 没有正确创建 PTE，也可能是 PML4 的索引计算错误。

## 构建与运行

本章完成了堆分配器的骨架搭建。涉及两个新文件：`kernel/mm/heap.hpp`（头文件，BlockHeader + Heap 类声明）和 `kernel/mm/heap.cpp`（init 和 dump_stats 的实现）。需要在 `kernel/CMakeLists.txt` 的 `big_kernel_common` 库中添加 `mm/heap.cpp`。

构建后运行 QEMU，串口输出应包含：

```
[BIG] VMM initialised, kernel PML4 at phys ...
[HEAP] Initialised at 0xFFFF800000000000, size 64 KB
[HEAP] Stats: total=64 KB, used=0 KB, free_list=63 KB, blocks=1
[BIG] Framebuffer initialised: ...
```

如果你在输出中看到 `[HEAP] OOM during init at offset XXXX` 而不是正常的初始化消息，说明 PMM 无法为堆提供足够的物理页。64 KB 需要 16 个物理页（16 * 4096 = 65536），在 QEMU 默认的 128 MB 内存配置下这不应该发生。检查 PMM 的初始化是否正确解析了 BootInfo 中的内存映射，确保可用物理页的数量足够覆盖内核映像、页表和堆的初始需求。

本章搭建了骨架，但堆分配器还不能分配内存——下一章将实现 alloc 的 first-fit 搜索和块分裂逻辑。

## 调试技巧

**init 时 OOM**: 如果看到 `[HEAP] OOM during init`，说明 PMM 没有足够的物理页来映射 64 KB 的堆区域。检查 PMM 的 free_page_count 输出——64 KB 需要 16 个物理页。在 QEMU 默认配置（128MB 内存）下这不应该发生，但如果你的 PMM 初始化有 bug 导致可用页数不足，就会在这里暴露。

**BlockHeader 大小不对**: 如果 static_assert 触发，检查你的字段类型和数量是否和上面描述的一致。常见的错误是 _pad 的大小写错了（比如写成 `_pad[10]`），或者忘记加 `[[gnu::packed]]` 属性导致编译器在 next 指针前插入了 6 字节的对齐填充。

**编译报错 "invalid application of sizeof to incomplete type"**: 这个错误说明编译器在看到 `static_assert` 时还不知道 BlockHeader 的完整定义。确保 `static_assert` 放在结构体定义之后（而不是之前）。

**dump_stats 的 free_total 和 total 对不上**: 初始状态下，free_total 应该等于 total - 32（一个 HEADER_SIZE 的开销）。如果差得更多，可能是初始空闲块的 size 计算有误——正确值是 `aligned_size - HEADER_SIZE`，不是 `aligned_size`。

**init 后分配立即 page fault**: 这通常意味着映射没有成功建立。在 init 的映射循环中加入 kprintf 打印每次映射的虚拟地址和物理地址，确认 `g_pmm.alloc_page()` 返回的不是 0，`g_vmm.map()` 也没有报错。如果 VMM 的 `map` 函数在映射 higher-half 地址时行为异常（比如它只映射了低于 4 GB 的地址），你需要检查 VMM 实现中的 PML4 索引计算是否正确处理了高地址。

## 扩展思考

**为什么不用位图代替链表？** 你可能会想到 PMM 用位图管理空闲页，为什么堆分配器不用同样的方式？原因是堆块的大小不固定——位图适用于固定大小的分配单元（比如一个页），每个 bit 代表一个单元的空闲/占用状态。堆块的大小从 1 字节到几 KB 都有，位图的粒度无法适应这种变化。链表虽然遍历是 O(n)，但每个块自带大小信息，天然支持变长分配。

**为什么 BlockHeader 放在块的前面而不是后面？** 有些分配器把元数据放在 payload 之后。但前面放置有一个显著优势——free 时只需要做 `ptr - HEADER_SIZE` 就能定位 header，不需要知道块的总大小。如果 header 放在后面，你就需要知道 payload 的大小才能算出 header 的位置，形成了一个"先有鸡还是先有蛋"的问题。

**alloc 返回的内存为什么总是清零的？** C 标准对 `malloc` 没有这个要求——malloc 返回的内存内容是未定义的。但 Cinux 的 alloc 总是清零 payload。这有两个好处：安全（不会泄露上一个使用者的数据）和方便（调用者不需要手动 memset）。代价是每次 alloc 多了一次 memzero 调用，但 memzero 的开销在内核堆的规模下可以忽略。Linux 的 `kzalloc` 也是同样的"清零分配"语义。

**内核堆和用户态堆有什么不同？** 用户态的 `malloc` 由 C 标准库（glibc、musl 等）实现，底层调用 `brk` 或 `mmap` 系统调用从操作系统获取内存。内核堆没有"更底层"可以求助——它必须自己管理物理页的分配和虚拟地址的映射。这意味着内核堆的实现必须直接调用 PMM 和 VMM，而不是像用户态那样通过系统调用。这种"自举"的性质使得内核堆的实现比用户态 malloc 更底层，但也更直接——你看到的每一行代码都在真实地操作硬件资源。

## 本章小结

本章搭建了堆分配器的骨架——BlockHeader 数据结构和 Heap 类的接口定义，以及 init 和 dump_stats 的完整实现。下一章我们将实现 alloc 的 first-fit 搜索和块分裂逻辑，让堆真正能分配内存。

| 概念 | 要点 |
|------|------|
| BlockHeader | 32 字节 packed 结构体：magic(0xDEADBEEF) + size + free + _pad[12] + next |
| static_assert | 编译期保证 sizeof(BlockHeader) == 32，任何字段修改导致大小变化都会被立即捕获 |
| [[gnu::packed]] | 消除编译器自动插入的对齐填充，确保精确控制内存布局 |
| Heap::init | 对齐大小 -> 逐页映射 -> 清零 -> 创建初始空闲块 -> 保存元数据 |
| 空闲链表 | free_list_ 指向链表头，空闲块通过 next 串联 |
| 堆地址 | 0xFFFF800000000000，64 KB 初始大小，在 VMM 之后初始化 |
| dump_stats | 遍历空闲链表统计 free_total 和 block_count，配合 used_ 输出四项指标 |
| 地址空间安全性 | 堆地址远离内核代码段、页表和 MMIO 区域，避免重叠 |
| 初始化顺序 | 对齐大小 -> 逐页映射 -> 清零 -> 创建初始空闲块 -> 保存元数据，五步顺序不可调换 |
