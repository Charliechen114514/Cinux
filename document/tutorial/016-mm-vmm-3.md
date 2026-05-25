---
title: 016-mm-vmm-3 · 虚拟内存管理
---

# 016-3 当 CPU 说"页不存在"：缺页异常与按需分页实战

> 标签：demand paging, 缺页异常, #PF, 测试验证
> 前置：[016-2 四级页表遍历：VMM 的 map、unmap 与 translate](016-mm-vmm-2.md)

## 前言

前两篇我们把页表项的每一位都拆明白了，把 VMM 的 map/unmap/translate 核心算法也讲透了。但仔细想想，到目前为止所有映射都是内核"主动"建立的——我们在代码里显式调用 `g_vmm.map()`，指定虚拟地址和物理地址，把页表建好，然后 CPU 才能访问那片内存。这是一种"预分配"模式：先建页表，再访问内存，顺序不能错。你要是在页表建好之前就访问，CPU 直接给你一个 #PF（Page Fault），然后 kernel panic——非常不讲情面。

问题是，真实的操作系统不会这么干。Linux 在 `malloc()` 的时候根本不分配物理页——它只在进程的地址空间描述里记一笔"这块虚拟地址合法"，真正的物理页分配要等到进程第一次访问那片内存、触发缺页异常（#PF）之后才发生。这就是所谓的按需分页（demand paging），它的好处很明显：进程申请 1GB 内存但只用了 4KB，那就只分配 4KB 的物理页，剩下的全是空气。没有 demand paging，`malloc(1GB)` 就得立刻占用 1GB 物理内存，这在内存有限的系统上根本不可行。Intel SDM Vol.3A Section 4.12（p.4-61）明确描述了这个机制："Portions of the linear-address space need not be mapped to the physical-address space; data for the unmapped addresses can be stored externally."——线性地址空间不需要全部映射到物理内存，这正是 demand paging 的理论基础。

Cinux 当前的 demand paging 还远不到 Linux 那种精细程度——我们没有进程地址空间描述、没有 swap、没有 page cache。但这个 tag 做了一件关键的事：在 #PF handler 里实现"页不存在就分配一个物理页并映射"的闭环，让"异常 → 处理 → 恢复执行"这条路径第一次跑通。然后我们用双轨测试策略——QEMU 集成测试验证真实硬件交互、Host 单元测试验证纯算法正确性——来确认这整个闭环是可靠的。今天这篇就来拆解这最后一块拼图。

## 环境说明

和前两篇一致：x86-64 QEMU 环境、GCC cross-compiler、higher-half 内核。PMM 在 tag 015 已经就绪，VMM 的 map/unmap/translate 在本系列第二篇已经实现。本篇新增的是 `exception_handlers.cpp` 中 `handle_pf` 函数的 demand-paging 逻辑、`main.cpp` 中 VMM 初始化的位置调整，以及两个测试文件。

## #PF 的硬件约定：CR2 与错误码

在写任何代码之前，我们需要搞清楚 CPU 在缺页异常发生时到底给了我们什么信息。这部分的权威参考是 Intel SDM Vol.3A Section 4.7（pp.4-37 到 4-38），它是理解 #PF handler 的前提条件——如果你不清楚错误码每个 bit 的含义，写出来的 handler 就是在猜谜。

当 CPU 在做地址翻译时发现某个 PTE 的 P（Present）位为 0，或者在权限检查时发现违反了 R/W 或 U/S 的约束，它就会触发 #PF（vector 14）。与此同时，CPU 会做两件事。第一，把引起异常的线性地址写入 CR2 寄存器——这个写入是硬件自动完成的，不需要软件干预，所以我们能从 CR2 里精确知道"哪个地址闯的祸"。注意 CR2 里存的地址是触发异常的那条指令真正想要访问的线性地址，不是指令本身的地址（指令地址在 `frame->rip` 里）。第二，把一个错误码压入栈——这个错误码不是普通的 x86 error code，而是 #PF 专用的位域格式，嵌入到 `InterruptFrame` 的 `error_code` 字段里。

