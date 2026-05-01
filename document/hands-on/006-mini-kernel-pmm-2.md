# 006-2 实现 Bitmap 物理内存管理器

## 导语

上一篇我们搭好了内存管理的基础定义——字面量和页对齐工具。现在轮到重头戏了：实现一个真正能用的物理内存管理器（Physical Memory Manager，PMM）。它将使用位图（Bitmap）来追踪哪些物理页是空闲的、哪些已被占用，然后提供分配和释放的接口。完成本文后，你的内核将能够在串口输出类似 `[MINI] PMM: Total 128MB, Free 124MB` 的信息，真正"掌握"自己的物理内存资源。

前置知识：上一篇的字面量和页对齐工具，以及 BootInfo 中 E820 内存映射的格式。

---

## 概念精讲

### 位图分配器的工作原理

位图分配器的核心思想极其简单：用一个比特位代表一个物理页的状态。如果这个位是 1，说明对应的页已被占用；如果是 0，说明空闲。对于 4GB 的物理地址空间和 4KB 的页大小，总共有 1M 个页，位图只需要 1M bits = 128KB。这 128KB 直接以静态数组的形式嵌入内核的数据段，不需要任何动态分配。

分配一个页的过程是：从头到尾扫描位图，找到第一个 0 位，把它设为 1，然后返回对应的物理地址（位索引乘以 4096）。释放则反过来：把物理地址除以 4096 得到索引，把对应位清零。这个扫描是 O(N) 的，但 128KB 的位图在现代 CPU 上扫一遍也就是微秒级别，对教学内核完全够用。

### 保守初始化策略

一个容易出错的设计选择是：初始化时该把位图设成什么状态？我们的做法是先把所有位设为 1（全部标记为已用），然后只把 E820 确认为可用的区域标记为空闲。这种"先全部锁定，再谨慎开放"的策略，好处是不会意外把保留内存（BIOS 区域、MMIO 等）当成可用内存分配出去，出了问题也容易排查——最多是可用内存少了，不会是用了不该用的内存。

### E820 低 1MB 过滤

Intel SDM Vol.3A Chapter 3 明确指出，物理地址空间的前 1MB 有特殊的遗留用途：中断向量表（IVT，0x0000-0x03FF）、BIOS 数据区（BDA）、视频内存（0xA0000-0xBFFFF）、以及 BIOS ROM（0xF0000-0xFFFFF）。这些区域虽然 E820 可能报告为"可用"（type=1），但实际使用它们会导致各种诡异问题。所以我们的 PMM 直接过滤掉低于 1MB 的所有区域。

过滤时要注意一个边界情况：如果 E820 某个可用区域跨越了 1MB 边界（比如从 0x80000 到 0x120000），我们不能直接跳过整个区域，而是要把 1MB 以下的部分截掉，只保留 1MB 以上的部分。

### 链接器符号的正确访问

PMM 需要知道内核自身占用了多少物理内存，以便把相应的页标记为已用。链接器脚本提供了 `__kernel_size` 符号来传递这个信息。但这里有一个巨大的坑：链接器符号不是变量，而是地址常量。编译器认为 `__kernel_size` 是一个存放在某个地址的变量，但链接器根本没有为它分配内存。如果你直接读取 `__kernel_size` 的值，实际上是在读取该符号地址处的内存内容——那里面大概率是 0 或者垃圾数据。

正确的做法是：声明为 `extern char __kernel_size`（`char` 类型只是为了满足语法，取地址最小），然后用 `(uint64_t)&__kernel_size` 取地址。对于链接器符号来说，地址本身就是它所代表的值。

你可以用 `nm` 工具验证：`nm build/kernel/mini/mini_kernel | grep __kernel_size` 会输出类似 `00000000000042f0 A __kernel_size`，其中 `A` 表示绝对符号，前面的数值就是实际的内核大小。

---

## 动手实现

### Step 1: 设计 PMM 的公开接口

**目标**：创建 `kernel/mini/mm/pmm.h`，定义 PMM 模块的公开接口和内部常量。

**设计思路**：PMM 的接口要尽量简洁——初始化、分配一页、释放一页、查询统计信息，总共五个函数。内部常量包括页大小（4KB）、最大管理内存（4GB）、位图大小（128KB）、以及低内存过滤边界（1MB）。接口设计遵循 UNIX 传统：分配失败返回 0（因为物理地址 0 永远不会被分配出来），而不是返回错误码。

