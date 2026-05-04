# 017-2 通读：init 与 alloc——从空白内存到一块可用的堆

## 概览

本文是 tag `017_mm_heap` 四篇通读教程的第二篇，完整 walkthrough `kernel/mm/heap.cpp` 的前半部分：常量定义、全局实例、辅助函数、`init()` 和 `alloc()`。其中 `alloc()` 是整个堆分配器最复杂的部分，处理 first-fit 搜索、对齐计算、front padding、块分裂以及自动扩展。

## 架构图

```
    alloc(size, align) 完整路径:

    size == 0? ──Yes──→ return nullptr
         │ No
    needed = size + (align - 1)
         │
    遍历 free list ──→ curr->size >= needed?
         │ 找到
    计算 aligned_payload, front_pad, tail_space
         │
    从 free list 移除当前块
         │
    front_pad >= MIN_SPLIT? ──Yes──→ 创建前部空闲块
    tail_space >= MIN_SPLIT? ──Yes──→ 创建尾部空闲块(分裂)
         │
    写入 alloc header, 清零 payload, 更新 used_
         │
    return payload

    (未找到 → expand → 递归重试)
```

## 代码精讲

### 常量定义与全局实例

```cpp
#include "kernel/mm/heap.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/arch/x86_64/paging_config.hpp"
#include "kernel/lib/kprintf.hpp"
#include "kernel/mm/pmm.hpp"
#include "kernel/mm/vmm.hpp"

namespace cinux::mm {

constexpr uint32_t HEAP_MAGIC    = 0xDEADBEEF;
constexpr uint32_t HEADER_SIZE   = sizeof(BlockHeader);
constexpr uint32_t MIN_SPLIT     = HEADER_SIZE + 16;   // min viable split
constexpr uint64_t EXPAND_PAGES  = 4;                   // expand by 16 KB at a time
constexpr uint64_t PAGE_FLAGS    = 0x03;                // present + writable

Heap g_heap;
```

五个编译期常量控制分配器行为。`HEAP_MAGIC = 0xDEADBEEF` 是 BlockHeader 的校验值。`HEADER_SIZE = 32`（由 static_assert 保证）。`MIN_SPLIT = 48` 是块分裂的最小阈值（header 32 + payload 16），保证分裂出的块至少能容纳最小有用分配。`EXPAND_PAGES = 4` 表示每次扩展至少 4 页（16 KB），平衡扩展频率和内存浪费。`PAGE_FLAGS = 0x03` 即 Present + Writable。

`g_heap` 全局对象会被自动零初始化，在 `init()` 调用前处于安全空状态。

### 三个内部辅助函数

```cpp
namespace {

uint64_t align_up(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

BlockHeader* header_from_ptr(void* ptr) {
    return reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uintptr_t>(ptr) - HEADER_SIZE);
}

void memzero(void* start, size_t len) {
    auto* p = static_cast<uint8_t*>(start);
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

}  // anonymous namespace
```

这三个函数放在匿名命名空间中，只在 `heap.cpp` 内可见。

`align_up` 是经典的向上对齐计算：`(value + align - 1) & ~(align - 1)`，全是位运算不需要除法。`header_from_ptr` 是 free 中最关键的地址计算——给定 payload 指针，回退 32 字节定位 BlockHeader。注意不管分配时 header 实际放在哪里（可能有 front padding），这个函数永远从 payload 回退 HEADER_SIZE，所以 free 只需用户传 payload 指针。`memzero` 是手写的清零循环，因为内核没有标准库的 memset。

