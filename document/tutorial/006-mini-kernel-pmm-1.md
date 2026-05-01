# 从零开始的物理内存管理——给内核装上"双手"

> 标签：PMM、位图分配器、E820、物理内存管理、内存字面量
> 前置：[005 Mini Kernel Entry](./005-mini-kernel-entry-3.md)

## 前言

上一章我们让内核有了"嘴巴"——它能通过串口告诉我们 BootInfo 里有什么、E820 内存映射长什么样。但说实话，现在的内核就像一个只会看不会动的旁观者，它清清楚楚地知道物理内存的布局，却连一个字节都分配不出来。想给页表分配一页内存？不行。想给内核栈找个地方？不行。想让进程管理器分配一个 task_struct？更不行。

这对于一个操作系统来说是致命的。内核是整个系统的"大管家"，它连自己的物理内存都管不了，后面的一切都无从谈起。所以这一章我们要给内核装上"双手"——物理内存管理器（Physical Memory Manager，PMM）。我们要从最底层的位图操作开始，一步步实现一个能管理 4GB 物理内存的分配器，让内核真正"掌握"自己的内存资源。

说实话，物理内存管理这个东西，教科书上讲得天花乱坠——伙伴系统、slab 分配器、NUMA 感知——但对于一个刚跑起来的 mini kernel 来说，我们需要的是最简单、最不容易出错的方案。位图分配器就是这样的存在：一个 bit 代表一个页，1 是已用，0 是空闲。128KB 的位图就能管理整个 4GB 地址空间，扫描一遍也就几微秒的事。

## 环境说明

我们的开发环境还是那套熟悉的配置：WSL2 上的 Ubuntu，工具链是 GNU AS（AT&T 语法）+ GCC/G++ + CMake。QEMU 作为模拟器，配置为 128MB 物理内存，通过 `-serial stdio` 和 `-debugcon file:debug.log` 获取输出。内核运行在 64 位长模式、higher-half 配置下，没有标准库、没有异常、没有 RTTI。编译选项是 `-ffreestanding -fno-exceptions -mcmodel=large` 这一套。

## 先把基础设施搭好

### 让数字开口说话——内存大小字面量

写过底层代码的人一定对这种东西不陌生：`4096`、`0x100000`、`0x40000000`。一个数字孤零零地杵在那里，你完全不知道它是 4KB 还是 1MB 还是 1GB，只能靠数零来猜，还特别容易数错。更糟糕的是 `0x200000`——这是 2MB 还是 32MB？答案是 2MB，但你得算半天才能确定。

C++11 给我们提供了一个优雅的解决方案：用户定义字面量。它的原理很简单——我们定义一些 `constexpr` 函数，名字形如 `operator""_KB`，编译器看到 `4_KB` 这个 token 时会自动调用它，返回值直接内联到使用处。因为整个计算是编译期常量表达式，生成的机器码和你手写 `4096` 完全一样，零运行时开销。

```cpp
namespace cinux::mini::mm::literals {

constexpr uint64_t operator""_KB(unsigned long long value) {
    return value * 1024ULL;
}

constexpr uint64_t operator""_MB(unsigned long long value) {
    return value * 1024ULL * 1024ULL;
}

constexpr uint64_t operator""_GB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace cinux::mini::mm::literals

namespace cinux::mini::mm {
    using namespace literals;
}
```

你会发现后缀名都是以下划线开头的——`_KB` 而不是 `KB`。这不是随意的选择，C++ 标准规定不带下划线的字面量后缀是为标准库保留的。虽然大多数编译器不会报错，但这属于未定义行为，你的代码在标准合规性上会有瑕疵。乘法操作中每个因子都显式标记为 `ULL`，这保证了即使在 32 位平台上中间结果也不会溢出。

字面量定义在一个子命名空间 `literals` 里，然后在上一级命名空间 `cinux::mini::mm` 中通过 `using namespace` 导入。这种模式既避免了污染全局命名空间，又让 mm 命名空间内的代码可以直接写 `4_KB`，不用加任何限定符。

### 对齐工具——内存管理的"尺子"

有了字面量，我们还需要一组对齐工具。内存管理里到处需要对齐操作——页表项的物理地址必须 4KB 对齐（Intel SDM Vol.3A Section 4.5 明确要求 PTE 的 bit 11:0 必须为零），E820 区域的边界需要按页对齐，未来的 buddy allocator 更是靠对齐来运作。

```cpp
namespace cinux::mini::mm {

constexpr uint64_t PAGE_SIZE_4K = 4_KB;
constexpr uint64_t PAGE_SIZE_2M = 2_MB;
constexpr uint64_t PAGE_SIZE_1G = 1_GB;

constexpr uint64_t align_up(uint64_t addr, uint64_t align) {
    return (addr + align - 1) & ~(align - 1);
}

constexpr uint64_t align_down(uint64_t addr, uint64_t align) {
    return addr & ~(align - 1);
}

constexpr bool is_aligned(uint64_t addr, uint64_t align) {
    return (addr & (align - 1)) == 0;
}

} // namespace cinux::mini::mm
```

三个页大小常量对应 x86-64 硬件支持的三级页大小。4KB 是标准页，Intel SDM Vol.3A Section 4.5 规定 4KB 页要求地址的低 12 位为零；2MB 大页要求低 21 位为零；1GB 巨型页要求低 30 位为零。2MB 和 1GB 在后续实现虚拟内存管理时才会用到，这里先定义好常量。

