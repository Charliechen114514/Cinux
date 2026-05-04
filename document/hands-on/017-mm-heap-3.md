# 017-3 释放、合并与自动扩展

## 导语

上一章我们把 alloc 写好了，内核终于能动态分配内存了。但一个只会分配不会释放的系统就像一个只进不出的蓄水池——早晚要溢出。本章我们要实现堆分配器的"排泄系统"：`Heap::free()` 负责回收不再使用的内存块，`coalesce()` 负责把相邻的空闲块合并成更大的块以减少碎片，`expand()` 负责在堆空间耗尽时向 VMM 申请新的虚拟地址空间。

这三者之间的关系是这样的：free 把块标记为空闲并加入空闲链表，然后调用 coalesce 尝试合并；alloc 在找不到足够大的空闲块时调用 expand 扩展堆区域。free + coalesce 解决的是"回收"问题，expand 解决的是"扩容"问题。两者配合，堆分配器才能在长时间运行的内核中保持健康。

知识前置：你需要理解上一章的 alloc 实现，特别是 BlockHeader 的布局、空闲链表的操作方式，以及 front padding 对头部位置的影响。本章的 coalesce 算法需要频繁做地址计算来判断两个块是否"相邻"，所以对虚拟地址空间的线性布局要有清晰的概念。

## 概念精讲

### 释放的安全检查：magic 校验与 double-free 检测

free 函数接收到一个 payload 指针后，第一步是通过 header_from_ptr 定位到 BlockHeader，然后检查 magic 字段是否为 0xDEADBEEF。如果 magic 不对，说明传入的指针不是一个有效的堆分配块——可能是野指针、栈上的地址、或者内存已经被损坏覆写了。此时 free 打印错误信息并直接返回，不修改任何堆状态。这是一种"fail-safe"策略：宁可泄漏也不破坏。

第二步是检查 free 标志是否已经是 1——如果是，说明这个块已经被释放过了（double-free）。double-free 是一种非常危险的 bug：如果不检查，空闲链表中会出现同一个块被插入两次的情况，后续的 alloc 可能两次分配到同一块内存，导致两个调用者互相覆写对方的数据。free 在检测到 double-free 后打印警告并返回，防止进一步破坏。

### Coalesce：碎片整理的核心算法

碎片化是所有堆分配器的天敌。假设你连续分配了三个 64 字节的块 A、B、C，然后全部释放。如果不做合并，空闲链表中会有三个 64 字节的小空闲块，总空闲空间是 192 字节。但此时如果有人请求分配 128 字节，搜索会失败——没有任何一个空闲块大到能容纳 128 字节，即使三个块加在一起绰绰有余。这就是外部碎片（external fragmentation）——空间总量够，但分布太散，无法满足大块请求。

coalesce 的作用就是把相邻的空闲块合并成更大的块。"相邻"不是指在空闲链表中紧挨着，而是在虚拟地址空间中紧挨着——块 X 的尾部（X 的地址 + HEADER_SIZE + X->size）恰好等于块 Y 的地址。这意味着 X 和 Y 在内存中是无缝连接的，中间没有任何间隙。

Cinux 的 coalesce 算法采用"反复扫描"的策略：用一个 `changed` 标志控制的外层 while 循环，每次内层循环遍历空闲链表寻找与目标块相邻的块，找到就合并并设 changed=true，外层循环再次执行直到没有新的合并发生。为什么要反复扫描？因为一次合并可能创造新的相邻关系——比如先合并 B+C，然后 (B+C) 又和 A 相邻，需要再合并一次。这种"传递性合并"在一次扫描中是做不到的。

内层循环的合并判断分两个方向。方向一：curr 在 block 之前（`curr_addr + HEADER_SIZE + curr->size == block_addr`）——把 block 从链表中摘除，把 block 的 size 加到 curr 的 size 上（再加上一个 HEADER_SIZE，因为两个块之间的那个头部不再需要了），然后让 block 指向 curr（curr 成了合并后的大块）。方向二：block 在 curr 之前（`block_addr + HEADER_SIZE + block->size == curr_addr`）——把 curr 从链表中摘除，把 curr 的 size 加到 block 上。两个方向的合并逻辑是对称的，区别只在于"谁吞并谁"。

合并时从链表中摘除块的操作需要注意——如果被摘除的块恰好是链表头（free_list_ == block 或 free_list_ == curr），需要更新 free_list_ 指向下一个节点。如果不在头部，就需要遍历链表找到被摘除节点的前驱，修改前驱的 next 指针。这是单链表删除操作的固有麻烦——没有 prev 指针，只能从头开始找。

