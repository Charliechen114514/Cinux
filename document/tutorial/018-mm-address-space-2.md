# PML4 分割与内核映射共享：从设计到代码

> 标签：AddressSpace, PML4, init_kernel, 构造函数, 析构函数, 移动语义, RAII
> 前置：[018-1 从一张页表到无数张：为什么进程需要独立的地址空间](018-mm-address-space-1.md)

## 前言

上一篇我们讨论了为什么每个进程需要独立的 PML4，对比了 xv6 的双页表方案和 Linux
的共享内核映射方案，也看过了 `AddressSpace` 类的接口设计概览。这一篇我们直接
扎进代码——`address_space.cpp` 的全部 198 行实现，逐段拆解。

你可能会觉得 198 行代码有什么好拆的？但这里面有不少"看着简单但坑很多"的地方。
比如 `init_kernel()` 就两行代码——但如果你忘了在构造 `AddressSpace` 之前调用它，
构造函数会从物理地址 0 处复制"内核条目"，等着你的就是一个非常漂亮的 page fault。
再比如析构函数的 `free_subtree` 递归释放——如果递归终止条件写错了，要么漏释放
导致内存泄漏，要么把数据页也释放了导致 use-after-free。先把代码读完，再回头看
设计，你会发现每一个看似随意的选择背后都有明确的理由。

## 环境说明

本章涉及的核心文件是 `kernel/mm/address_space.cpp`（198 行）和
`kernel/mm/address_space.hpp`（144 行）。工具链与上一篇一致——GCC cross-compiler、
QEMU、higher-half 内核。外部依赖：`paging.hpp` 提供 `read_cr3()`/`write_cr3()`
和 `PageEntry` 类型，`pmm.hpp`/`vmm.hpp` 提供全局的 `g_pmm`/`g_vmm`。

## 锚定内核页表：init_kernel() 与静态成员

我们从最简单的部分开始。`address_space.cpp` 的开头是静态成员初始化和常量定义。

```cpp
uint64_t AddressSpace::kernel_pml4_ = 0;
```

`kernel_pml4_` 的类外初始化是 C++ 静态成员的标准写法。它在 `init_kernel()` 调用
之前一直是 0——如果你忘了调用 `init_kernel()` 就创建 `AddressSpace`，构造函数
会执行 `phys_to_virt(0)` 也就是 `0 + KERNEL_VMA`，然后从这个地址读取"内核条目"，
结果完全不可预测。

接下来是常量定义：

```cpp
constexpr uint64_t KERNEL_VMA = 0xFFFFFFFF80000000ULL;
constexpr uint32_t USER_PML4_START = 0;
constexpr uint32_t USER_PML4_END   = 256;
constexpr int LEVEL_PDPT = 3;
constexpr int LEVEL_PD   = 2;
constexpr int LEVEL_PT   = 1;
```

`KERNEL_VMA` 是内核虚拟基地址，物理地址到虚拟地址的转换就是 `phys + KERNEL_VMA`。
`USER_PML4_START` 和 `USER_PML4_END` 定义用户空间的 PML4 条目范围：0 到 255，
覆盖 x86-64 虚拟地址的下半部分。`LEVEL_*` 三个常量用于 `free_subtree` 的递归
深度控制——PML4 下面是 PDPT（level 3），PDPT 下面是 PD（level 2），PD 下面
是 PT（level 1）。

然后是 `phys_to_virt`，放在匿名命名空间里（C++ 的"内部链接"惯用法，相当于
`static` 函数但不污染全局命名空间）：

```cpp
namespace {
PageEntry* phys_to_virt(uint64_t phys) {
    return reinterpret_cast<PageEntry*>(phys + KERNEL_VMA);
}
}  // anonymous namespace
```

`phys + KERNEL_VMA` 把物理地址加上 higher-half 偏移得到虚拟地址，再
`reinterpret_cast` 成 `PageEntry*`。这行代码背后有一个关键的假设：内核启动时
建立的 higher-half 直接映射覆盖了整个物理内存范围。如果某个物理地址没有被映射到
`phys + KERNEL_VMA` 处，访问结果就是 page fault。

最后是 `init_kernel()`：

