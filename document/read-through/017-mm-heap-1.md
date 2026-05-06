# 017-1 通读：BlockHeader 与 Heap 类定义——堆分配器的骨架

## 概览

本文是 tag `017_mm_heap` 四篇通读教程的第一篇，聚焦于 `kernel/mm/heap.hpp` 这一整个头文件。在整个堆分配器的架构中，这个头文件承担的角色是"骨架"——它定义了每一个堆块前面的元数据头（`BlockHeader`），声明了对外暴露的分配/释放接口（`Heap` 类），并且通过 `static_assert` 在编译期锁定了一个关键不变量：BlockHeader 恰好 32 字节。理解了这个头文件，后面读 `heap.cpp` 的实现代码就会顺畅得多，因为所有的算法逻辑都是在这些数据结构之上运作的。

## 架构图

```
    虚拟地址空间 (堆区域)
    ┌─────────────────────────────────────────────────────────┐
    │ 0xFFFF800000000000                                       │
    │ ┌──────────┬──────────────────────┐                      │
    │ │BlockHeader│     Payload          │  ... 更多块 ...     │
    │ │ 32 bytes  │  (size 字节)         │                      │
    │ └──────────┴──────────────────────┘                      │
    │                                                          │
    │  BlockHeader 字段:                                       │
    │  ┌───────┬──────┬──────┬──────────┬──────────┐          │
    │  │ magic │ size │ free │ _pad[12] │   next   │          │
    │  │ 4B    │ 4B   │ 4B   │  12B     │   8B     │          │
    │  └───────┴──────┴──────┴──────────┴──────────┘          │
    │  合计 = 32 bytes                                          │
    └─────────────────────────────────────────────────────────┘

    Heap 类内部状态:
    ┌────────────────────────────────────────┐
    │  base_      ──→ 堆区域起始虚拟地址       │
    │  size_      ──→ 堆区域总大小            │
    │  used_      ──→ 已分配字节数(含 header) │
    │  free_list_ ──→ 空闲块链表头            │
    └────────────────────────────────────────┘

    g_heap ── 全局 Heap 实例 (cinux::mm 命名空间)
```

## 代码精讲

### 头文件开场与命名空间

```cpp
/**
 * @file kernel/mm/heap.hpp
 * @brief Kernel heap allocator with first-fit, splitting, and coalescing
 *
 * Provides a linked-list-based heap allocator for dynamic memory allocation
 * inside the kernel.  Each block is preceded by a BlockHeader containing a
 * magic number for corruption detection.  Allocation uses a first-fit
 * strategy with block splitting, and freeing coalesces with adjacent free
 * blocks.  When the free list is exhausted, the heap is expanded
 * automatically via VMM::map().
 *
 * The global operator new / delete are redirected to Heap::alloc / free
 * so that C++ code can use the standard allocation syntax.
 *
 * Namespace: cinux::mm
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cinux::mm {
```

头文件的注释把整个设计思路概括得相当到位——first-fit 搜索、块分裂、相邻合并、VMM 自动扩展、C++ new/delete 重定向。这五个特性构成了 Cinux 堆分配器的完整功能清单，也是后面三篇文章逐个拆解的目标。`pragma once` 做包含保护，`stddef.h` 提供 `size_t`，`stdint.h` 提供定宽整数类型——这是内核头文件的标准开场白，没有多余依赖。

### BlockHeader——每个堆块的"身份证"

```cpp
struct [[gnu::packed]] BlockHeader {
    uint32_t magic;
    uint32_t size;       ///< Payload size (bytes, excluding this header)
    uint32_t free;       ///< 1 = free, 0 = in use
    uint8_t  _pad[12];   ///< Padding to reach 32 bytes total
    BlockHeader* next;   ///< Next block in the free list
};

static_assert(sizeof(BlockHeader) == 32, "BlockHeader must be 32 bytes");
```

我们先来逐字段拆解这个结构体。

`magic` 是一个 32 位的魔数常量，在 `heap.cpp` 中被定义为 `0xDEADBEEF`。它的作用是运行时校验——每次 `free()` 被调用时，我们会检查传入指针前面的 BlockHeader 的 magic 是否等于 `0xDEADBEEF`。如果不等于，要么是指针本身有问题（野指针或者偏移错误），要么是内存被踩了。double-free 的场景也能被部分捕获：第一次 free 之后，magic 仍然保持原值，但 `free` 标志被置为 1；第二次 free 时如果看到 `free == 1`，就知道这是重复释放。说实话，`0xDEADBEEF` 这个经典魔数几乎是操作系统教程里的标配了，从 MIT 的 xv6 到各种 hobby OS 教程都在用——它足够醒目，在内存转储中一眼就能认出来。

