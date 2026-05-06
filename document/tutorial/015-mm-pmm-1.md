# 015-1 从 E820 到位图：让内核看清物理内存的家底

> 标签：PMM, E820, 位图分配器, 物理内存管理
> 前置：[014 键盘驱动](014-driver-keyboard-1.md)

## 前言

到 tag 014 为止，我们的内核已经是一个"能跑能说话"的小家伙了——串口能打印、屏幕能显示、键盘能输入。但如果你问它一个最基本的问题："这台机器到底有多少物理内存？"它只能一脸茫然。我们的内核从第一天起就一直在"裸奔"——没有任何动态内存管理能力，所有空间要么是编译期静态分配的，要么是 bootloader 预留的。从这一章开始，我们要给内核装上一套完整的"记账系统"，让它知道自己有多少家底，哪些能花、哪些不能动。

这一章是整个内存管理子系统的地基。我们要做的第一件事就是搞清楚物理内存的布局——通过解析 BIOS E820 调用留下的数据，建立一张"内存地图"，然后基于这张地图设计一个位图分配器的数据结构。PMM 是后续所有内存管理的基础：没有它，VMM（虚拟内存管理器）无法分配页表页面，堆分配器无法获得内存池，进程地址空间无从谈起。

就像盖房子一样——PMM 是地基，VMM 是框架，堆分配器是内装修。地基没打好，上面再漂亮也是白搭。所以这一章我们要格外仔细，把每一个细节都搞清楚。

我们在 tag 006 中已经在 mini kernel 里实现过一个简易 PMM，这次是它的"大内核升级版"——接口更完整（支持连续分配）、扫描更快（64 位加速）、测试更完善（双轨测试策略）。如果你对 tag 006 的内容还有印象，会发现很多概念是相同的，只是实现更加成熟了。

## 环境说明

- 平台：x86_64, QEMU 默认配置（128MB 内存）
- 工具链：GCC 13+ cross-compiler, CMake + Ninja
- 内核模式：higher-half（VMA = 0xFFFFFFFF80000000）
- 特殊约束：内核禁用标准库和异常，所有内存管理代码在 freestanding 环境下运行

## E820：BIOS 给我们的"财产清单"

在 x86 PC 架构中，物理内存远不是"从 0 开始有一大块连续 RAM"那么简单。真实的物理地址空间充满了各种固定用途的区域——中断向量表在 0x00000000、BIOS 数据区在 0x00000400、VGA 文本缓冲区在 0x000B8000、BIOS ROM 在 0x000F0000，到了高地址还有 PCI MMIO 窗口、ACPI 表等等。BIOS 通过 INT 15h AX=0xE820 这个接口，把这些信息整理成一份"财产清单"交给操作系统。

这个 E820 接口最早出现在 1996 年左右的 BIOS 规范中，目的是替代之前只能报告总内存大小的旧接口（INT 15h AH=0x88）。旧接口只能告诉你"机器有 64MB 内存"，但不知道这些内存分布在哪里、中间有没有 MMIO 洞。E820 的出现让操作系统能精确地知道物理地址空间中的每一处"凹凸不平"——哪些是真正的 RAM，哪些是 PCI MMIO 窗口，哪些是 ACPI 回收区域。对于现代操作系统来说，这是启动阶段最关键的数据之一。

Cinux 的 bootloader 在实模式阶段已经调用了 E820，把结果填入了 BootInfo 结构体的 `mmap[]` 数组中——最多 32 条记录，每条包含 base（起始地址）、length（长度）、type（类型）和 acpi（ACPI 属性）四个字段。我们最关心的是 type=1 的条目，它表示"可用 RAM"——这才是内核可以自由使用的物理内存。

E820 返回的列表既不保证有序，也不保证不重叠。BIOS 的实现质量参差不齐，有些 BIOS 会返回重复的区域或者顺序混乱的条目。好在"无序"对位图操作来说不是问题——我们只是逐个标记每个区域的可用性，顺序无所谓。而"重叠"的情况在 QEMU 和主流 BIOS 中基本不会出现，所以我们暂时不做合并处理——如果将来需要在真实硬件上运行，可以在 parse_memory_map 中添加一个排序和合并步骤。

