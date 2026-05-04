# 017-4 C++ new/delete 接管与测试策略

## 导语

前三章我们实现了堆分配器的核心——init、alloc、free、coalesce、expand。它已经是一个功能完整的 kmalloc/kfree 了。但在 C++ 内核中，我们不会直接调用 `g_heap.alloc()` 和 `g_heap.free()`——C++ 有自己的动态内存语法：`new` 和 `delete`。到 tag 016 为止，Cinux 的 `operator new` 和 `operator delete` 都是"halt on use"的空壳——调用它们内核直接死锁。因为那时候还没有堆分配器。

本章要做两件事。第一，把 `operator new`/`delete` 的所有重载版本全部重定向到 `Heap::alloc`/`free`，这样内核中所有的 C++ 动态内存操作（包括全局对象的构造、容器的内存分配等）都会走我们的堆分配器。第二，搭建一套双层测试体系——host 端单元测试（用 mock 的 PMM/VMM 在 Linux 用户空间运行）和 QEMU 端集成测试（在真实内核环境中运行）——来验证堆分配器的正确性和鲁棒性。

知识前置：你需要了解 C++ 的 operator new/delete 重载机制（全局重载 vs 类重载、sized delete、aligned new），以及 tag 016 的 VMM 测试策略（host 端 mock + QEMU 端真实环境）。

## 概念精讲

### C++ 的 operator new/delete 家族

C++17 标准定义了一整套 operator new 和 operator delete 的重载版本。最基本的四个是：`operator new(size_t)` 分配单个对象、`operator new[](size_t)` 分配数组、`operator delete(void*)` 释放单个对象、`operator delete[](void*)` 释放数组。除此之外还有 sized delete 版本（`operator delete(void*, size_t)`），编译器在编译期已知对象大小时会调用这个版本，理论上可以用于优化释放操作——但在我们的实现中 size 参数被忽略，因为 BlockHeader 已经记录了大小。

C++17 还引入了 aligned new：`operator new(size_t, std::align_val_t)` 和对应的 delete 版本。`std::align_val_t` 是一个枚举类型的包装，用于传递对齐要求。比如 `new(std::align_val_t{64}) MyStruct` 会调用 `operator new(sizeof(MyStruct), std::align_val_t{64})`，请求 64 字节对齐的内存。我们的 `Heap::alloc` 天然支持对齐参数，所以实现 aligned new 只需要把 `std::align_val_t` 转换为 `size_t` 然后传递给 alloc。

所有这些重载版本都必须定义在全局命名空间中（不能在任何命名空间内），而且必须使用 C++ 的名称修饰（name mangling）——也就是说它们不能放在 `extern "C"` 块里。之前的空壳版本就是定义在 `extern "C"` 块外面的，新的重定向版本也是。

### 为什么要接管全局 new/delete？

你可能会问：为什么不直接用 `g_heap.alloc()` 和 `g_heap.free()`？有几个重要原因。第一，C++ 的 RAII 模式依赖 new/delete——当你写 `auto p = new MyClass()` 时，编译器自动调用 operator new 分配内存然后调用构造函数；写 `delete p` 时先调用析构函数再调用 operator delete。如果我们不接管 operator new/delete，这些语言特性就无法使用。第二，标准库容器（如果将来引入）内部的内存分配都通过 operator new——接管它意味着所有容器自动使用我们的堆分配器。第三，全局对象的动态初始化（构造函数在 main 之前运行）也可能触发 operator new——不接管的话这些构造函数会触发 halt。

### 双层测试策略

Cinux 的堆分配器测试分为两层。第一层是 host 端单元测试（`test/unit/test_heap.cpp`），在 Linux 用户空间编译运行。由于堆分配器的算法本身不依赖硬件（它只操作链表和地址计算），我们可以在 host 端完全重新实现一遍算法逻辑，用 malloc 分配的缓冲区代替 VMM 映射的虚拟地址空间。这一层测试的优势是运行速度快（毫秒级）、可以用 GDB 调试、可以构造极端测试用例（比如 1000 次随机 alloc/free 循环）。缺点是测试的是"重新实现的算法"而不是"真正的内核代码"——如果重新实现时引入了和原始代码不同的 bug，测试结果就不可信了。

第二层是 QEMU 端集成测试（`kernel/test/test_heap.cpp`），在真实内核环境中运行。它使用真正的 `g_heap` 全局实例，背后是真实的 PMM 和 VMM。这一层测试的优势是验证端到端的正确性——从 PMM 分配物理页、到 VMM 映射虚拟地址、到 Heap 分配 payload、到数据写入和读回——整个链条都在真实环境中运行。缺点是运行在 QEMU 中，速度较慢，调试不如 host 端方便。