**实现约束**：

- 所有声明放在 `cinux::mini::mm::pmm` 命名空间下
- 常量使用上一节定义的字面量（`4_KB`、`4_GB` 等）
- 初始化函数接收一个 `const void*` 指针，实际指向 BootInfo 结构——之所以用 `void*` 而不是 `BootInfo*`，是为了减少头文件依赖
- 分配函数返回分配到的物理页的起始地址（总是 4KB 对齐的）
- 释放函数接收物理地址，如果是 0 或者超出管理范围则静默忽略

**验证**：头文件编译通过即可。

---

### Step 2: 实现位图内部操作

**目标**：在 `kernel/mini/mm/pmm.cpp` 中实现位图的底层操作。

**设计思路**：位图是一个 `uint8_t` 数组，每个字节管理 8 个页的状态。核心操作有四个：把某一位设为 1（标记已用）、清为 0（标记空闲）、测试某一位是否为 1、以及扫描找到第一个 0 位。前三个都是 O(1) 的位运算：字节索引 = 页号 / 8，位偏移 = 页号 % 8，然后用位掩码进行操作。第四个 `find_first_free` 需要逐字节扫描，是 O(N) 的。

**实现约束**：

- 位图存储在一个模块内部的静态数组中，大小为 128KB（`BITMAP_SIZE` 常量）
- `set_bit`、`clear_bit`、`test_bit` 是文件内部函数（放在匿名命名空间里），不对外暴露
- `find_first_free` 实现朴素的逐字节扫描：先找到一个不等于 `0xFF` 的字节（说明至少有一个空闲位），然后在那个字节内逐位测试找到第一个 0
- 返回值用 `int64_t`，找不到时返回 -1；调用者需要检查负值
- 另外还需要两个区域操作函数：`mark_region_used` 和 `mark_region_free`，接收一个物理地址和长度，把覆盖到的所有页批量标记

**踩坑预警**：`mark_region_free` 在循环中无条件地对每个页调用 `clear_bit` 并递增计数器——它不检查这个页之前是不是已经空闲的。这意味着如果同一个区域被重复释放，空闲页计数会虚高。在初始化阶段这不是问题（保守策略保证一开始全是已用），但如果你在运行时调用 `free_page` 释放一个已经空闲的页，同样会有这个问题。我们的 `free_page` 公开接口里做了 `test_bit` 检查来防止这种情况，但内部的 `mark_region_free` 没有。

**验证**：这一步还无法独立运行，等 Step 4 整个 init 流程完成后一起验证。

---

### Step 3: 实现初始化流程

**目标**：实现 `pmm::init(boot_info)` 函数，完成位图的初始化。

**设计思路**：初始化分四步走。第一步把整个位图设为 `0xFF`（全部标记为已用），重置所有计数器。第二步遍历 BootInfo 中的 E820 内存映射表，把 type=1（可用 RAM）的区域标记为空闲，同时更新最高页索引。第三步把内核自身占用的区域标记为已用。第四步把 bootloader 区域（0x0-0x10000）也标记为已用。

E820 解析的细节值得多说两句。对于每个可用区域，我们首先要检查它的基地址是否低于 1MB。如果是，并且整个区域都在 1MB 以下，直接跳过；如果跨越了 1MB 边界，就把基地址调到 1MB、相应缩短长度。然后对基地址做 4KB 向上对齐，对结尾做 4KB 向下对齐，舍去不够一页的残余。最后才调用 `mark_region_free` 把这个区域内对齐好的页全部标记为空闲。

内核自身占用大小的获取方式是链接器符号的正确访问范本：声明 `extern "C" { extern char __kernel_size; }`，然后通过 `reinterpret_cast<uint64_t>(&__kernel_size)` 获取实际值。这个值再配合 BootInfo 中的 `kernel_phys_base`（内核加载的物理基地址），就能算出内核占用了哪些物理页。

**实现约束**：

