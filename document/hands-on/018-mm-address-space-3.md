# 018-3 CR3 切换、委托操作与隔离验证

## 导语

前两篇我们搭好了 AddressSpace 的骨架——类接口、构造/析构、move 语义全部就位，内核编译运行正常，init_kernel() 在启动时成功保存了内核 PML4 模板。但一个地址空间光"存在"还不够，它得能"用起来"。
这一篇要做三件事：实现 activate() 方法让 CPU 切换到指定的地址空间（本质就是往 CR3 写入一个新值）；实现 map/unmap/translate 对 VMM 的委托调用；
最后写两组测试来验证整个地址空间隔离机制——一组是在 QEMU 中运行的内核集成测试，一组是在开发机上运行的 host 单元测试。在 AS#1 里映射的页面，在 AS#2 里完全不可见——证明这一点就是 tag 018 的终极目标。

知识前置：理解 CR3 切换和 TLB 刷新的关系（tag 016 的概念），以及 VMM 的 map(virt, phys, flags, pml4_root) 接口——当第四个参数不为空时，
VMM 使用传入的 PML4 根而不是默认的内核 PML4 来遍历页表。你还需要了解项目的测试框架（big_kernel_test.h 中的 TEST_ASSERT_* 宏和 host 端的 TEST 宏）。

这一篇和前两篇的关系是：前两篇实现了 AddressSpace 的全部核心代码（类声明、构造/析构/move/init_kernel），这一篇实现剩余的四个方法（activate、map、unmap、translate）并用测试来验证整个模块。如果你跳过了前两篇直接来到这里，请至少先实现 init_kernel() 和构造函数——后续的测试依赖这两个方法。

## 概念精讲

### CR3 切换：地址空间切换的物理实现

activate() 做的事情用一句话就能说完：把 AddressSpace 的 pml4_phys_ 写入 CR3。就这么简单——但这个操作的后果是全局性的、不可逆的（至少在不再次调用 activate 之前）。
Intel SDM Vol.3A Section 4.5.2 明确指出，CR3 中的物理地址指向当前使用的 PML4 表。当你往 CR3 写入一个新值后，CPU 后续所有的虚拟地址翻译都从新的 PML4 开始遍历。
这意味着从这一刻起，CPU 看到的"虚拟地址世界"完全变了——它用的是新的页表，新的映射，新的地址空间。

写 CR3 还有一个隐式副作用：TLB 刷新。
Intel SDM Vol.3A Section 4.10.2 说明，当 CR4.PCIDE = 0 时（Cinux 没有开启 PCID），加载新的 CR3 值会使所有非全局页（G bit = 0 的页）的 TLB 缓存失效。
这是一件好事——我们不需要手动 flush TLB，硬件自动帮我们做了。当你从一个地址空间切换到另一个时，旧地址空间中的用户空间映射在 TLB 中的缓存全部失效，CPU 会从新地址空间的页表重新翻译——这正是我们想要的行为。

但这也意味着 activate() 的性能开销不小：CR3 切换后，CPU 需要重新遍历四级页表来填充 TLB，这会导致一段时间的 TLB miss 风暴。
在真实的操作系统中，这是进程切换的主要开销之一，也是 PCID（Process-Context Identifiers）机制被引入的原因。
PCID 允许 CPU 为不同的地址空间维护独立的 TLB 缓存（Intel SDM Vol.3A Section 4.10.1），避免每次切换都全部刷新。当 CR4.PCIDE=1 时，CR3 的低 12 位可以编码一个 12 位的进程标识符，TLB 条目会打上这个标识符的标签。Linux 从 3.17 版本开始支持 PCID，显著降低了进程切换的开销。
Cinux 目前不需要关心这个优化——我们的系统还很原始，不会有频繁的进程切换，PCID 留作未来的性能优化项目。