错误码的 bit 0 是 P 位——P=0 表示"页不存在"，P=1 表示"权限违反"。这是我们 demand-paging 逻辑的核心判断依据，整个 `handle_pf` 的分支就是围绕这一个 bit 展开的。bit 1 是 W/R 位——0 表示读操作触发的异常，1 表示写操作。bit 2 是 U/S 位——0 表示内核态（CPL=0）触发的，1 表示用户态（CPL=3）触发的。bit 3 是 RSVD 位——如果置 1 说明页表条目里保留了不该有的位（比如 PSE 或 PAT 设置不当），这在 4KB 页映射里不应该出现。bit 4 是 I/D 位——如果置 1 说明是在取指令时触发的缺页（executing from an unmapped page）。在 Cinux 当前的简化策略里我们不细分到 W/R、U/S、RSVD 这些位——只要 P=0 就尝试分配，P=1 就直接 panic。但随着内核变得越来越复杂（特别是有了用户态进程之后），这些位域会成为 CoW fault 检测、写时复制、用户空间权限管理的关键输入。

这里有一个容易混淆的地方：P=0 不一定是坏事。对于 demand paging 来说，P=0 恰恰是"这个页还没分配，请操作系统补上"的信号，是正常的工作流。只有 P=1 的情况——页存在但权限不够——才是真正的"非法访问"，需要向进程发送 SIGSEGV 或者内核 panic。OSDev Wiki 的 [Page Fault](https://wiki.osdev.org/Exceptions#Page_Fault) 页面对这些位域有非常清晰的总结，建议配合 Intel SDM 一起看。

## handle_pf 中的 demand-paging 逻辑

现在我们来看代码。`handle_pf` 函数位于 `kernel/arch/x86_64/exception_handlers.cpp`，在 tag 016 的 diff 里它新增了大约 15 行，这就是整个 demand-paging 的核心。

```cpp
void handle_pf(InterruptFrame* frame) {
    uint64_t fault_addr;
    __asm__ volatile("movq %%cr2, %0" : "=r"(fault_addr));

    uint64_t err = frame->error_code;

    // Demand-paging: try to allocate a page for not-present faults
    if ((err & 0x01) == 0) {
        uint64_t virt_page = fault_addr & ~0xFFFULL;
        uint64_t phys = cinux::mm::g_pmm.alloc_page();
        if (phys != 0 && g_vmm.map(virt_page, phys,
                                    cinux::arch::FLAG_PRESENT |
                                    cinux::arch::FLAG_WRITABLE)) {
            kprintf("[VMM] Demand-paged %p -> phys %p\n",
                    reinterpret_cast<void*>(virt_page),
                    reinterpret_cast<void*>(phys));
            return;
        }
    }
    // ... 后面是 fatal 诊断输出 ...
```

我们从上往下拆。第一步，`movq %%cr2, %0` 从 CR2 读出触发缺页的线性地址。第二步，`err & 0x01 == 0` 检查 P 位——如果是 0，说明是"页不存在"类缺页，我们有机会通过分配新页来修复它。第三步，`fault_addr & ~0xFFFULL` 把故障地址向下对齐到页边界，因为 CR2 里的地址可能是页内任意偏移（比如 `0x40000123`），但 map 操作是以整页为单位的，`~0xFFFULL` 清掉低 12 位就得到了 `0x40000000`。

然后是分配和映射。调用 PMM 的 `alloc_page()` 分配一个物理页，然后调用 VMM 的 `map()` 建立映射，标志位设为 Present + Writable。如果两个操作都成功，直接 `return`——CPU 执行 IRETQ 回到触发异常的那条指令重新执行。此时页表已经建好了，MMU 做地址翻译能找到有效的 PTE，指令正常通过。如果 PMM 分配失败（物理内存耗尽，`phys == 0`）或者映射失败（`map` 返回 false），代码直接落入下方的 fatal 路径——打印诊断信息并 panic。注意这里和更高级的实现不同：tag 016 的 handler 在分配失败后不会尝试释放物理页（因为 `alloc_page` 要么成功返回物理地址，要么返回 0 表示没分配），所以不存在"分配成功但映射失败需要回收"的场景——`phys != 0` 和 `map()` 的返回值在同一个 `if` 条件中做短路求值，只有两个都成功才进入 `return` 分支。

这意味着一个微妙之处：如果 `alloc_page` 成功（`phys != 0`）但 `map` 失败（返回 false），那个刚分配的物理页就泄漏了——它没有被映射到任何虚拟地址，也没有被归还给 PMM。这在 tag 016 的简化实现中是可以接受的，因为 `map` 失败的唯一原因是 OOM（`walk_level` 内部的 `alloc_page` 失败），而此时系统已经处于内存耗尽的边缘，panic 是合理的响应。

## 启动集成：VMM 在启动序列中的位置

VMM 初始化在 `main.cpp` 中的位置非常精确，我们从 diff 里可以清楚地看到：

```cpp
// Step 9: Initialise Physical Memory Manager
auto* boot_info = reinterpret_cast<const BootInfo*>(BOOT_INFO_PHYS);
cinux::mm::g_pmm.init(*boot_info);

// Step 10: Initialise Virtual Memory Manager
cinux::mm::g_vmm.init();
```

先 PMM 后 VMM 的顺序是硬性要求。VMM 的 `map` 在分配中间页表（PML4 → PDPT → PD → PT 这条链上缺失的任何一级）时需要调用 `PMM::alloc_page`——如果 PMM 还没初始化，分配就会返回 0 或者分配出垃圾，后果不堪设想。你可以把 PMM 想象成建材供应商，VMM 是施工队——施工队不可能在没有建材供应商的情况下开工。

`g_vmm.init()` 做的事情很简单：从 CR3 读出 bootloader 已经设置好的 PML4 物理地址并保存到成员变量里，作为后续所有页表操作的起点。这个设计第二篇已经详细讲过了，这里不再重复。

VMM 初始化被放在 framebuffer 初始化之前（Step 8 vs. Step 9），这个顺序看起来有点微妙——为什么不是先设好显示输出再搞虚拟内存？原因在于 VMM 初始化本身不需要任何输出，它只是读一个 CR3 寄存器然后存起来，快得几乎不花时间。而 framebuffer 初始化可能会涉及 MMIO 地址映射的调整，将来如果 VMM 要接管这些映射，它就需要先就位。当然在当前实现里这不构成实际依赖，但保持"底层设施先于上层功能"的顺序是一个好习惯。

在测试环境里，`kernel/test/main_test.cpp` 遵循完全一样的顺序：先跑 PMM 测试确保物理内存管理器正常工作，然后初始化 VMM，最后跑 VMM 测试。每个测试阶段都隐式假设它之前的所有基础设施已经就绪——这是集成测试的基本原则，也是为什么这些测试的运行顺序不能随便调换。

## 测试策略一：QEMU 集成测试

`kernel/test/test_vmm.cpp` 包含 10 个测试用例，全部在 QEMU 中运行，操作的是真实的页表、真实的 TLB 和真实的 PMM。这意味着每个测试都在真正的 x86-64 硬件上执行（虽然是 QEMU 模拟的，但页表遍历和 TLB 行为与真实 CPU 一致），而不是在 host 端的模拟环境里跑。如果某个测试忘了 flush TLB，QEMU 会忠实地用旧映射，测试就会失败——这正是集成测试的价值。这些测试高度重复，我们按功能分为五组，每组展示一个代表性测试并用文字概括其他。

**第一组：初始化验证**。test 1 检查 `g_vmm.kernel_pml4()` 非 0——这是后续所有测试的前提，如果 PML4 地址为 0 说明 init 没被调用或者 CR3 读到了异常值。别小看这个断言，它实际上在验证整个启动链"bootloader 建页表 → 内核读 CR3 → VMM 保存"是不是通的。

**第二组：map + translate 基本流程**。test 2 是标准流程：从 PMM 分配物理页，映射到 `0x20000000`，translate 验证返回正确的物理地址，最后 unmap 并释放物理页。这里选 `0x20000000` 作为测试地址是有讲究的——它不在 bootloader 已经映射的范围内（bootloader 映射的是 0-4MB 的恒等映射和 higher-half 映射），所以 map 操作一定会触发中间页表的分配，能完整测试 `walk_level` 的分配路径。test 3 在此基础上验证页内偏移保留——映射 `0x20010000`，translate `0x20010123`，期望得到 `phys + 0x123`。页内偏移保留是 translate 的基本语义，但如果实现时不小心在 translate 里做了页对齐，这个测试就会立刻暴露出来。

```cpp
// test 2: map + translate 标准流程
void test_map_translate() {
    uint64_t virt = 0x20000000ULL;
    uint64_t phys = g_pmm.alloc_page();
    TEST_ASSERT_NE(phys, 0u);

    bool ok = g_vmm.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);

    uint64_t result = g_vmm.translate(virt);
    TEST_ASSERT_EQ(result, phys);

    g_vmm.unmap(virt);
    g_pmm.free_page(phys);
}
```

**第三组：unmap 与未映射地址**。test 4 验证 unmap 的效果——先 map 再 translate 确认映射存在，然后 unmap，再次 translate 期望返回 0。这个测试隐式验证了 TLB flush：如果 `unmap` 里忘了调用 `flush_tlb`，translate 仍然可能从 TLB 缓存里找到旧映射而返回非零值。test 5 更直接——对一个从未映射过的地址调用 translate，期望返回 0。它们一起验证了"映射可以清除"和"未映射地址查询为空"两个基本语义。

**第四组：多页映射与覆盖**。test 6 映射两个不同虚拟地址到两个不同物理页，验证它们互不干扰。test 7 对同一虚拟地址连续 map 两次（remap），验证后者覆盖前者——`translate` 返回的是第二次映射的物理地址。test 8 对从未映射的地址调用 unmap，确认这是安全的空操作。这三个测试覆盖了"多映射共存"、"映射覆盖"和"空操作安全性"三种交互模式。

**第五组：高位地址与 demand paging**。test 9 验证内核空间高位 canonical 地址 `0xFFFFFFFF80000000`（即 `KERNEL_VMA`）的映射——这需要正确操作 PML4 的最高几个条目。test 10 是本篇最精彩的测试：向一个未映射的地址 `0x40000000` 直接写入魔数 `0xCAFEBABEDEADC0DE`，然后读回验证值一致。这段代码之所以不会 kernel panic，正是因为我们的 #PF handler 在中间介入了——CPU 写入时发现 PTE 不存在，触发 #PF，handler 分配物理页并映射，IRETQ 回来后写入成功。

```cpp
// test 10: demand paging 端到端验证
void test_demand_page() {
    volatile uint64_t* ptr =
        reinterpret_cast<volatile uint64_t*>(0x40000000ULL);
    *ptr = 0xCAFEBABEDEADC0DEULL;
    TEST_ASSERT_EQ(*ptr, 0xCAFEBABEDEADC0DEULL);
    uint64_t phys = g_vmm.translate(0x40000000ULL);
    TEST_ASSERT_NE(phys, 0u);
    g_vmm.unmap(0x40000000ULL);
    g_pmm.free_page(phys);
}
```

说实话，第一次看到这个测试通过的时候我还是挺兴奋的——从 #PF 触发到物理页分配到页表建立到指令重执行，整个链条一次跑通，这是虚拟内存子系统从"零件堆"变成"运转机器"的标志性时刻。如果你在自己实现的内核里跑到了这一步，建议停下来给自己鼓个掌，因为这个测试通过意味着你的异常处理、物理内存管理、虚拟内存管理三条线已经完全交汇了。

有一个值得注意的细节：`ptr` 被声明为 `volatile uint64_t*`。这个 volatile 不是摆设——它告诉编译器"这个内存位置可能在编译器看不到的地方被修改"，防止编译器把 `*ptr = ...` 和 `TEST_ASSERT_EQ(*ptr, ...)` 优化成只写不读。如果没有 volatile，编译器可能认为"刚写过这个地址，值不可能变"而直接用寄存器里的值做断言，跳过了实际的内存读取，于是 #PF 根本不会被触发，测试就失去了意义。

## 测试策略二：Host 单元测试与模拟框架

`test/unit/test_vmm.cpp` 是一个 420 行的 host 端测试文件，完全独立于内核运行——在你的开发机上直接编译执行，不需要 QEMU。它的核心思想是用纯软件模拟来验证 VMM 的页表遍历算法，而把硬件交互（TLB、CR3、真正的 #PF）留给 QEMU 集成测试去覆盖。

两条线互补，一条测算法，一条测硬件。这个双轨思路在内核开发里非常实用：硬件相关的测试跑得慢、需要特殊环境，但能验证真实交互；纯算法测试跑得快、可以在 CI 里频繁跑，但需要你把算法从硬件环境中剥离出来。

模拟框架由三个组件构成，我们先花点时间搞清楚它们的设计思路，因为这种"在 host 端模拟内核子系统"的模式在 OS 开发里会反复用到。

`MockPMM` 用一个 256 位的 bitmap 管理一个 256 页（1MB）的模拟物理内存池，基地址是 `0x2000000`（一个随便选的值，只要不和真实物理地址冲突就行）。分配时线性扫描 bitmap 找到第一个空闲位，置位后返回对应的模拟物理地址。这个设计比真实 PMM 简单得多——没有栈、没有锁、没有内存区域检测——但对于验证 VMM 的页表遍历算法来说，我们只需要"能分配出不同的物理页"这一条就够了。物理页的内容从哪来？这就是 `sim_memory` 的职责。

`sim_memory` 是 128 个 `alignas(4096) uint8_t[PAGE_SIZE]` 数组，充当模拟物理内存。MockPMM 分配的"物理地址"通过 `sim_virt_of()` 转换成 host 进程可以实际读写的指针——`sim_virt_of(0x2001000)` 返回的就是 `sim_memory[1]` 的地址。这个转换的角色和内核里的 `phys_to_virt` 完全一样：内核用 `phys + KERNEL_VMA` 做转换，host 测试用数组索引做转换，本质都是"给物理地址一个可以在当前环境中访问的指针"。`alignas(4096)` 不是可有可无的修饰——如果模拟页没有 4KB 对齐，把 `sim_memory[n]` 当作 `PageEntry[512]` 来访问时，末尾的字节可能跨页边界，导致未定义行为。

`TestVMM` 在这个模拟环境中重新实现了完整的 VMM 算法。它的 `walk_or_alloc`/`walk_only` 分别对应真实 VMM 的 `walk_level(true)`/`walk_level(false)`——拆成两个独立函数而不是用一个 bool 参数控制，是因为 host 测试不需要考虑代码体积的共用性，两个函数各自做一件事更清晰。

`map`/`unmap`/`translate` 的逻辑和真实 VMM 一一对应：map 走四级页表，缺中间表就从 MockPMM 分配，最后写 PTE；unmap 走四级页表，走到头了清零 PTE；translate 走四级页表，读到 PTE 后提取物理地址并保留页内偏移。这是算法正确性的独立验证——如果 host 测试和 QEMU 测试都通过了，我们有很高的信心说"VMM 的页表遍历算法是正确的"，因为两个完全不同的执行环境产生了相同的结果。

15 个 host 测试同样按功能分为五组。

**正常路径组**（6 个测试）覆盖 map+translate 基本流程、页内偏移保留、unmap 后返回 0、双页映射、未映射地址返回 0、remap 覆盖——和 QEMU 集成测试高度重叠，但验证的是纯算法正确性，跑得也更快。

**标志位组**（1 个）验证 map 时传入 `FLAG_PRESENT | FLAG_WRITABLE | FLAG_USER` 后 translate 仍然返回正确物理地址，确保标志位不会污染地址字段——这是对 `ADDR_MASK` 分离正确性的直接检验，如果 mask 不对，高位标志位就会被当成地址的一部分，translate 就会返回一个偏大的错误值。

**同 PT 多页组**（2 个）连续映射 16 个页到同一 PT 的不同 slot，验证全部 translate 正确，然后 unmap 其中一个确认相邻页不受影响——这测试了 PT 级别的隔离性，确保一个 PTE 的清零不会把整个 PT 搞坏。

**边界情况组**（3 个）包括 unmap 从未映射的地址（安全空操作）、高位 canonical 地址 `0xFFFFFFFF80000000` 的映射（验证 PML4 最高条目的索引计算）、完整的 map→unmap→remap 循环。

**PageEntry 组**（2 个）独立验证 `phys_addr()` 地址提取和 `is_present()` 位检查——这是对第一篇中 PageEntry 设计的单元级验证，不依赖任何 VMM 上下文。

你会发现这个双轨策略的价值在于：QEMU 测试能捕捉到"硬件交互错误"（比如忘了 flush TLB、CR3 读错了、#PF handler 没正确返回），但跑得慢且不容易覆盖边界情况；Host 测试能快速跑遍所有边界情况，但捕捉不到硬件交互错误。两条线各自覆盖对方的盲区，合在一起才能给出"VMM 是对的"这个结论。在实际开发中，我通常会先写 host 测试把算法逻辑调通，然后再写 QEMU 集成测试验证硬件交互——先快后慢，效率最高。

## 设计对比：Cinux vs. xv6 vs. Linux

说完了实现和测试，我们站远一点看看 Cinux 当前的 demand-paging 策略在整个操作系统光谱上处于什么位置。

Cinux 的做法是最简版：对所有 P=0 的缺页无条件分配一个新物理页并映射，不检查地址合法性、不维护使用上限、不做交换。这在教学内核里是合理的起步点——我们需要的只是验证"异常 -> 处理 -> 恢复"这条路径能跑通。这个 handler 总共不到 15 行有效代码，但它们完成了从硬件异常到软件处理到恢复执行的完整闭环。

xv6（RISC-V 版本）比 Cinux 稍进一步。它的 `usertrap()` 在捕获到 page fault（scause == 13 或 15）后，会检查故障地址是否落在进程的合法地址范围内——具体来说就是"不能超过进程的 `sz`（已分配内存大小），也不能低于进程的栈底"。如果地址合法，调用 `kalloc()` 分配物理页，用 `mappages()` 建立映射，然后恢复执行。xv6 也没有 swap 和 page cache，但它有"地址合法性校验"这一层——这比 Cinux 的"来者不拒"要安全一些，至少能防止用户进程随意访问任意虚拟地址。另外 xv6 在 `mappages()` 里对 remap 采取了 panic 策略——如果发现目标 PTE 已经是 present 的，直接 panic，不像 Cinux 那样静默覆盖。两种选择各有道理：xv6 认为"重复映射是 bug，应该尽早暴露"，Cinux 认为"remap 是合理的操作，静默覆盖更实用"。[xv6 源码](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c)中的 `vmfault()` 函数清楚地展示了这个模式。

Linux 则是完全体。它的 `do_page_fault()`（定义在 `arch/x86/mm/fault.c`）首先在 `mm_struct` 的 VMA（Virtual Memory Area）红黑树中查找故障地址是否落在某个合法区间——如果不落在任何 VMA 里，直接向进程发送 SIGSEGV。如果地址合法，Linux 还要区分多种情况：匿名页缺页就分配零页（lazy allocation），文件映射缺页就从 page cache 或磁盘读入（file-backed demand paging），CoW 缺页就复制物理页并修改页表（copy-on-write）。当物理内存不足时，Linux 的 page reclaim daemon（kswapd）会选择牺牲页换出到 swap 区，下次访问时再换入。这套完整机制需要"进程管理 + VMA 描述 + page cache + 块设备驱动"四个子系统协同工作，定义在 [Linux 内核源码](https://github.com/torvalds/linux/blob/master/arch/x86/mm/fault.c)里。Gorman 的 [Understanding the Linux Virtual Memory Manager](https://www.kernel.org/doc/gorman/html/understand/understand006.html) 对此有深入讲解。

值得一提的是，Linux 的 `do_page_fault()` 还处理了很多我们没遇到的边界情况。比如在缺页处理过程中可能发生递归缺页（因为 page table 本身也可能被换出去），Linux 用 `mmap_sem` 的读写锁和 `FAULT_FLAG_ALLOW_RETRY` 标志来处理这种情况——第一次尝试拿锁失败就释放并重试，而不是死等。又比如内核态缺页（比如 copy_from_user 触发的）需要特殊处理，不能用普通的用户态 fault 路径。这些边界情况在单核教学内核里不会出现，但在生产环境里是必须处理的。

Cinux 当前策略的局限性很明显：任何对未映射地址的访问都会成功（包括野指针），没有 swap 意味着物理内存用完就只能 panic，没有 CoW 意味着 `fork()` 必须完整复制父进程的所有物理页——这在物理内存有限的系统上会很快耗尽资源。但这些局限是刻意的——在内核开发的这个阶段，我们的目标是让基本机制跑通，而不是一步到位实现生产级的内存管理。过早优化是万恶之源这句话在内核开发里尤其成立：如果你在连"缺页异常能不能正确恢复"都还没验证的情况下就去设计 VMA 红黑树和 LRU 页替换，大概率会陷入过度设计的泥潭。

扩展路径也很清晰：先加 VMA 数据结构做地址合法性校验，然后加 CoW 支持 fork()，最后加 page cache 和 swap 做完整的 demand paging。每一步都是在前一步的基础上增量添加，不需要推翻重来。SerenityOS 的演进历史很好地印证了这条路——它也是从简单的 map/unmap/translate 起步，逐步添加 region-based VM、COW、mmap、swap，最终形成了一套相当完善的多层内存管理系统。Cinux 的 VMM 类设计里已经预留了 `pml4` 可选参数，这就是为将来 per-process 地址空间埋下的接口——每个进程有自己的 PML4 root，#PF handler 根据当前进程的 `mm_struct` 做出正确的分配决策。

## 收尾

到这里，tag 016 的三篇教程就全部完成了。第一篇我们拆解了页表项的硬件格式——从裸 `uint64_t` 到有名字有类型的 `PageEntry` 联合体；第二篇实现了 VMM 的核心遍历算法——`walk_level` 封装了四级页表的共性逻辑，`map`/`unmap`/`translate` 在此基础上各司其职；这一篇我们把缺页异常处理和 demand paging 串起来跑通，并用双轨测试策略验证了整个虚拟内存子系统。

回头看，从 bootloader 用 2MB 大页做粗粒度映射（tag 013），到 PMM 管理物理页（tag 015），再到 VMM 做 4KB 粒度的四级页表操作和按需分页（tag 016），Cinux 的虚拟内存子系统从无到有走完了第一阶段。虚拟内存不再是一个只存在于 Intel SDM 里的抽象概念，而是一套真正能在 QEMU 里跑起来的、可测试的、可扩展的代码。

接下来如果要继续演进，最自然的方向是 per-process 地址空间（让每个进程有独立的 PML4）和 CoW fork（在 #PF handler 里处理写时复制缺页）。`handle_pf` 中那个 `err & 0x01` 的分支已经为这些扩展留出了结构上的空间——P=1 的路径目前直接 panic，将来会变成 CoW fault 的处理入口。但那是将来的事了。

## 参考资料

- Intel SDM Vol.3A Section 4.7, pp.4-37 到 4-38：Page-Fault Exception 的完整描述，错误码格式（P/W/R/U/S/RSVD/I/D 位域），CR2 寄存器的含义
- Intel SDM Vol.3A Section 4.12, p.4-61：Virtual Memory 与 Demand Paging 的概念描述——"线性地址空间的某些部分不需要映射到物理地址空间"
- OSDev Wiki: [Page Fault](https://wiki.osdev.org/Exceptions#Page_Fault) — #PF 异常的错误码位域详解
- OSDev Wiki: [TLB](https://wiki.osdev.org/TLB) — TLB 缓存机制和刷新策略
- xv6 RISC-V `trap.c` 和 `vm.c`：[xv6-riscv 源码](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/trap.c) — xv6 的缺页处理和地址合法性校验
- Linux `arch/x86/mm/fault.c`：[Linux 源码](https://github.com/torvalds/linux/blob/master/arch/x86/mm/fault.c) — Linux 完整的 demand-paging 实现
- Mel Gorman, *Understanding the Linux Virtual Memory Manager*：[Chapter 6](https://www.kernel.org/doc/gorman/html/understand/understand006.html) — Linux 缺页处理机制详解
