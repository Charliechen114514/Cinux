# 017-2 First-fit 分配与块分裂

## 导语

上一章我们把堆的骨架搭好了——BlockHeader 定义完毕，init 能映射物理页并创建初始空闲块，dump_stats 能报告堆的健康状况。但这个堆现在还只是个空壳子：它只能坐在那里占据 64 KB 的虚拟地址空间，一个字节都分配不出去。本章我们要给它装上"收银台"——实现 `Heap::alloc()` 函数，让内核真正能按需获取任意大小的内存块。

alloc 的核心逻辑是 first-fit 搜索：从空闲链表头部开始，找到第一个足够大的空闲块，从中切出请求的大小，剩余部分如果足够大就分裂成一个新的空闲块。听起来很简单对吧？但实际实现中有好几个微妙的地方——对齐处理、front padding、最小分裂阈值——每一处都可能藏着一个让你调试到半夜的 bug。

知识前置：你需要理解上一章的 BlockHeader 布局（32 字节头部 + payload）和空闲链表的结构。本章的代码会频繁操作链表节点的插入和删除，所以基本的链表操作要烂熟于心。

本章的核心代码在 `kernel/mm/heap.cpp` 的前半部分（约 1-180 行），包括三个内部辅助函数和 `alloc()` 函数。`alloc()` 是整个堆分配器最复杂也最有趣的部分——它要处理 first-fit 搜索、对齐计算、front padding、块分裂。

## 概念精讲

### First-fit 策略：第一个够大的就是它

堆分配器有多种搜索策略，最常见的三种是 first-fit（首次适应）、best-fit（最佳适应）和 worst-fit（最差适应）。first-fit 从链表头部开始线性扫描，遇到的第一个大小足够的空闲块就拿来用；best-fit 遍历整个链表，找最小的那个足够大的块；worst-fit 找最大的块来分配（试图保留小空闲块的完整）。理论上 best-fit 产生的碎片最少，但每次分配都要遍历整个链表，O(n) 的开销在实践中不可忽视。first-fit 只需要找到第一个够大的就停下来，平均搜索长度是 best-fit 的一半左右。

Cinux 选择 first-fit 的原因很简单——它足够简单，足够快，碎片化程度在内核工作负载下完全可以接受。Linux 内核的 SLOB 分配器也使用 first-fit（在三个按大小分段的链表上），说明这个策略在量产环境中也是可行的。FreeRTOS 的 heap_4 同样使用 first-fit，这基本上是教学和小型 RTOS 的标准选择。

值得注意的是，first-fit 的性能取决于空闲链表的大小和请求的分布。如果空闲链表很长（几百个碎片块），每次分配都要扫描大量小块才能找到够用的。SLOB 通过按大小分级来缓解这个问题——小请求只在小块链表中搜索。Cinux 的内核堆在可预见的未来不会有这么多的空闲块，所以单链表 first-fit 足够用。

### 块分裂：别浪费多余的空间

当我们找到一个空闲块时，它的大小可能远远大于请求的大小。比如请求 64 字节，但找到的空闲块有 60000 字节的 payload 空间。如果把整个 60000 字节都分配出去，浪费就太大了。所以我们需要"分裂"——把原块切成两部分：前面一部分（用户请求的大小 + 头部）变成已分配块，后面剩余的部分变成一个新的空闲块。

分裂发生在空闲块的尾部（tail splitting），而不是头部。这是因为分配块的 header 必须紧贴在 payload 前面（否则 header_from_ptr 无法工作），所以 payload 的位置由对齐计算决定，header 放在 payload 前面，剩余的空间在 payload 之后。Tail splitting 在 `aligned_payload + size` 的位置创建新的空闲块。

但不是所有的剩余空间都值得分裂。如果分裂后剩余部分太小（不够放一个 BlockHeader 加上最小的 payload），那么创建一个几乎无用的碎片块反而会加剧碎片化。Cinux 设定了一个最小分裂阈值 MIN_SPLIT = HEADER_SIZE + 16 = 48 字节——如果尾部剩余空间不足 48 字节，就不分裂，这部分空间就成了"内部碎片"（internal fragmentation），跟着分配块一起被占用，直到这个块被释放。

48 字节这个数字是怎么来的？HEADER_SIZE 是 32 字节，这是创建一个新块头的硬性开销。再加上 16 字节的最小 payload——如果 payload 小于 16 字节，这个空闲块几乎不可能被任何有意义的分配请求命中，它就变成了"死碎片"。所以 32 + 16 = 48 是确保分裂后的新空闲块"有存在价值"的最低门槛。

