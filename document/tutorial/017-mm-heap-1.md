# 从页到字节：为什么内核需要堆分配器

> 标签：堆分配器, free list, BlockHeader, first-fit, coalescing
> 前置：[016-3 映射、翻译与缺页处理](016-mm-vmm-3.md)

## 前言

上一章结束时，Cinux 的内存管理栈已经搭好了两层底座——PMM 负责分配 4KB 的物理页，VMM 负责把物理页映射到虚拟地址空间。听起来好像"内存管理"已经做完了。但如果你试过在内核里写一句 `new int[10]`，就会立刻意识到一个问题：PMM 的最小粒度是 4096 字节，你想要 40 字节，它也给你一整页；VMM 更不用说，它只管"这个虚拟地址对应哪个物理页"，根本不知道你只想要一小块。说白了，我们的内核到现在还做不到"按需分配任意大小的内存"。

这对一个教学 OS 来说可能还能忍——毕竟你可以在内核代码里用静态数组凑合。但如果你以后要加文件系统的缓冲区缓存，你不知道运行时到底会有多少文件被打开；如果你要加网络栈的 packet buffer，你不知道会收到多大的包；如果你要实现进程控制块的动态创建，你不知道用户会同时跑多少个进程。这些场景的共同点是"大小和数量在编译时不可预知"——静态数组要么太大浪费内存，要么太小随时溢出。没有堆分配器就是寸步难行。

tag 017 的目标就是把内存管理栈的第三层搭起来——在 PMM 和 VMM 之上构建一个堆分配器（Heap Allocator），让内核代码终于可以用 `new` 和 `delete` 动态分配内存。整个 tag 涉及 13 个文件的变更，新增约 1900 行代码——其中 `heap.hpp` 和 `heap.cpp` 是核心实现，`crt_stub.cpp` 是 C++ new/delete 的重定向，两个 `test_heap.cpp` 加起来超过 1100 行测试代码。

这一章我们先解决"为什么"和"怎么设计"的问题。具体来说，我们先看清楚堆分配器在整个内存管理层级里的位置，对比几种经典的设计方案，然后讨论 Cinux 的选择和核心数据结构 BlockHeader 的设计动机。

读完本章你应该能回答这些问题：为什么不能直接用 PMM 分配所有内存？堆分配器和 PMM/VMM 是什么关系？为什么 Cinux 选择 first-fit 而不是 best-fit？BlockHeader 为什么是 32 字节？magic number 能检测哪些错误？

## 环境说明

本章涉及的新文件只有 `kernel/mm/heap.hpp`（头文件，126 行）和 `kernel/mm/heap.cpp`（实现，353 行）。工具链和运行环境与 tag 016 完全一致——GCC cross-compiler（`x86_64-elf`）、QEMU、higher-half 内核（`KERNEL_VMA = 0xFFFFFFFF80000000`）。PMM 和 VMM 必须已经初始化完毕，因为堆分配器的 init 和 expand 都会调用 `g_pmm.alloc_page()` 和 `g_vmm.map()`。

tag 017 是一个较大的里程碑——涉及 13 个文件的变更，新增约 1900 行代码（包含 1100+ 行测试代码）。但不要被数字吓到——核心实现只有 `heap.hpp` 和 `heap.cpp` 两个文件共 479 行。剩余的代码量主要来自测试文件（host 端 866 行 + QEMU 端 301 行）和 C++ new/delete 重定向（crt_stub.cpp 的 84 行变更）。

## 内存管理的三层栈：PMM -> VMM -> Heap

在讨论具体设计之前，我们先搞清楚堆分配器在 Cinux 内存架构里的位置。OSDev Wiki 对此有清晰的分层描述：底层是物理内存管理器（PMM），负责跟踪哪些物理页是空闲的，分配的单位是一整页（4096 字节）；中间是虚拟内存管理器（VMM），负责把物理页映射到进程的虚拟地址空间，操作的单位是页表项（PTE）；顶层才是堆分配器，在已映射的虚拟内存中管理任意大小的分配和释放，操作的单位是"块"（block），大小从 1 字节到几 KB 都可以。

这个层级关系不是随意决定的——每一层都依赖下一层提供的服务。PMM 不需要知道虚拟地址的存在，它只关心物理页帧号。VMM 需要 PMM 分配物理页来填充页表项，但它不关心"这块虚拟内存被切成了多小的块"。堆分配器则需要 VMM 提供连续的虚拟地址区域，然后自己在这个区域里做细粒度的切分和回收。当堆空间不足时，它再向 VMM 申请新的映射——这就形成了 PMM -> VMM -> Heap 的调用链。

