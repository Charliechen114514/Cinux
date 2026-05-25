---
title: 015-mm-pmm-3 · 物理内存管理
---

# 015-3 页分配、连续分配与测试

## 导语

前两章我们把 PMM 的数据结构和初始化流程搭建完毕——位图就位了，内核区域被保护了，E820 可用区域被正确解锁了。现在位图里躺着成千上万个 0 bit（空闲页），等着被分配出去。这一章要做的就是实现分配和释放的运行时接口，然后通过双轨测试（host 单元测试 + QEMU 内核集成测试）来验证整个 PMM 的正确性。

我们要实现两套 API：单页分配（`alloc_page` / `free_page`）和连续多页分配（`alloc_pages` / `free_pages`）。前者使用上一章实现的 64 位加速扫描，O(1) 到 O(N) 的时间复杂度取决于位图的稀疏程度；后者是 first-fit 线性扫描，O(N) 的时间复杂度。最后我们会看到怎么把 PMM 的统计信息打印到串口，确认一切运转正常。

知识前置：前两章的全部内容。你需要理解位图的 bit 语义（1=已用，0=空闲）、`bm_find_first_free` 的 64 位加速扫描原理、以及上一章中保守标记策略的八步初始化流程。

## 概念精讲

### 单页分配：扫描、标记、返回

`alloc_page` 的工作极其直观——调用 `bm_find_first_free` 找到第一个空闲 bit，把它置为 1（已用），然后把 bit 索引乘以 PAGE_SIZE 转换成物理地址返回给调用者。如果 `bm_find_first_free` 返回 -1（位图全满），说明没有可用页了，返回 0 作为 OOM 标志。

这里有一个设计决策：为什么用物理地址 0 作为 OOM 标志而不是用一个特殊的错误码？因为在 x86 PC 架构中，物理地址 0 是中断向量表（IVT）的位置——它永远不应该被分配给普通用途。我们的 PMM 管理的物理内存从 1MB 开始，所以物理地址 0 以下根本不在位图的管理范围内。调用者拿到返回值后只需要检查 `if (addr != 0)` 就能区分成功和 OOM——非常简洁。

`free_page` 做的事情更简单：把物理地址转换回 bit 索引，把对应的 bit 清零，增加 `free_pages_` 计数。但我们需要加入防御性检查——调用者可能传入无效的地址，比如物理地址 0（对 free_page(0) 执行 no-op）、超出管理范围的地址（索引 >= `highest_page_`）、或者对已经空闲的页再次 free（double-free）。对于这些情况，free_page 选择静默忽略而不是 panic——在一个教学 OS 中，过度防御比 crash 好。

### 连续多页分配：First-Fit 线性扫描

有些场景需要连续的物理页——比如 DMA 缓冲区（虽然 Cinux 暂时不需要）、或者大页（huge page）的映射。`alloc_pages(count)` 分配 count 个连续的物理页，返回起始物理地址。

实现策略是经典的 first-fit：从页 0 开始线性扫描位图，维护一个"连续空闲计数器" `run` 和一个"连续空闲区域起始" `start`。遇到空闲页时 `run++`，遇到已用页时 `run` 归零。当 `run >= count` 时，说明找到了足够长的连续空闲区域——把这 count 个页全部标记为已用，返回起始物理地址。如果扫描到 `highest_page_` 都没找到足够长的连续区域，返回 0（OOM）。

这个算法的最坏时间复杂度是 O(N)（N 是总页数；外层循环扫描 N 个页，内层循环最多执行一次标记 M 个页后返回，O(N+M) = O(N)）。对于我们的教学 OS 来说，物理内存通常在 32MB-128MB 范围内，总页数在几千到几万之间，线性扫描的延迟完全可以忽略。但如果你将来要把 PMM 用在性能敏感的场景中，应该考虑 buddy 分配器（SerenityOS 的方案）或者 per-order freelist（Linux 的方案）。

### 防御性 free 操作