如果你把 MIN_SPLIT 设得太小（比如 32，即只够放一个 header 没有 payload），空闲链表中会出现很多永远无法满足任何分配的"僵尸块"——它们白白占据了链表遍历的时间，却永远不可能被选中。如果设得太大（比如 128），则过于保守，大量的 tail remainder 被浪费为内部碎片。48 是一个在两者之间的平衡点。

### 对齐：让指针站在整齐的地址上

很多数据结构对内存地址的对齐有要求——比如 SSE 指令要求数据 16 字节对齐，某些硬件 DMA 描述符要求 4096 字节页对齐。Cinux 的 alloc 接受一个 align 参数（默认 16），保证返回的 payload 地址是 align 的整数倍。

对齐处理的关键在于"front padding"（前填充）。假设你找到一个空闲块，它的 payload 区域从地址 0x100020 开始，但你需要 4096 字节对齐。最近的 4096 对齐地址是 0x101000，两者之间有大约 4KB 的间隙。分配器会把 BlockHeader 放在 `0x101000 - 32 = 0x100FE0` 的位置（紧贴在 payload 前面），而 0x100020 到 0x100FE0 之间这段空间就是 front padding。如果这段空间足够大（>= MIN_SPLIT），就可以创建一个小空闲块；否则就作为内部碎片浪费掉。

对齐对性能的影响不仅体现在硬件要求上——对齐的数据访问通常更快。x86-64 的 L1 缓存行是 64 字节，如果一个 16 字节的数据结构跨越了两个缓存行，读取它需要两次缓存访问。如果数据结构对齐到 16 字节，它最多只占据两个缓存行的一半（16 / 64 = 0.25 行），访问效率更高。这就是为什么 alloc 默认 16 字节对齐——不是为了满足硬件的最低要求，而是为了更好的缓存性能。

这里有一个非常关键的设计决策：分配块的 BlockHeader 不一定在空闲块的起始位置！当存在 front padding 时，头部位于 `aligned_payload - HEADER_SIZE`，而不是原来空闲块的开头。这意味着在 free 的时候，从 payload 指针回退 HEADER_SIZE 就能找到正确的头部——而不需要知道原始空闲块从哪里开始。这就是为什么我们有一个专门的 `header_from_ptr` 辅助函数：它永远用 `ptr - HEADER_SIZE` 来定位头部，不依赖任何其他信息。

front padding 的处理方式有两种选择：一是把它浪费掉（内部碎片），二是如果够大就复用成一个小的空闲块。Cinux 选择了后者——当 `front_pad >= MIN_SPLIT` 时，把原始空闲块的 header 缩短，剩余部分变成一个小空闲块。这样即使大对齐请求产生了 front padding，空间也不会被完全浪费。

### alloc(0) 的语义

按照 C 标准库的惯例，`malloc(0)` 的行为是 implementation-defined 的——可以返回 nullptr，也可以返回一个唯一的非空指针。Cinux 选择返回 nullptr，这是最安全的做法——分配零字节在逻辑上没有意义，如果调用者这么做了，很可能是程序有 bug，直接拒绝比返回一个神秘的非空指针要好。

alloc(0) 返回 nullptr 还有另一个好处：它使得"alloc 失败返回 nullptr"的语义保持一致。调用者不需要区分"因为 size=0 返回了 nullptr"和"因为内存不足返回了 nullptr"——两种情况都是"分配没有成功"。

### alloc 返回清零的内存

alloc 在返回 payload 之前会调用 memzero 把整个 payload 区域清零。这不是 C 标准对 malloc 的要求（malloc 返回的内存内容是未定义的），但对内核堆来说是一个合理的默认行为。它消除了信息泄露的风险（上一个使用者的数据不会泄漏给新的分配者），也省去了调用者每次都要手动 memset 的麻烦。Linux 内核的 `kzalloc` 提供同样的"清零分配"语义。

## 动手实现

### Step 1: 实现内部辅助函数

**目标**: 提供 align_up、header_from_ptr 和 memzero 三个辅助函数。

**设计思路**: `align_up(value, align)` 是一个经典的位运算技巧：`(value + align - 1) & ~(align - 1)`。前提是 align 必须是 2 的幂——这个函数只用于内部，调用者保证传入合法的对齐值。`header_from_ptr(ptr)` 从 payload 指针回退 HEADER_SIZE 字节，得到 BlockHeader 的地址——这是一个简单的指针减法，但必须用 `reinterpret_cast` 在 uintptr_t 和指针之间转换。`memzero(start, len)` 逐字节清零——内核里没有 memset 可用（没有标准库），所以需要自己写。

