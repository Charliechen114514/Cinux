# 地址空间生命周期：从创建到隔离验证

> 标签：AddressSpace, CR3 切换, TLB, 隔离测试, free_subtree, QEMU
> 前置：[018-2 PML4 分割与内核映射共享：从设计到代码](018-mm-address-space-2.md)

## 前言

前两章我们把 `AddressSpace` 的设计动机和代码实现都过了一遍。但代码写完了只是
第一步——你怎么知道它真的能隔离？你怎么知道两个 `AddressSpace` 的用户空间映射
确实互不可见？你怎么知道析构函数真的把所有页表页都归还给了 PMM，一个都没漏？
你怎么知道 `activate()` 之后 CR3 确实切换了，而不是白写了一次？

答案当然是测试。但这个 tag 的测试策略值得单独拿出来讲——因为它体现了内核开发中
一个非常实用的工程模式：同一套逻辑，两套测试。QEMU 集成测试（308 行）跑在真实
的内核环境里，操作真实的 CR3、真实的 PMM 和 VMM，验证的是硬件交互的正确性；
Host 单元测试（771 行）跑在你的开发机上，用完全模拟的 MockPMM 和 sim_memory
复刻内核逻辑，验证的是算法的正确性。两套测试互补——QEMU 测试能在秒级确认"硬件
真的切换了"，Host 测试能在毫秒级跑完 20 个用例并精确检查 PMM 的分配/释放计数。

这一章我们沿着一个 `AddressSpace` 的完整生命周期走一遍：创建、映射、激活、销毁，
每个阶段用测试代码来验证行为。中间会穿插 CR3 切换和 TLB 刷新的硬件细节——这些
是理解 `activate()` 为什么"能工作"的关键。

## 环境说明

本章涉及的测试文件有两个：`kernel/test/test_address_space.cpp`（308 行，QEMU
集成测试）和 `test/unit/test_address_space.cpp`（771 行，Host 单元测试）。
QEMU 测试在内核启动后由 `main_test.cpp` 调用 `run_address_space_tests()` 执行，
前置条件是 PMM、VMM、Heap 全部初始化完毕，`init_kernel()` 已在 `main_test.cpp`
中调用。Host 测试直接在开发机上编译运行，不依赖任何内核代码。

## 第一步：创建——从 PMM 借一页 PML4

一个 `AddressSpace` 的生命从构造函数开始。QEMU 测试的前三个用例验证了最基本的
正确性：

第一个测试确认 `init_kernel()` 已经在 `main_test.cpp` 中被调用，保存了一个非零
的内核 PML4 地址。如果没有这一步，后续所有构造函数从 `kernel_pml4_` 复制内核
条目时读到的就是 0，整个地址空间的基础就不成立。

第二个测试创建一个 `AddressSpace`，验证它的 `pml4_phys()` 返回非零值（说明 PMM
确实分配了一页）且不等于 `kernel_pml4()`（说明它是一张独立的 PML4，不是内核
正在使用的那张）。第三个测试创建两个实例，验证它们的 PML4 地址互不相同——
每个 `AddressSpace` 从 PMM 拿到的都是不同的物理页。

Host 测试在这基础上走了更远。它不仅验证 PML4 地址非零，还逐条检查新 PML4 的
用户空间条目（0 到 255）是否全为 0，内核空间条目（256 到 511）是否与模板
逐条一致。这是用 `sim_memory` 数组直接检查内存内容做到的——Host 测试可以绕过
所有抽象层，直接看页表的原始字节。

## 第二步：映射——在空白画布上画出第一笔

构造完的 `AddressSpace` 是一张空白的 PML4——用户空间没有任何映射。接下来要做的
就是在上面 map 一个页面。

