# 017-4 通读：C++ new/delete 接管、启动集成与测试策略

## 概览

本文是 tag `017_mm_heap` 四篇通读教程的最后一篇，覆盖三个层面的集成代码。首先是 `kernel/arch/x86_64/crt_stub.cpp` 中的 operator new/delete 重定向——它让 C++ 的 `new`/`delete` 语法直接走我们的 Heap 分配器。其次是 `kernel/main.cpp` 中 Heap 初始化在启动序列中的位置。最后是两层测试代码：host 端的 `test/unit/test_heap.cpp`（866 行，纯算法验证）和 QEMU 端的 `kernel/test/test_heap.cpp`（301 行，真实 PMM/VMM 环境验证）。这三个部分分别解决了"怎么用""什么时候用""怎么验证"这三个问题，让堆分配器从一个独立的模块变成内核中真正可用的基础设施。

## 架构图

```
    C++ 代码               crt_stub.cpp              Heap 分配器
    ─────────              ────────────              ────────────
    new T          →  operator new(size)    →  g_heap.alloc(size)
    new T[n]       →  operator new[](size)  →  g_heap.alloc(size)
    new(align) T   →  operator new(size,    →  g_heap.alloc(size, align)
                       std::align_val_t)
    delete p       →  operator delete(p)    →  g_heap.free(p)
    delete[] p     →  operator delete[](p)  →  g_heap.free(p)
    delete(align)p →  operator delete(p,    →  g_heap.free(p)
                       std::align_val_t)

    启动序列 (kernel/main.cpp):
    ...
    Step 7:  PMM init
    Step 8:  VMM init
    Step 9:  Heap init ←── 本文重点
    Step 10: Framebuffer
    ...

    测试策略:
    ┌─────────────────────────┐    ┌─────────────────────────┐
    │ Host 端 (test/unit/)     │    │ QEMU 端 (kernel/test/)   │
    │                         │    │                         │
    │ TestHeap 类重新实现      │    │ 直接用 g_heap           │
    │ 算法 (calloc 做缓冲)    │    │ 真实 PMM + VMM          │
    │                         │    │                         │
    │ 25+ 测试用例:           │    │ 10 测试用例:            │
    │ · 基本分配/释放         │    │ · 基本分配/释放         │
    │ · 对齐 (16/64/4096)    │    │ · 对齐 (16/4096)        │
    │ · 块分裂               │    │ · coalesce              │
    │ · coalesce (3块合并)   │    │ · 压力 (50 cycles)      │
    │ · double-free 检测     │    │ · 数据完整性            │
    │ · 压力 (200 cycles)    │    │ · dump_stats            │
    │ · 数据完整性           │    │                         │
    │ · 内存耗尽             │    │                         │
    │ · 无重叠               │    │                         │
    │ · 记账不变量           │    │                         │
    └─────────────────────────┘    └─────────────────────────┘
```

## 代码精讲

### crt_stub.cpp——operator new/delete 的完整重定向

在 tag 016 及之前，`crt_stub.cpp` 中的 operator new/delete 都是"halt on use"的桩函数——调用就直接 `cli; hlt` 死循环，因为那时候还没有堆分配器。现在我们有了 `g_heap`，这些桩函数终于可以变成真正的实现了。

```cpp
#include <stdint.h>
#include <stddef.h>
#include <new>

#include "kernel/mm/heap.hpp"
```

新增的三个 include 很关键。`<stddef.h>` 提供 `size_t`，`<new>` 提供 `std::align_val_t`（C++17 的对齐 new 类型），`kernel/mm/heap.hpp` 提供我们的 `g_heap` 接口。

```cpp
void* operator new(unsigned long size) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size));
}

void* operator new[](unsigned long size) {
    return cinux::mm::g_heap.alloc(static_cast<size_t>(size));
}
```

单对象 new 和数组 new 的实现完全一样——都委托给 `g_heap.alloc()`。C++ 标准保证 `operator new[]` 被调用时传入的 size 已经包含了数组的所有元素大小（可能还有编译器插入的数组元数据），所以分配器不需要区分"单对象"和"数组"。`unsigned long` 到 `size_t` 的 static_cast 在 x86-64 上是安全的（两者都是 64 位）。

接下来是对齐版本的 new：

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

