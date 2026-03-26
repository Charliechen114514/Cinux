# 006 Mini Kernel PMM - 物理内存管理器

## 章节导语

上一章我们给内核装上了"嘴巴"，它能通过串口告诉我们自己在做什么。但说实话，现在的内核就像一个只有眼睛没有大脑的观察者——它能看到 BootInfo 里的内存布局，能打印出 E820 表项，但如果真的需要分配一块内存来存储数据，它完全束手无策。

这对于操作系统来说是致命的。内核需要内存来存储内核栈、页表、数据结构，将来还需要为用户进程分配内存。没有内存管理器，内核就是一个只能看不能动的"瘫痪状态"。

所以这一章我们要给内核装上"双手"——物理内存管理器（Physical Memory Manager，PMM）。我们将实现一个基于位图（Bitmap）的简单分配器，它能够管理 4GB 以下的物理内存，按 4KB 页粒度进行分配和释放。完成本章后，你将在串口看到 `[MINI] PMM: Total 128MB, Free 124MB` 这样的输出，证明内核真的能够"掌握"自己的内存资源了。

本章的前置知识是上一章的 BootInfo 解析和串口输出，以及对位操作的基本了解。

---

## 概念精讲

### 物理内存管理是什么

物理内存管理器是操作系统最基础的组件之一，它的职责是"记账"——记下哪块物理内存是空闲的，哪块已经被占用了。当内核需要内存时，它找一个空闲块标记为"已用"并返回地址；当内存不再需要时，它把这块地标记为"空闲"以备后用。

```
物理内存布局示意（128MB 机器）：
0x00000000 [低1MB保留] [Bootloader] [Mini Kernel] [空闲...] 0x07FFFFFF
           已用        已用         已用         可用
```

你可能会问：为什么不直接用 `malloc`？答案很简单——`malloc` 依赖操作系统提供的堆管理器，而我们现在正在写操作系统，没有更底层的系统可以依赖。我们必须从最原始的物理页管理开始。

### 为什么用位图而不是链表

位图分配器是 PMM 最简单的实现方式：用一个比特位代表一个物理页（4KB），1 表示已用，0 表示空闲。

```
位图示意（每个字节表示 8 个页的状态）：
Byte 0: 11111111  -> 页 0-7 全部已用
Byte 1: 00000001  -> 页 8-14 空闲，页 15 已用
Byte 2: 00100000  -> 页 16,18-23 空闲，页 17 已用
...
```

对于 4GB 内存，位图大小是 4GB / 4KB / 8 = 128KB，非常紧凑。相比之下，链表方式需要为每个空闲页维护一个节点，内存开销更大且实现更复杂。位图的另一个好处是查找连续空闲页很直观——找到连续 N 个 0 比特就行。

当然位图也有缺点：分配释放是 O(1) 的，但查找第一个空闲页是 O(N) 的（N 是页数）。不过对于我们的 Mini Kernel 来说，128KB 扫描完全可以接受。

### E820 内存映射回顾

BootInfo 里已经包含了 BIOS E820 调用返回的内存映射，我们需要解析它来确定哪些内存是可用的。

```
E820 内存类型（type 字段）：
1 = 可用 RAM（我们只管理这个）
2 = 保留（如 BIOS 区域、MMIO）
3 = ACPI 可回收
4 = ACPI NVS
```

我们的 PMM 会：
1. 初始时把所有页标记为"已用"（保守策略）
2. 遍历 E820 表，把所有 type=1 的区域标记为"空闲"
3. 把低 1MB 和内核自身占用的页重新标记为"已用"

这样既保证不会误用保留内存，又能正确管理可用内存。

### 链接器符号的正确访问方式

PMM 需要知道内核占用了多少内存，以便把这部分页标记为已用。链接器在 `linker.ld` 中提供了 `__kernel_size` 符号，但访问它有个坑。

```cpp
// 错误方式
uint64_t size = __kernel_size;  // 这是链接器符号的地址，不是值！

// 正确方式
uint64_t size = (uint64_t)&__kernel_size;  // 取地址才是实际的值
```

这是因为链接器符号不是真正的变量，而是一个地址标记。当你直接引用 `__kernel_size` 时，你得到的是它的虚拟地址（因为链接器把它当成了一个外部符号）。只有用 `&` 取地址，链接器才会把这个符号替换成实际的数值。

