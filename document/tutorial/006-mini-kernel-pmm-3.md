# PMM 实现精讲——位图、E820 与扩展方向

> 标签：PMM 实现、位图操作、E820 解析、保守初始化、测试框架
> 前置：[006-2 踩坑实录：链接器符号与入口点](./006-mini-kernel-pmm-2.md)

## 前言

前两篇我们把基础设施搭好了，也踩过了链接器符号和入口点的两个大坑。现在终于可以安心看 PMM 的核心实现了。这一篇我们会完整走读 `pmm.cpp` 的每一行代码，理解位图操作的具体实现、E820 内存映射的解析策略、以及保守初始化的设计考量。最后我们还会搭建一个轻量级的内核测试框架，用六个测试用例覆盖 PMM 的核心功能路径。

读完这篇，你不仅会理解"Cinux 的 PMM 是怎么实现的"，更会理解"为什么这样实现"——每一个设计决策背后都有对安全性和简洁性的权衡。

## 环境说明

开发环境延续前面的配置：QEMU 128MB 内存，debugcon 端口用于调试，串口用于正式输出。PMM 代码位于 `kernel/mini/mm/pmm.cpp`，测试代码位于 `kernel/mini/test/test_pmm.cpp`。构建系统使用 CMake 对象库，生产内核和测试内核共享同一批编译后的 `.o` 文件。

## 位图操作——PMM 的心脏

### 内部状态

```cpp
static uint64_t s_total_pages      = 0;
static uint64_t s_free_pages       = 0;
static uint64_t s_highest_page     = 0;
static uint8_t  s_bitmap[BITMAP_SIZE] = {0};
```

四个静态变量构成了 PMM 的全部状态。`128KB` 的 `s_bitmap` 数组直接嵌入内核的 BSS 段——因为它是 `static` 且初始化为 `{0}`，所以会被放在 BSS 段中，不占用 ELF 文件空间。`boot.S` 中的 `_start` 函数会在启动早期用 `rep stosb` 把 BSS 段清零，所以这个数组的初始值确实是全 0。不过我们马上就会在 `init` 中用 `0xFF` 覆盖它。

这里有一个微妙的点：`BITMAP_SIZE = 128KB` 意味着 PMM 固定消耗 128KB 的物理内存，不管实际有多少 RAM。对于 128MB 的 QEMU 配置来说，128KB 只占千分之一。但如果你的内核只有 2MB，128KB 就占了 6%——这时候可能需要考虑动态大小位图或者用更紧凑的数据结构。

### 位操作的三个原语

```cpp
void set_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] |= (1U << bit_idx);
}

void clear_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] &= ~(1U << bit_idx);
}

bool test_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    return (s_bitmap[byte_idx] & (1U << bit_idx)) != 0;
}
```

这三个函数的代码几乎完全对称——这是好事，对称意味着易理解。每个函数都把页号分解为"第几个字节"和"字节内第几位"，然后用位掩码做 OR（设置）、AND+取反（清除）、AND（测试）。时间复杂度都是 O(1)，因为每次只访问一个字节。

这里用的是 `1U << bit_idx` 而不是 `1 << bit_idx`。虽然 `bit_idx` 的范围是 0-7，用不到更高位，但 `1U` 保证了运算在 `unsigned int` 范围内进行，不会因为 `int` 的符号位扩展产生意外。

### find_first_free——唯一的 O(N) 操作

```cpp
int64_t find_first_free() {
    for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
        if (s_bitmap[byte_idx] != 0xFF) {
            uint8_t byte = s_bitmap[byte_idx];
            for (uint64_t bit_idx = 0; bit_idx < 8; bit_idx++) {
                if ((byte & (1U << bit_idx)) == 0) {
                    return static_cast<int64_t>(byte_idx * 8 + bit_idx);
                }
            }
        }
    }
    return -1;
}
```

这段代码做的是朴素扫描——逐字节检查，找到第一个不是全 1 的字节，再逐位找到第一个 0 位。外层循环有 128K 次迭代，内层最多 8 次。最坏情况下（位图全满）要扫描完整个位图，是 O(N) 的。

和 xv6 做个有趣的对比。xv6 用链表管理空闲页，分配是 O(1) 的弹出操作——从链表头取一个节点就行。但 xv6 无法在 O(1) 时间内回答"页 X 是否已被分配"这个问题。Cinux 的位图用 `test_bit` 可以在 O(1) 时间内查询任意页的状态，代价是分配需要 O(N) 扫描。这是经典的"查询 vs 修改"权衡——位图优化了查询，链表优化了分配/释放。