`free_page` 实现了三层防御。第一层：如果传入的物理地址是 0，直接返回——这是 OOM 标志，不是一个有效的物理页。第二层：如果物理地址对应的页索引超出了 `highest_page_`，直接返回——这可能是调用者传入了一个 MMIO 地址或者一个计算错误的地址。第三层：检查目标 bit 是否确实是 1（已用），如果不是 1 说明这是一个 double-free——对应的页已经是空闲的了，再次 clear 没有意义，而且会导致 `free_pages_` 被多加一次。

这三层防御保证了即使调用者犯了错误（比如 double-free 或者 free 了不属于 PMM 管理范围的地址），PMM 的内部状态也不会被破坏。这是一种"宽容接收、严格管理"的设计哲学——接口宽容地忽略无效输入，但内部状态始终保持一致。

### 双轨测试策略

Cinux 的 PMM 测试分两条轨道。第一条是 host 端单元测试（`test/unit/test_pmm.cpp`），在 Linux 用户空间编译运行。它重新实现了 PMM 的核心算法（位图操作、parse_memory_map），测试各种边界情况——类型过滤、低内存截断、4KB 对齐、OOM、double-free、连续分配在碎片化内存上失败等。这条轨道的优点是迭代速度快（不需要 QEMU）且可以方便地构造极端场景。

第二条是 QEMU in-kernel 集成测试（`kernel/test/test_pmm.cpp`），在真实的内核环境中运行。它使用真实的 BootInfo 数据（来自 QEMU 的 E820 报告）初始化 PMM，然后测试实际的分配/释放循环——单页分配后检查物理地址是否 4KB 对齐、16 页批量分配后检查 free_pages 计数是否正确恢复、连续 4 页分配后检查物理地址是否连续等。这条轨道验证的是"真正的内核能正确使用 PMM"——不存在 host mock 可能引入的偏差。

参考：

