# 释放、合并与自动扩展

> 标签：Heap::free, coalesce, Heap::expand, double-free, 碎片化
> 前置：[017-2 初始化与分配：first-fit 算法详解](017-mm-heap-2.md)

## 前言

上一篇我们跟着 `alloc()` 走完了分配的完整路径——从 first-fit 搜索到对齐计算到块分裂。但分配只是硬币的一面，内存用完了得还回来，还回来之后还得想办法把碎片拼成大块，不然下次分配就找不着够大的空间了。本章覆盖 `Heap::free()`、`coalesce()` 和 `expand()` 三个函数——它们分别是回收、合并和扩展的核心实现，和 `alloc()` 合在一起构成了堆分配器的完整生命周期。

这三个函数里，`coalesce()` 是最有设计趣味的一个——它需要在空闲链表中找出地址上相邻的块，然后把它们合并成一个更大的块。听起来简单，但"怎么判断相邻"和"合并后链表怎么维护"这两个问题牵扯出一堆边界情况。特别是"三个连续块释放"这种场景，合并是传递性的——A+B 合并后，(A+B) 和 C 又相邻了，需要再合并一次。我们先把 `free()` 讲清楚，然后逐步深入 coalesce 的细节。

## 环境说明

本章的代码覆盖 `kernel/mm/heap.cpp` 的后半部分（约 180-340 行），包括 `free()`、`coalesce()`、`expand()` 和 `dump_stats()`。运行环境和前一章完全一致——PMM 和 VMM 必须已初始化，堆已通过 `init()` 建立了初始空闲块。所有测试都可以在 QEMU 环境下通过 `make test` 触发。

## free()：带校验的块回收

### 三道安全检查

`free()` 的第一步不是直接操作链表，而是做安全检查。这三道检查在正常情况下不会被触发，但一旦你的代码出了问题（野指针、double-free、buffer overflow），它们就是救命的第一道防线——至少能告诉你"哪里出了问题"，而不是让你在一个莫名其妙的地方 debug 半天。

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
```

`free(nullptr)` 直接返回——C 标准规定这是合法的 no-op，C++ 的 `delete nullptr` 同理。我们不需要在调用侧做 `if (ptr != nullptr)` 判断，因为 free 内部已经处理了。接下来通过 `header_from_ptr` 从 payload 指针回退 32 字节定位 BlockHeader——这个操作本身就可能暴露问题，如果传入的指针不是 `alloc` 返回的（比如是一个栈变量或全局变量的地址），回退 32 字节后读到的 magic 几乎肯定不是 `0xDEADBEEF`。

第二道检查 magic 不等于 `0xDEADBEEF` 时报错。错误信息用"Double-free or corruption"这个措辞是因为 magic 不正确有两种常见原因：要么是 double-free 导致 header 被后续分配踩了（第一次 free 后这块内存可能被重新分配，新分配写了新的 header，第二次 free 看到的是新 header 的 magic，地址不同但 magic 值碰巧不对），要么是传入了一个彻底无效的指针导致读到了随机内存。

第三道检查 `block->free` 是专门的 double-free 检测。第一次 free 后 `block->free` 被设为 1，如果同一指针再次传入，我们发现它已经是 free 状态就直接拒绝。这里有一个微妙的假设——两次 free 之间没有人改写过这个 header。如果中间恰好有一次 alloc 复用了这块空间并写入了新的 header，那第二次 free 看到的 `free` 就是 0，不会被捕获。不过这种场景其实是合法的"分配-释放-再分配"序列，不是真正的 double-free。

### 回收到链表并触发合并

通过检查后，操作就简单了：

```cpp
    used_ -= HEADER_SIZE + block->size;
    block->free = 1;

    block->next = free_list_;
    free_list_  = block;

    coalesce(block);
}
```

`used_` 减去 `HEADER_SIZE + block->size` 来更新已使用量——和 alloc 中的 `used_ += HEADER_SIZE + size` 完全对称。所以当所有分配都被释放后，`used_` 应该精确归零。host 端的单元测试大量验证了这个不变量：200 轮 alloc/free 循环后，`used_` 必须回到 0。

标记 `block->free = 1`，然后用头插法挂到 `free_list_` 的头部——新释放的块总是成为链表的第一个节点。这里用头插法有一个微妙的好处：刚释放的块最有可能还在 CPU 缓存中（因为它刚被使用过），把它放在链表头部使得下一次 alloc 时最先搜索到它，cache hit 率更高。FreeRTOS 的 heap_4 也用了同样的头插策略。

最后调用 `coalesce(block)` 尝试和相邻的空闲块合并。这是 free 中最重的操作，也是最关键的——如果不做合并，堆会逐渐碎片化到无法使用。

## coalesce()：相邻空闲块的合并

### 为什么合并是必要的

先别急着看代码，我们先用一个具体的例子理解为什么需要 coalesce。假设堆里连续分配了三个 64 字节的块 A、B、C，地址依次递增，然后全部释放。如果不做合并，空闲链表里会有三个独立的小块，每个 payload 只有 64 字节。如果你现在想分配一个 192 字节的块，三个小块加起来够用但没有一个单独够大——分配失败，即使堆里明明有足够的空闲空间。这就是外部碎片化的经典表现。

coalesce 的作用是把地址上相邻的空闲块合并成一个更大的块。上面三个 64 字节的块合并后变成一个大块——payload 是 64+32+64+32+64 = 256 字节（中间两个被吸收的块的 header 空间也变成了 payload），可以轻松满足 192 字节的请求。QEMU 端的测试用例正是用这种方式验证 coalesce 的：分配三个 64 字节块，全部释放，然后分配一个 256 字节的块——如果 coalesce 不工作，这个分配就会失败。

### "循环到收敛"的合并策略

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

coalesce 采用了"循环直到收敛"的策略——外层 `while (changed)` 在每次成功合并后重新开始遍历。为什么？因为一次合并可能产生新的相邻关系。比如有三个紧邻的块 A、B、C，先合并 A+B 得到 AB，然后 AB 和 C 又相邻，需要再合并一次。单遍扫描无法处理这种传递性合并。`changed` 标志在每次合并后设为 true，触发下一轮搜索。我们的 host 测试用三种不同的释放顺序（左到右、右到左、先中间再两边）专门验证了这种传递性合并。

内层循环遍历空闲链表。`curr == block` 跳过自己——我们不会把自己和"自己"合并。`!curr->free` 跳过已分配的块——防御性检查，正常情况下空闲链表里不应该有已分配块。

### 两个方向的相邻检测

接下来是 coalesce 的核心逻辑——判断两个块在地址上是否相邻：

```cpp
            // 第一种情况：curr 在 block 紧前面
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

            // 第二种情况：block 在 curr 紧前面
            if (block_addr + HEADER_SIZE + block->size == curr_addr) {
                block->size += HEADER_SIZE + curr->size;
                if (prev) prev->next = curr->next;
                else      free_list_ = curr->next;
                changed = true;
                break;
            }