C++17 引入了 `std::align_val_t` 来支持 over-aligned 类型——当类型的对齐要求超过了 `alignof(max_align_t)`（通常是 16）时，编译器会自动调用带 `align_val_t` 的重载。比如 `struct alignas(4096) Page { ... }`，new 这样一个对象时编译器会调用 `operator new(sizeof(Page), std::align_val_t{4096})`。我们把它转发给 `g_heap.alloc(size, align)`，利用上一篇讲到的对齐计算逻辑来满足请求。

然后是 delete 系列：

```cpp
void operator delete(void* ptr) noexcept {
    cinux::mm::g_heap.free(ptr);
}

void operator delete(void* ptr, unsigned long) noexcept {
    cinux::mm::g_heap.free(ptr);
}

void operator delete[](void* ptr) noexcept {
    cinux::mm::g_heap.free(ptr);
}

void operator delete[](void* ptr, unsigned long) noexcept {
    cinux::mm::g_heap.free(ptr);
}

void operator delete(void* ptr, std::align_val_t) noexcept {
    cinux::mm::g_heap.free(ptr);
}

void operator delete(void* ptr, unsigned long, std::align_val_t) noexcept {
    cinux::mm::g_heap.free(ptr);
}
```

六个 delete 重载覆盖了所有 C++ 标准要求的签名组合：普通 delete、sized delete（C++14，第二个参数是块大小但被忽略）、数组 delete、对齐 delete。它们全部委托给 `g_heap.free(ptr)`。size 和 align 参数被直接忽略，因为我们的 `Heap::free` 只需要 payload 指针就能通过 `header_from_ptr` 定位 header 并获取所有需要的信息。如果你仔细看，会发现 `free(nullptr)` 是安全的（Heap::free 的第一步就是空指针检查），所以 delete 也不需要额外判空。

### kernel/main.cpp——Heap 在启动序列中的位置

```cpp
// Step 8: Initialise Virtual Memory Manager
cinux::mm::g_vmm.init();

// Step 9: Initialise kernel heap (64 KB initial region after kernel image)
constexpr uint64_t HEAP_VIRT_BASE = 0xFFFF800000000000ULL;
constexpr uint64_t HEAP_INITIAL_SIZE = 64 * 1024;
cinux::mm::g_heap.init(HEAP_VIRT_BASE, HEAP_INITIAL_SIZE);

// Step 10: Initialise framebuffer from BootInfo
```

Heap 的初始化被精确地放在 VMM 之后、Framebuffer 之前。这个顺序不是随意的——Heap::init 需要调用 `g_pmm.alloc_page()` 和 `g_vmm.map()`，所以 PMM 和 VMM 都必须先初始化完成。同时，堆被尽早初始化是因为后续的很多子系统（比如 Console 的字符串缓冲区、Keyboard 的事件队列等）都可能需要动态分配内存。C++ 的全局构造函数（通过 `_init_global_ctors` 调用）如果在堆初始化之前运行且需要 new，就会触发旧的 halt 桩函数——所以堆必须在所有可能使用 new/delete 的初始化步骤之前就绪。

`HEAP_VIRT_BASE = 0xFFFF800000000000` 是一个精心选择的地址。它是 x86-64 高半区（higher-half canonical）的起始地址——bit 63 到 bit 48 全是 1，bit 47 为 1 标志着内核空间。这个地址远高于内核代码段的映射区域（通常在 `0xFFFFFFFF80000000` 附近），所以不会和内核代码冲突。64 KB 的初始大小对于启动阶段的分配绰绰有余，如果后续不够了，expand 会自动增长。

### Host 端测试——test/unit/test_heap.cpp 关键用例

Host 端测试的核心思路是"镜像实现"——在用户态重新实现一遍 Heap 的算法，用 `calloc` 分配缓冲区代替 VMM 映射。这样做的好处是可以在主机上用 GDB 调试、用 valgrind 检查内存泄漏，速度也比 QEMU 快几个数量级。我们来挑几个最有代表性的测试用例讲解。

**TestHeap 类的 init 方法**用 `calloc` 分配一块干净的用户态内存，然后在起始处创建一个覆盖整个区域的空闲块。除了用 `calloc` 代替 PMM/VMM 之外，结构和 `Heap::init` 完全一样。`alloc` 和 `free_mem` 方法也是算法的精确镜像。TestHeap 还额外提供了几个诊断方法——`used()`、`free_total()`、`free_block_count()`、`validate_free_list()`、`has_valid_magic()`——这些在正式的 Heap 类中没有暴露，但对测试至关重要。

