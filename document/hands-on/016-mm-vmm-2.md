---
title: 016-mm-vmm-2 · 虚拟内存管理
---

# 016-2 VMM 核心算法：映射、解映射与翻译

## 导语

上一章我们把页表项的数据结构、分页常量和 TLB 刷新工具都准备好了。现在要把这些零件组装成一个真正能用的虚拟内存管理器——VMM。VMM 的核心能力就是三件事：把一个虚拟页映射到一个物理页（map）、解除一个映射（unmap）、查询一个虚拟地址映射到了哪个物理地址（translate）。听起来简单，但实现上有几个棘手的细节——中间级页表的按需分配、物理地址到虚拟地址的转换、以及 TLB 一致性的维护。

完成这一章后，我们的内核就拥有了动态修改页表的能力。你可以在运行时映射任意的虚拟地址到任意的物理页——这是进程地址空间管理、内存映射 I/O、按需分页等一切高级内存功能的基础。QEMU 测试会验证 map/translate/unmap 的完整闭环。

知识前置：上一章的 PageEntry 联合体、paging_config.hpp 常量、TLB 刷新函数必须已经实现。你需要理解 x86-64 四级页表的遍历过程（从 CR3 开始，依次查 PML4、PDPT、PD、PT 四张表），以及上一章 PMM 的 `alloc_page`/`free_page` 接口——VMM 在创建映射时需要从 PMM 申请物理页来建立中间级页表。本章的代码改动集中在三个文件：新建 `kernel/mm/vmm.hpp` 和 `kernel/mm/vmm.cpp`（VMM 类的声明和实现），以及修改 `kernel/main.cpp`（添加 VMM 初始化调用）。

## 概念精讲

### Higher-Half 页表访问：物理地址到虚拟地址的桥梁

这是 VMM 实现中最容易把人绕晕的一个概念。页表项中存储的地址是**物理地址**——因为 MMU 硬件在遍历页表时使用物理地址来查找下一级页表。但是我们的内核代码运行在虚拟地址空间中，所有内存访问（包括读写页表内容）使用的都是虚拟地址。所以当我们拿到一个 PTE 中存储的物理地址时，不能直接用它来访问那张页表——我们需要把它转换成对应的虚拟地址。

这是 VMM 实现中最容易把人绕晕的一个概念。页表项中存储的地址是**物理地址**——因为 MMU 硬件在遍历页表时使用物理地址来查找下一级页表。但是我们的内核代码运行在虚拟地址空间中，所有内存访问（包括读写页表内容）使用的都是虚拟地址。所以当我们拿到一个 PTE 中存储的物理地址时，不能直接用它来访问那张页表——我们需要把它转换成对应的虚拟地址。

Cinux 是一个 higher-half 内核，虚拟地址偏移量 `KERNEL_VMA = 0xFFFFFFFF80000000`。bootloader 在设置初始页表时，把所有物理内存做了 identity map（虚拟地址 = 物理地址）加上 higher-half map（虚拟地址 = 物理地址 + KERNEL_VMA）。所以对于任何已经被 identity + higher-half 映射的物理地址，我们可以用 `phys + KERNEL_VMA` 得到它的 higher-half 虚拟地址，然后通过这个虚拟地址来读写页表内容。

这个转换在 VMM 中由一个内部辅助函数 `phys_to_virt` 完成：输入物理地址，输出对应的 PageEntry 指针（虚拟地址）。每一步遍历页表时，从 PTE 中读出下一级页表的物理地址，用 `phys_to_virt` 转换成虚拟地址后，才能通过软件读写那张页表的 512 个表项。`phys_to_virt` 的返回类型选择 `PageEntry*` 而不是 `void*`，是因为它的调用场景几乎全是"拿到下一级页表的虚拟地址，然后按 PageEntry 数组去索引"，直接返回 `PageEntry*` 省去了调用处的强制转换。`KERNEL_VMA` 定义在 `vmm.cpp` 的匿名命名空间中作为编译期常量，而不是从某个全局头文件中获取——这是因为 VMM 是 KERNEL_VMA 的主要消费者，而且 KERNEL_VMA 的值必须和链接脚本中的 `VIRTUAL_BASE` 一致，如果将来修改了链接脚本中的虚拟地址基址，需要同步修改这个常量。

