# 016-1 从大页到 4KB 页：x86-64 页表项详解

> 标签：x86-64, 分页, PageEntry, TLB, paging_config
> 前置：[015-3 分配、释放与测试](015-mm-pmm-3.md)

## 前言

在 tag 013 里，bootloader 帮我们把内核映射进了虚拟地址空间——用的是 2MB 大页，直接往 PD（Page Directory）里塞条目，简单粗暴。tag 015 又给了我们 PMM，内核终于能动态分配物理页了。但你如果回想一下那个 `map_mmio` 函数，里面全是 `0x83` 这种魔数，TLB 刷新是手写的内联汇编，页表条目就是一个裸 `uint64_t`——所有操作都靠位掩码硬算。说白了，我们一直在用最原始的方式"手动拨盘"，连个像样的仪表盘都没有。

到了 tag 016，Cinux 要建一套真正的虚拟内存管理器（VMM），而 VMM 的根基就是精确到 4KB 的四级页表操作。2MB 大页做粗粒度映射挺好，但你没法在一个 2MB 页里只映射 4KB——硬件不允许。如果我们想让用户进程拥有独立的地址空间、想实现按需分页（demand paging）、想在将来做 copy-on-write 的 fork()，就必须走进 4KB 页的粒度，就必须能精细地操作每一个页表项（PTE）。这就是这一章要做的事情：先把"操作页表项"这件事本身做对，把基础设施铺好，然后再用它们去搭 VMM。

具体来说，本章覆盖三块内容——`paging_config.hpp` 把所有分页常量从魔数变成有名字的符号；`PageEntry` 联合体让 C++ 代码可以直接按名字读写 PTE 的每一个字段；TLB 刷新和 CR3 访问器则给了我们通知硬件"页表改了"的手柄。

## 环境说明

我们仍然在 x86-64 的 QEMU 环境下工作，工具链是 GCC cross-compiler（`x86_64-elf`），Cinux 内核运行在 higher-half 模式（`KERNEL_VMA = 0xFFFFFFFF80000000`）。当前的分页结构是 bootloader 建立的四级页表（PML4 -> PDPT -> PD -> PT），前 1GB 用 2MB 大页 identity mapping，内核镜像通过 higher-half 映射访问。PMM 已经就绪（tag 015），可以随时分配 4KB 物理页——这正是新页表所需的粒度。

## 消灭魔数：paging_config.hpp

### 问题——代码里散落的 0x83 和 512

如果你翻看 tag 015 的 `paging.cpp`，你会看到这样的代码：`PD_HUGE_PAGE_FLAGS = 0x83`、`PT_ENTRIES = 512`、手写的 `__asm__ volatile("invlpg ...")`。这些常量不仅在这一个文件里出现——`vmm.cpp` 要用移位量和索引宏，`exception_handlers.cpp` 要用 `FLAG_PRESENT` 来构造 PTE——如果每个文件自己定义一遍，不但维护成本高，改一个地方忘改另一个地方就是定时炸弹。所以 tag 016 做的第一件事不是写新功能，而是把所有分页常量集中到一个头文件里。

### 常量定义与背后的数学

```cpp
// kernel/arch/x86_64/paging_config.hpp
namespace cinux::arch {

constexpr uint64_t PAGE_SIZE   = 4096;
constexpr uint32_t PAGE_SHIFT  = 12;
constexpr uint32_t PT_ENTRIES  = 512;

constexpr uint32_t PT_SHIFT   = 12;
constexpr uint32_t PD_SHIFT   = 21;
constexpr uint32_t PDPT_SHIFT = 30;
constexpr uint32_t PML4_SHIFT = 39;
```

开头三个常量是分页系统的数学基础。x86-64 的标准页大小是 4096 字节，即 2^12，所以 `PAGE_SHIFT = 12`。每一级页表恰好有 512 个条目，因为虚拟地址中每级索引占 9 个 bit（2^9 = 512），而一个页表占满一整页：512 x 8 字节 = 4096 字节——这并不是巧合，而是硬件设计者刻意为之的对齐。

