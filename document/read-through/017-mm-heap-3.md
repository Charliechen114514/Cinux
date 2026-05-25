---
title: 017-mm-heap-3 · 堆管理
---

# 017-3 通读：free、coalesce 与 expand——回收、合并与动态扩展

## 概览

本文是 tag `017_mm_heap` 四篇通读教程的第三篇，完整 walkthrough `kernel/mm/heap.cpp` 的后半部分：`free()`、`coalesce()`、`expand()` 以及 `dump_stats()`。上一篇我们看到了 `alloc` 如何从空闲链表中切出一块交给调用者；现在我们来看它的逆过程——当调用者用完内存后，`free` 如何把块回收到链表中，`coalesce` 如何把相邻的小块拼回大块以减少碎片，以及当链表真的不够用时空闲区域如何通过 `expand` 自动增长。这三个函数合在一起，构成了堆分配器"用完-回收-扩展"的完整生命周期管理。

## 架构图

```
    free(ptr) → coalesce → expand 完整流程:

    free(ptr)
    │
    ├─ ptr == nullptr? → 直接返回
    ├─ magic 校验失败? → 打印错误, 返回
    ├─ block->free == 1? → double-free, 打印错误, 返回
    │
    ├─ used_ -= HEADER_SIZE + block->size
    ├─ block->free = 1
    ├─ 挂到 free_list_ 头部
    │
    └─→ coalesce(block)
         │
         │  循环直到没有更多合并:
         │  遍历 free_list_:
         │    ├─ curr 在 block 前面且相邻? → 合并到 curr
         │    └─ block 在 curr 前面且相邻? → 合并到 block
         │
         └─→ 返回

    expand(min_bytes) (当 alloc 找不到合适块时调用)
    │
    ├─ 计算 needed_pages (至少 EXPAND_PAGES = 4)
    ├─ 循环: g_pmm.alloc_page() + g_vmm.map()
    ├─ memzero 新区域
    ├─ 创建新空闲块, 挂到 free_list_
    └─ size_ += expand_size
```

## 代码精讲

### Heap::free——带校验的块回收

```cpp
void Heap::free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    auto* block = header_from_ptr(ptr);

    if (block->magic != HEAP_MAGIC) {
        cinux::lib::kprintf("[HEAP] Double-free or corruption at 0x%p "
                            "(magic=0x%x, expected 0x%x)\n",
                            ptr, block->magic, HEAP_MAGIC);
        return;
    }

    if (block->free) {
        cinux::lib::kprintf("[HEAP] Double-free detected at 0x%p\n", ptr);
        return;
    }

    used_ -= HEADER_SIZE + block->size;
    block->free = 1;

    block->next = free_list_;
    free_list_  = block;

    coalesce(block);
}
```

`free` 的第一步是空指针检查——`free(nullptr)` 在 C 标准中是定义行为（应该什么都不做），C++ 的 `operator delete` 规范也要求 `delete nullptr` 是安全的。所以我们直接返回，不做任何操作。

接下来通过上一篇中讲到的 `header_from_ptr` 宏从 payload 指针回退 32 字节定位到 BlockHeader。这个操作本身就可能暴露问题：如果传入的指针不是 alloc 返回的（比如是一个栈变量或者全局变量的地址），回退 32 字节后读到的 `magic` 几乎肯定不会是 `0xDEADBEEF`，于是第二个检查就会触发。错误信息里用了"Double-free or corruption"这个措辞——因为 magic 不正确有两种常见原因：要么是 double-free 导致 header 被踩了，要么是传入了一个无效指针导致读到了随机内存。

第三个检查 `block->free` 是专门的 double-free 检测。第一次 free 后 `block->free` 被设为 1，如果同一指针被再次传入 free，我们发现它已经是 free 状态，就打印"Double-free detected"并返回。这里有一个微妙的假设：第一次 free 之后没有人改写过这个 header。如果两次 free 之间有一次 alloc 恰好复用了这块空间并写入了新的 header，那么第二次 free 看到的 `block->free` 就是 0（新分配的状态），不会被捕获。不过这种情况其实不是 double-free，而是一个合法的"分配-释放-再分配"序列——真正的 double-free 是"没有中间的再分配就连续释放两次"，这种情况能被我们的检查可靠地捕获。

通过校验后，`used_` 减去 `HEADER_SIZE + block->size` 来更新已使用量。这和 alloc 中的 `used_ += HEADER_SIZE + size` 完全对称——每分配一次加，每释放一次减，所以当所有分配都被释放后，`used_` 应该精确归零。host 端的单元测试大量验证了这个不变量。

