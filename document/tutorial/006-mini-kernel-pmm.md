# 006 - Mini Kernel PMM - 物理内存管理器实现

> 作者：
> 标签：x86-64, 物理内存管理, Bitmap分配器, E820内存映射, 链接器符号, CMake Object库

---

## 前言

上章我们让 mini kernel 成功"活"了过来，串口能输出启动信息，C++ 运行时也正常工作。但老实说，内核现在还是个"无家可归"的状态——没有动态内存分配，每次要用内存要么提前声明全局变量，要么就只能在栈上凑合。这怎么能行呢？一个正经的操作系统，怎么能没有自己的内存管理器？

于是这一章，我们要实现一个**物理内存管理器**（Physical Memory Manager，简称 PMM）。听起来挺吓人，但其实核心思想很简单：维护一张 bitmap，每个 bit 代表一个 4KB 的物理页，1 表示已用，0 表示空闲。分配时找第一个 0，释放时把对应的 bit 改回 0 就行了。

但这章真正有意思的地方在于踩坑。首先是**链接器符号访问**的问题——很多人会以为 `extern uint64_t __kernel_size` 声明后直接访问就能拿到内核大小，结果读到的全是 0。然后是 **Object 库模式下全局构造函数不执行**的问题，这个问题一旦出现，现象会非常诡异：串口输出卡住，全局变量的值全是错的，但你就是找不到原因。

让我们把这些问题逐个拆开，看看背后的机制到底是什么样的。

---

## 环境说明

**开发环境**：
- OS: WSL2 Ubuntu 22.04 on Windows 11
- 编译器: GCC 13.3.0 (x86_64-linux-gnu)
- 构建系统: CMake 3.28.1
- 虚拟机: QEMU 8.2.0

**项目配置**：
- 内核虚拟地址: `0xFFFFFFFF80000000`（higher-half）
- 物理加载地址: `0x20000`（128KB）
- 页面大小: 4KB
- 最大支持内存: 4GB
- Bitmap 大小: 128KB（4GB / 4KB / 8 = 1M bits）

**本章新增文件**：
- `kernel/mini/mm/pmm.h` - PMM 接口定义
- `kernel/mini/mm/pmm.cpp` - PMM Bitmap 实现
- `kernel/mini/mm/memory_literals.h` - 内存大小字面量（4_KB, 1_MB 等）
- `kernel/mini/mm/mm_defines.h` - 内存管理通用定义
- `kernel/mini/test/test_pmm.cpp` - PMM 单元测试
- `kernel/mini/test/kernel_test.h` - 测试框架宏

---

## 第一步——链接器符号访问的坑

### 从一个奇怪的现象说起

PMM 初始化时，需要知道内核占用了多少物理内存，以便把这些页面标记为"已用"。最自然的方法是在链接器脚本里导出内核大小符号，然后在 C++ 代码中读取它。

我一开始是这么写的：

```cpp
// 错误写法
extern "C" {
    extern uint64_t __kernel_size;
}

void init(const BootInfo* info) {
    uint64_t kernel_size = __kernel_size;  // 读到的是 0！
    kprintf("Kernel size: %u bytes\n", kernel_size);
}
```

串口输出显示 `kernel_size = 0`，但我明明在 linker.ld 里定义了它：

```ld
PROVIDE(__kernel_size = (__mini_kernel_end - (KERNEL_Virt_BASE + KERNEL_PHYS_BASE)));
```

用 `nm` 检查符号是否存在：

```bash
$ nm build/kernel/mini/mini_kernel | grep __kernel_size
00000000000042f0 A __kernel_size
```

符号确实存在，值是 `0x42f0`（17136 字节）。但为什么 C++ 代码读到的是 0？

### 根本原因：链接器符号不是变量

这里的关键理解是：**链接器符号不是变量，而是地址常量**。

链接器在符号表中记录 `__kernel_size` 的值，但**不会为它分配内存**。当你写 `extern uint64_t __kernel_size` 时，编译器认为这是一个外部变量，会生成读取该地址内存的代码。但链接器没有分配内存，所以该地址处可能什么都没有，或者包含 0。