- 初始化顺序很重要：必须先全部设为已用，再释放可用区域，最后把内核和 bootloader 加回来。不能反过来，因为反过来无法保证保守性
- E820 条目的 `type` 字段只有值为 1 时才代表可用 RAM，其他值（2=保留，3=ACPI 可回收，4=ACPI NVS 等）一律跳过
- 对齐后的长度如果小于一页（4096 字节），整个区域都应跳过
- 最高页索引不能超过 `MAX_PAGES`（1M），否则后面的代码会越界访问位图

**踩坑预警**：最致命的坑就是链接器符号的访问方式。如果你不小心写了 `uint64_t size = __kernel_size;`（没有取地址），你会得到 0 或者一个莫名其妙的值，然后内核大小被算错，PMM 可能会把内核占用的内存标记为"可用"然后分配给别人，后果不堪设想。一定要用 `&__kernel_size` 取地址。

另一个容易出错的点是 E820 部分重叠的处理。如果你只写了 `if (base < 1MB) continue;` 而没有处理跨越边界的情况，那从 0x80000 到 0x120000 这种区域会被完全丢弃，白白损失 128KB 可用内存。

**验证**：初始化函数会在最后打印统计信息。预期输出类似：

```
[MINI] PMM: kernel_phys=0x20000, kernel_size=0x14000 (5 pages)
[MINI] PMM: marking bootloader 0x0-0x10000 used (16 pages)
[MINI] PMM: Total 32768 pages (128 MB), Free 32747 pages (127 MB)
```

数字根据你的 QEMU 内存配置会不同，但 Free 应该比 Total 小（因为内核、bootloader、位图本身都占了空间）。

---

### Step 4: 实现分配与释放

**目标**：实现 `alloc_page()` 和 `free_page()` 两个公开接口。

**设计思路**：`alloc_page` 调用 `find_first_free` 找到第一个空闲页的索引，把它标记为已用，递减空闲计数，然后把索引乘以页大小得到物理地址返回。如果 `find_first_free` 返回 -1（没有空闲页了），就返回 0 表示 OOM。`free_page` 则是反向操作：把物理地址转回页索引，检查有效性（地址不能为 0、索引不能越界、页必须是已用状态），然后清除位并递增计数。

**实现约束**：

- `alloc_page` 返回的物理地址保证是 4KB 对齐的（因为索引乘以 4096）
- `free_page` 对空地址（0）静默忽略——这是为了防止"释放空指针"式的误用导致 panic
- `free_page` 对越界地址也静默忽略——PMM 只管理 4GB 以下的内存
- `free_page` 会先检查页是否确实处于已用状态（`test_bit`），避免重复释放导致计数错误

**踩坑预警**：如果你在 `free_page` 中忘记了 `test_bit` 检查，对同一个页调用两次 `free_page` 会让空闲计数比实际多 1。看起来没什么大不了，但累积起来会导致 PMM 以为还有空闲页，实际上已经全部分配出去了——分配器返回一个实际已被使用的页，数据就毁了。

**验证**：可以在 `mini_kernel_main` 中临时测试：分配几个页，检查返回地址是否非零且 4KB 对齐，然后释放它们，检查空闲页数是否恢复。

---

### Step 5: 在 main.cpp 中调用 PMM 初始化

**目标**：在内核主函数中加入 PMM 初始化调用。

**设计思路**：在打印完 E820 信息之后、进入无限停机循环之前，调用 `pmm::init(boot_info)`。PMM 的 init 函数内部会通过 `kprintf` 输出初始化过程和最终统计信息，所以我们不需要额外打印什么。

**实现约束**：

- 需要 include `mm/pmm.h`
- 使用 `using` 声明简化调用：`using cinux::mini::mm::pmm::init;`
- init 必须在串口初始化之后调用（因为它内部用 kprintf 输出信息）
- 传入 `boot_info` 指针（不是地址），因为 `init` 的参数类型是 `const void*`

**验证**：

```bash
cmake --build build
# 运行 QEMU
qemu-system-x86_64 -m 128M -serial stdio -debugcon file:debug.log \
  -drive format=raw,file=build/boot/cinux.img \
  -display none 2>/dev/null

# 预期输出包含:
# [MINI] PMM: kernel_phys=0x20000, kernel_size=...
# [MINI] PMM: Total 32768 pages (128 MB), Free 32747 pages (127 MB)
```

---

## 构建与运行

把 `mm/pmm.cpp` 加入 `CMakeLists.txt` 的对象库源文件列表中：

