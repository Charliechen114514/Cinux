# 初始化与分配：first-fit 算法详解

> 标签：Heap::init, Heap::alloc, first-fit, 块分裂, 对齐, front padding
> 前置：[017-1 从页到字节：为什么内核需要堆分配器](017-mm-heap-1.md)

## 前言

上一篇我们搭好了理论框架——搞清楚了堆分配器在内存管理栈里的位置、对比了经典方案的取舍、设计了 BlockHeader 数据结构。现在该动手写代码了。本章覆盖 `Heap::init()` 和 `Heap::alloc()` 两个函数——一个是把一块空白虚拟内存变成可用堆的初始化过程，另一个是整个分配器最复杂也最有趣的部分：从空闲链表中切出一块刚好够用的内存交给调用者。

说实话，`alloc()` 是我在 Cinux 里写过的最"精巧"的函数之一。它要处理 first-fit 搜索、对齐计算、front padding、块分裂——这些需求互相纠缠，稍不留神就会出一个"只在 4096 对齐时才触发的 off-by-one"bug。我们一步一步来，按照代码的执行顺序逐段拆解。

## 环境说明

本章的核心代码在 `kernel/mm/heap.cpp` 的前半部分（约 1-180 行），包括常量定义、全局实例、三个内部辅助函数、`init()` 和 `alloc()`。运行环境要求 PMM 和 VMM 已经初始化完毕——在 `kernel/main.cpp` 的启动序列中，Heap init 被精确地放在 VMM init 之后（Step 9）。堆的起始虚拟地址是 `0xFFFF800000000000`，初始大小 64 KB（16 个物理页）。工具链不变——GCC cross-compiler、QEMU、higher-half 内核。

## 常量与辅助函数

### 五个编译期常量

`heap.cpp` 开头定义了五个控制分配器行为的常量：

```cpp
constexpr uint32_t HEAP_MAGIC    = 0xDEADBEEF;
constexpr uint32_t HEADER_SIZE   = sizeof(BlockHeader);  // 32
constexpr uint32_t MIN_SPLIT     = HEADER_SIZE + 16;     // 48
constexpr uint64_t EXPAND_PAGES  = 4;                     // 16 KB
constexpr uint64_t PAGE_FLAGS    = 0x03;                  // present + writable
```

`HEAP_MAGIC` 是 BlockHeader 的校验值。`HEADER_SIZE = 32` 由 static_assert 保证。`MIN_SPLIT = 48` 是块分裂的最小阈值——header 32 + payload 16，保证分裂出来的块至少能容纳最小有用的分配。设为 32（只有 header 没有 payload）会产生永远无法满足任何 alloc 的"僵尸块"，16 字节的 payload 下限保证了实用性。`EXPAND_PAGES = 4` 表示每次扩展至少 4 页（16 KB），平衡扩展频率和内存浪费。`PAGE_FLAGS = 0x03` 即 Present + Writable。

`g_heap` 全局对象会被自动零初始化，在 `init()` 调用前处于安全空状态——所有指针为 nullptr，所有计数器为 0。

### 三个内部辅助函数

三个辅助函数放在匿名命名空间中，只在 `heap.cpp` 内可见。`align_up` 是经典的向上对齐计算 `(value + align - 1) & ~(align - 1)`，全是位运算不需要除法。`header_from_ptr` 是 free 中最关键的地址计算——给定 payload 指针，回退 32 字节定位 BlockHeader。不管分配时 header 实际放在哪里（可能有 front padding），这个函数永远从 payload 回退 HEADER_SIZE，所以 free 只需用户传 payload 指针。`memzero` 是手写的清零循环——内核没有标准库的 memset，所以只能自己来。

## 初始化：从空白到可用

### 选址与映射