```cpp
void AddressSpace::init_kernel() {
    kernel_pml4_ = cinux::arch::read_cr3();
    cinux::lib::kprintf("[AS] Kernel PML4 saved at phys %p\n",
                        reinterpret_cast<void*>(kernel_pml4_));
}
```

就两行——读 CR3，打印日志。`read_cr3()` 执行 `mov %cr3, %0`。CR3 的 bit 51:12
存放着当前 PML4 表的物理页帧号。因为 Cinux 没有启用 PCID（CR4.PCIDE = 0），
`read_cr3()` 返回的值就是 PML4 的物理地址，低 12 位都是 0（4KB 对齐）。

这个函数在 `kernel_main()` 中作为 Step 9 调用。此时内核页表已经完全建立好，
CR3 指向的是最"标准"的内核 PML4。保存这个值后，后续所有 `AddressSpace` 构造
函数都从这个模板复制内核条目。

## 从零搭建：构造函数的三步走

构造函数是 `AddressSpace` 的核心——它从无到有地创建一张新的 PML4 页表。

```cpp
AddressSpace::AddressSpace() {
    pml4_phys_ = g_pmm.alloc_page();
    if (pml4_phys_ == 0) {
        cinux::lib::kprintf("[AS] FATAL: failed to allocate PML4 page\n");
        return;
    }

    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        pml4[i].raw = 0;
    }

    auto* kern_pml4 = phys_to_virt(kernel_pml4_);
    for (uint32_t i = USER_PML4_END; i < PT_ENTRIES; i++) {
        pml4[i].raw = kern_pml4[i].raw;
    }
}
```

第一步从 PMM 分配一页 4KB 物理内存作为新的 PML4。如果分配失败（PMM 耗尽），
`alloc_page()` 返回 0，构造函数打印错误日志并提前返回——此时 `pml4_phys_` 为 0，
后续所有操作都会失效，析构时也跳过释放。这是一种"退化状态"，对象存在但不可用。
在内核代码中我们不会 throw 异常（内核里没有标准库也没有异常处理运行时），所以
这种"构造失败但不崩溃"的模式是内核 C++ 中常见的做法。

第二步把整个 PML4 清零，512 个条目全部设为 `raw = 0`。PTE 的 bit 0（Present）
为 0 时 MMU 不会做地址翻译，访问对应虚拟地址会触发 page fault。这保证新地址空间
的用户空间从"完全空白"开始——没有任何残留映射。

第三步复制内核条目。从保存的内核 PML4 取出条目 256 到 511，逐条复制到新 PML4
的对应位置。这里用 `raw` 整体赋值——一条拷贝就完整保留了物理地址和所有标志位。
复制完成后，新 PML4 的上半部分和内核 PML4 完全一致。

接下来问题来了：这是"浅拷贝"——只复制了 PML4 条目本身，没有递归复制 PDPT/PD/PT
页表页。所以所有地址空间共享同一套内核空间的页表结构。这安全吗？答案是安全的，
而且这正是我们想要的行为。内核空间的页表在启动时一次性建立好之后几乎不会改变
（唯一的例外是动态 MMIO 映射）。所有地址空间共享同一套页表意味着修改一处全局
生效——如果做了深拷贝，每个地址空间都有独立的内核页表副本，新增映射就需要同步
到所有副本，复杂度不可接受。

## 递归拆毁：析构函数与 free_subtree

析构函数是构造函数的逆操作——把用户空间的页表子树全部释放掉。

```cpp
AddressSpace::~AddressSpace() {
    if (pml4_phys_ == 0) {
        return;
    }

    auto* pml4 = phys_to_virt(pml4_phys_);
    for (uint32_t i = USER_PML4_START; i < USER_PML4_END; i++) {
        if (pml4[i].is_present()) {
            free_subtree(pml4[i].phys_addr(), LEVEL_PDPT);
        }
    }

    g_pmm.free_page(pml4_phys_);
    pml4_phys_ = 0;
}
```

入口是防御性检查：`if (pml4_phys_ == 0) return;`。这覆盖了两种场景——构造函数
分配失败（`pml4_phys_` 一直是 0）和移动后源对象被"掏空"（移动操作把
`pml4_phys_` 置 0）。如果没有这个检查，析构一个被移走的对象就会把 `phys_to_virt(0)`
传给 `free_subtree`，后果不堪设想。

