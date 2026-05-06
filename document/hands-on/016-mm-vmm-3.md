# 016-3 缺页异常处理与按需分页

## 导语

前两章我们把 VMM 的核心三件套——map、unmap、translate——实现了，也跑通了基本的单元测试。但 VMM 的价值不只是"手动映射几个页然后查查地址"——它最强大的能力是配合硬件的缺页异常机制实现**按需分页**（demand paging）。简单来说就是：先不映射，等 CPU 真正访问的时候触发 page fault，然后在异常 handler 里现场分配物理页、建立映射、恢复执行。整个过程对触发访问的代码完全透明——它根本不知道自己曾经被 page fault 打断过。

这一章我们要做的就是更新 Cinux 的 #PF handler，让它能区分"合理的缺页"（按需分配的场景）和"真正的错误"（访问了不该访问的地址）。然后在 QEMU 中用一个经典的"先写入一个未映射地址，触发 #PF，handler 自动分配物理页，写入成功后读回验证"的端到端测试来证明整个链条运转正常。

知识前置：前两章的 VMM 实现必须完成。你需要理解 page fault 异常的硬件机制——CPU 在访问一个 Present=0 的 PTE 时触发 #PF（中断向量 14），把触发 fault 的线性地址存入 CR2，把错误码压入栈。我们在 tag 012 中已经注册了基本的 #PF handler，这一章要给它加上按需分配的逻辑。

## 概念精讲

### Page Fault 异常机制

Intel SDM Vol.3A Section 4.7 对 #PF 异常有完整描述。当 CPU 在虚拟地址翻译过程中遇到以下任何一种情况时，就会触发 page fault：

第一，页表遍历过程中遇到一个 Present=0 的表项——这意味着目标虚拟地址的映射不存在。第二，访问操作违反了 PTE 中的权限位——比如对一个 Read-Only 页（Writable=0）执行写操作，或者从 Supervisor 页（User=0）在用户态执行访问。第三，保留位被设置了——某些 CPU 特性（如 SMEP、SMAP、PKS）会引入额外的检查。

#PF 触发时，CPU 自动做三件事：把触发 fault 的线性地址写入 CR2 寄存器（这样 OS 就知道是哪个地址出了问题）；构造一个错误码并压入栈（错误码的各个 bit 描述了 fault 的具体原因）；把控制转移到 IDT 中向量 14 对应的 handler。等 handler 执行完毕后通过 `iretq` 返回时，CPU 会重新执行那条触发 fault 的指令——这意味着 handler 必须把问题解决掉，否则 CPU 回来执行同样的指令会再次触发 #PF，陷入无限循环。

错误码的位域布局如下：

```
+--+--+--+--+----+----+----+
|P |W |U |R |RSVD|Instr|PK |
|  |/R|/S|SVD|    |     |    |
+--+--+--+--+----+----+----+
 0  1  2  3   4    5

P (bit 0):  0 = 页不存在(not-present), 1 = 权限违反(protection violation)
W/R (bit 1): 0 = 读操作触发, 1 = 写操作触发
U/S (bit 2): 0 = 内核态触发, 1 = 用户态触发
RSVD (bit 3): 1 = 保留位被设置
Instr (bit 4): 1 = 取指触发(IF flag)
PK (bit 5):  1 = Protection Key 违反
```

对我们来说最重要的是 bit 0（P 位）。当 P=0 时，说明触发 fault 的原因是"页不存在"——这正是按需分页要处理的场景。当 P=1 时，说明页是存在的但访问被拒绝了——这是权限错误，按需分页无法处理，应该直接 panic。