两层测试互补——host 端覆盖大量边界情况和压力测试，QEMU 端验证真实环境的集成正确性。这种策略和 tag 015/016 中 PMM/VMM 的测试方式一脉相承。

### Host 端 mock 策略

Host 端测试不链接任何内核代码，而是把 Heap 的算法完全重新实现在测试文件中。具体做法是：定义一个 TestHeap 类，它的 init 方法用 `calloc` 分配一块内存（代替 VMM 映射），alloc 和 free_mem 方法重新实现 first-fit、split、coalesce 的逻辑（和 heap.cpp 中的算法完全一致）。测试用例通过 TestHeap 的公开接口（alloc、free_mem、used、free_total、free_block_count 等）验证分配器的各种行为。

TestHeap 还提供了一些诊断方法，比如 `validate_free_list()` 检查链表中是否有环（通过遍历计数上限）、所有块是否标记为 free、所有 magic 是否正确；`has_valid_magic(ptr)` 和 `is_block_free(ptr)` 用于从外部检查某个 payload 指针对应的块状态。这些方法在内核版本的 Heap 中没有（为了减少开销），但在测试中非常有价值。

## 动手实现

### Step 1: 重定向 operator new/delete 到 Heap

**目标**: 替换 crt_stub.cpp 中所有 operator new/delete 的 halt-on-use 空壳，改为调用 Heap::alloc 和 Heap::free。

**设计思路**: 需要实现八个重载版本。四个基本版本：`operator new(size_t)` 调用 `g_heap.alloc(size)`；`operator new[](size_t)` 调用 `g_heap.alloc(size)`；`operator delete(void*)` 调用 `g_heap.free(ptr)`；`operator delete[](void*)` 调用 `g_heap.free(ptr)`。两个 sized 版本：`operator delete(void*, size_t)` 和 `operator delete[](void*, size_t)`——size 参数忽略，直接调用 `g_heap.free(ptr)`。两个 aligned 版本：`operator new(size_t, std::align_val_t)` 调用 `g_heap.alloc(size, static_cast<size_t>(align))`；`operator delete(void*, std::align_val_t)` 调用 `g_heap.free(ptr)`（alignment 参数忽略，因为 free 不需要知道对齐信息）。

需要新增 `#include <new>` 来获取 `std::align_val_t` 的定义，以及 `#include <stddef.h>` 来获取 `size_t`。

**实现约束**: 所有函数定义在 `kernel/arch/x86_64/crt_stub.cpp` 中，位于 `extern "C"` 块之外（需要 C++ 名称修饰）。函数签名中的 `unsigned long` 对应 size_t（在 x86-64 上 size_t 就是 unsigned long）。使用 `static_cast<size_t>` 进行类型转换。noexcept 说明符不能少——operator delete 必须是 noexcept 的。

**踩坑预警**: 如果你忘了加 `#include <new>`，`std::align_val_t` 会编译报错。如果你把 aligned 版本的函数放在了 `extern "C"` 块里，链接器会找不到它们（因为 C 链接不进行名称修饰，而编译器生成的新表达式使用 C++ 链接）。另一个坑是 `operator delete(void*, unsigned long, std::align_val_t)` 这个三参数版本——它的参数顺序是 (指针, 大小, 对齐)，不是 (指针, 对齐, 大小)。顺序写反了编译不报错（类型恰好兼容），但运行时 alignment 参数拿到了 size 的值，会导致 alloc 使用错误的对齐。

**验证**: 在内核中尝试用 `new` 分配一个对象，然后用 `delete` 释放。比如在某处写 `auto* p = new int(42); *p; delete p;`——如果不触发 halt，说明重定向成功。更正式的验证在后续的测试中进行。

### Step 2: 编写 host 端单元测试

**目标**: 在 `test/unit/test_heap.cpp` 中实现 TestHeap 类和全面的测试用例。

**设计思路**: TestHeap 类的 init 方法用 `calloc` 分配内存并初始化第一个空闲块。alloc 和 free_mem 实现和 kernel 版本相同的 first-fit + split + coalesce 算法。不需要实现 expand（host 端的缓冲区是固定大小的，用于测试有限空间下的行为）。