正确的做法是**取符号的地址**：

```cpp
// 正确写法
extern "C" {
    extern char __kernel_size;  // 注意用 char 类型
}

void init(const BootInfo* info) {
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
    kprintf("Kernel size: %u bytes\n", kernel_size);
}
```

`&__kernel_size` 获取的是符号的**地址**，但对于链接器符号来说，**地址即值**。所以这里取地址得到的 `0x42f0` 才是真正的内核大小。

### 类型选择：为什么是 char？

虽然类型不影响取地址的结果，但习惯上用 `char` 或 `void`，因为这些类型语义上表示"字节"或"地址"，而不是某个具体大小的整数。

常见模式：

```cpp
extern char __kernel_size;        // 大小值
extern char __bss_start[];        // 数组起始（等价于指针）
extern char __mini_kernel_end;    // 位置标记
```

---

## 第二步——实现 Bitmap 物理内存分配器

### 数据结构设计

我们的 PMM 使用一个简单的 bitmap，每个 bit 代表一个 4KB 页面：

```
┌────────────────────────────────────────────────────────────┐
│ Bitmap Layout                                              │
├────────────────────────────────────────────────────────────┤
│                                                            │
│  Bit 0     → Page 0       (0x0000 - 0x0FFF)               │
│  Bit 1     → Page 1       (0x1000 - 0x1FFF)               │
│  Bit 2     → Page 2       (0x2000 - 0x2FFF)               │
│  ...                                                           │
│  Bit N     → Page N       (N * 4KB)                       │
│                                                            │
│  Bit = 0: 页面空闲                                          │
│  Bit = 1: 页面已用                                          │
│                                                            │
│  4GB 内存 → 1M 页 → 128KB Bitmap                           │
└────────────────────────────────────────────────────────────┘
```

### 核心常量定义

```cpp
// kernel/mini/mm/pmm.h
namespace cinux::mini::mm::pmm {

constexpr uint64_t PAGE_SIZE             = 4_KB;      // 4KB 页面
constexpr uint64_t MAX_MEMORY            = 4_GB;      // 最大支持 4GB
constexpr uint64_t MAX_PAGES             = MAX_MEMORY / PAGE_SIZE;  // 1M 页
constexpr uint64_t BITMAP_SIZE           = MAX_PAGES / 8;          // 128KB
constexpr uint64_t LOW_MEMORY_BOUNDARY   = 1_MB;      // 过滤低 1MB

}
```

这里我们使用了 C++11 的用户定义字面量（User-Defined Literals）来写内存大小，比直接写数字常量清晰得多：

```cpp
namespace cinux::mini::mm::literals {

constexpr uint64_t operator""_KB(unsigned long long value) {
    return value * 1024ULL;
}

constexpr uint64_t operator""_MB(unsigned long long value) {
    return value * 1024ULL * 1024ULL;
}

constexpr uint64_t operator""_GB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL;
}

}
```

用起来就是 `4_KB`、`1_MB`、`4_GB`，编译器在编译期就算好了，没有任何运行时开销。

### Bitmap 操作原语

Bitmap 的核心是三个原子操作：

```cpp
// 设置 bit（标记为已用）
void set_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] |= (1U << bit_idx);
}

// 清除 bit（标记为空闲）
void clear_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    s_bitmap[byte_idx] &= ~(1U << bit_idx);
}

// 测试 bit（检查是否已用）
bool test_bit(uint64_t index) {
    uint64_t byte_idx = index / 8;
    uint64_t bit_idx  = index % 8;
    return (s_bitmap[byte_idx] & (1U << bit_idx)) != 0;
}
```

这里没什么花哨的，就是基础的位操作。`byte_idx` 定位到哪个字节，`bit_idx` 定位到字节内的哪个 bit，然后用掩码操作即可。

### 查找第一个空闲页面

分配页面时，需要找到第一个为 0 的 bit：