我们会在实现中看到这个细节的正确用法。

---

## 动手实现

### Step 1：创建内存字面量头文件

**目标**：提供 `_KB`、`_MB`、`_GB` 等后缀操作符，让代码更易读。

**代码**（文件路径：`kernel/mini/mm/memory_literals.h`）：
```cpp
/**
 * @file kernel/mini/mm/memory_literals.h
 * @brief Custom Literal Operators for Memory Sizes
 *
 * Provides constexpr user-defined literal operators for KB, MB, GB, TB.
 * Freestanding-compatible - requires no standard library.
 */

#pragma once

#include <stdint.h>

namespace cinux::mini::mm::literals {

// Kilobyte literal operator
// Example: 4_KB -> 4096
constexpr uint64_t operator""_KB(unsigned long long value) {
    return value * 1024ULL;
}

// Megabyte literal operator
// Example: 1_MB -> 1048576
constexpr uint64_t operator""_MB(unsigned long long value) {
    return value * 1024ULL * 1024ULL;
}

// Gigabyte literal operator
// Example: 4_GB -> 4294967296
constexpr uint64_t operator""_GB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace cinux::mini::mm::literals

// Import literals into cinux::mini::mm namespace for convenience
namespace cinux::mini::mm {
    using namespace literals;
}
```

**解释**：
C++11 的用户定义字面量（User-defined literals）让我们可以写 `4_KB` 而不是 `4 * 1024`。这些函数都是 `constexpr`，所以编译器会在编译期计算结果，零运行时开销。我们把它们导入到 `cinux::mini::mm` 命名空间，这样在该命名空间内可以直接用 `1_MB` 而不用写 `literals::1_MB`。

**验证**：这一步只是头文件，编译通过即可。

---

### Step 2：创建内存管理通用定义

**目标**：提供对齐辅助函数和常用常量。

**代码**（文件路径：`kernel/mini/mm/mm_defines.h`）：
```cpp
/**
 * @file kernel/mini/mm/mm_defines.h
 * @brief Memory Management Common Definitions
 */

#pragma once

#include <stdint.h>

// Import memory literals (_KB, _MB, _GB, _TB)
#include "memory_literals.h"

namespace cinux::mini::mm {

// Common Page Size Definitions
constexpr uint64_t PAGE_SIZE_4K = 4_KB;   // 4096 bytes
constexpr uint64_t PAGE_SIZE_2M = 2_MB;   // 2097152 bytes
constexpr uint64_t PAGE_SIZE_1G = 1_GB;   // 1073741824 bytes

// Alignment Helpers (power of 2 only)
constexpr uint64_t align_up(uint64_t addr, uint64_t align) {
    return (addr + align - 1) & ~(align - 1);
}

constexpr uint64_t align_down(uint64_t addr, uint64_t align) {
    return addr & ~(align - 1);
}

constexpr bool is_aligned(uint64_t addr, uint64_t align) {
    return (addr & (align - 1)) == 0;
}

} // namespace cinux::mini::mm
```

**解释**：
`align_up` 和 `align_down` 是经典的位运算技巧。对于 2 的幂次对齐值，`align - 1` 的二进制是低位全 1（如对齐 4096 时是 `0xFFF`）。取反后得到高位全 1、低位全 0 的掩码，与操作就能清除低位。

```
align_up(0x1234, 0x1000):
0x1234 + 0x0FFF = 0x2333
~0x0FFF = 0xFFFFF000
0x2333 & 0xFFFFF000 = 0x2000
```

**验证**：编译通过即可。

---

### Step 3：实现 PMM 头文件

**目标**：定义 PMM 的接口和常量。