- [OSDev Wiki - Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) -- bitmap vs linked list vs buddy allocator 的比较
- Linux bootmem (LWN): [The Linux Bootmem Framework](https://lwn.net/Articles/761215/) -- Linux 的 bootmem 位图分配器与 Cinux 的 PMM 原理相同

## 动手实现

### Step 1: 实现 alloc_page 和 free_page

**目标**: 编写单页分配和释放的运行时接口。

**设计思路**: `alloc_page` 调用 `bm_find_first_free` 扫描位图。如果返回 -1，说明所有页都已用，返回 0。否则，把返回的 bit 索引对应的 bit 置为 1（调用 `bm_set`），减少 `free_pages_` 计数，然后返回 `bit_index * PAGE_SIZE` 作为物理地址。

`free_page` 接收一个物理地址。首先检查三个防御条件：物理地址为 0 则直接返回；转换为页索引后如果索引 >= `highest_page_` 则直接返回；用 `bm_test` 检查该 bit 是否为 1，如果不是 1（已经是空闲的）则直接返回。通过三层检查后，调用 `bm_clear` 清零该 bit，增加 `free_pages_` 计数。

**实现约束**: 两个方法都是 PMM 类的 public 成员。`alloc_page` 返回 `uint64_t`（物理地址，0 表示 OOM）。`free_page` 返回 `void`。位图操作使用上一章定义的匿名命名空间中的辅助函数。

**踩坑预警**: `bm_find_first_free` 返回的是 `int64_t`——在转换为 `uint64_t` 之前必须检查是否 < 0。如果你不小心写了 `uint64_t idx = bm_find_first_free(...)` 然后直接用，当返回 -1 时 `idx` 变成了一个巨大的正数（0xFFFFFFFFFFFFFFFF），后续的 `bm_set(bitmap_, idx)` 会写到离位图很远很远的内存位置，大概率 triple fault。记住：在有符号/无符号混合使用的场景中，先做符号检查再做强转。

**验证**: 我们会在后面的 QEMU 集成测试中全面验证这两个函数。现在可以暂时在 init 完成后加一个简单的测试：调用 `alloc_page()` 两次，检查返回的地址是否非零且 4KB 对齐，然后依次 `free_page` 回去，打印 free_pages 计数看是否恢复到初始值。当然，这段测试代码只是临时调试用的，正式测试应该在专门的测试文件中。

### Step 2: 实现 alloc_pages 和 free_pages（连续分配）

**目标**: 实现连续多页的分配和释放。

**设计思路**: `alloc_pages(count)` 分三种情况处理。如果 count 为 0，直接返回 0（没有意义的请求）。如果 count 为 1，直接委托给 `alloc_page()`——利用 64 位加速扫描的快速路径，不需要走 first-fit 线性扫描。如果 count > 1，执行 first-fit 扫描：从页 0 开始，维护 `run`（连续空闲计数）和 `start`（当前连续区域起始）。扫描到空闲页时 `run++`，扫描到已用页时 `run` 归零。当 `run` 达到 count 时，从 `start` 到 `start + count - 1` 逐页调用 `bm_set` 标记为已用，减少 `free_pages_` 计数，返回 `start * PAGE_SIZE`。如果整个位图扫描完都没找到足够长的连续区域，返回 0。

`free_pages(phys, count)` 简单得多——它就是一个循环，对从 phys 开始的每一页调用 `free_page`。由于 `free_page` 自带防御性检查，所以即使传入的范围中某些页本来就是空闲的，也不会出问题——只是那些页会被静默跳过。

**实现约束**: `alloc_pages` 返回 `uint64_t`（起始物理地址，0 表示 OOM），`free_pages` 返回 void。first-fit 扫描中的 `run` 和 `start` 使用 `uint64_t` 类型。逐页标记时用 `bm_set`（不是 `mark_region_used`，因为后者还要做状态检查和计数操作，而我们已经确定这些页是空闲的了——但为了保持计数一致性，仍然需要手动减少 `free_pages_`）。

**踩坑预警**: first-fit 扫描中有一个容易忽略的边界条件：当 `run` 达到 count 时的起始位置。正确的逻辑是 `start` 记录的是"当前连续空闲区域的第一页"——也就是说，当遇到一个已用页时 `run` 归零，遇到一个空闲页且 `run` 为 0 时把 `start` 设为当前页号。如果你把 `start` 的赋值逻辑写错了（比如每次空闲都更新 start），那么找到的连续区域就不对。另外一个性能相关的注意点：`alloc_pages` 对 count=1 走了 `alloc_page` 的快速路径，但如果你忘了这个优化，count=1 也会走 first-fit 扫描——结果正确但性能差 64 倍（因为 first-fit 是逐 bit 扫描的，而 `alloc_page` 使用了 64 位加速）。

**验证**: 在 QEMU 集成测试中会验证连续分配的正确性：分配 4 个连续页，检查返回地址非零且 4KB 对齐，检查 free_pages 减少了 4，释放后检查 free_pages 恢复。host 端测试中还会验证碎片化场景：把位图设成"棋盘"模式（偶数页空闲、奇数页已用），然后尝试分配 2 个连续页——应该返回 0（OOM），因为没有任何两个相邻的空闲页。

### Step 3: 实现 free_page_count 和 total_page_count 统计方法

**目标**: 提供查询接口，让外部模块获取 PMM 的状态信息。

**设计思路**: 这两个方法是纯查询方法——直接返回 `free_pages_` 和 `total_pages_` 成员变量的值。不需要任何计算或者锁——在当前的单核内核中，没有并发访问的问题。`free_page_count` 返回的是"当前有多少个空闲页"，`total_page_count` 返回的是"PMM 管理的总页数"。

这两个方法看似简单，但在调试和监控中非常有用。比如你可以在 alloc 后打印 `free_page_count()` 来确认分配确实生效了，或者在一系列操作后检查 `free_page_count()` 是否回到了初始值来确认没有内存泄漏。在 QEMU 测试中，这两个方法是几乎所有断言的基础。

**实现约束**: 两个方法都是 `const` 成员函数（不修改对象状态），返回 `uint64_t`。定义在头文件中声明、cpp 文件中实现。

**踩坑预警**: 如果你把这两个方法声明为 const 但在实现中忘了加 const 限定符，编译器会认为这是两个不同的函数（一个 const、一个非 const），链接时找不到 const 版本的实现会报错。这属于"编译器是对的，但报错信息可能很晦涩"的情况。

**验证**: 这两个方法会在集成测试中被大量使用，不需要独立验证。

### Step 4: 编写 QEMU in-kernel 集成测试

**目标**: 在 QEMU 内核环境中测试 PMM 的完整功能。

**设计思路**: 测试文件是 `kernel/test/test_pmm.cpp`，作为 `big_kernel_test` 可执行文件的一部分运行。测试在 `main_test.cpp` 的 `kernel_main` 中被调度——在 PMM init 完成之后调用 `run_pmm_tests()`。

测试用例设计为六个。第一个验证初始化统计：total_page_count > 0，free_page_count > 0，free <= total。第二个测试单页分配/释放循环：分配一页，检查返回地址非零且 4KB 对齐，检查 free_pages 减少了 1，释放后检查恢复。第三个测试批量分配/释放：连续分配 16 页，检查每页都非零且对齐，检查 free_pages 减少了 16，全部释放后检查恢复。第四个测试连续多页分配：调用 `alloc_pages(4)` 分配 4 个连续页，检查返回地址非零且对齐，释放后检查恢复。第五个测试 free_page(0) 的 no-op 行为：调用 `free_page(0)` 后 free_pages 不变。第六个测试 double-free 的 no-op 行为：分配一页，释放它，再次释放，第二次释放后 free_pages 不变。

每个测试用例都是一个独立的函数，放在独立的命名空间中（`test_pmm_init`、`test_pmm_alloc`、`test_pmm_bulk`、`test_pmm_contiguous`、`test_pmm_edge`），通过 `TEST_ASSERT_EQ`、`TEST_ASSERT_NE`、`TEST_ASSERT_GT`、`TEST_ASSERT_GE` 等宏做断言。入口函数 `run_pmm_tests()` 用 `TEST_SECTION` 和 `RUN_TEST` 宏组织测试的执行和报告。

**实现约束**: 测试文件 include `big_kernel_test.h`（测试框架）和 `kernel/mm/pmm.hpp`。使用 `cinux::mm::g_pmm` 全局实例。在 `main_test.cpp` 中，PMM init 必须在 `run_pmm_tests()` 之前执行——也就是说 `g_pmm.init(*boot_info)` 要在所有 PMM 测试之前被调用。入口函数 `run_pmm_tests()` 声明为 `extern "C"` 以避免 C++ name mangling。

**踩坑预警**: 测试中的断言宏（`TEST_ASSERT_EQ` 等）要求左右操作数的类型兼容——如果一个是 `uint64_t` 另一个是 `int`，可能会触发有符号/无符号比较警告。解决方法是在字面量后面加 `u` 后缀（比如 `0u` 而不是 `0`）。另外，测试中分配的页面必须在测试结束时全部释放——如果某个测试提前 return 了（比如断言失败后），忘记释放的页面会导致后续测试的 free_pages 计数不正确。虽然我们的测试框架在断言失败后通常会继续执行（而不是 abort），但保持"谁分配谁释放"的原则是好习惯。

**验证**: 构建 `big_kernel_test` 并在 QEMU 中运行。PMM 测试部分应该输出类似：

```
=== PMM Tests (015) ===
  [PASS] test_init_and_stats
  [PASS] test_alloc_free_cycle
  [PASS] test_bulk_alloc_free
  [PASS] test_alloc_pages_contiguous
  [PASS] test_free_zero_noop
  [PASS] test_double_free_noop
=== 6 passed, 0 failed ===
```

如果任何测试失败，输出会显示期望值和实际值的差异。

### Step 5: 完善 host 端单元测试

**目标**: 在 host 端覆盖位图分配器的算法正确性。

**设计思路**: 在上一章写好的 `parse_memory_map` 测试基础上，添加位图分配器相关的测试用例。由于 host 端无法使用真正的 PMM（它依赖内核的 linker symbol 和物理内存映射），我们在测试文件中重新实现一个 `TestPMM` 类——它复制了 PMM 的核心算法（位图操作、alloc_page、free_page、alloc_pages、free_pages），但使用普通的 `new` 分配位图而不是内核内存。

测试用例需要覆盖以下场景。第一个：单页分配返回 4KB 对齐的地址。第二个：1000 次连续分配/释放循环后 free_pages 计数恢复到初始值。第三个：所有页分配完毕后 alloc_page 返回 0（OOM）。第四个：free_page(0) 是 no-op。第五个：double-free 是 no-op。第六个：alloc_pages(4) 返回连续的物理地址。第七个：在碎片化内存（棋盘模式）上 alloc_pages(2) 失败。第八个：mark_free 和 mark_used 的计数正确，重复操作是 no-op。

**实现约束**: 测试文件是 `test/unit/test_pmm.cpp`，使用 Cinux 自带的 `test_framework.h`（define `TEST_FRAMEWORK_IMPL` 来拉入实现）。`TestPMM` 类放在匿名命名空间中。编译为独立的可执行文件 `test_pmm`，在 `test/CMakeLists.txt` 中定义目标。编译定义中加 `CINUX_HOST_TEST` 宏——虽然当前的 PMM 测试不依赖这个宏，但保持和其他 host 测试的一致性是好的。

**踩坑预警**: `TestPMM` 的构造函数接收的是"总页数"而不是"总内存大小"——如果你传入的是字节数而不是页数，位图会大得离谱。比如传入 128 * 1024 * 1024（128MB 的字节数）而不是 128 * 1024 * 1024 / 4096（128MB 的页数 = 32768），位图会尝试分配 16GB 的内存，直接 OOM。另外，`TestPMM` 的 `alloc_page` 方法中，页 0 对应的物理地址是 0——所以在构造测试时，应该从页 1 开始 mark_free（`mark_free(1, total_pages - 1)`），否则 `alloc_page` 可能返回 0 被误判为 OOM。

**验证**: 运行 host 端测试：

```
cmake --build build && cd build && ctest -L pmm --output-on-failure
```

所有 12 个测试用例（5 个 parse_memory_map + 7 个位图分配器 + 1 个 mark 计数）应该全部通过。如果有失败，ctest 会输出断言失败的具体位置和期望/实际值。

### Step 6: 更新构建系统和运行完整测试

**目标**: 确保所有测试目标正确集成到构建系统中。

**设计思路**: 需要在两个 CMakeLists.txt 中添加条目。首先是 `kernel/CMakeLists.txt`——在 `big_kernel_common` 库中添加 `mm/pmm.cpp`，在 `big_kernel_test` 可执行文件中添加 `test/test_pmm.cpp`。然后是 `test/CMakeLists.txt`——添加 `test_pmm` 可执行文件目标（编译 `unit/test_pmm.cpp`），设置 include 目录和编译定义，注册 ctest，最后把它加到 `test_host`、`test`、`test_verbose` 三个自定义目标的依赖列表中。

**实现约束**: `test_pmm` 目标的 include 目录需要包含 `${TEST_INCLUDE_DIRS}`（测试框架头文件路径）和 `${CMAKE_SOURCE_DIR}`（项目根目录，用于 include `boot/boot_info.h`）。ctest 的标签设为 `pmm`，方便按标签运行。

**踩坑预警**: 如果你忘了把 `test_pmm` 加入 `test_host` 目标的依赖列表，那么 `cmake --build build --target test_host` 不会编译 `test_pmm`——虽然单独运行 `ctest -L pmm` 会报 "test executable not found"。这是一个容易遗漏的步骤，因为每添加一个新的测试目标都需要在三个地方（`test_host`、`test`、`test_verbose`）手动加入依赖。另外，`big_kernel_test` 的源文件列表中也需要加入 `test/test_pmm.cpp`——如果你只加了 `mm/pmm.cpp` 到 `big_kernel_common` 而忘了加测试文件到 `big_kernel_test`，链接时会报 "undefined reference to run_pmm_tests"。

**验证**: 运行完整的测试流程：

```
cmake --build build && cd build && ctest --output-on-failure
```

所有测试标签（smoke、kprintf_format、gdt_idt、ata、elf_loader、big_kernel_loader、kprintf、pic、pit、font、console、framebuffer、keyboard、pmm）应该全部通过。特别关注 `pmm` 标签下的 12 个 host 端测试和 QEMU 内核测试中的 6 个 PMM 集成测试。

## 构建与运行

构建并运行完整测试套件：

```
cmake --build build && cd build && ctest -L pmm --output-on-failure
```

Host 端测试应该输出：

```
Test project /path/to/build
    Start 1: pmm
1/1 Test #1: pmm ..............................   Passed
```

QEMU in-kernel 测试需要通过 `test` 目标触发（会先跑 host 测试，然后启动 QEMU 运行内核测试）：

```
cmake --build build --target test-image
cmake --build build --target test
```

在 QEMU 的串口输出中，PMM 测试部分应该显示 6 个 PASS。

如果你只想验证 PMM 不 crash，最简单的方式是运行正常的 big_kernel（不是 test 版本），看串口输出中是否有 `[PMM] Total: XXMB, Free: YYMB` 这行，以及后续的初始化是否正常继续。

## 调试技巧

**alloc_page 始终返回 0（OOM）**: 首先检查 `free_pages_` 的值——如果在 init 完成后它就是 0，说明第五步（解锁可用区域）没有生效。用 kprintf 在 `mark_region_free` 调用前后打印 free_pages_ 的值来确认。如果 free_pages_ > 0 但 alloc_page 仍然返回 0，说明 `bm_find_first_free` 的扫描有 bug——用 kprintf 打印位图的前几个字节，看是否全为 0xFF。

**连续分配总是失败**: 如果 `alloc_pages(4)` 总是返回 0，可能的原因有两个。一是物理内存确实高度碎片化——不太可能在 QEMU 中发生（QEMU 的 E820 通常只报告一两个大的可用区域），但在某些 BIOS 配置下可能出现。二是 first-fit 扫描的 `run` 计数逻辑有 bug。调试方法是在扫描循环中加 kprintf，打印当前页号、bit 状态和 run 值，看扫描是否在正确的位置找到了连续空闲区域。

**host 测试中 TestPMM 的 free_count 不一致**: 首先确认 `mark_free` 和 `mark_used` 的计数逻辑是否正确——它们应该只在状态确实发生变化时才更新 `free_pages_`。如果连续调用两次 `mark_free(10, 5)`，第二次不应该增加 free_pages_。在测试中加断言检查每一步操作后 free_count 的精确值。

**QEMU 测试中 PMM init crash**: 这是上一章提到过的问题，但值得再强调一遍。最可能的原因是 linker symbol 取地址错误。在 `main_test.cpp` 中，PMM init 的调用和正式内核中的调用方式完全一样——`g_pmm.init(*boot_info)`——所以如果正式内核能正常 init 而测试内核 crash，问题可能出在测试内核的链接脚本中缺少了 `__kernel_stack_top` 符号的定义。

## 本章小结

| 概念 | 关键点 |
|------|--------|
| alloc_page | bm_find_first_free + bm_set，返回物理地址，0 = OOM |
| free_page | 三层防御：phys==0、越界、double-free，全部静默 no-op |
| alloc_pages | count=1 走快速路径，count>1 走 first-fit 线性扫描 |
| free_pages | 循环调用 free_page，依赖其防御性检查 |
| OOM 标志 | 物理地址 0，因为物理地址 0（IVT）不在 PMM 管理范围内 |
| Host 单元测试 | TestPMM 类复制核心算法，覆盖 parse_memory_map + 分配器 |
| QEMU 集成测试 | 真实 BootInfo 数据，6 个测试用例覆盖核心场景 |
| 构建集成 | kernel/CMakeLists.txt + test/CMakeLists.txt 添加 PMM 目标 |
