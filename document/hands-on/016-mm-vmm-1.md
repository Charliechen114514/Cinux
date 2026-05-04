# 016-1 页表项结构与分页配置

## 导语

到 tag 015 为止，PMM 已经能正确地分配和回收物理页了。但物理地址本身对我们来说没有直接意义——内核代码运行在虚拟地址空间里，MMU 在背后默默地把每一个虚拟地址翻译成物理地址，而翻译的依据就是页表。到目前为止我们的页表是 bootloader 用 2MB 大页静态搭好的"脚手架"，内核只是被动地享受着 identity map 和 higher-half map 的便利，从来没有亲手修改过任何一个页表项。

这一章要做的事情就是把页表从"黑箱"变成"工具箱"。我们需要定义一个精确匹配硬件格式的页表项（PTE）数据结构，把所有分页相关的常量集中管理起来，然后编写 TLB 刷新和 CR3 读写的基础设施。这些是下一章实现 VMM 的 map/unmap/translate 操作的绝对前提——不把砖头准备好，怎么砌墙？

知识前置：你需要了解 x86-64 四级分页的基本原理（PML4 -> PDPT -> PD -> PT -> 物理页），我们在 tag 006 的 mini kernel 中已经设置过初始页表；还要理解上一章 PMM 的接口（`alloc_page` 返回物理页、`free_page` 释放物理页），因为 VMM 在映射时需要从 PMM 申请物理页来创建中间级页表。

## 概念精讲

### x86-64 四级页表结构回顾

在 long mode（64 位模式）下，x86 使用四级页表完成虚拟地址到物理地址的翻译。Intel SDM Vol.3A Section 4.5 有完整的描述。一个 64 位虚拟地址被拆成五个部分：

```
 63          48|47    39|38    30|29    21|20    12|11      0
+------------+-------+-------+-------+-------+---------+
| 符号扩展    | PML4  | PDPT  | PD    | PT    | 页内偏移 |
| (canonical)| index | index | index | index | offset  |
+------------+-------+-------+-------+-------+---------+
  16 bits      9 bits  9 bits  9 bits  9 bits  12 bits
```

每一级页表有 512 个表项（9 位索引，2^9 = 512），每个表项 8 字节，所以一张页表恰好占一个 4KB 页——这不是巧合，而是硬件设计时精心安排的。页内偏移占低 12 位，正好寻址 4KB 页内的每一个字节。整个四级结构能寻址 2^48 = 256 TB 的虚拟地址空间（受 canonical address 约束，高 16 位必须是第 47 位的符号扩展）。

你可以把它想象成一个四层嵌套的目录系统：PML4 是根目录，它有 512 个抽屉（PDPT 表）；每个 PDPT 抽屉里又有 512 个子抽屉（PD 表）；每个 PD 子抽屉里有 512 个更小的抽屉（PT 表）；每个 PT 最底层抽屉里放着 512 张卡片，每张卡片指向一个 4KB 的物理页。要找一个虚拟地址对应的物理页，你得依次打开四层抽屉，每层根据虚拟地址中对应的那 9 位选择下一个抽屉，最后在第四层抽屉里找到物理页号。

CR3 寄存器保存着 PML4 表的物理基地址——这是整个翻译链条的起点。每次 CR3 被加载（比如进程切换时），CPU 就从新的 PML4 开始翻译，这就是为什么每个进程可以拥有独立的地址空间。