E820 的 type 字段含义如下：1 = Usable（可用 RAM），2 = Reserved（保留），3 = ACPI Reclaimable（ACPI 数据，启动后可回收），4 = ACPI NVS（ACPI 非易失性存储），5 = Unusable（不可用）。Cinux 只关心 type=1 的条目，其他一律跳过。在 QEMU 的默认配置中，通常会返回 2-3 个 type=1 的条目（低 640KB、1MB 到内存末尾），以及若干 type=2 的保留条目。

现在我们来看 `parse_memory_map` 是如何把 E820 的原始数据转换成内核可以直接使用的干净数据结构的：

```cpp
uint32_t parse_memory_map(const BootInfo& info,
                          MemoryRegion* regions,
                          uint32_t max_regions) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < info.mmap_count && count < max_regions; i++) {
        const auto& entry = info.mmap[i];
        if (entry.type != 1) continue;
```

函数签名接收三个参数：BootInfo 引用（数据源）、MemoryRegion 输出数组（结果）和数组容量。返回值是实际解析出的区域数量。注意这个函数不依赖任何 PMM 内部状态——它是一个纯函数，把它设计成自由函数而非 PMM 类的成员方法是有意为之的，这样 host 端单元测试可以独立测试 E820 解析逻辑，而不需要构造一个完整的 PMM 对象。

循环用双重约束 `i < info.mmap_count && count < max_regions` 来防止越界——既不越界读取 E820 条目，也不越界写入输出数组。第一道关卡就是类型过滤——只放行 type == 1（可用 RAM）的条目，其他值一律跳过。

```cpp
        uint64_t base   = entry.base;
        uint64_t length = entry.length;

        // Filter: everything below 1 MB is reserved
        if (base < LOW_MEM_BOUNDARY) {
            if (base + length <= LOW_MEM_BOUNDARY) continue;
            length -= LOW_MEM_BOUNDARY - base;
            base = LOW_MEM_BOUNDARY;
        }
```

第二道关卡是低内存过滤。`LOW_MEM_BOUNDARY` 设为 0x100000（1MB）。低 1MB 区域在 PC 架构中有特殊地位——那里充满了 IVT（0x0000-0x03FF）、BDA（0x0400-0x04FF）、VGA 文本缓冲区（0xB8000）、BIOS ROM（0xF0000-0xFFFFF）等固件数据结构。这些布局是 PC 兼容机的固件约定（详见 OSDev Wiki 的 "Memory Map (x86)" 页面），并非 Intel 架构规范的一部分——Intel SDM 中并没有规定这些地址的用途。内核不应该碰这些区域。

截断逻辑分两种情况：如果区域完全在 1MB 以下，直接跳过整条；如果跨越 1MB 边界，把基地址截断到 1MB，同时缩短长度——`length -= LOW_MEM_BOUNDARY - base` 先算"低 1MB 部分占了多少"，然后从总长度里减去。注意操作顺序：先减后赋值，不能先改了 base 再算差值。这种截断方式虽然保守——我们白白浪费掉传统低内存中确实空闲的某些部分——但换来了绝对的安全。在一个教学 OS 里，不到 1MB 的浪费完全值得。如果精确排除已知占用区域（IVT、BDA、BootInfo 等），实现复杂度会高得多，而且容易遗漏。

```cpp
        // Align base up, length down to 4 KB
        uint64_t aligned_base = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        length -= (aligned_base - base);
        length &= ~(PAGE_SIZE - 1);

        if (length < PAGE_SIZE) continue;
        regions[count++] = {aligned_base, length};
    }

    return count;
}
```

第三和第四道关卡是 4KB 对齐。Intel SDM Vol.3A Section 4.5 明确规定 long mode 的页表项中物理基地址必须是 4KB 对齐的（低 12 位用作标志位）。所以我们的 PMM 也必须以 4KB 为最小粒度——基地址向上对齐，长度向下截断，不足一页的区域直接丢弃。

对齐的核心公式是 `(base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)`——先用加法把 base "推"到下一个对齐边界（如果它本来就不对齐的话），然后用 AND 操作截断低 12 位。长度的处理类似：先减去对齐带来的偏移量，再用 AND 操作向下截断。这两个操作合在一起保证了输出的区域一定是 4KB 对齐的。

