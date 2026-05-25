---
title: 006-mini-kernel-pmm-1 · 物理内存管理
---

# 006-1 内存管理基础设施代码走读

## 概览

本文走读 tag 006 中三个头文件的完整代码：`memory_literals.h`（内存大小字面量）、`mm_defines.h`（页大小常量与对齐工具）、以及 `pmm.h`（PMM 公开接口）。它们构成了物理内存管理的"头文件层"——从底层的字面量运算符到上层的 PMM API，层层递进。走读完这三个文件后，你会清楚地理解 PMM 的设计意图和接口契约，为下一篇阅读核心实现打下基础。

关键设计决策一览：字面量使用 `constexpr` 保证零开销，命名空间分层隔离再按需导入，PMM 接口极简（五个函数），最大管理范围 4GB。

---

## 架构图

```
┌──────────────────────────────────────────────────────────────┐
│                     使用者 (main.cpp, 测试)                   │
└──────────────────────┬───────────────────────────────────────┘
                       │ #include "mm/pmm.h"
                       ▼
┌──────────────────────────────────────────────────────────────┐
│  pmm.h — PMM 公开接口                                        │
│  init() / alloc_page() / free_page() / free/total_page_count│
└──────────────────────┬───────────────────────────────────────┘
                       │ #include "mm_defines.h"
                       ▼
┌──────────────────────────────────────────────────────────────┐
│  mm_defines.h — 页大小常量 + 对齐工具                         │
│  PAGE_SIZE_4K/2M/1G / align_up / align_down / is_aligned    │
└──────────────────────┬───────────────────────────────────────┘
                       │ #include "memory_literals.h"
                       ▼
┌──────────────────────────────────────────────────────────────┐
│  memory_literals.h — C++ 用户定义字面量                        │
│  operator""_KB / _MB / _GB / _TB                             │
└──────────────────────────────────────────────────────────────┘
```

三个头文件形成了一个依赖链：底层是字面量工具，中间是通用定义，上层是 PMM 接口。使用者只需要 include 最顶层的 `pmm.h`，就能获得所有能力。

---

## 代码精讲

### memory_literals.h — 内存大小字面量

```cpp
#pragma once

#include <stdint.h>

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

constexpr uint64_t operator""_TB(unsigned long long value) {
    return value * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
}

} // namespace cinux::mini::mm::literals

namespace cinux::mini::mm {
    using namespace literals;
}
```

先看字面量的定义部分。四个 `operator""` 函数分别处理 KB、MB、GB、TB 四个量级，每个都是简单的乘以 1024 的幂次。`constexpr` 关键字告诉编译器这些函数可以在编译期求值——当你写 `4_KB` 的时候，编译器在编译阶段就算好了 `4 * 1024 = 4096`，生成的机器码里就是裸的立即数 `4096`，没有任何函数调用开销。

后缀名以下划线开头（`_KB` 而不是 `KB`）不是随意的。C++ 标准规定，不带下划线的字面量后缀是为标准库保留的，用户代码使用它们是未定义行为。虽然大多数编译器不会报错，但这是一个潜在的移植性陷阱。

乘法操作中每个因子都显式标记为 `ULL`（`unsigned long long`），这很重要。如果只写 `value * 1024`，而 `value` 在某些平台上可能被隐式转换为 32 位类型，中间结果 `1024 * 1024 = 1048576` 还好，但 `1024 * 1024 * 1024 = 1073741824` 在 32 位 `int` 下就会溢出。使用 `ULL` 确保整个计算在 64 位空间中进行。

命名空间的组织也值得注意。字面量定义在 `cinux::mini::mm::literals` 这个子命名空间里，然后在上一级命名空间 `cinux::mini::mm` 中用 `using namespace literals` 导入。这种"定义在子命名空间、按需导入"的模式既避免了字面量污染全局命名空间，又让 `mm` 命名空间内的代码可以直接写 `4_KB`。

### mm_defines.h — 页大小与对齐工具

```cpp
#pragma once

#include <stdint.h>

#include "memory_literals.h"

namespace cinux::mini::mm {

constexpr uint64_t PAGE_SIZE_4K = 4_KB;   // 4096 bytes
constexpr uint64_t PAGE_SIZE_2M = 2_MB;   // 2097152 bytes
constexpr uint64_t PAGE_SIZE_1G = 1_GB;   // 1073741824 bytes

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

这个文件是整个 mm 子系统的公共入口。它 include 了 `memory_literals.h`，所以任何 include `mm_defines.h` 的代码自动获得了字面量能力。

三个页大小常量对应 x86-64 硬件支持的三级页大小。4KB 是标准页，Intel SDM Vol.3A Section 4.5 明确规定 4KB 页要求地址的 bit 11:0 必须为零——这就是我们 `PAGE_SIZE_4K = 4096` 的由来。2MB 大页和 1GB 巨型页在后续实现虚拟内存管理时才会用到，这里先定义好。

三个对齐函数是经典的位运算技巧，前面已经解释过原理。这里只补充一个细节：这些函数要求 `align` 参数是 2 的幂，但函数内部不做检查。这是有意的——在教学内核中，对齐参数几乎总是编译期常量（`PAGE_SIZE`、`1_MB` 等），做运行时检查纯属浪费。

### pmm.h — PMM 公开接口

```cpp
#pragma once