Linux 的 bootmem 分配器也用位图扫描，但它做了一个小优化：缓存上次扫描位置，下次从缓存位置继续而不是从头开始。这把顺序分配的均摊复杂度从 O(N) 降到了 O(1)。Cinux 没有做这个优化，因为 128KB 位图扫描在现代 CPU 上非常快（缓存友好、顺序访问），优化带来的收益微乎其微。

如果将来 Cinux 需要管理更大的物理地址空间（比如 64GB，对应 2MB 位图），可以用 GCC 内建函数 `__builtin_ctzll`（Count Trailing Zeros）做 64 位字级别的快速扫描——一次检查 64 位而不是 8 位，扫描速度提升近 8 倍。

## E820 解析——从 BIOS 内存映射到可用页表

### 初始化的四步流程

`init` 函数的执行流程可以概括为四步，每一步都有明确的目的：

1. **全部锁定**：位图填 `0xFF`，计数器归零
2. **谨慎开放**：遍历 E820，把 type=1 的区域标记为空闲
3. **保护内核**：把内核自身占用的页标为已用
4. **保护引导器**：把 bootloader 区域标为已用

这个顺序很重要——必须先锁定再开放，反过来就不叫"保守策略"了。如果先全部标为空闲再标记保留区域为已用，你就必须完整列举所有"不可用"区域，漏一个就会出问题。保守策略只需要列举"可用"区域，E820 已经帮你做了这件事。

### 低 1MB 过滤的边界处理

E820 解析中最容易出错的环节是低 1MB 过滤。QEMU 报告的 E820 条目中，第一个通常是 `base=0x0, length=0x9FC00, type=1`——低 640KB 可用内存。这个区域完全在 1MB 以下，直接跳过没问题。

但有些情况下（比如不同的 BIOS 实现或 QEMU 版本），可能会出现跨越 1MB 边界的条目——比如 `base=0x80000, length=0xA0000`（从 512KB 到 1.5MB）。这种情况不能直接跳过，因为 1MB 以上的部分是可用内存。代码中的处理方式是截掉 1MB 以下的部分，保留 1MB 以上的：

```cpp
if (base < LOW_MEMORY_BOUNDARY) {
    if (length <= LOW_MEMORY_BOUNDARY - base) {
        continue;  // 整个区域都在 1MB 以下
    }
    length -= (LOW_MEMORY_BOUNDARY - base);
    base = LOW_MEMORY_BOUNDARY;
}
```

这个数学推导是这样的：如果 `base = 0x80000`（512KB），`LOW_MEMORY_BOUNDARY = 0x100000`（1MB），那么超出部分起始于 1MB，长度是原长度减去 `1MB - 512KB = 512KB`。如果原长度不到 512KB（即整个区域在 1MB 以下），直接跳过。

### 页对齐和残余处理

E820 条目的 base 和 length 不一定是 4KB 对齐的。一个条目可能从 `0x100100` 开始，到 `0x7FF0FF` 结束。我们需要把 base 向上对齐到 `0x101000`（下一个 4KB 边界），结尾向下对齐到 `0x7FF000`（上一个 4KB 边界）。对齐后如果剩余长度不够一个页（4096 字节），整个区域就废弃。

```cpp
uint64_t aligned_base   = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
uint64_t aligned_length = length - (aligned_base - base);

if (aligned_length < PAGE_SIZE) {
    continue;
}
```

这里 `aligned_base` 是向上对齐后的起始地址，`aligned_length` 用原长度减去对齐造成的偏移量得到。Intel SDM Vol.3A Section 4.5 要求 4KB 页的物理地址低 12 位必须为零，所以这一步对齐不是"建议"而是"必须"。

## 页分配与释放

```cpp
uint64_t alloc_page() {
    int64_t page_idx = find_first_free();
    if (page_idx < 0) {
        return 0;
    }
    set_bit(static_cast<uint64_t>(page_idx));
    s_free_pages--;
    return static_cast<uint64_t>(page_idx) * PAGE_SIZE;
}
```

分配逻辑非常直接：找空闲页、标记已用、返回地址。返回的物理地址天然是 4KB 对齐的（`page_idx * 4096` 的低 12 位永远是 0），满足 Intel SDM 对页对齐的要求。