QEMU 测试的做法很直接：从 PMM 分配一个物理页，在 `AddressSpace` 中把虚拟地址
`0x20000000` 映射到这个物理页，然后调用 `translate` 验证返回值等于物理地址。
这说明四级页表遍历建立了完整的 PML4 -> PDPT -> PD -> PT 链条。如果没有调用
`activate()`，这个映射只存在于"软件页表"中——VMM 通过 `&pml4_phys_` 找到了
正确的 PML4，遍历了正确的页表结构，但 CPU 硬件的 MMU 还在用 CR3 指向的内核
PML4 做地址翻译。这就是为什么测试里用的是 `as.translate()` 而不是直接通过指针
访问那个虚拟地址——`translate` 是软件遍历，不依赖 CR3。

Host 测试也做了 map 然后 translate 的验证，而且它还能检查 map 操作是否在
MockPMM 中分配了中间页表页。`map_full_walk` 在 `sim_memory` 里完整模拟了
四级页表遍历——PML4 不存在就分配 PDPT，PDPT 不存在就分配 PD，PD 不存在就分配
PT，最后在 PT 里写入映射。每次分配都会增加 `alloc_count`，析构时这些中间页
应该被全部归还。

unmap 和 translate 的反向测试也在这里覆盖——map 后 unmap 再 translate 应该
返回 0，从未 map 的地址 translate 也应该返回 0。

## 第三步：隔离——整个 tag 的核心里程碑

如果你只看一个测试，就看这个。QEMU 测试里的 Test 7 和 Host 测试里的两个隔离
测试是 tag 018 的核心里程碑。

```cpp
void test_cross_space_isolation() {
    cinux::mm::AddressSpace as1;
    cinux::mm::AddressSpace as2;

    uint64_t virt = 0x20020000ULL;
    uint64_t phys = g_pmm.alloc_page();

    bool ok = as1.map(virt, phys, FLAG_PRESENT | FLAG_WRITABLE);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ(as1.translate(virt), phys);

    TEST_ASSERT_EQ(as2.translate(virt), 0u);   // 隔离！
}
```

这段代码做的事情极其简单：创建两个 `AddressSpace`，在 as1 里映射虚拟地址
`0x20020000` 到一个物理页，然后验证 as1 的 translate 返回正确物理地址，而 as2
对同一个虚拟地址的 translate 返回 0。十来行代码，验证的是整个 tag 的核心命题：
每个 `AddressSpace` 拥有独立的用户空间页表。

为什么 as2 看不到 as1 的映射？因为 as1 的 `map` 操作传入的是 `&as1.pml4_phys_`，
VMM 在 as1 的 PML4 里建立了 PML4 -> PDPT -> PD -> PT 的完整链条。as2 的 PML4
是另一张独立的页表，上面没有任何用户空间的映射——它的条目 0 到 255 全是 0。
当 as2 的 `translate` 传入 `&as2.pml4_phys_` 时，VMM 在 as2 的 PML4 里找
`0x20020000` 对应的 PML4 条目，发现是空的（Present = 0），直接返回 0。

Host 测试在隔离方面做得更深。第二个隔离测试在 as1 和 as2 的同一个虚拟地址上
分别映射了不同的物理页，然后验证 as1 的 unmap 不影响 as2——证明不仅是"映射
不可见"，而且"操作也完全独立"。这个测试用例直接排除了"两个 AddressSpace 共享
了某些页表页"的可能性——如果共享了中间页表，as1 的 unmap 就会破坏 as2 的映射。

## 第四步：激活——CR3 切换与 TLB 刷新

`activate()` 是整个生命周期中最"硬件"的一步。它做的事情只有一件——把 PML4
物理地址写入 CR3 寄存器。但这一步触发的硬件行为远比表面复杂。

根据 Intel SDM Vol.3A Section 4.10.2 的规定，写入 CR3 会使处理器刷新所有
非全局的 TLB（Translation Lookaside Buffer）条目。TLB 是 CPU 内部的地址翻译
缓存——把最近用过的虚拟地址到物理地址的映射存起来，避免每次内存访问都走完整的
四级页表遍历。刷新 TLB 意味着切换后所有的内存访问都要重新做页表遍历，直到 TLB
重新被填充。这就是为什么进程切换是 TLB miss 的主要来源。