`Heap::init()` 接收两个参数：起始虚拟地址和初始大小。在 `main.cpp` 中我们选择了 `0xFFFF800000000000` 作为堆的基地址。这个地址是 x86-64 高半区（higher-half canonical）的起点——bit 63 到 bit 48 全是 1，bit 47 为 1 标志着内核空间。它远高于内核代码段的映射区域（`0xFFFFFFFF80000000` 附近），不会和内核代码冲突，又留出了足够大的增长空间——从 `0xFFFF800000000000` 到 `0xFFFFFFFF80000000` 之间有大约 128 TB 的虚拟地址空间。

```cpp
void Heap::init(uint64_t virt_base, uint64_t initial_size) {
    uint64_t aligned_size = align_up(initial_size, cinux::arch::PAGE_SIZE);

    for (uint64_t offset = 0; offset < aligned_size; offset += cinux::arch::PAGE_SIZE) {
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) {
            cinux::lib::kprintf("[HEAP] OOM during init at offset %u\n", offset);
            return;
        }
        g_vmm.map(virt_base + offset, phys, PAGE_FLAGS);
    }
```

第一步是把 `initial_size` 向上对齐到页大小（64 KB 已经是页对齐的，所以这里不变）。然后逐页分配物理内存并建立映射——64 KB 对应 16 次循环，每次调用 PMM 拿一个物理页，再调 VMM 把它映射到对应的虚拟地址。`alloc_page()` 返回 0 时打印 OOM 并返回，堆处于未完全初始化的状态。

你可能注意到了 `PAGE_FLAGS = 0x03`，即 Present + Writable。堆内存必须可写（否则写 payload 会触发 page fault），不需要 User 位（堆在内核空间，Ring 0 访问），也不需要 NX 位（内核代码本身就在堆区域里没有可执行的需求——至少现在没有）。

### 创建初始空闲块

映射完成后，整个 64 KB 区域就是一块干净的虚拟内存了。接下来把它初始化为一个巨大的空闲块：

```cpp
    memzero(reinterpret_cast<void*>(virt_base), static_cast<size_t>(aligned_size));

    auto* first = reinterpret_cast<BlockHeader*>(virt_base);
    first->magic = HEAP_MAGIC;
    first->size  = static_cast<uint32_t>(aligned_size - HEADER_SIZE);
    first->free  = 1;
    first->next  = nullptr;

    base_      = virt_base;
    size_      = aligned_size;
    used_      = 0;
    free_list_ = first;
```

`memzero` 把整个区域清零——PMM 分配的物理页可能包含旧数据（之前被其他子系统用过的物理页），而 `alloc` 承诺返回清零的内存，所以初始时必须先清理干净。然后在区域起始处创建第一个 BlockHeader，payload 大小设为 `aligned_size - HEADER_SIZE`——扣掉 32 字节的 header，剩下全部是可用空间。整个堆初始为"一个大空闲块"，空闲链表只有一个节点。

初始化完成后通过 kprintf 输出消息，串口上你会看到 `[HEAP] Initialised at 0xFFFF800000000000, size 64 KB`。

## First-fit 搜索：alloc 的主循环

`alloc()` 的完整路径比较长，我们按照执行顺序逐段拆解。

### 参数校验与空间估算

```cpp
void* Heap::alloc(size_t size, size_t align) {
    if (size == 0) {
        return nullptr;
    }
    if (align < 16) {
        align = 16;
    }
    size_t needed = size + (align - 1);
```

开头三步比较直接。`size == 0` 直接返回空指针——这不是错误，C 标准允许 `malloc(0)` 返回 null 或唯一指针，我们选择了更安全的 null。`align < 16` 时强制提升到 16，这是 x86-64 上 SSE 指令的最低对齐要求（SSE 操作 16 字节的 XMM 寄存器，如果数据没有 16 对齐会触发 `#GP`）。`needed = size + (align - 1)` 是考虑对齐后 worst-case 的空间需求——实际消耗可能比这小（如果空闲块的 header 恰好已经对齐了），但搜索时需要按最大可能来估算。

### 遍历空闲链表