你可以把 `phys_to_virt` 理解为内核和硬件之间的"翻译官"——硬件用物理地址说话（页表项里存的是物理地址），内核用虚拟地址说话（代码中用的是指针），翻译官在两者之间实时转换。如果翻译官搞错了（比如 KERNEL_VMA 的值和链接脚本不一致），你就会读写到完全错误的内存位置——而且这种 bug 特别难定位，因为症状可能是"莫名其妙的 page fault"或者"map 之后 translate 返回了错误的地址"。

参考：

- [OSDev Wiki - Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
- Intel SDM Vol.3A Section 4.5.4 -- PTE 中的地址字段是物理地址

### walk_level：单级页表遍历的抽象

四级页表的遍历看似需要四层嵌套的逻辑，但实际上每一级做的事情几乎一模一样：查当前表的第 N 项，如果存在则取出下一级表的物理地址，如果不存在则（可选地）分配一个新页作为下一级表。这个共性启发我们抽象出一个 `walk_level` 辅助函数来统一处理每一级。如果没有这个抽象，你需要在 map、unmap、translate 三个函数中分别手写四级遍历代码，每份代码结构相似但有微妙差异（是否分配、最后一步是读还是写），代码量翻三倍不说，三份逻辑之间的不一致几乎是必然的。`walk_level` 把这种共性提取成一处实现，三个操作只需要在调用方式和最后一步上做区分。

`walk_level` 接受三个参数：当前级页表（虚拟地址指针）、索引值、以及一个"是否需要分配"的标志。当表项的 Present 位为 1 时，直接用 `phys_to_virt` 把表项中的物理地址转换成虚拟地址返回——这是"跟随已有路径"的情况。当 Present 位为 0 时，根据 `should_alloc` 标志决定行为：如果 `should_alloc` 为 false，直接返回空指针（表示这条路走不通）；如果 `should_alloc` 为 true，调用 `g_pmm.alloc_page()` 分配一个新物理页，把它清零（新页表必须是全零的——未清零的垃圾数据会被硬件解释为 PTE，可能导致幽灵映射或保留位 fault），然后在当前级表项中填入新页的物理地址加上 Present + Writable 标志，最后返回新页表的虚拟地址。

这个设计让 map 操作可以逐级 walk 并在需要时自动创建中间表，而 unmap 和 translate 操作则使用 `should_alloc = false` 模式——只跟随已有的映射路径，绝不创建新的表项。

### VMM 类的设计哲学

VMM 类不是一个静态工具类——它是一个拥有实例状态的对象。这个决定是为了未来的扩展性预留的。目前内核只有一个地址空间（内核空间），所以只有一个全局 VMM 实例 `g_vmm`。但将来当我们实现用户进程时，每个进程都需要自己的页表根（自己的 PML4），VMM 的接口已经为此做好了准备——`map`、`unmap`、`translate` 都有一个可选的 `pml4` 参数，如果不传就使用初始化时保存的内核 PML4，如果传了就使用指定的 PML4 物理地址。

### map/unmap/translate 的语义边界

三个操作的语义设计有几个值得注意的边界条件。`map` 采用"静默覆盖"策略——如果目标虚拟地址已经映射了，旧映射会被直接替换，不会报错也不会 panic。这和 xv6 的 `mappages` 不同（xv6 在遇到已映射的 PTE 时直接 panic），但和 Linux 的 `pte_set` 类似。`unmap` 的"不释放物理页"设计也是一个刻意的分离——映射管理和物理内存生命周期管理是两个独立的关注点，调用者可能需要在 unmap 之后继续使用那个物理页（比如把它映射到另一个虚拟地址）。`translate` 用返回 0 表示"未映射"，这隐含了"物理地址 0 永远不会被分配"的假设——在 Cinux 中这是安全的，因为 PMM 从不分配物理地址 0 以下的页。

`init` 方法做的事情非常简单——读取 CR3 并保存为 `kernel_pml4_` 成员。这就是 VMM 的全部状态：一个 64 位整数。可以说 VMM 目前是一个"无状态的服务者"——它不拥有任何页表内存（那些物理页是 PMM 分配的），不维护映射的计数或链表，只是纯粹地操作硬件页表结构。在 tag 016 阶段，VMM 不包含任何锁机制，这在单核 QEMU 环境下是安全的。未来的多核支持会引入自旋锁来保护并发的 map/unmap 操作。

`map` 的返回值是 `bool`——成功返回 true，失败返回 false（PMM 分配物理页失败时）。`unmap` 的返回值是 `void`——解除映射不需要报告失败，如果目标地址本来就没映射，unmap 就是一个 no-op。`translate` 的返回值是 `uint64_t`——成功时返回物理地址（保留了页内偏移），失败时返回 0。选择 0 作为"未映射"的标志和 PMM 的 OOM 标志理由一样——物理地址 0 是 IVT 的位置，不应该出现在正常的映射中。

参考：

- xv6 `walk(pagetable, va, alloc)` -- Cinux 的 walk_level 在结构上与 xv6 的 walk 函数完全相同，区别在于 xv6 用循环遍历 3 级（RISC-V Sv39），Cinux 用连续调用遍历 4 级（x86-64）
- Linux `pgtable.c` -- Linux 的页表操作也是逐级 walk，但增加了 5 级支持和 per-process mm_struct 封装

## 动手实现

### Step 1: 定义 VMM 类接口

**目标**: 在 `vmm.hpp` 中定义 VMM 类的完整接口，包括 init、map、unmap、translate 和 kernel_pml4 访问器。

**设计思路**: VMM 类的公开接口只有五个方法。`init()` 从 CR3 读取当前 PML4 物理地址并保存。`map(virt, phys, flags, pml4)` 完成一次 4KB 页的映射——遍历四级页表，中间缺少的任何级别都会自动从 PMM 分配并链接，最终设置 PT 项为目标物理地址加标志位。`unmap(virt, pml4)` 清除指定虚拟地址的 PT 项并刷新 TLB——但不释放对应的物理页，由调用者负责回收。`translate(virt, pml4)` 遍历页表查询映射，返回物理地址（包含页内偏移），未映射返回 0。`kernel_pml4()` 返回保存的内核 PML4 物理地址。

私有成员只有一个 `uint64_t kernel_pml4_{}`——保存 init 时读取的 CR3 值。使用类内初始化（花括号语法）确保默认构造后值为 0。

全局实例 `g_vmm` 声明为 `extern VMM g_vmm`，在 `vmm.cpp` 中定义。和 PMM 的 `g_pmm` 保持一致的设计模式。

**实现约束**: 文件路径是 `kernel/mm/vmm.hpp`，命名空间 `cinux::mm`。`map` 的 `pml4` 参数类型是 `uint64_t*`（可选指针，指向 PML4 物理地址），不传时默认为 `nullptr`（使用内核 PML4）。注意这个接口设计——`pml4` 不是直接传物理地址值，而是传一个指向物理地址的指针。当指针为空时使用 `kernel_pml4_`；当指针非空时解引用获取 PML4 物理地址。这种间接层在将来实现进程切换时非常方便。

**踩坑预警**: `translate` 返回 0 表示"未映射"，但物理地址 0 本身也是一个有效的物理页（虽然我们不映射它）。如果未来有人映射了一个虚拟页到物理地址 0x0000，`translate` 就无法区分"映射到物理页 0"和"未映射"了。好在 Cinux 的 PMM 从 1MB 以上开始分配，物理地址 0 永远不会被分配出来，所以这不是问题——但如果你将来要做用户空间地址空间管理，可能需要重新考虑这个设计。

**验证**: 这个步骤只涉及头文件定义，编译通过即可。实际的运行时验证在 Step 4 中进行。

### Step 2: 实现 walk_level 内部辅助函数

**目标**: 编写单级页表遍历的核心逻辑，这是 map/unmap/translate 的共享基础设施。

**设计思路**: `walk_level` 是 VMM 所有操作的"发动机"。它封装了一个简单但关键的决策：在当前级页表中，目标索引处的表项是否有效？如果有效（Present=1），就跟随它到下一级；如果无效（Present=0），要么分配新表（map 操作需要），要么放弃（unmap/translate 操作）。

分配新页表的过程需要三个步骤。第一步，从 PMM 申请一个物理页——如果 PMM 返回 0（OOM），walk_level 返回空指针，map 操作整体失败。第二步，把新页表的所有 512 个表项清零——这是绝对必要的，因为 PMM 分配出来的物理页内容是不确定的（可能是之前被 free_page 释放后遗留的旧数据）。不清零的后果非常严重：硬件在遍历下一级页表时会读到垃圾 PTE，其中某些 bit 可能被意外置 1，导致 reserved-bit fault 或者幽灵映射。第三步，在当前级表项中写入新页的物理地址并设置 Present + Writable 标志——这样新页表就被链接到了四级结构中。

**实现约束**: `walk_level` 定义在 `vmm.cpp` 的匿名命名空间中（内部链接）。参数是 `PageEntry* table`（当前级页表的虚拟地址指针）、`uint64_t index`（目标索引，0-511）、`bool should_alloc`（是否允许分配新表）。返回 `PageEntry*`——成功时返回下一级表的虚拟地址指针，失败时返回 `nullptr`。函数内部需要访问 `g_pmm`（全局 PMM 实例）来分配物理页，以及 `phys_to_virt`（另一个匿名命名空间函数）来做物理到虚拟地址的转换。

**踩坑预警**: 新页表的清零是千万不能省略的步骤。你可能会想"PMM 分配的页面是不是已经是零了？"——答案是"不一定"。如果这个物理页之前被分配出去过、写入过数据、然后被 free 了，它里面装的就是上一次使用者的残留数据。如果你把一个装满垃圾的页表链接到页表结构中，CPU 在遍历时会把垃圾数据当作合法的 PTE 来解读——best case 是触发 page fault，worst case 是 CPU 沿着垃圾 PTE 中的地址继续"遍历"到了一个完全错误的物理地址，你写的数据就不知道落到哪里去了。这种 bug 的症状是间歇性的、不可重现的——因为残留数据的内容取决于之前的内存使用模式。所以，清零，无条件清零，每次都清零。

**验证**: walk_level 的验证需要在完整的 map/unmap/translate 操作中体现。暂时可以编译检查——如果编译通过且后续的 map 测试全部 PASS，说明 walk_level 的逻辑是正确的。

### Step 3: 实现 map、unmap 和 translate

**目标**: 编写 VMM 的三个核心操作的完整实现。

**设计思路**: 三个操作的共同骨架都是"四级遍历"——从 PML4 开始，依次调用 walk_level 走过 PDPT、PD、PT 四级。区别在于遍历的参数和最后一步的操作。

**map** 的流程是这样的：首先确定 PML4 的物理地址（从参数获取或使用 kernel_pml4_），然后用 `phys_to_virt` 得到 PML4 的虚拟地址。接着连续三次调用 `walk_level`——PML4 到 PDPT（传 `should_alloc = true`，如果 PML4 项为空就分配新的 PDPT），PDPT 到 PD（同理），PD 到 PT（同理）。任何一次 walk_level 返回空指针，map 就返回 false。三级中间遍历都成功后，最后设置 PT 项：`pt[pt_index].raw = (phys & ADDR_MASK) | (flags & ~ADDR_MASK)`——把物理地址放入地址字段，把标志位放入低 12 位和高位。然后调用 `flush_tlb(virt)` 刷新这个虚拟地址的 TLB 缓存。这里有一个细节值得注意：flags 是调用者传入的标志位组合，用 `flags & ~ADDR_MASK` 确保不会意外修改地址字段——虽然正常情况下调用者传入的 flags 只有低位标志位，但防御一下总是好的。

**unmap** 的遍历过程与 map 相同，但 `should_alloc` 全部传 false——如果中间任何一级不存在（说明这个虚拟地址本来就没有映射），直接返回（no-op）。如果四级遍历都成功了，把目标 PT 项的 `raw` 设为 0，然后调用 `flush_tlb(virt)`。注意 unmap 不释放物理页——被 unmap 的那个物理页仍然由 PMM 管理，调用者需要自己调用 `g_pmm.free_page(phys)` 来回收。unmap 也不释放中间级页表——即使一张 PT 中的所有 512 个表项都被清零了，PT 本身占用的物理页也不会被回收。这是一种保守策略，避免了复杂的引用计数逻辑，代价是可能浪费一些中间级页表的物理页。

**translate** 的遍历过程也是 `should_alloc = false`，但最后一步不是修改 PT 项而是读取它。如果 PT 项的 Present 位为 0，返回 0（未映射）。如果 Present 位为 1，提取物理地址（用 `phys_addr()`）并加上虚拟地址的页内偏移（`virt & (PAGE_SIZE - 1)`），返回完整的物理地址。保留页内偏移这一点很重要——调用者可能传入一个未对齐的虚拟地址来查询，比如 `translate(0x20010123)` 应该返回 `physical_base + 0x123`，而不是只返回页的基地址。

**实现约束**: 三个方法都是 VMM 类的 public 成员，定义在 `vmm.cpp` 中。需要 include `paging.hpp`（PageEntry、flush_tlb、read_cr3）、`paging_config.hpp`（索引宏、ADDR_MASK、FLAG_*）、`pmm.hpp`（g_pmm）和 `kprintf.hpp`（调试输出）。`map` 返回 `bool`，`unmap` 返回 `void`，`translate` 返回 `uint64_t`。

**踩坑预警**: map 操作在设置 PT 项时，是直接覆盖而不是先检查是否已经映射——如果你对一个已经映射的虚拟地址再次调用 map，旧的映射会被静默替换。这意味着你不会得到"双重映射"的错误提示，而是直接丢失了旧映射对应的物理页号。如果旧映射的物理页没有被记录下来，就造成了内存泄漏。xv6 在 `mappages` 中会检查 PTE 是否已经有效（如果有效就 panic），而 Cinux 选择了"静默覆盖"的策略——这是有意的简化，因为内核开发者在调用 map 时应该知道自己在做什么。但如果你在测试中发现物理内存莫名其妙地越来越少，先检查是否有地方对同一个虚拟地址重复调用了 map。

另一个容易忽略的点：`translate` 保留页内偏移的设计意味着，如果你想检查"某个虚拟地址是否被映射了"，你不能简单地检查 `translate(addr) != 0`——因为如果映射到的物理页恰好是物理地址 0（虽然不太可能），结果也是 0。不过如前所述，PMM 不会分配物理地址 0，所以在 Cinux 的场景下 `translate(addr) != 0` 等价于"已映射"。

**验证**: 编译并运行 QEMU 集成测试。串口输出中你应该看到 `VMM Tests (016)` section 下的以下测试全部 PASS：`map_translate`（映射后翻译得到正确物理地址）、`translate_offset`（翻译保留页内偏移）、`unmap_clears`（解除映射后翻译返回 0）、`translate_unmapped`（查询未映射地址返回 0）、`two_pages`（映射两个不同地址）、`remap`（重新映射覆盖旧映射）、`unmap_noop`（对未映射地址解除映射是安全的 no-op）、`high_address`（高半核地址空间的映射）。如果某个测试失败了，先看是哪个测试——如果 `map_translate` 失败但 `test_init_pml4` 通过了，问题在 `walk_level` 或 `phys_to_virt`；如果只有 `high_address` 失败，问题可能在 PML4 高位索引的计算上。

### Step 4: 在 kernel_main 中初始化 VMM

**目标**: 在内核启动序列中，在 PMM 初始化之后调用 VMM 的 init。

**设计思路**: VMM 的初始化非常轻量——只是读一次 CR3 并保存。但它必须在 PMM 初始化之后才能调用，因为 init 本身虽然不依赖 PMM，但后续的 map 操作会调用 `g_pmm.alloc_page()`。启动顺序是：BootInfo 解析 -> PMM init -> VMM init -> Framebuffer init -> ...

**实现约束**: 在 `kernel/main.cpp` 的 `kernel_main` 函数中，在 `g_pmm.init(*boot_info)` 之后、Framebuffer 初始化之前，添加 VMM 初始化调用。需要 include `kernel/mm/vmm.hpp`。

**踩坑预警**: 顺序绝对不能搞反。如果 VMM init 在 PMM init 之前调用，虽然 init 本身不会出错（它只是读 CR3），但如果你在 init 之后立刻尝试 map 操作，就会因为 PMM 未初始化而分配到垃圾物理页。如果你在 PMM init 之前就触发了 page fault（比如访问了未映射的地址），demand-paging handler 也会因为 PMM 未初始化而失败。另一个容易忽略的细节是：`map` 方法的 `pml4` 参数类型是 `uint64_t*`（指针），而不是 `uint64_t`（值）。这是因为我们希望支持"传空指针用默认值"的模式——如果类型是 `uint64_t`，就没有办法区分"使用默认值"和"传入 PML4 物理地址恰好为 0"的情况。

**验证**: 编译并运行内核。串口输出中应该看到 `[VMM] Initialised, kernel PML4 at phys ...` 这一行，出现在 `[PMM]` 统计信息之后。如果 PML4 物理地址是 0，说明 CR3 读取有问题——检查 `read_cr3()` 的内联汇编是否正确。如果地址非零但不对齐（低 12 位非零），可能是 CR3 格式理解有误——Intel SDM 规定 CR3 的 bit 51:12 才是 PML4 物理基地址，低 12 位是控制位。不过 `read_cr3()` 返回的是完整的 CR3 值，包括低 12 位，而 `phys_to_virt` 会把它当作物理地址来用——好在 bootloader 设置的 CR3 低 12 位通常是 0（PCD 和 PWT 都没启用），所以直接用 CR3 值作为 PML4 物理地址是安全的。

## 构建与运行

首先编译 host 端单元测试。在构建目录中运行 `ctest --output-on-failure`，确认所有 VMM 相关测试（标签 "vmm"）全部通过。Host 测试使用 mock PMM 模拟物理内存分配，验证 map/translate/unmap 的算法逻辑正确性——包括映射单页、保留偏移的翻译、解除映射、多页映射、重映射覆盖、未映射地址的处理、高地址映射等。

然后编译 `big_kernel_test` 目标并在 QEMU 中运行。串口输出中你应该看到：

```
[VMM] Initialised, kernel PML4 at phys 0xXXXXX
--- VMM Tests (016) ---
  PASS: test_init_pml4
  PASS: test_map_translate
  PASS: test_translate_offset
  PASS: test_unmap_clears
  PASS: test_translate_unmapped
  PASS: test_two_pages
  PASS: test_remap
  PASS: test_unmap_noop
  PASS: test_high_address
  PASS: test_demand_page
```

注意 `test_demand_page` 测试——它验证的是按需分页功能，这是下一章的重点内容，但它也依赖于本章实现的 VMM map 操作。

如果你看到的测试输出中有 `FAIL` 标记，先看是哪个测试失败了。如果所有 map 相关的测试都失败但 `test_init_pml4` 通过了，问题很可能出在 `walk_level` 的分配路径——检查 PMM 是否正确初始化、`alloc_page` 是否返回了有效的物理页。如果只有 `test_demand_page` 失败，那问题出在 #PF handler 而不是 VMM 核心逻辑——这在下一章中处理。

host 端单元测试可以更快地定位算法错误。在构建目录中运行 `ctest --output-on-failure -L vmm`，如果某个测试断言失败（比如 `translate` 返回了错误的物理地址），ctest 会打印期望值和实际值，帮助你精确定位是 walk 哪一级出了问题。host 测试的优势是可以在开发机上毫秒级完成，不需要等待 QEMU 启动。建议先调通 host 测试再跑 QEMU 集成测试——先快后慢，效率最高。

**CMake 修改要点**: `kernel/CMakeLists.txt` 的 `big_kernel_common` 库需要添加 `mm/vmm.cpp`（VMM 实现），`big_kernel_test` 需要添加 `test/test_vmm.cpp`（QEMU 集成测试）。`test/CMakeLists.txt` 需要新建 `test_vmm` 可执行目标（host 端单元测试），编译时定义 `CINUX_HOST_TEST` 宏，并把 `test_vmm` 加入 `test_host`、`test`、`test_verbose` 三个自定义目标的依赖列表。如果链接时报 "undefined reference to VMM::init()"，检查是否遗漏了 CMake 修改。

**测试用例地址选择**: QEMU 集成测试中使用的虚拟地址（如 `0x20000000`、`0x40000000`）是刻意选择的——它们不在 bootloader 已经映射的范围内（bootloader 映射的是 0-4MB 的恒等映射和 higher-half 映射），所以 map 操作一定会触发中间页表的分配，能完整测试 `walk_level` 的分配路径。高位地址测试 `0xFFFFFFFF80000000` 则验证了 PML4 最高条目的索引计算——这是内核 higher-half 映射依赖的地址范围。

## 调试技巧

**map 返回 false**: 如果 map 操作失败，唯一的可能就是 PMM 分配物理页失败（OOM）。检查 PMM 的 free_page_count——如果空闲页数很少，可能是因为前面某个地方泄漏了物理页（分配了但没有释放）。另一种可能是虚拟地址对应的四级路径中有多级不存在，需要同时分配多个中间级页表——如果一次性 map 很多不同的虚拟地址，每个地址可能需要最多 3 个中间页表（PML4 -> PDPT -> PD -> PT，最后一级 PT 需要 3 个中间表），所以 10 次 map 操作可能需要 30 个物理页。

**translate 返回错误值**: 如果 translate 返回了不正确的物理地址，问题可能出在 `phys_to_virt` 的转换上——检查 KERNEL_VMA 的值是否和链接脚本一致。你也可以用 GDB 连接 QEMU，在 map 操作后手动检查 PT 表项的内容：先用 `monitor info tlb` 看看 TLB 中是否缓存了新映射，然后用 `x/1gx` 查看特定 PT 表项的 raw 值。

**TLB 刷新遗漏的症状**: 如果你忘了在 map 或 unmap 后调用 `flush_tlb`，症状取决于具体操作。map 后不刷新：CPU 继续使用旧的 TLB 缓存（可能是"未映射"），写入到目标虚拟地址时会触发 page fault——但如果你刚好有按需分页 handler，它会分配一个新物理页映射上去，覆盖你的 map 结果，导致数据丢失。unmap 后不刷新：CPU 继续使用旧的 TLB 缓存（映射仍然有效），写入到已 unmap 的地址时不会报错，但物理页可能已经被 free 回 PMM 并重新分配给了别人——你正在往别人的内存里写数据。这就是为什么 `flush_tlb` 必须紧跟在 `pt[pt_idx].raw = ...` 之后，而且内联汇编的 `"memory"` clobber 也很关键——它防止编译器把页表写操作重排到 TLB 刷新之后。

**map 与 unmap 的对称性**: 你会发现 `map` 和 `unmap` 的四级遍历逻辑几乎完全对称，区别只在 `should_alloc` 参数和最后一步操作（写 PTE vs 清零 PTE）。这种对称性是 `walk_level` 抽象带来的好处——如果没有这个辅助函数，你需要在三个地方（map、unmap、translate）分别手写四级遍历，代码量翻三倍不说，三份逻辑之间微妙的不一致几乎是迟早的事。

**walk_level 清零遗漏**: 如果你分配了新的中间级页表但忘了清零，症状可能是间歇性的、不可重现的 page fault。因为未清零的页表里的垃圾数据会被硬件当作 PTE 解释，其中某些 bit 恰好被置 1 就会导致幽灵映射或保留位错误。这种 bug 最难定位——它取决于之前分配到同一个物理页的代码留下了什么数据。唯一的解决方案就是每次分配后无条件清零。

**GDB 页表遍历**: 你可以手动模拟四级页表遍历来调试 translate 问题。先读取 CR3（`monitor info registers` 或 `print $cr3`），然后根据虚拟地址计算出每一级的索引，依次用 `x/1gx` 读取对应表项的 raw 值。如果某一级的 raw 值全是 0 或者地址字段看起来不对，问题就出在那一级的 walk_level 上。

**phys_to_virt 验证**: 你可以通过一个简单的测试来验证 `phys_to_virt` 是否正确——在已知物理地址的页表（比如 CR3 指向的 PML4）上，用 `phys_to_virt(cr3_value)` 读取 PML4 的第一个条目，然后和 QEMU monitor 中 `x/1gx {cr3物理地址}` 的结果对比。如果两个值不一致，说明 `KERNEL_VMA` 的值和实际 higher-half 偏移不匹配。

**alloc_page 与 OOM 场景**: VMM 的 `walk_level` 在分配中间页表时调用 `g_pmm.alloc_page()`。如果 PMM 的空闲页不足，`alloc_page` 返回 0，walk_level 返回 nullptr，map 操作整体返回 false。在 QEMU 默认配置（128MB 内存）下，这种情况不太可能发生——但对于一个映射了大量虚拟地址的内核来说，每次 map 最多需要 3 个中间页表，所以映射 1000 个不同的虚拟地址可能需要消耗 3000 个物理页（约 12MB）。如果内核的其他子系统也大量使用物理内存，OOM 是可能的。

**物理页泄漏风险**: 由于 `map` 采用静默覆盖策略，如果你对同一个虚拟地址调用两次 `map` 而没有先 `unmap`，第一次映射的物理页就"丢失"了——PTE 被覆盖指向了新的物理页，旧的物理页不再被任何 PTE 引用，也没有被归还给 PMM。这就是物理内存泄漏。在测试中如果你发现 PMM 的空闲页数持续下降且不恢复，检查是否有地方对同一虚拟地址重复调用了 `map`。

**中间页表不回收的影响**: `unmap` 只清除 PT 条目，不回收中间级页表。这意味着如果你映射了 100 个虚拟页然后全部 unmap，中间级页表（PML4 条目、PDPT、PD）仍然占用物理内存。在 tag 016 的简化实现中这是可以接受的——物理内存充足，而引用计数会增加很多复杂性。未来可以在进程退出时批量回收整个页表树，这是 Linux 的 `free_pgtables()` 采用的策略。

## 本章小结

| 操作 | 遍历模式 | should_alloc | 最后一步 | TLB 刷新 |
|------|----------|-------------|---------|---------|
| map | PML4->PDPT->PD->PT | true（自动创建中间表） | 设置 PT 项 = phys + flags | flush_tlb(virt) |
| unmap | PML4->PDPT->PD->PT | false（路径不完整则 no-op） | 清零 PT 项 | flush_tlb(virt) |
| translate | PML4->PDPT->PD->PT | false（路径不完整返回 0） | 读取 PT 项，返回 phys+offset | 不需要 |

| 关键设计 | 说明 |
|----------|------|
| phys_to_virt | 物理地址 + KERNEL_VMA -> 虚拟地址，内核读写页表的桥梁 |
| walk_level | 单级遍历抽象：存在则跟随，不存在则分配或放弃，三参数设计（table, index, should_alloc） |
| 新页表清零 | 从 PMM 分配的页内容不确定，必须清零后才能作为页表使用，这是正确性的硬性要求 |
| 不释放中间表 | unmap 只清 PT 项，不回收中间级页表，保守但简单，回收推迟到进程退出时 |
| translate 保留偏移 | 返回物理地址 = 页基地址 + 页内偏移，支持未对齐查询 |
| 静默覆盖映射 | 对已映射地址 map 会直接覆盖旧映射，不报警告，调用者需自行避免泄漏 |
| pml4 可选参数 | 传 nullptr 使用内核默认 PML4，传指针使用指定页表根，为多进程预留接口 |
| 无锁设计 | tag 016 阶段无锁，单核安全，未来多核需引入自旋锁 |