然后标记 `block->free = 1`，把 block 挂到 `free_list_` 的头部。注意这里用的是"头插法"——新释放的块总是成为链表的第一个节点。这有一个微妙的好处：刚释放的块最有可能还在 CPU 缓存中（因为它刚被使用过），把它放在链表头部使得下一次 alloc 时最先搜索到它，cache hit 率更高。

最后调用 `coalesce(block)` 尝试和相邻的空闲块合并。

### Heap::coalesce——相邻空闲块的合并

```cpp
void Heap::coalesce(BlockHeader* block) {
    bool changed = true;
    while (changed) {
        changed = false;
        BlockHeader* prev = nullptr;
        BlockHeader* curr = free_list_;

        while (curr != nullptr) {
            if (curr == block || !curr->free) {
                prev = curr;
                curr = curr->next;
                continue;
            }

            uintptr_t curr_addr  = reinterpret_cast<uintptr_t>(curr);
            uintptr_t block_addr = reinterpret_cast<uintptr_t>(block);
```

coalesce 采用了"循环直到收敛"的策略。外层 `while (changed)` 循环在每次成功合并后重新开始遍历，因为一次合并可能产生新的相邻关系——比如有三个紧邻的块 A、B、C，先合并 A+B 得到 AB，然后 AB 和 C 又相邻，需要再合并一次。`changed` 标志在每次合并后设为 true，触发下一轮搜索。

内层循环遍历空闲链表中的每一个块。第一个检查 `curr == block` 跳过自己——我们不会把自己和"自己"合并。`!curr->free` 跳过已分配的块——虽然按理说空闲链表里不应该有已分配的块，但这个检查是一个防御措施。

接下来是两个方向的相邻检测：

```cpp
            if (curr_addr + HEADER_SIZE + curr->size == block_addr) {
                curr->size += HEADER_SIZE + block->size;
                if (free_list_ == block) {
                    free_list_ = block->next;
                } else {
                    auto* p = free_list_;
                    while (p && p->next != block) p = p->next;
                    if (p) p->next = block->next;
                }
                block = curr;
                changed = true;
                break;
            }
```

这是第一种情况：`curr` 在 `block` 的紧前面。判断条件 `curr_addr + HEADER_SIZE + curr->size == block_addr` 的意思是：curr 的末尾（header + payload）恰好紧挨着 block 的起始地址。如果成立，说明这两个块在内存中是物理相邻的，可以合并。合并方式是把 block 的大小加到 curr 上：`curr->size += HEADER_SIZE + block->size`，注意这里加的是 `HEADER_SIZE + block->size` 而不是 `block->size`——因为合并后 block 的 header 空间也变成了可用的 payload。

合并后需要把 block 从空闲链表中摘除（因为它已经被"吸收"到 curr 中了）。如果 block 恰好是链表头，直接把 `free_list_` 指向 block 的下一个；否则遍历链表找到 block 的前驱节点，绕过 block。然后 `block = curr` 更新合并后的"当前块"指针，`changed = true` 标记需要重试，`break` 跳出内层循环开始新一轮搜索。

```cpp
            if (block_addr + HEADER_SIZE + block->size == curr_addr) {
                block->size += HEADER_SIZE + curr->size;
                if (prev) prev->next = curr->next;
                else      free_list_ = curr->next;
                changed = true;
                break;
            }

            prev = curr;
            curr = curr->next;
        }
    }
}
```

第二种情况是 block 在 curr 的紧前面。判断条件是对称的：`block_addr + HEADER_SIZE + block->size == curr_addr`。这次合并方向相反——把 curr 吸收到 block 中。摘除 curr 的操作更简洁，因为我们有 `prev` 指针（curr 的前驱），直接 `prev->next = curr->next` 就能绕过 curr。如果 curr 是链表头（`prev == nullptr`），则更新 `free_list_`。

你会注意到第二种情况的摘除比第一种简单得多——第一种需要一个额外的线性搜索来找到 block 的前驱，因为内层循环的 `prev` 指向的是 `curr` 的前驱而不是 `block` 的前驱。这是一个可以优化的地方，但在内核堆的规模下（几百个块），一次线性搜索的开销可以忽略不计。

合并的收敛性是有保证的：每次合并严格减少空闲链表中的块数，而块数是有限的，所以外层循环最终一定会终止。最坏情况是所有块都相邻，n 个块需要 n-1 次合并，每次合并触一轮 O(n) 的搜索，总复杂度 O(n^2)。对于内核堆来说这完全可接受——几十个块已经是极端情况了。

### Heap::expand——当空闲链表不够用时