四个移位常量直接来自 Intel SDM Vol.3A Section 4.5.4（pp.4-22 到 4-25）对 64 位虚拟地址的位域划分。PT 索引在 bit 20:12（移位 12），PD 索引在 bit 29:21（移位 21），PDPT 索引在 bit 38:30（移位 30），PML4 索引在 bit 47:39（移位 39）。每一级比上一级多吃 9 个 bit，移位量每次加 9。这些数字不是随意的，它们是硬件规定的，软件必须遵守。

接下来是地址掩码和标志位掩码：

```cpp
constexpr uint64_t ADDR_MASK = 0x000FFFFFFFFFF000ULL;
constexpr uint64_t FLAG_MASK = 0xFFF0000000000FFFULL;
```

`ADDR_MASK` 提取 PTE 的 bit 51:12，共 40 位——这是物理页帧号所在的位置。bit 63 是 NX 位，bit 62:52 是可用位/保留位，都不属于物理地址。`FLAG_MASK` 则是 `ADDR_MASK` 的补集，覆盖低 12 位标志和高位标志，主要用于在映射时分离物理地址和权限控制位。

### 标志位：每一位都是硬件契约

```cpp
constexpr uint64_t FLAG_PRESENT  = 1ULL << 0;
constexpr uint64_t FLAG_WRITABLE = 1ULL << 1;
constexpr uint64_t FLAG_USER     = 1ULL << 2;
constexpr uint64_t FLAG_PWT      = 1ULL << 3;
constexpr uint64_t FLAG_PCD      = 1ULL << 4;
constexpr uint64_t FLAG_ACCESSED = 1ULL << 5;
constexpr uint64_t FLAG_DIRTY    = 1ULL << 6;
constexpr uint64_t FLAG_HUGE     = 1ULL << 7;
constexpr uint64_t FLAG_GLOBAL   = 1ULL << 8;
constexpr uint64_t FLAG_NX       = 1ULL << 63;
```

这组定义和 Intel SDM Vol.3A Table 4-20（p.4-31）的 PTE 格式一一对应，我们挑几个关键的来说。bit 0 的 Present 位是整个分页系统的"开关"——当 P=0 时，MMU 拒绝翻译，直接触发 #PF。这正是 demand paging 的前提：先把 PTE 的 P 位清零，等访问发生时在缺页处理函数里分配物理页、填好 PTE、再恢复执行。bit 1 的 Writable 控制写权限，bit 2 的 User 允许 Ring 3 访问——这两位组合出经典的读/写/执行权限矩阵。

bit 5 和 bit 6 的 Accessed 与 Dirty 是 CPU 硬件自动设置的"粘滞位"（SDM Vol.3A Section 4.8, p.4-39）。CPU 通过某个 PTE 做翻译就置 Accessed，对该页写入就置 Dirty，而且 CPU 永远不会帮你清零。对于现在的 Cinux 来说这两位暂时没用，但将来做页面置换算法时，它们就是最基本的热度信号。

bit 7 的 Huge 在不同层级有不同含义——在 PDPT 层置位是 1GB 大页，在 PD 层置位是 2MB 大页，在 PT 层则变成 PAT（Page Attribute Table）位。Cinux 用 `FLAG_HUGE` 统一命名，因为在非叶节点上它的语义确实就是"跳过下一级页表，直接映射大页"。bit 63 的 NX（No eXecute）禁止在该页上执行代码，是 W^X 安全策略的硬件基础。

### 索引提取宏

```cpp
constexpr uint64_t PML4_INDEX(uint64_t virt) { return (virt >> PML4_SHIFT) & 0x1FF; }
constexpr uint64_t PDPT_INDEX(uint64_t virt) { return (virt >> PDPT_SHIFT) & 0x1FF; }
constexpr uint64_t PD_INDEX(uint64_t virt)   { return (virt >> PD_SHIFT) & 0x1FF; }
constexpr uint64_t PT_INDEX(uint64_t virt)   { return (virt >> PT_SHIFT) & 0x1FF; }
```

四个函数做的事情完全一样——右移对应位数，然后和 `0x1FF`（9 个 1）做与操作，提取 9 位索引。它们都被标记为 `constexpr`，这意味着编译器在处理编译期已知的虚拟地址时可以直接算出索引，完全消除运行时开销。你会在下一章看到，这四个宏是 VMM 四级遍历的"导航工具"。

## PageEntry 联合体：C++ 位域直面硬件 PTE

### 为什么用联合体