接下来遍历 PML4 的用户空间部分（条目 0 到 255），对每个 present 的条目调用
`free_subtree`。如果条目没有被 map 操作创建过（不 present），直接跳过。
`free_subtree` 完成后，所有子页表页都已归还 PMM。最后归还 PML4 页本身，然后把
`pml4_phys_` 置 0——虽然对象马上销毁，但保持"自洽"状态是良好的编程习惯。

真正的核心在 `free_subtree` 里：

```cpp
void AddressSpace::free_subtree(uint64_t table_phys, int level) {
    auto* table = phys_to_virt(table_phys);

    for (uint32_t i = 0; i < PT_ENTRIES; i++) {
        if (!table[i].is_present()) {
            continue;
        }

        if (level > LEVEL_PT) {
            free_subtree(table[i].phys_addr(), level - 1);
        }

        g_pmm.free_page(table[i].phys_addr());
    }
}
```

这个函数的调用链是：析构函数遍历 PML4[0..255]，对每个 present 条目调用
`free_subtree(entry.phys_addr(), LEVEL_PDPT)`。函数收到页表页的物理地址和当前
层级，遍历所有 512 个条目。

关键在于递归终止条件：`if (level > LEVEL_PT)` 才继续递归。当 `level` 递减到
`LEVEL_PT`（值为 1）时不再递归——递归到此为止。但注意 `free_page` 在循环中是
无条件调用的，所以在 PT 层（level=1），PT 条目指向的数据页仍然会被 `free_page`
释放。在当前没有共享内存机制的设计中，每个 AddressSpace 独占其用户空间的物理页，
析构时全量释放是最简单的策略。递归顺序是"先深入，后释放"——保证释放父级页表页
之前子级已经处理完毕。

这里有个微妙但重要的点：在递归释放子级之后，我们仍然通过 `table[i].phys_addr()`
读取当前条目。这是安全的吗？是的——`free_subtree` 释放的是子级页表页（通过
`g_pmm.free_page(table[i].phys_addr())`），但释放只是把页帧标记为空闲，
内存内容不会立即被清零或覆盖。当前层级的 `table` 指针指向的页在所有子条目处理
完毕之后才由上一级的 `free_page` 释放。

## 所有权转移：移动构造与移动赋值

移动语义是现代 C++ 中管理独占资源的标准手段。`AddressSpace` 只有一个需要转移的
资源——`pml4_phys_`。

```cpp
AddressSpace::AddressSpace(AddressSpace&& other) noexcept
    : pml4_phys_(other.pml4_phys_) {
    other.pml4_phys_ = 0;
}
```

移动构造极其简洁——用 `other.pml4_phys_` 初始化自己的，然后把 `other.pml4_phys_`
置 0。纯粹的"指针偷窃"，被移走的对象进入退化状态（`pml4_phys_ == 0`），后续
析构时检查到 0 就直接 return。`noexcept` 标注很重要——标准库容器（如 `std::vector`）
在 realloc 时会优先使用移动操作，但前提是移动操作是 `noexcept` 的，否则它们
宁可走拷贝（安全性更高的选择）。既然拷贝已经被删了，移动必须标 `noexcept`，
否则容器根本无法编译。

移动赋值多了一步——先释放自己当前的资源，再接管对方的。释放逻辑和析构函数完全
一样——遍历用户空间条目、递归释放子树、归还 PML4 页。`if (this != &other)` 守卫
防止自赋值导致 double-free。你可能会觉得"谁会写 `as = std::move(as)` 这种
代码"——确实没人会手写，但 `container[i] = std::move(container[j])` 当 `i == j`
时间接触发。Host 单元测试里专门用 `auto& ref = as; as = std::move(ref);` 来
验证这个守卫——虽然编译器会发 `-Wself-move` 警告，但运行时的守卫确保了即使绕过
警告也不会出错。

## 委托与激活：map/unmap/translate/activate

最后四个方法——三个委托，一个激活。