**代码**（文件路径：`kernel/mini/mm/pmm.h`）：
```cpp
/**
 * @file kernel/mini/mm/pmm.h
 * @brief Physical Memory Manager (PMM) - Bitmap Allocator
 */

#pragma once

#include <stdint.h>
#include "mm_defines.h"

namespace cinux::mini::mm::pmm {

// Constants
constexpr uint64_t PAGE_SIZE             = 4_KB;        // 4KB pages
constexpr uint64_t MAX_MEMORY            = 4_GB;        // 4GB max supported
constexpr uint64_t MAX_PAGES             = MAX_MEMORY / PAGE_SIZE;     // 1M pages max
constexpr uint64_t BITMAP_SIZE           = MAX_PAGES / 8;             // 128KB bitmap
constexpr uint64_t LOW_MEMORY_BOUNDARY   = 1_MB;        // Filter out low 1MB

// Initialization
/**
 * @brief Initialize the PMM from BootInfo
 * @param boot_info Pointer to BootInfo structure from bootloader
 */
void init(const void* boot_info);

// Page Allocation
/**
 * @brief Allocate a single physical page
 * @return Physical address of allocated page, 0 if OOM
 */
uint64_t alloc_page();

/**
 * @brief Free a single physical page
 * @param phys Physical address of page to free
 */
void free_page(uint64_t phys);

// Statistics
/**
 * @brief Get total number of free pages
 */
uint64_t free_page_count();

/**
 * @brief Get total number of pages in system
 */
uint64_t total_page_count();

} // namespace cinux::mini::mm::pmm
```

**解释**：
PMM 的接口非常简洁：`init` 初始化、`alloc_page` 分配一页、`free_page` 释放一页、两个查询函数。返回 0 表示 OOM（Out Of Memory）是 UNIX 传统。

`BITMAP_SIZE = 128KB` 是因为 4GB / 4KB = 1M 页，每页用 1 bit 表示，1M bits = 128KB。

**验证**：编译通过即可。

---

### Step 4：实现 PMM 核心逻辑

**目标**：实现位图操作和内存管理逻辑。