有了常量，接下来是数据结构。Intel 规定每个页表项是一个 64 位整数，按位域解释：Present 在 bit 0，Writable 在 bit 1，物理地址在 bit 51:12。我们在 C++ 里需要两种访问方式——有时候需要整体读写一个 64 位值（比如 `entry.raw = phys | flags`），有时候需要按名字操作单个字段（比如检查 `entry.present` 是否为 1）。`PageEntry` 联合体正是为这两个需求而生的：

```cpp
union PageEntry {
    uint64_t raw;

    struct {
        uint64_t present  : 1;
        uint64_t writable : 1;
        uint64_t user     : 1;
        uint64_t pwt      : 1;
        uint64_t pcd      : 1;
        uint64_t accessed : 1;
        uint64_t dirty    : 1;
        uint64_t huge     : 1;
        uint64_t global   : 1;
        uint64_t _avail   : 3;
        uint64_t addr     : 40;
        uint64_t _avail2  : 11;
        uint64_t nx       : 1;
    };

    uint64_t phys_addr() const { return raw & ADDR_MASK; }
    void set_phys_addr(uint64_t phys) {
        raw = (raw & ~ADDR_MASK) | (phys & ADDR_MASK);
    }
    bool is_present() const { return (raw & FLAG_PRESENT) != 0; }
};

static_assert(sizeof(PageEntry) == 8, "PageEntry must be 8 bytes");
```

位域的排列顺序严格遵循 SDM Vol.3A Table 4-20 的 PTE 格式，从 bit 0 排到 bit 63。GCC 和 Clang 在 x86-64 小端序下按照从低位到高位的顺序排列位域，所以 `addr : 40` 被正确地放在了 bit 12 到 bit 51 的位置。`static_assert` 在编译期验证这一点——如果编译器的位域布局和硬件 PTE 格式不匹配，`PageEntry` 的大小就会偏离 8 字节，编译直接报错，绝不让问题溜到运行时。

三个 helper 方法封装了最频繁的操作。`phys_addr()` 用 `ADDR_MASK` 从 raw 中提取物理地址——你可能会问为什么不用位域的 `addr` 字段？因为位域访问依赖编译器对 40 位宽字段的语义理解，而直接做掩码操作是更加无歧义、也更易优化的方式。`set_phys_addr()` 用"保留标志位、替换地址位"的经典手法——`raw & ~ADDR_MASK` 把地址位清零，`| (phys & ADDR_MASK)` 拼入新地址。`is_present()` 用 `raw & FLAG_PRESENT` 而不是直接返回 `present`，同样的道理——一次 AND 操作比位域访问更可靠。

### 设计对比：Cinux vs xv6 vs Linux vs SerenityOS

说到页表项的数据结构设计，这里是一个非常有意思的工程选择分叉点，不同的 OS 根据自己的目标做出了截然不同的决策。

Cinux 选择了联合体 + 位域，理由很简单：只面向 x86-64 一种架构，不存在跨架构适配的需求，位域的可读性优势可以充分发挥——`entry.present` 比 `(entry >> 0) & 1` 直观太多，而且 `static_assert` 消除了"编译器可能不按预期排列位域"的顾虑。

xv6（RISC-V 版）的做法和 Cinux 类似但不完全相同。xv6 的 PTE 操作用的是裸 `uint64_t` 加一组掩码宏（`#define PTE_V (1L << 0)`、`#define PTE_R (1L << 1)`），用 `pa2pte()` 和 `pte2pa()` 在物理地址和 PTE 之间转换。本质上和 Cinux 的 `raw & ADDR_MASK` 是同一种思路，但 xv6 不用位域，因为 RISC-V 的 Sv39 PTE 格式中物理地址不是连续的（bit 53:10 是 PPN，中间有空隙），C++ 位域不好直接映射。xv6 的 walk 函数手动做移位和掩码来提取每一级索引——和 Cinux 的 `PML4_INDEX` / `PD_INDEX` 宏做的是同一件事，只是 xv6 直接内联在代码里而不提取成函数。