```cpp
void free_page(uint64_t phys) {
    if (phys == 0) {
        return;
    }
    uint64_t page_idx = phys / PAGE_SIZE;
    if (page_idx >= MAX_PAGES) {
        return;
    }
    if (test_bit(page_idx)) {
        clear_bit(page_idx);
        s_free_pages++;
    }
}
```

释放函数有三道防线。空地址（0）静默忽略——这是为了防止"释放空指针"式的误用。越界地址静默忽略——PMM 只管理 4GB 以下。`test_bit` 检查防止双重释放——如果页已经是空闲的，不做任何操作。Linux 的 bootmem 分配器在 `free_all_bootmem_core` 中释放页时也不检查双重释放，但那是启动阶段的批量操作，不会出问题。xv6 的 `kfree` 更暴力——它先 `memset(v, 1, PGSIZE)` 填充垃圾数据再释放，这个技巧能帮你检测 use-after-free，但对双重释放没有防护。Cinux 的 `test_bit` 检查在这一点上比 xv6 更安全。

## 测试框架与验证

### 模拟 BootInfo

测试需要一个可控的输入环境，所以我们构造了一个模拟的 BootInfo。它包含 QEMU 128MB 标准内存布局的三个 E820 条目：低 640KB 可用（type=1）、640KB-1MB 保留（type=2）、1MB-128MB 主内存（type=1）。和生产环境中 QEMU 实际报告的布局非常接近。

### 六个测试用例

测试覆盖了 PMM 的核心功能路径。链接器符号访问测试验证了那个最坑的陷阱——确认 `&__kernel_size` 得到非零值。初始化测试验证了 `total > 0`、`free > 0`、`free < total` 的基本约束。单页分配测试检查了地址非零且 4KB 对齐。多页分配测试批量分配 10 页再全部释放，验证计数一致性。边界测试覆盖了空地址释放、越界释放、双重释放三种异常输入。OOM 测试用只有 2MB 内存的 BootInfo 把分配器耗尽，确认返回 0。

### 和其他 OS 的测试策略对比

xv6 没有独立的内存管理器测试——它的 `kalloc`/`kfree` 通过实际运行用户程序来隐式验证。Linux 的 bootmem 在启动阶段有一个简单的 self-test（检查位图初始化后的总页数是否匹配），但大部分验证靠的是 boot 后的 `dmesg` 输出人工确认。SerenityOS 的 `TestKernel` 是一个独立的测试内核配置，可以在 QEMU 中自动运行并通过退出码报告结果。Cinux 的测试框架最接近 SerenityOS 的模式——独立的测试内核配置、自动运行、退出码报告。

## 收尾

到这里，Cinux 的物理内存管理器已经完整实现并经过测试。位图分配器虽然简单，但它覆盖了物理内存管理最核心的功能——分配、释放、状态追踪。保守初始化策略保证了不会误用保留内存，低 1MB 过滤避开了 x86 的遗留区域陷阱。

回头看，这个 PMM 的实现大概有 250 行代码，加上测试和基础设施大约 500 行。对于一个教学内核来说，这个规模恰到好处——大到能展示真实的设计决策（位图 vs 链表、保守 vs 乐观、静态 vs 动态），小到可以在一个下午理解全部细节。

下一章（`007_mini_kernel_interrupts`）我们要给内核装上"反应神经"——中断和异常处理。有了 PMM 提供的物理页分配能力，我们就能在后续实现虚拟内存管理（VMM），为页表分配物理页、建立映射、实现 demand paging。内存管理的路还很长，但第一步已经稳稳地迈出去了。

## 参考资料

- Intel SDM: Vol.3A Section 4.5 — 4KB 页的 12 位对齐要求，PTE bit 11:0 必须为零
- Intel SDM: Vol.3A Section 2.5 — CR3 持有 PML4 表的物理基地址，PMM 必须先于 VMM 存在
- Intel SDM: Vol.3A Chapter 3 — 物理地址空间前 1MB 的遗留区域
- OSDev Wiki: [Detecting Memory (x86)](https://wiki.osdev.org/Detecting_Memory_(x86)) — E820 INT 15h 接口、entry type 含义
- OSDev Wiki: [Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — 位图/栈/伙伴系统分配器设计对比
- xv6 kalloc: [xv6-public/kalloc.c](https://github.com/mit-pdos/xv6-public/blob/master/kalloc.c) — 链表式 O(1) 物理页分配器
- Linux bootmem: [LWN.net Articles/761215](https://lwn.net/Articles/761215/) — 从 bootmem 到 memblock 的演进