这四道关卡看似简单，但它们共同保证了输出数组的每一条记录都是"干净"的——type 正确、地址在 1MB 以上、4KB 对齐、长度至少一页。后续的 init 流程可以放心地使用这些数据，不需要再做任何额外的边界处理。这种"在入口处一次性清理干净"的设计模式在内核开发中非常常见——把复杂性集中在数据转换的边界上，让核心逻辑保持简洁。

## 位图：一人一页的"签到簿"

有了干净的内存区域列表，接下来需要一个数据结构来跟踪每一页的状态。我们选择了最直观的方案——位图（bitmap）：物理内存中的每一页（4KB）对应位图中的一个 bit，1 表示已占用，0 表示空闲。

位图的内存开销极小：128MB 内存只需要 32768 / 8 = 4096 字节 = 1 个页的位图，即使 4GB 内存也才 128KB。查询操作是 O(1) 的——给定一个物理地址，立刻就能算出对应的 bit 索引，然后用一次数组访问加一次位运算就知道这页是忙是闲。这种"一个 bit 代表一个页"的紧凑表示让位图成为物理内存管理中内存效率最高的数据结构之一。位图的另一个优势是它在物理内存中是连续的，对 cache 友好性来说，扫描连续数组比遍历分散的链表要高效得多。

接下来我们需要设计位图的四个核心操作函数，它们是后面所有分配和释放操作的地基。

为什么没有选更"高级"的方案呢？xv6 用的链表（free list）虽然分配释放都是 O(1)，但无法回答"页 X 是否已分配？"——检查 double-free 需要遍历整个链表。Linux 和 SerenityOS 用的 buddy 分配器虽然支持 O(log N) 连续分配，但实现复杂度远高于位图——SerenityOS 的 `PhysicalZone` 有上千行代码，而我们的整个 PMM 只有 267 行。

位图的四个基础操作极其简洁。`bm_set` 和 `bm_clear` 分别把对应 bit 置 1 和清 0，`bm_test` 检查 bit 是否为 1——这三个都是 O(1) 的单次数组访问加一次位运算。

真正有意思的是 `bm_find_first_free`——线性扫描位图找第一个空闲 bit。Cinux 的做法是把位图数组重新解释为 `uint64_t` 数组，每次检查 64 个 bit：

```cpp
int64_t bm_find_first_free(const uint8_t* bm, uint64_t highest_page,
                           uint64_t bitmap_size) {
    const auto* bm64 = reinterpret_cast<const uint64_t*>(bm);
    uint64_t qword_count = bitmap_size / sizeof(uint64_t);

    for (uint64_t i = 0; i < qword_count; i++) {
        if (bm64[i] != ~0ULL) {
            int bit = __builtin_ctzll(~bm64[i]);
            uint64_t idx = i * 64 + static_cast<uint64_t>(bit);
            if (idx < highest_page) return static_cast<int64_t>(idx);
        }
    }
    // ... tail bytes handling ...
    return -1;
}
```

核心技巧是 `__builtin_ctzll(~bm64[i])`——先对 64 位值取反（把 0 变成 1、1 变成 0），然后用 `ctzll`（count trailing zeros）找到最低位的 1 在哪个位置。编译器会把 `ctzll` 编译为 x86 的 BSF 指令，只需要一个时钟周期。这样一次操作就检查了 64 页，扫描速度最多提升 64 倍。

这里有一个必须强调的安全前提：`__builtin_ctzll` 在输入为 0 时是未定义行为。但由于我们在调用它之前已经检查了 `bm64[i] != ~0ULL`，所以 `~bm64[i]` 保证非零。这个守卫条件绝对不能删掉，否则在位图全满时触发 UB，而且不会 crash，只是返回错误的结果，特别难排查。

这段代码看起来不长，但它浓缩了位操作优化的精华。64 位扫描的理论加速比是 64 倍——逐 bit 扫描需要检查每一个 bit，而 64 位扫描一次就能跳过 64 个已用页。对于 128MB 内存（32768 页 = 4096 字节位图 = 512 个 qword），全满扫描只需 512 次 qword 比较，而逐字节扫描需要 4096 次。`__builtin_ctzll` 被 GCC/Clang 编译为 x86 的 BSF（Bit Scan Forward）或者 TZCNT 指令，只需要一个时钟周期。Linux 的 bootmem 分配器在扫描空闲页时也使用了类似的技巧。