**代码**（文件路径：`kernel/mini/mm/pmm.cpp`）：
```cpp
/**
 * @file kernel/mini/mm/pmm.cpp
 * @brief Physical Memory Manager (PMM) - Bitmap Allocator Implementation
 */

#include "pmm.h"
#include <stddef.h>
#include <stdint.h>
#include "../../../boot/boot_info.h"
#include "lib/kprintf.h"

namespace cinux::mini::mm::pmm {

// ============================================================
// Internal State
// ============================================================
static uint64_t s_total_pages         = 0;    // Total pages in system
static uint64_t s_free_pages          = 0;    // Free pages available
static uint64_t s_highest_page        = 0;    // Highest page index managed
static uint8_t  s_bitmap[BITMAP_SIZE] = {0};  // Bitmap storage

// External symbols from linker (use &symbol to get the value)
extern "C" {
    extern char __kernel_size;     // Kernel size in bytes
    extern char __mini_kernel_end; // End of kernel
}

namespace {

// ============================================================
// Bitmap Operations
// ============================================================
void set_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] |= (1U << bit_idx);
}

void clear_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] &= ~(1U << bit_idx);
}

bool test_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    return (s_bitmap[byte_idx] & (1U << bit_idx)) != 0;
}

// Find first zero bit (first free page)
int64_t find_first_free() {
    for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
        if (s_bitmap[byte_idx] != 0xFF) {
            // Found a byte with at least one free bit
            uint8_t byte = s_bitmap[byte_idx];
            for (uint64_t bit_idx = 0; bit_idx < 8; bit_idx++) {
                if ((byte & (1U << bit_idx)) == 0) {
                    return static_cast<int64_t>(byte_idx * 8 + bit_idx);
                }
            }
        }
    }
    return -1;  // No free pages
}

// ============================================================
// Memory Region Management
// ============================================================
void mark_region_used(uint64_t phys, uint64_t length) {
    uint64_t start_page = phys / PAGE_SIZE;
    uint64_t end_page   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t page = start_page; page < end_page; page++) {
        if (page < MAX_PAGES && !test_bit(page)) {
            set_bit(page);
            s_free_pages--;
        }
    }
}

void mark_region_free(uint64_t phys, uint64_t length) {
    uint64_t start_page = phys / PAGE_SIZE;
    uint64_t end_page   = (phys + length + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t page = start_page; page < end_page; page++) {
        if (page < MAX_PAGES) {
            clear_bit(page);
            s_free_pages++;
        }
    }
}

}  // anonymous namespace

// ============================================================
// Initialization
// ============================================================
void init(const void* boot_info) {
    const BootInfo* info = static_cast<const BootInfo*>(boot_info);

    // Step 1: Initialize bitmap - mark all pages as used
    for (uint64_t i = 0; i < BITMAP_SIZE; i++) {
        s_bitmap[i] = 0xFF;
    }
    s_total_pages  = 0;
    s_free_pages   = 0;
    s_highest_page = 0;

    // Step 2: Parse E820 memory map and mark available regions as free
    for (uint32_t i = 0; i < info->mmap_count; i++) {
        const MemoryMapEntry* entry = &info->mmap[i];

        // Only process usable memory (type = 1)
        if (entry->type != 1) {
            continue;
        }

        uint64_t base   = entry->base;
        uint64_t length = entry->length;

        // Update highest page
        uint64_t end_page = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
        if (end_page > s_highest_page) {
            s_highest_page = end_page;
            if (s_highest_page > MAX_PAGES) {
                s_highest_page = MAX_PAGES;
            }
        }

        // Filter out low 1MB (reserved by bootloader)
        if (base < LOW_MEMORY_BOUNDARY) {
            if (length <= LOW_MEMORY_BOUNDARY - base) {
                // Entire region is in low 1MB, skip it
                continue;
            }
            // Partial overlap: adjust base and length
            length -= (LOW_MEMORY_BOUNDARY - base);
            base = LOW_MEMORY_BOUNDARY;
        }

        // Align to page boundaries
        uint64_t aligned_base   = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t aligned_length = length - (aligned_base - base);

        if (aligned_length < PAGE_SIZE) {
            continue;
        }

        // Mark pages as free
        mark_region_free(aligned_base, aligned_length);
    }

    s_total_pages = s_highest_page;

    // Step 3: Mark kernel region as used
    // Use linker-provided __kernel_size (note: &__kernel_size gives the value)
    uint64_t kernel_phys = info->kernel_phys_base;
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
    lib::kprintf("[MINI] PMM: kernel_phys=0x%x, kernel_size=0x%x (%u pages)\n", kernel_phys,
                 kernel_size, (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE);
    mark_region_used(kernel_phys, kernel_size);

    // Step 4: Mark bootloader regions as used (0x0 - 0x10000)
    lib::kprintf("[MINI] PMM: marking bootloader 0x0-0x10000 used (%u pages)\n",
                 0x10000 / PAGE_SIZE);
    mark_region_used(0x0, 0x10000);

    // Debug output
    lib::kprintf("[MINI] PMM: Total %u pages (%u MB), Free %u pages (%u MB)\n", s_total_pages,
                 (s_total_pages * PAGE_SIZE) / 1_MB, s_free_pages,
                 (s_free_pages * PAGE_SIZE) / 1_MB);
}

// ============================================================
// Page Allocation
// ============================================================
uint64_t alloc_page() {
    int64_t page_idx = find_first_free();
    if (page_idx < 0) {
        return 0;  // OOM
    }

    set_bit(static_cast<uint64_t>(page_idx));
    s_free_pages--;

    return static_cast<uint64_t>(page_idx) * PAGE_SIZE;
}

void free_page(uint64_t phys) {
    if (phys == 0) {
        return;  // Null address, ignore
    }

    uint64_t page_idx = phys / PAGE_SIZE;
    if (page_idx >= MAX_PAGES) {
        return;  // Invalid address
    }

    if (test_bit(page_idx)) {
        clear_bit(page_idx);
        s_free_pages++;
    }
}

// ============================================================
// Statistics
// ============================================================
uint64_t free_page_count() {
    return s_free_pages;
}

uint64_t total_page_count() {
    return s_total_pages;
}

} // namespace cinux::mini::mm::pmm
```

**解释**：
这段代码的核心是初始化流程。第一步把所有页标记为已用（`0xFF`），这是保守策略——默认全不可用。然后遍历 E820 表，把可用内存（type=1）标记为空闲。接着把低 1MB、内核自身、Bootloader 区域重新标记为已用。

链接器符号 `__kernel_size` 的访问方式是 `(uint64_t)&__kernel_size`，取地址才是实际值。这是一个经典的坑，很多初学者会直接用 `__kernel_size`，结果得到的是虚拟地址而不是大小。

`find_first_free()` 的实现是朴素扫描——从低到高找第一个 0 bit。这没问题，因为 bitmap 只有 128KB，扫描很快。如果将来需要优化，可以用 `__builtin_ctzll`（Count Trailing Zeros）一次跳过一整字节。