**实现约束**: 三个函数都放在 `heap.cpp` 的匿名命名空间中（内部链接，不暴露给其他编译单元）。align_up 的参数和返回类型是 uint64_t，header_from_ptr 返回 `BlockHeader*`。

这三个函数虽然简单，但每一个都在后续的 alloc、free、coalesce、expand 中被频繁调用。`align_up` 在 alloc 中用于计算对齐 payload 地址，在 init 和 expand 中用于对齐页面大小。`header_from_ptr` 是 free 的第一步操作，每次释放都会调用。`memzero` 在 init、expand 和 alloc 中被调用，确保返回的内存是干净的。

**踩坑预警**: `~(align - 1)` 中的运算符优先级——`~` 的优先级高于 `-`，所以 `~align - 1` 会被解析为 `(~align) - 1`，结果完全错误。必须写成 `~(align - 1)`。这个坑在位操作中非常常见，而且编译器不会给你任何警告。

**验证**: 这些辅助函数的正确性会在后续的 alloc 测试中间接验证——如果 align_up 算错了对齐地址，alloc 返回的指针就不会对齐，测试会立刻失败。

### Step 2: 实现 alloc 的基本框架

**目标**: 搭建 alloc 函数的骨架——处理零大小请求、对齐下限、搜索循环。

**设计思路**: alloc 的入口做三件事。第一，检查 size 是否为 0——如果是，直接返回 nullptr。第二，把 align 调整到至少 16——低于 16 的对齐要求没有意义（BlockHeader 本身就 32 字节，默认对齐不会低于 16）。第三，计算"需要多大的空闲块才能满足这次分配"：needed = size + (align - 1)。这个公式考虑了最坏情况下的对齐开销——payload 可能需要在空闲块内偏移最多 (align - 1) 字节才能对齐。

对齐的最低要求为什么是 16 而不是 8？x86-64 的 System V ABI 要求栈上的数据至少 16 字节对齐，SSE 指令（如 `movaps`）要求操作数 16 字节对齐，否则触发 `#GP`。既然 alloc 返回的内存可能被用于任何数据结构，16 字节对齐是一个安全的最低要求。

然后进入 first-fit 搜索循环。用 prev 和 curr 两个指针遍历空闲链表。对每个 curr 块，首先检查 magic 是否正确——如果不正确，说明堆内存被损坏了，立即报错返回 nullptr。然后检查 curr 是否空闲且大小足够。如果条件满足，就进入实际的分配流程（下一步实现）；否则 prev = curr, curr = curr->next 继续搜索。

**踩坑预警**: needed 的计算可能看起来过于保守——如果空闲块恰好从对齐边界开始，对齐偏移就是 0，不需要任何额外空间。但我们在搜索时不知道空闲块的起始地址是否对齐，所以按最坏情况预估。实际分配时会在循环体内精确计算可用空间，如果不够就跳过（continue）。

另一个容易出错的地方是搜索循环中的 magic 校验位置。如果你把 magic 校验放在 `curr->free && curr->size >= needed` 检查之后，那么已分配的块（free=0）会被跳过而不检查 magic。但空闲链表中不应该包含已分配的块——如果有，说明之前的 alloc 或 free 有 bug 把一个已分配的块错误地留在了链表中。所以先检查 magic 是一个防御措施。

**验证**: 单独测试 alloc(0) 应该返回 nullptr。alloc(1) 应该返回一个非空的、16 字节对齐的指针。

alloc 搜索循环的一个微妙之处是"needed 的计算可能过于保守"。如果空闲块的 payload 恰好从对齐边界开始，对齐偏移就是 0，不需要任何额外空间。但我们在搜索时不知道空闲块的起始地址是否对齐，所以按最坏情况预估。实际分配时会在循环体内精确计算可用空间，不够就跳过——这就是 `usable < size` 检查的作用。

### Step 3: 实现分配核心逻辑（对齐 + 分裂 + 写头部）

**目标**: 在找到合适的空闲块后，计算对齐后的 payload 地址，处理 front padding 和 tail splitting，写入分配块头部。