```cpp
int64_t find_first_free() {
    // 先扫描字节级别，跳过全为 1 的字节
    for (uint64_t byte_idx = 0; byte_idx < BITMAP_SIZE; byte_idx++) {
        if (s_bitmap[byte_idx] != 0xFF) {
            // 找到一个有空闲 bit 的字节
            uint8_t byte = s_bitmap[byte_idx];
            for (uint64_t bit_idx = 0; bit_idx < 8; bit_idx++) {
                if ((byte & (1U << bit_idx)) == 0) {
                    return static_cast<int64_t>(byte_idx * 8 + bit_idx);
                }
            }
        }
    }
    return -1;  // OOM
}
```

这个实现的优化点在于先检查 `s_bitmap[byte_idx] != 0xFF`，这样如果整个字节都被占用，就直接跳过，不用逐个 bit 检查。对于大部分内存都被使用的场景，这个优化能省不少时间。

### 区域标记函数

PMM 初始化时需要批量标记一段物理内存区域为已用或空闲：

```cpp
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
```

这里注意 `end_page` 的计算：`(phys + length + PAGE_SIZE - 1) / PAGE_SIZE`。这个 `+ PAGE_SIZE - 1` 是为了向上取整——如果长度不是 4KB 的整数倍，最后一个不完整的页面也要算进去。

---

## 第三步——PMM 初始化流程

### 解析 E820 内存映射

BIOS 的 E820 调用返回系统中所有内存区域的列表，包括可用内存、保留内存、ACPI 内存等。bootloader 已经把这些信息收集到 `BootInfo->mmap[]` 数组中了。

PMM 的初始化流程：

```cpp
void init(const void* boot_info) {
    const BootInfo* info = static_cast<const BootInfo*>(boot_info);

    // Step 1: 初始化 bitmap - 所有页面标记为已用
    for (uint64_t i = 0; i < BITMAP_SIZE; i++) {
        s_bitmap[i] = 0xFF;
    }
    s_total_pages  = 0;
    s_free_pages   = 0;
    s_highest_page = 0;

    // Step 2: 解析 E820，标记可用内存为空闲
    for (uint32_t i = 0; i < info->mmap_count; i++) {
        const MemoryMapEntry* entry = &info->mmap[i];

        // 只处理可用内存（type = 1）
        if (entry->type != 1) {
            continue;
        }

        uint64_t base   = entry->base;
        uint64_t length = entry->length;

        // 更新最高页面索引
        uint64_t end_page = (base + length + PAGE_SIZE - 1) / PAGE_SIZE;
        if (end_page > s_highest_page) {
            s_highest_page = end_page;
            if (s_highest_page > MAX_PAGES) {
                s_highest_page = MAX_PAGES;
            }
        }

        // 过滤低 1MB（bootloader 保留）
        if (base < LOW_MEMORY_BOUNDARY) {
            if (length <= LOW_MEMORY_BOUNDARY - base) {
                // 整个区域都在低 1MB 内，跳过
                continue;
            }
            // 部分重叠：调整 base 和 length
            length -= (LOW_MEMORY_BOUNDARY - base);
            base = LOW_MEMORY_BOUNDARY;
        }

        // 对齐到页边界
        uint64_t aligned_base   = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint64_t aligned_length = length - (aligned_base - base);

        if (aligned_length < PAGE_SIZE) {
            continue;
        }

        // 标记为空闲
        mark_region_free(aligned_base, aligned_length);
    }

    s_total_pages = s_highest_page;
```

这里有几个关键的过滤和调整：

1. **只处理 type=1 的内存**：其他类型（保留、ACPI 等）不应该被分配
2. **过滤低 1MB**：这块区域被 bootloader 和 BIOS 数据结构占用，不能分配
3. **页面对齐**：确保只标记完整的页面

### 标记内核自身为已用

内核代码本身占用的物理内存不能被分配，需要标记为已用：