```cpp
    BlockHeader* prev = nullptr;
    BlockHeader* curr = free_list_;

    while (curr != nullptr) {
        if (curr->magic != HEAP_MAGIC) {
            cinux::lib::kprintf("[HEAP] Corrupt block at 0x%p (magic=0x%x)\n",
                                reinterpret_cast<void*>(curr), curr->magic);
            return nullptr;
        }

        if (curr->free && curr->size >= needed) {
```

经典的链表遍历双指针模式。`prev` 和 `curr` 一前一后——后续从链表摘除节点时需要 `prev` 来重接。每次迭代先检查 magic 是否正确——如果空闲链表被踩了（比如某个块的 buffer overflow 侵入了相邻块的 header），立即报错返回。搜索条件 `curr->free && curr->size >= needed` 是 first-fit 的核心——找到第一个够大的空闲块就停下来。

### 对齐计算：最精巧的部分

找到一个候选块后，接下来的代码是整个 alloc 中最容易出错的地方：

```cpp
            uintptr_t curr_addr = reinterpret_cast<uintptr_t>(curr);
            uintptr_t block_end = curr_addr + HEADER_SIZE + curr->size;
            uintptr_t aligned_payload = align_up(curr_addr + HEADER_SIZE, align);
            uintptr_t hdr_addr = aligned_payload - HEADER_SIZE;

            size_t usable = static_cast<size_t>(block_end - aligned_payload);
            if (usable < size) {
                prev = curr;
                curr = curr->next;
                continue;
            }
```

`curr_addr` 是空闲块的起始地址。`block_end` 是空闲块的结束地址。`aligned_payload` 是对齐后的 payload 地址——从 header 结束位置开始，向上取整到 `align` 的倍数。`hdr_addr = aligned_payload - HEADER_SIZE` 是分配块 header 的实际放置位置。这里有一个微妙之处：`hdr_addr` 不一定等于 `curr_addr`。在默认 16 字节对齐下，由于 BlockHeader 恰好 32 字节（32 是 16 的倍数），两者通常相等。但如果你请求 4096 字节对齐，`aligned_payload` 被推到下一个 4096 的倍数，`hdr_addr` 就落在空闲块内部，产生了"front padding"。

`usable = block_end - aligned_payload` 计算对齐后的实际可用空间。如果对齐把 payload 推得太靠后，可用空间不够 `size`，就跳过这个块继续搜索——这种情况在极端对齐要求下可能发生。

### 从链表摘除与 front padding 处理

通过 usable 检查后，先把当前块从空闲链表摘除，然后处理 front padding：

```cpp
            size_t front_pad = static_cast<size_t>(hdr_addr - curr_addr);
            size_t tail_space = static_cast<size_t>(block_end - (aligned_payload + size));

            if (prev != nullptr) {
                prev->next = curr->next;
            } else {
                free_list_ = curr->next;
            }

            if (front_pad >= MIN_SPLIT) {
                curr->size = static_cast<uint32_t>(front_pad - HEADER_SIZE);
                curr->next = free_list_;
                free_list_ = curr;
            }
```

`front_pad` 是空闲块起始到分配块 header 之间的字节数。如果 `front_pad >= MIN_SPLIT`（48 字节），就把空闲块的原始 header 利用起来——把 `size` 改成 `front_pad - HEADER_SIZE`，然后挂回空闲链表。不足 48 字节的 front pad 就作为内部碎片浪费掉。`tail_space` 是分配结束后到空闲块末尾的余量——这就是块分裂的来源。

### 块分裂：tail remainder

```cpp
            if (tail_space >= MIN_SPLIT) {
                auto* rem = reinterpret_cast<BlockHeader*>(aligned_payload + size);
                rem->magic = HEAP_MAGIC;
                rem->size  = static_cast<uint32_t>(tail_space - HEADER_SIZE);
                rem->free  = 1;
                rem->next  = free_list_;
                free_list_ = rem;
            }
```

分配结束后，如果 tail space 足够大（>= MIN_SPLIT），就在 payload 之后创建一个新的空闲块。`rem` 的位置是 `aligned_payload + size`，大小是 `tail_space - HEADER_SIZE`（扣掉 header 占用的 32 字节）。不足 MIN_SPLIT 的余量直接归入当前分配——反正也分配不了任何有意义的东西。