```

两个判断条件是对称的。第一种：`curr_addr + HEADER_SIZE + curr->size == block_addr` 意味着 curr 的末尾（header + payload）恰好紧挨着 block 的起始地址——curr 在 block 的紧前面。合并方式是把 block 吸收到 curr 中：`curr->size += HEADER_SIZE + block->size`，注意加的是 `HEADER_SIZE + block->size` 而不是单独 `block->size`——因为合并后 block 的 header 空间也变成了可用的 payload。

合并后需要把被吸收的块从空闲链表中摘除。第一种情况（摘除 block）需要额外的线性搜索来找到 block 的前驱——因为内层循环的 `prev` 指向的是 `curr` 的前驱而不是 `block` 的前驱。然后 `block = curr` 更新"当前块"指针——因为 curr 变成了合并后的大块，后续的合并要以它为基础。

第二种情况是对称的：block 在 curr 的紧前面，把 curr 吸收到 block 中。摘除 curr 的操作更简洁，因为我们有 `prev` 指针直接可用。

合并的收敛性是有保证的：每次合并严格减少空闲链表中的块数，而块数是有限的，所以外层循环最终一定会终止。最坏情况是所有块都相邻，n 个块需要 n-1 次合并，每次合并触发一轮 O(n) 的搜索，总复杂度 O(n^2)。对于内核堆来说完全可以接受——几十个块已经是极端情况了。

对比 FreeRTOS 的 heap_4，它的空闲链表是按地址排序的，合并时只需要检查前后邻居，一遍就够。Cinux 的链表是无序的（因为头插法），所以需要完整遍历来找邻居。这个取舍是有意为之的——排序链表在释放时需要找到正确的插入位置，O(n) 的搜索开销从 coalesce 转移到了 free，总开销不变但代码复杂度增加了。

## expand()：当空闲链表不够用时

### 计算扩展量

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

expand 的第一步是计算需要多少页来满足最小请求。`min_bytes + HEADER_SIZE` 加上 header 的开销，然后向上对齐到页大小。但至少要 `EXPAND_PAGES`（4 页 = 16 KB）。这个"最少扩展 16 KB"的策略是一种摊还思想——如果每次只扩展刚好够用的量（比如只申请 1 页来满足一个 100 字节的分配），频繁的小扩展会导致大量 PMM/VMM 调用和大量小碎片。一次多申请一些，既减少了扩展频率，也提高了后续分配的成功率。

### 映射新页面并创建空闲块

```cpp
    for (uint64_t offset = 0; offset < expand_size; offset += cinux::arch::PAGE_SIZE) {
        uint64_t phys = g_pmm.alloc_page();
        if (phys == 0) {
            cinux::lib::kprintf("[HEAP] OOM during expansion at offset %u\n", offset);
            return;
        }
        g_vmm.map(base_ + size_ + offset, phys, PAGE_FLAGS);
    }

    memzero(reinterpret_cast<void*>(base_ + size_),
            static_cast<size_t>(expand_size));

    auto* new_block = reinterpret_cast<BlockHeader*>(base_ + size_);
    new_block->magic = HEAP_MAGIC;
    new_block->size  = static_cast<uint32_t>(expand_size - HEADER_SIZE);
    new_block->free  = 1;
    new_block->next  = free_list_;
    free_list_       = new_block;

    size_ += expand_size;