### 自动扩展：堆的动态增长

当 alloc 搜索完整个空闲链表都找不到合适的块时（或者堆刚初始化就被分配了一个超大的请求），它调用 expand 向 VMM 申请更多空间。expand 的工作流程是：先计算需要多少个新页面（至少 4 页 = 16 KB），然后从 PMM 逐页分配物理内存并通过 VMM::map 映射到堆末尾。映射完成后，在新增区域的起始位置创建一个新的空闲块，插入空闲链表，更新堆的总大小。

扩展后，alloc 递归调用自己重试分配。因为 expand 保证新增空间至少有 `min_bytes + HEADER_SIZE` 字节（向上对齐到页面大小，且不少于 4 页），重试时一定能从新创建的空闲块中找到足够的空间。这个递归的终止条件是"新空间足够大"——在 PMM 内存耗尽之前，递归不会超过一层。

一个值得思考的设计问题：为什么最少扩展 4 页而不是 1 页？如果只按需扩展精确大小，每次分配大块都要扩展一次、映射一次，PMM 和 VMM 的调用开销会很高。4 页（16 KB）的批量扩展是一个简单的"预分配"策略——多扩展一点，供后续的小分配使用。Linux 内核和 glibc malloc 都采用类似的策略，只是预分配的量更大（通常是 128 KB 或更高）。