Linux 的做法最极端——完全不用位域，全部是 raw 值操作加架构相关的宏/内联函数。`pgtable.c` 里你会看到 `pte_val(pte) & _PAGE_PRESENT` 这种代码，每一层都通过 `pgd_val()`、`pud_val()`、`pmd_val()`、`pte_val()` 这样的访问器来解引用。这样做的原因是 Linux 必须支持 x86、ARM、RISC-V、LoongArch 等十几种架构，每种架构的 PTE 格式完全不同，位域根本无法统一。而且 Linux 在 x86-64 上支持 5 级分页（PGD -> P4D -> PUD -> PMD -> PTE），中间多了一层 P4D（Page 4-level Directory），在只支持 4 级的硬件上通过"折叠"（folding）来跳过这一层——这种灵活的层级处理用位域是做不到的。Linux 的 `flush_tlb_page()` 和 `flush_tlb_mm()` 也是架构相关的，x86 上用 INVLPG 和 CR3 重载，ARM 上用 TLBI 指令，RISC-V 上用 `sfence.vma`——对比之下 Cinux 的 `flush_tlb()` 只需要管 x86-64 一种情况。

SerenityOS 的选择和 Linux 类似但工程风格不同。它用 `PageTableEntry` 类封装 PTE，通过 getter/setter 方法（`is_present()`、`set_physical_address()`）操作字段，内部实现也是掩码操作而非位域。SerenityOS 的 `MemoryManager` 是一个庞大的单例，管理所有物理内存的映射——它把全部物理内存通过固定的 offset 直接映射到内核虚拟地址空间，这样页表内容的读写就不需要做 `phys_to_virt` 转换，直接加偏移量就行。这个设计简化了页表遍历（不需要每一步都转换地址），代价是内核虚拟地址空间的浪费——对于现在的 Cinux 来说，物理内存不大，还不值得做这种全映射。

总结一下设计选择的光谱：Cinux 用位域追求可读性和编译期校验，xv6 用裸值 + 宏追求简单透明，Linux 用多层抽象追求跨架构统一，SerenityOS 用类封装追求类型安全和全物理映射的便捷性。每种选择都有充分的理由，关键是理解自己的约束条件。

## TLB 刷新：告诉硬件"页表改了"

### 为什么需要手动刷新

页表修改完之后，你可能会觉得"好了，映射生效了"——但 CPU 内部的 TLB（Translation Lookaside Buffer）还在用旧的翻译结果缓存。CPU 根本不知道你改了页表——它只管查 TLB，命中就直接用。这意味着你把虚拟地址 0x200000 映射到了新的物理页，但 CPU 依然往旧物理页上写数据，而且不会报任何错误。这种 bug 非常阴险，因为"看起来程序在正常执行"，只是数据默默写到了错误的地方。Intel SDM Vol.3A Section 4.10（pp.4-43 到 4-52）明确说了：软件修改 paging structure 之后，必须显式通知处理器。

### 两种刷新粒度

```cpp
inline void flush_tlb(uint64_t virt) {
    __asm__ volatile("invlpg (%0)" : : "r"(virt) : "memory");
}

inline void flush_tlb_all() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
```

`flush_tlb` 用 `invlpg` 指令使单个虚拟地址的 TLB 条目失效——这是 SDM 推荐的单页刷新方式，只影响一个虚拟地址对应的 TLB 缓存和相关 paging-structure cache 条目，不会打扰其他页的翻译。在 VMM 的 `map` 和 `unmap` 操作中，每次只改一个 PTE，用 `invlpg` 是最高效的选择。

`flush_tlb_all` 的原理是重载 CR3：CPU 检测到 CR3 被写入后，会认为页表结构可能完全变了，于是把所有非 Global 的 TLB 条目全部作废。注意这里并不是真的"切换"页表——先读出 CR3 的值，再原样写回去，唯一的效果就是触发全量刷新。在 `map_mmio` 映射大范围 MMIO 区域后，用 `flush_tlb_all` 一次清除所有可能的过时缓存。

两处 `"memory"` clobber 告诉编译器"这条汇编可能读写内存，别把之前的内存操作优化掉"——如果编译器把 `pt[idx].raw = ...` 的写操作调度到 `invlpg` 之后执行，那刷新的时候页表还没更新，等于白刷。`volatile` 关键字确保编译器不会把整个 `invlpg` 优化掉——毕竟它没有输出操作数，优化器可能会觉得它是"无用的"。

### CR3 访问器：页表根的读与写

```cpp
inline uint64_t read_cr3() {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

inline void write_cr3(uint64_t cr3) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
```