```

和 `init` 中的逻辑完全一样——逐页分配物理内存、映射到虚拟地址。不同之处在于映射的起始位置是 `base_ + size_`，紧跟在当前堆区域的末尾。这意味着堆区域在虚拟地址空间中是连续增长的，所有已存在的块地址不受影响。新区域清零后创建一个覆盖整个新区域的空闲块，挂到 `free_list_` 头部。`size_` 增加扩展量。扩展完成后，`alloc` 会递归重试——新空闲块在链表头部，first-fit 搜索会第一个找到它。

这里有一个值得思考的问题：新空闲块会不会和堆末尾原有的空闲块相邻？答案是可能的。如果堆末尾恰好有一个空闲块（比如它是最后一次分配后 split 出来的 tail remainder），expand 后它和新块在地址上是紧邻的。但 coalesce 只在 `free()` 中被调用，expand 不会自动合并。这不是正确性问题——两个相邻的空闲块仍然可以正常工作，只是存在"堆末尾一个空闲块 + 紧接着的扩展区域空闲块"这种相邻但未合并的情况，直到下一次 free 触发 coalesce 时才可能合并。Cinux 选择了简单而不是完美。

## 碎片化：不可避免的代价

说到这里，我们必须诚实地面对碎片化这个问题。不管你用什么策略，堆分配器都会产生碎片。Cinux 的 free list + first-fit + coalescing 方案能缓解外部碎片（通过合并相邻空闲块），但无法消除内部碎片——front padding（对齐产生的间隙）和 MIN_SPLIT 以下的 tail remainder 都是不可回收的空间。

我们的 host 测试中有一个"记账不变量"测试，验证碎片率不超过 10%：`free_total > total * 9 / 10`。这个阈值是经验值——如果碎片超过 10%，说明分配策略有问题。在实际测试中，200 轮随机 alloc/free 后碎片率远低于这个阈值，说明 first-fit + coalescing 对于内核规模的分配模式是足够好的。

如果将来碎片化真的成为问题，有几个方向可以考虑。一是把空闲链表按大小分级（Linux SLOB 的做法），减少搜索开销的同时也能更好地匹配请求大小。二是在堆之上加一层 slab 分配器，为固定大小的对象维护专用缓存。三是实现堆收缩——当空闲链表中有大量尾部空间时，释放末尾的物理页回 PMM。不过这些都是后话了。

## dump_stats()：调试辅助

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

`dump_stats` 遍历空闲链表，累加所有标记为 free 的块的 size，统计总块数，然后输出四项信息：堆的总大小（`size_`）、已使用量（`used_`）、空闲链表中的可用总量（`free_total`）、空闲链表中的块数。注意 `free_total` 和 `size_ - used_` 不一定相等——因为块分裂和 front padding 会产生内部碎片，这些空间既不在 `used_` 中也不在空闲链表的 size 中。如果你在调试时发现 `total != used + free_list`，不要惊讶，差值就是内部碎片。这个函数在 QEMU 测试的 coalesce 用例中被调用，用来验证分配器状态。

## 收尾

到这一步，堆分配器的核心算法就完整了——`alloc` 从空闲链表中切出内存，`free` 带着安全检查把内存还回来，`coalesce` 把碎片拼成大块（用"循环到收敛"的策略处理传递性合并），`expand` 在链表耗尽时自动增长（最少 16 KB 的摊还策略），`dump_stats` 提供调试诊断。五个函数合在一起，构成了一个功能完备的动态内存分配器。

但这个分配器现在还是一个"孤岛"——内核的 C++ 代码要用 `new` 和 `delete`，不会直接调用 `g_heap.alloc()` 和 `g_heap.free()`。怎么把两者接起来？怎么保证这个分配器在各种极端场景下都能正常工作？下一章我们来看 C++ 集成和测试策略——`operator new/delete` 的重定向（包括六个 delete 签名的全覆盖）、对齐 new 的支持、以及 host + QEMU 两层测试的设计哲学。

## 参考资料

- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) — 堆分配器中合并（coalescing）的原理和实现策略，free list 的管理方式。
- FreeRTOS `heap_4.c` 源码: coalesce 的实现和 Cinux 几乎相同，但 FreeRTOS 的空闲链表按地址排序，合并只需检查前后邻居。
- dreamportdev/Osdev-Notes: [Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md) — 从 bump allocator 到 free list 到合并到扩展的渐进式教程。
- Linux 内核 `mm/slob.c`: SLOB 分配器的合并逻辑——释放时遍历按地址排序的链表，只做前向合并。
- xv6 RISC-V `kernel/kalloc.c`: 只有页级分配，没有合并和分裂——对比 Cinux 可以理解"为什么需要 sub-page 分配器"。