**对齐分配测试**验证了 alloc 在各种对齐值下的正确性。最关键的是 4096 字节对齐的测试——它先做一次小分配把空闲块推到非 4096 对齐的位置，然后请求 4096 对齐的分配。这迫使 alloc 使用 front padding 机制。测试不仅检查返回地址的对齐，还通过 `has_valid_magic` 验证 header 确实被放在了 `aligned_payload - 32` 的位置，free 之后 used_ 精确归零。

**三块合并测试**用三种不同的释放顺序（左到右、右到左、先中间再两边）验证 coalesce 的传递性。这三种顺序分别测试了不同的合并路径：左到右时先合并 A+B，然后 (A+B)+C；右到左时先合并 B+C，然后 A+(B+C)；先中间再两边时先释放 B 形成"孤岛"，然后释放 A 和 B 合并，最后合并 C。每次释放后 `validate_free_list()` 检查链表完整性——没有环、所有块 magic 正确、所有块标记为 free、所有块地址在缓冲区范围内。

**压力测试**分两个阶段。第一阶段做 200 轮循环，每轮分配 3 个不同大小的块，交错释放其中 2 个，再分配 1 个新块，最后全部释放。这模拟了真实内核中"反复分配释放不同大小对象"的负载模式。第二阶段一次性分配 100 个大小递增的块，然后全部释放，验证大批量分配后 used_ 能精确归零。

**记账不变量测试**验证了一个微妙的不变量：`free_total + free_block_count * HEADER_SIZE <= total`。等号在"没有内部碎片"时成立——所有空间要么在空闲块的 payload 中，要么在空闲块的 header 中。不等号在"有 front padding 浪费"时成立——部分空间因为对齐被丢弃，既不在 used_ 中也不在空闲链表中。测试还要求碎片率不超过 10%：`free_total > total * 9 / 10`。这个阈值是一个经验值——如果碎片超过 10%，说明分配策略有问题。

### QEMU 端测试——kernel/test/test_heap.cpp

QEMU 测试在真实的内核环境中运行——使用真正的 PMM 物理页分配、真正的 VMM 页表映射。相比 host 测试，它的覆盖范围更小（10 个用例 vs 25+），但它验证的是"端到端的正确性"——从 alloc 到页表映射到物理内存访问的整条链路。

**基本分配与数据完整性测试**分配一个 64 字节的块，写入 0-63 的递增模式，然后回读验证。这个测试看似简单，但它实际验证了整条路径：alloc 调用 PMM 分配物理页 → VMM 建立映射 → CPU 通过页表访问物理内存 → 写入成功 → 回读一致。如果 VMM 的映射有误（比如映射到了错误的物理页），或者页表标志位不对（比如忘了设 writable），这个测试就会触发 page fault。

**coalesce 测试**分配三个 64 字节的块，按"中间-左-右"的顺序释放，然后分配一个 256 字节的块。如果 coalesce 不工作，三个 64 字节的小块无法合并，256 字节的分配就会失败（因为链表里没有够大的单块）。这个测试在 QEMU 环境中特别有价值，因为它验证了 coalesce 在真实页表映射下的正确性——合并后的块跨越的虚拟地址范围必须全部有有效的映射。

**压力测试**做 50 轮循环，每轮分配大小在 16-272 字节之间变化的块，交错释放和重新分配。Phase 4 验证了"未被释放的块的 marker 值没有被踩"——每个块的首字节和尾字节都写入了独特的标记值，重新分配后检查这些标记是否还在。这能捕获"alloc 返回了重叠区域"这种灾难性的 bug。

## 设计决策

### Decision: Host 测试镜像实现算法，而非直接链接 Heap 源码

**问题**: Host 端测试应该怎么测内核堆分配器？

**本项目的做法**: 在 `test/unit/test_heap.cpp` 中重新实现一遍 Heap 的算法（TestHeap 类），用 `calloc` 做底层缓冲区，在用户态运行测试。

**备选方案**: 直接 include `heap.cpp` 并链接到 host 测试可执行文件，用 mock 的 PMM/VMM 函数替代真实实现。