CR3 寄存器保存着当前 PML4 表的物理地址（SDM Vol.3A Section 2.5, pp.2-14 到 2-16），这是整个页表树的根。`read_cr3` 在 VMM 初始化时读取 bootloader 设置的内核 PML4 地址，保存到 VMM 的内部状态中。`write_cr3` 则为将来的进程切换做准备——每个用户进程会有自己的 PML4，切换地址空间时需要把 CR3 指向新进程的页表根。要注意的是，`write_cr3` 会隐式触发全量 TLB 刷新（和 `flush_tlb_all` 效果相同），这也是进程切换代价高的原因之一——将来如果要做优化，可以研究 PCID（Process-Context Identifier）机制来避免每次切换都全量刷 TLB。

### 遗留代码的重构

有了这些基础设施，回头看 `paging.cpp` 的 MMIO 映射函数，你会发现重构的效果非常明显。原来的 `PD_HUGE_PAGE_FLAGS = 0x83`（即 Present + Writable + Huge = 1 + 2 + 128 = 0x83）变成了 `FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE`，一眼就能看出每个标志位的含义。原来手写的 `__asm__ volatile("invlpg (%0)" ...)` 变成了 `flush_tlb(cur)`，原来手写的 CR3 重载变成了 `flush_tlb_all()`。这不仅是整洁度的提升——更重要的是这些辅助函数成为了统一的 TLB 操作入口，将来如果需要加日志、加 SMP 的 IPI 广播刷新、或者统计 TLB miss 频率，只需要改一个地方。

## 收尾

到这一步，Cinux 的分页基础设施已经就位了：`paging_config.hpp` 集中管理所有常量，`PageEntry` 提供类型安全的 PTE 操作，TLB 刷新和 CR3 访问器让我们能正确地通知硬件。你现在可以用 `PageEntry entry; entry.raw = phys | FLAG_PRESENT | FLAG_WRITABLE;` 来构造一个映射条目，用 `PML4_INDEX(virt)` 来定位应该放在页表的哪个位置，用 `flush_tlb(virt)` 来确保 CPU 看到最新的翻译结果。

但这只是"砖块"——我们还没有用这些砖块去建房子。真正的 VMM 需要在四级页表树里自动遍历、自动分配中间页表、支持 map/unmap/translate 的完整操作。这正是下一章的内容：我们会实现 VMM 类的核心算法，走过 PML4 -> PDPT -> PD -> PT 的四级遍历，看看 `walk_level` 如何抽象出每一级的公共逻辑，以及 higher-half 地址转换如何让内核在虚拟地址空间里操作物理页表。

## 参考资料

- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：4 级分页的完整描述，PML4/PDPT/PD/PT 的索引方式和地址翻译流程。Cinux 的移位常量和索引宏直接来源于此。
- Intel SDM Vol.3A Table 4-20, p.4-31：PTE 格式定义，每一位的含义。Cinux 的 `PageEntry` 位域与此表一一对应。
- Intel SDM Vol.3A Section 4.8, p.4-39：Accessed 和 Dirty 标志位的硬件行为。
- Intel SDM Vol.3A Section 4.10, pp.4-43 到 4-52：TLB 缓存和 INVLPG 指令语义，CR3 重载对 TLB 的影响。
- Intel SDM Vol.3A Section 2.5, pp.2-14 到 2-16：CR3 寄存器格式。
- OSDev Wiki: [Paging (64-bit)](https://wiki.osdev.org/Paging) — x86-64 四级分页结构概述。
- OSDev Wiki: [TLB](https://wiki.osdev.org/TLB) — TLB 刷新方法对比。
- xv6 RISC-V `vm.c`：[GitHub](https://github.com/mit-pdos/xv6-riscv/blob/master/kernel/vm.c) — xv6 的 PTE 操作和 walk 函数，可对比 RISC-V Sv39 和 x86-64 的实现差异。
- Linux x86 pgtable：[GitHub](https://github.com/torvalds/linux/blob/master/arch/x86/mm/pgtable.c) — Linux 的多层 PTE 抽象和跨架构设计。
- SerenityOS Memory 子系统：[GitHub](https://github.com/SerenityOS/serenity/tree/master/Kernel/Memory/) — SerenityOS 的全物理映射和 `PageTableEntry` 类封装。