你可以把它类比为城市开发的过程：PMM 是土地局，划出一块块 4096 平方米的标准地块；VMM 是市政规划，把这些地块编上地址、接入基础设施；堆分配器则是开发商，在一块已编址的地上盖不同大小的房子，有人要 50 平的公寓就给他 50 平，有人要 200 平的商铺就给他 200 平，退租了还能把空间收回来重新分配。

## 经典方案对比：从最简到生产级

堆分配器的设计空间非常大，不同的方案在实现复杂度、碎片化程度、分配延迟之间做不同的权衡。我们挑四个有代表性的方案来对比，这样你能理解 Cinux 的选择落在什么位置。

**Bump allocator（指针推进分配器）** 是最简单的方案——维护一个指针指向当前空闲区域的起点，每次分配就把指针往前推 size 个字节。分配是 O(1)，实现极其简单，但有一个致命的问题：不支持 free。一旦分配出去，空间就永远收不回来了。这在某些嵌入式场景下是可以接受的——程序运行期间从来不释放内存，或者只在最后统一释放——但对于一个需要长期运行的内核来说完全不行。FreeRTOS 的 heap_1 就是这种方案。

**Free list（空闲链表分配器）** 是 bump allocator 的自然演进——每次 free 时把释放的块挂到一个链表上，下次 alloc 时从链表里找够大的块来复用。这解决了"不能释放"的问题，但如果不做 coalescing（相邻空闲块合并），链表里会逐渐充满极小的碎片，大分配越来越难满足。FreeRTOS 的 heap_2 就是 free list 不做 coalescing 的版本——已经标记为 legacy，官方推荐用 heap_4 替代。

**Buddy allocator（伙伴分配器）** 把内存按 2 的幂次分级管理。分配时从最小的能容纳请求的级别里取一块，如果当前级别没有空闲块就从更大的级别分裂；释放时检查它的"伙伴"（同一级别的另一半）是否也空闲，如果是就合并成更大的一块。Linux 内核的物理页管理就是基于 buddy 系统的。它的优势是合并操作非常高效——只需要检查固定的一个位置，O(1) 时间。但 buddy 系统有两个缺点：第一是只能分配 2 的幂次大小的块，造成不少内部碎片（比如请求 33 字节也得给你 64 字节）；第二是数据结构相对复杂，需要维护多个级别的链表或位图。

**Slab allocator（slab 分配器）** 是在 buddy 或 free list 之上再加一层——为系统中频繁分配的固定大小对象（比如进程描述符、文件描述符、网络缓冲区）维护专用缓存。每个 slab 缓存从底层分配器拿一大块内存，切成固定大小的小对象，分配和释放都是 O(1)。Linux 在 buddy 之上提供了 SLAB、SLOB、SLUB 三种 slab 实现，其中 SLOB（Simple List of Blocks）是最简单的，本质上就是一个按大小分级的 free list。

看完这四种方案的对比，你应该对堆分配器的设计空间有了直觉——实现复杂度和碎片化程度之间存在根本性的权衡。Bump allocator 最简单但不支持 free；buddy 合并高效但只能分配 2 的幂次大小；slab 在固定大小对象上性能最优但需要知道对象大小；free list + coalescing 是通用性最好的折中。Cinux 选择的就是最后一种。

## Cinux 的选择：first-fit free list + coalescing

看完上面的对比，Cinux 的选择就很自然了。我们的目标是教学内核，实现复杂度要控制住，但功能又不能太弱。最终选了 free list + first-fit 搜索 + 块分裂 + 相邻合并这个组合——这恰好也是 FreeRTOS heap_4 的方案，是工业级 RTOS 中最常用的堆实现之一。

这个方案在所有维度上都处于"中间位置"——比 bump allocator 复杂但支持 free，比 buddy 简单但支持任意大小分配，比 slab 通用但不如它在固定对象上高效。对于教学内核来说，这个中间位置正好——既能讲清楚堆分配器的核心算法（搜索、分裂、合并、扩展），又不至于陷入 buddy 或 slab 的实现细节中。

