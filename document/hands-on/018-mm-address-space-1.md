# 018-1 进程地址空间的设计与类接口

## 导语

到 tag 017 为止，我们的内核已经拥有了一套完整的内存管理基础设施：PMM 管理物理页的分配回收，VMM 负责虚拟地址到物理地址的映射翻译，Heap 在这之上提供了内核堆分配器。
但这一切都运行在同一个、全局唯一的地址空间里——bootloader 在启动时搭建的那套页表，内核从开机到关机都在用这一张 PML4。如果我们想支持多进程（哪怕只是最原始的两个进程轮流执行），第一件要做的事情就是给每个进程一张独立的页表。
不同的进程看到同一套虚拟地址，但背后对应着不同的物理页——这就是"地址空间隔离"，也是现代操作系统最基本的安全边界之一。这一章我们开始搭建 AddressSpace 这个抽象层，先把设计想清楚、把类接口定义好。
和之前的 tag 不太一样的是，这一篇我们把重点放在"设计"上——别急着动手写代码，磨刀不误砍柴工。

知识前置：你需要理解 tag 016 中实现的 x86-64 四级分页结构（PML4 -> PDPT -> PD -> PT），以及 VMM 的 map/unmap/translate 接口。
本章的核心就是在 VMM 之上再包一层——每个 AddressSpace 持有自己的 PML4 根，操作时把自己的 PML4 地址告诉 VMM。

## 概念精讲

### 为什么需要独立的地址空间

想象一下我们现在的处境：内核运行在一张全局唯一的页表上，所有代码、数据、MMIO 映射都挤在这张表里。到目前为止这没有任何问题，因为我们的内核是唯一的"进程"——键盘轮询循环和中断处理程序共享同一个地址空间，谁也不会干扰谁。
但如果我们想让两个用户程序同时存在于内存中呢？进程 A 在虚拟地址 0x400000 映射了自己的代码段，进程 B 也要在 0x400000 映射自己的代码段——两个进程争同一个虚拟地址，但必须指向不同的物理页。
这在单张页表里根本做不到，因为一个虚拟地址在一张页表里只能映射到一个物理页。

解决方案显而易见：给每个进程一张独立的页表。进程 A 切换运行时，CPU 使用 A 的页表；进程 B 切换运行时，CPU 使用 B 的页表。这样两个进程的 0x400000 就可以各自指向不同的物理页，互不干扰。
x86-64 用 CR3 寄存器来指定当前使用哪张 PML4 表。CR3 里存的是 PML4 表的物理基地址，CPU 每次翻译虚拟地址时都从 CR3 指向的 PML4 开始遍历。
所以"切换地址空间"的本质就是把新进程的 PML4 物理地址写入 CR3。这个操作会隐式刷新 TLB 中所有非全局页的缓存（Intel SDM Vol.3A Section 4.10.2），让 CPU 从新的页表重新翻译所有虚拟地址。

你可能会问：那 xv6 的做法呢？xv6（MIT 的教学操作系统，RISC-V 版本）在每次进入内核态时会从用户页表切换到内核的全局页表（`kernel_pagetable`），离开内核态时再切回来。
这种方式更干净（内核页表和用户页表完全分离），但每次系统调用都要做两次页表切换。
Cinux 选择了另一种方式——和 Linux 一样，把内核映射放到每个进程的页表里，这样进程切换时只需要一次 CR3 写入，而且系统调用和中断处理不需要额外切换页表。两种方式各有取舍，但对于教学操作系统来说，Cinux 的方式更简单。