### Heap::init——从空白虚拟地址到可用的初始堆

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

    memzero(reinterpret_cast<void*>(virt_base), static_cast<size_t>(aligned_size));

    auto* first = reinterpret_cast<BlockHeader*>(virt_base);
    first->magic = HEAP_MAGIC;
    first->size  = static_cast<uint32_t>(aligned_size - HEADER_SIZE);
    first->free  = 1;
    first->next  = nullptr;

    base_     = virt_base;
    size_     = aligned_size;
    used_     = 0;
    free_list_ = first;

    cinux::lib::kprintf("[HEAP] Initialised at 0x%p, size %u KB\n",
                        reinterpret_cast<void*>(virt_base),
                        aligned_size / 1024);
}
```

init 的第一步是把 `initial_size` 向上对齐到页大小。然后逐页分配物理内存并建立虚拟映射——64 KB 对应 16 次迭代，每次调用 `g_pmm.alloc_page()` 获取物理页再 `g_vmm.map()` 建立映射。`alloc_page()` 返回 0 时打印 OOM 并返回，堆处于未完全初始化的状态。

映射完成后 `memzero` 把整个虚拟区域清零——PMM 分配的物理页可能包含旧数据，而 alloc 承诺返回清零的内存。然后在区域起始处创建第一个 BlockHeader 覆盖整个堆区域：`first->size = aligned_size - HEADER_SIZE`，整个堆初始为"一个大空闲块"。最后存入元信息并打印初始化消息。

### Heap::alloc——first-fit 搜索与块分裂

现在我们进入整个分配器最复杂也最有趣的部分。`alloc` 的代码比较长，我们按照它的执行流程逐段分析。

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

开头三步是参数校验和调整。`size == 0` 直接返回空指针。`align < 16` 时强制提升到 16（x86-64 SSE 的对齐要求）。`needed = size + (align - 1)` 是考虑对齐后 worst-case 的空间需求。

接下来进入 first-fit 搜索的主循环：

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

`prev` 和 `curr` 是经典的链表遍历双指针。首先检查当前块的 magic 是否正确——如果不对说明空闲链表被踩了。搜索条件 `curr->free && curr->size >= needed` 是 first-fit 策略的核心：找到第一个够大的空闲块。

找到一个候选块后，进入对齐和分裂的计算：

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

这六行代码是整个 alloc 中最精巧的部分。`curr_addr` 是当前空闲块起始地址。`block_end` 是空闲块结束地址。`aligned_payload` 是对齐后的 payload 地址。`hdr_addr = aligned_payload - HEADER_SIZE` 是分配块 header 的位置。

`hdr_addr` 不一定等于 `curr_addr`——默认对齐下通常相等，但请求 4096 字节对齐时，`aligned_payload` 被推到下一个 4096 的倍数，`hdr_addr` 落在空闲块内部，产生 "front padding"。`usable = block_end - aligned_payload` 是对齐后的实际可用空间，不够 size 就跳过。

通过 usable 检查后，计算 front padding 和 tail space：

```cpp
            size_t front_pad = static_cast<size_t>(hdr_addr - curr_addr);
            size_t tail_space = static_cast<size_t>(block_end - (aligned_payload + size));

            if (prev != nullptr) {
                prev->next = curr->next;
            } else {
                free_list_ = curr->next;
            }
```

`front_pad` 是空闲块起始到分配块 header 之间的字节数，`tail_space` 是分配结束后到空闲块末尾之间的字节数。

在处理 padding 和分裂之前，先把当前块从空闲链表中摘除。front padding 可能复用当前块的 header，tail space 会创建新的空闲块，摘除后重新挂载逻辑更清晰。

接下来处理 front padding 和 tail space：

```cpp
            if (front_pad >= MIN_SPLIT) {
                curr->size = static_cast<uint32_t>(front_pad - HEADER_SIZE);
                curr->next = free_list_;
                free_list_ = curr;
            }

            if (tail_space >= MIN_SPLIT) {
                auto* rem = reinterpret_cast<BlockHeader*>(aligned_payload + size);
                rem->magic = HEAP_MAGIC;
                rem->size  = static_cast<uint32_t>(tail_space - HEADER_SIZE);
                rem->free  = 1;
                rem->next  = free_list_;
                free_list_ = rem;
            }