`MIN_SPLIT = 48` 这个阈值是经过权衡的。设为 32（只有 header 没有 payload）会产生永远无法满足任何分配的"僵尸块"——最小 alloc 需要 32 + 1 的空间，一个 payload 为 0 的块什么也做不了。16 字节的 payload 下限保证分裂出来的小块至少能响应最小的分配请求。设得更高（比如 64）则过于保守，会让更多空间被浪费在不可分裂的余量里。

### 写入分配块 header 并返回

```cpp
            auto* alloc_hdr = reinterpret_cast<BlockHeader*>(hdr_addr);
            alloc_hdr->magic = HEAP_MAGIC;
            alloc_hdr->size  = static_cast<uint32_t>(size);
            alloc_hdr->free  = 0;
            alloc_hdr->next  = nullptr;

            used_ += HEADER_SIZE + size;

            memzero(reinterpret_cast<void*>(aligned_payload), size);

            return reinterpret_cast<void*>(aligned_payload);
```

分配块的 header 写在 `hdr_addr`（即 `aligned_payload - HEADER_SIZE`），size 记录请求的原始值，free 设为 0。`used_` 增加 `HEADER_SIZE + size`——header 也算已使用。最后 payload 清零后返回。

注意这里有一个设计决策：`alloc` 返回的内存总是干净的。这不是 C 标准对 `malloc` 的要求（malloc 返回的内存内容是未定义的），但对于内核来说，清零分配是一个合理的默认行为——它消除了"泄露上一任使用者的数据"的安全风险，也省得调用者每次都要手动 memset。

### 找不到就扩展

```cpp
    expand(size + align + HEADER_SIZE);
    return alloc(size, align);
}
```

如果遍历完整链表都没找到合适的块，就调用 `expand()` 申请新空间，然后递归重试。递归是安全的，因为 expand 保证映射足够容纳 `size + align + HEADER_SIZE`。但有一个理论上的风险：如果 PMM 在 expand 过程中耗尽，alloc 最终会返回 nullptr。在实际的内核运行场景下，这通常意味着系统已经无法继续工作了。你也可以把递归改成循环来避免极端情况下的栈溢出——但对内核堆来说，连续多次 expand 耗尽 PMM 是几乎不可能发生的。

## 收尾

到这里，`init` 和 `alloc` 的完整逻辑就讲清楚了。`init` 从一块空白虚拟内存创建一个覆盖整个区域的巨大空闲块，`alloc` 用 first-fit 搜索在空闲链表上线性遍历，找到第一个够大的块后处理好对齐和分裂，返回清零的 payload 指针。front padding 和 tail space 的处理是 `alloc` 中最精巧的部分——front padding 把对齐产生的间隙复用成小空闲块（如果够大的话），tail space 把分配后的剩余空间分裂成新的空闲块。

但分配只是故事的一半。内存用完了需要释放，释放后需要把碎片拼回去，链表空了还需要自动扩展。下一章我们深入 `free()`、`coalesce()` 和 `expand()`——看看释放时的安全检查如何工作、相邻空闲块的合并算法如何处理"三个块连续释放"这种传递性场景、以及堆自动增长的机制如何和 VMM 集成。

## 参考资料

- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) — first-fit / best-fit 策略对比，块分裂和合并的基本原理。
- dreamportdev/Osdev-Notes: [Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md) — 从 bump 到 free list 到 split 到 coalescing 的渐进式教程，和 Cinux 的实现路径高度吻合。
- FreeRTOS `heap_4.c` 源码: 几乎相同的 first-fit + split 实现。FreeRTOS 不支持自定义对齐但有线程安全保护。
- Donald Knuth, *The Art of Computer Programming* Vol.1 Section 2.5: first-fit / best-fit 策略的理论分析，包括碎片化的数学论证。
- Linux SLOB 分配器 `mm/slob.c`: 基于 first-fit 的三链表分离设计，按大中小三级减少搜索时间。