```cpp
void test_activate_changes_cr3() {
    uint64_t saved_pml4 = cinux::mm::AddressSpace::kernel_pml4();
    uint64_t cr3_before = read_cr3();

    cinux::mm::AddressSpace as;
    as.activate();

    uint64_t cr3_after = read_cr3();
    TEST_ASSERT_EQ(cr3_after, as.pml4_phys());
    TEST_ASSERT_NE(cr3_after, cr3_before);

    // 关键！在 as 析构之前恢复内核 PML4
    write_cr3(saved_pml4);
}
```

这个测试在 `activate()` 前后各读一次 CR3，确认切换后的 CR3 等于
`as.pml4_phys()`。但最后一行 `write_cr3(saved_pml4)` 极其关键——在 `as`
析构之前必须恢复内核 PML4。为什么？因为 `as` 的析构函数会调用
`g_pmm.free_page(pml4_phys_)` 把 PML4 页归还给 PMM。如果此时 CR3 还指向这张
PML4，CPU 就在用已经"释放"的物理内存做地址翻译——这块内存可能立刻被其他分配
覆盖，下一次内存访问大概率 triple fault。

这个坑在测试代码中特别容易踩——因为测试函数返回时局部变量析构的顺序是定义好的
（反向析构），但 CR3 恢复必须发生在最后一个 `AddressSpace` 析构之前。QEMU 测试
里凡是调用了 `activate()` 的测试（Test 8 和 Test 9）都在 AddressSpace 对象
析构前手动恢复了 CR3。