**常见陷阱**：
1. 忘记 `reinterpret_cast<uint64_t>(&__kernel_size)` 中的 `&`，会得到错误的内核大小
2. E820 表中的 `base` 可能不是页对齐的，需要对齐后才能标记
3. 低 1MB 过滤逻辑要考虑"部分重叠"的情况，不能简单 `continue`

**验证**：编译通过即可。

---

### Step 5：在 main.cpp 中调用 PMM 初始化

**目标**：在内核入口点初始化 PMM 并输出统计信息。

**代码**（文件路径：`kernel/mini/main.cpp`）：
```cpp
extern "C" {
#include <stdint.h>
}

#include <boot_info.h>
#include "lib/kprintf.h"
#include "mm/pmm.h"

using cinux::mini::lib::kprintf;

extern "C" {
extern uint64_t __boot_info_ptr;
}

extern "C" [[noreturn]] void mini_kernel_main(uint64_t boot_info_addr) {
    BootInfo* boot_info = (BootInfo*)__boot_info_ptr;
    (void)boot_info_addr;

    // ============================================================
    // Kernel Entry Point
    // ============================================================
    kprintf("Cinux Mini Kernel v0.1.0\n");
    kprintf("BootInfo: entry_point=%p, kernel_phys_base=%p\n", boot_info->entry_point,
            boot_info->kernel_phys_base);
    kprintf("Boot Memory Info: mmap_count=%u\n", boot_info->mmap_count);
    for (uint32_t i = 0; i < boot_info->mmap_count; i++) {
        const MemoryMapEntry* entry = &boot_info->mmap[i];
        kprintf("  [%u] base=0x%016x, length=0x%016x, type=%u, acpi=%u\n", i, entry->base,
                entry->length, entry->type, entry->acpi);
    }

    // ============================================================
    // Initialize Physical Memory Manager
    // ============================================================
    using cinux::mini::mm::pmm::init;
    init(boot_info);

    // Halt
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
```

**解释**：
这段代码先打印 E820 内存布局（方便调试），然后调用 `pmm::init()` 初始化物理内存管理器。PMM 初始化过程中会输出统计信息，所以我们不需要额外调用 `kprintf`。

**验证**：现在可以运行了！

---

### Step 6：更新 CMakeLists.txt

**目标**：把新文件加入构建系统。

**代码**（文件路径：`kernel/mini/CMakeLists.txt`）：
```cmake
# Shared Object Library - 源文件只在一处维护
add_library(mini_kernel_common OBJECT
    arch/x86_64/boot.S
    arch/x86_64/crt_stub.cpp
    driver/serial.cpp
    lib/kprintf.cpp
    lib/private/format.cpp
    mm/pmm.cpp          # 新增
)
```

**解释**：
我们把 `pmm.cpp` 加入对象库，这样生产内核和测试内核都能用。头文件 `pmm.h`、`mm_defines.h`、`memory_literals.h` 不需要显式加入 CMake，因为 `target_include_directories` 已经包含了当前目录。

**验证**：`cmake --build build` 应该成功编译。

---

### Step 7：构建与运行

```bash
# 从上一章 tag 开始（005_mini_kernel_entry）
git checkout 005_mini_kernel_entry

# 配置构建
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug -B . -S ..

# 编译
make

# 运行
make run
```

**预期输出**：
```
1234Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xffffffff80020000, kernel_phys_base=0x20000
Boot Memory Info: mmap_count=3
  [0] base=0x0000000000000000, length=0x000000000009fc00, type=1, acpi=0
  [1] base=0x00000000000e0000, length=0x0000000000020000, type=2, acpi=0
  [2] base=0x0000000000100000, length=0x0000000007ef0000, type=1, acpi=0
[MINI] PMM: kernel_phys=0x20000, kernel_size=0x14000 (5 pages)
[MINI] PMM: marking bootloader 0x0-0x10000 used (16 pages)
[MINI] PMM: Total 32768 pages (128 MB), Free 32747 pages (127 MB)
```

解读输出：
- `mmap_count=3` 表示 E820 返回了 3 个内存条目
- 条目 0 是低 640KB 可用内存（被我们过滤掉了）
- 条目 1 是 640KB-1MB 保留区域（BIOS 区域）
- 条目 2 是 1MB-128MB 主内存
- `kernel_size=0x14000` = 80KB，占用 5 个 4KB 页
- 总共 32768 页 = 128MB，其中 32747 页空闲 = 127MB