```cpp
void Heap::expand(size_t min_bytes) {
    uint64_t needed_bytes = align_up(
        min_bytes + HEADER_SIZE, cinux::arch::PAGE_SIZE);
    uint64_t needed_pages = needed_bytes / cinux::arch::PAGE_SIZE;

    if (needed_pages < EXPAND_PAGES) {
        needed_pages = EXPAND_PAGES;
    }

    uint64_t expand_size = needed_pages * cinux::arch::PAGE_SIZE;
```

expand 的第一步是计算需要多少页来满足最小请求。`min_bytes + HEADER_SIZE` 加上了 header 的开销，然后向上对齐到页大小。`needed_pages` 是需要的页数，但至少要 `EXPAND_PAGES`（4 页 = 16 KB）。这个"最少扩展 4 页"的策略是一种启发式：如果每次只扩展刚好够用的量，那么频繁的小扩展会导致大量 PMM/VMM 调用和大量小碎片。一次多申请一些，既减少了扩展频率，也提高了后续分配的成功率。

```cpp
    for (uint64_t offset = 0; offset < expand_size; offset += cinux::arch::PAGE_SIZE) {
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) {
            cinux::lib::kprintf("[HEAP] OOM during expansion at offset %u\n", offset);
            return;
        }
        g_vmm.map(base_ + size_ + offset, phys, PAGE_FLAGS);
    }
```

和 `init` 中的逻辑完全一样：逐页分配物理内存、映射到虚拟地址。不同之处在于映射的起始位置是 `base_ + size_`——紧跟在当前堆区域的末尾。这意味着堆区域在虚拟地址空间中是连续增长的，所有已存在的块地址不受影响。如果 `alloc_page()` 在中途返回 0，我们打印 OOM 消息并返回——此时部分新页面已经被映射，但这些页面不会被任何块引用（因为后续创建的空闲块覆盖了整个新区域），所以不会造成问题。

```cpp
    memzero(reinterpret_cast<void*>(base_ + size_),
            static_cast<size_t>(expand_size));

    auto* new_block = reinterpret_cast<BlockHeader*>(base_ + size_);
    new_block->magic = HEAP_MAGIC;
    new_block->size  = static_cast<uint32_t>(expand_size - HEADER_SIZE);
    new_block->free  = 1;
    new_block->next  = free_list_;
    free_list_       = new_block;

    size_ += expand_size;

    cinux::lib::kprintf("[HEAP] Expanded by %u KB, total %u KB\n",
                        expand_size / 1024, size_ / 1024);
}
```

新区域被清零后，创建一个覆盖整个新区域的空闲块，挂到 `free_list_` 头部。然后 `size_` 增加扩展量，记录新的总大小。

这里有一个值得思考的问题：新创建的空闲块会不会和堆末尾原有的空闲块相邻？答案是：有可能。如果堆末尾恰好有一个空闲块（比如它是最后一次分配后 split 出来的 tail remainder），那么 expand 后它和新块在地址上是紧邻的。但 coalesce 只在 `free()` 中被调用，expand 不会自动合并。这意味着可能存在"堆末尾一个空闲块 + 紧接着的扩展区域空闲块"这种相邻但未合并的情况。这不是一个正确性问题——两个相邻的空闲块仍然可以正常工作，只是在下一次 free 触发 coalesce 时可能会被合并。如果你想更完美，可以在 expand 中对新块和可能的末尾空闲块做一次 coalesce，但目前的实现选择了简单。

### Heap::dump_stats——调试输出

```cpp
void Heap::dump_stats() const {
    uint64_t free_total = 0;
    uint64_t block_count = 0;

    BlockHeader* curr = free_list_;
    while (curr != nullptr) {
        if (curr->free) {
            free_total += curr->size;
        }
        block_count++;
        curr = curr->next;
    }

    cinux::lib::kprintf("[HEAP] Stats: total=%u KB, used=%u KB, free_list=%u KB, "
                        "blocks=%u\n",
                        size_ / 1024, used_ / 1024, free_total / 1024, block_count);
}
```

dump_stats 遍历空闲链表，累加所有标记为 free 的块的 size，统计总块数，然后输出四项信息：堆的总大小（`size_`）、已使用量（`used_`）、空闲链表中的可用总量（`free_total`）、空闲链表中的块数。注意 `free_total` 和 `size_ - used_` 不一定相等——因为块分裂和 front padding 会产生内部碎片，这些空间既不在 `used_` 中也不在空闲链表的 size 中。如果你在调试时发现 `total != used + free_list`，不要惊讶，差值就是内部碎片。这个函数在 QEMU 测试中被调用来验证分配器状态。

## 设计决策

