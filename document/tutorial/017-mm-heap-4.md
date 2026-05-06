# C++ 集成与质量保证

> 标签：operator new/delete, std::align_val_t, host 测试, QEMU 测试, 压力测试
> 前置：[017-3 释放、合并与自动扩展](017-mm-heap-3.md)

## 前言

前两章我们把堆分配器的核心算法写完了——alloc、free、coalesce、expand 四个函数构成了一个功能完备的分配器。但如果你在内核代码里写 `int* p = new int[10]`，链接器会报 `undefined reference to operator new`——因为内核不链接标准库，C++ 运行时的 new/delete 默认是没有实现的。

tag 016 及之前，Cinux 的 `crt_stub.cpp` 里放的是"halt on use"的桩函数——调用 new 或 delete 就直接 `cli; hlt` 死循环。说实话，这在没有堆分配器的时候是合理的——谁调谁死，至少不会静默地踩内存。但现在我们有了 `g_heap`，这些桩函数终于可以变成真正的实现了。

本章覆盖三个层面的集成代码：C++ new/delete 的重定向（怎么把 `new` 和 `delete` 接到 `g_heap` 上）、堆在启动序列中的位置（为什么必须放在 VMM 之后、Framebuffer 之前）、以及两层测试策略（host + QEMU）的设计。最后我们做一个跨项目的对比反思——看看 Cinux 的堆分配器在 FreeRTOS heap_4 和 Linux SLOB 面前到底处在什么位置。

## 环境说明

本章涉及的文件比较分散：`kernel/arch/x86_64/crt_stub.cpp`（new/delete 重定向，+84/-48 行变更）、`kernel/main.cpp`（启动序列集成，+36/-18 行变更）、`test/unit/test_heap.cpp`（host 端测试，866 行）、`kernel/test/test_heap.cpp`（QEMU 端测试，301 行）。host 测试在 Linux 用户态编译运行（`cmake --build build --target test_heap`），QEMU 测试在 x86-64 QEMU 里运行，底层依赖真实的 PMM + VMM。

本章完成后，tag 017 的全部内容就讲完了——从数据结构设计到核心算法实现，再到 C++ 集成和测试验证。Cinux 的内存管理栈三层架构终于完整了。

## operator new/delete：让 C++ 动态内存走堆分配器

### 从 halt 桩到真正的实现

先看一下 tag 016 的 old 代码是什么样的，这样你能感受到"终于能用了"的解脱感：

```cpp
// tag 016: 调用就直接死机
void* operator new(unsigned long size) {
    (void)size;
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

那时候没有堆分配器，任何对 `new` 的调用都是编程错误——内核不允许动态分配内存。所以桩函数直接禁中断并停机，让开发者立刻意识到问题。现在堆分配器就位了，我们把死循环替换成一行委托：

```cpp
void* operator new(unsigned long size) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size));
}

void* operator new[](unsigned long size) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size));
}
```

单对象 new 和数组 new 的实现完全一样——都委托给 `g_heap.alloc()`。C++ 标准保证 `operator new[]` 被调用时传入的 size 已经包含了数组的所有元素大小（可能还有编译器插入的数组元数据），所以分配器不需要区分单对象和数组。`unsigned long` 到 `size_t` 的 static_cast 在 x86-64 上是安全的——两者都是 64 位。

新增的 include 有三个：`<stddef.h>` 提供 `size_t`，`<new>` 提供 `std::align_val_t`，`kernel/mm/heap.hpp` 提供 `g_heap` 接口。注意这些 operator 必须放在 `extern "C"` 块外面——它们需要 C++ 的 name mangling 才能被链接器正确解析。

### 对齐 new：std::align_val_t 的支持

C++17 引入了 `std::align_val_t` 来支持 over-aligned 类型——当类型的对齐要求超过了 `alignof(max_align_t)`（通常是 16）时，编译器会自动调用带 `align_val_t` 的重载。比如 `struct alignas(4096) Page { ... }`，new 这样一个对象时编译器会调用带对齐参数的版本：

```cpp
void* operator new(unsigned long size, std::align_val_t align) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size),
                                   static_cast<size_t>(align));
}