`size` 记录的是 payload 的大小，单位是字节，不包括 BlockHeader 自身。也就是说，如果你调用 `alloc(64)`，那么对应的 BlockHeader 的 `size` 字段就是 64。这个字段用 `uint32_t` 而不是 `uint64_t` 是一个刻意的选择——单个堆块的最大 payload 被限制在约 4 GB，对于内核堆来说绰绰有余，而且省下了 4 个字节的头部开销。`free` 字段其实是一个布尔语义的标志位：1 表示空闲，0 表示已分配。这里用 `uint32_t` 而不是 `bool` 是因为我们需要精确控制结构体的内存布局——`bool` 在不同编译器上的大小可能不同（虽然 GCC 几乎总是 1 字节），而 `uint32_t` 永远是 4 字节。

接下来是 `_pad[12]`，12 字节的填充数组。它的唯一目的是把整个 BlockHeader 撑到 32 字节。我们等一下来算这笔账：`magic` 4 字节 + `size` 4 字节 + `free` 4 字节 + `_pad` 12 字节 + `next` 指针 8 字节（x86-64 上指针是 8 字节）= 32 字节。为什么要凑 32 字节？因为 32 是一个不错的对齐数——既满足 SIMD 操作的 16 字节对齐要求，又不会浪费太多空间。更重要的是，固定的 header 大小使得地址计算变得简单：payload 地址 = header 地址 + 32，header 地址 = payload 地址 - 32。

`next` 指针是空闲链表的"线"——所有空闲块通过这个字段串成一个单链表。已分配的块不使用这个字段（设为 `nullptr`），因为分配出去的块对分配器来说就是"不存在"的，只有释放时才重新挂回链表。

`[[gnu::packed]]` 属性告诉 GCC 不要在结构体字段之间插入填充字节。在默认情况下，编译器可能会在 `free`（4 字节）和 `next`（8 字节指针）之间插入 4 字节的对齐填充，使得 `next` 从 8 的倍数地址开始。但我们不希望这样——我们需要精确控制每个字段的偏移量，确保 BlockHeader 的大小是可预测的。`static_assert(sizeof(BlockHeader) == 32)` 就是一道安全网：如果有人修改了字段类型或数量导致大小不再是 32，编译直接报错。