### Decision: coalesce 采用"循环到收敛"而非单遍扫描

**问题**: 合并相邻空闲块时，是否需要处理"合并后产生新的相邻关系"这种情况？

**本项目的做法**: 外层 `while (changed)` 循环在每次合并后重试，直到没有更多合并发生。

**备选方案**: 单遍扫描——遍历一次链表，只合并第一次发现的机会。另一种方案是把所有空闲块按地址排序，然后顺序合并相邻的。

**为什么不选备选方案**: 单遍扫描无法处理传递性合并（A 和 B 合并后，AB 和 C 又相邻）。排序后合并虽然能一次处理所有相邻关系，但排序本身是 O(n log n)，而且在单链表上实现排序需要额外空间或复杂的 in-place 操作。我们的链表通常只有几十个块，O(n^2) 的"循环到收敛"和 O(n log n) 的"排序后合并"在实际性能上几乎无差别，但前者的实现简单得多。

**如果要扩展/改进**: 如果未来空闲链表的块数增长到几百甚至上千（比如长时间的内核运行），可以改为维护一个按地址排序的链表，合并时只需一次线性遍历。或者改用 buddy 系统的合并策略——每个块记录它属于哪个"order"（大小级别），释放时只和它的 buddy 合并，O(1) 时间复杂度。

### Decision: expand 时最少 4 页（16 KB），而非按需精确分配

**问题**: 堆扩展时应该申请多少页面？

**本项目的做法**: 至少 `EXPAND_PAGES = 4`（16 KB），或者满足当前请求所需的页数（取较大值）。

**备选方案**: 精确计算——只申请刚好够容纳当前请求的页数。或者固定一个较大的扩展量（比如 64 KB 或 128 KB）。

**为什么不选备选方案**: 精确分配的问题是"amortized allocation"（摊还分配）的缺失——如果程序反复分配 100 字节，每次都会触发一次 expand（只申请 1 页），PMM 和 VMM 的开销会积少成多。4 页（16 KB）是一个"不太小也不太大"的默认值，能容纳几十到几百个小分配而无需再次扩展。固定更大的扩展量（比如 64 KB）则可能对内存紧张的嵌入式场景不友好。16 KB 是 Linux 内核早期版本使用的默认分配粒度之一。

**如果要扩展/改进**: 可以实现"倍增扩展"策略——第一次扩展 16 KB，第二次 32 KB，第三次 64 KB，以此类推，直到达到一个上限。这类似于 `std::vector` 的扩容策略，在频繁分配的场景下能显著减少扩展次数。

## 扩展方向

1. **在 expand 中合并末尾空闲块** (⭐): 在创建新的空闲块之前，检查堆末尾是否已经有一个空闲块。如果是，直接扩展它而不是创建新块。这能减少碎片并避免 coalesce 的延迟。

2. **实现 slab 分配器作为堆的上层** (⭐⭐⭐): 参考 Linux 的 SLUB 分配器，在堆之上为常用大小的对象（比如 `task_struct`、`page` 结构体）维护专用缓存。slab 分配器从堆中获取大块内存，然后切成固定大小的小对象，分配/释放都是 O(1)。

3. **堆收缩（heap shrink）** (⭐⭐): 当空闲链表中有大量尾部空闲空间时，释放末尾的物理页回 PMM。这需要确保被释放的页上没有已分配的块——实现难度比扩展大得多。

4. **coalesce 的性能统计** (⭐): 在 coalesce 中统计合并次数和跳过次数。如果发现合并非常频繁，说明碎片化严重，可能需要调整 MIN_SPLIT 或扩展策略。

5. **buddy 系统合并替代方案** (⭐⭐⭐): 研究 buddy 分配器的合并策略（每个块记录 order，释放时只和固定位置的 buddy 合并），对比当前 coalesce 的 O(n^2) 最坏复杂度。buddy 系统的合并是 O(1) 的，但需要更多的元数据。

## 参考资料

- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) — 堆分配器中合并（coalescing）的原理和实现策略。
- FreeRTOS `heap_4.c` 源码: coalesce 的实现和 Cinux 几乎相同，但 FreeRTOS 的空闲链表是按地址排序的，合并时只需检查前后邻居。
- dreamportdev/Osdev-Notes: [Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md) — 从 bump allocator 到 free list 到合并到扩展的渐进式教程。
- Linux 内核 `mm/slob.c`: SLOB 分配器的合并逻辑——它在释放时遍历按地址排序的链表，只做前向合并。
- xv6 RISC-V `kernel/kalloc.c`: xv6 只有页级分配，没有合并和分裂——对比 Cinux 可以理解"为什么需要 sub-page 分配器"。