具体来说，这个方案包含五个关键机制：first-fit 搜索（从链表头部开始找第一个够大的块）、块分裂（把大块的剩余空间切成新的空闲块）、对齐处理（用 front padding 保证 payload 地址对齐）、相邻合并（释放时把地址上相邻的空闲块拼成大块）、自动扩展（空间不够时向 VMM 申请新页面）。这五个机制是所有通用堆分配器的基础，理解了它们就能看懂 Linux 的 SLOB、FreeRTOS 的 heap_4、甚至 glibc 的 ptmalloc。

选择 first-fit 而不是 best-fit 是一个有意识的决定。best-fit 需要遍历整个链表找最小的够用块，永远是 O(n)；first-fit 找到第一个够大的就停，最好情况 O(1)。而且 best-fit 倾向于留下很多极小的碎片——恰好比请求大一丢丢的块被选中后，剩余空间往往不够再分配任何东西。Donald Knuth 在 *The Art of Computer Programming* Vol.1 Section 2.5 中给出了 first-fit 和 best-fit 的理论分析——first-fit 在大多数实际负载下表现并不比 best-fit 差，但实现更简单。first-fit + coalescing 的组合在碎片控制和搜索效率之间取得了不错的平衡。

对比 Linux 的 SLOB 分配器，Cinux 的方案是简化版。SLOB 也用 first-fit，但它维护三个独立的链表按大小分级——小请求（< 256 字节）只搜索小链表，中等请求（< 1024 字节）搜索中链表，大请求搜索大链表——这样可以减少搜索开销。Cinux 只有一个链表，对于内核堆的规模（通常几十到几百个块）完全够用。如果将来内核变复杂了，分级链表或 slab 是自然的升级方向。

对比 FreeRTOS 的 heap_4，Cinux 和它最为接近。两者都用 first-fit + coalescing，都有块 header 记录元数据，都支持释放和复用。关键区别有三点：Cinux 支持 VMM 动态扩展（heap_4 用固定大小的静态数组 `configTOTAL_HEAP_SIZE`，运行时不能增长），Cinux 有 magic number 做运行时校验（heap_4 用链表尾部的 tail marker），Cinux 支持任意对齐（heap_4 不支持）。FreeRTOS 多了线程安全保护——Cinux 目前是单线程的。

## BlockHeader：每一块内存的"身份证"

设计确定了，接下来是核心数据结构。每个堆块前面需要放一个元数据头，记录这块的大小、是否空闲、以及在空闲链表中的位置。Cinux 把这个头叫做 `BlockHeader`。

```cpp
struct [[gnu::packed]] BlockHeader {
    uint32_t magic;       // 校验魔数 0xDEADBEEF
    uint32_t size;        // payload 大小（不含 header 自身）
    uint32_t free;        // 1 = 空闲, 0 = 已分配
    uint8_t  _pad[12];    // 填充到 32 字节
    BlockHeader* next;    // 空闲链表后继指针
};

static_assert(sizeof(BlockHeader) == 32, "BlockHeader must be 32 bytes");
```

我们来逐字段拆解，理解每一个设计决策。

### magic：运行时校验的第一道防线

`magic` 是一个 32 位的魔数常量，在 `heap.cpp` 中被定义为 `0xDEADBEEF`。它的作用是运行时校验——每次 `free()` 被调用时，我们会检查传入指针前面的 BlockHeader 的 magic 是否正确。如果不对，要么是指针本身有问题，要么是内存被踩了。double-free 的场景也能被部分捕获：第一次 free 之后 `free` 标志被置为 1，第二次 free 时如果看到 `free == 1`，就知道这是重复释放。

### size 和 free：块的元数据

`size` 记录的是 payload 的大小，单位是字节，不包括 BlockHeader 自身。如果你调用 `alloc(64)`，那么对应的 BlockHeader 的 `size` 字段就是 64。这个字段用 `uint32_t` 而不是 `uint64_t` 是一个刻意的选择——单个堆块的最大 payload 被限制在约 4 GB，对于内核堆来说绑绑有余，而且省下了 4 个字节的头部开销。`free` 字段其实是一个布尔语义的标志位，这里用 `uint32_t` 而不是 `bool` 是因为我们需要精确控制结构体的内存布局——`bool` 在不同编译器上的大小可能不同。

### _pad[12]：预留空间

`_pad[12]` 是 12 字节的填充数组，唯一目的是把整个 BlockHeader 撑到 32 字节。我们来算这笔账：`magic` 4 + `size` 4 + `free` 4 + `_pad` 12 + `next` 8（x86-64 上指针 8 字节）= 32。