对齐函数的原理是经典的位运算技巧：对于 2 的幂次对齐值，`align - 1` 的二进制低位全是 1（比如 `4096 - 1 = 0xFFF`，12 个 1），取反后得到低位全 0 的掩码。`align_up` 先加上 `align - 1` 保证"进位"，然后 AND 掩码完成截断；`align_down` 直接 AND 掩码。这些函数全部是 `constexpr`，可以在编译期使用，也可以在运行时使用。

### PMM 的接口设计——极简主义

现在我们来看 PMM 的公开接口。设计哲学很简单：五个函数覆盖初始化、分配、释放、查询四个核心操作。多一个都是浪费，少一个又不够用。

```cpp
namespace cinux::mini::mm::pmm {

constexpr uint64_t PAGE_SIZE           = 4_KB;
constexpr uint64_t MAX_MEMORY          = 4_GB;
constexpr uint64_t MAX_PAGES           = MAX_MEMORY / PAGE_SIZE;
constexpr uint64_t BITMAP_SIZE         = MAX_PAGES / 8;
constexpr uint64_t LOW_MEMORY_BOUNDARY = 1_MB;

void init(const void* boot_info);
uint64_t alloc_page();
void free_page(uint64_t phys);
uint64_t free_page_count();
uint64_t total_page_count();

} // namespace cinux::mini::mm::pmm
```

常量部分推算一下：`MAX_MEMORY = 4GB`，最多管理 `4GB / 4KB = 1M` 个页，位图大小是 `1M / 8 = 128KB`。`LOW_MEMORY_BOUNDARY = 1MB` 是低内存过滤阈值——Intel SDM Vol.3A Chapter 3 告诉我们，物理地址空间的前 1MB 有特殊的遗留用途：中断向量表（IVT，0x0000-0x03FF）、BIOS 数据区（BDA）、视频内存（0xA0000-0xBFFFF）、以及 BIOS ROM（0xF0000-0xFFFFF），这些区域绝对不能拿来分配。

`init` 接收 `const void*` 而不是 `const BootInfo*`——这是一个减少头文件耦合的设计选择，PMM 的头文件不需要 include `boot_info.h`。`alloc_page` 返回 0 表示 OOM，这是 UNIX 传统——物理地址 0 在低 1MB 保留区内，永远不会被分配出来，所以用 0 做哨兵值是安全的。

### 和其他操作系统的对比——设计哲学的差异

说到物理内存管理，每个操作系统都走过类似的路，但具体实现千差万别。Linux 的启动阶段内存管理经历了三代演进：v1.0 用的是最原始的指针递增（`memory_start` 指针不断前移），v2.3.23 引入了 bootmem 位图分配器（和 Cinux 的 PMM 在概念上完全一样——一个 bit 代表一个页，清零表示可用、置位表示忙碌），现代 Linux 则用 memblock 替代了 bootmem——memblock 基于紧凑的区域数组，用 add/reserve 模型管理内存，避免了位图大小随物理地址空间线性增长的问题。Linux 最终在运行时切换到伙伴系统（buddy allocator），用多级位图管理 4KB 到 512KB 的不同大小块。Cinux 的 PMM 在功能上对应 Linux 的 bootmem 阶段，这是教学内核最合适的起点——简单、直观、容易正确实现。

xv6 选择了完全不同的路线——链表式空闲页管理。每个空闲页的前几个字节存储一个 `next` 指针，形成单链表。`kalloc` 是 O(1) 的链表弹出操作，`kfree` 是 O(1) 的链表头插入操作。xv6 的做法零额外内存开销（空闲列表就住在空闲页本身里），但它无法高效回答"页 X 是否已被分配"这个问题——要回答这个需要遍历整个链表。Cinux 的位图可以用 `test_bit` 在 O(1) 时间内查询任意页的状态，这在调试和防御性检查时非常有用。xv6 还有一个巧妙的调试技巧：`kfree` 时用 `memset(v, 1, PGSIZE)` 把释放的页填充为垃圾数据，帮助检测 use-after-free bug。这个模式值得 Cinux 在未来借鉴。

SerenityOS 的物理页管理则更贴近 C++ 风格——它用引用计数的 `PhysicalPage` 对象来管理物理页的生命周期，每个 `PhysicalPage` 持有一个物理地址和一个引用计数。分配和释放通过智能指针自动管理，避免了手动配对的 alloc/free 调用。这种方式在安全性上优于 Cinux 当前的裸指针接口，但实现复杂度也高得多——它需要内核堆分配器的支持才能创建 `PhysicalPage` 对象，而内核堆分配器又依赖物理页分配器。SerenityOS 通过在启动阶段使用特殊的"快速分配路径"来解决这个鸡生蛋问题。

## 收尾

到这里我们已经定义了 PMM 的完整接口和基础常量。三个头文件（`memory_literals.h`、`mm_defines.h`、`pmm.h`）形成了一个清晰的依赖层次，从底层的字面量工具到上层的 PMM API。在下一篇中，我们将深入 PMM 的核心实现——位图操作、E820 解析、保守初始化策略，以及那个差点让我血压拉满的链接器符号陷阱。

## 参考资料

- Intel SDM: Vol.3A Chapter 3 — 物理地址空间布局，前 1MB 的遗留区域（IVT/BDA/Video/BIOS）
- Intel SDM: Vol.3A Section 4.5 — 4KB 页对齐要求，PTE bit 11:0 必须为零
- OSDev Wiki: [Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — 位图、栈、伙伴系统分配器对比
- Linux bootmem 历史: [LWN.net Articles/761215](https://lwn.net/Articles/761215/) — Linux 从 bootmem 到 memblock 的演进
- xv6 kalloc: [xv6-public/kalloc.c](https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c) — 链表式物理页分配器