测试用例需要覆盖以下场景。第一组：基本功能——alloc 返回非空指针、默认 16 字节对齐、alloc(0) 返回 nullptr。第二组：对齐——64 字节对齐、4096 字节对齐。第三组：分裂——大块分配后空闲链表仍有剩余、分裂后的块 magic 正确。第四组：释放与合并——free(nullptr) 是安全的 no-op、free 减少 used 计数、相邻块正向合并、相邻块反向合并。第五组：double-free 检测——两次 free 同一个指针不会损坏 used 计数。第六组：多块分配——50 个连续分配全部成功、全部释放后 used 为 0。第七组：大对齐下的 front padding——4096 对齐的分配能正确工作，free 也能正确释放。第八组：三块相邻合并——无论释放顺序（正序、反序、乱序），三个相邻块最终都能合并。第九组：数据完整性——分配的内存被清零、写入的数据能正确读回、free 后重新 alloc 得到的内存也被清零。第十组：压力测试——200 次交错 alloc/free 循环加上 100 块分配后全部释放，验证无泄漏。第十一组：边界情况——alloc(1) 最小分配、堆耗尽时返回 nullptr、大块分配（接近页面大小）。第十二组：空闲链表完整性——碎片化分配释放后 validate_free_list 通过。

**实现约束**: 测试文件定义 `TEST_FRAMEWORK_IMPL` 宏并 include `test_framework.h`。BlockHeader 结构体在测试文件内部重新定义（镜像 kernel 版本），和 kernel 代码没有链接关系。测试用例使用 `TEST("name") { ... }` 宏。

**踩坑预警**: TestHeap 中 coalesce 的实现必须和 kernel 版本完全一致，否则测试的覆盖范围就打了折扣。最容易出偏差的地方是"链表头摘除"的处理——在 TestHeap 中也要检查 free_list_ == block 的情况。另一个要注意的是，TestHeap 的 free_mem 函数不调用 expand——如果缓冲区耗尽，alloc 返回 nullptr。这意味着"堆耗尽"测试在 host 端是可测的（给一个很小的缓冲区比如 1 页，然后不断分配直到返回 nullptr），但在 kernel 端不可测（因为 expand 会自动扩展）。

**验证**: 构建并运行 host 端测试：

```bash
cmake --build build && cd build && ctest -L heap --output-on-failure
```

所有测试用例应通过。如果有失败，ctest 会打印具体的断言失败信息和行号。

### Step 3: 编写 QEMU 端集成测试

**目标**: 在 `kernel/test/test_heap.cpp` 中实现使用真实 g_heap 的测试用例。

**设计思路**: QEMU 端测试在 `big_kernel_test` 中运行，使用真正的 `cinux::mm::g_heap` 全局实例。测试前需要确保堆已经初始化（在 `main_test.cpp` 中调用 `g_heap.init`）。

测试用例覆盖以下场景。第一个：基本分配——分配 64 字节，写入递增序列，读回验证。第二个：默认对齐——验证 alloc(64) 返回的地址是 16 字节对齐的。第三个：页对齐——alloc(64, 4096) 返回 4096 对齐的地址。第四个：alloc(0) 返回 nullptr。第五个：free(nullptr) 不 crash。第六个：多块分配不重叠——分配三个 64 字节块，写入不同模式（0xAA、0xBB、0xCC），读回验证各块数据独立。第七个：合并——分配三个块，全部释放，再分配一个 256 字节的大块，验证合并后空间足够。第八个：奇数大小——分配 37、23、41 字节，验证全部 16 字节对齐且数据正确。第九个：压力测试——50 次循环，每次分配不同大小的块，中间穿插释放和重新分配。第十个：dump_stats 不 crash。

**实现约束**: 测试文件定义在 `kernel/test/test_heap.cpp` 中，使用 `big_kernel_test.h` 的 TEST_ASSERT 宏。由于内核中没有 memset，需要自己实现一个简单的 kmemset 辅助函数。入口函数是 `run_heap_tests()`，声明在 `big_kernel_test.h` 中，在 `main_test.cpp` 中调用。

**踩坑预警**: QEMU 端测试使用的堆地址是 `0xFFFFFFFF80100000ULL`（和正式内核不同，因为测试内核的地址空间布局不同）。如果你用了正式内核的地址 `0xFFFF800000000000`，可能会和测试内核的代码段重叠。另外，QEMU 端测试的初始堆大小也是 64 KB，但 50 次压力测试可能触发 expand——这是正常的，测试不应该假设堆空间固定不变。

**验证**: 构建 big_kernel_test 并在 QEMU 中运行：

```bash
cmake --build build && ./build/run_qemu_test.sh
```

串口输出应该显示：