为什么要凑 32 字节？首先是地址计算的简便性——32 是 2 的幂，`payload = header + 32` 和 `header = payload - 32` 在生成的机器码里只是一个加减法。其次是缓存友好性——x86-64 的 L1 缓存行是 64 字节，32 字节的 header 恰好占半行，一个小的 payload 可以和 header 共享一行缓存。第三是为将来留了扩展空间——`_pad[12]` 提供了 12 字节的预留位，如果以后需要加入分配来源的文件名哈希或 CRC 校验值，直接征用就行。

`[[gnu::packed]]` 属性确保编译器不在字段之间插入对齐填充。默认情况下，GCC 可能在 `free`（4 字节）和 `next`（8 字节指针）之间插入 4 字节的填充使 `next` 对齐到 8 的倍数——但这会让整个结构体变成 36 字节。`packed` 消除了这种不确定性，`static_assert` 是编译期安全网：任何人修改了字段导致大小不再是 32，编译直接报错。

在 x86-64 上，`[[gnu::packed]]` 不会引起对齐 fault——x86 硬件支持未对齐访问。但在 ARM 等架构上，未对齐访问会触发异常。Cinux 只运行在 x86-64 上，所以不需要担心这个问题。

`next` 指针是空闲链表的"线"——所有空闲块通过这个字段串成一个单链表。已分配的块不使用这个字段（设为 `nullptr`），因为分配出去的块对分配器来说就是"不存在"的，只有释放时才重新挂回链表。

这种设计意味着空闲链表只追踪空闲块，不追踪已分配的块。你不能遍历"所有块"——因为已分配的块没有被串联起来。如果你想做堆完整性检查（扫描所有块确认没有损坏），就需要从堆的起始地址开始线性扫描，通过每个块的 size 字段跳到下一个块。

### 为什么 BlockHeader 放在 payload 前面

如果 header 放在后面，free 时你需要知道 payload 的大小才能定位 header——但大小存在 header 里，形成了循环依赖。前面放置让 `free(ptr)` 只需做 `ptr - 32` 即可定位 header。另一个好处是 header 和 payload 在内存中连续，对 CPU 预取器更友好。

## Magic number 的调试价值

`0xDEADBEEF` 这个经典魔数几乎是操作系统教程里的标配了——从 MIT 的 xv6 到各种 hobby OS 教程都在用。它足够醒目，在内存转储中一眼就能认出来。

每次 `free()` 被调用时，我们从 payload 指针回退 32 字节定位 BlockHeader，然后检查 magic 是否等于 `0xDEADBEEF`。如果不对，要么是指针本身有问题（野指针或者偏移错误），要么是内存被踩了——相邻块的 buffer overflow 把 header 的 magic 覆盖成了垃圾值。double-free 也能被捕获：第一次 free 之后 header 的 `free` 标志被置为 1，第二次 free 时如果看到 `free == 1`，就知道这是重复释放。

你可能会觉得这种检查太简陋了——确实，它比不上 AddressSanitizer（ASan）那种编译器级别的完整内存错误检测。但 ASan 依赖编译器插桩，内核环境下配置复杂，运行时开销也不小。更重量级的方案还包括给 payload 前后加 canary 值（检测 buffer overflow）、维护已分配块的哈希表（在 free 时做指针合法性查找）——这些对教学 OS 的初始实现来说偏重了。magic number 的优势在于零额外内存开销、O(1) 的校验速度，以及能捕获最常见的两类错误。

对比 FreeRTOS 的 heap_4，它不用 magic number，而是在空闲链表尾部放一个特殊的 tail marker 块来做边界校验。Cinux 选择 magic number 是因为它更直观——在 QEMU 的内存转储中 `0xDEADBEEF` 一眼就能认出来，调试的时候非常方便。

magic number 最大的局限性是它不能检测 buffer overflow——如果一个块写越界了，覆写的是下一个块的 header（包括 magic），而不是自己的 header。当 free 下一个块时才会发现 magic 被踩了，但这时候已经不知道是"谁"踩的了。更完整的方案是给 payload 的最后 8 字节加一个 canary 值，在 free 时检查。但对于教学 OS 的初始实现来说，magic number 在"简单"和"有用"之间是一个不错的平衡点。