```cpp
    // Step 3: 标记内核区域为已用
    uint64_t kernel_phys = info->kernel_phys_base;
    uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
    lib::kprintf("[MINI] PMM: kernel_phys=0x%x, kernel_size=0x%x (%u pages)\n", kernel_phys,
                 kernel_size, (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE);
    mark_region_used(kernel_phys, kernel_size);

    // Step 4: 标记 bootloader 区域为已用 (0x0 - 0x10000)
    lib::kprintf("[MINI] PMM: marking bootloader 0x0-0x10000 used (%u pages)\n",
                 0x10000 / PAGE_SIZE);
    mark_region_used(0x0, 0x10000);

    // 调试输出
    lib::kprintf("[MINI] PMM: Total %u pages (%u MB), Free %u pages (%u MB)\n", s_total_pages,
                 (s_total_pages * PAGE_SIZE) / 1_MB, s_free_pages,
                 (s_free_pages * PAGE_SIZE) / 1_MB);
}
```

这里我们输出了 PMM 的统计信息：总页面数和空闲页面数，单位换算成 MB 更直观。

---

## 第四步——Object 库模式下全局构造函数不执行的坑

### 问题现象

在把 CMake 构建从静态库改为 Object 库后，我发现串口输出卡在 `!is_tx_ready()` 无限循环。调试发现 `Serial::base_port` 成员为 0，而不是预期的 `0x3F8`。

用 debugcon（端口 0xE9）输出调试信息，发现：

```
OPLUUH T...T...T...
T 00 00 00 00  (base_port=0x0, LSR_port=0x5, LSR=0x0)
```

- `OPLUUH`: bootloader 的输出
- `T`: 串口 putc() 的 timeout 标记
- `00 00 00 00`: base_port 高/低字节、LSR 值均为 0

这说明 `Serial` 构造函数根本没有被调用！

### 排查过程

首先检查 `.init_array` 是否包含构造函数：

```bash
$ objdump -s -j .init_array build/kernel/mini/mini_kernel

Contents of section .init_array:
 ffffffff80022378 0d050280 ffffffff                    ........
```

init_array 中确实有 `0xffffffff8002050d`，检查这个符号：

```bash
$ nm build/kernel/mini/mini_kernel | grep "_GLOBAL__sub_I"
ffffffff8002050d t _GLOBAL__sub_I__ZN5cinux4mini6serial6SerialC2Et
```

符号存在。那么问题在哪里？在 `_init_global_ctors()` 里添加调试输出：

```cpp
void _init_global_ctors() {
    __asm__ volatile("movb $0x7B, %%al; outb %%al, $0xE9" ::: "memory");  // '{'
    // ...
}
```

结果没有看到 `{` 字符，说明 `_init_global_ctors()` 本身没有被调用！

再检查 `boot.S` 的 `_start` 函数，它应该在启动时输出 `1234`：

```asm
_start:
    movb $0x31, %al    # '1'
    outb %al, $0xE9
    # ...
    call _init_global_ctors
    movb $0x34, %al    # '4'
    outb %al, $0xE9
```

但 debug.log 中没有 `1234`，说明 `_start` 没有被执行！

### 根本原因：bootloader 跳转地址错误

检查内核的 entry point 和 bootloader 跳转地址：

```bash
# 内核 entry point
$ readelf -h build/kernel/mini/mini_kernel | grep Entry
Entry point address: 0xffffffff8002012a

# bootloader 跳转地址 (stage2.S:304)
movq $0xFFFFFFFF80020000, %rax  # 硬编码地址！
```

bootloader 跳转到 `0xFFFFFFFF80020000`，但 entry point 在 `0xFFFFFFFF8002012a`，相差 `0x12a` 字节！

检查 `0x80020000` 处是什么：

```bash
$ objdump -d build/kernel/mini/mini_kernel | grep "80020000:"
ffffffff80020000:  55  push   %rbp
```

这是 `mini_kernel_main` 的开头（`0x55` = push rbp），所以 bootloader 输出的 `UU` 就是这个函数被直接执行了！

### 为什么之前没问题？

使用静态库时，链接器将所有对象文件合并，`_start` 符号可能恰好被放在了开头位置。但 Object 库模式下，CMake 的 `target_sources()` 添加对象文件的顺序可能影响了链接器排列符号的顺序。

### 解决方案：确保 _start 在 .text 开头

**修改 1：链接器脚本**

```ld
SECTIONS {
    . = KERNEL_Virt_BASE + KERNEL_PHYS_BASE;

    .text : AT(ADDR(.text) - KERNEL_Virt_BASE) {
        *(.text.start)        /* _start 必须在最前面！ */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }
    // ...
}
```