### Heap 类——分配器的"控制中心"

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

    uint64_t      base_{};
    uint64_t      size_{};
    uint64_t      used_{};
    BlockHeader*  free_list_{};
};
```

公开接口只有四个方法，我们逐个过一遍。

`init(virt_base, initial_size)` 负责初始化堆区域。它接收一个起始虚拟地址和一个初始大小（字节），然后通过 PMM 分配物理页、通过 VMM 建立映射，最后在映射好的虚拟内存中创建第一个覆盖整个区域的空闲块。在 `kernel/main.cpp` 中，我们用 `0xFFFF800000000000` 作为起始地址、64 KB 作为初始大小来调用它——这个地址选在内核高半区（higher-half canonical address），不会和用户空间地址冲突。

`alloc(size, align = 16)` 是分配接口，返回一个 `void*` 指向 payload。默认对齐是 16 字节，但调用者可以指定更大的对齐值（比如 4096 用于页对齐分配）。它的工作流程是：遍历空闲链表找到第一个够大的块（first-fit），处理好对齐和块分裂，然后把 payload 指针返回给调用者。如果遍历完了还找不到合适的块，它会调用 `expand()` 从 VMM 申请新页面，然后递归重试。

`free(ptr)` 是释放接口。它首先做安全检查——验证 magic 是否正确、是否已经是 free 状态——然后标记为 free、挂回空闲链表、调用 `coalesce()` 尝试和相邻空闲块合并。

`dump_stats()` 是一个调试辅助函数，遍历空闲链表统计总空闲字节数和块数，然后通过 kprintf 输出。在调试碎片化问题或者验证分配器正确性时非常有用。

私有接口有两个。`expand(min_bytes)` 在空闲链表耗尽时被调用，通过 PMM 分配新的物理页、VMM 建立映射、创建新的空闲块，然后把新块挂到链表上。`coalesce(block)` 在 free 时被调用，负责把刚释放的块和它在地址上相邻的空闲块合并成一个更大的块。

四个私有成员变量中，`base_` 记录堆区域的起始虚拟地址，`size_` 记录当前堆的总大小（含已分配和空闲），`used_` 跟踪已分配的字节数（包括 header），`free_list_` 指向空闲链表的第一个节点。所有成员都用 `{}` 进行了值初始化（即整数归零、指针归空），确保全局 `g_heap` 对象在 `init()` 被调用之前处于安全的零状态。

一个值得注意的设计细节是 `used_` 的定义——它包含了 header 的大小。也就是说，如果分配了 64 字节的 payload，`used_` 增加 96（32 header + 64 payload），而不是 64。这使得 `total - used` 不等于"空闲链表中的总空间"，因为空闲链表中的块也有 header 开销。理解这个定义对于解读 `dump_stats` 的输出很重要。

### 全局实例 g_heap

```cpp
/// Global Heap instance.
extern Heap g_heap;

}  // namespace cinux::mm
```

`g_heap` 是全局唯一的 Heap 实例，定义在 `heap.cpp` 中，通过 `extern` 声明暴露给其他编译单元。内核的 C++ `operator new` / `operator delete` 重定向就是通过 `cinux::mm::g_heap.alloc()` 和 `cinux::mm::g_heap.free()` 来实现的。把它放在 `cinux::mm` 命名空间下是为了和 PMM 的 `g_pmm`、VMM 的 `g_vmm` 保持一致的命名风格——这三个全局对象构成了 Cinux 内存管理从底层到高层的完整栈：PMM 管物理页，VMM 管虚拟映射，Heap 管任意大小的动态分配。

这种"单例"模式在内核中非常普遍。Linux 内核的 `kmalloc` 本质上也是全局可用的分配接口，FreeRTOS 用一个静态数组 `ucHeap[configTOTAL_HEAP_SIZE]` 作为堆空间。Cinux 的做法比 FreeRTOS 更灵活——堆区域不是静态数组，而是通过 VMM 动态映射的虚拟地址空间，可以在运行时通过 expand 自动增长。这意味着内核堆的大小不受编译期常量限制，而是取决于物理内存的可用量。

## 设计决策

### Decision: 单一全局堆实例而非多堆

**问题**: 堆分配器应该有一个全局实例还是允许多个独立实例？

**本项目的做法**: 单一全局实例 `cinux::mm::g_heap`。

**备选方案**: 允许创建多个 Heap 实例，每个管理不同的虚拟地址区域。比如网络栈有自己的堆，驱动有自己的堆。

**为什么不选备选方案**: 多堆实例增加了管理复杂度——operator new/delete 只能重定向到一个堆（除非用线程局部存储或 arena 参数），多个堆需要手动选择使用哪个。对于 Cinux 当前的单核内核来说，单堆实例足够。如果将来需要隔离（比如防止某个子系统的内存泄漏影响其他子系统），可以在堆之上实现 arena 分配，而不是创建多个 Heap 对象。

**如果要扩展/改进**: 在 Heap 类中加入 arena 参数，alloc 时指定从哪个 arena 分配。或者像 Linux 的 `kmalloc` 那样用不同的 flag 标记分配来源。

### Decision: BlockHeader 固定 32 字节，而非变长头部

**问题**: 每个堆块前面需要一个元数据头来记录大小、空闲状态、链表指针等信息。这个头部应该多大？

**本项目的做法**: 用 `[[gnu::packed]]` 加 `_pad[12]` 把 BlockHeader 固定为恰好 32 字节，并用 `static_assert` 在编译期锁定。

**备选方案**: 一种更紧凑的做法是去掉 `_pad`，让头部只有 20 字节（4+4+4+8），或者在 32 位系统上甚至只有 12 字节（4+4+4，指针 4 字节）。很多教学 OS 正是这样做的。

**为什么不选备选方案**: 32 字节的头部虽然比最小可能值多出 12 字节，但带来了两个好处。首先是地址计算的简便性——32 是 2 的幂，`payload = header + 32` 和 `header = payload - 32` 在生成的机器码中只是一个简单的加法或减法，不需要乘法或移位。其次是 32 字节对齐意味着 BlockHeader 本身总是从 32 的倍数地址开始，这对 CPU 的缓存行利用率更友好——x86-64 的 L1 缓存行是 64 字节，一个 32 字节的 header 恰好占半行，一个小的 payload（比如 32 字节以内）可以和 header 共享一行缓存。

**如果要扩展/改进**: 如果未来需要在 BlockHeader 中加入更多元数据（比如分配来源的文件名/行号用于调试、时间戳用于生命周期分析、引用计数用于共享内存），`_pad[12]` 提供的 12 字节空间可以直接征用，不需要改变结构体大小。

### Decision: 用 magic number 做运行时校验，而非更重的调试方案

**问题**: 堆分配器是最容易出现 use-after-free、double-free、buffer-overflow 等内存错误的组件。如何在不显著增加运行时开销的前提下提供基本的防护？

**本项目的做法**: 在 BlockHeader 中加入 `magic = 0xDEADBEEF`，在 `free()` 时校验。double-free 通过检查 `free` 标志位来检测。野指针通过 magic 不匹配来暴露。

**备选方案**: 更重量级的方案包括：给每个 payload 的前后各加一个 canary（哨兵值），检测 buffer overflow；维护一个已分配块的哈希表，在 free 时做指针合法性查找；用 ASan (AddressSanitizer) 做完整的内存错误检测。

**为什么不选备选方案**: canary 和哈希表的开销对于教学 OS 的初始实现来说偏重。ASan 依赖编译器插桩，内核环境下的支持比较复杂。magic number 的优势在于零额外内存开销（只有一个字段）、O(1) 的校验速度，以及能捕获最常见的两类错误（double-free 和明显的野指针）。它当然不能捕获所有错误——比如 buffer underflow 不会破坏 header，但这已经是一个在"简单"和"有用"之间不错的平衡点。

**如果要扩展/改进**: 可以引入 `#ifdef HEAP_DEBUG` 条件编译，在调试模式下给每个 payload 的最后 8 字节写入一个尾部 canary（比如 `0xCAFEBABE`），在 free 时检查 canary 是否被改写。这种方案的开销是每个分配多占 8 字节，但能检测到绝大多数 buffer overflow。