```bash
# 确认 CMakeLists.txt 中有 mm/pmm.cpp
grep "mm/pmm.cpp" kernel/mini/CMakeLists.txt
# 预期输出: mm/pmm.cpp

# 构建
cmake --build build

# 运行
qemu-system-x86_64 -m 128M -serial stdio \
  -drive format=raw,file=build/boot/cinux.img \
  -display none 2>/dev/null
```

预期串口输出（数字可能略有不同）：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xffffffff80020000, kernel_phys_base=0x20000
Boot Memory Info: mmap_count=3
  [0] base=0x0000000000000000, length=0x000000000009fc00, type=1, acpi=0
  [1] base=0x00000000000e0000, length=0x0000000000020000, type=2, acpi=0
  [2] base=0x0000000000100000, length=0x0000000007ef0000, type=1, acpi=0
[MINI] PMM: kernel_phys=0x20000, kernel_size=0x14000 (5 pages)
[MINI] PMM: marking bootloader 0x0-0x10000 used (16 pages)
[MINI] PMM: Total 32768 pages (128 MB), Free 32747 pages (127 MB)
```

解读这些数字：`mmap_count=3` 表示 E820 返回了 3 个条目；条目 0 是低 640KB 可用内存（被低 1MB 过滤掉了）；条目 1 是 640KB-1MB 保留区域（BIOS 区域，跳过）；条目 2 是 1MB 到 128MB 的主内存。`kernel_size=0x14000`（80KB）占用 5 个 4KB 页。总共 32768 页 = 128MB，其中 32747 页空闲 = 约 127MB。

---

## 调试技巧

### 常见问题排查

**问题 1：Free 页数显示 0**

这通常意味着 E820 解析出了问题。检查 `mark_region_free` 是否被正确调用——在 init 函数中加一行临时输出，看看 `mmap_count` 是多少、每个条目的 type 是什么。如果你的 QEMU 配置的内存不是 128M 而是 32M 或其他值，数字会不同但 Free 不应该为 0。

**问题 2：Total 页数异常大（超过 1M = 1048576）**

检查 `s_highest_page` 是否被正确限制在 `MAX_PAGES` 以内。正常情况下 QEMU 128MB 对应的 highest_page 应该是 32768（0x8000000 / 4096）。如果看到 1M 或更大的数字，说明某个 E820 条目的 base + length 计算溢出了，或者 QEMU 报告了超过 4GB 的内存。

**问题 3：内核大小显示为 0**

这是链接器符号访问错误——你忘了用 `&` 取地址。用 `nm` 工具验证符号值：

```bash
nm build/kernel/mini/mini_kernel | grep __kernel_size
# 应该输出: 000000000000XXXX A __kernel_size
# 如果输出的值是 0，说明链接器脚本里的计算有问题
```

### 使用 GDB 调试

```bash
# 终端 1: 启动 QEMU 调试模式
qemu-system-x86_64 -m 128M -serial stdio -s -S \
  -drive format=raw,file=build/boot/cinux.img -display none

# 终端 2: 连接 GDB
gdb build/kernel/mini/mini_kernel.elf
(gdb) target remote :1234
(gdb) break pmm::init
(gdb) continue
# 到达断点后可以检查变量
(gdb) print s_free_pages
(gdb) print s_total_pages
```

---

## 本章小结

本文实现了 PMM 的核心逻辑：一个基于位图的物理页分配器，使用保守初始化策略从 E820 内存映射中构建可用内存视图。关键设计决策包括：位图而非链表（简单直观、O(1) 查询任意页状态）、保守初始化（先锁定全部再谨慎开放）、过滤低 1MB（避免碰遗留硬件区域）、以及正确的链接器符号访问（取地址而非直接读取）。

| 关键概念 | 说明 |
|---------|------|
| 位图分配器 | 1 bit 代表 1 页，128KB 管理 4GB |
| 保守初始化 | 先全标已用，再释放可用区域 |
| 低 1MB 过滤 | IVT/BDA/视频内存/BIOS ROM 保留区 |
| 链接器符号 | `&__kernel_size` 取地址才是值 |

下一篇我们将把 PMM 集成到构建系统中，编写单元测试，并解决一个由 CMake 对象库引起的全局构造函数调用问题。