**修改 2：boot.S**

```asm
.section .text.start, "ax"
.code64

.global _start
.type _start, @function

_start:
    cli
    // ...
```

验证修复：

```bash
$ readelf -h build/kernel/mini/mini_kernel | grep Entry
Entry point address: 0xffffffff80020000  # ✅ 现在对齐了

$ objdump -d build/kernel/mini/mini_kernel | head -5
ffffffff80020000 <_start>:
ffffffff80020000:  fa  cli
ffffffff80020001:  b0 31  mov $0x31,%al
```

调试输出对比：

```
修复前: OPLUUHT...T...T... T 00 00 00 00
修复后: OPLUUH1234{E...}C...
         ^^^^  boot.S _start 输出
             ^ _init_global_ctors
               ^ 调用构造函数
                 ^ Serial 构造函数
```

---

## 第五步——测试 PMM

### 单元测试框架

为了让 PMM 的开发更可控，我写了一个简单的测试框架：

```cpp
// kernel/mini/test/kernel_test.h
#define TEST_SECTION(name) \
    kprintf("\n=== %s ===\n", name);

#define RUN_TEST(fn) \
    do { \
        kprintf("Running %s...\n", #fn); \
        fn(); \
        kprintf("  PASSED\n"); \
    } while(0)

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            kprintf("  FAILED: %s\n", #cond); \
            while(1) { __asm__ volatile("cli; hlt"); } \
        } \
    } while(0)

#define TEST_ASSERT_EQ(a, b) TEST_ASSERT((a) == (b))
#define TEST_ASSERT_NE(a, b) TEST_ASSERT((a) != (b))
#define TEST_ASSERT_GT(a, b) TEST_ASSERT((a) > (b))
#define TEST_ASSERT_GE(a, b) TEST_ASSERT((a) >= (b))
#define TEST_ASSERT_LT(a, b) TEST_ASSERT((a) < (b))
```

### Mock BootInfo

测试时需要构造假的 `BootInfo`，因为实际运行环境不一定方便：

```cpp
namespace test_mock {
    static BootInfo s_test_boot_info;
    static MemoryMapEntry s_test_mmap[4];

    BootInfo* create_test_bootinfo() {
        // QEMU 典型内存布局：128MB
        // Entry 0: Low 640KB (usable)
        s_test_mmap[0].base = 0x00000000;
        s_test_mmap[0].length = 0x000A0000;
        s_test_mmap[0].type = 1;  // Usable

        // Entry 1: 640KB-1MB (reserved)
        s_test_mmap[1].base = 0x000A0000;
        s_test_mmap[1].length = 0x00060000;
        s_test_mmap[1].type = 2;  // Reserved

        // Entry 2: Main memory 1MB-128MB
        s_test_mmap[2].base = 0x00100000;
        s_test_mmap[2].length = 127 * 1024 * 1024;
        s_test_mmap[2].type = 1;  // Usable

        s_test_boot_info.mmap_count = 3;
        s_test_boot_info.entry_point = 0xFFFFFFFF80020000;
        s_test_boot_info.kernel_phys_base = 0x20000;
        s_test_boot_info.kernel_size = 0x40000;  // 256KB

        for (uint32_t i = 0; i < 3; i++) {
            s_test_boot_info.mmap[i] = s_test_mmap[i];
        }

        return &s_test_boot_info;
    }
}
```

### 测试用例

测试覆盖了以下场景：