> 参考：Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25
> 参考：[OSDev Wiki - Paging (64-bit)](https://wiki.osdev.org/Paging)

### 页表项的位域布局

每一个页表项（PTE）是一个 64 位的整数，其中既有硬件定义的标志位，也有指向下一级页表或最终物理页的地址字段。Intel SDM Vol.3A Section 4.5.4, Table 4-20, p.4-31 给出了精确的位域定义。我们用一张 ASCII 表来展示 4KB 页的 PTE 布局：

```
+--+--+--+--+--+--+--+--+--+-------+----------+--+--+
|P |RW|US|PW|PC|A |D |PS|G |Avail  |物理地址   |Av|XD|
|  |  |  |T |D |  |  |  |  |(3bits)|(40bits)  |(11)| |
+--+--+--+--+--+--+--+--+--+-------+----------+--+--+
 0  1  2  3  4  5  6  7  8  9-11    12-51      52-62 63

P    (bit 0):  Present——存在位。为 1 表示该映射有效。
RW   (bit 1):  Read/Write——读写位。为 1 表示可写。
US   (bit 2):  User/Supervisor——特权位。为 1 表示用户态可访问。
PWT  (bit 3):  Page Write-Through——写穿透缓存策略。
PCD  (bit 4):  Page Cache Disable——禁用缓存。
A    (bit 5):  Accessed——访问位。CPU 在翻译经过此条目时自动置 1。
D    (bit 6):  Dirty——脏位。CPU 对映射的页执行写操作时自动置 1。
PS   (bit 7):  Page Size——在非叶节点中同 Huge 标志。
G    (bit 8):  Global——全局页。CR3 切换时 TLB 不 flush。
Avail(9-11):   软件可用位，OS 自由使用。
Addr (12-51):  物理地址字段。指向下一级页表或物理页帧。
Avail(52-62):  更多软件可用位。
XD   (bit 63): Execute Disable——禁止执行位。
```

其中最关键的字段是 bit 0（Present）和 bit 12-51（物理地址）。Present 位决定了这条映射是否有效——如果 CPU 尝试访问一个 Present=0 的 PTE 指向的地址，就会触发 Page Fault（#PF，异常号 14）。物理地址字段占 40 位，所以最多支持 52 位物理地址（2^52 = 4 PB），不过目前的 QEMU 默认配置远用不完。

注意 bit 5 和 bit 6 这两个"粘性位"——CPU 会在特定条件下自动设置它们，但永远不会自动清除。操作系统负责在需要的时候清零它们。这个特性在未来的页面置换算法中会很有用。

> 参考：Intel SDM Vol.3A Section 4.5.4, Table 4-20, p.4-31
> 参考：Intel SDM Vol.3A Section 4.8, p.4-39 (Accessed and Dirty flags)

### TLB：硬件的翻译缓存

CPU 每次访问内存都需要做虚拟地址到物理地址的翻译，而四级页表遍历意味着每一次内存访问实际上要读四次页表加上最终的数据访问——五次内存操作。这显然太慢了，所以 x86 引入了 Translation Lookaside Buffer（TLB），一个专门缓存虚拟地址到物理地址翻译结果的高速缓存。

TLB 的存在对软件来说大部分时候是透明的——你修改了页表，TLB 会"自动"知道吗？不会。这正是最坑的地方。如果你修改了一个 PTE（比如映射了一个新页或者解除了一项映射），但 TLB 里还缓存着旧的翻译结果，CPU 就会继续使用旧的映射，完全无视你对页表的修改。Intel SDM Vol.3A Section 4.10 对此有详细说明。

所以我们需要两种 TLB 失效操作。第一种是单页失效——用 `invlpg` 指令让 CPU 失效掉某一个特定虚拟地址的 TLB 缓存。这适合在 map 或 unmap 单个页面之后使用，开销最小。第二种是全局失效——重新加载 CR3 寄存器（读出 CR3 的值再写回去），这会让所有非全局页（G bit = 0 的页）的 TLB 缓存失效。适合在批量修改了页表之后使用。

Cinux 选择在每次 map/unmap 操作后对目标虚拟地址执行 `invlpg`，这是最精确、最安全的方式——你改了哪个页就刷哪个，不会影响其他映射的 TLB 缓存命中率。

> 参考：Intel SDM Vol.3A Section 4.10, pp.4-43 to 4-52
> 参考：[OSDev Wiki - TLB](https://wiki.osdev.org/TLB)

### CR3 寄存器：页表的根

CR3（也叫 PDBR，Page Directory Base Register）保存着当前使用的 PML4 表的物理基地址。Intel SDM Vol.3A Section 2.5 描述了 CR3 的完整格式——在我们的场景中，只需要关注一点：CR3 的低 12 位是一些控制位（比如 PCD、PWT），真正有效的物理基地址在 bit 12 及以上。

因为我们 boot 时设置的页表已经把 CR3 指向了一个有效的 PML4 表，所以 VMM 的初始化只需要读取当前的 CR3 值并保存下来，后续所有的 map/unmap/translate 操作都以这个 PML4 为起点进行遍历。`read_cr3` 和 `write_cr3` 这两个辅助函数封装了相应的内联汇编——直接用 `mov` 指令操作 CR3 即可。

## 动手实现

### Step 1: 创建 paging_config.hpp 分页常量头文件

**目标**: 把所有分页相关的常量集中到一个头文件中，供 paging.hpp、vmm.cpp 和 exception handler 共同使用。

**设计思路**: 之前 tag 中的分页常量散落在各个文件里——PAGE_SIZE 可能在 pmm.hpp 里，PT_ENTRIES 在 paging.cpp 的匿名命名空间里，flag 值用裸数字 0x83 表示。这种碎片化的做法在项目还小的时候没什么问题，但当我们需要在 paging.hpp、vmm.cpp、exception handler 三个地方同时引用这些常量时，就必须把它们提取到一个公共头文件中。

新头文件需要定义四组常量。第一组是页面尺寸：PAGE_SIZE（4096）和 PAGE_SHIFT（12），这是整个分页系统的基础粒度。第二组是每级页表的位移量：PT_SHIFT（12）、PD_SHIFT（21）、PDPT_SHIFT（30）、PML4_SHIFT（39）——每一级的位移量比上一级多 9 位（因为每级索引 9 位）。第三组是标志位：用 `1ULL << N` 的形式定义 FLAG_PRESENT（bit 0）、FLAG_WRITABLE（bit 1）、FLAG_USER（bit 2）、FLAG_PWT（bit 3）、FLAG_PCD（bit 4）、FLAG_ACCESSED（bit 5）、FLAG_DIRTY（bit 6）、FLAG_HUGE（bit 7）、FLAG_GLOBAL（bit 8）、FLAG_NX（bit 63），以及地址掩码 ADDR_MASK（`0x000FFFFFFFFFF000ULL`，提取 PTE 中的物理地址字段）。第四组是索引提取函数：PML4_INDEX、PDPT_INDEX、PD_INDEX、PT_INDEX——它们都是 `constexpr` 函数，接受虚拟地址，右移对应级数后与 `0x1FF` 做 AND 运算，提取出 9 位索引。

**实现约束**: 文件路径是 `kernel/arch/x86_64/paging_config.hpp`，命名空间 `cinux::arch`。所有常量用 `constexpr` 定义（不是宏）。索引提取函数也是 `constexpr`。头文件使用 `#pragma once` 作为 include guard。

**踩坑预警**: ADDR_MASK 的值是 `0x000FFFFFFFFFF000ULL`——注意那个 `ULL` 后缀。如果你忘了加，在某些编译器配置下 `0x000FFFFFFFFFF000` 会被当成 32 位常量，高位被截断，结果变成了 `0xFFFFF000`，物理地址的提取就全错了。另外，标志位定义必须用 `1ULL << N` 而不是 `1 << N`，因为 FLAG_NX 在 bit 63，如果用 32 位的 1 左移 63 位，行为是未定义的（C++ 标准规定移位量大于等于位宽是 UB）。

**验证**: 这个文件只包含常量定义，编译通过即可。你可以在 `paging.cpp` 中用这些常量替换掉之前的裸数字（比如把 `0x83` 替换为 `FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE`），如果编译和运行都正常，说明常量定义正确。

### Step 2: 定义 PageEntry 联合体

**目标**: 创建一个精确匹配硬件 PTE 格式的 8 字节联合体，同时提供原始 64 位访问和按名称访问各个位域的能力。

**设计思路**: x86 的页表项本质上就是一个 64 位的整数，硬件按位域来解读它。C++ 的匿名联合体（union）配合匿名位域结构体正好能表达这种"同一块内存，两种视角"的关系。联合体的一个成员是 `uint64_t raw`——可以直接整体读写 64 位值；另一个成员是一个匿名结构体，包含 present（1 bit）、writable（1 bit）、user（1 bit）、pwt（1 bit）、pcd（1 bit）、accessed（1 bit）、dirty（1 bit）、huge（1 bit）、global（1 bit）、_avail（3 bits）、addr（40 bits）、_avail2（11 bits）、nx（1 bit）——总共 64 位，和 raw 完全重叠。

位域的声明顺序和宽度必须严格按照 Intel SDM 的 PTE 格式，从低位到高位依次排列。C++ 标准保证在匿名联合体中，位域成员和 raw 共享同一块内存，所以修改 `entry.present = 1` 会立即反映在 `entry.raw` 中。

除了位域成员，还需要三个辅助方法。`phys_addr()` 用 `raw & ADDR_MASK` 提取物理地址字段——注意不是直接读 `addr` 位域成员，因为位域的物理布局在不同编译器上可能有差异（虽然 GCC/Clang 的行为是一致的），使用掩码操作更可靠。`set_phys_addr(phys)` 用 `(raw & ~ADDR_MASK) | (phys & ADDR_MASK)` 来设置物理地址，保留标志位不变。`is_present()` 检查 Present 位是否为 1。

最后，用一个 `static_assert(sizeof(PageEntry) == 8)` 确保联合体的大小确实是 8 字节——如果位域总宽度算错了（比如某一位多声明了一位），编译时会直接报错。

**实现约束**: PageEntry 定义在 `kernel/arch/x86_64/paging.hpp` 中，命名空间 `cinux::arch`。它是一个联合体（`union`），不是 `class` 或 `struct`。辅助方法 `phys_addr`、`set_phys_addr`、`is_present` 定义在联合体内部。需要 include `paging_config.hpp` 来获取 `ADDR_MASK` 和 `FLAG_PRESENT`。

**踩坑预警**: 位域的布局依赖于编译器的实现。幸运的是，GCC 和 Clang 在 x86-64 平台上对 `uint64_t` 位域的处理是完全一致的——按声明顺序从低位到高位排列。但如果你移植到其他平台或使用 MSVC，位域布局可能不同。所以 `static_assert` 不只是装饰——它是你唯一的编译期防线。另一个要注意的是，位域不能取地址——你不能写 `&entry.present`，因为位域成员可能不占完整的字节。所有对位域的访问必须通过值操作（读或写），不能通过指针。

**验证**: 在 paging.hpp 中定义好 PageEntry 后，可以通过编译检查来验证。同时 `static_assert` 会在编译期确认大小正确。你还可以在后续编写 host 测试时手动构造一些 PageEntry 值来验证位域布局——比如设 `raw = FLAG_PRESENT | FLAG_WRITABLE | 0x10300000`，然后检查 `phys_addr()` 是否返回 `0x10300000`、`is_present()` 是否返回 true。

### Step 3: 实现 TLB 刷新和 CR3 读写辅助函数

**目标**: 提供三个内联函数来管理 TLB 和 CR3——单页刷新、全局刷新、CR3 读取。

**设计思路**: `flush_tlb(virt)` 使用 `invlpg` 指令使指定虚拟地址的 TLB 缓存失效。这个指令的汇编模板是 `invlpg (%0)`，其中 `%0` 是包含目标虚拟地址的寄存器操作数。内联汇编中需要 `"memory"` clobber，因为 TLB 失效会改变后续内存访问的行为——编译器不能假设之前的内存加载结果在 `invlpg` 之后还有效。

`flush_tlb_all()` 通过重新加载 CR3 实现全局 TLB 刷新。做法是先读出 CR3 的值（`mov %%cr3, %0`），然后原封不动地写回去（`mov %0, %%cr3`）。这个操作会让所有非全局页的 TLB 缓存失效。同样需要 `"memory"` clobber。注意这个函数不改变 CR3 的值——它只是触发一次刷新副作用。

`read_cr3()` 和 `write_cr3(cr3)` 封装了 CR3 的读写操作。`read_cr3` 用 `mov %%cr3, %0` 读取 CR3 值并返回；`write_cr3` 用 `mov %0, %%cr3` 写入新值（这会触发全局 TLB 刷新，同时切换到新的页表根）。

**实现约束**: 四个函数都定义在 `kernel/arch/x86_64/paging.hpp` 中，命名空间 `cinux::arch`，全部是 `inline` 函数。`flush_tlb` 和 `flush_tlb_all` 返回 `void`；`read_cr3` 返回 `uint64_t`；`write_cr3` 接受 `uint64_t` 参数、返回 `void`。内联汇编使用 `__asm__ volatile` 确保不会被优化掉。

**踩坑预警**: `invlpg` 指令要求操作数是一个有效的虚拟地址，但实际上这个地址不需要当前被映射——`invlpg` 只是让 TLB 中关于这个地址的缓存失效，如果 TLB 里本来就没有这条缓存，它就是一个 no-op。所以调用 `flush_tlb` 的安全性不需要依赖目标地址是否已经映射。另外，`flush_tlb_all` 的 `"memory"` clobber 是必须的，如果你忘了加，编译器可能把 `flush_tlb_all()` 前后的内存操作重排序——在修改页表后调用 `flush_tlb_all()` 但 clobber 缺失的情况下，编译器可能把页表写入操作移到 TLB 刷新之后，导致 CPU 看到的还是旧的页表内容。

**验证**: 最直接的验证方式是把 `paging.cpp` 中原有的 `reload_cr3()` 函数和 `__asm__ volatile("invlpg (%0)" : : "r"(cur))` 替换为 `flush_tlb_all()` 和 `flush_tlb(cur)` 调用。如果替换后 MMIO 映射仍然正常工作（framebuffer 能正常显示），说明 TLB 刷新函数的封装是正确的。

### Step 4: 重构 paging.cpp 使用新常量

**目标**: 把 `paging.cpp` 中的裸数字和内联汇编替换为 `paging_config.hpp` 中定义的常量和 `paging.hpp` 中定义的辅助函数。

**设计思路**: 具体的替换有三处。第一处，原来定义 `PD_HUGE_PAGE_FLAGS = 0x83` 和 `PDPT_1GB_PAGE_FLAGS = 0x83`，现在替换为 `FLAG_PRESENT | FLAG_WRITABLE | FLAG_HUGE`——虽然值一样，但语义清晰得多。第二处，原来使用 `__asm__ volatile("invlpg (%0)" : : "r"(cur))` 做 TLB 刷新，现在替换为 `flush_tlb(cur)`。第三处，原来有一个内部的 `reload_cr3()` 函数，现在替换为 `flush_tlb_all()`。

这个重构的目的是消除 `paging.cpp` 中的重复定义，让它只保留"使用常量"的代码而不是"定义常量"的代码。重构后 `paging.cpp` 不再包含 `PAGE_SIZE`、`PT_ENTRIES`、`reload_cr3` 的定义——全部由 `paging_config.hpp` 和 `paging.hpp` 提供。

**实现约束**: `paging.cpp` 需要新增 `#include "kernel/arch/x86_64/paging_config.hpp"`。删除内部的 `PT_ENTRIES` 常量定义和 `reload_cr3()` 函数。原来的 `PD_HUGE_PAGE_FLAGS` 和 `PDPT_1GB_PAGE_FLAGS` 保留为匿名命名空间中的 `constexpr` 变量，但值改为标志位常量的组合。

**踩坑预警**: 重构时要确保没有遗漏的 `reload_cr3()` 调用点。如果 `map_mmio` 函数中还有地方调用了旧的 `reload_cr3()` 而你忘了替换，链接时会报未定义符号——这还好，不会静默出错。但如果你在替换过程中不小心把 `flush_tlb_all()` 的括号写漏了（写成 `flush_tlb_all;` 而不是 `flush_tlb_all();`），编译器不会报错——它会把这当成一个合法的表达式语句（函数名转换为函数指针然后丢弃），结果就是 TLB 没有被刷新。

**验证**: 重构完成后重新编译并运行内核。在 QEMU 中观察 framebuffer 是否正常初始化——如果 `map_mmio` 函数因为 TLB 刷新失败而写入错误位置，framebuffer 会完全黑屏或者显示乱码。同时检查串口输出中 `[PMM]` 的统计信息是否正常——PMM 初始化依赖正确的页表映射来访问位图。

## 构建与运行

整个 tag 016 的构建和验证分两步。

首先是 host 端单元测试，确认分页常量和 PageEntry 的位域布局在 host 编译环境下是正确的。在项目根目录执行构建命令后，运行 `ctest --output-on-failure` 查看 VMM 相关的测试结果。host 测试中会有专门检查 PageEntry 的 `phys_addr()` 和 `is_present()` 方法的测试用例，验证位域掩码操作的正确性。

然后是 QEMU in-kernel 集成测试。编译 `big_kernel_test` 目标后，用 QEMU 启动。串口输出中你应该能看到 `[VMM] Initialised, kernel PML4 at phys ...` 的消息，后面跟着一系列 `PASS` 标记。特别注意 VMM 初始化输出的 PML4 物理地址——它应该是一个非零的、4KB 对齐的物理地址（通常是 0x10000 或 0x20000，取决于 bootloader 的设置）。

## 调试技巧

**PageEntry 位域布局错误**: 如果你怀疑位域布局不对（比如 `phys_addr()` 返回了错误的值），可以在 host 测试中打印 `sizeof(PageEntry)` 和各个位域成员的偏移量。正确情况下，`present` 在 bit 0，`writable` 在 bit 1，`addr` 从 bit 12 开始占 40 位，`nx` 在 bit 63。如果偏移量不对，检查是否遗漏了某个位域成员。

**TLB 刷新遗漏**: 如果你在映射了一个新页后发现写入的数据"消失"了（写了之后读回来是垃圾），最可能的原因是忘了调用 `flush_tlb`。CPU 还在使用 TLB 中缓存的旧映射（可能是"不存在"），所以你的写入实际到了一个完全错误的位置。在 QEMU monitor 中可以用 `info tlb` 命令检查当前 TLB 的内容，确认你的映射是否被正确缓存。

**QEMU 调试**: 用 `qemu-system-x86_64 -d int` 可以在 QEMU 中记录所有中断和异常，包括 page fault。如果看到意料之外的 #PF，检查 CR2 的值（触发 fault 的虚拟地址）和 error code 的各个位。在 GDB 中，你可以用 `monitor info tlb` 查看 TLB 状态，用 `x/1gx {PML4的虚拟地址}` 手动遍历页表结构。

## 本章小结

| 概念 | 要点 |
|------|------|
| 四级页表 | PML4 -> PDPT -> PD -> PT -> 物理页，每级 512 项 x 8 字节 = 4KB |
| 虚拟地址拆分 | PML4 索引(47:39) + PDPT 索引(38:30) + PD 索引(29:21) + PT 索引(20:12) + 偏移(11:0) |
| PageEntry 联合体 | 64 位 raw + 位域结构体 + phys_addr/set_phys_addr/is_present 辅助方法 |
| 标志位 | P(bit0), RW(bit1), US(bit2), A(bit5), D(bit6), PS/Huge(bit7), G(bit8), XD(bit63) |
| ADDR_MASK | 0x000FFFFFFFFFF000ULL，提取 PTE 中的物理地址字段 |
| TLB 刷新 | invlpg（单页）+ 重载 CR3（全局），修改 PTE 后必须刷新 |
| CR3 | 保存 PML4 物理基地址，read/write_cr3 封装内联汇编 |
| paging_config.hpp | 集中定义 PAGE_SIZE/SHIFT, PT_ENTRIES, 各级 SHIFT, FLAG_*, ADDR_MASK, INDEX 宏 |