```cpp
bool AddressSpace::map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return g_vmm.map(virt, phys, flags, &pml4_phys_);
}

void AddressSpace::unmap(uint64_t virt) {
    g_vmm.unmap(virt, &pml4_phys_);
}

uint64_t AddressSpace::translate(uint64_t virt) {
    return g_vmm.translate(virt, &pml4_phys_);
}

void AddressSpace::activate() {
    cinux::arch::write_cr3(pml4_phys_);
}
```

`map`/`unmap`/`translate` 是纯粹的委托——调用 `g_vmm` 的对应接口，传入
`&pml4_phys_` 作为自定义页表根。VMM 接受可选的 `uint64_t* pml4_out` 参数：
传入非空指针就用它指向的 PML4 做遍历起点，传入 `nullptr` 就用默认内核 PML4。
这种设计让 `AddressSpace` 成了 VMM 的轻量"所有权包装器"。

`activate()` 是整个类中唯一接触硬件寄存器的方法。`write_cr3(pml4_phys_)` 把
PML4 物理地址写入 CR3。根据 Intel SDM Vol.3A Section 4.10.2，写入 CR3 会自动
使所有非 Global 的 TLB 条目失效。这也是为什么进程切换是 TLB miss 的主要来源——
每次切换都丢掉了上一个进程积累的 TLB 缓存。Intel 提供了 PCID（Process-Context
Identifier，详见 SDM Vol.3A Section 4.10.1）机制来缓解这个问题——当
CR4.PCIDE=1 时，CR3 的低 12 位可以编码一个进程标识符，允许 CPU 缓存多个地址空间
的 TLB 条目。但 Cinux 目前没有启用 PCID，每次 `activate()` 都会导致全量 TLB
刷新。

`activate()` 不保存也不恢复之前的 CR3 值——调用者负责在适当时机恢复到内核 PML4。
QEMU 集成测试里凡是调用了 `activate()` 的测试，在 AddressSpace 对象析构之前都
会手动调用 `write_cr3(saved_pml4)` 恢复内核页表。如果忘了这步——析构释放了 CR3
正在使用的 PML4 页，CPU 用已归还的物理内存做地址翻译，三步之内 triple fault。

## 收尾

到这里我们已经把 `address_space.cpp` 的全部代码过了一遍。核心逻辑可以归纳为
三个"一句话"：构造时"从 PMM 借一页 PML4，上半抄内核"，析构时"用户空间的页表
子树全部归还 PMM"，激活时"把 PML4 地址写入 CR3 让硬件切换地址空间"。每一句
背后都有具体的设计考量——浅拷贝内核条目避免了同步问题，递归释放只走 PDPT/PD/PT
三层避免误放数据页，`noexcept` 移动操作保证容器兼容性。

但代码写完了，怎么验证它确实实现了进程隔离？下一章我们看地址空间的完整生命周期——
从创建到映射到切换到销毁，用 11 个 QEMU 集成测试和 20 个 Host 单元测试来验证
每一个环节。其中最核心的那个测试只有十来行代码：创建两个 AddressSpace，在
第一个里 map 一个页面，在第二个里 translate 同一个地址——返回 0。隔离成立。

## 参考资料

- Intel SDM Vol.3A Section 4.5.2, pp.4-20 to 4-21：CR3 寄存器格式，
  `init_kernel()` 和 `activate()` 的硬件基础。
- Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25：四级页表遍历流程，
  `free_subtree` 的递归结构与此对应。
- Intel SDM Vol.3A Section 4.10.1：PCID 机制，CR4.PCIDE=1 后 CR3 低 12 位
  编码进程标识符，允许缓存多个地址空间的 TLB 条目。
- Intel SDM Vol.3A Section 4.10.2：CR3 写入导致非全局 TLB 条目失效，
  `activate()` 的隐式 TLB 刷新机制来源。
- OSDev Wiki: [Paging](https://wiki.osdev.org/Paging) — 四级分页结构详解，
  CR3 格式与 TLB 行为。
- OSDev Wiki: [TLB](https://wiki.osdev.org/TLB) — TLB 刷新方法与 CR3 重载
  的影响。
- Linux `pgd_alloc()` / `pgd_free()`：PGD 分配和释放逻辑，与 Cinux 的构造/
  析构模式高度相似，参考 [kernel-internals.org](https://kernel-internals.org/mm/mmap/)。