**设计思路**: 当 first-fit 搜索命中一个足够大的空闲块时，分配过程分六步走。第一步，计算对齐后的 payload 地址：`aligned_payload = align_up(curr_addr + HEADER_SIZE, align)`。这里 curr_addr 是空闲块的起始虚拟地址，加上 HEADER_SIZE 跳过头部，然后向上对齐到 align。第二步，计算分配块头部应该放在哪里：`hdr_addr = aligned_payload - HEADER_SIZE`。注意 hdr_addr 不一定等于 curr_addr——如果需要 front padding，hdr_addr 会比 curr_addr 更高。第三步，计算 payload 区域尾部到块末尾还剩多少空间：`tail_space = block_end - (aligned_payload + size)`。第四步，把当前空闲块从空闲链表中摘除（prev->next = curr->next 或 free_list_ = curr->next）。第五步，处理 front padding——如果 hdr_addr 和 curr_addr 之间的间隙 >= MIN_SPLIT，把原空闲块缩短成一个小的空闲块重新插入链表；否则忽略这段间隙（浪费掉）。第六步，处理 tail splitting——如果 tail_space >= MIN_SPLIT，在分配块之后创建一个新的空闲块并插入链表。最后，在 hdr_addr 位置写入新的 BlockHeader（magic、size、free=0、next=nullptr），更新 used_ 计数器，清零 payload 区域，返回 payload 指针。

这六步中，从链表摘除当前块（第四步）必须在处理 front padding 和 tail space（第五六步）之前。原因是：front padding 可能复用当前块的 header（curr 保留在链表中成为一个小空闲块），tail space 会创建新的空闲块并插入链表头部。如果先处理 front padding 再摘除，链表操作会变得混乱——curr 可能已经不再是链表中的节点，但 prev 仍然指向它。先摘除再重新挂载，逻辑更清晰。

这里有一个容易忽略的检查：即使 curr->size >= needed（空闲块按最坏估计够大），对齐后的实际可用空间 `usable = block_end - aligned_payload` 可能小于 size。这种情况发生在空闲块虽然"按最坏估计够大"，但实际对齐后 payload 可用空间不够——此时必须跳过这个块，继续搜索下一个。

这个检查在实际中很少被触发，但在某些边界情况下会出现。例如：空闲块有 100 字节 payload，用户请求 80 字节但需要 4096 对齐。如果空闲块的 payload 区域恰好在 4096 边界之前 100 字节处，对齐后 aligned_payload 被推到下一个 4096 边界，但 block_end 在 aligned_payload 之前——usable 为负数（作为无符号整数会下溢为巨大值），但 `usable < size` 检查会正确捕获这种情况。

**实现约束**: 所有指针运算通过 `reinterpret_cast<uintptr_t>` 和 `reinterpret_cast<BlockHeader*>` 在整数和指针之间转换。尾部新空闲块的 BlockHeader 位于 `aligned_payload + size` 处，需要设置 magic、size（tail_space - HEADER_SIZE）、free=1，然后插入空闲链表头部。

**踩坑预警**: 这里的最大陷阱是 front padding 场景下的 header_from_ptr。假设空闲块从地址 A 开始，但分配的 payload 在对齐后的地址 P，头部放在 P - 32。当你后来调用 free(P) 时，header_from_ptr 会计算 P - 32，找到正确的头部——但如果你的实现不小心把头部放在了 A（空闲块的起始位置）而不是 P - 32，free 的时候就找不到正确的头部，magic 校验会失败。所以，务必把分配块的头部写在 `aligned_payload - HEADER_SIZE`，而不是空闲块的起始位置。

这个陷阱在默认 16 字节对齐下不会触发——因为 BlockHeader 恰好 32 字节（32 是 16 的倍数），空闲块的 payload 起始地址和 allocated payload 地址相同，不需要 front padding。只有当你测试更大的对齐值（如 4096 字节）时，这个 bug 才会浮出水面。所以"alloc(64, 4096) 然后 free"是一个必须包含在测试中的用例。

另一个坑：front padding 空闲块的 next 指向原来的 free_list_（插入到链表头部），而不是 null。如果你忘了设置 next，这个空闲块就变成了链表的"终点"，后面所有空闲块都无法被搜索到。

tail splitting 创建的新空闲块也有类似的坑——新块的 next 必须指向当前的 free_list_（不是 nullptr）。如果你把 `rem->next` 设为了 nullptr，这个新块就变成了链表的末尾，之前的所有空闲块都被"遗忘"了。这种 bug 在分配少量块时不会出现（因为链表很短），但在压力测试中会导致"堆空间明明够用但分配失败"的现象。

**验证**: 实现完成后，可以写一个简单的测试——连续分配三个 64 字节的块，验证它们返回的指针互不相同且都是 16 字节对齐的。然后调用 dump_stats，检查 used 和 free_total 的数值是否符合预期。