```

如果 `front_pad >= MIN_SPLIT`（48 字节），我们就在原来的空闲块起始处创建一个新的小空闲块，payload 大小为 `front_pad - HEADER_SIZE`。不足 48 字节的 front_pad 就作为内部碎片浪费掉了。tail space 的处理同理——在 payload 结束后创建新的空闲块，大小为 `tail_space - HEADER_SIZE`，不足 MIN_SPLIT 的余量归入当前分配。

最后，写入分配块的 header 并返回：

```cpp
            auto* alloc_hdr = reinterpret_cast<BlockHeader*>(hdr_addr);
            alloc_hdr->magic = HEAP_MAGIC;
            alloc_hdr->size  = static_cast<uint32_t>(size);
            alloc_hdr->free  = 0;
            alloc_hdr->next  = nullptr;

            used_ += HEADER_SIZE + size;

            memzero(reinterpret_cast<void*>(aligned_payload), size);

            return reinterpret_cast<void*>(aligned_payload);
        }

        prev = curr;
        curr = curr->next;
    }
```

分配块的 header 写在 `hdr_addr`（即 `aligned_payload - HEADER_SIZE`），size 记录请求的原始值，free 设为 0。`used_` 增加 `HEADER_SIZE + size`（header 也算已使用）。payload 清零后返回——alloc 返回的内存总是干净的。

如果遍历完整链表没找到合适的块，就走扩展路径：

```cpp
    expand(size + align + HEADER_SIZE);
    return alloc(size, align);
}
```

调用 `expand()` 申请新空间（至少容纳 `size + align + HEADER_SIZE`），然后递归重试。递归是安全的，因为 expand 保证映射足够空间。如果 PMM 在 expand 中耗尽，alloc 返回 nullptr。

## 设计决策

### Decision: first-fit 而非 best-fit

**问题**: 空闲链表的搜索策略应该怎么选？

**本项目的做法**: first-fit——遍历链表，找到第一个够大的块就用。

**备选方案**: best-fit 遍历整个链表找最小的够用块；worst-fit 总是选最大的块。

**为什么不选备选方案**: best-fit 时间复杂度总是 O(n)，first-fit 最优 O(1)。best-fit 还倾向于留下很多极小碎片——恰好比请求大一丢丢的块被选中后，剩余空间往往不够 MIN_SPLIT，全部变成内部碎片。first-fit + coalescing 的组合在碎片和性能之间取得了不错的平衡，也是 FreeRTOS heap_4 和 Linux SLOB 的选择。

**如果要扩展/改进**: 可以将空闲链表按块大小排序，或维护多个链表按大小分级（Linux SLOB 的做法）。

### Decision: MIN_SPLIT = HEADER_SIZE + 16

**问题**: 块分裂时，剩余部分至少要多大才值得分裂？

**本项目的做法**: `MIN_SPLIT = 32 + 16 = 48` 字节。

**备选方案**: 设为 `HEADER_SIZE`（32 字节），允许 payload 为 0 的"空块"；或设为 64 字节。

**为什么不选备选方案**: 32 字节的 MIN_SPLIT 会产生永远无法满足任何 alloc 的"僵尸块"——最小 alloc 需要 32 + 1 的空间。16 字节的 payload 下限保证分裂出来的小块至少能响应小请求。设得更高则过于保守。

**如果要扩展/改进**: 根据分配历史动态调整 MIN_SPLIT。

## 扩展方向

1. **实现 best-fit 搜索策略并对比** (⭐⭐): 在 `alloc` 中增加编译开关选择 first-fit 或 best-fit，用相同随机序列测试碎片化程度和搜索时间。
2. **分离的空闲链表（segregated free lists）** (⭐⭐⭐): 参考 Linux SLOB，维护多个链表按大小分级，小请求只搜索小链表。
3. **分配统计与碎片化报告** (⭐): 在 `alloc`/`free` 中累计统计信息（分配次数、最大连续空闲块、碎片化率），通过 `dump_stats` 输出。
4. **非递归的 expand+alloc 重试** (⭐): 改为循环重试，避免极端情况下递归过深。

## 参考资料

- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) — first-fit / best-fit 策略对比，块分裂和合并的基本原理。
- FreeRTOS `heap_4.c` 源码: 几乎相同的 first-fit + split 实现，对比 Cinux 不支持自定义对齐但有线程安全保护。
- Donald Knuth, *The Art of Computer Programming* Vol.1 Section 2.5: first-fit / best-fit 策略的理论分析。
- dreamportdev/Osdev-Notes: [Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md) — 从 bump 到 free list 的完整教程。