> 参考：Intel SDM Vol.3A Section 4.5.2, pp.4-20 to 4-21（CR3 在四级分页中的角色）
> 参考：OSDev Wiki — [Higher Half Kernel](https://wiki.osdev.org/Higher_Half_Kernel)
> 参考：xv6 RISC-V `kernel/proc.c` — `proc_pagetable()` 和 `userinit()`

### PML4 的分割：用户空间 vs 内核空间

x86-64 的 48 位 canonical 地址空间在 PML4 层面被天然地一分为二。PML4 有 512 个表项（索引 0-511），每个表项控制 512 GB 的虚拟地址空间。
索引 0-255 覆盖低位地址（0x0000000000000000 - 0x00007FFFFFFFFFFF），这就是用户空间；
索引 256-511 覆盖高位地址（0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF），这就是内核空间。

这个分割并不是硬件强制的——从 CPU 的角度来看，512 个 PML4 条目地位完全平等。
之所以有"用户空间"和"内核空间"的划分，是因为 canonical address 约定：bit 47 是"符号位"，低半部分的 bit 48-63 全是 0，高半部分的 bit 48-63 全是 1。
PML4 索引由虚拟地址的 bit 39-47 决定，所以 bit 47 = 0 对应索引 0-255，bit 47 = 1 对应索引 256-511。
这个分割恰好和 x86 的特权级机制配合——PML4 条目的 U/S 位（bit 2）控制着这 512 GB 区域是否允许用户态（ring 3）访问。

这个分割给我们的设计带来了一个极其便利的特性：每个进程的 PML4 表中，用户空间的那一半（条目 0-255）是进程私有的，而内核空间的那一半（条目 256-511）在所有进程之间是完全相同的。
所有进程共享同一套内核映射——这和 Linux 的做法如出一辙。
Linux 的 `pgd_alloc()` 在创建新的页全局目录时，就是从内核的 PGD 中复制高半部分条目（源码见 `mm/pgtable.c`），Cinux 做的事情在原理上完全一样。
所以当一个新进程被创建时，我们只需要分配一张全新的 PML4 表，清零所有 512 个条目，然后把内核 PML4 的条目 256-511 逐个复制过来。
这张新 PML4 表的用户空间部分是空的——就像一张白纸，进程可以自由地在上面建立自己的映射。这种"上半部分私有、下半部分共享"的模式，就是所谓的 Higher Half Kernel 设计。

> 参考：Intel SDM Vol.3A Section 4.5.4, pp.4-22 to 4-25（四级分页地址翻译流程）
> 参考：Intel SDM Vol.3A Table 4-15, pp.4-26 to 4-27（PML4 条目格式）
> 参考：OSDev Wiki — [Paging](https://wiki.osdev.org/Paging)

### init_kernel()：保存"模板"

在内核启动的早期阶段，bootloader 已经帮我们搭建好了一套完整的页表——identity map、higher-half map、MMIO 映射等等。这套页表的 PML4 物理地址存在 CR3 里。
我们的 `init_kernel()` 静态方法只需要做一件事：把 CR3 的值读出来，保存到一个静态成员变量中。
这个被保存的值就是"内核 PML4 模板"——以后每创建一个新的 AddressSpace，都从这个模板复制内核部分的条目（PML4[256-511]）。

调用时机很重要：`init_kernel()` 必须在 VMM 初始化之后、任何 AddressSpace 对象创建之前调用。
因为 VMM 的 `init()` 完成后，页表才处于一个稳定的、我们可以信赖的状态——VMM 可能会在初始化过程中修改 CR3 指向的页表（比如映射 MMIO 区域），所以我们必须等 VMM 完全就绪后再读取 CR3。
而如果在 `init_kernel()` 之前就尝试构造 AddressSpace，内核 PML4 的值还是 0，复制过来的内核条目就全是空的——一旦 activate 到那张页表，内核立刻就失去所有映射，CPU 连下一条指令都取不到，
接下来就是三重故障重启了。在 Linux 中，等价的工作是由 `swapper_pg_dir` 完成的——内核在编译时就把初始页表静态分配好了，启动后所有进程的 PGD 都从这个模板复制。
Cinux 的做法更动态一些：我们在运行时读取 CR3，因为我们的初始页表是 bootloader 动态构建的，地址不是固定的。

### 类设计概览

在动手写接口之前，我们先整理一下 AddressSpace 类需要提供的全部能力。从职责上分，它要做三组事情。
第一组是生命周期管理：构造函数创建一张新的 PML4 表（从 PMM 分配、清零、复制内核条目），析构函数释放所有用户空间页表页和 PML4 页本身，拷贝被禁止，移动被允许——这是 RAII 管理物理页所有权的标准做法。
第二组是页表操作：map、unmap、translate 三个方法委托给 VMM，但传入自己的 PML4 根而不是用默认的内核 PML4，这样每个 AddressSpace 的映射操作只影响自己的页表，不会污染其他地址空间。
第三组是地址空间切换：activate() 把当前 AddressSpace 的 PML4 物理地址写入 CR3，让 CPU 切换到这个地址空间。
这个操作是进程切换的核心——未来调度器在切换进程时，就会调用新进程的 AddressSpace 的 activate()。

## 动手实现

### Step 1: 创建 AddressSpace 类声明文件

**目标**: 在 `kernel/mm/address_space.hpp` 中定义 AddressSpace 类的完整接口，包括构造/析构、静态初始化、页表操作和成员变量。

**设计思路**: AddressSpace 是一个 RAII 风格的资源管理类——构造时分配 PML4 物理页并初始化，析构时递归释放所有用户空间页表并归还 PML4 页。类的核心数据成员只有一个：`pml4_phys_`，一个 `uint64_t`，保存着这张 PML4 表的物理基地址。此外还有一个静态成员 `kernel_pml4_`，保存启动时从 CR3 读出的内核 PML4 物理地址——所有 AddressSpace 实例共享这一个值。构造函数执行三步操作：从 PMM 分配一页作为新的 PML4 表，把整张表清零（512 个条目全部置 0），从内核 PML4 复制条目 256-511。析构函数负责清理：遍历用户空间部分（PML4 条目 0-255），对每个 present 的条目递归释放整棵子树（PDPT -> PD -> PT），最后释放 PML4 页本身。递归释放子树的逻辑放在私有方法 free_subtree 中，它接收一个页表的物理地址和当前层级（3 表示 PDPT，2 表示 PD，1 表示 PT），递归遍历并释放所有中间级页表页。

拷贝语义被明确删除（`= delete`），因为两个 AddressSpace 不能共享同一张物理 PML4 表。
移动语义是支持的——移动构造和移动赋值把 pml4_phys_ 的所有权转移给目标对象，源对象的 pml4_phys_ 被置 0，这样源对象析构时检测到 0 就跳过释放。
公开操作包括 map(virt, phys, flags) 返回 bool 表示映射是否成功、unmap(virt) 无返回值、translate(virt) 返回物理地址（未映射返回 0），
以及 activate() 把 pml4_phys_ 写入 CR3。前三个直接委托给 VMM 的对应方法，传入 `&pml4_phys_` 作为自定义 PML4 根。

**实现约束**: 命名空间 `cinux::mm`，头文件用 `#pragma once` 作为 include guard。需要 include `<stdint.h>`。类声明中把构造/析构、拷贝删除、移动支持、静态初始化、页表操作、访问器分成不同的注释区块，保持可读性。

**踩坑预警**: `kernel_pml4_` 是一个静态成员变量，它必须在 .cpp 文件中被定义并初始化为 0——如果你忘了在实现文件中写这行定义，链接器会报未定义符号错误（`undefined reference to cinux::mm::AddressSpace::kernel_pml4_`）。另外，删除拷贝构造和拷贝赋值的同时一定要提供移动构造和移动赋值，否则 AddressSpace 对象就无法被放入容器——未来的进程表会需要这个能力。

**验证**: 这个步骤只需要编译通过即可。类声明本身不涉及运行时行为，在后续步骤中实现各个方法后才能做运行时验证。

### Step 2: 实现 init_kernel() 静态方法

**目标**: 在 `kernel/mm/address_space.cpp` 中实现 `init_kernel()`，读取当前 CR3 并保存为内核 PML4 模板。

**设计思路**: 实现非常直截了当——调用之前在 tag 016 中写好的 `read_cr3()` 内联函数，把返回值赋给静态成员 `kernel_pml4_`。然后用 kprintf 打印一条日志，显示保存的 PML4 物理地址，方便后续调试时确认 init 是否正确执行。日志格式建议使用 `[AS]` 前缀，这样可以通过 `grep '\[AS\]'` 快速过滤出 AddressSpace 相关的输出。在 kernel/main.cpp 的启动序列中，我们需要把 `init_kernel()` 的调用插入到 VMM 初始化之后、Heap 初始化之前。具体修改是把原来的 Step 9（Heap 初始化）往后挪一位变成 Step 10，新插入一个 Step 9 来调用 `AddressSpace::init_kernel()`。同时需要在 main.cpp 顶部新增 `#include "kernel/mm/address_space.hpp"`。

**实现约束**: address_space.cpp 需要包含 kernel/arch/x86_64/paging.hpp（获取 read_cr3）、kernel/lib/kprintf.hpp（日志输出）、kernel/mm/address_space.hpp。静态成员 kernel_pml4_ 的定义（`uint64_t AddressSpace::kernel_pml4_ = 0;`）要放在匿名命名空间之外、命名空间 `cinux::mm` 之内。这是 C++ 的要求——静态成员变量只能在类声明中"声明"，必须在某个 .cpp 文件中"定义"一次。

**踩坑预警**: 调用 init_kernel() 的时机是关键。如果你在 VMM 初始化之前就调用了它，CR3 指向的可能还是 bootloader 临时搭建的页表，而不是内核最终的页表——虽然在这个项目中两者的 PML4 恰好相同（bootloader 和内核使用同一张 PML4 表），但这是一个脆弱的隐式假设。正确的调用位置是 VMM init() 之后。另外，init_kernel() 只能调用一次——重复调用会覆盖掉之前保存的值。如果未来需要更严格的保障，可以加一个 `static bool initialized = false;` 的标志位，重复调用时直接 panic。

**验证**: 修改 main.cpp 后重新编译运行内核，在串口输出中应该能看到类似 `[AS] Kernel PML4 saved at phys 0x...` 的日志。这个物理地址应该和之前 VMM 初始化时输出的 PML4 地址一致——通常是 0x10000 或 0x20000 附近的一个 4KB 对齐地址。

```bash
# 编译并运行内核，检查 init_kernel 日志
cmake --build build --target big_kernel && \
  qemu-system-x86_64 -kernel build/big_kernel.bin -serial stdio -display none 2>&1 | grep '\[AS\]'
```

## 构建与运行

到目前为止我们只实现了类声明和 init_kernel()。
编译后运行内核，串口输出中应该出现 `[AS] Kernel PML4 saved at phys ...`，后续的 Heap、Framebuffer、Console 初始化应该不受影响——因为我们只是读取并保存了 CR3，
没有修改任何页表。
如果串口输出在 [AS] 日志之后突然中断，大概率是 init_kernel() 的插入位置不对——检查 main.cpp 中的调用顺序是否正确，确认 VMM 在 Step 8 初始化，AddressSpace 在 Step 9 初始化。
另外记得更新 kernel/CMakeLists.txt，把 mm/address_space.cpp 加入 big_kernel_common 的源文件列表中。

## 调试技巧

**init_kernel 返回 0**: 如果日志显示 `Kernel PML4 saved at phys (nil)` 或 0x0，说明 read_cr3() 返回了 0。这几乎不可能发生——CR3 在 bootloader 阶段就被设置了。如果真的出现了，大概率是你在错误的时机调用了 init_kernel——比如在分页还没启用之前。检查 main.cpp 中的调用顺序。

**链接错误 "undefined reference to kernel_pml4_"**: 这是忘记在 .cpp 文件中定义静态成员变量的典型症状。确保 address_space.cpp 中有 `uint64_t AddressSpace::kernel_pml4_ = 0;` 这行，而且它在 `cinux::mm` 命名空间内，不在匿名命名空间里。

**编译错误 "use of deleted function"**: 如果你尝试在测试代码中拷贝一个 AddressSpace 对象（比如 `auto as2 = as1;`），编译器会报这个错——这正是我们想要的。如果你确实需要转移所有权，使用 `std::move()`。

**CMakeLists 忘记添加源文件**: 如果编译时出现 `undefined reference to cinux::mm::AddressSpace::init_kernel()` 之类的链接错误，检查 kernel/CMakeLists.txt 中 big_kernel_common 的源文件列表是否包含了 mm/address_space.cpp。

## 本章小结

到这里我们已经完成了 AddressSpace 的骨架：类接口定义清晰，init_kernel() 在启动时保存了内核 PML4 模板，CMakeLists 也更新好了。
整个模块的"蓝图"已经画出来了——RAII 风格的 PML4 所有权管理、拷贝删除 + 移动支持、委托给 VMM 的页表操作、CR3 切换的 activate。
下一步是实现构造函数和析构函数——那是资源管理的核心，也是最容易踩坑的地方（递归释放子树要精确控制终止层级、move 语义的所有权转移要保证自赋值安全）。这些留到下一篇文章。