**为什么不选备选方案**: 直接链接会引入所有内核头文件的依赖（`kprintf`、`pmm.hpp`、`vmm.hpp`），这些头文件又依赖 `paging_config.hpp`、`serial.hpp` 等等，编译依赖链会变得非常复杂。镜像实现虽然需要维护两份代码，但它完全自包含——不需要任何内核头文件，编译和运行速度快，可以用 GDB 单步调试。测试的是"算法逻辑"而不是"系统集成"，和 QEMU 端的测试形成互补。

**如果要扩展/改进**: 可以把 Heap 的核心算法抽成一个模板类（参数化底层内存操作），host 测试和内核代码都实例化这个模板。这样既消除了代码重复，又保持了测试的独立性。

### Decision: 覆盖全部 6 个 operator delete 签名

**问题**: C++ 标准定义了多个 operator delete 重载。我们需要实现哪些？

**本项目的做法**: 实现全部 6 个：普通 delete、sized delete、数组 delete、sized 数组 delete、aligned delete、sized aligned delete。全部委托给 `g_heap.free(ptr)`。

**备选方案**: 只实现最基本的 `operator delete(void*)` 和 `operator delete[](void*)`，其余的让编译器回退到基本版本。

**为什么不选备选方案**: GCC 从某个版本开始会在 `-O2` 下生成 sized delete 的调用（如果存在对应的重载），这能帮助编译器优化。如果不提供 sized delete 的实现，链接器在启用 sized delete 时会报错。aligned delete 同理——如果定义了 aligned new 但不定义 aligned delete，在某些情况下编译器会生成未定义的 delete 调用。六个重载全部实现虽然显得冗余，但保证了在任何编译选项下都不会出现链接错误。

**如果要扩展/改进**: 在 `operator delete` 中加入 `HEAP_DEBUG` 条件编译的日志输出，记录每次 delete 的指针地址和块大小。这对调试内存泄漏非常有帮助。

## 扩展方向

1. **实现 kmalloc/kfree C 风格接口** (⭐): 在 `heap.hpp` 中导出 `extern "C" void* kmalloc(size_t)` 和 `extern "C" void kfree(void*)` 函数。这样汇编代码或 C 风格的驱动代码也能使用堆分配器，而不必依赖 C++ 的 new/delete。

2. **Host 测试中加入 ASan 支持** (⭐⭐): 用 `gcc -fsanitize=address` 编译 host 测试，让 ASan 自动检测 buffer overflow、use-after-free 等问题。对比 TestHeap 自己的 magic 校验，ASan 能捕获更多种类的错误。

3. **QEMU 端测试加入 double-free 检测用例** (⭐⭐): 当前 QEMU 测试没有 double-free 测试（因为 double-free 会导致 kprintf 输出但不 panic，测试框架不好捕获）。可以实现一个"检查串口输出包含特定字符串"的辅助函数，验证 double-free 确实被检测到。

4. **实现 thread-safety（自旋锁保护）** (⭐⭐): 当前的 Heap 不是线程安全的——如果中断处理程序和主循环同时调用 alloc/free，链表会被踩。在 Heap 类中加入一个自旋锁（`cli/sti` 或者 `xchg` based spinlock），在 alloc 和 free 的入口加锁、出口解锁。

5. **内存泄漏检测器** (⭐⭐): 在 `init` 中注册一个"关机钩子"，在内核退出前遍历所有已分配的块（不仅仅是空闲链表，而是扫描整个堆区域），打印所有未释放的块的地址和大小。这对于长运行的内核来说是非常有价值的调试工具。

## 参考资料

- C++17 Standard Section 21.6: Dynamic memory management — operator new/delete 的完整规范，包括 aligned new/delete 和 sized delete 的语义。
- OSDev Wiki: [Memory Allocation](https://wiki.osdev.org/Memory_Allocation) — 内核堆分配器与 C++ new/delete 集成的概述。
- FreeRTOS `heap_4.c` 源码: 对比 Cinux 的 TestHeap 镜像实现和 FreeRTOS 的测试方法——FreeRTOS 用任务创建/删除来间接测试分配器。
- GCC Manual: [Aligned Memory Allocation](https://gcc.gnu.org/onlinedocs/gcc/Common-Variable-Attributes.html) — `std::align_val_t` 和 over-aligned 分配的 GCC 实现。
- Linux 内核 `mm/slab.h` / `mm/slub.c`: Linux 的 kmalloc/kfree 接口设计——对比 Cinux 的新旧/delete 和 Linux 的 kmalloc，理解"C++ 风格 vs C 风格"的接口设计差异。