void* operator new[](unsigned long size, std::align_val_t align) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size),
                                   static_cast<size_t>(align));
}
```

我们把 `std::align_val_t` 转成 `size_t` 传给 `alloc` 的第二个参数。上一篇讲过的对齐计算逻辑——front padding、aligned payload、hdr_addr 偏移——就是在这些场景下被触发的。FreeRTOS 的 heap_4 不支持自定义对齐，这是 Cinux 的一个设计优势——内核里经常需要页对齐的分配（比如分配一个新页表），有了 `align_val_t` 就可以直接用 `new(std::align_val_t{4096}) PageTable` 而不必手写对齐逻辑。

### 六个 delete 重载：覆盖所有标准签名

delete 系列比重定向 new 还要"啰嗦"一些——C++ 标准定义了多个 delete 签名，我们全部实现了（总共 6 个 delete 重载 + 4 个 new 重载 = 10 个）：

为什么要覆盖全部 6 个 delete？GCC 从某个版本开始会在 `-O2` 下生成 sized delete 的调用（如果存在对应的重载）。如果不提供 sized delete 的实现，链接器在启用 sized delete 时会报 `undefined reference`。aligned delete 同理——如果定义了 aligned new 但不定义 aligned delete，在某些情况下编译器会生成未定义的 delete 调用。六个重载全部实现保证了在任何编译选项下都不会出现链接错误。

```cpp
void operator delete(void* ptr) noexcept {
    cinux::mm::g_heap.free(ptr);
}

void operator delete(void* ptr, unsigned long) noexcept {
    cinux::mm::g_heap.free(ptr);   // sized delete, size 被忽略
}

void operator delete[](void* ptr) noexcept {
    cinux::mm::g_heap.free(ptr);
}

void operator delete[](void* ptr, unsigned long) noexcept {
    cinux::mm::g_heap.free(ptr);   // sized array delete
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    cinux::mm::g_heap.free(ptr);   // aligned delete
}

void operator delete(void* ptr, unsigned long, std::align_val_t) noexcept {
    cinux::mm::g_heap.free(ptr);   // sized aligned delete
}
```

六个重载覆盖了所有组合：普通 delete、sized delete（C++14 引入，第二个参数是块大小但被我们忽略）、数组 delete、sized 数组 delete、对齐 delete、sized 对齐 delete。它们全部委托给 `g_heap.free(ptr)`。size 和 align 参数被直接忽略，因为 `Heap::free` 只需要 payload 指针就能通过 `header_from_ptr` 定位 header 并获取所有信息——size 在 header 里存着，对齐信息在释放时不需要。

## 启动序列：Heap 放在哪里

### init 的位置不能乱

```cpp
// Step 8: Initialise Virtual Memory Manager
cinux::mm::g_vmm.init();

// Step 9: Initialise kernel heap (64 KB initial region after kernel image)
constexpr uint64_t HEAP_VIRT_BASE = 0xFFFF800000000000ULL;
constexpr uint64_t HEAP_INITIAL_SIZE = 64 * 1024;
cinux::mm::g_heap.init(HEAP_VIRT_BASE, HEAP_INITIAL_SIZE);