magic number 的另一个用途是诊断"野指针"——如果你 free 了一个栈变量的地址，`header_from_ptr` 回退 32 字节后读到的值几乎不可能是 `0xDEADBEEF`。这种检测不是 100% 可靠（理论上栈上可能恰好有这个值），但在实践中极少出现误报。它给了你一个"很可能是野指针"的信号，比什么都不检查要好得多。

## Heap 类的全貌

有了 BlockHeader，我们还需要一个"控制中心"来管理整个堆区域。`Heap` 类的接口非常精简——公开只有四个方法，私有只有两个辅助方法和四个成员变量：

```cpp
class Heap {
public:
    void init(uint64_t virt_base, uint64_t initial_size);
    void* alloc(size_t size, size_t align = 16);
    void free(void* ptr);
    void dump_stats() const;

private:
    void expand(size_t min_bytes);
    void coalesce(BlockHeader* block);

    uint64_t      base_{};       // 堆区域起始虚拟地址
    uint64_t      size_{};       // 堆区域总大小
    uint64_t      used_{};       // 已分配字节数（含 header）
    BlockHeader*  free_list_{};  // 空闲链表头
};
```

让我们逐个过一遍。

```cpp
class Heap {
public:
    void init(uint64_t virt_base, uint64_t initial_size);
    void* alloc(size_t size, size_t align = 16);
    void free(void* ptr);
    void dump_stats() const;

private:
    void expand(size_t min_bytes);
    void coalesce(BlockHeader* block);

    uint64_t      base_{};       // 堆区域起始虚拟地址
    uint64_t      size_{};       // 堆区域总大小
    uint64_t      used_{};       // 已分配字节数（含 header）
    BlockHeader*  free_list_{};  // 空闲链表头
};
```

`init` 接收起始虚拟地址和初始大小，通过 PMM 分配物理页、VMM 建立映射，然后创建第一个覆盖整个区域的空闲块。`alloc` 用 first-fit 搜索空闲链表，找到够大的块就分裂并返回 payload 指针；找不到就调用 `expand` 自动扩展，然后递归重试。`free` 校验 magic、标记为空闲、挂回链表、调用 `coalesce` 合并相邻空闲块。`dump_stats` 是调试辅助，遍历链表统计信息。

四个私有成员中，`base_` 和 `size_` 描述堆区域的边界——`base_` 是起始虚拟地址，`size_` 是当前总大小（含已分配和空闲），堆在虚拟地址空间中从 `base_` 到 `base_ + size_` 连续增长。`used_` 跟踪已分配量（包括 header），`free_list_` 指向空闲链表的第一个节点。全部用 `{}` 值初始化，保证全局 `g_heap` 在 `init()` 调用前处于安全的零状态。

全局唯一的 `g_heap` 实例定义在 `heap.cpp` 中，通过 `extern` 声明暴露给其他编译单元。它和 PMM 的 `g_pmm`、VMM 的 `g_vmm` 保持一致的命名风格——三个对象构成了 Cinux 内存管理从底层到高层的完整栈：PMM 管物理页，VMM 管虚拟映射，Heap 管任意大小的动态分配。内核的 `operator new` / `operator delete` 重定向就是通过 `cinux::mm::g_heap.alloc()` 和 `cinux::mm::g_heap.free()` 来实现的。

这种"单例"模式在内核中非常普遍。Linux 内核的 `kmalloc` 本质上也是全局可用的分配接口，FreeRTOS 用一个静态数组 `ucHeap[configTOTAL_HEAP_SIZE]` 作为堆空间。Cinux 的做法比 FreeRTOS 更灵活——堆区域不是静态数组，而是通过 VMM 动态映射的虚拟地址空间，可以在运行时通过 expand 自动增长。

注意 `used_` 的定义——它包含了 header 的大小。分配 64 字节 payload 时 `used_` 增加 96（32 header + 64 payload），不是 64。这使得 `total - used` 不等于"空闲链表中的总空间"，因为空闲链表中的块也有 header 开销。理解这个定义对于解读 `dump_stats` 的输出很重要。

私有方法 `expand(min_bytes)` 在空闲链表耗尽时被调用，通过 PMM 分配新的物理页、VMM 建立映射。`coalesce(block)` 在 free 时被调用，把刚释放的块和地址上相邻的空闲块合并成一个更大的块。这两个方法是堆分配器"回收"和"扩容"机制的核心，我们将在第三篇文章中详细讲解。

## 收尾