```cpp
// Test 1: 链接器符号访问
namespace test_linker_symbols {
    void test_linker_symbol_access() {
        uint64_t kernel_size = reinterpret_cast<uint64_t>(&__kernel_size);
        TEST_ASSERT_GT(kernel_size, 0);
        // ...
    }
}

// Test 2: PMM 初始化
namespace test_pmm_init {
    void test_initialization() {
        init(test_mock::create_test_bootinfo());
        uint64_t total = total_page_count();
        uint64_t free = free_page_count();
        TEST_ASSERT_GT(total, 0);
        TEST_ASSERT_GT(free, 0);
    }
}

// Test 3: 单页分配
namespace test_pmm_alloc {
    void test_single_allocation() {
        init(test_mock::create_test_bootinfo());
        uint64_t page = alloc_page();
        TEST_ASSERT_NE(page, 0);
        TEST_ASSERT((page & (PAGE_SIZE - 1)) == 0);  // 4KB 对齐
        free_page(page);
    }
}

// Test 4: 多页分配
namespace test_pmm_multi {
    void test_multiple_allocations() {
        init(test_mock::create_test_bootinfo());
        constexpr int N = 10;
        uint64_t pages[N];
        for (int i = 0; i < N; i++) {
            pages[i] = alloc_page();
            TEST_ASSERT_NE(pages[i], 0);
        }
        // 验证 free 页数减少了 N
        TEST_ASSERT_EQ(free_page_count(), free_before - N);
        // 释放
        for (int i = 0; i < N; i++) {
            free_page(pages[i]);
        }
    }
}

// Test 5: 边界情况
namespace test_pmm_edge {
    void test_edge_cases() {
        // 释放页 0（应被忽略）
        free_page(0);
        // 释放无效地址
        free_page(MAX_MEMORY + PAGE_SIZE);
        // 双重释放
        uint64_t page = alloc_page();
        free_page(page);
        free_page(page);  // 应该无效果
    }
}

// Test 6: OOM 处理
namespace test_pmm_oom {
    void test_oom() {
        init(test_mock::create_small_bootinfo());  // 只有 2MB
        uint64_t page;
        int count = 0;
        while ((page = alloc_page()) != 0) {
            count++;
        }
        TEST_ASSERT_EQ(free_page_count(), 0);
        TEST_ASSERT_EQ(alloc_page(), 0);  // 应返回 0
    }
}
```

---

## 验证步骤

构建并运行内核：

```bash
cmake -B build -DCINUX_BUILD_TESTS=ON
cmake --build build

# 运行生产内核
qemu-system-x86_64 \
    -drive format=raw,file=build/image/cinux.img \
    -serial stdio
```

预期输出：

```
Cinux Mini Kernel v0.1.0
BootInfo: entry_point=0xFFFFFFFF80020000, kernel_phys_base=0x20000
Boot Memory Info: mmap_count=3
  [0] base=0x0000000000000000, length=0x00000000000a0000, type=1, acpi=0
  [1] base=0x00000000000a0000, length=0x0000000000060000, type=2, acpi=0
  [2] base=0x0000000000100000, length=0x0000000007e00000, type=1, acpi=0
[MINI] PMM: kernel_phys=0x20000, kernel_size=0x42f0 (17 pages)
[MINI] PMM: marking bootloader 0x0-0x10000 used (16 pages)
[MINI] PMM: Total 2048 pages (8 MB), Free 1975 pages (7 MB)
```

QEMU 默认分配 128MB 内存，但我们的 PMM 当前只管理前 8MB（由于 `MAX_MEMORY = 4_GB` 的限制，实际上 bitmap 可以管理更多，但受限于 E820 返回的最高页面）。如果需要支持更多内存，可以调整 `MAX_MEMORY` 常量。

---

## 总结与展望

到这里我们完成了物理内存管理器的实现。这章踩的两个坑——链接器符号访问和 Object 库全局构造函数——都是典型的"知其然不知其所以然"的问题。查资料时你会发现很多地方都告诉你"要这么做"，但很少解释"为什么必须这么做"。

下章我们要实现虚拟内存管理器（VMM），建立页表映射机制。这会涉及到更复杂的地址转换和页表管理，但也正是现代操作系统的核心魅力所在。

---

## 参考资料

- [Intel SDM Vol. 3A: Chapter 4 - Paging](https://software.intel.com/content/www/us/en/develop/articles/intel-sdm.html)
- [OSDev Wiki: Physical Memory Manager](https://wiki.osdev.org/Paging)
- [OSDev Wiki: E820](https://wiki.osdev.org/Detecting_Memory_(x86))
- [LD Documentation: SECTIONS](https://sourceware.org/binutils/docs/ld/SECTIONS.html)