// Step 10: Initialise framebuffer from BootInfo
```

Heap 的初始化被精确地放在 VMM 之后、Framebuffer 之前。这个顺序不是随意的——`Heap::init` 内部调用 `g_pmm.alloc_page()` 和 `g_vmm.map()`，所以 PMM（Step 7）和 VMM（Step 8）必须先就绪。同时，堆被尽早初始化是因为后续的很多子系统都可能需要动态分配——Console 的字符串缓冲区、Keyboard 的事件队列、文件系统的缓存。如果哪个子系统的初始化代码里用了 `new`，而堆还没初始化，就会触发那个老的 halt 桩函数——内核直接死机，连个错误信息都看不到。

C++ 全局构造函数（通过 `_init_global_ctors` 调用）是一个隐藏的陷阱。在 Cinux 的启动序列中，全局构造函数在 Step 3 就被调用了——远早于堆的初始化。这意味着全局对象的构造函数里不能使用 `new`。随着内核功能增加，这个约束需要一直记住。

`HEAP_VIRT_BASE = 0xFFFF800000000000` 是一个精心选择的地址。它是 x86-64 高半区的起始地址，远高于内核代码段的映射区域（`0xFFFFFFFF80000000` 附近），不会和内核代码冲突。从 PML4 的角度看，PML4 索引是 256，从 PML4[256] 到 PML4[511] 有约 128 TB 的虚拟地址空间可供堆增长。64 KB 的初始大小对于启动阶段绰绰有余，expand 会在需要时自动增长。

## 两层测试：为什么 host 和 QEMU 都要测

### 测试策略的动机

内核代码的测试有一个根本性的难题：你不能在主机上直接跑内核代码，因为它依赖特权指令、物理内存管理、页表映射等硬件相关的设施。常见的应对策略有两种——要么在模拟器里跑（QEMU），要么在主机上用 mock 替代硬件依赖。Cinux 两种都做了，而且它们测的是不同的东西。

host 端测试（`test/unit/test_heap.cpp`，866 行）测试的是"算法逻辑"——first-fit 搜索对不对、分裂条件对不对、coalesce 的合并顺序对不对、200 轮压力测试后有没有泄漏。它的优势是速度极快（毫秒级完成），可以用 GDB 单步调试，可以用 valgrind 检查内存错误。如果算法有 bug，host 测试能最快地暴露出来。

QEMU 端测试（`kernel/test/test_heap.cpp`，301 行）测试的是"端到端的正确性"——从 alloc 到页表映射到物理内存访问的整条链路。它的基本测试分配 64 字节，写入 0-63 的递增模式，然后回读验证。这个测试看似简单，但它实际验证了整条路径：alloc 调用 PMM 分配物理页，VMM 建立映射，CPU 通过页表访问物理内存，写入成功，回读一致。如果 VMM 的映射有误或者页表标志位不对，这个测试就会触发 page fault。

### host 端：镜像实现的技术

host 测试的核心思路是"镜像实现"——在用户态重新实现一遍 Heap 的算法（TestHeap 类），用 `calloc` 分配缓冲区代替 VMM 映射。这样做的原因是直接链接 `heap.cpp` 会引入所有内核头文件的依赖（`kprintf`、`pmm.hpp`、`vmm.hpp`），这些头文件又依赖 `paging_config.hpp`、`serial.hpp` 等，编译依赖链会变得非常复杂。镜像实现虽然需要维护两份代码，但它完全自包含——不需要任何内核头文件。

镜像实现的最大风险是两边的算法有微妙差异——host 测试全部通过但 kernel 中实际有 bug。解决这个问题的方法是在 QEMU 端也运行足够的测试用例，特别是 alloc、free、coalesce 的核心路径。如果 QEMU 端测试通过但 host 端失败（或者反过来），说明两边的算法实现不一致，需要逐行对比。

TestHeap 类除了用 `calloc` 代替 PMM/VMM 之外，结构和 `Heap` 完全一样。它额外提供了几个诊断方法——`used()`、`free_total()`、`free_block_count()`、`validate_free_list()`、`has_valid_magic()`、`is_block_free()`——这些在正式的 Heap 类中没有暴露，但对测试至关重要。

`validate_free_list()` 特别值得一提——它检查链表的完整性：没有环（记录遍历步数，超过 10000 就判定有环）、所有块 magic 正确、所有块标记为 free、所有块地址在缓冲区范围内。这个方法在每次 coalesce 测试后被调用，确保合并操作没有搞坏链表。

### 压力测试与泄漏检测

host 端的压力测试分两个阶段。第一阶段做 200 轮循环，每轮分配 3 个不同大小的块（32、64、128 字节），交错释放其中 2 个，再分配 1 个新块（48 字节），最后全部释放。这模拟了真实内核中"反复分配释放不同大小对象"的负载模式——每次循环结束时 `used_` 不一定归零（因为还有未释放的块），但全部 200 轮结束后必须归零。第二阶段一次性分配 100 个大小递增的块（从 16 字节到 412 字节），然后全部释放，验证大批量分配后 `used_` 能精确归零。

QEMU 端的压力测试做 50 轮循环，每轮分配大小在 16-272 字节之间变化的块，交错释放和重新分配。关键的验证点在 Phase 4——每个块的首字节和尾字节都写入了独特的标记值（`buf[0] = i`，`buf[sizes[i]-1] = i ^ 0xFF`），重新分配后检查这些标记是否还在。这能捕获"alloc 返回了重叠区域"这种灾难性的 bug——两个不同的指针指向了重叠的内存区域。

### 记账不变量与碎片率

host 测试中有一个特别巧妙的用例验证了一个微妙的不变量：`free_total + free_block_count * HEADER_SIZE <= total`。等号在"没有内部碎片"时成立——所有空间要么在空闲块的 payload 中，要么在空闲块的 header 中。不等号在"有 front padding 浪费"时成立——部分空间因为对齐被丢弃，既不在 `used_` 中也不在空闲链表中。

测试还要求碎片率不超过 10%：`free_total > total * 9 / 10`。这个阈值是一个经验值——如果碎片超过 10%，说明分配策略有问题。在实际测试中，即使在 200 轮随机 alloc/free 后，碎片率也远低于这个阈值，说明 first-fit + coalescing 对于内核规模的分配模式是足够好的。

在 host 测试中还有一个"内存耗尽"测试——给 TestHeap 一个只有 1 页（4096 字节）的缓冲区，然后不断分配直到返回 nullptr。这个测试验证了分配器在极端条件下的行为——不会 crash，而是优雅地返回空指针。在 kernel 端，expand 会自动扩展堆，所以"耗尽"的场景不太可能出现（除非 PMM 本身耗尽了物理内存）。

## 对比反思：Cinux 在设计光谱上的位置

写到这里，我们不妨回头看看 Cinux 的堆分配器在整个 OS 生态中处于什么位置。这有助于理解 Cinux 的设计选择落在"简单教学"和"生产级"之间的哪个点上。

### 对比 FreeRTOS heap_4

Cinux 和 FreeRTOS heap_4 是最接近的同类。两者都用 first-fit + coalescing，都有块 header 记录元数据，都支持释放和复用。关键区别有三点：Cinux 支持 VMM 动态扩展（heap_4 用固定大小的静态数组 `configTOTAL_HEAP_SIZE`，运行时不能增长），Cinux 有 magic number 做运行时校验（heap_4 用链表尾部的 tail marker），Cinux 支持任意对齐（heap_4 不支持）。FreeRTOS 多了线程安全保护（通过 critical sections，即 `taskENTER_CRITICAL` / `taskEXIT_CRITICAL`）和堆统计 API（`xPortGetFreeHeapSize()`、`vPortGetHeapStats()`）——Cinux 目前是单线程的，但 `dump_stats()` 提供了基本的统计功能。

### 对比 Linux SLOB

Cinux 是简化版。SLOB 也用 first-fit，但它维护三个独立的链表按大小分级——小请求（< 256 字节）只搜索小链表，中等请求（< 1024 字节）搜索中链表，大请求搜索大链表——这样可以减少搜索开销。SLOB 还支持 NUMA 感知分配、per-CPU 缓存、以及 slab 层的对象复用——这些都是生产级内核需要的优化。Cinux 的单链表设计在内核堆的规模下（通常几十到几百个块）完全够用，但如果将来内核变复杂了，分级链表或 slab 是自然的升级方向。Linux 默认使用的是 SLUB 分配器（比 SLOB 更复杂但性能更好），SLOB 只在嵌入式和小内存场景下被选用。

**对比 xv6**，Cinux 走的路完全不同。xv6 的 `kalloc.c` 只做页级分配——用嵌入式链表管理，LIFO 策略，没有变长分配、没有分裂、没有合并。xv6 用填充 junk 值做基本的调试支持——这和 Cinux 的 magic number 校验有异曲同工之妙，但粒度更粗。MIT 的设计哲学是"能少就少"，但代价是内核里无法高效使用小块内存。Cinux 选择了另一条路——在 PMM + VMM 之上构建完整的 sub-page 堆分配器，用更多的代码换来了更灵活的内存使用。

### 总结对比

| 特性 | Cinux | FreeRTOS heap_4 | Linux SLOB | xv6 kalloc |
|------|-------|-----------------|------------|------------|
| 分配策略 | first-fit | first-fit | first-fit (3 lists) | 页级 LIFO |
| 合并 | coalescing | coalescing | 前向合并 | 无 |
| 动态扩展 | VMM expand | 固定大小 | buddy 扩展 | 无 |
| 对齐支持 | 任意对齐 | 无 | 有 | 无 |
| 调试支持 | magic number | tail marker | minimal | junk fill |
| 线程安全 | 无 | critical section | 自旋锁 | 无 |

## 收尾

到这里，tag 017 的全部内容就讲完了。我们构建了一个完整的内核堆分配器——BlockHeader 数据结构（32 字节固定 header + magic 校验）、first-fit 搜索（线性遍历空闲链表）、块分裂（MIN_SPLIT = 48 的阈值控制）、相邻合并（"循环到收敛"处理传递性合并）、自动扩展（最少 16 KB 的摊还策略）、C++ new/delete 重定向（10 个重载版本全覆盖）、两层测试（host 端 866 行算法测试 + QEMU 端 301 行集成测试）。从 PMM 的整页分配到 Heap 的任意大小分配，Cinux 的内存管理栈终于完整了。

回顾一下四篇文章覆盖的内容。第一篇解决了"为什么需要堆"和"怎么设计"的问题——三层内存管理栈的定位、经典方案的对比、BlockHeader 的设计决策。第二篇实现了分配的核心算法——init、alloc 的 first-fit 搜索、对齐计算、块分裂。第三篇实现了回收和扩展——free 的安全检查、coalesce 的传递性合并、expand 的摊还策略。第四篇（本章）完成了集成和验证——new/delete 重定向、双层测试、跨项目对比。

堆分配器是内核中最容易出现 bug 的组件之一——野指针、double-free、buffer overflow、内存泄漏，每一个都能让内核静默崩溃。我们的 magic number 校验和两层测试策略提供了基本的安全网，但如果你将来要给 Cinux 加更多功能（文件系统、网络栈、用户进程），堆的负载会指数级增长，到时候可能需要考虑更重的调试方案——比如在 `_pad[12]` 里放 canary 值检测 buffer overflow，或者实现 `#ifdef HEAP_DEBUG` 条件编译的详细日志。

下一章（tag 018）会进入地址空间管理——让每个用户进程拥有自己独立的页表，实现内核空间和用户空间的隔离。堆分配器在这个过程中会被大量使用——进程控制块、页表结构、用户栈的分配都需要动态内存。到时候你会发现，一个好的堆分配器是后续所有复杂功能的地基。

## 参考资料

- C++17 Standard Section 21.6: Dynamic memory management — operator new/delete 的完整规范，包括 aligned new/delete 和 sized delete 的语义。
- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) — 内核堆分配器与 C++ new/delete 集成的概述。
- FreeRTOS `heap_4.c` 源码: 对比 Cinux 的两层测试策略和 FreeRTOS 的任务创建/删除间接测试方法。
- GCC Manual: [Common Variable Attributes](https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html) — `std::align_val_t` 和 over-aligned 分配的 GCC 实现。
- Linux 内核 `mm/slab.h` / `mm/slub.c`: Linux 的 kmalloc/kfree 接口设计——对比 Cinux 的 new/delete 和 Linux 的 C 风格分配接口。