你也可以用 dump_stats 来验证块分裂是否正确工作。分配一个 64 字节的块后，空闲链表中应该只剩一个块（初始大块的 tail remainder），它的 size 应该约等于 63 KB - 64 = 约 64480 字节（再减去一些 header 开销）。如果 tail splitting 没有执行（free_list 为空），说明 MIN_SPLIT 的值设得太大或者 tail_space 的计算有误。

### Step 4: 处理堆空间不足的情况

**目标**: 当空闲链表中找不到合适的块时，调用 expand 扩展堆区域，然后重试分配。

**设计思路**: 搜索循环正常结束后如果还没有找到合适的块（curr 变成了 nullptr），说明当前堆空间不够用了。此时调用 `expand(size + align + HEADER_SIZE)` 向 VMM 申请新的虚拟地址空间。expand 函数会在堆的末尾映射新的物理页并创建新的空闲块（下一章详细讲解）。扩展完成后，递归调用 `alloc(size, align)` 重试分配。这个递归是安全的——expand 保证至少添加 size + align + HEADER_SIZE 字节的新空间，所以重试时一定能找到足够大的空闲块，不会无限递归。

**踩坑预警**: 理论上，如果 PMM 在 expand 过程中耗尽物理内存，expand 会提前返回（不完整扩展），然后重试时可能再次搜索失败，再次调用 expand，如此循环直到栈溢出。在实践中，内核堆的扩展需求不会太大（几百 KB 到几 MB），128 MB 的 QEMU 内存绰绰有余。但如果你在做压力测试时内核突然 triple fault，可能是这个递归在极端情况下把栈撑爆了。

一个更安全的做法是把递归改为循环——在 alloc 末尾用 `goto retry` 或者把整个搜索逻辑包在 `while(true)` 循环里。但递归在这里最多只有一层（expand 保证映射足够空间），所以实际上不是问题。只有当 expand 因为 OOM 失败时才会出现无限递归，而 OOM 本身就是系统无法继续工作的信号。

**验证**: 分配超过 64 KB 的内存（比如分配一个 80 KB 的块），应该能触发 expand。串口输出中会看到 `[HEAP] Expanded by XX KB, total XX KB` 的消息，然后分配成功返回。

## 本章小结

本章实现了 alloc 的完整逻辑——first-fit 搜索、对齐计算、front padding、块分裂、以及空间不足时的自动扩展。alloc 是堆分配器中最复杂的函数，它需要在一次链表遍历中同时处理搜索、对齐和分裂三个需求。理解了 alloc 之后，下一章的 free、coalesce 和 expand 就相对简单了。

## 构建与运行

本章完成了 `Heap::alloc()` 的实现，在 `heap.cpp` 中新增了约 100 行代码（包含辅助函数和搜索逻辑）。构建后运行 QEMU 测试：

```bash
cmake --build build && ./build/run_qemu.sh
```

如果你在 main.cpp 中临时添加几个 alloc 调用和 dump_stats，串口输出应该类似：

```
[HEAP] Initialised at 0xFFFF800000000000, size 64 KB
[HEAP] Stats: total=64 KB, used=0 KB, free_list=63 KB, blocks=1
```

分配几个块后再调用 dump_stats：

```
[HEAP] Stats: total=64 KB, used=1 KB, free_list=62 KB, blocks=1
```

注意 `used` 和 `free_list` 的和不等于 `total`——差值是 header 开销和内部碎片。每次分配的 used 增量是 `HEADER_SIZE + size`（不只是 size），所以分配一个 64 字节的块实际上消耗了 96 字节（32 header + 64 payload）。

## 调试技巧

**alloc 返回的指针不对齐**: 在 alloc 的搜索循环里，`aligned_payload` 的计算依赖于 `align_up`。如果你发现返回的地址 `% 16 != 0`，检查 align_up 的实现——最常见的问题是 `~(align - 1)` 写成了 `~align - 1`。你也可以用 kprintf 打印 `aligned_payload` 的值和 `aligned_payload % align`，确认对齐计算正确。

**分配后写入数据但读回不一致**: 如果写入 payload 的数据和读回的不同，可能是 VMM 的映射有问题——两个不同的虚拟地址映射到了同一个物理页，或者某个虚拟地址没有被映射。在 QEMU 中可以使用 `info tlb` 命令检查 TLB 内容，确认堆区域的虚拟地址确实映射到了正确的物理页。