到这一步，我们已经讲清楚了堆分配器在整个内存管理层级中的位置、经典方案的取舍、以及 Cinux 的设计选择和核心数据结构。BlockHeader 的 32 字节布局和 `0xDEADBEEF` 魔数是后续所有算法的基础——alloc、free、coalesce、expand 都在这个数据结构上运作。整个 Heap 类的公开接口只有四个方法，但它们背后牵扯出 first-fit 搜索、块分裂、相邻合并、对齐计算、自动扩展等一系列精巧的机制。

tag 017 是 Cinux 内存管理栈的最后一块拼图。从 tag 015 的 PMM（物理页管理），到 tag 016 的 VMM（虚拟映射），再到 tag 017 的 Heap（任意大小分配），三层架构完整了。有了堆分配器，内核代码终于可以用 `new` 和 `delete` 动态分配内存——这在后续的文件系统、网络栈、进程管理中都是不可或缺的基础设施。

## 参考资料

- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) -- 内核堆分配器的总体架构描述，分层模型（PMM -> VMM -> Heap）的来源，free list 和 coalescing 的基本原理。
- dreamportdev/Osdev-Notes: [Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md) -- 从 bump allocator 到 free list 到 coalescing 到扩展的渐进式教程，和 Cinux 的设计路径高度吻合。
- FreeRTOS heap_4 文档: [FreeRTOS Memory Management](https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/09-Memory-management/01-Memory-management) -- heap_4 的 first-fit + coalescing 设计说明，以及五种堆实现的对比。这个页面同时包含了 heap_1 到 heap_5 的对比表格，是理解不同堆策略权衡的好资源。
- xv6 RISC-V `kalloc.c`: [GitHub](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kalloc.c) -- 页级分配器，对比 Cinux 可理解 sub-page 分配器的必要性。
- xv6 RISC-V `kalloc.c`: [GitHub](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/kalloc.c) -- 页级分配器，对比 Cinux 可理解 sub-page 分配器的必要性。xv6 用填充 junk 值的方式做基本的调试支持。
- Linux SLOB 分配器 `mm/slob.c`: 基于 first-fit 的三链表分离设计，是最简单的 Linux 内核分配器。
- Donald Knuth, *The Art of Computer Programming* Vol.1 Section 2.5: first-fit / best-fit 策略的理论分析和碎片化论证。Knuth 的数学论证解释了为什么 first-fit 在大多数实际负载下表现并不比 best-fit 差。
- Linux SLUB 分配器 `mm/slub.c`: Linux 默认的 slab 实现，比 SLOB 更复杂但性能更好。对比 Cinux 可以理解 slab 层的对象复用策略。
- GCC Manual: [Common Variable Attributes](https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html) -- `[[gnu::packed]]` 属性的官方文档。

## 下一步

但数据结构只是骨架，算法才是灵魂。下一章我们深入 `Heap::init()` 和 `Heap::alloc()` 的实现细节，看看堆是怎么从一块空白虚拟内存变成可用的、first-fit 搜索在空闲链表上是怎么走的、块分裂的阈值是怎么定的、以及面对对齐要求时那个"front padding"的技巧是怎么处理的。

第三篇将覆盖 `free()`、`coalesce()` 和 `expand()`——回收、合并和扩展的完整实现。第四篇则是 C++ new/delete 的重定向和双层测试策略。四篇文章完整覆盖 tag 017 的全部内容。

如果你迫不及待想看到完整的代码，可以直接阅读 `kernel/mm/heap.hpp` 和 `kernel/mm/heap.cpp`。头文件只有 126 行，实现文件 353 行——加起来不到 500 行代码就实现了一个功能完备的堆分配器。这体现了 first-fit free list + coalescing 方案的核心优势：简单但够用。

在继续阅读之前，建议你先思考一个问题：如果 alloc 找不到足够大的空闲块，应该怎么办？你可以选择返回错误（让调用者处理），也可以选择自动扩展堆区域（从 VMM 申请更多空间）。Cinux 选择了后者——让分配器对调用者完全透明，调用者不需要知道"堆空间不够了"这件事。但这个选择带来了一个有趣的递归结构：alloc 调用 expand，expand 调用 VMM::map，VMM::map 又调用 PMM::alloc_page。如果你在阅读代码时跟踪这个调用链，会发现"分配内存"这个看似简单的操作，实际上穿越了整个内存管理栈的三层。