## xv6 做了什么不同？

说了这么多位图的实现，我们来看看别的 OS 是怎么做的。xv6 用了一种完全不同的方案——链表（free list）。每个空闲页的前几个字节被当作一个 `struct run { struct run *next; }` 指针，所有空闲页串成一个单向链表。分配就是从链表头 pop 一个节点，释放就是 push 回去——O(1) 的时间复杂度，零额外内存开销。

这个设计非常巧妙：不需要额外的位图或者数组来跟踪页状态，空闲页本身就是元数据的载体。但它有一个致命的缺点——无法回答"页 X 是否已分配？"。如果你想检查某个物理页是不是被 double-free 了，你必须遍历整个链表。在 xv6 这种小规模系统里这不是问题，但在一个有上百万页的生产系统中，这个操作是不可接受的。另一个限制是链表方案不支持连续分配——因为它不跟踪页的位置关系，只维护一个单链表。

位图方案正好相反——查询是 O(1) 的，但分配是 O(N) 的。不过通过 64 位扫描加速，实际开销已经被大幅降低。而且位图的内存开销很小：128MB 内存只需要 4KB 的位图（恰好一个页），即使 4GB 内存也才 128KB。位图的另一个好处是它在物理内存中是连续的——对于 cache 友好性来说，扫描一个连续数组比遍历一个分散的链表要高效得多。

## Linux 的选择

Linux 的早期启动内存管理经历了一个有趣的演变过程。2.3 之前的版本用最原始的"指针碰撞"分配器——维护一个指针，每次分配就把指针往后移，释放就不管了。2.3.23pre3 引入了 bootmem 位图分配器——和 Cinux 的原理几乎一模一样：一个 bit 对应一个页，保守初始化后按 E820 解锁。bootmem 的设计者选择位图的原因和我们一样——启动阶段代码需要简洁可靠，位图是最不容易出错的方案。

从 4.17 开始，Linux 用 memblock 替代了 bootmem——memblock 是一个基于区段的分配器，不再用位图而是用"保留区域列表"和"可用区域列表"来管理内存。启动完成后，Linux 切换到 buddy 分配器作为运行时的物理内存管理器。Cinux 的 PMM 在概念上就对应 Linux 的 bootmem 阶段——简洁的位图分配器，为后续更复杂的管理器铺路。

这个从简单到复杂的演变路径非常值得学习。很多初学者一上来就想实现 buddy 分配器，但 buddy 的正确实现需要对物理内存布局的精确了解（由 E820 提供）、对页状态的精确跟踪（由位图提供）、以及完善的测试基础设施。Cinux 选择了和 Linux 相同的路径：先用最简单的位图把内存管理跑通，等基础设施成熟后再考虑升级到更高效的分配器。

这种渐进式的开发策略在系统编程中非常重要。每一层抽象都应该独立可验证——PMM 可以通过位图操作来验证，VMM 可以通过页表映射来验证，堆分配器可以通过分配释放循环来验证。如果你一上来就实现一个集成了所有功能的复杂分配器，当出现 bug 时你根本不知道是哪一层出了问题。

Cinux 的 PMM 总共只有 267 行代码（包括注释和空行），但它在 QEMU 和真实硬件上都能正确工作。这证明了"简单即是可靠"的原则——在系统编程中，简洁的代码往往比聪明的代码更可靠。

## PMM 类的接口设计

在深入实现之前，我们先看一下 PMM 类的公共接口。它定义在 `kernel/mm/pmm.hpp` 中：

```cpp
class PMM {
public:
    void init(const BootInfo& info);
    uint64_t alloc_page();       // 分配单页，返回物理地址，0=OOM
    void free_page(uint64_t phys);
    uint64_t alloc_pages(uint64_t count);  // 分配连续多页
    void free_pages(uint64_t phys, uint64_t count);
    uint64_t free_page_count() const;
    uint64_t total_page_count() const;

private:
    void mark_region_used(uint64_t phys, uint64_t length);
    void mark_region_free(uint64_t phys, uint64_t length);

    uint8_t* bitmap_{};
    uint64_t total_pages_{};
    uint64_t free_pages_{};
    uint64_t highest_page_{};
    uint64_t bitmap_size_{};
};
```