## 扩展方向

1. **在 `_pad` 中嵌入分配来源信息** (⭐⭐): 12 字节的 padding 足以存放一个 `uint32_t` 的分配 ID 或者源文件哈希值。在 `alloc` 时写入，在 `dump_stats` 或 free 失败时输出，可以帮助追踪"谁分配了这个块"。

2. **将 free 标志从 uint32_t 改为 enum class** (⭐): 定义 `enum class BlockState : uint32_t { Free = 1, Used = 0 }`，替换裸 `uint32_t free`。这不会改变内存布局，但会让代码的意图更清晰，也防止误写 `free = 2` 这样的非法值。

3. **实现 BlockHeader 的 CRC 校验** (⭐⭐⭐): 在 header 的最后 4 字节（从 `_pad` 中划出）存储整个 header 的 CRC32。每次操作 header 时重新计算并比对，能检测到更隐蔽的内存踩踏。

4. **多堆实例支持** (⭐): 当前只有一个全局 `g_heap`。如果未来需要为不同子系统提供独立的堆（比如网络栈有自己的堆，驱动有自己的堆），可以研究如何将 Heap 类改为可实例化的设计，每个实例管理不同的虚拟地址区域。

5. **lockdep 式的 free list 遍历验证** (⭐⭐): 在 `dump_stats` 中加入 free list 的完整性验证：检查是否有环（记录遍历步数，超过总块数就说明有环）、检查所有空闲块的 magic 是否正确、检查所有空闲块的地址范围是否在 `[base_, base_ + size_)` 内。这在调试链表损坏时非常有用。

6. **堆收缩（heap shrink）** (⭐⭐): 当空闲链表中有大量尾部空闲空间时，释放末尾的物理页回 PMM。这需要确保被释放的页上没有已分配的块——实现难度比扩展大得多。

## 参考资料

- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) — 内核堆分配器的总体设计思路，包括 free list、块分裂、合并的概述。
- dreamportdev/Osdev-Notes: [Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md) — 从 bump allocator 到 free list 到合并再到扩展的渐进式教程，和 Cinux 的设计路径高度吻合。
- FreeRTOS `heap_4.c` 源码: 和 Cinux 最接近的工业级实现。对比 FreeRTOS 的 `BlockLink_t` 和 Cinux 的 `BlockHeader`，会发现结构几乎一样，区别在于 FreeRTOS 用链表尾部的 tail marker 而非 magic number 做校验。
- Linux 内核 SLOB 分配器 (`mm/slob.c`): 最简单的 Linux 内核分配器，也是基于 first-fit free list，但用了三个独立的链表（小/中/大）来减少搜索时间。