如果看到这些输出，恭喜你——物理内存管理器工作正常！

---

## 调试技巧

### 常见问题排查

**问题 1：Free 页数显示 0**
- 检查 `mark_region_free` 是否被正确调用
- 确认 E820 表中有 type=1 的可用内存条目
- 验证 `LOW_MEMORY_BOUNDARY` 过滤逻辑没有把所有内存都过滤掉

**问题 2：Total 页数异常大（如超过 1M）**
- 检查 `s_highest_page` 是否被限制在 `MAX_PAGES` 以内
- QEMU 默认 128MB 内存，如果看到 512MB 说明内存解析有问题

**问题 3：内核大小显示异常**
- 确认使用了 `(uint64_t)&__kernel_size` 而不是直接 `__kernel_size`
- 检查链接器脚本中 `__kernel_size` 的计算公式

**问题 4：分配返回 0（OOM）但还有空闲页**
- 检查 `find_first_free()` 的扫描范围是否正确
- 确认 `BITMAP_SIZE` 足够大（128KB）

### 使用 QEMU Monitor 调试

```bash
# 启动 QEMU 时加入 `-monitor stdio` 参数
qemu-system-x86_64 -m 128M -monitor stdio ...

# 在 QEMU Monitor 中
(qemu) info mem    # 查看内存映射
(qemu) xp /128x 0x20000  # 查看内核物理内存
```

### GDB 调试示例

```bash
# 终端 1：启动 QEMU 调试模式
make run-debug

# 终端 2：连接 GDB
gdb build/kernel/mini/mini_kernel.elf
(gdb) target remote :1234
(gdb) break pmm::init
(gdb) continue
(gdb) print s_bitmap[0]  # 查看第一个字节
(gdb) print s_free_pages
```

### 串口输出技巧

如果怀疑某个计算有问题，可以在代码中插入临时调试输出：

```cpp
lib::kprintf("[DEBUG] base=0x%x, aligned_base=0x%x, length=0x%x\n",
             base, aligned_base, aligned_length);
```

---

## 本章小结

### 新增关键函数一览

| 函数 | 功能 | 所在文件 |
|------|------|----------|
| `operator""_KB()` | KB 字面量操作符 | `mm/memory_literals.h` |
| `operator""_MB()` | MB 字面量操作符 | `mm/memory_literals.h` |
| `align_up(addr, align)` | 向上对齐 | `mm/mm_defines.h` |
| `pmm::init(boot_info)` | 初始化 PMM | `mm/pmm.cpp` |
| `pmm::alloc_page()` | 分配一页物理内存 | `mm/pmm.cpp` |
| `pmm::free_page(phys)` | 释放一页物理内存 | `mm/pmm.cpp` |
| `pmm::free_page_count()` | 获取空闲页数 | `mm/pmm.cpp` |
| `pmm::total_page_count()` | 获取总页数 | `mm/pmm.cpp` |

### PMM 数据结构速查

| 常量 | 值 | 说明 |
|------|------|------|
| `PAGE_SIZE` | 4096 | 4KB 页大小 |
| `MAX_MEMORY` | 4GB | 支持的最大内存 |
| `MAX_PAGES` | 1,048,576 | 4GB / 4KB |
| `BITMAP_SIZE` | 131,072 | 128KB 位图 |
| `LOW_MEMORY_BOUNDARY` | 1MB | 过滤低 1MB |

### 链接器符号访问规则

| 符号 | 正确访问方式 | 错误访问方式 |
|------|--------------|--------------|
| `__kernel_size` | `(uint64_t)&__kernel_size` | `__kernel_size` |
| `__mini_kernel_end` | `(uint64_t)&__mini_kernel_end` | `__mini_kernel_end` |

### 下一章预告

现在内核能"记住"自己的内存了，下一章（`007_mini_kernel_interrupts`）我们要给内核装上"反应神经"——中断和异常处理。我们会实现基础的 GDT 和 IDT，让内核能够处理断点异常（#BP）和页错误（#PF），为后续的系统调用和外部中断打好基础。

有了 PMM，我们终于可以在运行时动态分配内存了，这对实现内核栈、页表等动态数据结构至关重要。