Intel 提供了 PCID（Process-Context Identifier）机制来缓解 TLB 刷新的性能问题。
当 CR4.PCIDE = 1 时，CR3 的低 12 位可以编码一个 12 位的进程标识符，TLB 条目
会打上这个标识符的标签。切换地址空间时只需提供不同的 PCID，TLB 中属于旧 PCID
的条目不会被刷新——CPU 可以同时缓存多个地址空间的翻译结果。但 Cinux 目前没有
启用 PCID，每次 `activate()` 都是全量 TLB 刷新。OSDev Wiki 的
[TLB](https://wiki.osdev.org/TLB) 页面对 PCID 机制有简要描述，Intel SDM
Vol.3A Section 4.10.1 和 Table 4-13 有完整的规格说明。

## 第五步：销毁——递归释放与内核保护

一个 `AddressSpace` 的生命在析构时结束。析构函数遍历用户空间的 PML4 条目（0 到
255），对每个 present 的条目调用 `free_subtree` 递归释放整个页表子树。

QEMU 测试的 Test 11 验证了析构不会破坏内核映射。它在一个作用域内创建
`AddressSpace`、映射用户页、让对象析构，然后检查 `kernel_pml4()` 值不变。
这验证了 `free_subtree` 只释放 PML4[0..255] 的子树，绝不触及条目 256 到 511
的内核页表。如果范围算错，后续代码执行会立即 page fault。

Host 测试在析构验证上走了更远。它比较析构前后 `MockPMM` 的 `free_count` 差值，
确认 map 创建的 PDPT/PD/PT 页在析构时全部被释放。一个典型的 map 操作会创建
最多三个中间页表页（PDPT、PD、PT 各一个），加上 PML4 本身，析构应该归还四个页。
Host 测试还能精确检查每个物理页的分配状态——`is_allocated(pml4_phys)` 在析构后
应该返回 false。

多子树测试映射了两个不同 PML4 槽位的地址（`0x00000000` 和 `0x8000000000`，
分属 PML4[0] 和 PML4[1]），验证析构能正确处理多棵独立子树的情况。这两棵子树
的释放是互不干扰的——PML4 的不同条目指向不同的 PDPT 根，`free_subtree` 对每棵
子树独立递归。

## 对比：xv6 和 Linux 的地址空间生命周期

回头看 xv6 的地址空间生命周期，它的流程和 Cinux 有明显的差异。xv6 在
`proc_pagetable()` 中创建用户页表——分配空白的根页表，映射 trampoline 和
trapframe 两个特殊页面。进程切换时，`swtch()` 先保存 callee-saved 寄存器到
`context` 结构体，然后 `scheduler()` 选择下一个进程，`proc_pagetable()` 已经在
更早的时候创建好了。但关键的区别在于 xv6 在 trap 进内核时切换到独立的
`kernel_pagetable`——这意味着 xv6 的地址空间切换比 Cinux 更频繁（每次 trap 都
要切），但每个用户页表更轻量（没有内核条目的复制开销）。

Linux 的 `mm_struct` 生命周期和 Cinux 的 `AddressSpace` 几乎平行。`fork()` 时
`dup_mm()` 创建新的 `mm_struct`，调用 `pgd_alloc()` 分配新 PGD 并复制内核条目
——对应 Cinux 的构造函数。`exit()` 时 `mmput()` 最终调用 `pgd_free()` 递归释放
用户空间的页表子树——对应 Cinux 的析构函数。`switch_mm()` 在上下文切换时把新
进程的 PGD 写入 CR3——对应 Cinux 的 `activate()`。Cinux 的 `AddressSpace`
本质上就是 Linux `mm_struct` 的最小子集，拿掉了 VMA、信号量、RSS 统计等高级
功能，只保留了最核心的页表根管理。

xv6 的 `proc_freepagetable()` 和 Cinux 的析构函数也有一个重要的差异：xv6 会
先 unmap trampoline 和 trapframe，然后调用 `uvmfree()` 递归释放所有用户页——
注意 xv6 会释放数据页（物理页帧），而 Cinux 的 `free_subtree` 只释放页表结构
页（PDPT/PD/PT），不碰数据页。这是因为 xv6 的每个进程的物理内存完全由该进程
拥有，进程结束时内存全部归还；而 Cinux 将数据页的管理权留给了上层，为将来的
共享内存和 COW fork 做准备。

## 收尾

到这里我们完整走过了 `AddressSpace` 的生命周期——从构造时分配 PML4、到映射时
建立页表链条、到隔离时互不可见、到激活时切换 CR3、到销毁时递归释放。11 个 QEMU
集成测试和 20 个 Host 单元测试从不同角度验证了每一个环节，其中最核心的隔离测试
只有十来行代码却验证了整个 tag 的核心命题。

tag 018 到这里就完成了。我们有了 PMM 管理物理页、VMM 管理页表映射、Heap 管理
动态分配、AddressSpace 管理每进程地址空间——内存管理的四层架构已经基本成型。
下一步自然是进程本身——tag 019 将引入进程控制块和上下文切换，`AddressSpace`
将被嵌入进程结构体中，真正成为"每个进程的独立世界"。

## 参考资料

- Intel SDM Vol.3A Section 4.10.1, Table 4-13：PCID 机制，CR4.PCIDE=1 后 CR3
  低 12 位编码进程标识符，允许缓存多个地址空间的 TLB 条目。
- Intel SDM Vol.3A Section 4.10.2：CR3 写入导致非全局 TLB 条目失效，
  进程切换性能分析的基础。
- Intel SDM Vol.3A Section 4.10.4：`INVLPG` 指令用于单页 TLB 失效，
  未来 unmap 操作的优化方向。
- OSDev Wiki: [TLB](https://wiki.osdev.org/TLB) — TLB 刷新方法，CR3 重载
  和 INVLPG 的使用场景。
- OSDev Wiki: [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel) —
  PML4 分割策略，测试中 `USER_PML4_END = 256` 的设计依据。
- xv6 RISC-V `proc.c`:
  [GitHub](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/proc.c) —
  `proc_pagetable()` 和 `proc_freepagetable()` 的完整实现。
- xv6 RISC-V `vm.c`:
  [GitHub](https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/vm.c) —
  `uvmcreate()`、`uvmfree()`、`uvmcopy()` 的实现，数据页释放策略。
- Linux `mm_struct`:
  [kernel.org 文档](https://www.kernel.org/doc/html/next/mm/process_addrs.html) —
  `fork()`/`exit()`/`switch_mm()` 的完整生命周期。
- Mel Gorman, *Understanding the Linux Virtual Memory Manager*, Chapter 4：
  `dup_mm()`/`mmput()`/`switch_mm()` 的实现细节。