**分配后 free_list 为空**: 如果分配了一个小块（比如 64 字节）后 dump_stats 显示 blocks=0，说明 tail splitting 没有执行。检查 tail_space 的计算——可能是 block_end 算错了，或者 MIN_SPLIT 的值设得太大。正确的 tail_space 应该是 `block_end - (aligned_payload + size)`，也就是分配块之后到空闲块末尾的剩余字节数。

**front padding 导致 free 失败**: 如果在测试大对齐（4096）时 free 报 "magic mismatch"，检查分配块的头部是否写在了 `aligned_payload - HEADER_SIZE` 而不是空闲块的起始地址。正确的 header_from_ptr 逻辑是 `ptr - HEADER_SIZE`——如果 free 时 payload 指针减去 32 后找不到正确的 magic，说明分配时头部位置写错了。

**多次分配后地址重叠**: 如果两个 alloc 调用返回了重叠的地址范围，说明 tail splitting 创建的新空闲块的位置或大小不对。新块的 BlockHeader 应该位于 `aligned_payload + size`，它的 size 应该是 `tail_space - HEADER_SIZE`。如果新块的位置算错了（比如忘了加 HEADER_SIZE），它的 payload 区域就会和上一个分配块重叠。

**used_ 的值不准确**: 如果分配后 used_ 的增长量不是 `HEADER_SIZE + size`，说明计算公式写错了。常见的错误是只加了 `size` 没加 `HEADER_SIZE`，或者把 `size` 算成了 `needed`（包含了 alignment padding）。正确的做法是 `used_ += HEADER_SIZE + size`，其中 `size` 是用户请求的原始大小，不包含对齐 padding。

## 本章小结

| 概念 | 要点 |
|------|------|
| First-fit | 线性扫描空闲链表，找到第一个足够大的块 |
| 对齐计算 | aligned_payload = align_up(block_start + HEADER_SIZE, align) |
| Front padding | 头部位于 aligned_payload - HEADER_SIZE，间隙可能成为小空闲块 |
| Tail splitting | 剩余空间 >= MIN_SPLIT (48) 时创建新空闲块 |
| MIN_SPLIT | HEADER_SIZE(32) + 16 = 48 字节，确保新空闲块有存在价值 |
| alloc(0) | 返回 nullptr，拒绝零大小请求 |
| 搜索失败 | 调用 expand 扩展堆区域，递归重试分配 |
| needed 计算 | size + (align - 1)，按最坏对齐开销预估所需空间 |

## 扩展方向

如果你想进一步改进 alloc 的性能，有几个方向可以考虑：

**分离空闲链表**：将空闲块按大小范围分到不同的链表中（如 SLOB 的三链表设计）。小请求（< 256 字节）只搜索小链表，减少搜索时间。这在空闲块数量很多时效果明显，但对于几十个块的内核堆来说改善有限。

**Best-fit 搜索**：遍历整个链表找到最小的够用块。理论上碎片更少，但每次分配都是 O(n)。可以通过维护按大小排序的链表来折中——插入时按大小排序（O(n)），但搜索时可以提前终止。

**Buddy 系统替代**：用 buddy 分配器替代 free list，合并操作变为 O(1)。但 buddy 系统只能分配 2 的幂次大小的块，内部碎片更大。适合作为物理页管理（Linux 的做法），不太适合内核堆的任意大小分配。

**缓存最近释放的块**：维护一个小的"最近释放"缓存（比如最近释放的 4 个块），alloc 时先在缓存中查找。刚释放的块很可能还在 CPU 缓存中，命中率更高。这是 glibc malloc 的 ptmalloc 使用的策略之一。

alloc 是堆分配器中最复杂的函数。理解了 first-fit 搜索、对齐计算、front padding 和 tail splitting 之后，后面的 free、coalesce 和 expand 相对来说就更容易理解了。下一章我们将实现这三个函数，完成堆分配器的核心功能。

到目前为止，本章的代码约 100 行，覆盖了 alloc 的完整实现路径。每个分支（零大小、搜索命中、搜索失败）都有对应的处理逻辑。在正常的使用场景下，alloc 的大部分时间花在搜索循环中——遍历空闲链表找到第一个够大的块。

构建命令和运行方式和上一章完全相同。本章新增的代码全在 `kernel/mm/heap.cpp` 中，不需要修改 `CMakeLists.txt`。

如果你在测试中遇到 alloc 返回 nullptr 但堆明明有空间的情况，请参考上面的"调试技巧"部分逐一排查。