#include <stdint.h>
#include "mm_defines.h"

namespace cinux::mini::mm::pmm {

constexpr uint64_t PAGE_SIZE           = 4_KB;
constexpr uint64_t MAX_MEMORY          = 4_GB;
constexpr uint64_t MAX_PAGES           = MAX_MEMORY / PAGE_SIZE;
constexpr uint64_t BITMAP_SIZE         = MAX_PAGES / 8;
constexpr uint64_t LOW_MEMORY_BOUNDARY = 1_MB;

void init(const void* boot_info);

uint64_t alloc_page();

void free_page(uint64_t phys);

uint64_t free_page_count();

uint64_t total_page_count();

} // namespace cinux::mini::mm::pmm
```

PMM 的接口设计遵循了"够用就好"的原则——五个函数覆盖了初始化、分配、释放、查询四个核心操作，不多不少。

常量部分值得逐个推算。`MAX_MEMORY = 4_GB` 意味着 PMM 只管理物理地址 0 到 4GB 的范围，这在 32 位时代是合理的上限，对于教学内核来说也足够了。`MAX_PAGES = 4GB / 4KB = 1M`，即最多管理 100 万个页。`BITMAP_SIZE = 1M / 8 = 128KB`，这是位图占用的字节数——每个字节管理 8 个页的状态，1 bit 代表 1 页。`LOW_MEMORY_BOUNDARY = 1MB` 是我们设定的过滤阈值，低于这个地址的物理内存不做管理。

`init` 的参数类型是 `const void*` 而不是 `const BootInfo*`，这是一个减少头文件耦合的设计选择——PMM 的头文件不需要 include `boot_info.h`，实现文件里才需要。调用者传入的实际上是 `BootInfo*`，在 `init` 内部通过 `static_cast` 转换。

`alloc_page` 返回 0 表示 OOM（内存不足），这是 UNIX 的传统——物理地址 0 永远不会被分配出来（它在低 1MB 保留区内），所以用 0 作为"无效地址"是安全的。

---

## 设计决策

### 决策：字面量 vs 宏常量

**问题**：内存大小常量应该用宏（`#define PAGE_SIZE 4096`）、const 变量、还是用户定义字面量？

**本项目的做法**：使用 C++ 用户定义字面量（`4_KB`、`1_MB`）配合 `constexpr` 常量。

**备选方案**：用宏 `#define KB(x) ((x) * 1024)` 或者裸数字加注释 `4096 /* 4KB */`。

**为什么不选备选方案**：宏没有类型安全，在预处理阶段做文本替换容易产生意外行为。裸数字加注释是人类可读性最差的方式——注释可能过时，数字本身不会说话。字面量方案在编译期完成计算、有明确的类型（`uint64_t`）、并且在调用处自文档化——`4_KB` 比 `4096` 易读得多。

**如果要扩展/改进**：可以考虑添加 `constexpr` 单位转换函数（`to_kb(bytes)`、`to_mb(bytes)`），让反向转换也有好的可读性。此外，可以用 `static_assert` 在编译期验证常量之间的约束关系（比如 `MAX_MEMORY % PAGE_SIZE == 0`）。

### 决策：位图大小 vs 动态计算

**问题**：位图大小应该硬编码为 128KB 还是根据实际物理内存动态计算？

**本项目的做法**：硬编码为 128KB 的静态数组。

**备选方案**：根据 E820 检测到的最大物理地址动态计算位图大小，在 init 中分配。

**为什么不选备选方案**：动态分配位图需要先有一个内存分配器——但位图本身就是内存分配器的一部分。这是典型的鸡生蛋问题。硬编码 128KB 静态数组虽然浪费了一些空间（128MB 内核只需要 4KB 位图），但彻底避免了引导阶段的分配依赖。对于教学内核来说，128KB 的固定开销完全可接受。

**如果要扩展/改进**：如果将来需要支持超过 4GB 的物理内存，可以考虑把位图改为 BSS 段中的动态大小数组，在 init 时根据实际物理内存大小来设定有效范围。或者像 Linux 的 memblock 那样改用基于区域的分配器，完全绕开位图大小的问题。

---

## 扩展方向

- **添加编译期断言**：在 `mm_defines.h` 中加入 `static_assert` 验证常量约束（如页大小必须是 2 的幂），难度低
- **添加 `_PB`（Petabyte）字面量**：按照现有模式扩展即可，难度低
- **实现 `align_up` 的溢出检查**：添加 `constexpr` 安全版本，在溢出时返回原地址或报错，难度中等
- **对齐函数的 `static_assert` 约束**：用 C++20 的 `requires` 或 SFINAE 约束 `align` 必须是 2 的幂，难度中等
- **添加单位转换函数**：`to_kb(bytes)`、`to_mb(bytes)` 等反向转换，让输出格式化更方便，难度低

---

## 参考资料

- Intel SDM: Vol.3A Section 4.5 — 4KB 页的 12 位对齐要求，PTE 中 bit 11:0 必须为零
- OSDev Wiki: [Page Frame Allocation](https://wiki.osdev.org/Page_Frame_Allocation) — 位图分配器的设计选择和复杂度分析
- cppreference: [User-defined literals](https://en.cppreference.com/w/cpp/language/user_literal) — C++ 字面量运算符的语法和约束