> 参考：Intel SDM Vol.3A Section 4.7, pp.4-37 to 4-38
> 参考：[OSDev Wiki - Page Fault](https://wiki.osdev.org/Exceptions#Page_Fault)

### 按需分页的工作原理

Intel SDM Vol.3A Section 4.12 明确描述了按需分页的概念："线性地址空间的某些部分不需要映射到物理地址空间；未映射地址的数据可以存储在外部（比如磁盘）。当 PTE 中 P=0 时，对该地址的访问会触发 #PF。操作系统处理这个 fault 的方式是加载相应的页、更新 PTE、然后恢复执行。"

按需分页是现代操作系统最重要的内存管理策略之一。它带来的好处是多方面的。首先是延迟分配——进程调用 `malloc` 时不需要立刻分配物理内存，只需要在地址空间中预留虚拟地址范围，等真正访问时再分配。其次是内存超额使用——系统可以给所有进程分配的虚拟内存总和超过实际物理内存大小，因为大部分虚拟内存在任何时刻都不会被同时访问。第三是内存共享和写时复制（COW）——多个进程可以共享同一个物理页，等某个进程尝试写入时再触发 #PF，handler 分配一个新的物理页并复制数据。

Cinux 在 tag 016 中实现的按需分页是最简单的版本：任何 not-present fault 都会被自动分配一个新的物理页并映射上去。这没有区分"合理的缺页"和"野指针访问"——任何对未映射地址的访问都会被"原谅"并自动分配物理页。在真正的 OS 中，你需要检查 fault 地址是否落在进程的合法虚拟内存区域（VMA）内，但 Cinux 还没有进程和 VMA 的概念，所以暂时采用这种"有 fault 就分配"的简单策略。

> 参考：Intel SDM Vol.3A Section 4.12, p.4-61
> 参考：Linux 缺页处理 -- `do_page_fault` -> `handle_mm_fault` -> `handle_pte_fault` 的完整链条
> 参考：xv6 `vmfault()` -- 检查进程大小边界后分配并映射，概念上与 Cinux 相同

### CR2 寄存器：Fault 地址的记录者

CR2 是一个专用寄存器，专门用来保存触发 page fault 的线性地址。CPU 在触发 #PF 时自动把出问题的虚拟地址写入 CR2，软件在 handler 中读取它来获取 fault 地址。

有一个细节需要注意：CR2 中保存的地址是**未对齐的**——它精确指向触发 fault 的那条内存访问指令试图访问的字节地址，而不是页对齐的地址。所以我们在 handler 中需要对它做页对齐处理（低 12 位清零），然后才能传给 `VMM::map`——因为 map 操作的粒度是整个 4KB 页。

另外，如果 #PF handler 本身的执行过程中也触发了 #PF（比如 handler 的栈没有被映射），CPU 会触发 double fault（#DF，向量 8）而不是递归调用 #PF handler。如果 double fault handler 也出问题了，就是 triple fault——CPU 直接 reset。所以 #PF handler 的代码路径必须保证不会触发嵌套的 #PF，这意味着 handler 使用的栈和代码必须已经被正确映射。

### 双轨测试策略

和 PMM 一样，VMM 也采用双轨测试策略。

第一条轨道是 host 端单元测试（`test/unit/test_vmm.cpp`）。它在 Linux 用户空间运行，使用一个完全模拟的内存环境——`MockPMM` 类模拟物理内存分配（256 页的模拟内存池），`sim_memory` 数组模拟物理内存内容（128 页，每页 4KB，4096 字节对齐），`TestVMM` 类完整复刻了 VMM 的核心算法。这条轨道覆盖了 map/translate/unmap 的所有基本路径和边界情况——映射单页、保留偏移的翻译、解除映射、多页映射、重映射覆盖、未映射地址处理、高地址映射、同一 PT 中的多个映射、以及 PageEntry 位域的正确性。host 测试的优点是速度快（毫秒级完成）且可以方便地构造极端场景，缺点是它不测试真实的硬件交互——没有真正的 TLB、没有真正的 CR3、没有真正的 page fault。

第二条轨道是 QEMU in-kernel 集成测试（`kernel/test/test_vmm.cpp`）。它在真实的内核环境中运行，使用真正的页表、真正的 PMM、真正的 TLB。除了验证 map/translate/unmap 的基本功能外，它还包含一个关键的**按需分页测试**：把一个指针指向一个绝对没有被映射过的地址（比如 0x40000000），然后直接往那个地址写入一个魔术值。这条写入指令会触发 #PF，handler 自动分配物理页并映射，然后 CPU 重新执行写入指令——这次成功了。测试随后读回那个地址的值，验证魔术值完好无损，最后用 `translate` 确认映射确实存在，用 `unmap` 清理并释放物理页。这条轨道验证的是"硬件异常 -> handler 处理 -> 指令恢复执行"的完整闭环。

> 参考：xv6 测试策略 -- xv6 也有在内核中直接访问 unmapped 地址来触发 page fault 的测试用例

## 动手实现

### Step 1: 更新 #PF handler 加入按需分配逻辑

**目标**: 修改 `kernel/arch/x86_64/exception_handlers.cpp` 中的 `handle_pf` 函数，在打印诊断信息之前先尝试按需分配。

**设计思路**: 原来的 `handle_pf` 只是读 CR2 和错误码然后 panic。现在我们要在 panic 之前加一道"尝试修复"的逻辑。具体来说：

第一步，读取 CR2 获取 fault 地址。这一步已经在原有代码中存在。

第二步，检查错误码的 bit 0。如果 bit 0 为 0（P=0），说明这是一个 not-present fault——有可能通过按需分配来修复。如果 bit 0 为 1（P=1），说明这是一个权限违反，按需分配解决不了，直接跳到 panic 路径。

第三步，对 fault 地址做页对齐（低 12 位清零）。

第四步，从 PMM 分配一个物理页。如果分配失败（OOM），跳到 panic 路径。

第五步，调用 `VMM::map` 建立映射，标志位设为 Present + Writable。如果 map 失败，跳到 panic 路径。

第六步，如果分配和映射都成功了，打印一条调试信息（包含虚拟地址和物理地址），然后直接从 handler 返回——CPU 会重新执行那条触发 fault 的指令，这次页表已经准备好了，指令正常执行。

如果走到 panic 路径（P=1 的权限违反，或者 OOM，或者 map 失败），沿用原来的诊断输出：打印 fault 地址、错误码解析（present/protection、read/write、user/supervisor）、然后 panic。

**实现约束**: 修改文件是 `kernel/arch/x86_64/exception_handlers.cpp`。需要新增 include `kernel/arch/x86_64/paging_config.hpp`（FLAG_PRESENT、FLAG_WRITABLE）、`kernel/mm/pmm.hpp`（g_pmm）和 `kernel/mm/vmm.hpp`（g_vmm）。按需分配的代码段应该插入在"读 CR2 和 error code 之后、打印诊断之前"的位置。使用 `using cinux::mm::g_vmm` 来简化代码。

**踩坑预警**: 按需分配的判断条件是 `(err & 0x01) == 0`，而不是 `err & 0x01 == 0`——后者因为运算符优先级会被解析为 `err & (0x01 == 0)`，也就是 `err & false`，永远为 0，你的 handler 永远不会进入按需分配路径，所有 page fault 都会 panic。这种优先级陷阱在 C/C++ 中极其常见，而且编译器不会给出任何警告。

另一个需要注意的点：按需分配 handler 不应该对已经映射的地址做任何操作。如果 fault 地址已经被映射了但 PTE 的权限不允许当前操作（比如写一个 Read-Only 页），按理说不应该分配新页覆盖它——但我们的 handler 只检查 P bit，P=1 的情况直接跳到 panic，所以不会错误地覆盖已有的映射。

**验证**: 最直接的验证方式是下一节中编写的 demand-page 测试用例。你也可以手动在内核代码中加一个临时的测试——对一个从未映射过的地址执行写入，看串口是否输出 `[VMM] Demand-paged ...` 的消息。但要注意，如果按需分配的逻辑有 bug 导致 handler 没有正确返回，你会看到 CPU 不断重试那条触发 fault 的指令，不断触发 #PF，最终看起来的症状就是"内核卡死，串口疯狂打印 demand-paged 消息"——这是一个很好的调试信号，说明 handler 的返回路径有问题。

### Step 2: 编写 QEMU 按需分页集成测试

**目标**: 在 `kernel/test/test_vmm.cpp` 中添加一个测试用例，验证完整的按需分页闭环。

**设计思路**: 这个测试的核心思路极其简单但极其有效——把一个 volatile 指针指向一个绝对没有被映射过的地址（比如 0x40000000），然后直接写入一个魔术值（比如 0xCAFEBABEDEADC0DE）。volatile 关键字确保编译器不会优化掉这个写入操作。

当 CPU 执行这条写入指令时，MMU 发现 0x40000000 对应的 PTE 中 Present=0，于是触发 #PF。handler 读 CR2 得到 0x40000000，检查错误码 bit 0 为 0（not-present），分配一个物理页，调用 map 建立映射，然后返回。CPU 重新执行写入指令——这次页表已经准备好了，写入成功。

接下来测试用 `translate` 查询 0x40000000 对应的物理地址，确认映射已经存在（返回值非零）。然后读回那个地址的值，验证它确实等于写入的魔术值。最后 unmap 那个地址并释放物理页，保持环境干净。

这个测试验证了以下完整链条：未映射地址的访问触发 #PF -> handler 正确读取 CR2 和错误码 -> handler 正确判断 P=0 -> PMM 分配物理页成功 -> VMM map 建立映射成功 -> TLB 刷新生效 -> CPU 重试指令成功 -> 数据写入正确 -> 后续 translate 和 readback 验证通过。

**实现约束**: 测试函数放在 `kernel/test/test_vmm.cpp` 中的 `test_vmm_demand` 命名空间里。使用 `volatile uint64_t*` 类型的指针来防止编译器优化。魔术值用 `0xCAFEBABEDEADC0DEULL`。测试的最后需要 unmap 并 free_page 来清理，避免影响后续测试。在 `run_vmm_tests()` 入口函数中注册这个测试。

**踩坑预警**: 按需分页测试是整个 VMM 测试套件中"最脆弱"的一环——它依赖于 #PF handler 的正确实现、PMM 的正常工作、VMM map 操作的正确性、以及 TLB 刷新的生效。如果这个测试 PASS 了，基本可以确定 tag 016 的所有核心功能都是正确的。如果这个测试 FAIL 了，debug 方向是：先检查串口输出中有没有 `[VMM] Demand-paged ...` 消息（没有的话说明 handler 没有进入按需分配路径），然后检查 PMM 的 free_page_count 是否足够（不够的话 alloc_page 返回 0），最后检查 map 返回值是否为 true。

如果你看到的现象是"内核卡死，串口不断打印 demand-paged 消息"，说明 handler 成功分配并映射了物理页，但 handler 返回后 CPU 重试指令时又触发了 #PF。可能的原因是 TLB 没有被正确刷新（CPU 还在使用旧的"未映射"缓存），或者 map 操作写入的 PTE 有问题（地址字段错误导致 CPU 遍历到了错误的位置）。

**验证**: 在 QEMU 中运行 `big_kernel_test`，串口输出应该包含：

```
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
VMM Tests (016): 10 passed, 0 failed
```

在 `test_demand_page` 之前，你应该能在串口中看到 `[VMM] Demand-paged 0x40000000 -> phys 0xXXXXXX` 的消息，这就是按需分配 handler 在实时工作的证据。

### Step 3: 编写 host 端 VMM 单元测试

**目标**: 在 `test/unit/test_vmm.cpp` 中实现 host 端的纯算法测试，使用 mock PMM 验证 VMM 的 map/translate/unmap 逻辑。

**设计思路**: Host 端测试的核心挑战是：VMM 的算法依赖物理内存来存储页表，但 host 环境中没有真正的 PMM。解决方案是用一个固定的 `alignas(4096) uint8_t` 二维数组来模拟物理内存——`sim_memory[128][4096]` 给我们 128 个"物理页"，每一页都是 4096 字节对齐的。`MockPMM` 类管理一个 256 页的模拟内存池（地址从 0x2000000 开始），用简单的位图分配器跟踪哪些页被分配了。

`sim_phys_of` 和 `sim_virt_of` 两个函数实现了模拟物理地址到 host 虚拟地址的双向转换——让 VMM 算法在 host 上跑的时候，它以为自己在操作物理地址，实际上操作的是 `sim_memory` 数组。

`TestVMM` 类完整复刻了内核中 VMM 的算法——walk_or_alloc（对应 walk_level 的分配模式）、walk_only（对应 walk_level 的不分配模式）、map、unmap、translate。算法逻辑和内核版本完全一致，只是物理-虚拟地址转换使用了 `sim_virt_of` 而不是 `phys_to_virt`。

Host 测试覆盖的场景包括十五个用例：基本的 map + translate、translate 保留页内偏移、unmap 后 translate 返回 0、映射两个不同地址、translate 未映射地址返回 0、remap 覆盖旧映射、标志位正确存储、同一 PT 中映射多个页、unmap 一个页不影响兄弟页、对未映射地址 unmap 是 no-op、高半核地址映射、完整的 map/unmap/remap 循环，以及两个 PageEntry 联合体的单元测试（phys_addr 提取和 is_present 检查）。

**实现约束**: 文件路径是 `test/unit/test_vmm.cpp`。需要定义 `CINUX_HOST_TEST` 宏。使用项目的 `test_framework.h`（`TEST`、`ASSERT_TRUE`、`ASSERT_EQ`、`ASSERT_FALSE` 等宏）。分页常量（PAGE_SIZE、PT_ENTRIES、各级 SHIFT、ADDR_MASK、FLAG_*、索引提取函数）需要在测试文件中镜像定义（因为 host 环境不能 include 内核头文件）。

**踩坑预警**: `sim_memory` 数组的对齐至关重要——`alignas(4096)` 确保每张"模拟页表"的起始地址是 4096 字节对齐的。如果对齐不正确，`sim_phys_of` 计算出的物理地址可能有偏移，导致页表遍历时读到错误的表项。另外，MockPMM 的 `alloc_page` 返回的物理地址从 `MOCK_POOL_BASE`（0x2000000）开始，而 `sim_memory` 数组在 host 内存中的实际位置无关紧要——重要的是 `sim_phys_of` 和 `sim_virt_of` 的转换是一致的。

还有一个微妙的地方：host 测试中的 `walk_or_alloc` 在分配新页后用 `memset(new_table, 0, PAGE_SIZE)` 清零，而内核版本用 `for` 循环逐项清零。两者在功能上等价（因为 PageEntry 的 raw 是 uint64_t，全零意味着所有位域都是零），但 `memset` 更简洁高效。

**验证**: 在 host 端运行 `ctest --output-on-failure -L vmm`。所有十五个测试用例应该全部 PASS。如果有任何 FAIL，ctest 会打印具体的断言失败信息——比如 `ASSERT_EQ(vmm.translate(virt), phys)` 失败时会显示期望值和实际值，帮助你定位是 walk 哪一级出了问题。

### Step 4: 更新构建系统

**目标**: 在 `kernel/CMakeLists.txt` 和 `test/CMakeLists.txt` 中添加 VMM 相关的编译目标。

**设计思路**: 需要四处修改。第一处，在 `kernel/CMakeLists.txt` 的 `big_kernel_common` 库中添加 `mm/vmm.cpp`——这是 VMM 的实现文件。第二处，在 `big_kernel_test` 可执行文件的源文件列表中添加 `test/test_vmm.cpp`——这是 QEMU 集成测试。第三处，在 `test/CMakeLists.txt` 中添加 `test_vmm` 可执行文件的编译规则——host 端单元测试。第四处，把 `test_vmm` 加入到 `test_host`、`test`、`test_verbose` 这三个自定义目标的依赖列表中。

**实现约束**: `test_vmm` 的编译需要定义 `CINUX_HOST_TEST` 宏（通过 `target_compile_definitions`）。include 目录复用已有的 `TEST_INCLUDE_DIRS` 变量。ctest 标签设为 "vmm"。

**踩坑预警**: 如果你忘了把 `mm/vmm.cpp` 加到 `big_kernel_common` 中，链接时会报 "undefined reference to VMM::init()" 之类的错误——这是最明显的提示。如果你忘了把 `test/test_vmm.cpp` 加到 `big_kernel_test` 中，`run_vmm_tests()` 函数不会被编译进去，但 `main_test.cpp` 中对它的调用会在链接时报未定义符号——同样很明显。host 端测试的 `test_vmm` 是完全独立的可执行文件，不依赖任何内核代码，所以它不会因为内核头文件的修改而受影响。

**验证**: 执行完整的构建流程。`big_kernel_test` 和 `test_vmm` 两个目标都应该成功编译。运行 `ctest --output-on-failure` 确认所有测试（包括之前的 PMM 测试和新加的 VMM 测试）全部通过。运行 QEMU 确认内核启动正常、串口输出中 VMM 初始化和测试结果都正确。

## 构建与运行

完整的构建和验证流程：

**Host 端测试**: 在构建目录中执行 `cmake --build . --target test_host` 编译所有 host 测试，然后 `ctest --output-on-failure` 运行。确认 vmm 标签下的十五个测试全部通过。

**QEMU 集成测试**: 编译 `big_kernel_test` 目标后，用 QEMU 启动。完整启动命令应包含 `-serial stdio -display none -device isa-debug-exit,iobase=0xf4,iosize=0x04` 参数（这些在项目的 CMake 配置中已经设置好了）。串口输出中你应该看到：

```
[PMM] Total: XXXMB, Free: XXXMB
[VMM] Initialised, kernel PML4 at phys 0xXXXXX
--- PMM Tests (015) ---
  PASS: ...
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
VMM Tests (016): 10 passed, 0 failed
```

其中 `test_demand_page` 是最关键的——它验证了按需分页的完整闭环。在它执行时你应该能看到一条 `[VMM] Demand-paged 0x40000000 -> phys 0xXXXXXX` 的消息。

## 调试技巧

**按需分页无限循环**: 如果你看到串口不断打印 `[VMM] Demand-paged 0x...` 的消息，说明 handler 成功映射了物理页但 CPU 重试时又触发了 #PF。最可能的原因是 TLB 刷新没有生效——检查 map 操作后是否调用了 `flush_tlb(virt)`。另一个可能是 map 写入的物理地址有误——用 QEMU monitor 的 `x/1gx` 命令检查 PT 表项的 raw 值，确认地址字段和标志位都正确。

**按需分页测试卡死但无输出**: 如果 `test_demand_page` 测试卡死且串口没有任何 demand-paged 消息，说明 #PF handler 根本没有进入按需分配路径。可能的原因：错误码检查条件写反了（`err & 0x01 == 0` 而不是 `(err & 0x01) == 0`）、handler 没有被正确注册到 IDT、或者触发的不是 #PF 而是 double fault（检查 GDB 中是否捕获到了 #DF）。

**QEMU monitor 调试页表**: 连接 QEMU monitor 后可以用 `info tlb` 查看 TLB 缓存内容，用 `info mem` 查看当前活跃的内存映射。如果 map 操作后 TLB 中没有出现新的映射项，说明 TLB 刷新可能没有正确执行，或者 map 写入的 PT 表项地址不对。

**GDB 断点调试**: 在 `handle_pf` 函数开头设断点，当 #PF 触发时检查 frame->error_code 的值和 CR2 的值。如果 CR2 的值和触发 fault 的地址不一致，可能是 handler 读 CR2 的汇编有问题。如果 error_code 的 bit 0 是 1（表示 protection violation 而非 not-present），说明目标地址已经被映射了但权限不匹配——这种情况下按需分配不应该介入。

**区分 handler 故障和 map 故障**: 如果按需分页失败，你需要在 handler 中加更多的调试输出——在每一步都打印状态：CR2 的值、error_code 的值、alloc_page 返回的物理地址、map 的返回值。这样可以精确定位是哪一步出了问题。当然，调试完记得把多余的 kprintf 删掉（或改为只在 DEBUG 模式下输出），否则串口会被刷屏。

## 本章小结

| 组件 | 功能 | 关键点 |
|------|------|--------|
| #PF handler 更新 | 检查 error code bit 0，P=0 时尝试按需分配 | P=0 是 not-present，P=1 是权限违反 |
| CR2 读取 | 获取触发 fault 的精确线性地址 | 需要页对齐后才能传给 map |
| 按需分配流程 | alloc_page -> map(PRESENT+WRITABLE) -> return | 分配或映射失败时 fall through 到 panic |
| Host mock 测试 | MockPMM + sim_memory + TestVMM | 15 个用例覆盖基本路径和边界情况 |
| QEMU 集成测试 | 真实 #PF -> handler -> 恢复 -> 读回验证 | demand_page 测试验证完整闭环 |
| PageEntry 位域测试 | phys_addr() 和 is_present() 的掩码操作 | 确保 raw 值的解读与 Intel SDM 一致 |

| 故障码解读 | bit 0 = 0 | bit 0 = 1 |
|-----------|-----------|-----------|
| 含义 | 页不存在 (not-present) | 权限违反 (protection) |
| Cinux 处理 | 按需分配物理页并映射 | 打印诊断并 panic |
| 典型场景 | 首次访问未映射的地址 | 写 Read-Only 页 |