> 参考：Intel SDM Vol.3A Section 4.5.2, pp.4-20 to 4-21（CR3 与 PML4 的关系）
> 参考：Intel SDM Vol.3A Section 4.10.2（TLB 刷新行为）
> 参考：Intel SDM Vol.3A Section 4.10.1, Table 4-13（PCID 机制）
> 参考：OSDev Wiki — [TLB](https://wiki.osdev.org/TLB)

有一点需要特别强调：activate() 的安全性完全依赖于构造函数的正确性。
因为我们构造 AddressSpace 时复制了内核 PML4 的条目 256-511，所以 activate 之后内核映射（higher-half 直接映射、MMIO 映射等）依然有效——CPU 可以继续取指令、访问内核数据、读写串口。
但如果你在构造函数里忘了复制内核条目，或者复制的是全零的模板（因为 init_kernel 没被调用），activate 之后内核立刻就失去所有映射——CPU 连下一条指令都取不到，直接三重故障重启。
所以 activate() 是一把双刃剑：用对了，地址空间切换丝滑无比；用错了，一秒钟炸回 QEMU 命令行。

> 参考：Intel SDM Vol.3A Section 4.5.2, pp.4-20 to 4-21（CR3 与 PML4 的关系）
> 参考：Intel SDM Vol.3A Section 4.10.2（TLB 刷新行为）
> 参考：Intel SDM Vol.3A Section 4.10.1, Table 4-13（PCID 机制）
> 参考：OSDev Wiki — [TLB](https://wiki.osdev.org/TLB)

### 委托模式：AddressSpace 作为薄包装

map、unmap 和 translate 这三个操作不需要 AddressSpace 自己实现页表遍历——tag 016 的 VMM 已经把四级页表遍历写得清清楚楚了，包括中间级页表的按需分配、PTE 的设置、
TLB 的单页刷新（invlpg）。AddressSpace 只需要把自己的 pml4_phys_ 传给 VMM，告诉它"请用我的 PML4 根来操作"就行了。

这就是"委托"模式：AddressSpace 是一个 RAII 风格的所有权包装器，它管理 PML4 页的生命周期（分配、初始化、释放），但具体的映射操作委托给 VMM 执行。
VMM 的 map/unmap/translate 方法都有一个可选的 pml4_root 参数——传空指针时使用默认的内核 PML4（全局地址空间），传非空指针时使用指针指向的值作为 PML4 根。
所以 AddressSpace 的 map 调用就是 `g_vmm.map(virt, phys, flags, &pml4_phys_)`——传入 pml4_phys_ 的地址，VMM 解引用这个指针拿到 PML4 物理地址，
然后从这张 PML4 开始遍历。unmap 和 translate 同理。

这种设计让 AddressSpace 成了一个非常轻量的类——它的公开接口只有十来个方法，核心数据成员只有两个 8 字节的整数。但它承担了非常重要的语义角色：把"页表根的生命周期管理"和"页表遍历算法"这两个职责清晰地分离开来。如果你以后需要在页表操作中加日志、加锁、或者支持 5 级分页，只需要修改 VMM——AddressSpace 自动受益。

这种设计避免了代码重复——页表遍历逻辑只在 VMM 中存在一份。如果以后需要修改遍历逻辑（比如支持大页映射、支持 5 级分页），只需要改 VMM，AddressSpace 自动受益。
AddressSpace 的三个委托方法加起来不到十行代码，但它们把"在哪个地址空间中操作"这个语义清晰地表达了出来——你在 AddressSpace 上调 map，就是在它私有的 PML4 里建映射；
在内核全局 VMM 上调 map（不传 pml4_root），就是在内核的页表里建映射。

### 跨地址空间隔离：tag 018 的终极验证

隔离验证的核心思路是这样的：创建两个 AddressSpace 对象 AS#1 和 AS#2。在 AS#1 中映射一个虚拟地址（比如 0x20000000）到某个物理页（从 PMM 申请）。
然后检查两件事——AS#1 的 translate(0x20000000) 应该返回那个物理页的地址（映射有效），而 AS#2 的 translate(0x20000000) 应该返回 0（映射不存在）。
同一个虚拟地址，在两个不同的地址空间中产生了不同的翻译结果——这就证明了两个地址空间的用户空间部分是完全独立的。

更进一步，我们还可以验证 activate 的效果：在 AS#1 映射了页面之后，调用 AS#1 的 activate() 把 CR3 切换到 AS#1 的 PML4。此时 CPU 在翻译用户空间地址时使用的就是 AS#1 的页表。
然后切回内核 PML4（这一步不能忘！），再销毁 AS#1——这就是一个完整的"创建 -> 映射 -> 激活 -> 切回 -> 销毁"的生命周期。

这些验证需要两组测试来实现。第一组是 QEMU 集成测试——运行在真实的内核环境中，使用真实的 PMM 和 VMM，测试结果最可信。集成测试共 11 个用例，覆盖 init_kernel 验证、构造基本正确性、map/unmap/translate 操作、跨空间隔离（核心里程碑）、CR3 切换验证和析构安全性。
第二组是 host 单元测试——在开发机上编译运行，使用模拟的 PMM（位图分配器）和模拟的 VMM（简化的页表遍历），优点是执行速度快、可以用 valgrind 和 AddressSanitizer 检查内存错误。Host 测试共 20 个用例，覆盖构造（4个）、析构（2个）、移动语义（4个）、init_kernel（1个）、map/unmap/translate（3个）、隔离（2个）、activate（1个）、子树释放（2个）和内核保护（1个）。
两组测试互补：集成测试验证真实环境下的行为，单元测试验证逻辑的正确性。

测试策略的设计也值得简要说明。为什么不只用一组测试？QEMU 集成测试的优势是"真实"——操作真正的 CR3、PMM、VMM，能发现硬件交互层面的 bug（比如 activate 后忘了恢复 CR3）。但 QEMU 测试需要完整启动内核（GDT、IDT、PMM、VMM 全初始化），运行在秒级，不适合频繁执行。而且真实 PMM 被整个内核共享，测试无法独占来精确计数分配/释放次数。Host 单元测试的优势是"快速且可观测"——毫秒级完成 20 个用例，可以精确检查 PMM 的内部状态。但它不能验证硬件交互——毕竟没有真正的 CR3 和 TLB。

## 动手实现

### Step 6: 实现 activate() 方法

**目标**: 在 address_space.cpp 中实现 activate()，将当前 AddressSpace 的 PML4 物理地址写入 CR3。

**设计思路**: 调用 tag 016 中封装好的 write_cr3() 内联函数，参数是 pml4_phys_。一行代码就够了。但你要理解这一行代码背后的全部含义——write_cr3 会触发一次完全的 TLB 刷新（所有非全局页），所以 activate() 调用之后，之前地址空间中的用户空间映射在 TLB 中全部失效。内核映射不受影响——因为所有 AddressSpace 的 PML4 都复制了相同的内核条目，指向相同的内核 PDPT 表。

**实现约束**: 需要使用 `cinux::arch::write_cr3()`（定义在 kernel/arch/x86_64/paging.hpp 中）。调用之前不需要任何特殊准备——CR3 可以在任何时候被写入，前提是你写入的 PML4 物理地址是有效的。

**踩坑预警**: 这是最危险的一步，怎么强调都不过分。如果你在 activate() 之后没有及时切回内核 PML4，而接下来的代码又访问了仅在内核 PML4 中映射的地址（比如堆分配、kprintf 的缓冲区），内核可能会继续运行一段时间（因为 TLB 中还有残留的缓存），但一旦 TLB miss 发生而新页表里没有对应的映射，就会触发 page fault。在我们的设计中，所有 AddressSpace 的 PML4 都复制了内核条目，所以内核映射在切换后依然有效——这是一个关键的安全网。但如果你在构造函数里忘了复制内核条目，activate() 之后内核立刻就失去映射了。

**验证**: 在 activate() 调用前后分别读取 CR3（read_cr3()），验证 CR3 确实变成了 AddressSpace 的 pml4_phys() 值。注意：验证完之后一定要把 CR3 切回内核 PML4——write_cr3(AddressSpace::kernel_pml4())——否则后续代码运行在一个"别人"的地址空间里，一旦这个 AddressSpace 被析构，CPU 就在用一张已经不存在的页表了。

activate() 之后内核仍然能正常运行的关键是：构造函数已经复制了 PML4[256..511] 的内核条目到新地址空间中。这意味着 activate 后内核映射（higher-half 直接映射、MMIO 映射、堆映射等）依然有效——CPU 可以继续取指令、访问内核数据。但如果你在构造函数里忘了复制内核条目，或者复制的是全零的模板（因为 init_kernel 没被调用），activate 之后内核立刻失去所有映射——CPU 连下一条指令都取不到，直接三重故障。

activate() 是一把双刃剑：用对了，地址空间切换丝滑无比；用错了（忘记恢复 CR3、构造函数有 bug），一秒钟炸回 QEMU 命令行。所以在测试代码中使用 activate() 时要格外小心。

### Step 7: 实现 map/unmap/translate 委托

**目标**: 实现 map、unmap 和 translate 三个方法，将操作委托给 VMM。

**设计思路**: 三个方法都是单行委托。map(virt, phys, flags) 调用 `g_vmm.map(virt, phys, flags, &pml4_phys_)`，返回 VMM 的返回值——一个 bool，表示映射是否成功（如果中间级页表分配失败会返回 false）。unmap(virt) 调用 `g_vmm.unmap(virt, &pml4_phys_)`，没有返回值。translate(virt) 调用 `g_vmm.translate(virt, &pml4_phys_)`，返回物理地址——如果该虚拟地址在当前 AddressSpace 的页表中没有映射，返回 0。关键点在于传入的是 &pml4_phys_——PML4 物理地址的指针，而不是值。VMM 需要的是一个指针，因为它要解引用来获取 PML4 根。三个方法都不需要额外的 TLB 刷新或错误处理——VMM 内部已经在 map/unmap 后对目标地址执行了 invlpg。

**实现约束**: 需要 include kernel/mm/vmm.hpp 来获取 g_vmm。三个方法的声明和实现都很简洁，可以直接放在 .cpp 文件中。

**踩坑预警**: 如果 g_vmm 没有被正确初始化（比如在 VMM init 之前就创建了 AddressSpace 并调用了 map），委托调用会使用未初始化的 VMM 状态，结果完全不可预测。在我们的启动序列中，VMM 在 Step 8 初始化，AddressSpace 的 init_kernel 在 Step 9，所以正常启动流程不会有问题。但如果你在测试代码中手动创建 AddressSpace 对象，必须确保 VMM 已经初始化。

**验证**: 创建一个 AddressSpace，调用 map 映射一个用户空间地址（比如 0x20000000），传入 FLAG_PRESENT | FLAG_WRITABLE 作为 flags。然后调用 translate 检查是否返回了正确的物理地址。接着调用 unmap，再 translate 应该返回 0。

### Step 8: 编写集成测试——跨地址空间隔离验证

**目标**: 在 kernel/test/test_address_space.cpp 中编写集成测试，在 QEMU 中运行，验证地址空间隔离。

**设计思路**: 集成测试运行在 QEMU 的内核环境中，使用真实的 PMM 和 VMM。我们设计一组测试用例来覆盖 AddressSpace 的所有核心功能。第一组测试验证基础功能：init_kernel 保存了非零的内核 PML4 值；构造函数创建了一张和内核 PML4 不同的 PML4 表（物理地址不同）；两个 AddressSpace 实例有不同的 PML4 根。第二组测试验证映射操作：map 一个页面后 translate 返回正确的物理地址；unmap 后 translate 返回 0；translate 一个从未映射过的地址返回 0；同一地址空间内映射两个不同页面都能正确翻译。第三组是核心隔离测试：创建 AS#1 和 AS#2，在 AS#1 中映射虚拟地址 V。验证 AS#1 的 translate(V) 返回正确的物理地址，而 AS#2 的 translate(V) 返回 0。第四组测试验证 activate 的行为：创建一个 AddressSpace 并 activate，然后读取 CR3 确认它变成了 AddressSpace 的 pml4_phys() 值。第五组测试验证析构安全：在一个作用域内创建 AddressSpace 并映射页面，作用域结束后对象析构，验证内核 PML4 的值没有变化。

测试入口函数需要在 kernel/test/main_test.cpp 中注册——声明 `extern "C" void run_address_space_tests()`，在 kernel_main 中的 Heap 测试之后调用。
记得在调用测试之前先执行 AddressSpace::init_kernel()。测试入口函数被声明为 extern "C" 是为了避免 C++ 的名称修饰（name mangling），让 main_test.cpp 能通过函数名直接调用。

集成测试中每个测试用独立命名空间包裹（比如 test_as_init、test_as_construct、test_as_isolation），避免变量名冲突。这个命名惯例在 big_kernel_test 框架中很常见——所有测试用例在同一个二进制中运行，命名空间是隔离变量最简单的方式。

**实现约束**: 测试文件使用项目的 big_kernel_test.h 框架（TEST_ASSERT_EQ、TEST_ASSERT_NE、TEST_ASSERT_TRUE 等宏）。每个测试放在独立的命名空间中（比如 test_as_init、test_as_construct、test_as_isolation），避免变量名冲突。入口函数声明为 `extern "C" void run_address_space_tests()`。

**踩坑预警**: 这一点怎么强调都不够——在测试中使用 activate() 的用例，必须在 AddressSpace 对象离开作用域之前调用 write_cr3(saved_pml4) 恢复 CR3。如果你忘了这一步，测试代码本身还能跑（因为所有 AddressSpace 的 PML4 都有内核映射），但对象析构时会释放 PML4 物理页——而 CPU 正在使用这张 PML4 表。释放后 CPU 再做任何页表翻译都可能读到被覆盖的垃圾数据，触发三重故障。这个问题在 host 端单元测试中不会出现（因为没有真正的 CR3），只有在 QEMU 集成测试中才会暴露——而且可能不是每次都崩溃，取决于 PMM 什么时候把释放的页重新分配出去。这种"偶尔崩溃"的 bug 是最难定位的，所以提前预防比事后排查重要得多。

**验证**: 编译 big_kernel_test 目标并在 QEMU 中运行。串口输出中应该能看到 AddressSpace Tests (018) 的测试节标题，后面跟着 11 个 PASS。特别关注跨空间隔离测试和 activate 测试——如果这两个通过，说明整个 AddressSpace 机制工作正常。

```bash
# 编译并运行集成测试
cmake --build build --target big_kernel_test && \
  qemu-system-x86_64 -kernel build/big_kernel_test.bin \
    -serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04 2>&1 | \
    grep -A 20 "AddressSpace Tests"
```

### Step 9: 编写 Host 端单元测试

**目标**: 在 test/unit/test_address_space.cpp 中编写 host 端单元测试，使用模拟的 PMM 和 VMM 验证 AddressSpace 的逻辑。

**设计思路**: Host 端测试不运行在 QEMU 中，而是直接在开发机上编译执行。因为没有真正的 PMM 和 VMM，我们需要用模拟的替代品。模拟 PMM 用一个简单的位图分配器实现——维护一个固定大小的物理页池（比如 512 页，基地址从 0x2000000 开始），alloc_page 从池中分配第一个空闲页，free_page 归还给池。位图用 uint8_t 数组实现，每个 bit 代表一个物理页的占用状态。同时记录 alloc_count 和 free_count，方便测试中断言资源的分配释放是否匹配。模拟页表内存用一块 host 端的对齐数组：`alignas(4096) uint8_t sim_memory[SIM_PAGES][4096]`。sim_virt_of(phys) 函数把模拟的物理地址转换成数组中的指针——这模拟了 Cinux 中 higher-half 直接映射的角色。物理地址在 Host 测试中变成纯逻辑概念——只是 sim_memory 数组的索引偏移量。

在模拟环境上重新实现一个 TestAddressSpace 类，它的逻辑和真正的 AddressSpace 完全相同，但使用模拟的 PMM 和 VMM。
然后编写测试用例覆盖：构造（分配 PML4、清零用户空间条目、复制内核条目）、析构（释放 PML4 和子树）、move 构造/赋值（所有权转移和自赋值安全）、map/translate/unmap（基本功能）、跨空间隔离。
Host 端测试需要在 test/CMakeLists.txt 中注册——添加 test_address_space 可执行文件，编译时定义 -DCINUX_HOST_TEST，添加到 test_host 和 test 的依赖列表中。

每个测试用例使用独立的 TestFixture 实例来重置状态——TestFixture 的构造函数清零 sim_memory，setup_kernel_pml4() 方法分配一个假的内核 PML4 并在条目 256 和 511 设置 present 条目（用于验证复制逻辑）。这确保了测试之间的完全隔离，前一个测试的状态不会泄漏到后一个测试中。

**实现约束**: 编译条件 -DCINUX_HOST_TEST。使用项目自己的 test_framework.h（TEST 宏和 RUN_ALL_TESTS）。TestAddressSpace 不链接任何内核代码——它完全独立实现，包括 PageEntry 联合体、分页常量等。模拟池大小在 256 页左右对于单元测试是够用的。

**踩坑预警**: 模拟物理内存的大小（SIM_PAGES）决定了你能分配多少页。如果测试中创建的 AddressSpace 太多或者 map 的页面太多，模拟 PMM 会 OOM 返回 0。每个 AddressSpace 的构造需要 1 页 PML4，map 一个页面需要最多 3 页中间级页表（PDPT + PD + PT）。如果你创建了 10 个 AddressSpace 各 map 10 个页面在不同位置，那需要 10 + 300 = 310 页——已经超出了 256 页的模拟池。所以测试用例要控制 AddressSpace 的数量，一般同时存在 2-3 个就够了。另一个需要注意的地方是测试之间的状态隔离——每个测试用例应该创建独立的 TestFixture 实例（包含独立的 MockPMM 和干净的 sim_memory），避免前一个测试的状态泄漏到后一个测试中。

**验证**: 通过 CTest 运行 host 端测试，所有测试应该通过。Host 测试执行速度快（毫秒级），适合在开发过程中频繁运行。如果测试失败，仔细看错误信息——它会告诉你哪个断言失败了，以及实际的返回值是什么。

Host 测试中的 MockPMM 用一个 512 位的位图管理模拟物理页池。如果测试中创建的 AddressSpace 太多或 map 的页面太多（每个 AddressSpace 的构造需要 1 页 PML4，map 一个页面需要最多 3 页中间级页表），模拟 PMM 会 OOM 返回 0。所以测试用例中同时存在的 AddressSpace 一般不超过 2-3 个。

Host 测试的另一个重要设计决策是：TestAddressSpace 完全复刻了内核 AddressSpace 的逻辑，而不是直接链接内核代码。这看起来像代码重复，但有两个好处：Host 环境中没有 g_pmm、g_vmm、write_cr3、phys_to_virt 这些依赖，mock 的工程量和复刻相当；而且复刻的逻辑完全透明，调试时能直接看到模拟层的内部状态。

```bash
# 运行 host 端单元测试
cmake --build build --target test_address_space && \
  cd build && ctest -R address_space --output-on-failure
```

## 构建与运行

整个 tag 018 的验证分两层，分别对应上面的两组测试。第一层是 host 端单元测试。
在项目根目录执行 `cmake --build build --target test_host` 或直接运行 `ctest -R address_space --output-on-failure`。
你应该看到所有测试通过——包括构造验证、析构验证、move 语义验证、map/unmap/translate 功能验证、以及跨空间隔离验证。Host 测试执行速度快（毫秒级），适合在开发过程中频繁运行。第二层是 QEMU 集成测试。
构建 big_kernel_test 后用 QEMU 启动，串口输出中应该显示 11 个 AddressSpace 测试全部通过。
重点关注的是 test_cross_space_isolation（跨空间隔离）和 test_activate_changes_cr3（CR3 切换）——前者验证了地址空间隔离的核心功能，后者验证了 CR3 切换的正确性。

```bash
# 完整验证流程
cmake --build build --target test && cd build && ctest --output-on-failure
```

## 调试技巧

**activate 后三重故障**: 如果在 activate() 调用后立刻三重故障，说明目标 AddressSpace 的 PML4 表有问题——要么内核映射没复制好（构造函数 bug），要么 PML4 物理地址本身就是无效的（分配失败但你没检查）。用 QEMU 的 `-d int` 参数查看异常信息，确认 CR2 的值和 error code。如果 CR2 指向一个内核空间的地址（比如 0xFFFFFFFF80000000 附近），说明内核映射没复制到新 PML4 中；如果 CR2 指向用户空间地址，那可能是正常的 page fault（用户空间本来就没映射），但此时不应该三重故障——检查 IDT 中是否注册了 page fault handler。

**隔离测试失败——AS#2 能看到 AS#1 的映射**: 这说明两个 AddressSpace 的 PML4 在用户空间部分"共享"了某些条目——这不应该发生。最可能的原因是构造函数清零 PML4 的时候出了问题——要么没清零（忘了循环），要么清零后又意外覆盖了（后续代码的 bug）。另一个可能性是 VMM 的 map 操作使用了错误的 PML4 根——检查 map 委托调用时传入的是不是 &pml4_phys_（当前对象的成员变量地址），而不是某个全局变量或别的对象的地址。

**Host 测试中模拟内存越界**: 如果模拟 VMM 的页表遍历访问了 sim_memory 之外的内存（valgrind 或 ASan 报错），说明 sim_virt_of 函数返回了空指针或者越界指针。检查模拟 PMM 分配的物理地址是否在 MOCK_POOL_BASE 到 MOCK_POOL_BASE + SIM_PAGES * PAGE_SIZE 的范围内。

**测试顺序依赖**: 如果某个测试单独运行通过但在全套测试中失败，说明测试之间有状态泄漏——前一个测试修改了全局状态（比如 kernel_pml4_），后一个测试依赖了这个被修改的状态。确保每个测试用例都使用独立的 TestFixture 实例来重置状态。

**QEMU 测试中 CR3 恢复遗漏**: 如果某个使用 activate() 的测试在退出后导致三重故障，检查是否在 AddressSpace 析构之前恢复了 CR3。这个坑在测试代码中特别容易踩到——因为局部变量的析构发生在函数返回时，但 CR3 恢复必须发生在最后一个 AddressSpace 析构之前。QEMU 测试中 Test 8 和 Test 9 都需要手动恢复 CR3——如果在复制测试代码时漏掉了这行，就会踩坑。建议在所有使用 activate() 的测试中，把 CR3 恢复放在函数的最后一行，在所有局部变量析构之前。

**Host 测试中 sim_memory 越界**: 如果模拟 VMM 的页表遍历访问了 sim_memory 之外的内存（valgrind 或 ASan 报错），说明 sim_virt_of 函数返回了空指针或者越界指针。检查模拟 PMM 分配的物理地址是否在 MOCK_POOL_BASE 到 MOCK_POOL_BASE + SIM_PAGES * PAGE_SIZE 的范围内。如果 MockPMM 和 sim_memory 的大小不匹配（比如 MockPMM 分配了超出 sim_memory 范围的地址），就会出现越界访问。

## 本章小结

到这里 tag 018 的全部内容就完成了。activate() 通过写 CR3 切换地址空间，隐式刷新 TLB；map/unmap/translate 委托给 VMM 执行，传入自己的 PML4 根来确保操作发生在正确的地址空间中；
集成测试和 host 单元测试共同证明了跨地址空间隔离确实有效——在 AS#1 中建立的映射对 AS#2 完全不可见。

AddressSpace 是我们迈向多进程的第一块基石。
未来每个进程都会持有一个 AddressSpace 实例，进程切换时调度器调用新进程的 activate() 来切换地址空间，进程销毁时 AddressSpace 的析构函数自动释放所有用户空间页表页——完全符合 RAII 的理念。
下一站是 tag 019 的进程上下文管理——有了地址空间隔离，接下来就是进程的创建、调度和上下文切换了。那才是真正让操作系统"活起来"的部分。

### 关于 tag 018 的变更规模

整个 tag 018 涉及 10 个文件的变更，新增约 1470 行代码。核心实现是 address_space.hpp（144 行）和 address_space.cpp（198 行），另外有 308 行 QEMU 集成测试和 771 行 Host 单元测试。测试代码的总量（1079 行）远超实现代码（342 行），这反映了内核开发中的一个重要原则：和硬件交互的代码需要更严格的测试覆盖，因为 bug 的表现形式往往更隐蔽、更难定位。

tag 018 还修改了 kernel/main.cpp（插入 init_kernel 调用）、kernel/CMakeLists.txt（添加源文件）和 test/CMakeLists.txt（添加测试目标）。这些都是必要的"脚手架"修改，确保新代码被正确集成到构建系统中。