```
=== Heap Tests (017) ===
  PASS: test_basic_alloc
  PASS: test_default_alignment
  ...
  PASS: test_many_cycles
  PASS: test_dump_no_crash
=== Heap Tests: XX passed, 0 failed ===
```

### Step 4: 将测试集成到构建系统

**目标**: 在 CMakeLists.txt 中注册新的测试目标。

**设计思路**: 有两处需要修改。第一处，`kernel/CMakeLists.txt`——在 `big_kernel_common` 库中添加 `mm/heap.cpp`（之前章节已经做了），在 `big_kernel_test` 的源文件列表中添加 `test/test_heap.cpp`。第二处，`test/CMakeLists.txt`——添加 `test_heap` 测试可执行文件，链接 `unit/test_heap.cpp`，加入 `test_host` 和 `test` 的依赖列表。

**踩坑预警**: 确保 `test_heap` 目标的 include 路径正确——它需要能找到 `test_framework.h`、`kernel/mm/heap.hpp`（用于类型定义的镜像参考，虽然不直接 include）、以及各种内核头文件。如果 include 路径有误，编译时会报找不到头文件的错误。

**验证**: 完整构建并运行所有测试：

```bash
cmake --build build && cd build && ctest --output-on-failure
```

所有测试（包括之前的 PMM、VMM、kprintf 测试）加上新增的 heap 测试应该全部通过。

## 构建与运行

本章的修改集中在 `kernel/arch/x86_64/crt_stub.cpp`（operator new/delete 重定向）、`test/unit/test_heap.cpp`（host 端测试）和 `kernel/test/test_heap.cpp`（QEMU 端测试）。

运行 host 端测试：

```bash
cmake --build build && cd build && ctest -L heap --output-on-failure
```

运行 QEMU 端测试：

```bash
cmake --build build && ./build/run_qemu_test.sh
```

运行全部测试（包括之前的所有 tag）：

```bash
cmake --build build && cd build && ctest --output-on-failure
```

## 调试技巧

**operator new 调用后内核死锁**: 如果在堆初始化之前有代码调用了 new，它会触发旧的 halt-on-use 代码（如果你忘了替换的话）或者触发 g_heap 的未初始化访问。检查 `crt_stub.cpp` 是否已经完整替换了所有 halt-on-use 的空壳。用 `nm` 或 `objdump` 检查编译后的目标文件，确认 operator new 指向了调用 g_heap.alloc 的实现而不是死循环。

**host 测试中 TestHeap 行为和 kernel Heap 不一致**: 这是最棘手的问题——如果 TestHeap 的重新实现和 kernel Heap 有微妙差异，host 测试可能全部通过但 kernel 中实际有 bug。解决办法是在 QEMU 端测试中也运行足够的测试用例，特别是 alloc、free、coalesce 的核心路径。如果 QEMU 端测试通过但 host 端失败（或者反过来），说明两边的算法实现不一致，需要逐行对比 alloc 和 free 的逻辑。

**压力测试后 used_ 不为 0**: 这是最经典的内存泄漏信号。在 host 测试中，如果 200 次 alloc/free 循环后 used_ 不是 0，说明某些分配没有被正确释放，或者 free 时 used_ 的更新有 bug。在循环的每一步打印 used_ 的值，找到它开始偏离预期的那个循环次数，然后检查那个循环中的 alloc/free 调用。

**ctest 显示测试崩溃（segfault）**: 如果 host 测试崩溃，通常是因为空闲链表被损坏（野指针或 double-free 未被检测到）。在 TestHeap 的 validate_free_list 中加入更详细的检查——遍历链表时打印每个块的地址、magic 和 size，在崩溃前定位到问题块。

## 本章小结

| 概念 | 要点 |
|------|------|
| operator new/delete | 八个重载版本全部重定向到 g_heap.alloc/free |
| aligned new | std::align_val_t 转为 size_t 传给 alloc 的 align 参数 |
| sized delete | size 参数忽略，BlockHeader 已记录大小 |
| Host 端测试 | TestHeap 用 calloc mock VMM，重新实现算法，覆盖边界情况和压力测试 |
| QEMU 端测试 | 使用真实 g_heap + PMM + VMM，验证端到端正确性 |
| 双层测试互补 | host 端覆盖大量用例，QEMU 端验证真实环境集成 |
| 压力测试 | 200+ 次 alloc/free 循环，验证 used_ 归零（无泄漏） |
| validate_free_list | 检查环、magic、free 标志、地址范围，诊断链表损坏 |