几个值得注意的设计决策：所有状态都是实例成员而非 static，这样便于测试和未来的 NUMA 扩展。在实际的生产级内核中，NUMA 系统的每个 node 都有独立的物理内存管理器，实例化设计让这种扩展成为可能。

位图指针用 `uint8_t*` 而非 `uint64_t*`，虽然 64 位扫描时需要 reinterpret_cast，但基础操作（set/clear/test）以字节为单位更直观，也更容易正确处理尾部字节。私有成员全部使用 C++11 类内初始化，默认构造的对象所有指针为 nullptr、所有整数为 0——全局实例 `g_pmm` 在静态初始化阶段就被正确置为零状态，不存在"static initialization order fiasco"问题。全局实例声明为 `extern PMM g_pmm`，整个内核通过 `cinux::mm::g_pmm` 访问唯一的 PMM 实例。

`free_page_count` 和 `total_page_count` 是 const 方法——纯查询，不修改状态。这为后续加锁提供了便利（const 方法通常只需要读锁），而且在测试中它们是几乎所有断言的基础——分配是否生效、释放是否正确，最终都反映在 free_page_count 的变化上。

`highest_page_` 这个名字有点微妙——它实际上存储的是"总页数"（即最大页索引 + 1），而不是"最高页号"。代码中 `highest_page_ = max_addr / PAGE_SIZE` 计算出来的是需要管理的页数，后续被用作 `p < highest_page_` 的循环上界。虽然这个命名不太理想，但在代码逻辑中它的用途是明确的。

`bitmap_size_` 存储的是位图的字节数，用 `(highest_page_ + 7) / 8` 向上取整到字节边界——这是位操作中经典的"除法向上取整"公式。

## 到这里

这一章我们搞定了两件事：从 E820 原始数据中提取干净的可用内存区域，以及设计了位图的核心操作函数。`parse_memory_map` 通过四道过滤关卡（类型过滤、低内存过滤、4KB 对齐、大小检查）把 BIOS 留下的混沌数据整理成了干净的可用内存区域列表。位图辅助函数提供了 O(1) 的精确操作和 O(N/64) 的快速扫描能力。

下一章要把这些零件组装成一套完整的初始化流程——位图放在哪里、如何保护内核占用的内存、如何处理 higher-half 地址转换。那些才是 PMM 真正容易翻车的地方，所以请确保这一章的内容都理解透彻了再继续。

如果你对 E820 的硬件细节还感兴趣，OSDev Wiki 的 "Detecting Memory (x86)" 页面有一个完整的 E820 调用流程和返回值格式的说明。对于位图分配器的更多变体（比如二级索引加速、SIMD 扫描），OSDev Wiki 的 "Page Frame Allocation" 页面也有很好的讨论。

## 参考资料

- Intel SDM: Vol.3A Section 4.5 — 4KB 页的 12 位对齐要求，页表项格式
- [OSDev Wiki - Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) — E820 INT 15h 接口详情，type 字段含义
- [OSDev Wiki - Memory Map (x86)](https://wiki.osdev.org/Memory_Map_(x86)) — PC 低 1MB 物理内存布局约定（IVT、BDA、VGA、BIOS ROM 等）
- [OSDev Wiki - Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — bitmap/stack/buddy 方案比较
- xv6 `kalloc.c`: [GitHub](https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c) — 链表分配器实现
- Linux bootmem: [LWN](https://lwn.net/Articles/761215/) — bootmem 到 memblock 的演变
- SerenityOS `PhysicalZone.h`: [GitHub](https://github.com/SerenityOS/serenity/blob/master/Kernel/Memory/PhysicalZone.h) — 13 级 buddy 分配器实现

> Host 端单元测试覆盖了 `parse_memory_map` 的 5 个边界情况，位图分配器的 7 个场景，以及 mark 操作的计数正确性。所有测试都可以通过 `ctest -L pmm --output-on-failure` 运行。