> 参考：[OSDev Wiki - Memory Allocation](https://wiki.osdev.org/Memory_Allocation)（Heap Expansion 章节）
> 参考：[dreamportdev Osdev Notes - Heap Allocation](https://github.com/dreamportdev/Osdev-Notes/blob/master/04_Memory_Management/05_Heap_Allocation.md)

## 动手实现

### Step 1: 实现 Heap::free

**目标**: 安全地回收一个分配块，将其标记为空闲并加入空闲链表，然后调用 coalesce。

**设计思路**: free 做六件事。第一，检查传入的指针是否为 nullptr——如果是，直接返回（free(nullptr) 是安全的 no-op，这是 C 标准要求的行为）。第二，通过 header_from_ptr 定位 BlockHeader——把指针强转为 uintptr_t，减去 HEADER_SIZE，再转回 BlockHeader*。第三，检查 magic 是否为 0xDEADBEEF——如果不是，打印 "Double-free or corruption" 的警告并返回。第四，检查 free 标志是否为 0——如果已经是 1，打印 "Double-free detected" 的警告并返回。第五，更新 used_ 计数器（减去 HEADER_SIZE + block->size），把 block 的 free 标志设为 1，然后把 block 插入空闲链表的头部（block->next = free_list_, free_list_ = block）。第六，调用 coalesce(block) 尝试合并相邻空闲块。

**实现约束**: 函数定义在 `heap.cpp` 中。错误消息通过 kprintf 输出到串口，包含出错地址和 magic 值，方便调试。

**踩坑预警**: free(nullptr) 的 no-op 行为非常重要——C++ 的 operator delete 规定 delete nullptr 是合法的（什么都不做）。如果你的 free 对 nullptr 调用 header_from_ptr，会访问地址 0xFFFFFFFFFFFFFFE0（0 - 32），直接触发 page fault。所以 nullptr 检查必须在 header_from_ptr 之前。

另一个坑：used_ 的减法。正确公式是 `used_ -= HEADER_SIZE + block->size`，不是 `used_ -= block->size`。忘记加 HEADER_SIZE 会导致 used_ 逐渐变成一个不正确的值——永远比实际使用量少 HEADER_SIZE * (分配次数 - 释放次数)。在压力测试中，如果所有块都被释放后 used_ 不是 0，大概率是这里的公式写错了。

**验证**: 分配一个块然后立即释放，调用 dump_stats 检查 used_ 是否恢复为 0。

### Step 2: 实现 Heap::coalesce

**目标**: 将目标块与空闲链表中所有相邻的空闲块合并。

**设计思路**: coalesce 使用一个 changed 标志控制的外层 while 循环。每次内层循环遍历整个空闲链表，对每个 curr 块（跳过目标块自身和非空闲块），检查它与 block 是否相邻。判断相邻的方法是地址计算：如果 `curr_addr + HEADER_SIZE + curr->size == block_addr`，说明 curr 紧挨在 block 前面；如果 `block_addr + HEADER_SIZE + block->size == curr_addr`，说明 block 紧挨在 curr 前面。

合并操作分三部分：第一，把被吞并块的 size 加到存活块上（加上 HEADER_SIZE，因为中间的头部不再需要了）。第二，从链表中摘除被吞并的块——这需要特殊处理链表头的情况。如果被吞并块是 free_list_，直接让 free_list_ 指向它的下一个节点；否则遍历链表找到它的前驱，修改前驱的 next 指针跳过它。第三，如果 curr 吞并了 block，让 block 指针指向 curr（因为 curr 成了合并后的大块，后续循环需要以它为基准继续合并）。

每次合并后设 changed=true 并 break 内层循环，外层循环重新开始扫描。这种"合并一次就重新扫描"的策略保证了传递性合并的正确性——A+B 合并后形成的大块可以和 C 继续合并。

**实现约束**: 函数定义在 `heap.cpp` 中，是 Heap 的私有方法。参数是刚被 free 的那个块的指针。所有地址运算使用 uintptr_t，合并后的 size 用 uint32_t 存储（一个 uint32_t 可以表示最大 4 GB 的 payload，对内核堆来说绑绑有余）。

**踩坑预警**: 合并时从链表中摘除被吞并块的操作有一个容易遗漏的边界情况——如果被吞并块（比如 block）恰好是链表头节点，你不能用"找前驱"的方式摘除它（它没有前驱），必须直接更新 free_list_。这个 bug 在测试中不容易发现，因为大多数情况下刚 free 的块确实在链表头部。但如果释放顺序导致被吞并块不在头部（比如先释放 A，再释放 B，然后 coalesce(A) 时需要摘除 B，而 B 可能不是链表头），就会在链表中留下一个"幽灵节点"——它的内容已经被合并到别的块了，但链表还指向它。

另一个要注意的是合并后 block 指针的更新。方向一（curr 吞并 block）时，`block = curr` 是必须的——否则下一轮循环还会用旧的 block 地址去检查相邻性，而那个地址的内容已经不合法了。方向二（block 吞并 curr）时，block 不需要变，因为合并后的大块起始地址还是 block。

**验证**: 分配三个连续的 64 字节块 A、B、C，然后依次释放 B、A、C。释放 B 时没有相邻空闲块（A 和 C 还在用），不合并。释放 A 时 A 和 B 相邻，合并为 (A+B)。释放 C 时 C 和 (A+B) 相邻，合并为 (A+B+C)。最终空闲链表应该只有一个大块，dump_stats 显示 blocks=1、used=0。

### Step 3: 实现 Heap::expand

**目标**: 在堆末尾映射新的物理页，创建新的空闲块，扩展堆的总大小。

**设计思路**: expand 分五步执行。第一步，计算需要多少个新页面：把 min_bytes + HEADER_SIZE 向上对齐到 PAGE_SIZE，除以 PAGE_SIZE 得到页数。如果页数小于 EXPAND_PAGES（4），强制设为 4——保证每次至少扩展 16 KB。第二步，在堆末尾逐页映射新物理页：从 PMM 分配物理页，用 VMM::map 映射到 `base_ + size_ + offset` 的虚拟地址，标志位是 present + writable。如果 PMM 返回 0（内存不足），打印 OOM 错误并立即返回。第三步，清零新映射的区域。第四步，在新区域的起始位置创建一个空闲块——size 等于新增总大小减去 HEADER_SIZE，free=1，next 指向当前的 free_list_，然后让 free_list_ 指向这个新块。第五步，更新 size_ 总大小。

**实现约束**: 函数是 Heap 的私有方法。需要使用 PMM 的 alloc_page、VMM 的 map，以及 paging_config.hpp 中的 PAGE_SIZE。expand_size 变量必须是 uint64_t 类型，因为涉及到虚拟地址运算。

**踩坑预警**: expand 的映射位置是 `base_ + size_ + offset`——注意是加上当前的 size_（扩展前的总大小），不是加上 size_ + offset。`base_ + size_` 就是堆的当前末尾，新页面紧挨着堆的最后一个字节往后排。如果这里算错了位置，新映射的页面可能覆盖其他内核数据结构或者落到未映射区域。

另一个要注意的是更新 size_ 的时机——必须在创建空闲块之后更新，因为新空闲块的创建依赖于旧的 size_ 值（作为新区域的起始偏移量）。如果你先更新了 size_ 再创建空闲块，空闲块的位置就会偏移到错误的地方。

**验证**: 分配一个超过 64 KB 的块（比如 80 KB），观察串口输出：

```
[HEAP] Expanded by 80 KB, total 144 KB
```

然后 dump_stats 应该显示 total=144 KB，used 包含 80 KB 的分配，free_list 有剩余空间。

### Step 4: 端到端验证——分配释放合并扩展

**目标**: 综合测试 alloc、free、coalesce 和 expand 的协作。

**设计思路**: 写一个完整的测试序列来验证整个流程。第一步，分配三个 64 字节的块 A、B、C，确认三个地址互不相同。第二步，全部释放——先释放 B（中间），再释放 A（前），再释放 C（后）。每一步都应该触发合并。第三步，dump_stats 验证 used=0，free_list 包含一个大块。第四步，分配一个 256 字节的大块——如果合并正确工作，这个分配应该成功（三个 64 字节块加上它们之间的 BlockHeader 被合并成一个大空闲块，足以容纳 256 字节）。第五步，释放这个大块，验证 used 恢复为 0。第六步，分配一个超过初始堆大小的块（比如 80 KB），触发 expand，验证扩展成功。

**踩坑预警**: 在验证合并效果时，不要假设 free_list 中只有一个块——由于 front padding 的存在，某些分配/释放序列可能在空闲链表中留下几个小的不可合并的碎片。重点是验证 used_ 回到 0 以及大块分配能成功，而不是 block 数量的精确值。

**验证**: 在 QEMU 中运行测试：

```bash
cmake --build build && ./build/run_qemu.sh
```

串口输出应该展示完整的分配释放序列，没有 magic 校验错误，没有 OOM 错误，最终 dump_stats 显示合理的数值。

## 构建与运行

本章完成了 `Heap::free()`、`Heap::coalesce()` 和 `Heap::expand()` 三个函数的实现。至此堆分配器的核心功能已经全部到位：init 初始化、alloc 分配、free 释放、coalesce 合并、expand 扩展。

构建后运行 QEMU，串口输出应该包含堆初始化消息以及后续操作（如果有测试代码）的统计信息：

```bash
cmake --build build && ./build/run_qemu.sh
```

## 调试技巧

**free 后 used_ 不为 0**: 在 free 中用 kprintf 打印 `HEADER_SIZE + block->size` 的值，确认减去的量是正确的。如果 used_ 始终比预期大 HEADER_SIZE 的倍数，说明你忘了在减法中包含头部大小。

**coalesce 不工作（合并后仍然是多个小空闲块）**: 在 coalesce 的内层循环中，对每个 curr 打印它的地址、尾部地址（`curr_addr + HEADER_SIZE + curr->size`），以及 block 的地址。检查"curr 尾部 == block 地址"或"block 尾部 == curr 地址"的条件是否曾经满足。最常见的问题是 size 字段记录的是 payload 大小（不含头部），但相邻判断需要加上 HEADER_SIZE——如果你的地址计算漏了 HEADER_SIZE，条件永远不会满足。

**expand 后分配仍然失败**: 检查 expand 是否正确更新了 size_。如果 size_ 没有增长，下一次 expand 会把新页面映射到和上一次相同的位置，覆盖之前的扩展。同时检查新空闲块是否正确插入到 free_list_——如果 next 指针设置错误，新块可能变成了链表末端，但后续的搜索从链表头开始，应该能找到它。

**递归 alloc 栈溢出**: 如果 expand 因为 PMM OOM 失败（没有真正扩展），但 alloc 还是递归调用自己，就会形成无限递归。在 expand 中检查 OOM 后打印错误并返回——不要调用 alloc 重试。alloc 的重试逻辑在 expand 之后执行，如果 expand 没有成功扩展，递归的 alloc 会再次失败再次调 expand，形成循环。在实际内核中这种情况极少发生，但压力测试中要注意。

## 本章小结

| 概念 | 要点 |
|------|------|
| free 安全检查 | nullptr 直接返回，magic 校验防止野指针，free 标志检测 double-free |
| coalesce 相邻判断 | curr_addr + HEADER_SIZE + curr->size == block_addr（或反向） |
| 传递性合并 | changed=true 外层循环，合并后重新扫描，确保 A+B+C 能完整合并 |
| 链表头摘除 | 被吞并块可能是 free_list_ 头节点，需要直接更新 free_list_ |
| expand 流程 | 计算页数 -> 逐页映射 -> 清零 -> 创建空闲块 -> 更新 size_ |
| 最少扩展量 | EXPAND_PAGES = 4（16 KB），避免频繁小扩展的开销 |
| used_ 计算 | free 时减去 HEADER_SIZE + size，全部释放后 used_ 应为 0 |
